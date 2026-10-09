/*
 * atomic_ops(3) and membar_ops(3) for the rump kernel on m68k AmigaOS.
 *
 * NetBSD/m68k implements these in assembly; here GCC's __atomic builtins
 * do the same job (they compile to CAS on 68020 and up, which Emu68
 * provides).  Emu68 runs the emulated CPU on a single core, so the memory
 * barriers only have to stop the compiler from reordering.
 *
 * 64-bit variants are deliberately absent: NetBSD/m68k does not define
 * __HAVE_ATOMIC64_OPS either.
 */

#include <sys/types.h>
#include <sys/atomic.h>

#define	ORDER	__ATOMIC_SEQ_CST

#define	DEF_ADD(name, tret, targ, tdelta)				\
void									\
atomic_add_##name(volatile targ *p, tdelta d)				\
{									\
	(void)__atomic_fetch_add((volatile tret *)p, d, ORDER);		\
}									\
tret									\
atomic_add_##name##_nv(volatile targ *p, tdelta d)			\
{									\
	return __atomic_add_fetch((volatile tret *)p, d, ORDER);	\
}

#define	DEF_BITOP(op, name, tret, targ, tval)				\
void									\
atomic_##op##_##name(volatile targ *p, tval v)				\
{									\
	(void)__atomic_fetch_##op((volatile tret *)p, v, ORDER);	\
}									\
tret									\
atomic_##op##_##name##_nv(volatile targ *p, tval v)			\
{									\
	return __atomic_##op##_fetch((volatile tret *)p, v, ORDER);	\
}

#define	DEF_CAS(name, tret, targ, tval)					\
tret									\
atomic_cas_##name(volatile targ *p, tval expected, tval new)		\
{									\
	tret e = (tret)expected;					\
	__atomic_compare_exchange_n((volatile tret *)p, &e, (tret)new,	\
	    0, ORDER, ORDER);						\
	return e;							\
}									\
tret									\
atomic_cas_##name##_ni(volatile targ *p, tval expected, tval new)	\
{									\
	return atomic_cas_##name(p, expected, new);			\
}

#define	DEF_SWAP(name, tret, targ, tval)				\
tret									\
atomic_swap_##name(volatile targ *p, tval v)				\
{									\
	return __atomic_exchange_n((volatile tret *)p, (tret)v, ORDER);	\
}

#define	DEF_INCDEC(name, tret, targ)					\
void									\
atomic_inc_##name(volatile targ *p)					\
{									\
	(void)__atomic_fetch_add((volatile uintptr_t *)p, 1, ORDER);	\
}									\
tret									\
atomic_inc_##name##_nv(volatile targ *p)				\
{									\
	return (tret)__atomic_add_fetch((volatile uintptr_t *)p, 1, ORDER); \
}									\
void									\
atomic_dec_##name(volatile targ *p)					\
{									\
	(void)__atomic_fetch_sub((volatile uintptr_t *)p, 1, ORDER);	\
}									\
tret									\
atomic_dec_##name##_nv(volatile targ *p)				\
{									\
	return (tret)__atomic_sub_fetch((volatile uintptr_t *)p, 1, ORDER); \
}

DEF_ADD(32, uint32_t, uint32_t, int32_t)
DEF_ADD(int, unsigned int, unsigned int, int)
DEF_ADD(long, unsigned long, unsigned long, long)
DEF_ADD(ptr, void *, void, ssize_t)

DEF_BITOP(and, 32, uint32_t, uint32_t, uint32_t)
DEF_BITOP(and, uint, unsigned int, unsigned int, unsigned int)
DEF_BITOP(and, ulong, unsigned long, unsigned long, unsigned long)
DEF_BITOP(or, 32, uint32_t, uint32_t, uint32_t)
DEF_BITOP(or, uint, unsigned int, unsigned int, unsigned int)
DEF_BITOP(or, ulong, unsigned long, unsigned long, unsigned long)

DEF_CAS(32, uint32_t, uint32_t, uint32_t)
DEF_CAS(uint, unsigned int, unsigned int, unsigned int)
DEF_CAS(ulong, unsigned long, unsigned long, unsigned long)
DEF_CAS(ptr, void *, void, void *)

DEF_SWAP(32, uint32_t, uint32_t, uint32_t)
DEF_SWAP(uint, unsigned int, unsigned int, unsigned int)
DEF_SWAP(ulong, unsigned long, unsigned long, unsigned long)
DEF_SWAP(ptr, void *, void, void *)

DEF_INCDEC(32, uint32_t, uint32_t)
DEF_INCDEC(uint, unsigned int, unsigned int)
DEF_INCDEC(ulong, unsigned long, unsigned long)
DEF_INCDEC(ptr, void *, void)

uint16_t
atomic_cas_16(volatile uint16_t *p, uint16_t expected, uint16_t new)
{
	__atomic_compare_exchange_n(p, &expected, new, 0, ORDER, ORDER);
	return expected;
}

uint8_t
atomic_cas_8(volatile uint8_t *p, uint8_t expected, uint8_t new)
{
	__atomic_compare_exchange_n(p, &expected, new, 0, ORDER, ORDER);
	return expected;
}

#define	DEF_MEMBAR(name)						\
void									\
membar_##name(void)							\
{									\
	__atomic_signal_fence(ORDER);					\
}

DEF_MEMBAR(acquire)
DEF_MEMBAR(release)
DEF_MEMBAR(producer)
DEF_MEMBAR(consumer)
DEF_MEMBAR(sync)
DEF_MEMBAR(enter)
DEF_MEMBAR(exit)
