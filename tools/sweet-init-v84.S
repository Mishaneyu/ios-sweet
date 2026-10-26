/*
 * EL0 arm64e (Apple A13 / ARMv8.4-A) bring-up & instruction-emulator self-test
 * for iOS 26 (xnu-12377) on ARMv8.2-A hardware (SM8150 / SM8150-AC) and QEMU
 * virt (-cpu cortex-a76).
 *
 * Assemble & wrap:
 *   clang --target=arm64e-apple-ios26.0 -mcpu=apple-a13 -c tools/sweet-init-v84.S -o sweet-init-v84.o
 *   python3 tools/mkinit.py sweet-init-v84.o sweet-init-v84.macho
 *
 * Exercises:
 *   1. CPU_SUBTYPE_ARM64E (0x80000002) exec grading in XNU
 *   2. Hint-space PAC (paciasp / autiasp)
 *   3. Register PAC/AUT/XPAC (pacia, autia, pacda, autda, xpaci, xpacd)
 *   4. Generic PAC MAC (pacga determinism & low-32 zero invariant)
 *   5. Authenticated branches/returns (blraa, braaz, retaa)
 *   6. Authenticated loads (ldraa pos offset, neg offset, pre-index writeback)
 *   7. ARMv8.3 FEAT_JSCVT (fjcvtzs exact Z=1, modulo-2^32 wrap Z=0, NaN -> 0)
 *   8. ARMv8.4 FEAT_LRCPC2 (stlurb/ldapurb/ldapursb/stlur/ldapur neg unscaled)
 *   9. ARMv8.4 FEAT_FlagM / FEAT_DIT / FEAT_SHA3 (cfinv, setf8, msr dit, eor3, bcax, rax1, xar)
 */
#define SYS_write	4
#define SYS_open	5
#define SYS_getpid	20
#define SYS_select	93
#define O_RDWR		2

	.text
	.globl	_start
	.p2align 2
_start:
	mov	x9, sp
	and	x9, x9, #~15
	sub	sp, sp, #64		/* 64-byte aligned scratch frame */

	adr	x0, path
	mov	x1, #O_RDWR
	mov	x2, #0
	mov	x16, #SYS_open
	svc	#0x80
	b.cs	hang
	mov	x19, x0

	adr	x1, banner
	adr	x2, banner_end
	sub	x2, x2, x1
	bl	write_console

	/* Suite 1 & 2: Hint PAC + Register PAC/AUT/XPAC */
	mov	w20, #'1'
	paciasp
	autiasp

	mov	w20, #'2'
	adr	x0, pac_callee
	mov	x8, x0
	mov	x1, #0x1234
	pacia	x0, x1
	autia	x0, x1
	cmp	x0, x8
	b.ne	fail
	pacda	x0, x1
	autda	x0, x1
	xpaci	x0
	xpacd	x0
	cmp	x0, x8
	b.ne	fail

	/* Suite 3: pacga determinism & low-32 zero */
	mov	w20, #'3'
	pacga	x2, x0, x1
	pacga	x3, x0, x1
	cmp	x2, x3
	b.ne	fail
	tst	x2, #0xffffffff
	b.ne	fail

	/* Suite 4: blraa, braaz, retaa */
	mov	w20, #'4'
	adr	x8, pac_callee
	mov	x9, #0x55aa
	mov	x0, #0
	blraa	x8, x9
	cmp	x0, #0x777
	b.ne	fail

	/* Suite 5: ldraa (pos, neg, pre-index writeback) */
	mov	w20, #'5'
	ldr	x0, lit_a
	ldr	x1, lit_b
	ldr	x2, lit_c
	str	x0, [sp, #0]
	str	x1, [sp, #16]
	str	x2, [sp, #32]
	mov	x8, sp
	ldraa	x3, [x8, #16]
	cmp	x3, x1
	b.ne	fail
	add	x8, sp, #32
	ldraa	x4, [x8, #-16]
	cmp	x4, x1
	b.ne	fail
	add	x8, sp, #32
	ldraa	x5, [x8, #-32]!
	cmp	x5, x0
	b.ne	fail
	mov	x6, sp
	cmp	x8, x6
	b.ne	fail

	/* Suite 6: ARMv8.3 FEAT_JSCVT (fjcvtzs) */
	mov	w20, #'6'
	/* -42.0 exact -> w0 = -42, Z = 1 */
	ldr	d0, dbl_neg42
	fjcvtzs	w0, d0
	b.ne	fail			/* Z must be 1 for exact int32 */
	cmn	w0, #42
	b.ne	fail
	/* 4294967301.75 (2^32 + 5.75) -> w0 = 5, Z = 0 */
	ldr	d0, dbl_wrap5
	fjcvtzs	w0, d0
	b.eq	fail			/* Z must be 0 when truncated/wrapped */
	cmp	w0, #5
	b.ne	fail
	/* quiet NaN -> w0 = 0, Z = 0 */
	ldr	d0, dbl_nan
	fjcvtzs	w0, d0
	b.eq	fail
	cmp	w0, #0
	b.ne	fail

	/* Suite 7: ARMv8.4 FEAT_LRCPC2 (stlur* / ldapur* negative unscaled) */
	mov	w20, #'7'
	add	x8, sp, #32
	mov	w0, #0x85
	stlurb	w0, [x8, #-5]
	ldapurb	w1, [x8, #-5]
	cmp	w1, #0x85
	b.ne	fail
	ldapursb x2, [x8, #-5]
	mov	x3, #-123		/* 0x85 as signed int8 = -123 */
	cmp	x2, x3
	b.ne	fail
	ldr	x0, lit_c
	stlur	x0, [x8, #-16]
	ldapur	x1, [x8, #-16]
	cmp	x1, x0
	b.ne	fail

	/* Suite 8: ARMv8.4 FEAT_FlagM + FEAT_DIT */
	mov	w20, #'8'
	mov	x0, #0
	adds	xzr, x0, #0		/* C = 0 */
	cfinv				/* C = 1 */
	b.cc	fail
	cfinv				/* C = 0 */
	b.cs	fail
	mov	w0, #0x80
	setf8	w0			/* N = 1, Z = 0 */
	b.pl	fail
	b.eq	fail
	msr	dit, #1
	msr	dit, #0

	/* Suite 9: ARMv8.4 FEAT_SHA3 (eor3, bcax, rax1, xar) */
	mov	w20, #'9'
	mov	x0, #0x0f0f
	mov	x1, #0x00ff
	mov	x2, #0x000f
	dup.2d	v0, x0
	dup.2d	v1, x1
	dup.2d	v2, x2
	eor3.16b v3, v0, v1, v2		/* 0x0f0f ^ 0x00ff ^ 0x000f = 0x0fffb? 0x0f0f^0x00ff=0x0ff0 ^ 0x000f = 0x0fff */
	fmov	x3, d3
	cmp	x3, #0x0fff
	b.ne	fail
	bcax.16b v4, v0, v1, v2		/* 0x0f0f ^ (0x00ff & ~0x000f) = 0x0f0f ^ 0x00f0 = 0x0fff */
	fmov	x4, d4
	cmp	x4, #0x0fff
	b.ne	fail
	rax1.2d	v5, v0, v1		/* v0 ^ rol64(v1, 1) = 0x0f0f ^ 0x01fe = 0x0ef1 */
	fmov	x5, d5
	mov	x6, #0x0ef1
	cmp	x5, x6
	b.ne	fail
	xar.2d	v6, v0, v1, #60		/* ror64(0x0f0f ^ 0x00ff, 60) = rol64(0x0ff0, 4) = 0xff00 -> wait: mov immediate */
	fmov	x5, d6
	mov	x6, #0xff00
	cmp	x5, x6
	b.ne	fail

	/* All 9 suites passed! Print success with PID. */
	adr	x1, pass_msg
	adr	x2, pass_pid
	sub	x2, x2, x1
	bl	write_console

	mov	x16, #SYS_getpid
	svc	#0x80
	add	w0, w0, #'0'
	strb	w0, [sp]
	mov	x1, sp
	mov	x2, #1
	bl	write_console

	adr	x1, pass_pid
	adr	x2, pass_end
	sub	x2, x2, x1
	bl	write_console

tick:
	mov	x0, #0
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	adr	x4, five_sec
	mov	x16, #SYS_select
	svc	#0x80

	adr	x1, alive
	adr	x2, alive_end
	sub	x2, x2, x1
	bl	write_console
	b	tick

pac_callee:
	paciasp
	adr	x10, pac_callee_tail
	braaz	x10
pac_callee_tail:
	mov	x0, #0x777
	retaa

fail:
	adr	x1, fail_msg
	adr	x2, fail_end
	sub	x2, x2, x1
	bl	write_console
	strb	w20, [sp]
	mov	w0, #'\n'
	strb	w0, [sp, #1]
	mov	x1, sp
	mov	x2, #2
	bl	write_console
hang:
	b	hang

write_console:
	mov	x0, x19
	mov	x16, #SYS_write
	svc	#0x80
	ret

	.p2align 3
lit_a:		.quad	0x1122334455667788
lit_b:		.quad	0xaabbccddeeff0011
lit_c:		.quad	0x0123456789abcdef
dbl_neg42:	.quad	0xc045000000000000	/* -42.0 */
dbl_wrap5:	.quad	0x41f00000005c0000	/* 4294967301.75 = 2^32 + 5.75 */
dbl_nan:	.quad	0x7ff8000000000000	/* quiet NaN */
five_sec:
	.quad	5
	.quad	0
path:
	.asciz	"/dev/console"
banner:
	.ascii	"\n*** SWEET iOS 26 arm64e / ARMv8.4 self-test starting ***\n"
banner_end:
pass_msg:
	.ascii	"*** SWEET v8.4 + arm64e SELF-TEST: ALL 9 SUITES PASSED (PID "
pass_pid:
	.ascii	") ***\n"
pass_end:
fail_msg:
	.ascii	"*** SWEET v8.4 SELF-TEST FAILED AT SUITE "
fail_end:
alive:
	.ascii	"SWEET init-v84: still alive\n"
alive_end:
