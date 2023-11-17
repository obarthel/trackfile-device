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

#ifndef _UNIT_H
#define _UNIT_H

/****************************************************************************/

#ifndef _TOOLS_H
#include "tools.h"
#endif /* _TOOLS_H */

#ifndef _MFM_ENCODING_H
#include "mfm_encoding.h"
#endif /* _MFM_ENCODING_H */

#ifndef _CACHE_H
#include "cache.h"
#endif /* _CACHE_H */

/****************************************************************************/

/* Each unit has its own state information and data to manage.
 * While you can access the unit data structures through the
 * device base, access to some fields of the unit data requires
 * that you hold the unit lock.
 */
struct TrackFileUnit
{
	struct TDU_PublicUnit			tfu_Unit;

	struct TrackFileDevice *		tfu_Device;					/* Convenient... */
	LONG							tfu_UnitNumber;				/* Bears the number of this unit */

	struct Process *				tfu_Process;				/* This is the process managing the unit; can be NULL */

	struct MsgPort					tfu_ControlPort;			/* Unit control messages go here */

	BOOL							tfu_Stopped;				/* FALSE if the unit still processes commands, TRUE otherwise. */

	struct SignalSemaphore			tfu_Lock;					/* Hold this to access certain fields of the unit data */

	BOOL							tfu_TurnMotorOff;			/* Eventually, turn off the motor */

	struct MinList					tfu_ChangeIntList;			/* Change notifications added by TD_ADDCHANGEINT */
	struct Interrupt *				tfu_RemoveInt;				/* Single change notification set by TD_REMOVE */

	struct timerequest				tfu_TimeRequest;			/* So the motor can be turned off automatically */
	struct MsgPort					tfu_TimePort;
	UWORD							tfu_Pad1;

	BPTR							tfu_File;					/* Will be ZERO if no medium is present */
	LONG							tfu_FilePosition;			/* Current file seek position, or -1 if not known */
	LONG							tfu_FileSize;				/* Needed for bounds checking in many commands */

	LONG							tfu_DriveType;				/* Either a DD or HD 3.5" disk drive (see <devices/trackdisk.h>) */
	LONG							tfu_NumCylinders;			/* 80 for a 3.5" disk drive for a 5.25" disk drive with 80 cylinders */
	LONG							tfu_NumHeads;				/* This should be 2 */
	LONG							tfu_NumTracks;				/* Total number of tracks, calculated from the above */

	struct AlignedMemoryAllocation	tfu_TrackMemory;			/* Memory allocated best suited for the file system */
	struct MsgPort *				tfu_TrackFileSystem;		/* File system process responsible for the disk image file */
	APTR							tfu_TrackData;				/* Read/write cache for this unit; holds exactly one track */
	LONG							tfu_TrackDataSize;			/* Size of the read/write cache in bytes */
	struct fletcher64_checksum		tfu_TrackDataChecksum;		/* Checksum for the track data */

	struct fletcher64_checksum *	tfu_DiskChecksumTable;		/* If not NULL, individual track checksums. */
	LONG							tfu_DiskChecksumTableLength;
	struct fletcher64_checksum		tfu_DiskChecksum;			/* Checksum covering all the tracks. */

	LONG							tfu_CurrentTrackNumber;		/* Which track is currently in the read/write cache; can be -1 */

	LONG							tfu_RootDirTrackNumber;
	ULONG							tfu_FileSystemSignature;
	ULONG							tfu_BootBlockChecksum;
	LONG							tfu_RootDirBlockOffset;
	TEXT							tfu_RootDirName[32];
	struct DateStamp				tfu_RootDirDate;
	BOOL							tfu_RootDirValid;

	BOOL							tfu_MotorEnabled;			/* True if a read, write or seek operation is in progress,
																 * or if the client just turned the drive motor on
																 */
	BOOL							tfu_TrackDataChanged;		/* True if the read/write cache contents have been modified */
	BOOL							tfu_ChangesMade;			/* True if track data was ever written back to the file */
	BOOL							tfu_WriteProtected;			/* True if the medium cannot be written to */
	BOOL							tfu_ChecksumUpdated;		/* True if the track checksums were updated, but not the disk checksum */
	BOOL							tfu_IgnoreTrackChecksum;	/* True if the current track checksum should be ignored when
																 * writing back the track.
																 */

	/************************************************************************/

	#if defined(ENABLE_MFM_ENCODING)

		struct mfm_code_context *	tfu_MFMCodeContext;
		ULONG						tfu_PRNGState;

	#endif /* ENABLE_MFM_ENCODING */

	/************************************************************************/

	#if defined(ENABLE_CACHE)

		struct MinList				tfu_CacheNodeList;			/* All the CacheNodes used by this unit */
		ULONG						tfu_CacheAccesses;			/* Total number cache accesses */
		ULONG						tfu_CacheMisses;			/* Number of cache misses */
		BOOL						tfu_CacheEnabled;			/* Is the cache currently active for this unit? */
		BOOL						tfu_PrefillCache;			/* When loading a medium, fill the entire cache? */

	#endif /* ENABLE_CACHE */
};

/****************************************************************************/

/* The unit process receives control messages which concern mainly whether
 * a medium should be ejected or inserted. But shutting down a unit process
 * so that it releases as much unit memory as possible is needed, too.
 */
enum trackfilecontrol_msg_t
{
	TFC_Stop,
	TFC_Insert,
	TFC_Eject,
	TFC_ChangeWriteProtection,
	TFC_ChangeEnableCache,
};

/****************************************************************************/

/* This is a control message such as is sent to a unit. It contains
 * all the fields which any of the control messages might need.
 */
struct TrackFileControlMsg
{
	struct Message				tfcm_Message;			/* Standard message header */

	enum trackfilecontrol_msg_t	tfcm_Type;				/* What kind of operation to perform */

	LONG						tfcm_Error;				/* Any error code to return to the client */

	BPTR						tfcm_File;				/* This is needed by TFC_Insert */
	LONG						tfcm_FileSize;			/* This is needed by TFC_Insert */

	BOOL						tfcm_WriteProtected;	/* This is needed by TFC_Insert and TFC_ChangeWriteProtection */

	BOOL						tfcm_Value;				/* This is needed by TFC_ChangeEnableCache */
};

/****************************************************************************/

VOID UnitProcessEntry(VOID);
LONG send_unit_control_command(struct TrackFileUnit *tfu, LONG type, BPTR file, LONG file_size, BOOL write_protected, LONG value);
struct TrackFileUnit * find_unit_by_number(struct TrackFileDevice * tfd, LONG unit_number);
LONG eject_image_file(struct TrackFileUnit * tfu);
VOID trigger_change(struct TrackFileUnit * tfu);
BOOL unit_is_active(struct TrackFileUnit *tfu);
BOOL unit_medium_is_present(struct TrackFileUnit *tfu);
BOOL unit_medium_is_busy(struct TrackFileUnit * tfu);

/****************************************************************************/

#endif /* _UNIT_H */
