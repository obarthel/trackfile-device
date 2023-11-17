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

#ifndef _MACROS_H
#define _MACROS_H

/****************************************************************************/

#ifndef AbsExecBase
#define AbsExecBase (*(struct Library **)4)
#endif /* AbsExecBase */

/****************************************************************************/

#define CANNOT !
#define NOT !
#define NO !
#define OK (0)
#define SAME (0)
#define ZERO ((BPTR)NULL)

/****************************************************************************/

#define FLAG_IS_SET(v, f)	(((v) & (f)) == (f))
#define FLAG_IS_CLEAR(v, f)	(((v) & (f)) ==  0 )

/****************************************************************************/

#define SET_FLAG(v, f)		((void)(v |=  (f)))
#define CLEAR_FLAG(v, f)	((void)(v &= ~(f)))

/****************************************************************************/

/* This is a simple test to check if a BPTR may point
 * to a valid address. The test involves looking at the
 * two most significant bits of the BPTR which must be 0.
 */
#define IS_VALID_BPTR_ADDRESS(ptr) (((ptr) & 0xC0000000) == 0)

/****************************************************************************/

/* Check if the type of directory entry given by the FileInfoBlock
 * data structure refers to a file, or possibly a hard link to a file.
 */
#define FIB_IS_FILE(fib) ((fib)->fib_DirEntryType < 0)

/****************************************************************************/

/* Macro to get longword-aligned stack space for a structure.
 * Uses ANSI token concatenation to form a name for the char array
 * based on the variable name, then creates an appropriately
 * typed pointer to point to the first longword boundary in the
 * char array allocated.
 */
#define D_S(type, name) \
	char a_##name[sizeof(type)+3]; \
	type * name = (type *)((LONG)(a_##name+3) & ~3)

/****************************************************************************/

/* This probably should be in <dos/dosextens.h>, but isn't...
 * We need it while "formatting" a medium.
 */
#define ID_BAD_DISK (0x42414400L) /* 'BAD\0' */

/****************************************************************************/

/* Maximum length of the root directory name (the "volume name"). This does
 * not include the length field or NUL-termination.
 */
#define MAX_ROOT_DIRECTORY_NAME_LEN 30

/****************************************************************************/

/* These used to be in <devices/trackdisk.h> until 1986 (Kickstart 1.2) but
 * are still relevant for this command because they describe the properties
 * of the 3.5" disk drives which "trackfile.device" supports.
 */
#define NUMCYLS		80
#define NUMHEADS	2

/****************************************************************************/

#endif /* _MACROS_H */
