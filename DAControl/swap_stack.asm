*
* :ts=8
*
* A trackdisk.device which uses ADF disk image files and its
* sidekick, the trusty DAControl shell command.
*
* Copyright (C) 2020 by Olaf Barthel <obarthel at gmx dot net>
*
* Permission is hereby granted, free of charge, to any person obtaining a
* copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation
* the rights to use, copy, modify, merge, publish, distribute, sublicense,
* and/or sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
* THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
* DEALINGS IN THE SOFTWARE.
*
*****************************************************************************
*
* The secret of life is to enjoy the passage of time.
*

	include "exec/macros.i"

*----------------------------------------------------------------------

	section text,code

*----------------------------------------------------------------------

	xdef    _swap_stack_and_call

	;	long __asm swap_stack_and_call(register __a0 APTR parameter,
	;	                               register __a1 APTR function,
	;	                               register __a2 struct StackSwapStruct * stk,
	;	                               register __a6 struct Library * SysBase);

_swap_stack_and_call:

	movem.l	d2/d3/d4,-(sp)

	move.l	a0,d2		; Save these two as StackSwap() will end up
	move.l	a1,d3		; clobbering their contents.

	move.l	a2,a0
	JSRLIB	StackSwap

	move.l	d2,a0		; Restore the parameter
	move.l	d3,a1
	jsr	(a1)		; Invoke the routine to be called with A6=SysBase.
	move.l	d0,d4		; Save the return value

	move.l	a2,a0		; Restore the original stack.
	JSRLIB	StackSwap

	move.l	d4,d0		; Restore the return value

	movem.l	(sp)+,d2/d3/d4
	rts

*----------------------------------------------------------------------

	end
