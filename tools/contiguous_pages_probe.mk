$(PROBE_DIR)/pmm.o: tools/contiguous_pages_probe.c kernel/pmm.c
	$(CC) $(CFLAGS) -c $< -o $@

contig_probe_objs := $(subst $(BUILD)/kernel/pmm.o,$(PROBE_DIR)/pmm.o,$(KERNEL_OBJS))

$(PROBE_DIR)/kernel.elf: $(contig_probe_objs) kernel/linker.ld
	$(LD) $(LDFLAGS) --wrap=fb_init -T kernel/linker.ld -o $@ $(contig_probe_objs)

$(PROBE_DIR)/kernel.bin: $(PROBE_DIR)/kernel.elf
	$(OBJCOPY) -O binary $< $@

$(PROBE_DIR)/stage2.bin: boot/stage2.asm $(PROBE_DIR)/kernel.bin
	$(NASM) -f bin -DKERNEL_SECTORS=$$($(PY) -c "import os; print((os.path.getsize('$(PROBE_DIR)/kernel.bin')+511)//512)") $< -o $@

$(PROBE_DIR)/os.img: $(BUILD)/stage1.bin $(PROBE_DIR)/stage2.bin $(PROBE_DIR)/kernel.bin $(BUILD)/fs.img
	$(PY) tools/mkimage.py $(wordlist 1,3,$^) $@ $(lastword $^)
