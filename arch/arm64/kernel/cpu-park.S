/*
 * cpu park routines
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/linkage.h>
#include <asm/assembler.h>
#include <asm/sysreg.h>
#include <asm/virt.h>

.text
.pushsection 	.idmap.text, "ax"

/*
 * __cpu_park(el2_switch, park_address) - Helper for cpu_park
 *
 * @el2_switch: Flag to indicate a swich to EL2 is needed, passed to cpu_park.
 * @park_address - where cpu will keep on looking for address to jump
 *
 * Put the CPU into the wfe and check for valid none zero secondary address
 * at parked address when a event is received. If secondary address is
 * valid then jump to it.
 */

SYM_CODE_START(__cpu_park)
	/* Clear sctlr_el1 flags. */
	mrs	x12, sctlr_el1
	ldr	x13, =SCTLR_ELx_FLAGS
	bic	x12, x12, x13
	msr	sctlr_el1, x12
	isb
	mov	x3, 0
	str	x3, [x1]
1:
	wfe
	ldr	x2, [x1]
	cmp	x2, #0
	b.eq	1b

	mov	x3, 0
	str	x3, [x1]

	cbz	x0, 2f				// el2_switch?

	mov	x0, x2				// entry
	hvc	#0			// no return

2:
	ret	x2

SYM_CODE_END(__cpu_park)

.popsection