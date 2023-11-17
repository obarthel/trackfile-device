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
#include "system_headers.h"
#endif /* _SYSTEM_HEADERS_H */

/****************************************************************************/

#include <proto/trackfile.h>

/****************************************************************************/

#ifndef _TRACKFILE_DEVICE_H
#include "trackfile_device.h"
#endif /* _TRACKFILE_DEVICE_H */

/****************************************************************************/

#include "assert.h"

/****************************************************************************/

#include "functions.h"
#include "tools.h"
#include "unit.h"

/****************************************************************************/

/* This may come in handy for volume node testing later. */
struct short_bcpl_string
{
	TEXT text[4];
};

/****************************************************************************/

/* Look for a volume node with a specific name and creation date which
 * might already be in use. Once we find one, we send the equivalent to
 * Lock(":", SHARED_LOCK) to the file system which tends to the volume
 * node (with a ZERO current directory lock). If the file system returns
 * a valid file lock instead of ZERO and an error code, then that
 * volume node is probably currently in use.
 */
static BPTR
get_volume_root_lock(
	const TEXT *				volume_name,
	const struct DateStamp *	volume_date,
	struct TrackFileDevice *	tfd)
{
	USE_EXEC(tfd);
	USE_DOS(tfd);
	USE_UTILITY(tfd);

	D_S(struct StandardPacket, sp);
	D_S(struct short_bcpl_string, short_path_name);

	struct MsgPort reply_port;
	struct DosList * dol;
	BPTR result = ZERO;
	size_t volume_name_len;
	const UBYTE * dol_name;
	size_t dol_name_len;

	ENTER();

	volume_name_len = strlen(volume_name);

	SHOWSTRING(volume_name);
	SHOWVALUE(volume_name_len);

	/* Set up the DOS packet return port. We only use
	 * it locally...
	 */
	memset(&reply_port, 0, sizeof(reply_port));

	init_msgport(&reply_port, FindTask(NULL), SIGB_SINGLE);

	/* Now fill in a standard DOS packet which we will
	 * use to obtain a lock on the root directory of
	 * a volume.
	 */
	memset(sp, 0, sizeof(*sp));

	sp->sp_Msg.mn_Node.ln_Name	= (char *)&sp->sp_Pkt;
	sp->sp_Pkt.dp_Link			= &sp->sp_Msg;
	sp->sp_Pkt.dp_Port			= &reply_port;

	/* This is a BCPL string, really, of length 1 with the
	 * path name ":" requested as the object to obtain
	 * a lock on. Note that the text buffer has space only
	 * for a short string (maximum 4 characters including
	 * the terminating NUL).
	 */
	strcpy(short_path_name->text, "\1:");

	/* Set up the packet for the equivalent of
	 * Lock(":", SHARED_LOCK) with a ZERO current
	 * directory.
	 */
	sp->sp_Pkt.dp_Type = ACTION_LOCATE_OBJECT;
	sp->sp_Pkt.dp_Arg1 = ZERO;
	sp->sp_Pkt.dp_Arg2 = MKBADDR(short_path_name);
	sp->sp_Pkt.dp_Arg3 = SHARED_LOCK;

	/* Look at every single volume node which might
	 * fit the bill...
	 */
	for(dol = NextDosEntry(LockDosList(LDF_VOLUMES|LDF_READ), LDF_VOLUMES) ;
	    dol != NULL ;
	    dol = NextDosEntry(dol, LDF_VOLUMES))
	{
		/* There should be a file system attached to it. */
		if(dol->dol_Task == NULL)
			continue;

		/* The name should match what we came to look for. */
		dol_name		= BADDR(dol->dol_Name);
		dol_name_len	= dol_name[0];

		if(dol_name_len != volume_name_len || Strnicmp(&dol_name[1], volume_name, volume_name_len) != SAME)
			continue;

		/* The volume creation date and time should match, too. */
		if(memcmp(&dol->dol_misc.dol_volume.dol_VolumeDate, volume_date, sizeof(*volume_date)) != SAME)
			continue;

		/* Ask the file system to return a lock on the root
		 * directory of the volume.
		 */
		sp->sp_Pkt.dp_Res1 = ZERO;
		sp->sp_Pkt.dp_Res2 = OK;

		SetSignal(0, (1UL << reply_port.mp_SigBit));
		PutMsg(dol->dol_Task, &sp->sp_Msg);
		WaitPort(&reply_port);

		/* If this actually worked then the file system is
		 * currently in possession of the volume node and
		 * would not appreciate it if a different file system
		 * process snatched it away.
		 */
		result = sp->sp_Pkt.dp_Res1;
		if(result != ZERO)
			break;
	}

	UnLockDosList(LDF_VOLUMES|LDF_READ);

	RETURN(result);
	return(result);
}

/****************************************************************************/

/* Update the disk checksum if necessary. This will be necessary
 * if individual track checksums have been updated, since the
 * disk checksum is an aggregate of all the track checksums.
 */
static void
update_disk_checksum(struct TrackFileUnit * tfu)
{
	ENTER();

	/* Was one of the track checksums updated? */
	if(tfu->tfu_ChecksumUpdated && tfu->tfu_DiskChecksumTable != NULL)
	{
		/* Update the disk checksum, which aggregates the
		 * track checksums. An extra track is reserved for
		 * storing the image file size. The size is part of
		 * the checksum to make sure that two disk image
		 * files of different sizes, and padded with zero
		 * bytes, do not yield the same disk checksum.
		 */

		ASSERT( tfu->tfu_NumTracks < tfu->tfu_DiskChecksumTableLength+1 );

		tfu->tfu_DiskChecksumTable[tfu->tfu_NumTracks].f64c_high	= 0;
		tfu->tfu_DiskChecksumTable[tfu->tfu_NumTracks].f64c_low		= tfu->tfu_FileSize;

		fletcher64_checksum(
			tfu->tfu_DiskChecksumTable,
			sizeof(*tfu->tfu_DiskChecksumTable) * (tfu->tfu_NumTracks + 1),
			&tfu->tfu_DiskChecksum);

		D(("disk checksum for unit %ld = %08lx%08lx", tfu->tfu_UnitNumber,
			tfu->tfu_DiskChecksum.f64c_high, tfu->tfu_DiskChecksum.f64c_low));

		tfu->tfu_ChecksumUpdated = FALSE;
	}

	LEAVE();
}

/****************************************************************************/

/****** trackfile.device/TFStartUnitTagList **********************************
*
*   NAME
*	TFStartUnitTagList - Start a trackfile.device unit, so that it
*	    may be used with TFInsertMediaTagList().
*
*   SYNOPSIS
*	unit = TFStartUnitTagList(which_unit, tags)
*	     D0                           D0       A0
*
*	LONG TFStartUnitTagList(LONG which_unit, CONST struct TagItem *tags);
*
*	LONG TFStartUnitTags(LONG which_unit, ...);
*
*   FUNCTION
*	Before a trackfile.device unit can take control of an ADF disk image file
*	it first needs to be started. Starting a unit involves allocating memory
*	for the maintenance of the unit as well as creating a Process which will
*	respond to the commands the unit has to perform.
*
*   INPUTS
*	which_unit -- Which unit to start. Unit numbers must be >= 0. If you do
*	    not have a preference for a specific unit number, use
*	    TFSU_NextAvailableUnit instead which will pick the next currently
*	    unused unit number for you.
*
*	tags -- Pointer to a list of TagItems; this may be NULL.
*
*   TAGS
*	TF_DriveType (LONG) -- You may request that the unit to be started
*	    should be specific type. The type must either be DRIVE3_5 (for a
*	    double-density 3.5" disk) or DRIVE3_5_150RPM (for a high-density
*	    3.5" disk). Defaults to DRIVE3_5.
*
*	TF_EnableChecksums (BOOL) -- As a disk is being used, a checksum may be
*	    calculated over all the tracks which can be useful to identify
*	    specific disk contents. This checksum is updated in real time as
*	    disk data is being modified. Additional memory may need to be
*	    allocated in order to keep track of the checksums, and a small
*	    overhead may be incurred when track data is being modified.
*
*	    The primary purpose of the checksums is to detect if disk image
*	    files with identical contents are to be inserted. If this is
*	    detected, the attempt will be aborted with an error code returned
*	    by the TFInsertMediaTagList() function.
*
*	    The TF_EnableChecksums tag value defaults to FALSE.
*
*	TF_MaxCacheMemory (ULONG) -- trackfile.device may make use of a cache
*	    which is shared by all units, speeding up disk file read accesses.
*	    How much memory (in bytes) may be used by the cache at a time for
*	    all the units may be preset once.
*
*	    Please note that TF_MaxCacheMemory is only supported if the unit
*	    is TFUNIT_CONTROL and the cache has not been set up yet. Defaults
*	    to 0 (the cache is not set up).
*
*   RESULT
*	unit - If successful, the number of the unit started (a value >= 0) or
*	    otherwise a negative value indicating an error.
*
*   ERRORS
*	TFERROR_Denied -- TFStartUnitTagList() can only be called by a Process,
*	    never by a Task.
*
*	TFERROR_InvalidDriveType -- The TF_DriveType tag value is not known or
*	    supported.
*
*	TFERROR_UnitBusy -- The unit you wanted to start is already operational.
*
*	TFERROR_OutOfMemory -- Memory allocation has failed.
*
*	TFERROR_ProcessFailed -- The unit Process could not be started, e.g.
*	    because of memory allocation failure. Check with dos.library/IoErr()
*	    to learn more.
*
*   SEE ALSO
*	<exec/errors.h>, <devices/trackdisk.h>, <devices/trackfile.h>,
*	TFInsertMediaTagList(), TFStopUnitTagList(), TFGetUnitData()
******************************************************************************
*
*/

LONG ASM
tf_start_unit_taglist(
	REG(d0, LONG which_unit),
	REG(a0, struct TagItem * tags),
	REG(a6, struct TrackFileDevice *tfd))
{
	USE_EXEC(tfd);
	USE_DOS(tfd);
	USE_UTILITY(tfd);

	/* 80 cylinders and 2 heads for a 3.5" disk drive. */
	const int num_cylinders	= NUMCYLS;
	const int num_heads		= NUMHEADS;

	struct TrackFileUnit * existing_tfu = NULL;
	struct TrackFileUnit * tfu;
	LONG drive_type;
	LONG result;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	ASSERT( FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS );

	SHOWVALUE(which_unit);
	SHOWPOINTER(tags);

	SHOWMSG("obtaining device lock");
	ObtainSemaphore(&tfd->tfd_Lock);

	/* Paranoia? */
	if(FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS)
	{
		SHOWMSG("this function cannot be called safely by a Task, it needs a Process");

		result = TFERROR_Denied;
		goto out;
	}

	#if defined(ENABLE_CACHE)
	{
		/* If the cache has not been set up yet, check if the
		 * client asked for the cache to be a specific size
		 * greater than 0. If this is found, create the
		 * cache data structures and set the maximum amount
		 * of memory which the cache may use.
		 */
		if(tfd->tfd_CacheContext == NULL)
		{
			ULONG cache_size;

			SHOWMSG("cache has not been set up yet; checking for cache size option");

			cache_size = GetTagData(TF_MaxCacheMemory, 0, tags);
			if(cache_size > 0)
			{
				D(("TF_MaxCacheMemory = %lu", cache_size));

				tfd->tfd_CacheContext = create_cache_context(tfd, TD_SECTOR * NUMSECS);
				if(tfd->tfd_CacheContext == NULL)
				{
					SHOWMSG("could not create cache");

					result = ERROR_NO_FREE_STORE;
					goto out;
				}

				D(("setting maximum cache size to %lu bytes", cache_size));

				change_cache_size(tfd->tfd_CacheContext, cache_size);
			}
			else
			{
				SHOWMSG("won't set up the cache just now");
			}
		}
		else
		{
			SHOWMSG("cache has already been set up; ignoring any TF_MaxCacheMemory tags");
		}
	}
	#endif /* ENABLE_CACHE */

	/* Ask for a specific type of drive? */
	drive_type = GetTagData(TF_DriveType, DRIVE3_5, tags);
	if(drive_type != DRIVE3_5 && drive_type != DRIVE3_5_150RPM)
	{
		SHOWMSG("this is not a supported drive type");

		result = TFERROR_InvalidDriveType;
		goto out;
	}

	if (drive_type == DRIVE3_5)
		SHOWMSG("drive type = DRIVE3_5");
	else if (drive_type == DRIVE3_5_150RPM)
		SHOWMSG("drive type = DRIVE3_5_150RPM");
	else
		SHOWMSG("drive type = DRIVE5_25");

	/* Any unit will do? */
	if(which_unit < 0)
	{
		SHOWMSG("any unit will do");

		/* No unit currently active? */
		if(IsMinListEmpty(&tfd->tfd_UnitList))
		{
			SHOWMSG("no unit is currently active; using unit 0");

			which_unit = 0;
		}
		/* We need to find a unit which isn't currently in use. */
		else
		{
			LONG max_unit_number = -1;

			for(tfu = (struct TrackFileUnit *)tfd->tfd_UnitList.mlh_Head ;
			    tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ != NULL ;
			    tfu = (struct TrackFileUnit *)tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ)
			{
				if(NOT unit_is_active(tfu) || NOT unit_medium_is_present(tfu))
				{
					D(("we'll reuse unit #%ld", tfu->tfu_UnitNumber));

					which_unit = tfu->tfu_UnitNumber;

					existing_tfu = tfu;
					break;
				}
			}

			/* Will we have to make up a new unit? */
			if(existing_tfu == NULL)
			{
				for(tfu = (struct TrackFileUnit *)tfd->tfd_UnitList.mlh_Head ;
				    tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ != NULL ;
				    tfu = (struct TrackFileUnit *)tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ)
				{
					if(max_unit_number < tfu->tfu_UnitNumber)
						max_unit_number = tfu->tfu_UnitNumber;
				}

				SHOWVALUE(max_unit_number);

				/* Pick an unused unit number and also make sure
				 * that the last used number is not MAXINT.
				 */
				if(max_unit_number < 0 || (max_unit_number + 1) < 0)
				{
					SHOWMSG("no usable unit available");

					result = TFERROR_UnitBusy;
					goto out;
				}

				which_unit = max_unit_number + 1;

				D(("using unit %ld", which_unit));
			}
		}
	}
	else
	{
		D(("checking unit %ld", which_unit));

		existing_tfu = find_unit_by_number(tfd, which_unit);
		if(existing_tfu == NULL)
			SHOWMSG("this unit is not yet running");
	}

	/* We'll try to reuse the existing unit. */
	if(existing_tfu != NULL)
	{
		SHOWMSG("reusing existing unit");

		tfu = existing_tfu;
	}
	/* No unit can be reused? Then we'll have to make something up. */
	else
	{
		SHOWMSG("allocating memory for new unit");

		tfu = AllocVec(sizeof(*tfu), MEMF_ANY|MEMF_PUBLIC|MEMF_CLEAR);
		if(tfu == NULL)
		{
			SHOWMSG("not enough memory");

			result = TFERROR_OutOfMemory;
			goto out;
		}

		tfu->tfu_NumCylinders			= num_cylinders;
		tfu->tfu_NumHeads				= num_heads;
		tfu->tfu_NumTracks				= num_cylinders * num_heads;
		tfu->tfu_Device					= tfd;
		tfu->tfu_UnitNumber				= which_unit;
		tfu->tfu_CurrentTrackNumber		= -1;

		/* If checksums are enabled for this unit, allocate memory
		 * for storing these.
		 */
		if(GetTagData(TF_EnableChecksums, FALSE, tags) != FALSE)
		{
			LONG allocation_size;

			/* We allocate memory for up to 160 tracks, plus one extra
			 * record which will contain the disk size information.
			 */
			tfu->tfu_DiskChecksumTableLength = NUMCYLS * NUMHEADS;

			D(("disk and track checksums are enabled for unit %ld; %ld tracks will be available",
				which_unit, tfu->tfu_DiskChecksumTableLength));

			allocation_size = sizeof(*tfu->tfu_DiskChecksumTable) * (1+tfu->tfu_DiskChecksumTableLength);

			tfu->tfu_DiskChecksumTable = AllocVec(allocation_size, MEMF_ANY|MEMF_PUBLIC);
			if(tfu->tfu_DiskChecksumTable == NULL)
			{
				SHOWMSG("not enough memory for track checksum table");

				FreeVec(tfu);

				result = TFERROR_OutOfMemory;
				goto out;
			}
		}
		else
		{
			D(("unit %ld does not use disk and track checksums", which_unit));
		}

		/* This is the default setup used by trackdisk.device V36
		 * and beyond. Not much here is useful beyond the current
		 * track number.
		 */
		tfu->tfu_Unit.tdu_Comp01Track		= tfu->tfu_NumCylinders;
		tfu->tfu_Unit.tdu_Comp10Track		= (UWORD)-1;
		tfu->tfu_Unit.tdu_Comp11Track		= (UWORD)-1;
		tfu->tfu_Unit.tdu_StepDelay			= 3000;
		tfu->tfu_Unit.tdu_SettleDelay		= 15000;
		tfu->tfu_Unit.tdu_RetryCnt			= 10;
		tfu->tfu_Unit.tdu_CurrTrk			= (UWORD)-1;
		tfu->tfu_Unit.tdu_CalibrateDelay	= 4000;

		InitSemaphore(&tfu->tfu_Lock);

		NewMinList(&tfu->tfu_ChangeIntList);

		#if defined(ENABLE_CACHE)
		{
			NewMinList(&tfu->tfu_CacheNodeList);
		}
		#endif /* ENABLE_CACHE */

		D(("adding unit 0x%08lx with number %lu", tfu, tfu->tfu_UnitNumber));

		AddHeadMinList(&tfd->tfd_UnitList, &tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node);
	}

	/* Is no unit process active at the moment?
	 * Then we'll have to do something about that.
	 */
	if(tfu->tfu_Process == NULL)
	{
		struct Message unit_start_message;
		struct MsgPort unit_reply_port;
		struct Process * unit_process;

		TEXT unit_process_name[256];

		/* Setting/updating the drive type is safe
		 * only as long as the unit is not
		 * currently active.
		 */
		tfu->tfu_DriveType = drive_type;

		/* Pick a name for the unit process. */
		local_snprintf(tfd, unit_process_name, sizeof(unit_process_name), "%s V%ld.%ld unit #%lu",
			tfd->tfd_Device.dd_Library.lib_Node.ln_Name,
			tfd->tfd_Device.dd_Library.lib_Version,
			tfd->tfd_Device.dd_Library.lib_Revision,
			which_unit
		);

		SHOWMSG("launching the unit process");

		/* Launch the unit process without also cloning the
		 * directories of the parent Process or its local
		 * variables. The unit process does not need them.
		 */
		unit_process = CreateNewProcTags(
			NP_Name,		unit_process_name,
			NP_Entry,		UnitProcessEntry,
			NP_Priority,	5,
			NP_WindowPtr,	-1,
			NP_ConsoleTask,	NULL,
			NP_HomeDir,		ZERO,
			NP_CurrentDir,	ZERO,
			NP_CopyVars,	FALSE,
			NP_Path,		ZERO,
		TAG_DONE);

		if(unit_process == NULL)
		{
			D(("unit process creation failed, error=%ld", IoErr()));

			/* Remove the unit if we just created it. */
			if(tfu != existing_tfu)
			{
				Remove(&tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node);

				FreeVec(tfu->tfu_DiskChecksumTable);
				FreeVec(tfu);
			}

			result = TFERROR_ProcessFailed;
			goto out;
		}

		SHOWMSG("finishing initialization of child process");

		/* Send a startup message to the unit process, then wait for it
		 * to return the message. We need to set up a message port
		 * and the message to be sent first.
		 */
		memset(&unit_reply_port, 0, sizeof(unit_reply_port));

		init_msgport(&unit_reply_port, FindTask(NULL), SIGB_SINGLE);

		memset(&unit_start_message, 0, sizeof(unit_start_message));

		/* The unit data structure address is transported
		 * through the message name pointer.
		 */
		unit_start_message.mn_Node.ln_Name	= (char *)tfu;
		unit_start_message.mn_ReplyPort		= &unit_reply_port;
		unit_start_message.mn_Length		= sizeof(unit_start_message);

		/* Important: Clear the signal bit before we wait for
		 *            the message to come back.
		 */
		SetSignal(0, (1UL << unit_reply_port.mp_SigBit));
		PutMsg(&unit_process->pr_MsgPort, &unit_start_message);
		WaitPort(&unit_reply_port);

		/* Did the process fail to start correctly? */
		if(tfu->tfu_Process == NULL)
		{
			SHOWMSG("unit process creation failed");

			/* Remove the unit if we just created it. */
			if(tfu != existing_tfu)
			{
				Remove(&tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node);

				FreeVec(tfu->tfu_DiskChecksumTable);
				FreeVec(tfu);
			}

			result = TFERROR_ProcessFailed;
			goto out;
		}
	}

	SHOWMSG("that went well");

	result = which_unit;

 out:

	SHOWMSG("releasing device lock");
	ReleaseSemaphore(&tfd->tfd_Lock);

	RETURN(result);
	return(result);
}

/***********************************************************************/

/****** trackfile.device/TFStopUnitTagList ***********************************
*
*   NAME
*	TFStopUnitTagList - Stop a trackfile.device unit, releasing as much
*	    memory possible.
*
*   SYNOPSIS
*	error = TFStopUnitTagList(which_unit, tags)
*	  D0                         D0        A0
*
*	LONG TFStopUnitTagList(LONG which_unit, CONST struct TagItem *tags);
*
*	LONG TFStopUnitTags(LONG which_unit, ...);
*
*   FUNCTION
*	If a trackfile.device unit is currently operational but no longer
*	needed, it can be shut down. This will release some memory such as used
*	by the unit Process. If a unit is stopped, it can be restarted through
*	the TFStartUnitTagList() function.
*
*   INPUTS
*	which_unit -- Which unit to stop. Unit numbers must be >= 0.
*
*	tags -- Pointer to a list of TagItems; this may be NULL.
*
*   TAGS
*	No tags are currently applicable for the TFStopUnitTagList()
*	function.
*
*   RESULT
*	error - Zero if successful, otherwise an error code is returned.
*
*   ERRORS
*	TFERROR_UnitNotFound -- The unit you wanted to stop is not currently
*	    known or active.
*
*	ERROR_OBJECT_IN_USE -- The unit you wanted to stop still has an ADF disk
*	    file attached and is busy. You may want to eject the ADF disk image
*	    file first through the TFEjectMediaTagList() function.
*
*	TFERROR_Aborted -- The unit you wanted to shut is already in the process
*	    of shutting and has ignored your request to shut down.
*
*   SEE ALSO
*	<exec/errors.h>, <devices/trackdisk.h>, <devices/trackfile.h>,
*	TFEjectMediaTagList(), TFStartUnitTagList(), TFGetUnitData()
******************************************************************************
*
*/

LONG ASM
tf_stop_unit_taglist(
	REG(d0, LONG which_unit),
	REG(a0, struct TagItem * tags),
	REG(a6, struct TrackFileDevice *tfd))
{
	USE_EXEC(tfd);

	struct TrackFileUnit * tfu;
	LONG result;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	SHOWVALUE(which_unit);
	SHOWPOINTER(tags);

	SHOWMSG("obtaining device lock");
	ObtainSemaphore(&tfd->tfd_Lock);

	/* Let's see which unit was requested. */
	tfu = find_unit_by_number(tfd, which_unit);
	if(tfu == NULL)
	{
		D(("didn't find unit %ld", which_unit));

		result = TFERROR_UnitNotFound;
		goto out;
	}

	SHOWMSG("telling unit process to shut down");

	/* Now ask the unit Process to shut down. */
	result = send_unit_control_command(tfu, TFC_Stop, ZERO, 0, FALSE, -1);
	if(result != OK)
	{
		D(("that didnt't work (error=%ld)", result));
		goto out;
	}

	SHOWMSG("that went well");

	result = OK;

 out:

	SHOWMSG("releasing device lock");
	ReleaseSemaphore(&tfd->tfd_Lock);

	RETURN(result);
	return(result);
}

/***********************************************************************/

/****** trackfile.device/TFInsertMediaTagList ********************************
*
*   NAME
*	TFInsertMediaTagList - Insert a floppy disk into a
*	    trackfile.device unit by loading a floppy disk image file.
*
*   SYNOPSIS
*	error = TFInsertMediaTagList(which_unit, tags)
*	  D0                            D0        A0
*
*	LONG TFInsertMediaTagList(LONG which_unit,
*	                          CONST struct TagItem *tags);
*
*	LONG TFInsertMediaTags(LONG which_unit, ...);
*
*   FUNCTION
*	After a trackfile.device unit has been started, or its "floppy disk" has
*	been ejected, a floppy disk image file may be "inserted" into it. You
*	need to specify either the name of the floppy disk image file in
*	question or you may provide an already opened file.
*
*   INPUTS
*	which_unit -- Which unit to insert the disk image file into.
*	    Unit numbers must be >= 0.
*
*	tags -- Pointer to a list of TagItems; this may be NULL.
*
*   TAGS
*	TF_ImageFileName (STRPTR) -- Name of the floppy disk image file
*	    to "insert". This may include the full path to the file or may be
*	    just the file name relative to the current directory of the Process
*	    calling this function. Defaults to NULL.
*
*	    Please note that the TF_ImageFileName TagItem will be ignored if
*	    you use the TF_ImageFileHandle with a non-NULL file handle value.
*
*	TF_ImageFileHandle (BPTR) -- Use an already open file instead of
*	    opening a named file. Defaults to NULL.
*
*	    Please note that the TF_ImageFileName TagItem will be ignored if
*	    you use the TF_ImageFileHandle with a non-NULL file handle value.
*
*	TF_WriteProtected (BOOL) -- You may request that the disk image
*	    file to be used should be write-enabled. Defaults to FALSE.
*
*	TF_EnableUnitCache (BOOL) - If the shared read cache for all
*	    trackfile.device units was enabled, you may request that the unit
*	    for which you are now loading a disk image file should make use of
*	    that cache. Defaults to FALSE.
*
*	TF_PrefillUnitCache (BOOL) - If you enabled the use of the
*	    shared unit cache (and that cache is active) you may want the entire
*	    disk image file you are loading disk to be cached. This involves
*	    reading the file and storing its contents in the cache first,
*	    though. Defaults to FALSE.
*
*   RESULT
*	error - Zero if successful, otherwise an error code is returned.
*
*   ERRORS
*	TFERROR_Denied -- Only a Process may call the
*	    TFInsertMediaTagList() function, never a Task.
*
*	TFERROR_UnitNotFound -- The unit you requested is either not
*	    known or not currently active.
*
*	TFERROR_AlreadyInUse -- The unit you wanted to insert the disk
*	    image file into is already using a disk image.
*
*	TFERROR_InvalidFile -- The disk image file handle you provided
*	    is unusable. Check with dos.library/IoErr() for more information.
*
*	TFERROR_NoFileGiven -- You need to provide either the
*	    TF_ImageFileName or TF_ImageFileHandle tags to indicate the disk
*	    image file to be used.
*
*	TFERROR_InvalidFileSize -- The disk image file you provided does
*	    not match the supported 880 KByte or 1760 KByte disk image file
*	    types.
*
*	TFERROR_DuplicateVolume -- There is already is a disk mounted
*	    which shares the same volume name and volume creation signature with
*	    the disk image file you want to insert. trackfile.device refuses to
*	    use it because it would likely cause an AmigaDOS crash.
*
*	TFERROR_DuplicateDisk -- You enabled the disk checksumming when
*	    you started this unit, and there is currently a disk inserted in one
*	    of the other trackfile.device units which exactly matches the disk
*	    checksum of the disk image file you wanted to insert.
*
*	TFERROR_OutOfMemory -- Memory could not be allocated.
*
*   SEE ALSO
*	<exec/errors.h>, <devices/trackdisk.h>, <devices/trackfile.h>,
*	<dos/dos.h> TFEjectMediaTagList(), TFStartUnitTagList(),
*	TFGetUnitData()
******************************************************************************
*
*/

/* If possible, call the TFExamineFileSize() function through the
 * library base rather than calling the local function.
 */
#if defined(__SASC)
LONG TFExamineFileSize( LONG file_size );
#pragma libcall TrackFileBase TFExamineFileSize 54 001
#endif /* __SASC */

LONG ASM
tf_insert_media_taglist(
	REG(d0, LONG						which_unit),
	REG(a0, struct TagItem *			tags),
	REG(a6, struct TrackFileDevice *	tfd))
{
	USE_EXEC(tfd);
	USE_DOS(tfd);
	USE_UTILITY(tfd);

	struct TrackFileUnit * which_tfu;
	struct TrackFileUnit * tfu;
	LONG result;
	struct TagItem * list = tags;
	struct TagItem * ti;
	BPTR image_file_handle = ZERO;
	struct FileHandle * fh;
	STRPTR image_file_name = NULL;
	BPTR file = ZERO;
	D_S(struct FileInfoBlock, fib);
	BPTR image_file_handle_lock = ZERO;
	BOOL found_identical_disk;
	BOOL write_protected = TRUE;
	BPTR unit_file_lock;
	struct AlignedMemoryAllocation track_memory;
	LONG drive_type;
	int bytes_per_sector, blocks_per_disc, sectors_per_track, num_surfaces, num_cylinders, sectors_per_block;
	int num_reserved_blocks;
	int root_directory_block_number, root_directory_track_number, root_directory_block_offset;
	LONG num_bytes_read;
	UBYTE * track_buffer;
	LONG track_size;
	BOOL volume_already_in_use = FALSE;
	BOOL prefill_unit_cache = FALSE;
	BOOL change_unit_cache = FALSE;
	BOOL enable_unit_cache = FALSE;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	ASSERT( FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS );

	memset(&track_memory, 0, sizeof(track_memory));

	SHOWVALUE(which_unit);
	SHOWPOINTER(tags);

	SHOWMSG("obtaining device lock");
	ObtainSemaphore(&tfd->tfd_Lock);

	/* Paranoia? */
	if(FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS)
	{
		SHOWMSG("this function cannot be called safely by a Task, it needs a Process");

		result = TFERROR_Denied;
		goto out;
	}

	/* Let's see which unit was requested. */
	which_tfu = find_unit_by_number(tfd, which_unit);
	if(which_tfu == NULL)
	{
		D(("didn't find unit %ld", which_unit));

		result = TFERROR_UnitNotFound;
		goto out;
	}

	/* You can change the medium only if there is currently
	 * none present.
	 */
	if(unit_medium_is_present(which_tfu))
	{
		result = TFERROR_AlreadyInUse;
		goto out;
	}

	/* Now let's see which file we can use, or if there
	 * is a file/path name provided to open it with.
	 */
	while((ti = NextTagItem(&list)) != NULL)
	{
		switch(ti->ti_Tag)
		{
			/* The client supplied the name of an image
			 * file. We'll have to open it later.
			 */
			case TF_ImageFileName:

				image_file_name = (STRPTR)ti->ti_Data;

				if(image_file_name != NULL)
					D(("TF_ImageFileName='%s'", image_file_name));
				else
					D(("TF_ImageFileName=NULL"));

				break;

			/* The client supplied a ready-made file handle.
			 * Note that if you provide both a file name and
			 * a file handle, then the file name will be
			 * ignored.
			 */
			case TF_ImageFileHandle:

				image_file_handle = (BPTR)ti->ti_Data;

				if(image_file_handle != ZERO)
					D(("TF_ImageFileHandle=0x%08lx", image_file_handle));
				else
					D(("TF_ImageFileHandle=ZERO"));

				break;

			/* The client may want the medium to be writable. */
			case TF_WriteProtected:

				write_protected = (BOOL)(ti->ti_Data != 0);

				D(("TF_WriteProtected=%s", write_protected ? "TRUE" : "FALSE"));

				break;

		#if defined(ENABLE_CACHE)

			case TF_EnableUnitCache:

				D(("TF_EnableUnitCache=%s", ti->ti_Data ? "TRUE" : "FALSE"));

				change_unit_cache = TRUE;
				enable_unit_cache = (BOOL)(ti->ti_Data != FALSE);

				break;

			case TF_PrefillUnitCache:

				D(("TF_PrefillUnitCache=%s", ti->ti_Data ? "TRUE" : "FALSE"));

				prefill_unit_cache = (ti->ti_Data != FALSE);

				break;

		#endif /* ENABLE_CACHE */

			default:

				break;
		}
	}

	/* If provided, the file handle should look
	 * reasonably sound.
	 */
	if(image_file_handle != ZERO && NOT IS_VALID_BPTR_ADDRESS(image_file_handle))
	{
		SHOWMSG("that file handle doesn't look so good");

		result = TFERROR_InvalidFile;
		goto out;
	}

	/* If no file handle was provided, let's hope that
	 * a file/path name was.
	 */
	if(image_file_handle == ZERO)
	{
		if(image_file_name == NULL)
		{
			SHOWMSG("we need a file name");

			result = TFERROR_NoFileGiven;
			goto out;
		}

		file = Open(image_file_name, MODE_OLDFILE);
		if(file == ZERO)
		{
			result = IoErr();

			D(("that file '%s' didn't open (error=%ld)", image_file_name, result));

			goto out;
		}

		image_file_handle = file;
	}

	/* Let's have a look at the file. We are
	 * mostly interested in the file size but
	 * the file protection flags are
	 * interesting, too.
	 */
	if(CANNOT ExamineFH(image_file_handle, fib))
	{
		result = IoErr();

		/* Hm... this may be a file system which does not
		 * support ExamineFH() or any of the new AmigaDOS 2.x
		 * library functions.
		 */
		if(result == ERROR_ACTION_NOT_KNOWN)
		{
			BOOL file_size_known = FALSE;

			SHOWMSG("cannot obtain file information through ExamineFH()");

			/* Do we have a name to go on? Then calling Lock() and Examine()
			 * will be easier and faster to accomplish than the alternative
			 * of using Seek(), especially on large files.
			 */
			if(image_file_name != NULL)
			{
				image_file_handle_lock = Lock(image_file_name, SHARED_LOCK);
				if(image_file_handle_lock != ZERO)
					file_size_known = (BOOL)(Examine(image_file_handle_lock, fib) != DOSFALSE);
			}

			/* If we did not learn the file size by examining the file,
			 * we will use Seek() instead. Note that this method does
			 * not scale well for large files.
			 */
			if(NOT file_size_known)
			{
				/* Start with a clean slate. */
				memset(fib, 0, sizeof(*fib));

				/* Try to divine the file size by different means.
				 * We move the file seek pointer to the end of the
				 * file and then back to the beginning again. This
				 * should yield the file size.
				 */
				if(Seek(image_file_handle, 0, OFFSET_END) != -1)
				{
					LONG position;

					position = Seek(image_file_handle, 0, OFFSET_BEGINNING);
					if(position >= 0)
					{
						D(("file size = %ld bytes", position));

						fib->fib_Size = position;

						result = OK;
					}
					else
					{
						D(("seek operation failed, error=%ld", IoErr()));
					}
				}
				else
				{
					D(("seek operation failed, error=%ld", IoErr()));
				}
			}
		}

		if(result != OK)
		{
			D(("could not examine file (error=%ld)", result));

			goto out;
		}
	}

	/* This will be used later. */
	fh = BADDR(image_file_handle);

	/* If possible, call the TFExamineFileSize() function through the
	 * library base rather than calling the local function.
	 */
	#if defined(__SASC)
	{
		struct TrackFileDevice * TrackFileBase = tfd;

		drive_type = TFExamineFileSize(fib->fib_Size);
	}
	#else
	{
		drive_type = tf_examine_file_size(fib->fib_Size, tfd);
	}
	#endif /* __SASC */

	/* This should either be a standard double density
	 * disk or a high density disk. That's 80 cylinders
	 * and either 11 or 22 sectors per track.
	 */
	if(drive_type == TFEFS_Unsupported)
	{
		D(("file is the wrong size (%ld bytes); should be either %ld (DD) or %ld (HD)",
			fib->fib_Size,
			which_tfu->tfu_NumTracks * NUMSECS * TD_SECTOR,
			which_tfu->tfu_NumTracks * (2*NUMSECS) * TD_SECTOR));

		result = TFERROR_InvalidFileSize;
		goto out;
	}

	/* If the file is write-protected, assume that we won't be
	 * able to write to it for the time being.
	 */
	if(FLAG_IS_SET(fib->fib_Protection, FIBF_WRITE))
	{
		if(NOT write_protected)
			SHOWMSG("file is write-protected; switching to read-only access");

		write_protected = TRUE;
	}

	/* Make sure that this file is not currently in use
	 * by a different unit already. The FFS always has
	 * trouble telling identical volumes apart.
	 *
	 * Note that DupLockFromFH() may fail because the
	 * file system does not support the packet type
	 * underlying its functionality.
	 */
	if(image_file_handle_lock == ZERO)
		image_file_handle_lock = DupLockFromFH(image_file_handle);

	if(image_file_handle_lock != ZERO)
	{
		D_S(struct InfoData, disk_info);

		/* While we're at it, check if the disk is currently
		 * write-protected.
		 */
		if(Info(image_file_handle_lock, disk_info))
		{
			/* If the disk on which the file resides is write-protected
			 * or currently being validated, assume that we won't be
			 * able to write to the file either.
			 */
			if(disk_info->id_DiskState != ID_VALIDATED)
			{
				if(NOT write_protected)
					SHOWMSG("disk on which the file is found is not writable; switching to read-only access");

				write_protected = TRUE;
			}
		}
	}
	else
	{
		D(("could not get lock on this file, error=%ld", IoErr()));
	}

	ASSERT( fib->fib_Size > 0 );

	which_tfu->tfu_FileSize = fib->fib_Size;

	ASSERT( which_tfu->tfu_NumTracks > 0 );

	/* Now for the hard part: we need to figure out if the disk has
	 * a valid root directory and if the file system type (ID_DOS_DISK,
	 * ID_FFS_DISK, etc.) is well-known. The point of reading all
	 * of that information is to learn early on whether there is
	 * a volume of the same name as the disk's name which also
	 * shares its volume creation date. Such duplicate volumes will
	 * cause file systems to crash, and we hope to avoid this
	 * as early as possible.
	 */
	which_tfu->tfu_FileSystemSignature = ID_UNREADABLE_DISK;
	which_tfu->tfu_BootBlockChecksum = ~0UL;
	which_tfu->tfu_RootDirValid = FALSE;
	which_tfu->tfu_FilePosition = -1;

	track_size = fib->fib_Size / which_tfu->tfu_NumTracks;

	ASSERT( track_size > 0 );

	/* We do not have a track buffer yet. This will happen only
	 * directly before the disk image file is accepted. Hence
	 * we need a temporary track buffer now.
	 */
	result = allocate_aligned_memory(tfd, fh->fh_Type, track_size, &track_memory);
	if(result != OK)
	{
		D(("could not allocate temporary track buffer (size=%ld)", track_size));

		goto out;
	}

	track_buffer = track_memory.ama_Aligned;

	/* We take this as a given. */
	bytes_per_sector = TD_SECTOR;

	/* One sector per block is the default for a floppy disk drive. */
	sectors_per_block = 1;

	/* A floppy disk features two reserved blocks, which can
	 * contain the boot block code and at least the file system
	 * signature.
	 */
	num_reserved_blocks = 2;

	/* These we already know. */
	num_surfaces = which_tfu->tfu_NumHeads;
	num_cylinders = which_tfu->tfu_NumCylinders;

	/* This would be 11 or 22. */
	sectors_per_track = track_size / bytes_per_sector;

	SHOWVALUE(sectors_per_track);

	/* Figure out which block holds the root block. */
	blocks_per_disc = sectors_per_track * num_surfaces * num_cylinders / sectors_per_block;

	root_directory_block_number = (blocks_per_disc - 1 + num_reserved_blocks) / 2;

	SHOWVALUE(root_directory_block_number);

	/* Which track does the root block reside on? */
	root_directory_track_number = (root_directory_block_number * sectors_per_block) / sectors_per_track;

	SHOWVALUE(root_directory_track_number);

	/* And where on that track would we find the root block?
	 * We need the number of bytes from the beginning
	 * of the track.
	 */
	root_directory_block_offset =
		bytes_per_sector * sectors_per_block * root_directory_block_number -
		root_directory_track_number * sectors_per_track * bytes_per_sector;

	SHOWVALUE(root_directory_block_offset);

	/* Begin by looking into the reserved blocks. We are interested
	 * in the file system signature information.
	 */
	if(Seek(image_file_handle, 0, OFFSET_BEGINNING) == -1)
	{
		result = IoErr();

		D(("could not seek to beginning of the disk image file (error=%ld)", result));

		goto out;
	}

	ASSERT( num_reserved_blocks * sectors_per_block * bytes_per_sector <= track_size );

	num_bytes_read = Read(image_file_handle, track_buffer, num_reserved_blocks * sectors_per_block * bytes_per_sector);
	if(num_bytes_read == -1)
	{
		result = IoErr();

		D(("could not read reserved blocks of the disk image file (error=%ld)", result));

		goto out;
	}

	SHOWVALUE(num_bytes_read);

	/* Which type of file system is this? We only care about
	 * the Amiga default file system and its variants, e.g.
	 * OFS, FFS, DCFS, etc.
	 */
	if(num_bytes_read >= BOOTSECTS * bytes_per_sector * sectors_per_block)
	{
		which_tfu->tfu_FileSystemSignature = *(ULONG *)track_buffer;

		D(("file system signature = 0x%08lx", which_tfu->tfu_FileSystemSignature));

		which_tfu->tfu_BootBlockChecksum =
			calculate_boot_block_checksum((ULONG *)track_buffer, BOOTSECTS * bytes_per_sector * sectors_per_block);

		D(("boot block checksum = 0x%08lx", which_tfu->tfu_BootBlockChecksum));
	}

	/* Is this an AmigaDOS disk, e.g. OFS, FFS, DCFS, etc.? */
	if((which_tfu->tfu_FileSystemSignature & 0xFFFFFF00) == ID_DOS_DISK)
	{
		LONG root_directory_position;

		SHOWMSG("file system signature seems to be for the Amiga default file system");

		root_directory_position = root_directory_block_offset + root_directory_track_number * sectors_per_track * bytes_per_sector;

		D(("seek to position %ld", root_directory_position));

		/* Now read the root directory block proper. */
		if(Seek(image_file_handle, root_directory_position, OFFSET_BEGINNING) == -1)
		{
			result = IoErr();

			D(("could not seek to root directory of the disk image file (error=%ld)", result));

			goto out;
		}

		ASSERT( bytes_per_sector * sectors_per_block <= track_size );

		num_bytes_read = Read(image_file_handle, track_buffer, bytes_per_sector * sectors_per_block);
		if(num_bytes_read == -1)
		{
			result = IoErr();

			D(("could not read root directory of the disk image file (error=%ld)", result));

			goto out;
		}

		/* Did we get what we came for? */
		if(num_bytes_read == bytes_per_sector * sectors_per_block)
		{
			const struct RootDirBlock * rdb = (struct RootDirBlock *)track_buffer;

			/* Is the root block really what we need? */
			if(root_directory_is_valid(rdb))
			{
				TEXT root_directory_name[32];
				BPTR root_dir_lock;
				size_t len;

				len = rdb->rdb_Name[0];
				if(len >= sizeof(root_directory_name))
					len = sizeof(root_directory_name)-1;

				CopyMem(&rdb->rdb_Name[1], root_directory_name, len);
				root_directory_name[len] = '\0';

				D(("volume name = \"%s\"", root_directory_name));
				D(("creation date and time = %ld/%ld/%ld",
					rdb->rdb_DiskInitialization.ds_Days,
					rdb->rdb_DiskInitialization.ds_Minute,
					rdb->rdb_DiskInitialization.ds_Tick));

				which_tfu->tfu_RootDirTrackNumber	= root_directory_track_number;
				which_tfu->tfu_RootDirBlockOffset	= root_directory_block_offset;
				which_tfu->tfu_RootDirDate			= rdb->rdb_DiskInitialization;
				which_tfu->tfu_RootDirValid			= TRUE;

				ASSERT( len+1 <= sizeof(which_tfu->tfu_RootDirName) );

				CopyMem(root_directory_name, which_tfu->tfu_RootDirName, len+1);

				/* Now check if there is one volume node (or maybe more than one)
				 * which uses the same name as the disk and also uses the same
				 * creation date, and also has a file system process attached
				 * to it which uses it.
				 */
				root_dir_lock = get_volume_root_lock(which_tfu->tfu_RootDirName, &which_tfu->tfu_RootDirDate, tfd);
				if(root_dir_lock != ZERO)
				{
					SHOWMSG("Bad luck: there is already a volume equivalent to the disk contents.");

					UnLock(root_dir_lock);

					/* We can't use this... */
					volume_already_in_use = TRUE;
				}
				else
				{
					SHOWMSG("no file system process uses a volume equivalent to the disk contents.");
				}
			}
			else
			{
				SHOWMSG("root directory information is invalid");
			}
		}
		else
		{
			D(("failed to read %ld bytes, got %ld instead", bytes_per_sector * sectors_per_block, num_bytes_read));
		}
	}
	else
	{
		SHOWMSG("this does not appear to be an Amiga file system disk");
	}

	if(volume_already_in_use)
	{
		SHOWMSG("there is a volume node in active use which matches the volume data of this disk");

		result = TFERROR_DuplicateVolume;
		goto out;
	}

	/* Calculate the disk/track checksums for this unit file? */
	if(which_tfu->tfu_DiskChecksumTable != NULL)
	{
		int i;

		SHOWMSG("setting up the disk and track checksums for the file now");

		ASSERT( which_tfu->tfu_NumTracks > 0 );
		ASSERT( track_size > 0 );
		ASSERT( track_buffer != NULL );

		/* We may have just received this file handle as is, and
		 * it's not a given that the read position refers to the
		 * start of the file.
		 */
		if(Seek(image_file_handle, 0, OFFSET_BEGINNING) == -1)
		{
			result = IoErr();

			D(("could not seek to beginning of the disk image file (error=%ld)", result));

			goto out;
		}

		ASSERT( which_tfu->tfu_NumTracks <= which_tfu->tfu_DiskChecksumTableLength );

		/* We read the image file one track at a time and
		 * calculate the track checksum for each.
		 */
		for(i = 0 ; i < which_tfu->tfu_NumTracks ; i++)
		{
			num_bytes_read = Read(image_file_handle, track_buffer, track_size);
			if (num_bytes_read == -1)
			{
				result = IoErr();

				D(("could not read track %ld (error=%ld)", i, result));

				goto out;
			}
			else if (num_bytes_read != track_size)
			{
				D(("failed to read %ld bytes of track data; got only %ld", track_size, num_bytes_read));

				result = TFERROR_InvalidFileSize;
				goto out;
			}

			fletcher64_checksum(track_buffer, track_size, &which_tfu->tfu_DiskChecksumTable[i]);
		}

		/* Aggregate the track checksums to produce the disk checksum. */
		which_tfu->tfu_ChecksumUpdated = TRUE;

		update_disk_checksum(which_tfu);
	}

	/* We try to verify that the disk image file is not currently
	 * mounted by a different unit. This would likely lead the
	 * file system processes to slip up since they would battle
	 * for the same volume, for example.
	 *
	 * This test involves checking if the file to be used is the
	 * same as one currently used by a different unit. This can
	 * either mean that the files are identical, or that the
	 * disk and track checksums are identical.
	 */
	found_identical_disk = FALSE;

	for(tfu = (struct TrackFileUnit *)tfd->tfd_UnitList.mlh_Head ;
	    NOT found_identical_disk && tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ != NULL ;
	    tfu = (struct TrackFileUnit *)tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ)
	{
		if(tfu == which_tfu)
			continue;

		D(("obtaining unit %ld lock", tfu->tfu_UnitNumber));
		ObtainSemaphore(&tfu->tfu_Lock);

		D(("checking unit #%ld", tfu->tfu_UnitNumber));

		unit_file_lock = ZERO;

		if(unit_medium_is_present(tfu))
		{
			/* Compare the disk checksums, if possible, to
			 * find two disk images with identical contents.
			 */
			if(which_tfu->tfu_DiskChecksumTable != NULL && tfu->tfu_DiskChecksumTable != NULL)
			{
				update_disk_checksum(tfu);

				found_identical_disk = (BOOL)(compare_fletcher64_checksums(&which_tfu->tfu_DiskChecksum, &tfu->tfu_DiskChecksum) == SAME);

				if(found_identical_disk)
					SHOWMSG("found identical disk contents");
			}

			/* If necessary, also check if file handles
			 * refer to the same image file.
			 *
			 * Note that DupLockFromFH() may fail because the
			 * file system does not support the packet type
			 * underlying its functionality.
			 */
			if(NOT found_identical_disk)
			{
				SHOWMSG("getting a lock on that file");

				unit_file_lock = DupLockFromFH(tfu->tfu_File);
			}
		}

		D(("releasing unit %ld lock", tfu->tfu_UnitNumber));
		ReleaseSemaphore(&tfu->tfu_Lock);

		if(image_file_handle_lock != ZERO && unit_file_lock != ZERO)
		{
			ASSERT( NOT found_identical_disk );

			found_identical_disk = (SameLock(image_file_handle_lock, unit_file_lock) == LOCK_SAME);

			UnLock(unit_file_lock);
		}

		D(("found identical disk = %s", found_identical_disk ? "yes" : "no"));
	}

	if(found_identical_disk)
	{
		SHOWMSG("this file is already in use by a different unit or is a different file with identical contents");

		result = TFERROR_DuplicateDisk;
		goto out;
	}

	SHOWMSG("file is not currently in use");

	UnLock(image_file_handle_lock);
	image_file_handle_lock = ZERO;

	#if defined(ENABLE_CACHE)
	{
		if(change_unit_cache)
		{
			if(enable_unit_cache)
				D(("enabling cache for unit #%ld", which_tfu->tfu_UnitNumber));
			else
				D(("disabling cache for unit #%ld", which_tfu->tfu_UnitNumber));

			/* On second thought, let's see if we can enable the cache
			 * in the first place. As of this writing there is no good
			 * solution for high density 3.5" disks.
			 */
			if(drive_type == DRIVE3_5_150RPM)
			{
				SHOWMSG("disabling cache since it doesn't make good sense yet");

				enable_unit_cache = FALSE;
			}

			which_tfu->tfu_CacheEnabled = enable_unit_cache;

			which_tfu->tfu_CacheAccesses	= 0;
			which_tfu->tfu_CacheMisses		= 0;
		}

		D(("prefill unit #%ld cache = %s", which_tfu->tfu_UnitNumber, prefill_unit_cache ? "TRUE" : "FALSE"));

		which_tfu->tfu_PrefillCache = prefill_unit_cache;
	}
	#endif /* ENABLE_CACHE */

	/* Ask the unit to use the new medium. */
	result = send_unit_control_command(which_tfu, TFC_Insert, image_file_handle, fib->fib_Size, write_protected, -1);
	if(result != OK)
	{
		D(("that didnt't work (error=%ld)", result));
		goto out;
	}

	/* If this succeeded, then the file is now in the
	 * hands of the unit Process.
	 */
	file = ZERO;

	SHOWMSG("that went well");

	result = OK;

 out:

	free_aligned_memory(tfd, &track_memory);

	if(file != ZERO)
		Close(file);

	if(image_file_handle_lock != ZERO)
		UnLock(image_file_handle_lock);

	SHOWMSG("releasing device lock");
	ReleaseSemaphore(&tfd->tfd_Lock);

	RETURN(result);
	return(result);
}

/***********************************************************************/

/****** trackfile.device/TFEjectMediaTagList *********************************
*
*   NAME
*	TFEjectMediaTagList - Attempt to remove a disk image file from
*	     currently active unit, "ejecting" it.
*
*   SYNOPSIS
*	error = TFEjectMediaTagList(which_unit, tags)
*	  D0                           D0        A0
*
*	LONG TFEjectMediaTagList(LONG which_unit,
*	                         CONST struct TagItem *tags);
*
*	LONG TFEjectMediaMediaTags(LONG which_unit, ...);
*
*   FUNCTION
*	When a disk image file is no longer useful, memory can be saved
*	by ejecting it from the trackfile.device it is currently attached
*	to. This may involve writing back currently buffered data to the
*	volume on which the disk image file resides, so that it may be
*	removed safely.
*
*   INPUTS
*	which_unit -- Which unit to eject the disk image file from.
*	    Unit numbers must be >= 0.
*
*	tags -- Pointer to a list of TagItems; this may be NULL.
*
*   TAGS
*	TF_Timeout (LONG) -- How many seconds to wait for the file
*	    system to "settle" down before an attempt is made to eject the disk.
*	    This must be 5 seconds or more. If fewer than 5 seconds are
*	    requested, an attempt will be made to eject the disk immediately;
*	    this attempt may fail because the disk is still being used. The
*	    timeout defaults to 0 seconds, which means that no time is spent
*	    waiting for the disk image file to "settle down".
*
*	    If you asked for a timeout, and the disk image file has not yet been
*	    ejected because it is busy, you can hit [Ctrl+C] to stop waiting and
*	    cancel the attempt to eject the disk.
*
*   RESULT
*	error - Zero if successful, otherwise an error code is returned.
*
*   ERRORS
*	TFERROR_Denied -- Only a Process may call the
*	    TFEjectMediaTagList() function, never a Task.
*
*	TFERROR_UnitNotFound -- The unit you requested is either not
*	    known or not currently active.
*
*	ERROR_BREAK -- You hit [Ctrl+C] before the timeout had elapsed,
*	    waiting for the disk image file to settle down.
*
*	TDERR_SeekError -- The disk image file size or the file's
*	    properties have changed since the last time it was accessed. It may
*	    have been corrupted or truncated. Check with dos.library/IoErr()
*	    for more information.
*
*	TDERR_WriteProt -- The disk image file used to be writable, but
*	    now something happened which prevents buffered data from getting
*	    written back to it. Check with dos.library/IoErr() for more
*	    information.
*
*	TDERR_DiskChanged -- The disk image file has changed, or the
*	    volume on which it is stored is no longer in the same state as it
*	    used to be. Check with dos.library/IoErr() for more information.
*
*   SEE ALSO
*	<exec/errors.h>, <devices/trackdisk.h>, <devices/trackfile.h>,
*	<dos/dos.h> TFInsertMediaTagList(), TFStartUnitTagList(),
*	TFGetUnitData()
******************************************************************************
*
*/

LONG ASM
tf_eject_media_taglist(
	REG(d0, LONG which_unit),
	REG(a0, struct TagItem * tags),
	REG(a6, struct TrackFileDevice *tfd))
{
	USE_EXEC(tfd);
	USE_DOS(tfd);
	USE_UTILITY(tfd);

	BOOL device_is_locked = FALSE;
	struct TrackFileUnit * tfu;
	LONG timeout;
	LONG result;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	ASSERT( FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS );

	/* Paranoia? */
	if(FindTask(NULL)->tc_Node.ln_Type != NT_PROCESS)
	{
		SHOWMSG("this function cannot be called safely by a Task, it needs a Process");

		result = TFERROR_Denied;
		goto out;
	}

	SHOWVALUE(which_unit);
	SHOWPOINTER(tags);

	SHOWMSG("obtaining device lock");
	ObtainSemaphore(&tfd->tfd_Lock);
	device_is_locked = TRUE;

	/* Let's see which unit was requested. */
	tfu = find_unit_by_number(tfd, which_unit);
	if(tfu == NULL)
	{
		D(("didn't find unit %ld", which_unit));

		result = TFERROR_UnitNotFound;
		goto out;
	}

	SHOWMSG("telling unit process to eject the medium");

	/* The file system may take its time to flush its
	 * track buffer to disk, which can take several
	 * seconds (3 for the FFS). If you try to eject
	 * the medium while the file system is still busy,
	 * then the command will immediately fail. But
	 * if you retry the attempt, then it may succeed.
	 */
	timeout = GetTagData(TF_Timeout, 0, tags);
	if(timeout >= 5)
	{
		int times_waited = 0;
		BOOL wait = FALSE;

		SHOWVALUE(timeout);

		SHOWMSG("releasing device lock");
		ReleaseSemaphore(&tfd->tfd_Lock);
		device_is_locked = FALSE;

		/* Assume that failure lies ahead. */
		result = TDERR_DriveInUse;

		/* Each loop will wait for about 1/2 second. */
		while(times_waited < timeout * 2)
		{
			/* We delay a bit for every loop iteration
			 * except for the very first one.
			 */
			if(wait)
			{
				/* Give the client a chance to stop
				 * this retry/delay.
				 */
				if(CheckSignal(SIGBREAKF_CTRL_C))
				{
					result = ERROR_BREAK;
					break;
				}

				Delay(TICKS_PER_SECOND / 2);
				times_waited++;
			}

			wait = TRUE;

			/* If we can eject the medium right now
			 * then the job's done.
			 */
			result = send_unit_control_command(tfu, TFC_Eject, ZERO, 0, FALSE, -1);
			if(result == OK)
				break;

			/* If the drive is busy, we wait for another
			 * 1/2 second and try again. Otherwise we quit
			 * with an error condition set.
			 */
			if(result != TDERR_DriveInUse)
				break;
		}
	}
	/* No retry/delay will be used. */
	else
	{
		result = send_unit_control_command(tfu, TFC_Eject, ZERO, 0, FALSE, -1);
	}

	if(result != OK)
	{
		D(("that didnt't work (error=%ld)", result));
		goto out;
	}

	SHOWMSG("that went well");

	result = OK;

 out:

	if(device_is_locked)
	{
		SHOWMSG("releasing device lock");
		ReleaseSemaphore(&tfd->tfd_Lock);
	}

	RETURN(result);
	return(result);
}

/***********************************************************************/

/****** trackfile.device/TFGetUnitData ***************************************
*
*   NAME
*	TFGetUnitData - Obtain information about the current state of a
*	    trackfile.device unit.
*
*   SYNOPSIS
*	data = TFGetUnitData(which_unit)
*	 D0                      D0
*
*	struct TrackFileUnitData * TFGetUnitData(LONG which_unit)
*
*   FUNCTION
*	You can retrieve information about the current state of each
*	trackfile.device unit which may be suitable for display in a form
*	comparable to the AmigaDOS "Info" shell command.
*
*	Information includes whether or not a disk image file is currently
*	used by the unit, the name of that disk image file, which AmigaDOS
*	file system device name the unit uses (if any), which checksum the
*	disk image file has, and whether this is a bootable disk.
*
*	You can retrieve information for individual unit as well as ask
*	for the complete list of currently active units.
*
*	The data structure returned by TFGetUnitData() must be released
*	with TFFreeUnitData() when it is no longer needed.
*
*   INPUTS
*	which_unit -- Which unit to retrieve information for; this should
*	    be a number >= 0. Alternatively, you can retrieve information
*	    for all currently active unit by using TFGUD_AllUnits instead.
*
*   RESULT
*	data - Pointer to a "struct TrackFileUnitData" data structure list
*	    or NULL. If NULL is returned, check with dos.library/IoErr()
*	    for more information. When no longer needed, release the
*	    memory allocated with the TFFreeUnitData() function.
*
*   NOTES
*	TFGetUnitData() will return a "snapshot" of the current state of
*	the respective units. Each such snapshot represents an
*	independent, complete copy of the unit state information and will
*	need to be released when no longer needed. More active units will
*	require more memory to store the snapshot.
*
*   SEE ALSO
*	<devices/trackdisk.h>, <devices/trackfile.h>, TFFreeUnitData()
*
******************************************************************************
*
*/

struct TrackFileUnitData * ASM
tf_get_unit_data(REG(d0, LONG which_unit), REG(a6, struct TrackFileDevice *tfd))
{
	USE_EXEC(tfd);
	USE_DOS(tfd);

	struct Process * this_process = (struct Process *)FindTask(NULL);
	APTR old_window_ptr = NULL;
	struct TrackFileDevice * TrackFileBase = tfd;
	struct TrackFileUnitData * result = NULL;
	struct TrackFileUnitData * tfud;
	struct TrackFileUnitData * first_tfud = NULL;
	struct TrackFileUnitData * previous_tfud = NULL;
	struct TrackFileUnit * which_tfu;
	TEXT path_name[MAX_PATH_SIZE];
	struct DosList * dol;
	LONG error = OK;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	ASSERT( FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS );

	SHOWVALUE(which_unit);

	/* Paranoia? */
	if(this_process->pr_Task.tc_Node.ln_Type != NT_PROCESS)
	{
		/* This is not a Process... */
		this_process = NULL;

		SHOWMSG("this function cannot be called safely by a Task, it needs a Process");
		goto out;
	}

	/* Make sure that NameFromFH() will not put up an AmigaDOS
	 * requester window in case there's trouble.
	 */
	old_window_ptr = this_process->pr_WindowPtr;

	this_process->pr_WindowPtr = (APTR)-1;

	/* Collect information on all the currently
	 * known units. We allocate a record for each
	 * we find and then (for now) just remember
	 * which unit number was involved. The remaining
	 * information will be filled in later.
	 */
	if(which_unit < 0)
	{
		struct TrackFileUnit * tfu;
		LONG num_units = 0;

		SHOWMSG("obtaining device lock");
		ObtainSemaphore(&tfd->tfd_Lock);

		SHOWMSG("collecting information on all units");

		for(tfu = (struct TrackFileUnit *)tfd->tfd_UnitList.mlh_Head ;
		    tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ != NULL && error == OK;
		    tfu = (struct TrackFileUnit *)tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ)
		{
			tfud = AllocVec(sizeof(*tfud), MEMF_ANY|MEMF_PUBLIC|MEMF_CLEAR);
			if(tfud == NULL)
			{
				SHOWMSG("not enough memory");

				error = ERROR_NO_FREE_STORE;
				break;
			}

			/* Build a singly-linked list the complicated way. */
			if(first_tfud == NULL)
				first_tfud = tfud;

			if(previous_tfud != NULL)
				previous_tfud->tfud_Next = tfud;

			previous_tfud = tfud;

			tfud->tfud_DriveType = -1;
			tfud->tfud_UnitNumber = tfu->tfu_UnitNumber;

			num_units++;
		}

		SHOWMSG("releasing device lock");
		ReleaseSemaphore(&tfd->tfd_Lock);

		if(error != OK)
			goto out;

		D(("total of %ld unit(s) found", num_units));
	}
	/* Otherwise, collect information for just a single unit. */
	else
	{
		which_tfu = find_unit_by_number(tfd, which_unit);
		if(which_tfu == NULL)
		{
			D(("didn't find unit %ld", which_unit));

			error = TFERROR_UnitNotFound;
			goto out;
		}

		tfud = AllocVec(sizeof(*tfud), MEMF_ANY|MEMF_PUBLIC|MEMF_CLEAR);
		if(tfud == NULL)
		{
			SHOWMSG("not enough memory");

			error = ERROR_NO_FREE_STORE;
			goto out;
		}

		first_tfud = tfud;

		tfud->tfud_DriveType = -1;
		tfud->tfud_UnitNumber = which_tfu->tfu_UnitNumber;
	}

	/* Now fill each record with information, if possible. The
	 * plan is to hold onto the device lock, and the respective
	 * unit lock, for as briefly as possible.
	 */
	for(tfud = first_tfud ;
	    tfud != NULL ;
	    tfud = tfud->tfud_Next)
	{
		/* Which unit are we interested in? Let's hope it's
		 * still around.
		 */
		which_tfu = find_unit_by_number(tfd, tfud->tfud_UnitNumber);
		if(which_tfu == NULL)
		{
			/* Try the next unit if we cannot find this one
			 * any more. Note that this should never happen
			 * because units are never removed once they
			 * have been created.
			 */
			D(("unit %ld went away", tfud->tfud_UnitNumber));
			continue;
		}

		/* Grab the unit lock, so that the file and process
		 * information will not change while we're looking
		 * at them.
		 */
		D(("obtaining unit %ld lock", which_tfu->tfu_UnitNumber));
		ObtainSemaphore(&which_tfu->tfu_Lock);

		/* Update the disk checksum if necessary. */
		update_disk_checksum(which_tfu);

		tfud->tfud_Size				= sizeof(*tfud);
		tfud->tfud_DriveType		= which_tfu->tfu_DriveType;
		tfud->tfud_IsActive			= unit_is_active(which_tfu);
		tfud->tfud_MediumIsPresent	= unit_medium_is_present(which_tfu);
		tfud->tfud_IsBusy			= unit_medium_is_busy(which_tfu);
		tfud->tfud_IsWritable		= NOT which_tfu->tfu_WriteProtected;
		tfud->tfud_ChecksumsEnabled	= (BOOL)(which_tfu->tfu_DiskChecksumTable != NULL);

		#if defined(ENABLE_CACHE)
		{
			tfud->tfud_CacheEnabled		= which_tfu->tfu_CacheEnabled;
			tfud->tfud_CacheAccesses	= which_tfu->tfu_CacheAccesses;
			tfud->tfud_CacheMisses		= which_tfu->tfu_CacheMisses;
		}
		#endif /* ENABLE_CACHE */

		ASSERT( sizeof(tfud->tfud_Checksum) == sizeof(which_tfu->tfu_DiskChecksum) );

		CopyMem(&which_tfu->tfu_DiskChecksum, &tfud->tfud_Checksum, sizeof(tfud->tfud_Checksum));

		/* Try to get the name of the file, if possible. */
		if(unit_medium_is_present(which_tfu))
		{
			D(("trying to get the name of the file used by unit %ld", tfud->tfud_UnitNumber));

			/* Obtaining the full name of the file may not work if the
			 * underlying file system does not support the packet type
			 * required.
			 */
			if(NameFromFH(which_tfu->tfu_File, path_name, sizeof(path_name)))
			{
				size_t len = strlen(path_name);

				tfud->tfud_FileName = AllocVec(len+1, MEMF_ANY|MEMF_PUBLIC);
				if(tfud->tfud_FileName != NULL)
				{
					D(("and the name is \"%s\"", path_name));
					strcpy(tfud->tfud_FileName, path_name);
				}
				else
				{
					SHOWMSG("out of memory");
				}
			}
			else
			{
				D(("couldn't get it; error=%ld", IoErr()));
			}
		}

		/* Fill in what we know about the disk's volume name, its
		 * creation time and date.
		 */
		tfud->tfud_VolumeValid = which_tfu->tfu_RootDirValid;
		if(tfud->tfud_VolumeValid)
		{
			ASSERT( sizeof(which_tfu->tfu_RootDirName) == sizeof(tfud->tfud_VolumeName) );

			CopyMem(which_tfu->tfu_RootDirName, tfud->tfud_VolumeName, sizeof(tfud->tfud_VolumeName));

			CopyMem(&which_tfu->tfu_RootDirDate, &tfud->tfud_VolumeDate, sizeof(tfud->tfud_VolumeDate));
		}

		/* And this is what we know about the reserved blocks. */
		tfud->tfud_FileSysSignature		= which_tfu->tfu_FileSystemSignature;
		tfud->tfud_BootBlockChecksum	= which_tfu->tfu_BootBlockChecksum;

		D(("releasing unit %ld lock", which_tfu->tfu_UnitNumber));
		ReleaseSemaphore(&which_tfu->tfu_Lock);

		D(("trying to get the device name used by unit %ld", tfud->tfud_UnitNumber));

		/* Let's see if the AmigaDOS device name for this unit is known. */
		for(dol = NextDosEntry(LockDosList(LDF_DEVICES|LDF_READ), LDF_DEVICES) ;
		    dol != NULL ;
		    dol = NextDosEntry(dol, LDF_DEVICES))
		{
			struct fs_startup_msg fsm;

			if(decode_file_sys_startup_msg(SysBase, dol->dol_misc.dol_handler.dol_Startup, &fsm))
			{
				if(fsm.fsm_device_unit == tfud->tfud_UnitNumber &&
					strcmp(fsm.fsm_device_name, tfd->tfd_Device.dd_Library.lib_Node.ln_Name) == SAME)
				{
					const TEXT * device_name = BADDR(dol->dol_Name);
					int device_name_len = device_name[0];

					tfud->tfud_DeviceName = AllocVec(device_name_len+1, MEMF_ANY|MEMF_PUBLIC);
					if(tfud->tfud_DeviceName != NULL)
					{
						CopyMem(&device_name[1], tfud->tfud_DeviceName, device_name_len);
						tfud->tfud_DeviceName[device_name_len] = '\0';

						D(("and the name is \"%s\"", tfud->tfud_DeviceName));
					}
					else
					{
						SHOWMSG("out of memory");
					}

					break;
				}
			}
		}

		UnLockDosList(LDF_DEVICES|LDF_READ);
	}

	SHOWMSG("that went well");

	result = first_tfud;
	first_tfud = NULL;

 out:

	/* Free the unit data list, if neccessary. */
	if(first_tfud != NULL)
		TFFreeUnitData(first_tfud);

	if(this_process != NULL)
	{
		this_process->pr_WindowPtr = old_window_ptr;

		if(error != OK)
			SetIoErr(error);
	}

	RETURN(result);
	return(result);
}

/***********************************************************************/

/****** trackfile.device/TFFreeUnitData **************************************
*
*   NAME
*	TFFreeUnitData - Free the copy of the unit information allocated
*	    by the TFGetUnitData function().
*
*   SYNOPSIS
*	TFFreeUnitData(data)
*	                A0
*
*	VOID TFFreeUnitData(struct TrackFileUnitData * data);
*
*   FUNCTION
*	The data returned by the TFGetUnitData() function needs to be
*	freed when no longer needed. Each "snaphot" of the current unit
*	state takes up memory which should be released as soon as
*	possible.
*
*   INPUTS
*	data -- Pointer to the "struct TrackFileUnitData" list as returned
*	    by the TFGetUnitData() function. This may be NULL.
*
*   SEE ALSO
*	<devices/trackdisk.h>, <devices/trackfile.h>, TFGetUnitData()
*
******************************************************************************
*
*/

VOID ASM
tf_free_unit_data(REG(a0, struct TrackFileUnitData * first_tfud), REG(a6, struct TrackFileDevice *tfd))
{
	USE_EXEC(tfd);

	struct TrackFileUnitData * next_tfud;
	struct TrackFileUnitData * tfud;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	for(tfud = first_tfud ;
	    tfud != NULL ;
	    tfud = next_tfud)
	{
		next_tfud = tfud->tfud_Next;

		if(tfud->tfud_FileName != NULL)
			FreeVec(tfud->tfud_FileName);

		if(tfud->tfud_DeviceName != NULL)
			FreeVec(tfud->tfud_DeviceName);

		FreeVec(tfud);
	}

	LEAVE();
}

/***********************************************************************/

/****** trackfile.device/TFChangeUnitTagList *********************************
*
*   NAME
*	TFChangeUnitTagList - Change the mode of operation for a
*	    trackfile.device unit or for all currently active units.
*
*   SYNOPSIS
*	error = TFChangeUnitTagList(which_unit, tags)
*	 D0                             D0       A0
*
*	LONG TFChangeUnitTagList(LONG which_unit,
*	                         const struct TagItem *tags);
*
*	LONG TFChangeUnitTags(LONG which_unit, ...);
*
*   FUNCTION
*	How a unit was set up when it was started and when a disk image
*	file was loaded may not be useful for long. For example, a
*	write-protected disk should really be writable, or caching for a
*	unit should be enabled. TFChangeUnitTagList() provides the means
*	to change these parameters, and more.
*
*   INPUTS
*	which_unit -- Which specific unit to change, which must be a
*	    number >= 0. Alternatively, you can change the global
*	    configuration for all units by using TFUNIT_CONTROL.
*
*	tags -- Pointer to a list of TagItems; this may be NULL.
*
*   TAGS
*	TF_TagItemFailed (struct TagItem **) -- If one of the changes you
*	    wanted to make is not supported or the change attempt has
*	    failed, you can find out which TagItem specifically caused a
*	    problem. Its address will be placed in the pointer you
*	    placed in the TF_TagItemFailed item's ti_Data member.
*
*	TF_WriteProtected (BOOL) -- You may change whether or not the disk
*	    image file which is attached to this unit should be
*	    write-enabled or write-protected. This involves, if
*	    necessarily, "ejecting" the disk, changing the protection and
*	    re-inserting it again. Please note that asking for the medium
*	    to be write-enabled may not be possible if the volume on which
*	    it resides is write-protected.
*
*	TF_MaxCacheMemory (ULONG) -- How much memory may be used for the
*	    shared unit cache can be configured here. This is a setting
*	    which affects all units and therefore requires that you
*	    specify TFUNIT_CONTROL as the unit number.
*
*	    The amount of cache memory is given as the maximum number of bytes
*	    to allocate and manage for it. A value of 0 will disable the
*	    cache.
*
*	TF_EnableUnitCache (BOOL) -- Whether or not a unit should make use
*	    of the shared unit cache can controlled for each unit
*	    individually. Please note that if the maximum amount of memory
*	    the shared unit cache may use is set to 0 then no unit will be
*	    able to use the shared cache and enabling it will have no
*	    effect.
*
*   RESULT
*	error - Zero if successful, otherwise an error code is returned.
*
*   NOTES
*	TFChangeUnitTagList() will stop processing the TagItem list as
*	soon as an error is detected. Some of the TagItems may get
*	processed before further processing is aborted.
*
*   SEE ALSO
*	<devices/trackdisk.h>, <devices/trackfile.h>
*
******************************************************************************
*
*/

LONG ASM
tf_change_unit_taglist(
	REG(d0, LONG which_unit),
	REG(a0, struct TagItem * tags),
	REG(a6, struct TrackFileDevice *tfd))
{
	USE_EXEC(tfd);
	USE_UTILITY(tfd);

	struct TrackFileUnit * tfu;
	LONG result;
	struct TagItem ** tag_item_failed;
	struct TagItem * list = tags;
	struct TagItem * ti;
	BOOL is_write_protected;
	BOOL enable_cache;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	SHOWVALUE(which_unit);
	SHOWPOINTER(tags);

	/* We may need this later to indicate which tag item
	 * operation failed. It's possible that one day multiple
	 * changes may have to be performed on this unit and
	 * without a hint as to which one failed, you won't know
	 * what you could do differently to make them all
	 * succeed.
	 */
	ti = FindTagItem(TF_TagItemFailed, tags);
	if(ti != NULL)
		tag_item_failed = (struct TagItem **)ti->ti_Data;
	else
		tag_item_failed = NULL;

	/* Make sure that we only provide a TagItem pointer
	 * when we have to.
	 */
	if(tag_item_failed != NULL)
		(*tag_item_failed) = NULL;

	SHOWMSG("obtaining device lock");
	ObtainSemaphore(&tfd->tfd_Lock);

	/* Let's see which unit was requested. */
	if(which_unit != TFUNIT_CONTROL)
	{
		tfu = find_unit_by_number(tfd, which_unit);
		if(tfu == NULL)
		{
			D(("didn't find unit %ld", which_unit));

			result = TFERROR_UnitNotFound;
			goto out;
		}
	}
	else
	{
		tfu = NULL;
	}

	/* What are we going to change? */
	while((ti = NextTagItem(&list)) != NULL)
	{
		switch(ti->ti_Tag)
		{
			/* Change the write protection of the medium without
			 * ejecting the medium first?
			 */
			case TF_WriteProtected:

				D(("TF_WriteProtected=%s", ti->ti_Data ? "TRUE" : "FALSE"));

				/* The control unit does not support this operation. */
				if(which_unit == TFUNIT_CONTROL)
				{
					SHOWMSG("the control unit does not support this operation");

					if(tag_item_failed != NULL)
						(*tag_item_failed) = ti;

					result = TFERROR_NotSupported;
					goto out;
				}

				is_write_protected = (BOOL)(ti->ti_Data != 0);

				ASSERT( tfu != NULL );

				result = send_unit_control_command(tfu, TFC_ChangeWriteProtection, ZERO, 0, is_write_protected, -1);
				if(result != OK)
				{
					D(("that didn't work (error=%ld)", result));

					if(tag_item_failed != NULL)
						(*tag_item_failed) = ti;

					goto out;
				}

				break;

		#if defined(ENABLE_CACHE)

			/* Change how much memory the shared cache may use? */
			case TF_MaxCacheMemory:

				D(("TF_MaxCacheMemory=%lu", ti->ti_Data));

				/* Only the control unit supports this operation. */
				if(which_unit != TFUNIT_CONTROL)
				{
					SHOWMSG("only the control unit supports this operation");

					if(tag_item_failed != NULL)
						(*tag_item_failed) = ti;

					result = TFERROR_NotSupported;
					goto out;
				}

				if(tfd->tfd_CacheContext != NULL)
					change_cache_size(tfd->tfd_CacheContext, ti->ti_Data);

				break;

			/* Change whether the unit cache is enabled? */
			case TF_EnableUnitCache:

				D(("TF_EnableUnitCache=%s", ti->ti_Data ? "TRUE" : "FALSE"));

				/* The control unit does not support this operation. */
				if(which_unit == TFUNIT_CONTROL)
				{
					SHOWMSG("the control unit does not support this operation");

					if(tag_item_failed != NULL)
						(*tag_item_failed) = ti;

					result = TFERROR_NotSupported;
					goto out;
				}

				enable_cache = (BOOL)(ti->ti_Data != FALSE);

				ASSERT( tfu != NULL );

				result = send_unit_control_command(tfu, TFC_ChangeEnableCache, ZERO, 0, FALSE, enable_cache);
				if(result != OK)
				{
					D(("that didn't work (error=%ld)", result));

					if(tag_item_failed != NULL)
						(*tag_item_failed) = ti;

					goto out;
				}

				break;

		#endif /* ENABLE_CACHE */

			default:

				break;
		}
	}

	SHOWMSG("that went well");

	result = OK;

 out:

	SHOWMSG("releasing device lock");
	ReleaseSemaphore(&tfd->tfd_Lock);

	RETURN(result);
	return(result);
}

/***********************************************************************/

/****** trackfile.device/TFExamineFileSize ***********************************
*
*   NAME
*	TFExamineFileSize - Return the trackdisk.device drive type
*	    associated with the size of an Amiga disk image file.
*
*   SYNOPSIS
*	type = TFExamineFileSize(file_size)
*	 D0                          D0
*
*	LONG TFExamineFileSize(LONG file_size);
*
*   FUNCTION
*	Check if a disk image file represents a supported 3.5" or 5.25"
*	Amiga floppy disk by checking its size.
*
*   INPUTS
*	file_size - Size of the Amiga disk image file in bytes.
*
*   RESULT
*	type - If the file size is matches one of the supported Amiga
*	    floppy disk format DRIVE3_5 (3.5" 80 track double density
*	    disk), DRIVE5_25 (5.25" 40 track disk) or DRIVE3_5_150RPM
*	    (3.5" 80 track high density disk) will be returned. Otherwise
*	    -1 will be returned.
*
*   NOTES
*	5.25" floppy disk images are not currently supported.
*
*   SEE ALSO
*	<devices/trackdisk.h>
*
******************************************************************************
*
*/

/* Check if a disk image file is either likely for a 3.5" DD disk,
 * or a 3.5" HD disk. We don't do 40 track 5.25" disks yet. Returns
 * the drive type associated with an image file size or -1 if the
 * size does not match anything useful.
 */
LONG ASM
tf_examine_file_size(
	REG(d0, LONG						file_size),
	REG(a6, struct TrackFileDevice *	tfd))
{
	static const struct { LONG drive_type; LONG size; } file_sizes[] =
	{
		{ DRIVE3_5, 		NUMCYLS * NUMHEADS * NUMSECS     * TD_SECTOR },
		{ DRIVE3_5_150RPM,	NUMCYLS * NUMHEADS * (2*NUMSECS) * TD_SECTOR },

		{ -1, -1 },
	};

	USE_EXEC(tfd);

	LONG result = TFEFS_Unsupported;
	int i;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	SHOWVALUE(file_size);

	for(i = 0 ; file_sizes[i].drive_type != -1 ; i++)
	{
		if(file_size == file_sizes[i].size)
		{
			result = file_sizes[i].drive_type;
			break;
		}
	}

	RETURN(result);
	return(result);
}
