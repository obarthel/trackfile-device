/*
 * :ts=4
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
 */

#ifndef _SWAP_STACK_H
#define _SWAP_STACK_H

/****************************************************************************/

#ifndef _COMPILER_H
#include "compiler.h"
#endif /* _COMPILER_H */

/****************************************************************************/

/* The function being called with the stack swapped will
 * receive a single parameter in register A0 and a pointer
 * to the exec.library base in register A6. The result
 * returned, if any, will be passed back through the
 * swap_stack_and_call() function.
 */
typedef LONG (* ASM stack_swapped_func_t)(
	REG(a0, APTR parameter),
	REG(a6, struct Library * sysbase)
);

/****************************************************************************/

/* Swaps the current Task's stack with the provided one, calls a
 * function with well-defined parameters, restores the original
 * stack and returns the function's result.
 */
LONG ASM swap_stack_and_call(
	REG(a0, APTR parameter),
	REG(a1, stack_swapped_func_t function),
	REG(a2, struct StackSwapStruct * stk),
	REG(a6, struct Library * sysbase)
);

/****************************************************************************/

#endif /* _SWAP_STACK_H */
