#
# parisc/Makefile
#
# This file is included by the global makefile so that you can add your own
# architecture-specific flags and dependencies. Remember to do have actions
# for "archclean" and "archdep" for cleaning up and making dependencies for
# this architecture
#
# This file is subject to the terms and conditions of the GNU General Public
# License.  See the file "COPYING" in the main directory of this archive
# for more details.
#
# Copyright (C) 1994 by Linus Torvalds
# Portions Copyright (C) 1999 The Puffin Group
#
# Modified for PA-RISC Linux by Paul Lahaie, Alex deVries, 
# Mike Shaver, Helge Deller and Martin K. Petersen
#

ifdef CONFIG_PARISC_SELF_EXTRACT
boot := arch/parisc/boot
KBUILD_IMAGE := $(boot)/bzImage
else
KBUILD_IMAGE := vmlinuz
endif

KBUILD_DEFCONFIG := default_defconfig

NM		= sh $(srctree)/arch/parisc/nm
CHECKFLAGS	+= -D__hppa__=1
LIBGCC		= $(shell $(CC) $(KBUILD_CFLAGS) -print-libgcc-file-name)
export LIBGCC

ifdef CONFIG_64BIT
UTS_MACHINE	:= parisc64
CHECKFLAGS	+= -D__LP64__=1
CC_ARCHES	= hppa64
LD_BFD		:= elf64-hppa-linux
else # 32-bit
CC_ARCHES	= hppa hppa2.0 hppa1.1
LD_BFD		:= elf32-hppa-linux
endif

export LD_BFD

ifneq ($(SUBARCH),$(UTS_MACHINE))
	ifeq ($(CROSS_COMPILE),)
		CC_SUFFIXES = linux linux-gnu unknown-linux-gnu
		CROSS_COMPILE := $(call cc-cross-prefix, \
			$(foreach a,$(CC_ARCHES), \
			$(foreach s,$(CC_SUFFIXES),$(a)-$(s)-)))
	endif
endif

OBJCOPY_FLAGS =-O binary -R .note -R .comment -S

cflags-y	:= -pipe

# These flags should be implied by an hppa-linux configuration, but they
# are not in gcc 3.2.
cflags-y	+= -mno-space-regs

# -mfast-indirect-calls is only relevant for 32-bit kernels.
ifndef CONFIG_64BIT
cflags-y	+= -mfast-indirect-calls
endif

# Currently we save and restore fpregs on all kernel entry/interruption paths.
# If that gets optimized, we might need to disable the use of fpregs in the
# kernel.
cflags-y	+= -mdisable-fpregs

# Use long jumps instead of long branches (needed if your linker fails to
# link a too big vmlinux executable). Not enabled for building modules.
ifdef CONFIG_MLONGCALLS
KBUILD_CFLAGS_KERNEL += -mlong-calls
endif

# Without this, "ld -r" results in .text sections that are too big (> 0x40000)
# for branches to reach stubs. And multiple .text sections trigger a warning
# when creating the sysfs module information section.
ifndef CONFIG_64BIT
KBUILD_CFLAGS_MODULE += -ffunction-sections
endif

# select which processor to optimise for
cflags-$(CONFIG_PA7000)		+= -march=1.1 -mschedule=7100
cflags-$(CONFIG_PA7200)		+= -march=1.1 -mschedule=7200
cflags-$(CONFIG_PA7100LC)	+= -march=1.1 -mschedule=7100LC
cflags-$(CONFIG_PA7300LC)	+= -march=1.1 -mschedule=7300
cflags-$(CONFIG_PA8X00)		+= -march=2.0 -mschedule=8000

head-y			:= arch/parisc/kernel/head.o 

KBUILD_CFLAGS	+= $(cflags-y)

kernel-y			:= mm/ kernel/ math-emu/

core-y	+= $(addprefix arch/parisc/, $(kernel-y))
libs-y	+= arch/parisc/lib/ $(LIBGCC)

drivers-$(CONFIG_OPROFILE)		+= arch/parisc/oprofile/

boot	:= arch/parisc/boot

PALO := $(shell if (which palo 2>&1); then : ; \
	elif [ -x /sbin/palo ]; then echo /sbin/palo; \
	fi)

PALOCONF := $(shell if [ -f $(src)/palo.conf ]; then echo $(src)/palo.conf; \
	else echo $(obj)/palo.conf; \
	fi)

palo lifimage: vmlinuz
	@if test ! -x "$(PALO)"; then \
		echo 'ERROR: Please install palo first (apt-get install palo)';\
		echo 'or build it from source and install it somewhere in your $$PATH';\
		false; \
	fi
	@if test ! -f "$(PALOCONF)"; then \
		cp $(src)/arch/parisc/defpalo.conf $(obj)/palo.conf; \
		echo 'A generic palo config file ($(obj)/palo.conf) has been created for you.'; \
		echo 'You should check it and re-run "make palo".'; \
		echo 'WARNING: the "lifimage" file is now placed in this directory by default!'; \
		false; \
	fi
	$(PALO) -f $(PALOCONF)

BOOT_TARGETS    = zImage Image palo lifimage
INSTALL_TARGETS = zinstall install

PHONY += bzImage $(BOOT_TARGETS) $(INSTALL_TARGETS)

# Default kernel to build
all: bzImage

zImage: vmlinuz
Image: vmlinux

bzImage: vmlinux
	$(Q)$(MAKE) $(build)=$(boot) $(boot)/$@

ifdef CONFIG_PARISC_SELF_EXTRACT
vmlinuz: bzImage
	$(OBJCOPY) $(boot)/bzImage $@
else
vmlinuz: vmlinux
	@gzip -cf -9 $< > $@
endif

install:
	$(CONFIG_SHELL) $(src)/arch/parisc/install.sh \
			$(KERNELRELEASE) vmlinux System.map "$(INSTALL_PATH)"
zinstall:
	$(CONFIG_SHELL) $(src)/arch/parisc/install.sh \
			$(KERNELRELEASE) vmlinuz System.map "$(INSTALL_PATH)"

CLEAN_FILES	+= lifimage
MRPROPER_FILES	+= palo.conf

define archhelp
	@echo  '* vmlinux	- Uncompressed kernel image (./vmlinux)'
	@echo  '  vmlinuz	- Compressed kernel image (./vmlinuz)'
	@echo  '  palo		- Bootable image (./lifimage)'
	@echo  '  install	- Install uncompressed vmlinux kernel using'
	@echo  '		  (your) ~/bin/$(INSTALLKERNEL) or'
	@echo  '		  (distribution) /sbin/$(INSTALLKERNEL) or'
	@echo  '		  copy to $$(INSTALL_PATH)'
	@echo  '  zinstall	- Install compressed vmlinuz kernel'
endef

# we require gcc 3.3 or above to compile the kernel
archprepare: checkbin
checkbin:
	@if test "$(cc-version)" -lt "0303"; then \
		echo -n "Sorry, GCC v3.3 or above is required to build " ; \
		echo "the kernel." ; \
		false ; \
	fi
