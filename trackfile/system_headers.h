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

#ifndef _SYSTEM_HEADERS_H
#define _SYSTEM_HEADERS_H

/****************************************************************************/

#include <exec/interrupts.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/devices.h>
#include <exec/errors.h>
#include <exec/memory.h>

#include <devices/trackdisk.h>
#include <devices/bootblock.h>
#include <devices/scsidisk.h>
#include <devices/timer.h>

#include <workbench/startup.h>
#include <workbench/icon.h>

/****************************************************************************/

#include <dos/filehandler.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/dosasl.h>
#include <dos/rdargs.h>
#include <dos/stdio.h>
#include <dos/dos.h>

#if defined(__amigaos4__)

#include <dos/obsolete.h>

#define __NOLIBBASE__
#define __USE_SYSBASE

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/timer.h>
#include <proto/icon.h>

#else

/* For building this with clang, for example. */
#if defined(__GNUC__) && !defined(AMIGA)
#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/utility_protos.h>
#include <clib/timer_protos.h>
#include <clib/icon_protos.h>

/* Normal build for SAS/C and GCC 68k. */
#else

#define __NOLIBBASE__

#if defined(__GNUC__)

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/icon.h>
#include <proto/timer.h>

#else

#include <clib/exec_protos.h>
#include <clib/dos_protos.h>
#include <clib/utility_protos.h>
#include <clib/icon_protos.h>
#include <clib/timer_protos.h>

#include <pragmas/exec_sysbase_pragmas.h>
#include <pragmas/dos_pragmas.h>
#include <pragmas/utility_pragmas.h>
#include <pragmas/icon_pragmas.h>
#include <pragmas/timer_pragmas.h>

#endif /* __GNUC__ */

/* Ugly workaround for trouble with ancient inline
 * header files for GCC 68k which cause problems
 * if Printf() is called with only the format
 * string and no parameters.
 */
#if defined(__GNUC__)

/* Substitute the Printf() and FPrintf() macros with a call to
 * local inline functions.
 */
#if defined Printf
#undef Printf
#define Printf LocalPrintf
#endif /* Printf */

#if defined FPrintf
#undef FPrintf
#define FPrintf LocalFPrintf
#endif /* FPrintf */

#include <stdarg.h>

extern struct Library * DOSBase;

__inline__ static int __attribute__ ((format (printf, 1, 2))) LocalPrintf(const char * fmt, ...)
{
	va_list args;
	int result;

	va_start(args,fmt);
	result = VPrintf(fmt, (LONG *)args);
	va_end(args);

	return(result);
}

__inline__ static int __attribute__ ((format (printf, 2, 3))) LocalFPrintf(BPTR file, const char * fmt, ...)
{
	va_list args;
	int result;

	va_start(args,fmt);
	result = VFPrintf(file, fmt, (LONG *)args);
	va_end(args);

	return(result);
}

/* This can cause trouble with "amiga_compiler.h", which
 * does not define SAVE_DS.
 */
#define SAVE_DS __attribute__((__saveds__))

#endif /* __GNUC__ */

#endif /* __GNUC__ && !AMIGA */

#endif /* __amigaos4__ */

/****************************************************************************/

#include <clib/alib_protos.h>

/****************************************************************************/

/* From the NewStyleDevices specification. */
#ifndef NSCMD_DEVICEQUERY

#define NSCMD_DEVICEQUERY 0x4000

struct NSDeviceQueryResult
{
	/* Standard information */
	ULONG	nsdqr_DevQueryFormat;		/* this is type 0 */
	ULONG	nsdqr_SizeAvailable;		/* bytes available */

	/* Common information (READ ONLY!) */
	UWORD	nsdqr_DeviceType;			/* what the device does */
	UWORD	nsdqr_DeviceSubType;		/* depends on the main type */
	UWORD *	nsdqr_SupportedCommands;	/* 0 terminated list of cmd's */
};

#define NSDEVTYPE_TRACKDISK 5 /* like trackdisk.device */

#endif /* NSCMD_DEVICEQUERY */

/****************************************************************************/

#include <strings.h>
#include <stdarg.h>

/****************************************************************************/

/* Platform- and compiler-specific macros. */
#include "compiler.h"

/****************************************************************************/

#endif /* _SYSTEM_HEADERS_H */
