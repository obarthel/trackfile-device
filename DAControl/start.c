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

#define __USE_SYSBASE
#include <proto/exec.h>

#include <proto/dos.h>

/****************************************************************************/

#include <string.h>

/****************************************************************************/

#include "compiler.h"
#include "macros.h"
#include "global_data.h"
#include "process_icons.h"
#include "cmd_main.h"
#include "swap_stack.h"

/****************************************************************************/

#include "assert.h"

/* This is a workaround for SAS/C which comes into play if the debug
 * build is enabled. Because assert.lib contains optional output to
 * a file dos.library functions will be compiled in, and these need
 * to reference DOSBase. We won't use them here, but unless we
 * provide a DOSBase the SAS/C runtime library will pull in a lot
 * of code that shouldn't be part of this program.
 */
#ifdef DEBUG
struct DosLibrary * DOSBase;
#endif /* DEBUG */

/****************************************************************************/

/* How much stack space does this program need to run
 * without getting itself into trouble?
 */
#define MINIMUM_STACK_SIZE 6000

/****************************************************************************/

/* This is used for stack size usage detection. */
#define STACK_FILL_COOKIE 0xA3

/****************************************************************************/

/* This is used by the _start() function below. */
static LONG stack_size_available(struct GlobalData * gd);

/****************************************************************************/

long
_start(void)
{
	struct GlobalData * gd;
	int rc = RETURN_FAIL;

	/* This enables full debug output. */
	SETDEBUGLEVEL(2);

	SETPROGRAMNAME("DAControl");

	ENTER();

	gd = AllocateGlobalData(AGDL_ExecDos|AGDL_Utility);
	if(gd != NULL)
	{
		USE_EXEC(gd);
		USE_DOS(gd);

		struct WBStartup * startup_message = gd->gd_WBenchMsg;
		const size_t new_stack_size = 32 + ((MINIMUM_STACK_SIZE + 31UL) & ~31UL);
		struct StackSwapStruct * stk = NULL;
		UBYTE * new_stack = NULL;

		/* Try to make sure that enough stack space is
		 * available for DAControl to run.
		 */
		D(("stack size available = %lu", stack_size_available(gd)));

		if(stack_size_available(gd) < MINIMUM_STACK_SIZE)
		{
			/* Allocate our own stack, of the right size. */
			stk = AllocMem(sizeof(*stk), MEMF_PUBLIC|MEMF_ANY);
			new_stack = AllocMem(new_stack_size, MEMF_ANY);

			if(stk != NULL && new_stack != NULL)
			{
				/* Fill the stack with a specific value so that we
				 * may later get an estimate of how much stack space
				 * was required to perform the program's functions.
				 */
				#if DEBUG
				{
					memset(new_stack, STACK_FILL_COOKIE, new_stack_size);
				}
				#endif /* DEBUG */

				/* Fill in the lower and upper bounds, then take care of
				 * the stack pointer itself.
				 */
				stk->stk_Lower		= new_stack;
				stk->stk_Upper		= (ULONG)new_stack + new_stack_size;
				stk->stk_Pointer	= (APTR)(stk->stk_Upper - 32);

				rc = OK;
			}
			else
			{
				SHOWMSG("could not allocate stack swapping data structures");

				rc = RETURN_FAIL;

				if(stk != NULL)
				{
					FreeMem(stk, sizeof(*stk));
					stk = NULL;
				}

				if(new_stack != NULL)
				{
					FreeMem(new_stack, new_stack_size);
					new_stack = NULL;
				}
			}
		}
		/* No need to allocate a new stack. */
		else
		{
			rc = RETURN_OK;
		}

		/* Can we proceed safely with either a new stack to be used,
		 * or no need to have allocated one?
		 */
		if(rc == RETURN_OK)
		{
			/* Started from shell? */
			if(startup_message == NULL)
			{
				SHOWMSG("started from shell");

				if(stk == NULL)
					rc = cmd_main(gd);
				else
					rc = swap_stack_and_call(gd, (stack_swapped_func_t)cmd_main, stk, SysBase);
			}
			/* Handle Workbench startup separately. */
			else
			{
				BPTR old_cd;

				SHOWMSG("started from Workbench");

				old_cd = CurrentDir(startup_message->sm_ArgList[0].wa_Lock);

				if(stk == NULL)
					process_icons(gd);
				else
					swap_stack_and_call(gd, (stack_swapped_func_t)process_icons, stk, SysBase);

				CurrentDir(old_cd);

				rc = OK;
			}
		}

		/* Report how much stack space was actually used? */
		#if 0
		{
			if(stk != NULL)
			{
				extern void kprintf(const char * fmt, ...);
				size_t stack_size_used = 0;
				size_t i;

				/* Note that we start 4 bytes above the stack base
				 * address because exec.library/StackSwap() will
				 * overwrite the first 32 bit word of the new
				 * stack with a cookie value.
				 */
				for(i = sizeof(ULONG) ; i < new_stack_size ; i++)
				{
					if(new_stack[i] != STACK_FILL_COOKIE)
					{
						stack_size_used = new_stack_size - i;
						break;
					}
				}

				/* D(("stack size used = %lu bytes", stack_size_used)); */

				kprintf("DAControl stack size used = %lu bytes\n", stack_size_used);
			}
		}
		#endif

		/* Free stack swapping data structure and the stack
		 * memory which we might have allocated above.
		 */
		if(stk != NULL)
			FreeMem(stk, sizeof(*stk));

		if(new_stack != NULL)
			FreeMem(new_stack, new_stack_size);

		/* Note that FreeGlobalData() also takes care of
		 * replying the Workbench startup message if this
		 * program was started from Workbench, that is...
		 */
		FreeGlobalData(gd);
	}

	RETURN(rc);
	return(rc);
}

/****************************************************************************/

/* The stack_size_available() function needs to be able to access
 * the current stack pointer address in a sort of portable manner
 * through the get_sp() macro.
 */
#if defined(__SASC)

#include <dos.h>

#define get_sp() ((BYTE *)getreg(REG_A7))

#else

#define get_sp() ((BYTE *)0)

#endif /* __SASC */

/****************************************************************************/

/* How much stack space is currently available to this Process? */
static LONG
stack_size_available(struct GlobalData * gd)
{
	USE_EXEC(gd);

	const struct Process * pr = (struct Process *)FindTask(NULL);
	const BYTE * lower = (BYTE *)pr->pr_Task.tc_SPLower;
	const BYTE * current_sp = get_sp();
	LONG result;

	if(current_sp > lower)
		result = current_sp - lower;
	else
		result = 0;

	return(result);
}
