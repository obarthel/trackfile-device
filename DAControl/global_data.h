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

#ifndef _GLOBAL_DATA_H
#define _GLOBAL_DATA_H

/****************************************************************************/

#ifndef EXEC_LIBRARIES_H
#include <exec/libraries.h>
#endif /* EXEC_LIBRARIES_H */

#ifndef EXEC_IO_H
#include <exec/io.h>
#endif /* EXEC_IO_H */

#ifndef DOS_DOSEXTENS_H
#include <dos/dosextens.h>
#endif /* DOS_DOSEXTENS_H */

#ifndef WORKBENCH_STARTUP_H
#include <workbench/startup.h>
#endif /* WORKBENCH_STARTUP_H */

/****************************************************************************/

/* This is the local data which all the functions in this
 * command are using.
 */
struct GlobalData
{
	struct Library *	gd_SysBase;
	struct Library *	gd_DOSBase;
	struct Library *	gd_UtilityBase;

	struct Library *	gd_TrackFileBase;
	struct IOStdReq		gd_TrackFileDevice;

	struct WBStartup *	gd_WBenchMsg;

	BPTR				gd_LoadedFileSystem;
	STRPTR				gd_LoadedFileSystemName;
	BOOL				gd_LoadedFileSystemUsed;

	BOOL				gd_UseChecksums;

	STRPTR				gd_DiskImageFileName;

	struct DevProc *	gd_DevProc;
};

/****************************************************************************/

/* These are for referencing library bases off the global data. */
#define USE_EXEC(gd) \
	struct Library * SysBase = gd->gd_SysBase

#define USE_DOS(gd) \
	struct Library * DOSBase = gd->gd_DOSBase

#define USE_UTILITY(gd) \
	struct Library * UtilityBase = gd->gd_UtilityBase

#define USE_TRACKFILE(gd) \
	struct Library * TrackFileBase = gd->gd_TrackFileBase

/****************************************************************************/

/* Which libraries should be opened by AllocateGlobalData(). */
#define AGDL_ExecDos	0			/* exec.library and dos.library (default) */
#define AGDL_Utility	(1UL<<0)	/* utility.library is required */
#define AGDL_Locale		(1UL<<1)	/* locale.library would be nice */

/****************************************************************************/

extern VOID FreeGlobalData(struct GlobalData * gd);
extern struct GlobalData * AllocateGlobalData(ULONG which_libraries);

/****************************************************************************/

#endif /* _GLOBAL_DATA_H */
