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
          -Ikernel/include -MMD -MP
LDFLAGS := -nostdlib -z max-page-size=0x1000 --no-warn-rwx-segments

KERNEL_CSRC := $(wildcard kernel/*.c)
KERNEL_ASRC := $(wildcard kernel/*.asm)
KERNEL_OBJS := $(BUILD)/kernel/entry.o \
               $(filter-out $(BUILD)/kernel/entry.o, \
                 $(patsubst kernel/%.asm,$(BUILD)/kernel/%.o,$(KERNEL_ASRC))) \
               $(patsubst kernel/%.c,$(BUILD)/kernel/%.o,$(KERNEL_CSRC))

QEMU_BASE := -drive file=$(BUILD)/os.img,format=raw -no-reboot \
             -device rtl8139,netdev=n0 -netdev user,id=n0

.PHONY: all run run-headless clean

all: $(BUILD)/os.img

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

$(BUILD)/stage1.bin: boot/stage1.asm
	@mkdir -p $(BUILD)
	$(NASM) -f bin $< -o $@

$(BUILD)/stage2.bin: boot/stage2.asm $(BUILD)/kernel.bin
	$(NASM) -f bin \
	  -DKERNEL_SECTORS=$$(( ( $$(stat -c %s $(BUILD)/kernel.bin) + 511 ) / 512 )) \
	  $< -o $@

$(BUILD)/os.img: $(BUILD)/stage1.bin $(BUILD)/stage2.bin $(BUILD)/kernel.bin tools/mkimage.py
	$(PY) tools/mkimage.py $(BUILD)/stage1.bin $(BUILD)/stage2.bin \
	  $(BUILD)/kernel.bin $@

run: all
	$(QEMU) $(QEMU_BASE) -serial stdio

run-headless: all
	$(QEMU) $(QEMU_BASE) -display none -serial stdio

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/kernel/*.d)
