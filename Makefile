# KestrelOS build. Run from Linux/WSL: make, make run, make test
CC      := gcc
LD      := ld
OBJCOPY := objcopy
NASM    := nasm
PY      := python3
QEMU    := qemu-system-x86_64

BUILD := build

.DEFAULT_GOAL := all

# Build configuration. `config` (repo root) generates kernel/include/config.h
# and this fragment; it is included above the wildcards so the object and app
# lists can be filtered. GNU make remakes an included file and restarts
# itself, so the rule further down keeps it current on its own.
-include $(BUILD)/config.mk

CFLAGS := -m64 -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
          -fno-omit-frame-pointer -Wall -Wextra -O2 -g \
          -Ikernel/include -Iabi -MMD -MP
LDFLAGS := -nostdlib -z max-page-size=0x1000 --no-warn-rwx-segments

UCFLAGS := -m64 -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -Wall -Wextra -O2 -g \
           -Ilibc/include -Iabi -Ilibgui \
           -Ilibz -Ilibtls -Ilibimg -Ilibweb -Ilibjs -MMD -MP
AR      := ar

KERNEL_CSRC := $(filter-out $(CONFIG_KERNEL_EXCLUDE), $(wildcard kernel/*.c))
KERNEL_ASRC := $(wildcard kernel/*.asm)
KERNEL_OBJS := $(BUILD)/kernel/entry.o \
               $(filter-out $(BUILD)/kernel/entry.o, \
                 $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(KERNEL_ASRC))) \
               $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(KERNEL_CSRC))

# crt0 is linked explicitly first on every app, so it stays out of the
# archive; everything else is pulled in on demand by the linker.
CRT0      := $(BUILD)/libc/crt0.o
LIBC_OBJS := $(filter-out $(CRT0), \
               $(patsubst libc/%.c,$(BUILD)/libc/%.o,$(wildcard libc/*.c)) \
               $(patsubst libc/%.asm,$(BUILD)/libc/%.o,$(wildcard libc/*.asm)))
LIBC_A    := $(BUILD)/libc.a

LIBGUI_OBJS := $(patsubst libgui/%.c,$(BUILD)/libgui/%.o,$(wildcard libgui/*.c))
LIBGUI_A    := $(BUILD)/libgui.a

# Browser stack. Each is its own archive so a program that does not use a
# library does not carry it: only the browser links libweb, and only https
# pulls in libtls.
BROWSER_LIBS := libz libtls libimg libweb libjs
BROWSER_ARCHIVES := $(foreach l,$(BROWSER_LIBS),$(BUILD)/$(l).a)

define BROWSER_LIB_RULE
$(BUILD)/$(1)/%.o: $(1)/%.c
	@mkdir -p $$(dir $$@)
	$(CC) $(UCFLAGS) -c $$< -o $$@

$(BUILD)/$(1).a: $$(patsubst $(1)/%.c,$(BUILD)/$(1)/%.o,$$(wildcard $(1)/*.c))
	@mkdir -p $(BUILD)
	$$(if $$^,$(AR) rcs $$@ $$^,$(AR) rcs $$@)
endef
$(foreach l,$(BROWSER_LIBS),$(eval $(call BROWSER_LIB_RULE,$(l))))

# The cascade's DOM binding (css_dom_ops / css_style_dom_sink) is compiled
# in only here, so the CSS engine stays usable without a DOM.
$(BUILD)/libweb/style.o: libweb/style.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -DCSS_WITH_DOM -c $< -o $@

# apps/html.c is the browser's rendering engine, not a program.
APP_LIBS  := html
APP_NAMES := $(filter-out $(APP_LIBS) $(CONFIG_APPS_EXCLUDE), \
               $(patsubst apps/%.c,%,$(wildcard apps/*.c)))
APP_BINS  := $(patsubst %,$(BUILD)/apps/%,$(APP_NAMES))

# Packages are built like apps but staged into .kpkg archives instead of /bin.
PKG_NAMES := $(notdir $(wildcard packages/*))
PKG_SRC   := $(wildcard packages/*/src/*.c)
KPKGS     := $(patsubst %,$(BUILD)/repo/%.kpkg,$(PKG_NAMES))
PKG_PROGS := $(foreach s,$(PKG_SRC),\
               $(BUILD)/pkg/$(word 2,$(subst /, ,$(s)))/root/bin/$(notdir $(basename $(s))))
PKG_DATA  := $(shell find packages -path '*/root/*' -type f 2>/dev/null)

ROOTFS_SRC := $(shell find rootfs -type f 2>/dev/null)

QEMU_BASE := -drive file=$(BUILD)/os.img,format=raw -no-reboot \
             -device rtl8139,netdev=n0 -netdev user,id=n0

.PHONY: all run run-headless test smoke fsck screenshot vm-images clean help \
        reconfig checkconfig

all: $(BUILD)/os.img

# ---------------- kernel ----------------

$(BUILD)/kernel/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/kernel/%.o: kernel/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/kernel.elf: $(KERNEL_OBJS) kernel/linker.ld
	$(LD) $(LDFLAGS) -T kernel/linker.ld -o $@ $(KERNEL_OBJS)

$(BUILD)/kernel.bin: $(BUILD)/kernel.elf
	$(OBJCOPY) -O binary $< $@

# ---------------- kernel modules ----------------
# Built exactly like the kernel but never linked: the ET_REL object *is*
# the .kmod. -fno-common keeps SHN_COMMON symbols out (the loader refuses
# them) and -fno-asynchronous-unwind-tables keeps .eh_frame -- which is
# SHF_ALLOC, so it would be copied and relocated for nothing -- out.
MODCFLAGS := -m64 -ffreestanding -nostdlib -fno-stack-protector -fno-pic \
             -fno-pie -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
             -mcmodel=kernel -fno-common -fno-asynchronous-unwind-tables \
             -Wall -Wextra -O2 -Ikernel/include -Iabi -MMD -MP

MOD_NAMES := $(patsubst modules/%.c,%,$(wildcard modules/*.c))
MOD_BINS  := $(patsubst %,$(BUILD)/modules/%.kmod,$(MOD_NAMES))

$(BUILD)/modules/%.kmod: modules/%.c
	@mkdir -p $(dir $@)
	$(CC) $(MODCFLAGS) -c $< -o $@

# ---------------- configuration ----------------

# One mkconfig run writes both outputs; the header rule only orders them.
# Outputs are rewritten only when their content changes, so regenerating
# the configuration does not force a needless kernel rebuild.
$(BUILD)/config.mk: config tools/mkconfig.py
	@mkdir -p $(BUILD)
	$(PY) tools/mkconfig.py --config config \
	  --header kernel/include/config.h --mk $@ --quiet

kernel/include/config.h: $(BUILD)/config.mk
	@:

reconfig:
	$(PY) tools/mkconfig.py --config config \
	  --header kernel/include/config.h --mk $(BUILD)/config.mk

checkconfig:
	$(PY) tools/mkconfig.py --config config --check

# ---------------- bootloader ----------------

$(BUILD)/stage1.bin: boot/stage1.asm
	@mkdir -p $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/stage2.bin: boot/stage2.asm $(BUILD)/kernel.bin
	$(NASM) -f bin \
	  -DKERNEL_SECTORS=$$(( ( $$(stat -c %s $(BUILD)/kernel.bin) + 511 ) / 512 )) \
	  $< -o $@

# ---------------- userspace ----------------

$(BUILD)/libc/%.o: libc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/libc/%.o: libc/%.asm
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(BUILD)/libgui/%.o: libgui/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

# Archives, so an app links only the objects it actually references.
$(LIBC_A): $(LIBC_OBJS)
	$(AR) rcs $@ $^

$(LIBGUI_A): $(LIBGUI_OBJS)
	$(AR) rcs $@ $^

$(BUILD)/apps/%.o: apps/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

USER_LINK = $(LD) -nostdlib -z max-page-size=0x1000 -T apps/user.ld -o $@

$(BUILD)/apps/%: $(BUILD)/apps/%.o $(CRT0) $(LIBC_A) $(LIBGUI_A) apps/user.ld
	$(USER_LINK) $(CRT0) $< $(LIBGUI_A) $(LIBC_A)

# The browser also needs its rendering engine and the whole web stack.
# Archive order matters: libweb calls libz/libtls/libimg/libjs, so those
# follow it on the line, and libc is last because everything calls it.
$(BUILD)/apps/browser: $(BUILD)/apps/browser.o $(BUILD)/apps/html.o $(CRT0) \
                       $(LIBC_A) $(LIBGUI_A) $(BROWSER_ARCHIVES) apps/user.ld
	$(USER_LINK) $(CRT0) $(BUILD)/apps/browser.o $(BUILD)/apps/html.o \
	  $(BUILD)/libweb.a $(BUILD)/libjs.a $(BUILD)/libimg.a \
	  $(BUILD)/libtls.a $(BUILD)/libz.a $(LIBGUI_A) $(LIBC_A)

# tlstest is the only non-browser program that needs the TLS stack.
$(BUILD)/apps/tlstest: $(BUILD)/apps/tlstest.o $(CRT0) $(LIBC_A) \
                       $(LIBGUI_A) $(BUILD)/libtls.a apps/user.ld
	$(USER_LINK) $(CRT0) $(BUILD)/apps/tlstest.o $(LIBGUI_A) \
	  $(BUILD)/libtls.a $(LIBC_A)

# ---------------- packages ----------------

define PKG_PROG_RULE
$(BUILD)/pkgobj/$(1)/$(2).o: packages/$(1)/src/$(2).c
	@mkdir -p $$(dir $$@)
	$(CC) $(UCFLAGS) -c $$< -o $$@

$(BUILD)/pkg/$(1)/root/bin/$(2): $(BUILD)/pkgobj/$(1)/$(2).o $(CRT0) $(LIBC_A) \
                                 $(LIBGUI_A) apps/user.ld
	@mkdir -p $$(dir $$@)
	$(LD) -nostdlib -z max-page-size=0x1000 -T apps/user.ld -o $$@ \
	  $(CRT0) $(BUILD)/pkgobj/$(1)/$(2).o $(LIBGUI_A) $(LIBC_A)
endef
$(foreach p,$(PKG_NAMES),$(foreach s,$(wildcard packages/$(p)/src/*.c),\
  $(eval $(call PKG_PROG_RULE,$(p),$(notdir $(basename $(s)))))))

$(BUILD)/repo/%.kpkg: packages/%/manifest $(PKG_PROGS) tools/mkpkg.py $(PKG_DATA)
	@mkdir -p $(BUILD)/pkg/$*/root $(BUILD)/repo
	if [ -d packages/$*/root ]; then cp -r packages/$*/root/. $(BUILD)/pkg/$*/root/; fi
	$(PY) tools/mkpkg.py --manifest packages/$*/manifest \
	  --root $(BUILD)/pkg/$*/root --out $@

$(BUILD)/repo/index.kpi: $(KPKGS) tools/mkrepo.py
	$(PY) tools/mkrepo.py --repo $(BUILD)/repo --out $@

# ---------------- filesystem image ----------------

$(BUILD)/fs.img: $(APP_BINS) $(MOD_BINS) tools/mkfs.py $(ROOTFS_SRC) $(KPKGS) \
                 $(BUILD)/repo/index.kpi
	rm -rf $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs/bin $(BUILD)/rootfs/dev $(BUILD)/rootfs/run \
	         $(BUILD)/rootfs/tmp $(BUILD)/rootfs/var/log \
	         $(BUILD)/rootfs/lib/modules \
	         $(BUILD)/rootfs/var/pkg/repo $(BUILD)/rootfs/var/pkg/db \
	         $(BUILD)/rootfs/var/pkg/cache
	if [ -d rootfs ]; then cp -r rootfs/. $(BUILD)/rootfs/; fi
	cp $(APP_BINS) $(BUILD)/rootfs/bin/
	cp $(MOD_BINS) $(BUILD)/rootfs/lib/modules/
	cp $(KPKGS) $(BUILD)/repo/index.kpi $(BUILD)/rootfs/var/pkg/repo/
	$(PY) tools/mkfs.py --mode /etc/shadow:0600 --mode /var/pkg/db:0700 \
	  --mode /tmp:0777 --mode /run:0777 \
	  $(BUILD)/rootfs $@ 32

# ---------------- disk image ----------------

$(BUILD)/os.img: $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin \
                 $(BUILD)/fs.img tools/mkimage.py
	$(PY) tools/mkimage.py $(BUILD)/stage1.bin $(BUILD)/stage2.bin \
	  $(BUILD)/kernel.bin $@ $(BUILD)/fs.img

run: all
	$(QEMU) $(QEMU_BASE) -serial stdio

run-headless: all
	$(QEMU) $(QEMU_BASE) -display none -serial stdio

test: all
	$(PY) tools/e2e.py

smoke: all
	$(PY) tools/e2e.py --smoke

fsck: $(BUILD)/fs.img
	$(PY) tools/kfsck.py -l $<

screenshot: all
	sh tools/screenshot.sh $(BUILD)/shots/kestrel.png 8 $(CMD)

vm-images: all
	sh tools/mkvm.sh

help:
	@echo "KestrelOS build targets:"
	@echo "  make            build build/os.img (bootable raw disk image)"
	@echo "  make run        boot it in QEMU (window + serial on stdio)"
	@echo "  make run-headless   boot it with serial only"
	@echo "  make test       run the end-to-end suite in headless QEMU"
	@echo "  make smoke      boot-only quick check"
	@echo "  make fsck       validate + list the generated KFS image"
	@echo "  make screenshot capture the VGA console to a PNG"
	@echo "  make vm-images  convert to VirtualBox/VMware disk formats"
	@echo "  make clean      remove build/"

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/modules/*.d) \
         $(wildcard $(BUILD)/kernel/*.d) $(wildcard $(BUILD)/libc/*.d) \
         $(wildcard $(BUILD)/libgui/*.d) \
         $(foreach l,$(BROWSER_LIBS),$(wildcard $(BUILD)/$(l)/*.d)) \
         $(wildcard $(BUILD)/apps/*.d)
