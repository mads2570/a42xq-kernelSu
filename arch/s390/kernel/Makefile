# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the linux kernel.
#

ifdef CONFIG_FUNCTION_TRACER

# Do not trace tracer code
CFLAGS_REMOVE_ftrace.o		= $(CC_FLAGS_FTRACE)

# Do not trace early setup code
CFLAGS_REMOVE_early.o		= $(CC_FLAGS_FTRACE)
CFLAGS_REMOVE_early_nobss.o	= $(CC_FLAGS_FTRACE)

endif

GCOV_PROFILE_early.o		:= n
GCOV_PROFILE_early_nobss.o	:= n

KCOV_INSTRUMENT_early.o		:= n
KCOV_INSTRUMENT_early_nobss.o	:= n

UBSAN_SANITIZE_early.o		:= n
UBSAN_SANITIZE_early_nobss.o	:= n

KASAN_SANITIZE_early_nobss.o	:= n

#
# Passing null pointers is ok for smp code, since we access the lowcore here.
#
CFLAGS_smp.o		:= -Wno-nonnull

#
# Disable tailcall optimizations for stack / callchain walking functions
# since this might generate broken code when accessing register 15 and
# passing its content to other functions.
#
CFLAGS_stacktrace.o	+= -fno-optimize-sibling-calls
CFLAGS_dumpstack.o	+= -fno-optimize-sibling-calls

#
# Pass UTS_MACHINE for user_regset definition
#
CFLAGS_ptrace.o		+= -DUTS_MACHINE='"$(UTS_MACHINE)"'

obj-y	:= traps.o time.o process.o base.o early.o setup.o idle.o vtime.o
obj-y	+= processor.o sys_s390.o ptrace.o signal.o cpcmd.o ebcdic.o nmi.o
obj-y	+= debug.o irq.o ipl.o dis.o diag.o vdso.o early_nobss.o
obj-y	+= sysinfo.o lgr.o os_info.o machine_kexec.o pgm_check.o
obj-y	+= runtime_instr.o cache.o fpu.o dumpstack.o guarded_storage.o sthyi.o
obj-y	+= entry.o reipl.o relocate_kernel.o kdebugfs.o alternative.o
obj-y	+= nospec-branch.o

extra-y				+= head64.o vmlinux.lds

obj-$(CONFIG_SYSFS)		+= nospec-sysfs.o
CFLAGS_REMOVE_nospec-branch.o	+= $(CC_FLAGS_EXPOLINE)

obj-$(CONFIG_MODULES)		+= module.o
obj-$(CONFIG_SMP)		+= smp.o
obj-$(CONFIG_SCHED_TOPOLOGY)	+= topology.o
obj-$(CONFIG_HIBERNATION)	+= suspend.o swsusp.o
obj-$(CONFIG_AUDIT)		+= audit.o
compat-obj-$(CONFIG_AUDIT)	+= compat_audit.o
obj-$(CONFIG_COMPAT)		+= compat_linux.o compat_signal.o
obj-$(CONFIG_COMPAT)		+= compat_wrapper.o $(compat-obj-y)
obj-$(CONFIG_EARLY_PRINTK)	+= early_printk.o
obj-$(CONFIG_STACKTRACE)	+= stacktrace.o
obj-$(CONFIG_KPROBES)		+= kprobes.o
obj-$(CONFIG_FUNCTION_TRACER)	+= mcount.o ftrace.o
obj-$(CONFIG_CRASH_DUMP)	+= crash_dump.o
obj-$(CONFIG_UPROBES)		+= uprobes.o
obj-$(CONFIG_JUMP_LABEL)	+= jump_label.o

obj-$(CONFIG_KEXEC_FILE)	+= machine_kexec_file.o kexec_image.o
obj-$(CONFIG_KEXEC_FILE)	+= kexec_elf.o

obj-$(CONFIG_PERF_EVENTS)	+= perf_event.o perf_cpum_cf.o perf_cpum_sf.o
obj-$(CONFIG_PERF_EVENTS)	+= perf_cpum_cf_events.o perf_regs.o

obj-$(CONFIG_TRACEPOINTS)	+= trace.o

# vdso
obj-y				+= vdso64/
obj-$(CONFIG_COMPAT)		+= vdso32/

chkbss := head64.o early_nobss.o
include $(srctree)/arch/s390/scripts/Makefile.chkbss
