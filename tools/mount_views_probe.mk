$(PROBE_DIR)/mount_views_probe.o: tools/mount_views_probe.c
	$(CC) $(CFLAGS) -c $< -o $@

$(PROBE_DIR)/kernel.elf: $(KERNEL_OBJS) $(PROBE_DIR)/mount_views_probe.o kernel/linker.ld
	$(LD) $(LDFLAGS) -T kernel/linker.ld -o $@ $(filter %.o,$^)

$(PROBE_DIR)/kernel.bin: $(PROBE_DIR)/kernel.elf
	$(OBJCOPY) -O binary $< $@

$(PROBE_DIR)/stage2.bin: boot/stage2.asm $(PROBE_DIR)/kernel.bin
	$(NASM) -f bin -DKERNEL_SECTORS=$$($(PY) -c "import os; print((os.path.getsize('$(PROBE_DIR)/kernel.bin')+511)//512)") $< -o $@

$(PROBE_DIR)/os.img: $(BUILD)/stage1.bin $(PROBE_DIR)/stage2.bin $(PROBE_DIR)/kernel.bin $(BUILD)/fs.img
	$(PY) tools/mkimage.py $(wordlist 1,3,$^) $@ $(lastword $^)
