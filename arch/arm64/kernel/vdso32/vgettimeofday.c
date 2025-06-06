// SPDX-License-Identifier: GPL-2.0
/*
 * ARM64 compat userspace implementations of gettimeofday() and similar.
 *
 * Copyright (C) 2018 ARM Limited
 *
 */

int __vdso_clock_gettime(clockid_t clock,
			 struct old_timespec32 *ts)
{
	return __cvdso_clock_gettime32(clock, ts);
}

int __vdso_clock_gettime64(clockid_t clock,
			   struct __kernel_timespec *ts)
{
	return __cvdso_clock_gettime(clock, ts);
}

int __vdso_gettimeofday(struct __kernel_old_timeval *tv,
			struct timezone *tz)
{
	return __cvdso_gettimeofday(tv, tz);
}

int __vdso_clock_getres(clockid_t clock_id,
			struct old_timespec32 *res)
{
	return __cvdso_clock_getres_time32(clock_id, res);
}

time_t __vdso_time(time_t *time)
{
	return __cvdso_time(time);
}

/* Avoid unresolved references emitted by GCC */

void __aeabi_unwind_cpp_pr0(void)
{
}

void __aeabi_unwind_cpp_pr1(void)
{
}

void __aeabi_unwind_cpp_pr2(void)
{
}
