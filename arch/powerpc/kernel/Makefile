# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the linux kernel.
#

CFLAGS_ptrace.o		+= -DUTS_MACHINE='"$(UTS_MACHINE)"'

subdir-ccflags-$(CONFIG_PPC_WERROR) := -Werror

ifdef CONFIG_PPC64
CFLAGS_prom_init.o	+= $(NO_MINIMAL_TOC)
endif
ifdef CONFIG_PPC32
CFLAGS_prom_init.o      += -fPIC
CFLAGS_btext.o		+= -fPIC
endif

CFLAGS_setup_32.o += $(DISABLE_LATENT_ENTROPY_PLUGIN)
CFLAGS_cputable.o += $(DISABLE_LATENT_ENTROPY_PLUGIN)
CFLAGS_prom_init.o += $(DISABLE_LATENT_ENTROPY_PLUGIN)
CFLAGS_btext.o += $(DISABLE_LATENT_ENTROPY_PLUGIN)
CFLAGS_prom.o += $(DISABLE_LATENT_ENTROPY_PLUGIN)

ifdef CONFIG_FUNCTION_TRACER
# Do not trace early boot code
CFLAGS_REMOVE_cputable.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_prom_init.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_btext.o = $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_prom.o = $(CC_FLAGS_FTRACE)
endif

obj-y				:= cputable.o ptrace.o syscalls.o \
				   irq.o align.o signal_32.o pmc.o vdso.o \
				   process.o systbl.o idle.o \
				   signal.o sysfs.o cacheinfo.o time.o \
				   prom.o traps.o setup-common.o \
				   udbg.o misc.o io.o dma.o misc_$(BITS).o \
				   of_platform.o prom_parse.o
obj-$(CONFIG_PPC64)		+= setup_64.o sys_ppc32.o \
				   signal_64.o ptrace32.o \
				   paca.o nvram_64.o firmware.o
obj-$(CONFIG_VDSO32)		+= vdso32/
obj-$(CONFIG_PPC_WATCHDOG)	+= watchdog.o
obj-$(CONFIG_HAVE_HW_BREAKPOINT)	+= hw_breakpoint.o
obj-$(CONFIG_PPC_BOOK3S_64)	+= cpu_setup_ppc970.o cpu_setup_pa6t.o
obj-$(CONFIG_PPC_BOOK3S_64)	+= cpu_setup_power.o
obj-$(CONFIG_PPC_BOOK3S_64)	+= mce.o mce_power.o
obj-$(CONFIG_PPC_BOOK3E_64)	+= exceptions-64e.o idle_book3e.o
obj-$(CONFIG_PPC_BARRIER_NOSPEC) += security.o
obj-$(CONFIG_PPC64)		+= vdso64/
obj-$(CONFIG_ALTIVEC)		+= vecemu.o
obj-$(CONFIG_PPC_970_NAP)	+= idle_power4.o
obj-$(CONFIG_PPC_P7_NAP)	+= idle_book3s.o
procfs-y			:= proc_powerpc.o
obj-$(CONFIG_PROC_FS)		+= $(procfs-y)
rtaspci-$(CONFIG_PPC64)-$(CONFIG_PCI)	:= rtas_pci.o
obj-$(CONFIG_PPC_RTAS)		+= rtas.o rtas-rtc.o $(rtaspci-y-y)
obj-$(CONFIG_PPC_RTAS_DAEMON)	+= rtasd.o
obj-$(CONFIG_RTAS_FLASH)	+= rtas_flash.o
obj-$(CONFIG_RTAS_PROC)		+= rtas-proc.o
obj-$(CONFIG_PPC_DT_CPU_FTRS)	+= dt_cpu_ftrs.o
obj-$(CONFIG_EEH)              += eeh.o eeh_pe.o eeh_dev.o eeh_cache.o \
				  eeh_driver.o eeh_event.o eeh_sysfs.o
obj-$(CONFIG_GENERIC_TBSYNC)	+= smp-tbsync.o
obj-$(CONFIG_CRASH_DUMP)	+= crash_dump.o
obj-$(CONFIG_FA_DUMP)		+= fadump.o
ifdef CONFIG_PPC32
obj-$(CONFIG_E500)		+= idle_e500.o
endif
obj-$(CONFIG_6xx)		+= idle_6xx.o l2cr_6xx.o cpu_setup_6xx.o
obj-$(CONFIG_TAU)		+= tau_6xx.o
obj-$(CONFIG_HIBERNATION)	+= swsusp.o suspend.o
ifdef CONFIG_FSL_BOOKE
obj-$(CONFIG_HIBERNATION)	+= swsusp_booke.o
else
obj-$(CONFIG_HIBERNATION)	+= swsusp_$(BITS).o
endif
obj64-$(CONFIG_HIBERNATION)	+= swsusp_asm64.o
obj-$(CONFIG_MODULES)		+= module.o module_$(BITS).o
obj-$(CONFIG_44x)		+= cpu_setup_44x.o
obj-$(CONFIG_PPC_FSL_BOOK3E)	+= cpu_setup_fsl_booke.o
obj-$(CONFIG_PPC_DOORBELL)	+= dbell.o
obj-$(CONFIG_JUMP_LABEL)	+= jump_label.o

extra-y				:= head_$(BITS).o
extra-$(CONFIG_40x)		:= head_40x.o
extra-$(CONFIG_44x)		:= head_44x.o
extra-$(CONFIG_FSL_BOOKE)	:= head_fsl_booke.o
extra-$(CONFIG_PPC_8xx)		:= head_8xx.o
extra-y				+= vmlinux.lds

obj-$(CONFIG_RELOCATABLE)	+= reloc_$(BITS).o

obj-$(CONFIG_PPC32)		+= entry_32.o setup_32.o
obj-$(CONFIG_PPC64)		+= dma-iommu.o iommu.o
obj-$(CONFIG_KGDB)		+= kgdb.o
obj-$(CONFIG_BOOTX_TEXT)	+= btext.o
obj-$(CONFIG_SMP)		+= smp.o
obj-$(CONFIG_KPROBES)		+= kprobes.o
obj-$(CONFIG_OPTPROBES)		+= optprobes.o optprobes_head.o
obj-$(CONFIG_KPROBES_ON_FTRACE)	+= kprobes-ftrace.o
obj-$(CONFIG_UPROBES)		+= uprobes.o
obj-$(CONFIG_PPC_UDBG_16550)	+= legacy_serial.o udbg_16550.o
obj-$(CONFIG_STACKTRACE)	+= stacktrace.o
obj-$(CONFIG_SWIOTLB)		+= dma-swiotlb.o

pci64-$(CONFIG_PPC64)		+= pci_dn.o pci-hotplug.o isa-bridge.o
obj-$(CONFIG_PCI)		+= pci_$(BITS).o $(pci64-y) \
				   pci-common.o pci_of_scan.o
obj-$(CONFIG_PCI_MSI)		+= msi.o
obj-$(CONFIG_KEXEC_CORE)	+= machine_kexec.o crash.o \
				   machine_kexec_$(BITS).o
obj-$(CONFIG_KEXEC_FILE)	+= machine_kexec_file_$(BITS).o kexec_elf_$(BITS).o
ifdef CONFIG_HAVE_IMA_KEXEC
ifdef CONFIG_IMA
obj-y				+= ima_kexec.o
endif
endif

obj-$(CONFIG_AUDIT)		+= audit.o
obj64-$(CONFIG_AUDIT)		+= compat_audit.o

obj-$(CONFIG_PPC_IO_WORKAROUNDS)	+= io-workarounds.o

obj-y				+= trace/

ifneq ($(CONFIG_PPC_INDIRECT_PIO),y)
obj-y				+= iomap.o
endif

obj64-$(CONFIG_PPC_TRANSACTIONAL_MEM)	+= tm.o

obj-$(CONFIG_PPC64)		+= $(obj64-y)
obj-$(CONFIG_PPC32)		+= $(obj32-y)

ifneq ($(CONFIG_XMON)$(CONFIG_KEXEC_CORE)(CONFIG_PPC_BOOK3S),)
obj-y				+= ppc_save_regs.o
endif

obj-$(CONFIG_EPAPR_PARAVIRT)	+= epapr_paravirt.o epapr_hcalls.o
obj-$(CONFIG_KVM_GUEST)		+= kvm.o kvm_emul.o

# Disable GCOV & sanitizers in odd or sensitive code
GCOV_PROFILE_prom_init.o := n
UBSAN_SANITIZE_prom_init.o := n
GCOV_PROFILE_machine_kexec_64.o := n
UBSAN_SANITIZE_machine_kexec_64.o := n
GCOV_PROFILE_machine_kexec_32.o := n
UBSAN_SANITIZE_machine_kexec_32.o := n
GCOV_PROFILE_kprobes.o := n
UBSAN_SANITIZE_kprobes.o := n
GCOV_PROFILE_kprobes-ftrace.o := n
UBSAN_SANITIZE_kprobes-ftrace.o := n
UBSAN_SANITIZE_vdso.o := n

extra-$(CONFIG_PPC_FPU)		+= fpu.o
extra-$(CONFIG_ALTIVEC)		+= vector.o
extra-$(CONFIG_PPC64)		+= entry_64.o
extra-$(CONFIG_PPC_OF_BOOT_TRAMPOLINE)	+= prom_init.o

extra-y				+= systbl_chk.i
$(obj)/systbl.o:		systbl_chk

quiet_cmd_systbl_chk = CALL    $<
      cmd_systbl_chk = $(CONFIG_SHELL) $< $(obj)/systbl_chk.i

PHONY += systbl_chk
systbl_chk: $(src)/systbl_chk.sh $(obj)/systbl_chk.i
	$(call cmd,systbl_chk)

ifdef CONFIG_PPC_OF_BOOT_TRAMPOLINE
$(obj)/built-in.a:		prom_init_check

quiet_cmd_prom_init_check = CALL    $<
      cmd_prom_init_check = $(CONFIG_SHELL) $< "$(NM)" "$(obj)/prom_init.o"

PHONY += prom_init_check
prom_init_check: $(src)/prom_init_check.sh $(obj)/prom_init.o
	$(call cmd,prom_init_check)
endif

clean-files := vmlinux.lds
