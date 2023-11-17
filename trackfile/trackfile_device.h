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

#ifndef _TRACKFILE_DEVICE_H
#define _TRACKFILE_DEVICE_H

/****************************************************************************/

/* This allows for an optional track cache to be enabled. */
#ifndef _CACHE_H
#include "cache.h"
#endif /* _CACHE_H */

/****************************************************************************/

/* A small collection of handy macros starts here */

/****************************************************************************/

/* This is also defined in amiga.lib, but something simpler is good, too. */
#define AbsExecBase (*(struct Library **)4)

/****************************************************************************/

#define CANNOT !
#define NOT !
#define OK (0)
#define SAME (0)
#define ZERO ((BPTR)NULL)
#define BUSY ((struct IORequest *)NULL)
#define MILLION 1000000

/****************************************************************************/

#define FLAG_IS_SET(v, f)	(((v) & (f)) == (f))
#define FLAG_IS_CLEAR(v, f)	(((v) & (f)) ==  0 )

/****************************************************************************/

#define SET_FLAG(v, f)		((void)(v |=  (f)))
#define CLEAR_FLAG(v, f)	((void)(v &= ~(f)))

/****************************************************************************/

/* Macro to get longword-aligned stack space for a structure Uses ANSI token
 * catenation to form a name for the char array based on the variable name,
 * then creates an appropriately typed pointer to point to the first longword
 * boundary in the char type array allocated.
 */
#define D_S(type, name) \
	char a_##name[sizeof(type)+3]; \
	type *name = (type *)((LONG)(a_##name+3) & ~3)

/****************************************************************************/

/* This is a simple test to check if a BPTR may point to a valid address.
 * The test involves looking at the two most significant bits of the BPTR
 * which must be 0.
 */
#define IS_VALID_BPTR_ADDRESS(ptr) (((ptr) & 0xC0000000) == 0)

/****************************************************************************/

/* Maximum path name size, e.g. when looking up the name of of a directory or
 * file associated with a Lock or FileHandle.
 */
#define MAX_PATH_SIZE 1024

/****************************************************************************/

/* For the moment we're only pretending to support
 * 80 track 3.5" disk drives.
 */
#define NUMCYLS		80
#define NUMHEADS	2

/****************************************************************************/

/* This is used to initialize an RTF_AUTOINIT type device/library. */
struct InitTable
{
	ULONG	it_Size;			/* Data space size */
	APTR	it_FunctionTable;	/* Pointer to function initializers */
	APTR	it_DataTable;		/* Pointer to data initializers */
	APTR	it_InitRoutine;		/* Routine to run */
};

/****************************************************************************/

/* This is what the device itself uses for its units and other global data. */
struct TrackFileDevice
{
	struct Device			tfd_Device;			/* Standard device header */

	/* The following data mimics the layout of the V33 trackdisk.device
	 * which, oddly enough, some software is still expected to peek into.
	 * Not that it helps, but we do our best to present a bit of
	 * semi-useless information here. Note that none of these files are
	 * used by trackfile.device!
	 */
	UBYTE					tfd_TD_Flags;
	UBYTE					tfd_TD_Pad;

	struct Unit *			tfd_TD_Units[4];

	struct Library *		tfd_TD_Sysbase;
	struct Library *		tfd_TD_GfxBase;
	struct Library *		tfd_TD_DiskBase;

	struct List				tfd_TD_MemList;

	struct Device *			tfd_TD_TimerBase0;
	struct Unit *			tfd_TD_TimerUnit0;
	struct Device *			tfd_TD_TimerBase1;
	struct Unit *			tfd_TD_TimerUnit1;

	struct Library *		tfd_TD_CIABase;

	struct Interrupt		tfd_TD_ResetInterrupt;
	struct IOStdReq			tfd_TD_ResetRequest;

	/* And that's all for the trackdisk.device mimicry above. */

	struct Library *		tfd_SysBase;		/* Global library bases */
	struct Library *		tfd_DOSBase;
	struct Library *		tfd_UtilityBase;

	BPTR					tfd_SegList;		/* Returned when unloaded */

	struct MinList			tfd_UnitList;		/* All the device units */

	struct SignalSemaphore	tfd_Lock;			/* Protects access to global data */
	UWORD					tfd_Pad1;

	/************************************************************************/

	#if defined(ENABLE_CACHE)

		struct CacheContext *	tfd_CacheContext;	/* The global cache, if enabled. */

	#endif /* ENABLE_CACHE */
};

/****************************************************************************/

/* The following are block type and data structure definitions used by the
 * Amiga default file system, and specifically the file system root directory
 * block.
 */

#define T_SHORT 2	/* Primary type for root directory block */
#define ST_ROOT 1	/* Secondary type for root directory block */

/* This is the 512 byte version you would find on a 3.5" or 5.25" floppy disk.
 * Note that the size of the hash table grows with the block size and the
 * rdb_BitMapFlag field will follow it. For a 512 block the hash table can
 * contain only exactly 72 entries.
 */
struct RootDirBlock
{
	ULONG				rdb_PrimaryType;		/* Must be T_SHORT */
	ULONG				rdb_OwnKey;				/* Must be zero for root directory */
	ULONG				rdb_BlockCount;			/* Must be zero for root directory */
	LONG				rdb_HashTableSize;		/* Must be >= 72 */
	ULONG				rdb_Reserved[1];
	LONG				rdb_Checksum;
	ULONG				rdb_HashTable[72];		/* size = (block size in bytes / sizeof(LONG)) - 56 */

	/************************************************************************/

	LONG				rdb_BitMapFlag;			/* position = block size in bytes - sizeof(LONG) * 50 */
	ULONG				rdb_BitMapBlocks[25];
	ULONG				rdb_BitMapExtension;	/* NOTE: does not exist in OFS; is 26th BitMap block entry */
	struct DateStamp	rdb_LastChange;			/* NOTE: is volume change date for OFS */
	TEXT				rdb_Name[32];
	ULONG				rdb_LinkChain;			/* Not used by root directory */
	LONG				rdb_NumBlocksUsed;		/* NOTE: valid only for LNFS */
	struct DateStamp	rdb_LastBitmapChange;	/* NOTE: does not exist in OFS */
	struct DateStamp	rdb_DiskInitialization;
	ULONG				rdb_FileSystemType;		/* NOTE: valid only for LNFS */
	ULONG				rdb_Parent;				/* Must be zero for root directory */
	ULONG				rdb_FirstDirList;		/* NOTE: valid only for DCFS */
	LONG				rdb_SecondaryType;		/* Must be ST_ROOT */
};

/****************************************************************************/

/* These are used for referencing library base addresses off the
 * trackfile.device device base itself.
 */

#define USE_EXEC(tfd) \
	struct Library * SysBase = tfd->tfd_SysBase

#define USE_DOS(tfd) \
	struct Library * DOSBase = tfd->tfd_DOSBase

#define USE_UTILITY(tfd) \
	struct Library * UtilityBase = tfd->tfd_UtilityBase

/****************************************************************************/

#endif /* _TRACKFILE_DEVICE_H */
