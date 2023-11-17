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

#include "macros.h"
#include "global_data.h"

/****************************************************************************/

#define __USE_SYSBASE
#include <proto/exec.h>

/****************************************************************************/

VOID
FreeGlobalData(struct GlobalData * gd)
{
	if(gd != NULL)
	{
		struct WBStartup * startup_message = gd->gd_WBenchMsg;
		struct Library * SysBase = gd->gd_SysBase;

		if(gd->gd_UtilityBase != NULL)
			CloseLibrary(gd->gd_UtilityBase);

		if(gd->gd_DOSBase != NULL)
			CloseLibrary(gd->gd_DOSBase);

		FreeVec(gd);

		if(startup_message != NULL)
		{
			Forbid();

			ReplyMsg((struct Message *)startup_message);
		}
	}
}

/****************************************************************************/

struct GlobalData *
AllocateGlobalData(ULONG which_libraries)
{
	struct Library * SysBase = (struct Library *)AbsExecBase;

	LONG error = ERROR_INVALID_RESIDENT_LIBRARY;
	struct GlobalData * result = NULL;
	struct GlobalData * gd = NULL;
	struct Process * this_process;
	struct WBStartup * startup_message;

	this_process = (struct Process *)FindTask(NULL);

	/* Pick up the Workbench startup message. This is
	 * can only work if the program is not running
	 * in the CLI. We can't call dos.library/Cli()
	 * here because that would require Kickstart 2.0
	 * or higher.
	 */
	if(this_process->pr_CLI == ZERO)
	{
		WaitPort(&this_process->pr_MsgPort);
		startup_message = (struct WBStartup *)GetMsg(&this_process->pr_MsgPort);
	}
	else
	{
		startup_message = NULL;
	}

	/* Kickstart 2.04 or higher required. */
	if(SysBase->lib_Version < 37)
		goto out;

	gd = AllocVec(sizeof(*gd), MEMF_PUBLIC|MEMF_CLEAR);
	if(gd == NULL)
	{
		error = ERROR_NO_FREE_STORE;
		goto out;
	}

	gd->gd_SysBase = SysBase;

	gd->gd_DOSBase = OpenLibrary("dos.library", 37);
	if(gd->gd_DOSBase == NULL)
		goto out;

	gd->gd_UtilityBase = OpenLibrary("utility.library", 37);
	if(gd->gd_UtilityBase == NULL)
		goto out;

	error = 0;

	/* Let the command deal with the Workbench
	 * startup message.
	 */
	gd->gd_WBenchMsg = startup_message;
	startup_message = NULL;

	result = gd;
	gd = NULL;

 out:

	FreeGlobalData(gd);

	if(error != 0)
		this_process->pr_Result2 = error;

	/* Reply the Workbench startup message, effectively
	 * quitting the program because it cannot work under
	 * the current conditions.
	 */
	if(startup_message != NULL)
	{
		Forbid();

		ReplyMsg((struct Message *)startup_message);
	}

	return(result);
}
