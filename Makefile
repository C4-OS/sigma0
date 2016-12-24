# these paths should be provided by the parent makefile
KERNEL_ROOT    = 
INITFS_TARBALL =
CROSS          =

SIGMA0_INCLUDE = -I./include/
MINIFT_INCLUDE = -I./miniforth/include/
KERNEL_INCLUDE = -I$(KERNEL_ROOT)/include/ \
				 -I$(KERNEL_ROOT)/arch/$(ARCH)/include/

KERN_CC = $(CROSS)gcc
KERN_LD = $(CROSS)ld

SIGMA0_CFLAGS  = -Wall -g -O2 -ffreestanding -nostdlib -nodefaultlibs \
				 -nostartfiles -fno-builtin -fpie -fpic \
				 $(SIGMA0_INCLUDE) $(MINIFT_INCLUDE) $(KERNEL_INCLUDE)

sig-objs  = sigma0.o display.o tar.o elf.o
sig-objs += miniforth/out/miniforth.a
sig-objs += init_commands.o
sig-objs += initfs.o

.PHONY: all
all: c4-$(ARCH)-sigma0

%.o: %.c
	@echo CC $< -c -o $@
	@$(KERN_CC) $(SIGMA0_CFLAGS) $< -c -o $@

%.o: %.fs
	@echo LD $< -o $@
	@$(KERN_LD) -r -b binary -o $@ $<

initfs.o: $(INITFS_TARBALL)
	@cp $(INITFS_TARBALL) .
	@echo LD *.tar -o $@
	@$(KERN_LD) -r -b binary -o $@ *.tar

miniforth/out/miniforth.a:
	@cd miniforth; make lib CC=$(KERN_CC) CFLAGS="$(SIGMA0_CFLAGS)";

sigma0-$(ARCH).elf: $(sig-objs)
	@$(KERN_CC) $(SIGMA0_CFLAGS) -T linker.ld $(sig-objs) -o $@

c4-$(ARCH)-sigma0: sigma0-$(ARCH).elf
	@echo CC $< -o $@
	@$(CROSS)objcopy -O binary $< $@

.PHONY: clean
clean:
	rm -rf c4-$(ARCH)-sigma0 $(sig-objs) sigma0-$(ARCH).elf \
		miniforth/out/miniforth.a $(user-obj) *.tar
