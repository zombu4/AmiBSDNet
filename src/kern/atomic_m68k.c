/*
 * atomic_ops(3) and membar_ops(3) for the rump kernel on m68k AmigaOS.
 *
 * NetBSD/m68k implements these in assembly; here GCC's __atomic builtins
 * do the same job (they compile to CAS on 68020 and up, which Emu68
 * provides).  Emu68 runs the emulated CPU on a single core, so the memory
 * barriers only have to stop the compiler from reordering.
 *
 * CAS needs an aligned operand: on a 68060, CAS with a misaligned
 * effective address is one of the instructions the CPU traps to
 * Motorola's software package (68060SP isp.doc,
 * downloads/sources/linux-m68k/isp.doc:46,174), and Emu68 translates a
 * CAS whose address is not aligned to its size to a sequence that is not
 * atomic (Emu68 src/M68k_LINE0.c:2752-2808, CAS_UNSAFE()).  m68k aligns
 * 32-bit members of kernel structures to 2 bytes only, so a misaligned
 * operand is done inside rumpuser_amiga_atomic_begin()/_end() (Disable()
 * on the host: src/host/rumpuser_amiga.c) instead.
 *
 * 64-bit variants are deliberately absent: NetBSD/m68k does not define
 * __HAVE_ATOMIC64_OPS either.
 */

#include <sys/types.h>
#include <sys/atomic.h>

/* host side (src/host/rumpuser_amiga.c; the rump prefix keeps the names
   out of the rumpns_ rename) */
void	rumpuser_amiga_atomic_begin(void);
void	rumpuser_amiga_atomic_end(void);

#define	ORDER	__ATOMIC_SEQ_CST

#define	MISALIGNED(p, t)	(((uintptr_t)(p) & (sizeof(t) - 1)) != 0)

/* the read-modify-write of a misaligned word, as one section */
#define	SLOW_OP(t, rt, p, expr, retold)					\
do {									\
	typedef t slow_t_;						\
	volatile slow_t_ *q_ = (volatile slow_t_ *)(p);			\
	slow_t_ o_, n_;							\
									\
	rumpuser_amiga_atomic_begin();					\
	o_ = *q_;							\
	n_ = (expr);							\
	*q_ = n_;							\
	rumpuser_amiga_atomic_end();					\
	return (rt)((retold) ? o_ : n_);				\
} while (0)

#define	DEF_ADD(name, tret, targ, tdelta)				\
void									\
atomic_add_##name(volatile targ *p, tdelta d)				\
{									\
	(void)atomic_add_##name##_nv(p, d);				\
}									\
tret									\
atomic_add_##name##_nv(volatile targ *p, tdelta d)			\
{									\
	if (MISALIGNED(p, tret))					\
		SLOW_OP(tret, tret, p, (tret)((uintptr_t)o_ + (uintptr_t)d),	\
		    0);							\
	return __atomic_add_fetch((volatile tret *)p, d, ORDER);	\
}

#define	DEF_ADD_INT(name, tret, targ, tdelta)				\
void									\
atomic_add_##name(volatile targ *p, tdelta d)				\
{									\
	(void)atomic_add_##name##_nv(p, d);				\
}									\
tret									\
atomic_add_##name##_nv(volatile targ *p, tdelta d)			\
{									\
	if (MISALIGNED(p, tret))					\
		SLOW_OP(tret, tret, p, o_ + (tret)d, 0);			\
	return __atomic_add_fetch((volatile tret *)p, d, ORDER);	\
}

#define	DEF_BITOP(op, sym, name, tret, targ, tval)			\
void									\
atomic_##op##_##name(volatile targ *p, tval v)				\
{									\
	(void)atomic_##op##_##name##_nv(p, v);				\
}									\
tret									\
atomic_##op##_##name##_nv(volatile targ *p, tval v)			\
{									\
	if (MISALIGNED(p, tret))					\
		SLOW_OP(tret, tret, p, o_ sym v, 0);				\
	return __atomic_##op##_fetch((volatile tret *)p, v, ORDER);	\
}

#define	DEF_CAS(name, tret, targ, tval)					\
tret									\
atomic_cas_##name(volatile targ *p, tval expected, tval new)		\
{									\
	tret e = (tret)expected;					\
									\
	if (MISALIGNED(p, tret))					\
		SLOW_OP(tret, tret, p, o_ == e ? (tret)new : o_, 1);		\
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
	if (MISALIGNED(p, tret))					\
		SLOW_OP(tret, tret, p, (tret)v, 1);				\
	return __atomic_exchange_n((volatile tret *)p, (tret)v, ORDER);	\
}

#define	DEF_INCDEC(name, tret, targ)					\
void									\
atomic_inc_##name(volatile targ *p)					\
{									\
	(void)atomic_inc_##name##_nv(p);				\
}									\
tret									\
atomic_inc_##name##_nv(volatile targ *p)				\
{									\
	if (MISALIGNED(p, uintptr_t))					\
		SLOW_OP(uintptr_t, tret, p, o_ + 1, 0);			\
	return (tret)__atomic_add_fetch((volatile uintptr_t *)p, 1, ORDER); \
}									\
void									\
atomic_dec_##name(volatile targ *p)					\
{									\
	(void)atomic_dec_##name##_nv(p);				\
}									\
tret									\
atomic_dec_##name##_nv(volatile targ *p)				\
{									\
	if (MISALIGNED(p, uintptr_t))					\
		SLOW_OP(uintptr_t, tret, p, o_ - 1, 0);			\
	return (tret)__atomic_sub_fetch((volatile uintptr_t *)p, 1, ORDER); \
}

DEF_ADD_INT(32, uint32_t, uint32_t, int32_t)
DEF_ADD_INT(int, unsigned int, unsigned int, int)
DEF_ADD_INT(long, unsigned long, unsigned long, long)
DEF_ADD(ptr, void *, void, ssize_t)

DEF_BITOP(and, &, 32, uint32_t, uint32_t, uint32_t)
DEF_BITOP(and, &, uint, unsigned int, unsigned int, unsigned int)
DEF_BITOP(and, &, ulong, unsigned long, unsigned long, unsigned long)
DEF_BITOP(or, |, 32, uint32_t, uint32_t, uint32_t)
DEF_BITOP(or, |, uint, unsigned int, unsigned int, unsigned int)
DEF_BITOP(or, |, ulong, unsigned long, unsigned long, unsigned long)

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

	if (MISALIGNED(p, uint16_t))
		SLOW_OP(uint16_t, uint16_t, p, o_ == expected ? new : o_, 1);
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
