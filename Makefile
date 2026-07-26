# KestrelOS build. Run from Linux/WSL: make, make run, make test
CC      := gcc
LD      := ld
OBJCOPY := objcopy
NASM    := nasm
PY      := python3
QEMU    := qemu-system-x86_64

BUILD := build

CFLAGS := -m64 -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-pie \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel \
          -fno-omit-frame-pointer -Wall -Wextra -O2 -g \
          -Ikernel/include -Iabi -MMD -MP
LDFLAGS := -nostdlib -z max-page-size=0x1000 --no-warn-rwx-segments

UCFLAGS := -m64 -ffreestanding -nostdlib -fno-stack-protector -fno-pic -fno-pie \
           -mno-red-zone -Wall -Wextra -O2 -g -Ilibc/include -Iabi -MMD -MP

KERNEL_CSRC := $(wildcard kernel/*.c)
KERNEL_ASRC := $(wildcard kernel/*.asm)
KERNEL_OBJS := $(BUILD)/kernel/entry.o \
               $(filter-out $(BUILD)/kernel/entry.o, \
                 $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(KERNEL_ASRC))) \
               $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(KERNEL_CSRC))

LIBC_OBJS := $(patsubst libc/%.c,$(BUILD)/libc/%.o,$(wildcard libc/*.c)) \
             $(patsubst libc/%.asm,$(BUILD)/libc/%.o,$(wildcard libc/*.asm))

APP_NAMES := $(patsubst apps/%.c,%,$(wildcard apps/*.c))
APP_BINS  := $(patsubst %,$(BUILD)/apps/%,$(APP_NAMES))

ROOTFS_SRC := $(shell find rootfs -type f 2>/dev/null)

QEMU_BASE := -drive file=$(BUILD)/os.img,format=raw -no-reboot \
             -device rtl8139,netdev=n0 -netdev user,id=n0

.PHONY: all run run-headless test clean

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

$(BUILD)/apps/%.o: apps/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/apps/%: $(BUILD)/apps/%.o $(LIBC_OBJS) apps/user.ld
	$(LD) -nostdlib -z max-page-size=0x1000 -T apps/user.ld -o $@ \
	  $(BUILD)/libc/crt0.o $(BUILD)/apps/$*.o \
	  $(filter-out $(BUILD)/libc/crt0.o,$(LIBC_OBJS))

# ---------------- filesystem image ----------------

$(BUILD)/fs.img: $(APP_BINS) tools/mkfs.py $(ROOTFS_SRC)
	rm -rf $(BUILD)/rootfs
	mkdir -p $(BUILD)/rootfs/bin
	if [ -d rootfs ]; then cp -r rootfs/. $(BUILD)/rootfs/; fi
	cp $(APP_BINS) $(BUILD)/rootfs/bin/
	$(PY) tools/mkfs.py $(BUILD)/rootfs $@ 32

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

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/kernel/*.d) $(wildcard $(BUILD)/libc/*.d) \
         $(wildcard $(BUILD)/apps/*.d)
