/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ARCH_POWERPC_UACCESS_H
#define _ARCH_POWERPC_UACCESS_H

#include <asm/ppc_asm.h>
#include <asm/processor.h>
#include <asm/page.h>
#include <asm/extable.h>
#include <asm/kup.h>

/*
 * The fs value determines whether argument validity checking should be
 * performed or not.  If get_fs() == USER_DS, checking is performed, with
 * get_fs() == KERNEL_DS, checking is bypassed.
 *
 * For historical reasons, these macros are grossly misnamed.
 *
 * The fs/ds values are now the highest legal address in the "segment".
 * This simplifies the checking in the routines below.
 */

#define MAKE_MM_SEG(s)  ((mm_segment_t) { (s) })

#define KERNEL_DS	MAKE_MM_SEG(~0UL)
#ifdef __powerpc64__
/* We use TASK_SIZE_USER64 as TASK_SIZE is not constant */
#define USER_DS		MAKE_MM_SEG(TASK_SIZE_USER64 - 1)
#else
#define USER_DS		MAKE_MM_SEG(TASK_SIZE - 1)
#endif

#define get_ds()	(KERNEL_DS)
#define get_fs()	(current->thread.addr_limit)

static inline void set_fs(mm_segment_t fs)
{
	current->thread.addr_limit = fs;
	/* On user-mode return check addr_limit (fs) is correct */
	set_thread_flag(TIF_FSCHECK);
}

#define segment_eq(a, b)	((a).seg == (b).seg)

#define user_addr_max()	(get_fs().seg)

#ifdef __powerpc64__
/*
 * This check is sufficient because there is a large enough
 * gap between user addresses and the kernel addresses
 */
#define __access_ok(addr, size, segment)	\
	(((addr) <= (segment).seg) && ((size) <= (segment).seg))

#else

static inline int __access_ok(unsigned long addr, unsigned long size,
			mm_segment_t seg)
{
	if (addr > seg.seg)
		return 0;
	return (size == 0 || size - 1 <= seg.seg - addr);
}

#endif

#define access_ok(type, addr, size)		\
	(__chk_user_ptr(addr), (void)(type),		\
	 __access_ok((__force unsigned long)(addr), (size), get_fs()))

/*
 * These are the main single-value transfer routines.  They automatically
 * use the right size if we just have the right pointer type.
 *
 * This gets kind of ugly. We want to return _two_ values in "get_user()"
 * and yet we don't want to do any pointers, because that is too much
 * of a performance impact. Thus we have a few rather ugly macros here,
 * and hide all the ugliness from the user.
 *
 * The "__xxx" versions of the user access functions are versions that
 * do not verify the address space, that must have been done previously
 * with a separate "access_ok()" call (this is used when we do multiple
 * accesses to the same area of user memory).
 *
 * As we use the same address space for kernel and user data on the
 * PowerPC, we can just do these as direct assignments.  (Of course, the
 * exception handling means that it's no longer "just"...)
 *
 */
#define get_user(x, ptr) \
	__get_user_check((x), (ptr), sizeof(*(ptr)))
#define put_user(x, ptr) \
	__put_user_check((__typeof__(*(ptr)))(x), (ptr), sizeof(*(ptr)))

#define __get_user(x, ptr) \
	__get_user_nocheck((x), (ptr), sizeof(*(ptr)), true)
#define __put_user(x, ptr) \
	__put_user_nocheck((__typeof__(*(ptr)))(x), (ptr), sizeof(*(ptr)), true)

#define __get_user_allowed(x, ptr) \
	__get_user_nocheck((x), (ptr), sizeof(*(ptr)), false)
#define __put_user_allowed(x, ptr) \
	__put_user_nocheck((__typeof__(*(ptr)))(x), (ptr), sizeof(*(ptr)), false)

#define __get_user_inatomic(x, ptr) \
	__get_user_nosleep((x), (ptr), sizeof(*(ptr)))
#define __put_user_inatomic(x, ptr) \
	__put_user_nosleep((__typeof__(*(ptr)))(x), (ptr), sizeof(*(ptr)))

extern long __put_user_bad(void);

/*
 * We don't tell gcc that we are accessing memory, but this is OK
 * because we do not write to any memory gcc knows about, so there
 * are no aliasing issues.
 */
#define __put_user_asm(x, addr, err, op)			\
	__asm__ __volatile__(					\
		"1:	" op " %1,0(%2)	# put_user\n"		\
		"2:\n"						\
		".section .fixup,\"ax\"\n"			\
		"3:	li %0,%3\n"				\
		"	b 2b\n"					\
		".previous\n"					\
		EX_TABLE(1b, 3b)				\
		: "=r" (err)					\
		: "r" (x), "b" (addr), "i" (-EFAULT), "0" (err))

#ifdef __powerpc64__
#define __put_user_asm2(x, ptr, retval)				\
	  __put_user_asm(x, ptr, retval, "std")
#else /* __powerpc64__ */
#define __put_user_asm2(x, addr, err)				\
	__asm__ __volatile__(					\
		"1:	stw %1,0(%2)\n"				\
		"2:	stw %1+1,4(%2)\n"			\
		"3:\n"						\
		".section .fixup,\"ax\"\n"			\
		"4:	li %0,%3\n"				\
		"	b 3b\n"					\
		".previous\n"					\
		EX_TABLE(1b, 4b)				\
		EX_TABLE(2b, 4b)				\
		: "=r" (err)					\
		: "r" (x), "b" (addr), "i" (-EFAULT), "0" (err))
#endif /* __powerpc64__ */

#define __put_user_size_allowed(x, ptr, size, retval)		\
do {								\
	retval = 0;						\
	switch (size) {						\
	  case 1: __put_user_asm(x, ptr, retval, "stb"); break;	\
	  case 2: __put_user_asm(x, ptr, retval, "sth"); break;	\
	  case 4: __put_user_asm(x, ptr, retval, "stw"); break;	\
	  case 8: __put_user_asm2(x, ptr, retval); break;	\
	  default: __put_user_bad();				\
	}							\
} while (0)

#define __put_user_size(x, ptr, size, retval)			\
do {								\
	allow_write_to_user(ptr, size);				\
	__put_user_size_allowed(x, ptr, size, retval);		\
	prevent_write_to_user(ptr, size);			\
} while (0)

#define __put_user_nocheck(x, ptr, size, do_allow)			\
({								\
	long __pu_err;						\
	__typeof__(*(ptr)) __user *__pu_addr = (ptr);		\
	__typeof__(*(ptr)) __pu_val = (x);			\
	__typeof__(size) __pu_size = (size);			\
								\
	if (!is_kernel_addr((unsigned long)__pu_addr))		\
		might_fault();					\
	__chk_user_ptr(__pu_addr);				\
	if (do_allow)								\
		__put_user_size(__pu_val, __pu_addr, __pu_size, __pu_err);	\
	else									\
		__put_user_size_allowed(__pu_val, __pu_addr, __pu_size, __pu_err); \
								\
	__pu_err;						\
})

#define __put_user_check(x, ptr, size)					\
({									\
	long __pu_err = -EFAULT;					\
	__typeof__(*(ptr)) __user *__pu_addr = (ptr);			\
	__typeof__(*(ptr)) __pu_val = (x);				\
	__typeof__(size) __pu_size = (size);				\
									\
	might_fault();							\
	if (access_ok(VERIFY_WRITE, __pu_addr, __pu_size))			\
		__put_user_size(__pu_val, __pu_addr, __pu_size, __pu_err); \
									\
	__pu_err;							\
})

#define __put_user_nosleep(x, ptr, size)			\
({								\
	long __pu_err;						\
	__typeof__(*(ptr)) __user *__pu_addr = (ptr);		\
	__typeof__(*(ptr)) __pu_val = (x);			\
	__typeof__(size) __pu_size = (size);			\
								\
	__chk_user_ptr(__pu_addr);				\
	__put_user_size(__pu_val, __pu_addr, __pu_size, __pu_err); \
								\
	__pu_err;						\
})


extern long __get_user_bad(void);

/*
 * This does an atomic 128 byte aligned load from userspace.
 * Upto caller to do enable_kernel_vmx() before calling!
 */
#define __get_user_atomic_128_aligned(kaddr, uaddr, err)		\
	__asm__ __volatile__(				\
		".machine push\n"			\
		".machine altivec\n"			\
		"1:	lvx  0,0,%1	# get user\n"	\
		" 	stvx 0,0,%2	# put kernel\n"	\
		".machine pop\n"			\
		"2:\n"					\
		".section .fixup,\"ax\"\n"		\
		"3:	li %0,%3\n"			\
		"	b 2b\n"				\
		".previous\n"				\
		EX_TABLE(1b, 3b)			\
		: "=r" (err)			\
		: "b" (uaddr), "b" (kaddr), "i" (-EFAULT), "0" (err))

#define __get_user_asm(x, addr, err, op)		\
	__asm__ __volatile__(				\
		"1:	"op" %1,0(%2)	# get_user\n"	\
		"2:\n"					\
		".section .fixup,\"ax\"\n"		\
		"3:	li %0,%3\n"			\
		"	li %1,0\n"			\
		"	b 2b\n"				\
		".previous\n"				\
		EX_TABLE(1b, 3b)			\
		: "=r" (err), "=r" (x)			\
		: "b" (addr), "i" (-EFAULT), "0" (err))

#ifdef __powerpc64__
#define __get_user_asm2(x, addr, err)			\
	__get_user_asm(x, addr, err, "ld")
#else /* __powerpc64__ */
#define __get_user_asm2(x, addr, err)			\
	__asm__ __volatile__(				\
		"1:	lwz %1,0(%2)\n"			\
		"2:	lwz %1+1,4(%2)\n"		\
		"3:\n"					\
		".section .fixup,\"ax\"\n"		\
		"4:	li %0,%3\n"			\
		"	li %1,0\n"			\
		"	li %1+1,0\n"			\
		"	b 3b\n"				\
		".previous\n"				\
		EX_TABLE(1b, 4b)			\
		EX_TABLE(2b, 4b)			\
		: "=r" (err), "=&r" (x)			\
		: "b" (addr), "i" (-EFAULT), "0" (err))
#endif /* __powerpc64__ */

#define __get_user_size_allowed(x, ptr, size, retval)		\
do {								\
	retval = 0;						\
	__chk_user_ptr(ptr);					\
	if (size > sizeof(x))					\
		(x) = __get_user_bad();				\
	switch (size) {						\
	case 1: __get_user_asm(x, ptr, retval, "lbz"); break;	\
	case 2: __get_user_asm(x, ptr, retval, "lhz"); break;	\
	case 4: __get_user_asm(x, ptr, retval, "lwz"); break;	\
	case 8: __get_user_asm2(x, ptr, retval);  break;	\
	default: (x) = __get_user_bad();			\
	}							\
} while (0)

#define __get_user_size(x, ptr, size, retval)			\
do {								\
	allow_read_from_user(ptr, size);			\
	__get_user_size_allowed(x, ptr, size, retval);		\
	prevent_read_from_user(ptr, size);			\
} while (0)

/*
 * This is a type: either unsigned long, if the argument fits into
 * that type, or otherwise unsigned long long.
 */
#define __long_type(x) \
	__typeof__(__builtin_choose_expr(sizeof(x) > sizeof(0UL), 0ULL, 0UL))

#define __get_user_nocheck(x, ptr, size, do_allow)			\
({								\
	long __gu_err;						\
	__long_type(*(ptr)) __gu_val;				\
	__typeof__(*(ptr)) __user *__gu_addr = (ptr);	\
	__typeof__(size) __gu_size = (size);			\
								\
	__chk_user_ptr(__gu_addr);				\
	if (!is_kernel_addr((unsigned long)__gu_addr))		\
		might_fault();					\
	barrier_nospec();					\
	if (do_allow)								\
		__get_user_size(__gu_val, __gu_addr, __gu_size, __gu_err);	\
	else									\
		__get_user_size_allowed(__gu_val, __gu_addr, __gu_size, __gu_err); \
	(x) = (__typeof__(*(ptr)))__gu_val;			\
								\
	__gu_err;						\
})

#define __get_user_check(x, ptr, size)					\
({									\
	long __gu_err = -EFAULT;					\
	__long_type(*(ptr)) __gu_val = 0;				\
	__typeof__(*(ptr)) __user *__gu_addr = (ptr);		\
	__typeof__(size) __gu_size = (size);				\
									\
	might_fault();							\
	if (access_ok(VERIFY_READ, __gu_addr, __gu_size)) {		\
		barrier_nospec();					\
		__get_user_size(__gu_val, __gu_addr, __gu_size, __gu_err); \
	}								\
	(x) = (__force __typeof__(*(ptr)))__gu_val;				\
									\
	__gu_err;							\
})

#define __get_user_nosleep(x, ptr, size)			\
({								\
	long __gu_err;						\
	__long_type(*(ptr)) __gu_val;				\
	__typeof__(*(ptr)) __user *__gu_addr = (ptr);	\
	__typeof__(size) __gu_size = (size);			\
								\
	__chk_user_ptr(__gu_addr);				\
	barrier_nospec();					\
	__get_user_size(__gu_val, __gu_addr, __gu_size, __gu_err); \
	(x) = (__force __typeof__(*(ptr)))__gu_val;			\
								\
	__gu_err;						\
})


/* more complex routines */

extern unsigned long __copy_tofrom_user(void __user *to,
		const void __user *from, unsigned long size);

#ifdef __powerpc64__
static inline unsigned long
raw_copy_in_user(void __user *to, const void __user *from, unsigned long n)
{
	unsigned long ret;

	barrier_nospec();
	allow_user_access(to, from, n);
	ret = __copy_tofrom_user(to, from, n);
	prevent_user_access(to, from, n);
	return ret;
}
#endif /* __powerpc64__ */

static inline unsigned long raw_copy_from_user(void *to,
		const void __user *from, unsigned long n)
{
	unsigned long ret;
	if (__builtin_constant_p(n) && (n <= 8)) {
		ret = 1;

		switch (n) {
		case 1:
			barrier_nospec();
			__get_user_size(*(u8 *)to, from, 1, ret);
			break;
		case 2:
			barrier_nospec();
			__get_user_size(*(u16 *)to, from, 2, ret);
			break;
		case 4:
			barrier_nospec();
			__get_user_size(*(u32 *)to, from, 4, ret);
			break;
		case 8:
			barrier_nospec();
			__get_user_size(*(u64 *)to, from, 8, ret);
			break;
		}
		if (ret == 0)
			return 0;
	}

	barrier_nospec();
	allow_read_from_user(from, n);
	ret = __copy_tofrom_user((__force void __user *)to, from, n);
	prevent_read_from_user(from, n);
	return ret;
}

static inline unsigned long
raw_copy_to_user_allowed(void __user *to, const void *from, unsigned long n)
{
	if (__builtin_constant_p(n) && (n <= 8)) {
		unsigned long ret = 1;

		switch (n) {
		case 1:
			__put_user_size_allowed(*(u8 *)from, (u8 __user *)to, 1, ret);
			break;
		case 2:
			__put_user_size_allowed(*(u16 *)from, (u16 __user *)to, 2, ret);
			break;
		case 4:
			__put_user_size_allowed(*(u32 *)from, (u32 __user *)to, 4, ret);
			break;
		case 8:
			__put_user_size_allowed(*(u64 *)from, (u64 __user *)to, 8, ret);
			break;
		}
		if (ret == 0)
			return 0;
	}

	return __copy_tofrom_user(to, (__force const void __user *)from, n);
}

static inline unsigned long
raw_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	unsigned long ret;

	allow_write_to_user(to, n);
	ret = raw_copy_to_user_allowed(to, from, n);
	prevent_write_to_user(to, n);
	return ret;
}

unsigned long __arch_clear_user(void __user *addr, unsigned long size);

static inline unsigned long clear_user(void __user *addr, unsigned long size)
{
	unsigned long ret = size;
	might_fault();
	if (likely(access_ok(VERIFY_WRITE, addr, size))) {
		allow_write_to_user(addr, size);
		ret = __arch_clear_user(addr, size);
		prevent_write_to_user(addr, size);
	}
	return ret;
}

static inline unsigned long __clear_user(void __user *addr, unsigned long size)
{
	return clear_user(addr, size);
}

extern long strncpy_from_user(char *dst, const char __user *src, long count);
extern __must_check long strnlen_user(const char __user *str, long n);

extern long __copy_from_user_flushcache(void *dst, const void __user *src,
		unsigned size);
extern void memcpy_page_flushcache(char *to, struct page *page, size_t offset,
			   size_t len);

#define user_access_begin(type, ptr, len) access_ok(type, ptr, len)
#define user_access_end()		  prevent_user_access(NULL, NULL, ~0ul)

#define unsafe_op_wrap(op, err) do { if (unlikely(op)) goto err; } while (0)
#define unsafe_get_user(x, p, e) unsafe_op_wrap(__get_user_allowed(x, p), e)
#define unsafe_put_user(x, p, e) unsafe_op_wrap(__put_user_allowed(x, p), e)
#define unsafe_copy_to_user(d, s, l, e) \
	unsafe_op_wrap(raw_copy_to_user_allowed(d, s, l), e)

#endif	/* _ARCH_POWERPC_UACCESS_H */
