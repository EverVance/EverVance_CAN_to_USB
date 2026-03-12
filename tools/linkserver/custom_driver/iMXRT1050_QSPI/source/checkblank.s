//*****************************************************************************
// LPCXpresso Flash driver - checkblank.s
//
// Assembler routine to check flash is erased
//
//*****************************************************************************
//
// Copyright 2014, 2018, 2020, 2021 NXP
// All rights reserved.
//
// SPDX-License-Identifier: BSD-3-Clause
//*****************************************************************************

.syntax unified
 /* Put these codes into .text section */
 .text
 .align 4
 .section ".text.checkblank","ax",%progbits

/*uint32_t checkblank(uint32_t adr, uint32_t words, uint32_t blankval)
*/
  .global checkblank
   /* Declare  as global..Otherwise the linker won't find it */
  .func
  .thumb_func
checkblank:
    PUSH {r4-r11,lr}
loop:
    LDMIA r0!, {r4-r11}
    CMP r4, R2
	BNE fail
    CMP r5, R2
	BNE fail
	CMP r6, R2
	BNE fail
	CMP r7, R2
	BNE fail
	CMP r8, R2
	BNE fail
    CMP r9, R2
	BNE fail
	CMP r10, R2
	BNE fail
	CMP r11, R2
	BNE fail
	SUBS r1,#8
	BNE loop
blank:
    MOVS r0,#0
    POP {r4-r11,pc}

fail:
    MOVS r0,#1
    POP {r4-r11,pc}


