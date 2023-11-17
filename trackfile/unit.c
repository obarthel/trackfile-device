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

#include <devices/trackfile.h>

/****************************************************************************/

#ifndef _TRACKFILE_DEVICE_H
#include "trackfile_device.h"
#endif /* _TRACKFILE_DEVICE_H */

/****************************************************************************/

#include "assert.h"

/****************************************************************************/

#include "unit.h"
#include "tools.h"
#include "commands.h"
#include "cache.h"

/****************************************************************************/

/* Start the unit "turn-off" timer which ticks about every 2.5 seconds. */
static VOID
start_timer(struct TrackFileUnit * tfu)
{
	struct TrackFileDevice * tfd = tfu->tfu_Device;

	USE_EXEC(tfd);

	/* ENTER(); */

	ASSERT( tfu != NULL );
	ASSERT( tfu->tfu_TimeRequest.tr_node.io_Message.mn_Node.ln_Type != NT_MESSAGE );

	tfu->tfu_TimeRequest.tr_node.io_Command	= TR_ADDREQUEST;
	tfu->tfu_TimeRequest.tr_time.tv_secs	= 2;
	tfu->tfu_TimeRequest.tr_time.tv_micro	= MILLION / 2;

	SendIO((struct IORequest *)&tfu->tfu_TimeRequest);

	/* LEAVE(); */
}

/****************************************************************************/

/* This is the process which handles all the I/O requests for a
 * unit which cannot be processed immediately in the device
 * BeginIO() function. It also receives control commands, such
 * as for inserting/ejecting storage media.
 */
VOID
UnitProcessEntry(VOID)
{
	struct Library * SysBase = AbsExecBase;
	struct Library * DOSBase;

	struct Process * this_process;
	struct TrackFileUnit * tfu;
	struct TrackFileDevice * tfd;
	struct Message * unit_start_message;
	ULONG io_mask;
	ULONG control_mask;
	ULONG time_mask;
	ULONG signals_received;
	ULONG signal_mask;
	struct IORequest * io;
	struct TrackFileControlMsg * tfcm;
	struct TrackFileControlMsg * exit_tfcm = NULL;
	struct FileHandle * fh;
	LONG track_data_size;
	BOOL unit_is_locked = FALSE;
	LONG error;

	/* Wait for the startup message to arrive. We will
	 * return it when the unit Process is fully operational,
	 * or if the unit Process initialization has failed.
	 */
	this_process = (struct Process *)FindTask(NULL);

	WaitPort(&this_process->pr_MsgPort);

	unit_start_message = GetMsg(&this_process->pr_MsgPort);

	ASSERT( unit_start_message != NULL );

	/* The unit data structure arrives through the
	 * message name pointer.
	 */
	tfu = (struct TrackFileUnit *)unit_start_message->mn_Node.ln_Name;

	/* And from this we initialize the related device data structure
	 * as well as the dos.library base address.
	 */
	tfd = tfu->tfu_Device;

	DOSBase = tfd->tfd_DOSBase;

	D(("--- process for unit #%ld is starting up (%s) ---", tfu->tfu_UnitNumber, this_process->pr_Task.tc_Node.ln_Name));

	/* Make sure that the unit process can receive messages
	 * from these MsgPorts.
	 */
	init_msgport(&tfu->tfu_Unit.tdu_Unit.unit_MsgPort, (struct Task *)this_process, SIGBREAKB_CTRL_D);
	init_msgport(&tfu->tfu_ControlPort, (struct Task *)this_process, SIGBREAKB_CTRL_E);
	init_msgport(&tfu->tfu_TimePort, (struct Task *)this_process, SIGBREAKB_CTRL_F);

	/* Also set up the time request for the unit. */
	tfu->tfu_TimeRequest.tr_node.io_Message.mn_Node.ln_Type	= NT_REPLYMSG;
	tfu->tfu_TimeRequest.tr_node.io_Message.mn_Length		= sizeof(tfu->tfu_TimeRequest);
	tfu->tfu_TimeRequest.tr_node.io_Message.mn_ReplyPort	= &tfu->tfu_TimePort;

	SHOWMSG("opening timer.device");

	error = OpenDevice(TIMERNAME, UNIT_VBLANK, (struct IORequest *)&tfu->tfu_TimeRequest, 0);
	if(error != OK)
	{
		D(("that didn't work (error=%ld)", error));
		goto out;
	}

	SHOWMSG("returning the start message");

	/* Indicate successful startup by filling in the
	 * unit Process pointer.
	 */
	tfu->tfu_Process = this_process;

	/* Return the startup message, and make sure that
	 * this is done only once.
	 */
	ReplyMsg(unit_start_message);
	unit_start_message = NULL;

	/* Get ready to receive IORequests and other interesting signals. */
	io_mask			= (1UL << tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_SigBit);
	control_mask	= (1UL << tfu->tfu_ControlPort.mp_SigBit);
	time_mask		= (1UL << tfu->tfu_TimePort.mp_SigBit);

	signal_mask = io_mask | control_mask | time_mask;

	start_timer(tfu);

	ASSERT( signal_mask != 0 );

	/* And wait for something interesting to happen. */
	signals_received = 0;

	do
	{
		/* If there are no signals pending, wait for
		 * a signal to arrive. Otherwise, poll the
		 * currently active signals and process
		 * them.
		 */
		if(signals_received == 0)
		{
			/* D(("process for unit %ld is waiting for something to do...", tfu->tfu_UnitNumber)); */

			signals_received = Wait(signal_mask);

			/* SHOWMSG("got something to do at last"); */
		}
		/* Just update the signals which are currently pending. */
		else
		{
			signals_received |= SetSignal(0, signal_mask) & signal_mask;
		}

		/* Process an I/O request? */
		if(FLAG_IS_SET(signals_received, io_mask))
		{
			/* Is this unit still processing commands? */
			if(NOT tfu->tfu_Stopped)
			{
				/* Is there another IORequest in the queue? */
				io = (struct IORequest *)GetMsg(&tfu->tfu_Unit.tdu_Unit.unit_MsgPort);
				if(io != NULL)
				{
					/* We are now busy. */
					SET_FLAG(tfu->tfu_Unit.tdu_Unit.unit_flags, UNITF_INTASK);

					D(("BEGIN: unit #%ld performs this command (io=0x%08lx)", tfu->tfu_UnitNumber, io));

					perform_io((struct IOStdReq *)io);

					D(("END: unit #%ld performs this command (io=0x%08lx)", tfu->tfu_UnitNumber, io));

					/* Careful, the TD_ADDCHANGEINT I/O request will
					 * be kept in a list if the command has succeeded.
					 * Hence we cannot reply it.
					 */
					if(io->io_Command != TD_ADDCHANGEINT || io->io_Error != OK)
					{
						D(("unit #%ld replying io=0x%08lx", tfu->tfu_UnitNumber, io));

						ReplyMsg(&io->io_Message);
					}
				}
				/* No, we may have to wait for another one to arrive. */
				else
				{
					/* We are no longer busy. */
					CLEAR_FLAG(tfu->tfu_Unit.tdu_Unit.unit_flags, UNITF_INTASK);

					CLEAR_FLAG(signals_received, io_mask);
				}
			}
			/* Keep the I/O request in the queue and do not
			 * act upon it.
			 */
			else
			{
				D(("unit #%ld is stopped and won't process the queued I/O request just yet", tfu->tfu_UnitNumber));

				/* We are no longer busy. */
				CLEAR_FLAG(tfu->tfu_Unit.tdu_Unit.unit_flags, UNITF_INTASK);

				/* Ignore that I/O request. */
				CLEAR_FLAG(signals_received, io_mask);
			}
		}

		/* Do periodic maintenance and cleanup work? */
		if(FLAG_IS_SET(signals_received, time_mask))
		{
			/* SHOWMSG("time to do maintenance and cleanup work"); */

			/* Restart the timer first. */
			WaitIO((struct IORequest *)&tfu->tfu_TimeRequest);
			start_timer(tfu);

			/* Should we write back any changes made to the
			 * track buffer and turn off the motor?
			 */
			if(tfu->tfu_TurnMotorOff)
			{
				/* We only do this if the unit is not currently
				 * busy. There may be more commands to come.
				 */
				if(FLAG_IS_CLEAR(tfu->tfu_Unit.tdu_Unit.unit_flags, UNITF_INTASK))
				{
					SHOWMSG("unit is not currently busy");

					/* Write back any changes made to the
					 * track buffer?
					 */
					if(tfu->tfu_TrackDataChanged)
					{
						SHOWMSG("changes were made to the track buffer; writing it back");

						error = write_back_track_data(tfu);
						if(error != OK)
							D(("writing back the track buffer failed (error=%ld)", error));
					}
					else
					{
						SHOWMSG("no track buffer changes had to be written back");
					}

					SHOWMSG("turning off the motor");

					turn_off_motor(tfu);
				}

				tfu->tfu_TurnMotorOff = FALSE;
			}
			else
			{
				/* SHOWMSG("no cleanup work necessary"); */
			}

			CLEAR_FLAG(signals_received, time_mask);
		}

		/* Process a control message? */
		if(FLAG_IS_SET(signals_received, control_mask))
		{
			tfcm = (struct TrackFileControlMsg *)GetMsg(&tfu->tfu_ControlPort);
			if(tfcm != NULL)
			{
				tfcm->tfcm_Error = OK;

				switch(tfcm->tfcm_Type)
				{
					/* Shut down? */
					case TFC_Stop:

						D(("TFC_Stop: process for unit %ld needs to quit", tfu->tfu_UnitNumber));

						/* We can only quit if the file has been
						 * ejected already.
						 */
						if(unit_medium_is_present(tfu))
						{
							/* We can't quit yet. */
							tfcm->tfcm_Error = ERROR_OBJECT_IN_USE;

							D(("process for unit %ld cannot quit just yet", tfu->tfu_UnitNumber));

							break;
						}

						D(("obtaining unit %ld lock", tfu->tfu_UnitNumber));
						ObtainSemaphore(&tfu->tfu_Lock);
						unit_is_locked = TRUE;

						tfu->tfu_Process = NULL;
						tfu->tfu_Stopped = FALSE;

						free_aligned_memory(tfd, &tfu->tfu_TrackMemory);

						tfu->tfu_TrackData = NULL;

						#if defined(ENABLE_MFM_ENCODING)
						{
							free_mfm_code_context(SysBase, tfu->tfu_MFMCodeContext);
							tfu->tfu_MFMCodeContext = NULL;
						}
						#endif /* ENABLE_MFM_ENCODING */

						tfu->tfu_TrackDataSize = 0;

						D(("process for unit %ld will now quit", tfu->tfu_UnitNumber));

						/* We will reply this message shortly
						 * before this process exits.
						 */
						exit_tfcm = tfcm;
						tfcm = NULL;

						break;

					/* Medium should be inserted? */
					case TFC_Insert:

						D(("TFC_Insert: process for unit %ld needs to perform a medium insertion", tfu->tfu_UnitNumber));

						/* Is there already a medium present? */
						if(unit_medium_is_present(tfu))
						{
							tfcm->tfcm_Error = TFERROR_AlreadyInUse;

							SHOWMSG("there is still a medium present which needs to be ejected first");
							break;
						}

						/* Just make sure that the file is still there. */
						if(tfcm->tfcm_File == ZERO)
						{
							tfcm->tfcm_Error = TFERROR_NoFileGiven;

							SHOWMSG("no file handle was provided");
							break;
						}

						D(("file size = %ld", tfcm->tfcm_FileSize));

						track_data_size = tfcm->tfcm_FileSize / tfu->tfu_NumTracks;

						D(("track size = %ld bytes", track_data_size));

						fh = (struct FileHandle *)BADDR(tfcm->tfcm_File);

						/* Has the track buffer size changed, or the file system
						 * which used to be responsible for the last file?
						 */
						if(tfu->tfu_TrackDataSize != track_data_size ||
						   tfu->tfu_TrackFileSystem != fh->fh_Type)
						{
							struct AlignedMemoryAllocation new_track_memory;
							APTR new_mfm_code_context;

							if(tfu->tfu_TrackDataSize > 0)
								D(("track size has changed from %ld -> %ld bytes", tfu->tfu_TrackDataSize, track_data_size));
							else
								D(("track size is %ld bytes", track_data_size));

							#if defined(ENABLE_MFM_ENCODING)
							{
								new_mfm_code_context = create_mfm_code_context(SysBase, track_data_size / TD_SECTOR);
								if(new_mfm_code_context == NULL)
								{
									SHOWMSG("out of memory");

									tfcm->tfcm_Error = TFERROR_OutOfMemory;
									break;
								}
							}
							#endif /* ENABLE_MFM_ENCODING */

							if(allocate_aligned_memory(tfd, fh->fh_Type, track_data_size, &new_track_memory) != OK)
							{
								SHOWMSG("out of memory");

								#if defined(ENABLE_MFM_ENCODING)
								{
									free_mfm_code_context(SysBase, new_mfm_code_context);
								}
								#endif /* ENABLE_MFM_ENCODING */

								tfcm->tfcm_Error = TFERROR_OutOfMemory;
								break;
							}

							free_aligned_memory(tfd, &tfu->tfu_TrackMemory);

							tfu->tfu_TrackMemory = new_track_memory;

							tfu->tfu_TrackData = tfu->tfu_TrackMemory.ama_Aligned;

							tfu->tfu_TrackDataSize = track_data_size;

							tfu->tfu_TrackFileSystem = fh->fh_Type;

							#if defined(ENABLE_MFM_ENCODING)
							{
								free_mfm_code_context(SysBase, tfu->tfu_MFMCodeContext);
								tfu->tfu_MFMCodeContext = new_mfm_code_context;

								/* This may become useful later. Note that the
								 * pseudo-random-number generator initial state
								 * must not be zero or otherwise the generator
								 * will get stuck returning an infinite sequence
								 * of zeroes...
								 */
								tfu->tfu_PRNGState = 1 | (((ULONG)tfu->tfu_MFMCodeContext) ^ (((ULONG)this_process) >> 1));

								ASSERT( tfu->tfu_PRNGState != 0 );
							}
							#endif /* ENABLE_MFM_ENCODING */
						}

						D(("obtaining unit %ld lock", tfu->tfu_UnitNumber));
						ObtainSemaphore(&tfu->tfu_Lock);

						/* The drive type follows the size of the image file. */
						Forbid();

						if(track_data_size == (2 * NUMSECS) * TD_SECTOR)
							tfu->tfu_DriveType = DRIVE3_5_150RPM;
						else
							tfu->tfu_DriveType = DRIVE3_5;

						Permit();

						if (tfu->tfu_DriveType == DRIVE3_5)
							SHOWMSG("drive type = DRIVE3_5");
						else if (tfu->tfu_DriveType == DRIVE3_5_150RPM)
							SHOWMSG("drive type = DRIVE3_5_150RPM");
						else
							SHOWMSG("drive type = DRIVE5_25");

						tfu->tfu_WriteProtected	= tfcm->tfcm_WriteProtected;
						tfu->tfu_File			= tfcm->tfcm_File;
						tfu->tfu_FileSize		= tfcm->tfcm_FileSize;

						/* Change the file access mode to reflect
						 * if write access is permitted. Note that
						 * MODE_READWRITE just indicates the intention
						 * to maybe write to the file. It does not
						 * imply exclusive access to the file.
						 *
						 * We need to stick with shared access to the
						 * file because otherwise DupLockFromFH() would
						 * fail, and that would keep us from checking
						 * if the user tried to mount the same file
						 * twice.
						 */
						if(NOT tfu->tfu_WriteProtected)
						{
							SHOWMSG("changing file access mode");
							ChangeMode(CHANGE_FH, tfu->tfu_File, MODE_READWRITE);
							SHOWMSG("done.");
						}

						D(("releasing unit %ld lock", tfu->tfu_UnitNumber));
						ReleaseSemaphore(&tfu->tfu_Lock);

						/* Reply the control message before we trigger
						 * the change notification.
						 */
						ReplyMsg(&tfcm->tfcm_Message);
						tfcm = NULL;

						/* Make no assumptsion about the current file position. */
						tfu->tfu_FilePosition = -1;

						/* Prefill the cache for this unit by reading the
						 * entire disk image file first?
						 */
						#if defined(ENABLE_CACHE)
						{
							/* For now this only works for image files of
							 * 80 track double density disks.
							 */
							SHOWPOINTER(tfd->tfd_CacheContext);
							SHOWVALUE(tfu->tfu_CacheEnabled);
							SHOWVALUE(tfu->tfu_DriveType);

							if(tfu->tfu_PrefillCache && tfd->tfd_CacheContext->cc_MaxCacheSize < tfu->tfu_FileSize)
							{
								D(("cache cannot hold enough data (%ld bytes) for a complete prefill of unit #%ld (%ld bytes)",
									tfd->tfd_CacheContext->cc_MaxCacheSize, tfu->tfu_UnitNumber, tfu->tfu_FileSize));

								tfu->tfu_PrefillCache = FALSE;
							}

							SHOWVALUE(tfu->tfu_PrefillCache);

							if(tfd->tfd_CacheContext != NULL &&
							   tfu->tfu_CacheEnabled &&
							   tfu->tfu_DriveType != DRIVE3_5_150RPM &&
							   tfu->tfu_PrefillCache &&
							   tfu->tfu_FileSize >= tfd->tfd_CacheContext->cc_MaxCacheSize)
							{
								D(("filling the cache for unit #%ld", tfu->tfu_UnitNumber));

								tfu->tfu_FilePosition = -1;

								/* Start from the top... */
								if(Seek(tfu->tfu_File, 0, OFFSET_BEGINNING) != -1)
								{
									LONG num_track_bytes_read;
									LONG which_track;

									/* Read each track, then store it in the cache. */
									for(which_track = 0 ; which_track < tfu->tfu_NumTracks ; which_track++)
									{
										num_track_bytes_read = Read(tfu->tfu_File, tfu->tfu_TrackData, tfu->tfu_TrackDataSize);
										if(num_track_bytes_read == -1 || num_track_bytes_read < tfu->tfu_TrackDataSize)
											break;

										update_cache_contents(tfd->tfd_CacheContext,
											tfu, which_track,
											tfu->tfu_TrackData,
											tfu->tfu_TrackDataSize,
											UDN_Allocate);
									}
								}
							}
							else
							{
								D(("won't fill the cache for unit #%ld", tfu->tfu_UnitNumber));
							}

							tfu->tfu_PrefillCache = FALSE;
						}
						#endif /* ENABLE_CACHE */

						trigger_change(tfu);

						D(("process for unit %ld has performed a medium insertion", tfu->tfu_UnitNumber));

						break;

					/* Medium should be removed? */
					case TFC_Eject:

						if(unit_medium_is_present(tfu))
						{
							D(("TFC_Eject: process for unit %ld needs to perform a medium removal", tfu->tfu_UnitNumber));

							if(unit_medium_is_busy(tfu))
							{
								if(tfu->tfu_TrackDataChanged)
									SHOWMSG("changes have not been written back to medium");

								if(tfu->tfu_MotorEnabled)
									SHOWMSG("motor is still turned on");

								tfcm->tfcm_Error = TDERR_DriveInUse;
								break;
							}

							tfcm->tfcm_Error = eject_image_file(tfu);
							if(tfcm->tfcm_Error != OK)
							{
								D(("medium ejection failed, error=%ld", tfcm->tfcm_Error));
								break;
							}

							/* If the cache is enabled, drop all the cache entries
							 * previously used by this disk image file. The entries
							 * will be reused later.
							 */
							#if defined(ENABLE_CACHE)
							{
								if(tfd->tfd_CacheContext != NULL)
									invalidate_cache_entries_for_unit(tfd->tfd_CacheContext, tfu);
							}
							#endif /* ENABLE_CACHE */

							/* Reply the control message before we trigger
							 * the change notification.
							 */
							ReplyMsg(&tfcm->tfcm_Message);
							tfcm = NULL;

							trigger_change(tfu);
						}
						else
						{
							D(("TFC_Eject: process for unit %ld needs to perform a medium removal, but there is no medium present", tfu->tfu_UnitNumber));
						}

						break;

					/* Change whether the medium is write-protected or not? */
					case TFC_ChangeWriteProtection:

						D(("TFC_ChangeWriteProtection: unit %ld needs changing the medium write protection to %s (is %s now)",
							tfu->tfu_UnitNumber,
							tfcm->tfcm_WriteProtected ? "write-protected" : "write-enabled",
							tfu->tfu_WriteProtected ? "write-protected" : "write-enabled"
						));

						/* Do we need to do anything at all? */
						if(tfu->tfu_WriteProtected == (tfcm->tfcm_WriteProtected != FALSE))
						{
							SHOWMSG("no action necessary");
							break;
						}

						/* Without a disk image file there's no sense in
						 * trying to change anything.
						 */
						if(NOT unit_medium_is_present(tfu))
						{
							D(("unit %ld currently has no medium inserted", tfu->tfu_UnitNumber));

							tfcm->tfcm_Error = TFERROR_NoMediumPresent;
							break;
						}

						/* Does the medium still has unfinished business associated with it? */
						if(unit_medium_is_busy(tfu))
						{
							if(tfu->tfu_TrackDataChanged)
								SHOWMSG("changes have not been written back to medium");

							if(tfu->tfu_MotorEnabled)
								SHOWMSG("motor is still turned on");

							tfcm->tfcm_Error = TDERR_DriveInUse;
							break;
						}

						/* Check if we can actually remove the write protection,
						 * since the volume on which the disk image file resides
						 * may not be write-enabled, or the disk image file itself
						 * may not be write-enabled either.
						 */
						if(NOT tfcm->tfcm_WriteProtected)
						{
							D_S(struct InfoData, disk_info);
							D_S(struct FileInfoBlock, fib);
							BPTR parent_dir;

							/* Let's have a look at the volume on which
							 * the file resides.
							 *
							 * Note that ParentOfFH() may fail because the
							 * file system does not support the packet type
							 * underlying its functionality.
							 */
							parent_dir = ParentOfFH(tfu->tfu_File);
							if(parent_dir != ZERO)
							{
								if(Info(parent_dir, disk_info))
								{
									UnLock(parent_dir);

									/* So, is it write-enabled or not? */
									if(disk_info->id_DiskState != ID_VALIDATED)
									{
										SHOWMSG("file's volume is not write-enabled");

										tfcm->tfcm_Error = TFERROR_ReadOnlyVolume;
										break;
									}

									/* Now check if the file itself is write-enabled. */
									if(ExamineFH(tfu->tfu_File, fib))
									{
										if(FLAG_IS_SET(fib->fib_Protection, FIBF_WRITE))
										{
											SHOWMSG("file is write-protected");

											tfcm->tfcm_Error = TFERROR_ReadOnlyFile;
										}
									}
									else
									{
										D(("could not get examine file (error=%ld)", IoErr()));
									}
								}
								else
								{
									D(("could not get file's disk information (error=%ld)", IoErr()));
									UnLock(parent_dir);
								}
							}
							else
							{
								D(("could not get file's parent directory lock (error=%ld)", IoErr()));
							}
						}

						/* Make the change. */
						tfu->tfu_WriteProtected = (tfcm->tfcm_WriteProtected != FALSE);

						break;

				#if defined(ENABLE_CACHE)

					/* Change whether the unit uses the cache or not? */
					case TFC_ChangeEnableCache:

						D(("TFC_ChangeEnableCache: unit %ld needs to have caching %s (is %s now)",
							tfu->tfu_UnitNumber,
							(tfcm->tfcm_Value != FALSE) ? "enabled" : "disabled",
							tfu->tfu_CacheEnabled ? "enabled" : "disabled"
						));

						if(tfu->tfu_CacheEnabled == (tfcm->tfcm_Value != FALSE))
						{
							SHOWMSG("no action necessary");
							break;
						}

						tfu->tfu_CacheEnabled = (tfcm->tfcm_Value != FALSE);

						if(NOT tfu->tfu_CacheEnabled && tfd->tfd_CacheContext != NULL)
						{
							D(("cache is disabled for unit %ld; also invalidating the unit cache", tfu->tfu_UnitNumber));
							invalidate_cache_entries_for_unit(tfd->tfd_CacheContext, tfu);

							tfu->tfu_CacheAccesses	= 0;
							tfu->tfu_CacheMisses	= 0;
						}

						break;

				#endif /* ENABLE_CACHE */

					default:

						D(("reject unknown action %ld", tfcm->tfcm_Type));

						tfcm->tfcm_Error = ERROR_ACTION_NOT_KNOWN;
						break;
				}

				if(tfcm != NULL)
					ReplyMsg(&tfcm->tfcm_Message);
			}
			else
			{
				CLEAR_FLAG(signals_received, control_mask);
			}
		}

		/* This loop stops as soon as the unit shuts down.
		 * The shutdown is triggered by a control message
		 * of type TFC_Stop.
		 */
	}
	while(exit_tfcm == NULL);

	D(("unit %ld process is winding down...", tfu->tfu_UnitNumber));

 out:

	/* Wrap up the timer.device use. */
	if(tfu->tfu_TimeRequest.tr_node.io_Device != NULL)
	{
		SHOWMSG("shutting down the timer use");

		/* Stop the ticking timer, if necessary. */
		if(CheckIO((struct IORequest *)&tfu->tfu_TimeRequest) == BUSY)
			AbortIO((struct IORequest *)&tfu->tfu_TimeRequest);

		WaitIO((struct IORequest *)&tfu->tfu_TimeRequest);

		CloseDevice((struct IORequest *)&tfu->tfu_TimeRequest);

		SHOWMSG("timer shut down.");
	}

	/* Note: We drop into Disable() and not into Forbid()
	 *       because BeginIO() is sort of permitted to
	 *       be called from interrupt code. This means
	 *       our cleanup operation below, which discards
	 *       any I/O requests that may have arrived
	 *       since we started shutting down, must make
	 *       sure that we don't miss anything.
	 */
	Disable();

	if(unit_is_locked)
	{
		D(("releasing unit %ld lock", tfu->tfu_UnitNumber));
		ReleaseSemaphore(&tfu->tfu_Lock);
	}

	/* Throw all the inbound IO requests away which
	 * have accumulated since we entered Disable()
	 * state.
	 */
	SHOWMSG("bouncing all pending I/O requests");

	ASSERT( NOT unit_medium_is_present(tfu) );

	while((io = (struct IORequest *)GetMsg(&tfu->tfu_Unit.tdu_Unit.unit_MsgPort)) != NULL)
	{
		D(("   .oOo.oOo.oOo. 0x%08lx...", io));

		io->io_Error = IOERR_ABORTED;

		ReplyMsg(&io->io_Message);
	}

	SHOWMSG("bouncing all pending control requests");

	while((tfcm = (struct TrackFileControlMsg *)GetMsg(&tfu->tfu_ControlPort)) != NULL)
	{
		D(("   .oOo.oOo.oOo. 0x%08lx...", tfcm));

		tfcm->tfcm_Error = TFERROR_Aborted;

		ReplyMsg(&tfcm->tfcm_Message);
	}

	/* Return the message which triggered the unit
	 * Process shutdown, if necessary.
	 */
	if(exit_tfcm != NULL)
	{
		SHOWMSG("returning exit message");

		ReplyMsg(&exit_tfcm->tfcm_Message);
	}

	/* Return the start message in case the setup went wrong... */
	if(unit_start_message != NULL)
	{
		SHOWMSG("unit setup failed; returning the start message");

		ReplyMsg(unit_start_message);
	}

	D(("--- process for unit #%ld is really shutting down now ---", tfu->tfu_UnitNumber));
}

/****************************************************************************/

/* Send a control command to a specific unit, such as for inserting
 * or ejecting a storage medium, or for the unit to shut down.
 */
LONG
send_unit_control_command(
	struct TrackFileUnit *	tfu,
	LONG					type,
	BPTR					file,
	LONG					file_size,
	BOOL					write_protected,
	LONG					value)
{
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	struct TrackFileControlMsg tfcm;
	struct MsgPort mp;

	USE_EXEC(tfd);

	ASSERT( tfu != NULL && tfd != NULL );

	/* We'll build the reply port locally. Don't do this at home, kids! */
	memset(&mp, 0, sizeof(mp));

	init_msgport(&mp, FindTask(NULL), SIGB_SINGLE);

	/* Now fill in the control command's message. */
	memset(&tfcm, 0, sizeof(tfcm));

	tfcm.tfcm_Message.mn_ReplyPort	= &mp;
	tfcm.tfcm_Message.mn_Length		= sizeof(tfcm);
	tfcm.tfcm_Type					= type;
	tfcm.tfcm_Error					= OK;
	tfcm.tfcm_File					= file;
	tfcm.tfcm_FileSize				= file_size;
	tfcm.tfcm_WriteProtected		= write_protected;
	tfcm.tfcm_Value					= value;

	Forbid();

	/* Is the unit still active? */
	if(tfu->tfu_Process != NULL)
	{
		D(("sending control message to unit #%ld", tfu->tfu_UnitNumber));

		/* Important: clear SIGF_SINGLE before we
		 *            eventually drop into WaitPort().
		 */
		SetSignal(0, (1UL << mp.mp_SigBit));
		PutMsg(&tfu->tfu_ControlPort, &tfcm.tfcm_Message);
		WaitPort(&mp);

		D(("control message for unit #%ld has been returned", tfu->tfu_UnitNumber));
	}
	/* So the unit is no longer active. */
	else
	{
		tfcm.tfcm_Error = TFERROR_UnitNotActive;
	}

	Permit();

	return(tfcm.tfcm_Error);
}

/****************************************************************************/

/* Given the unit number, try to find its corresponding unit
 * data structure on the device's global list. Returns the requested
 * unit data structure or otherwise NULL if not found.
 */
struct TrackFileUnit *
find_unit_by_number(struct TrackFileDevice * tfd, LONG unit_number)
{
	struct TrackFileUnit * result = NULL;
	struct TrackFileUnit * tfu;

	USE_EXEC(tfd);

	ENTER();

	ASSERT( tfd != NULL );

	D(("trying to find unit %ld", unit_number));

	SHOWMSG("obtaining device lock");
	ObtainSemaphore(&tfd->tfd_Lock);

	for(tfu = (struct TrackFileUnit *)tfd->tfd_UnitList.mlh_Head ;
	    tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ != NULL ;
	    tfu = (struct TrackFileUnit *)tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ)
	{
		if(unit_number == tfu->tfu_UnitNumber)
		{
			/* Move this unit to the head of the list, so that the
			 * most frequently-used units will be retrieved
			 * fastest.
			 */
			if(tfu != (struct TrackFileUnit *)tfd->tfd_UnitList.mlh_Head)
			{
				D(("shuffling unit #%ld to the head of the list", tfu->tfu_UnitNumber));

				Remove(&tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node);
				AddHeadMinList(&tfd->tfd_UnitList, &tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node);
			}

			result = tfu;
			break;
		}
	}

	SHOWMSG("releasing device lock");
	ReleaseSemaphore(&tfd->tfd_Lock);

	RETURN(result);
	return(result);
}

/****************************************************************************/

/* Prepare for removal of the disk image file by flushing the track
 * buffer to the file, then closing the file. The drive motor will
 * be turned off and the contents of the track buffer will be
 * invalidated.
 */
LONG
eject_image_file(struct TrackFileUnit * tfu)
{
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	BPTR file;

	USE_EXEC(tfd);
	USE_DOS(tfd);

	LONG error;

	ENTER();

	ASSERT( FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS );

	D(("obtaining unit %ld lock", tfu->tfu_UnitNumber));
	ObtainSemaphore(&tfu->tfu_Lock);

	/* If necessary, write back any data that may still
	 * be in the track buffer.
	 */
	if(tfu->tfu_TrackDataChanged)
	{
		SHOWMSG("write back track data");

		error = write_back_track_data(tfu);
		if(error != OK)
		{
			D(("writing the track data back failed, error=%ld", error));
			goto out;
		}
	}

	/* We change the file handle under Forbid() so that
	 * the immediate device commands which reference it
	 * can look at it without having to grab the unit
	 * lock first.
	 */
	Forbid();

	file = tfu->tfu_File;
	tfu->tfu_File = ZERO;

	Permit();

	/* Close the file? We check the file handle again
	 * because an error may have caused the file to
	 * be closed already.
	 */
	if(file != ZERO)
	{
		SHOWMSG("closing the file");

		/* If the file was changed, we may want the file system
		 * to write back any buffered changes to disk first.
		 */
		if(tfu->tfu_ChangesMade)
		{
			struct FileHandle * fh = (struct FileHandle *)BADDR(file);

			/* Careful about NIL: and other surprises. */
			if(fh->fh_Type != NULL)
			{
				SHOWMSG("ask the file system to flush its buffers");

				DoPkt(fh->fh_Type, ACTION_FLUSH,	0, 0, 0, 0, 0);

				SHOWMSG("file system should have flushed its buffers.");
			}
		}

		Close(file);

		SHOWMSG("file is closed now.");
	}

	mark_track_buffer_as_invalid(tfu);
	turn_off_motor(tfu);

	/* Any changes made to the unit file have been
	 * accounted for now.
	 */
	tfu->tfu_ChangesMade = FALSE;

	error = OK;

 out:

	D(("releasing unit %ld lock", tfu->tfu_UnitNumber));
	ReleaseSemaphore(&tfu->tfu_Lock);

	RETURN(error);
	return(error);
}

/****************************************************************************/

/* Update the medium change counter, then notify all registered clients
 * that the medium was removed or inserted.
 */
VOID
trigger_change(struct TrackFileUnit * tfu)
{
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	struct IOStdReq * io;

	USE_EXEC(tfd);

	ENTER();

	ASSERT( tfu != NULL && tfd != NULL );

	Forbid();

	D(("change is coming for unit %ld (%ld -> %ld)",
		tfu->tfu_UnitNumber,
		tfu->tfu_Unit.tdu_Counter,
		tfu->tfu_Unit.tdu_Counter+1));

	tfu->tfu_Unit.tdu_Counter++;

	/* Trigger the legacy disk change interrupt, if necessary. */
	if(tfu->tfu_RemoveInt != NULL)
	{
		D(("trigger TD_REMOVE interrupt 0x%08lx", tfu->tfu_RemoveInt));

		Cause(tfu->tfu_RemoveInt);

		SHOWMSG("seems to have survived that...");
	}

	/* In spite of what the name of the list says, it contains
	 * IOStdReq nodes. And these in turn contain the respective
	 * Interrupt structure pointer in the io_Data field.
	 *
	 * We trigger each disk change interrupt in turn.
	 */
	for(io = (struct IOStdReq *)tfu->tfu_ChangeIntList.mlh_Head ;
	    io->io_Message.mn_Node.ln_Succ != NULL ;
	    io = (struct IOStdReq *)io->io_Message.mn_Node.ln_Succ)
	{
		D(("trigger TD_ADDCHANGEINT interrupt 0x%08lx (io=0x%08lx)", io->io_Data, io));

		ASSERT( io->io_Data != NULL );

		Cause(io->io_Data);

		SHOWMSG("seems to have survived that...");
	}

	Permit();

	LEAVE();
}

/****************************************************************************/

/* Check if a unit is active, that is, if it currently has a
 * running process attached. This check can make good sense only
 * if the unit is currently locked for inspection.
 */
BOOL
unit_is_active(struct TrackFileUnit * tfu)
{
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	BOOL is_active;

	USE_EXEC(tfd);

	ASSERT( tfu != NULL );

	D(("obtaining unit %ld lock", tfu->tfu_UnitNumber));
	ObtainSemaphore(&tfu->tfu_Lock);

	is_active = (tfu->tfu_Process != NULL);

	D(("releasing unit %ld lock", tfu->tfu_UnitNumber));
	ReleaseSemaphore(&tfu->tfu_Lock);

	return(is_active);
}

/****************************************************************************/

/* Check if the unit currently has a medium present. This is
 * good enough for a quick test to see if the unit could possibly
 * report information on the disk, but if you need to access the
 * unit's track buffer or its image file, then you need to lock
 * the unit for inspection first.
 */
BOOL
unit_medium_is_present(struct TrackFileUnit * tfu)
{
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	BOOL is_present;

	USE_EXEC(tfd);

	ASSERT( tfu != NULL );

	D(("obtaining unit %ld lock", tfu->tfu_UnitNumber));
	ObtainSemaphore(&tfu->tfu_Lock);

	is_present = (tfu->tfu_File != ZERO);

	D(("releasing unit %ld lock", tfu->tfu_UnitNumber));
	ReleaseSemaphore(&tfu->tfu_Lock);

	return(is_present);
}

/****************************************************************************/

/* Check if the unit is currently busy, which means that the
 * motor is currently running.
 */
BOOL
unit_medium_is_busy(struct TrackFileUnit * tfu)
{
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	BOOL is_busy;

	USE_EXEC(tfd);

	ASSERT( tfu != NULL );

	D(("obtaining unit %ld lock", tfu->tfu_UnitNumber));
	ObtainSemaphore(&tfu->tfu_Lock);

	is_busy = tfu->tfu_MotorEnabled;

	D(("releasing unit %ld lock", tfu->tfu_UnitNumber));
	ReleaseSemaphore(&tfu->tfu_Lock);

	return(is_busy);
}
