ALL_CLEAN += sigma0-clean

SIGMA0_INCLUDE = -Isigma0/include/
SIGMA0_CFLAGS  = -ffreestanding \
				 -nostartfiles -fpic $(SIGMA0_INCLUDE) $(C4_CFLAGS)

sig-objs = sigma0/sigma0.o sigma0/tar.o sigma0/initfs.o
sig-libs = $(BUILD)/lib/libc.a

sigma0/%.o: sigma0/%.c
	@echo CC $< -c -o $@
	@$(C4_CC) $< -c -o $@ $(SIGMA0_CFLAGS) 

sigma0/initfs.o: $(BUILD)/initfs.tar
	@echo LD *.tar -o $@.tmp.o
	@$(C4_LD) -r -b binary -o $@.tmp.o $<
	@binary_name="_binary_$$(echo $< | tr '\\/\-. ' _)"; \
	$(CROSS)objcopy --redefine-sym $${binary_name}_start=initfs_start \
	                --redefine-sym $${binary_name}_end=initfs_end \
	                --redefine-sym $${binary_name}_size=initfs_size \
	                $@.tmp.o $@
	@rm $@.tmp.o

$(BUILD)/sigma0-$(ARCH).elf: $(sig-objs)
	@$(C4_CC) -T sigma0/linker.ld \
		-o $@ $(sig-objs) $(sig-libs) $(SIGMA0_CFLAGS) 

$(BUILD)/c4-$(ARCH)-sigma0: $(BUILD)/sigma0-$(ARCH).elf
	@echo OBJCOPY $< $@
	@$(CROSS)objcopy -O binary $< $@

.PHONY: clean
sigma0-clean:
	rm -f $(sig-objs) sigma0-$(ARCH).elf
	rm -f $(BUILD)/sigma0-$(ARCH).elf
	rm -f $(BUILD)/c4-$(ARCH)-sigma0
