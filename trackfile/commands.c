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

#ifndef _TRACKFILE_DEVICE_H
#include "trackfile_device.h"
#endif /* _TRACKFILE_DEVICE_H */

/****************************************************************************/

#include "assert.h"

/****************************************************************************/

#include "commands.h"
#include "tools.h"
#include "unit.h"
#include "mfm_encoding.h"
#include "cache.h"

/****************************************************************************/

/* Check if the IOStdReq.io_Offset field is suitable for reading or
 * writing, with regard to the size of the disk. We follow the rules
 * of the trackdisk.device here, which insists that any position must
 * be a multiple of the sector size (512 bytes).
 */
static LONG
check_offset(const struct IOStdReq * io)
{
	const struct TrackFileUnit * tfu;

	LONG error = IOERR_BADLENGTH;

	ASSERT( io != NULL );

	ASSERT( (io->io_Offset % TD_SECTOR) == 0 );

	if((io->io_Offset % TD_SECTOR) != 0)
	{
		D(("offset %ld is not a multiple of the sector size (%ld bytes)", io->io_Offset, TD_SECTOR));
		goto out;
	}

	tfu = (struct TrackFileUnit *)io->io_Unit;

	ASSERT( tfu != NULL );

	/* Note that it's sort of OK for the position to be
	 * right beyond the last byte of the disk. This is
	 * why we test for "greater than".
	 */
	ASSERT( io->io_Offset <= (ULONG)tfu->tfu_FileSize );

	if(io->io_Offset > (ULONG)tfu->tfu_FileSize)
	{
		D(("offset %ld lies beyond the last data bytes (%ld)", io->io_Offset, tfu->tfu_FileSize));
		goto out;
	}

	error = OK;

 out:

	return(error);
}

/****************************************************************************/

/* If this is an extended command (ETD_CLEAR, ETD_FORMAT, ETD_MOTOR,
 * ETD_RAWREAD, ETD_RAWWRITE, ETD_READ, ETD_SEEK, ETD_UPDATE, or ETD_WRITE)
 * verify that the maximum permitted change counter for the command
 * has not been exceeded.
 *
 * We also check if the I/O request has the required minimum size
 * suitable for an extended command.
 */
static LONG
check_extended_command(const struct IOStdReq * io)
{
	LONG error;

	if(FLAG_IS_SET(io->io_Command, TDF_EXTCOM))
	{
		const struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
		const struct IOExtTD * iotd = (struct IOExtTD *)io;
		ULONG counter;

		/* This should be at least a 'struct IOExtTD', or
		 * otherwise the iotd_Count and iotd_SecLabel
		 * fields are unavailable.
		 */

		ASSERT( io->io_Message.mn_Length >= sizeof(*iotd) );

		if(io->io_Message.mn_Length < sizeof(*iotd))
		{
			D(("I/O request is too short: %lu (should have been at least %lu)",
				io->io_Message.mn_Length, sizeof(*iotd)));

			error = IOERR_BADLENGTH;
			goto out;
		}

		/* Is this I/O request already stale? The command can
		 * only be executed if the disk change counter matches
		 * the drive's media change counter, or exceeds it.
		 *
		 * The counter is updated under Forbid() conditions,
		 * so we can safely read it here without using
		 * Forbid()/Permit() guards.
		 */
		counter = tfu->tfu_Unit.tdu_Counter;
		if(iotd->iotd_Count < counter)
		{
			error = TDERR_DiskChanged;
			goto out;
		}
	}

	error = OK;

 out:

	return(error);
}

/****************************************************************************/

/* Most commands involve the use of the io_Data, io_Length, io_Offset
 * and io_Actual fields only present in the 'struct IOStdReq' and
 * absent from the shorter 'struct IORequest'. We make sure that the
 * size of the request matches the requirements.
 */
static LONG
check_io_request_size(const struct IOStdReq * io)
{
	LONG error;

	ASSERT( io != NULL );

	/* This should be at least a 'struct IOStdReq', or
	 * otherwise the io_Length, io_Actual and io_Data
	 * fields are unavailable.
	 */

	/* Note: The v40 ffs will have io->io_Message.mn_Length=0 for all
	 *       quick and non-quick command, which is more than just
	 *       unfortunate...
	 */
	ASSERT( io->io_Message.mn_Length == 0 || io->io_Message.mn_Length >= sizeof(*io) );

	if(io->io_Message.mn_Length != 0 && io->io_Message.mn_Length < sizeof(*io))
	{
		D(("I/O request is too short: %lu (should have been at least %lu)",
			io->io_Message.mn_Length, sizeof(*io)));

		error = IOERR_BADLENGTH;
		goto out;
	}

	error = OK;

 out:

	return(error);
}

/****************************************************************************/

/* For those commands which use the io_Data and io_Length fields
 * there are requirements for the values stored therein. It could be
 * a minimum length, for example, a certain length alignment or
 * an address alignment. We always check for a NULL io_Data field
 * and optionally (if the alignment and length requirements are
 * not zero, respectively) the io_Length and io_Data values.
 */
static LONG
check_io_request_data_and_length(
	const struct IOStdReq *	io,
	ULONG					length_minimum,
	ULONG					length_alignment,
	ULONG					data_alignment)
{
	LONG error;

	ASSERT( io != NULL );

	/* Are the minimum length requirements satisfied? */
	ASSERT( length_minimum == 0 || io->io_Length >= length_minimum );

	if(length_minimum > 0 && io->io_Length < length_minimum)
	{
		D(("io_Length (%lu) < minimum length (%lu)", io->io_Length, length_minimum));

		error = IOERR_BADLENGTH;
		goto out;
	}

	/* Are the length alignment requirements satisfied? */
	ASSERT( length_alignment == 0 || (io->io_Length % length_alignment) == 0 );

	if(length_alignment > 0 && (io->io_Length % length_alignment) != 0)
	{
		D(("io_Length (%lu) is not a multiple of %lu", io->io_Length, length_alignment));

		error = IOERR_BADLENGTH;
		goto out;
	}

	/* Is the data address just wrong? */
	ASSERT( io->io_Data != NULL );
	if(io->io_Data == NULL)
	{
		SHOWMSG("io_Data is NULL");

		error = IOERR_BADADDRESS;
		goto out;
	}

	/* Are the data address alignment requirements satisfied? */
	ASSERT( data_alignment == 0 || (((ULONG)io->io_Data) % data_alignment) == 0 );

	if(data_alignment > 0 && (((ULONG)io->io_Data) % data_alignment) != 0)
	{
		D(("io_Data address (0x%08lx) is not aligned to %lu bytes", io->io_Data, data_alignment));

		error = IOERR_BADADDRESS;
		goto out;
	}

	error = OK;

 out:

	return(error);
}

/****************************************************************************/

/* Close the disk image file associated with this unit,
 * unless it has already been closed. This will briefly
 * hold the unit lock. This function is only called
 * in emergency situations in which the file must be
 * considered unusable now. As a side-effect, the
 * track buffer contents are discarded.
 */
static VOID
close_unit_file(struct TrackFileUnit * tfu)
{
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	BPTR file;

	USE_EXEC(tfd);
	USE_DOS(tfd);

	ENTER();

	ASSERT( FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS );

	D(("obtaining unit %ld lock", tfu->tfu_UnitNumber));
	ObtainSemaphore(&tfu->tfu_Lock);

	Forbid();

	file = tfu->tfu_File;
	tfu->tfu_File = ZERO;

	Permit();

	if(file != ZERO)
		Close(file);

	mark_track_buffer_as_invalid(tfu);

	tfu->tfu_ChangesMade = FALSE;

	D(("releasing unit %ld lock", tfu->tfu_UnitNumber));
	ReleaseSemaphore(&tfu->tfu_Lock);

	LEAVE();
}

/****************************************************************************/

/* Read a complete track into the unit's track buffer, replacing
 * its contents. If necessary, the current track buffer contents
 * may have to be written back to the file first.
 */
static LONG
read_track_data(struct TrackFileUnit * tfu, LONG which_track)
{
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG num_track_bytes_read = 0;
	LONG new_position;
	LONG error;

	USE_EXEC(tfd);
	USE_DOS(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	if(tfu->tfu_CurrentTrackNumber != -1)
		D(("currently keeping track %ld in the buffer, need to move to track %ld", tfu->tfu_CurrentTrackNumber, which_track));
	else
		D(("moving to track %ld", which_track));

	ASSERT( NOT tfu->tfu_TrackDataChanged );
	ASSERT( tfu->tfu_File != ZERO );
	ASSERT( 0 <= which_track && which_track < tfu->tfu_NumTracks );

	ASSERT( NOT multiplication_overflows(which_track, tfu->tfu_TrackDataSize) );
	ASSERT( which_track * tfu->tfu_TrackDataSize >= 0 );
	ASSERT( which_track * tfu->tfu_TrackDataSize < tfu->tfu_FileSize );

	ASSERT( FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS );

	ASSERT( tfu->tfu_TrackDataSize > 0 );

	new_position = which_track * tfu->tfu_TrackDataSize;

	/* If the cache feature is enabled, try to find the
	 * data in the cache rather than reading it from
	 * the disk image file.
	 */
	#if defined(ENABLE_CACHE)
	{
		BOOL read_data_from_file = TRUE;
		BOOL use_cache;

		SHOWPOINTER(tfd->tfd_CacheContext);
		SHOWVALUE(tfu->tfu_CacheEnabled);
		SHOWVALUE(tfu->tfu_DriveType);

		use_cache = (BOOL)(
			tfd->tfd_CacheContext != NULL &&
			tfu->tfu_CacheEnabled &&
			tfu->tfu_DriveType != DRIVE3_5_150RPM
		);

		/* Let's see if we can find this track in the cache, however the
		 * cache must be enabled for this to work out.
		 */
		if(use_cache)
		{
			tfu->tfu_CacheAccesses++;

			if(read_cache_contents(tfd->tfd_CacheContext,
			   tfu, which_track,
			   tfu->tfu_TrackData, tfu->tfu_TrackDataSize))
			{
				/* So we got what we came for. */
				read_data_from_file = FALSE;

				num_track_bytes_read = tfu->tfu_TrackDataSize;
			}
			else
			{
				/* This is a true cache miss. */
				tfu->tfu_CacheMisses++;
			}

			#if DEBUG
			{
				ULONG access_count = tfu->tfu_CacheAccesses;
				ULONG miss_count = tfu->tfu_CacheMisses;
				ULONG hit_count = access_count - miss_count;
				ULONG hit_percent;

				hit_percent = (10000 * hit_count) / access_count;

				D(("unit #%ld cache stats: access/hit/miss=%lu/%lu/%lu, hit%=%ld.%03ld%%",
					tfu->tfu_UnitNumber,
					access_count, hit_count, miss_count,
					hit_percent / 100, hit_percent % 100
				));
			}
			#endif /* DEBUG */
		}

		/* Do we have to read the data from the file after all? */
		if(read_data_from_file)
		{
			#if DEBUG
			{
				LONG current_file_position;

				current_file_position = Seek(tfu->tfu_File, 0, OFFSET_CURRENT);

				SHOWVALUE(tfu->tfu_FilePosition);
				SHOWVALUE(current_file_position);
				SHOWVALUE(new_position);

				ASSERT( tfu->tfu_FilePosition < 0 || tfu->tfu_FilePosition == current_file_position );
			}
			#endif /* DEBUG */

			/* Move to the file position which matches the track number. */
			if(new_position != tfu->tfu_FilePosition)
			{
				if(Seek(tfu->tfu_File, new_position, OFFSET_BEGINNING) == -1)
				{
					D(("that seek didn't work (error=%ld)", IoErr()));

					/* We probably don't know where we are now... */
					tfu->tfu_FilePosition = -1;

					error = TDERR_NoSecHdr;
					goto out;
				}

				tfu->tfu_FilePosition = new_position;
			}

			/* Read the track data we came for. */
			num_track_bytes_read = Read(tfu->tfu_File, tfu->tfu_TrackData, tfu->tfu_TrackDataSize);
			if(num_track_bytes_read == tfu->tfu_TrackDataSize)
			{
				ASSERT( tfu->tfu_FilePosition >= 0 );

				tfu->tfu_FilePosition += num_track_bytes_read;

				/* Update the cache or maybe create a new cache entry. */
				if(use_cache)
				{
					update_cache_contents(tfd->tfd_CacheContext,
						tfu, which_track,
						tfu->tfu_TrackData, tfu->tfu_TrackDataSize,
						UDN_Allocate);
				}
			}
			/* That didn't work out... */
			else
			{
				/* We need to invalidate this cache entry since
				 * we don't actually know what could be read.
				 */
				if(use_cache)
					invalidate_cache_entry(tfd->tfd_CacheContext, CACHE_KEY(tfu->tfu_UnitNumber, which_track));
			}
		}
	}
	#else
	{
		#if DEBUG
		{
			LONG current_file_position;

			current_file_position = Seek(tfu->tfu_File, 0, OFFSET_CURRENT);

			SHOWVALUE(tfu->tfu_FilePosition);
			SHOWVALUE(current_file_position);
			SHOWVALUE(new_position);

			ASSERT( tfu->tfu_FilePosition < 0 || tfu->tfu_FilePosition == current_file_position );
		}
		#endif /* DEBUG */

		/* Move to the file position which matches the track number. */
		if(new_position != tfu->tfu_FilePosition)
		{
			if(Seek(tfu->tfu_File, new_position, OFFSET_BEGINNING) == -1)
			{
				D(("that seek didn't work (error=%ld)", IoErr()));

				/* We probably don't know where we are now... */
				tfu->tfu_FilePosition = -1;

				error = TDERR_NoSecHdr;
				goto out;
			}

			tfu->tfu_FilePosition = new_position;
		}

		D(("reading %ld bytes from file at position %ld, to go into track file buffer 0x%08lx",
			tfu->tfu_TrackDataSize, tfu->tfu_FilePosition, tfu->tfu_TrackData));

		/* Read the track data we came for. */
		num_track_bytes_read = Read(tfu->tfu_File, tfu->tfu_TrackData, tfu->tfu_TrackDataSize);
		if(num_track_bytes_read == tfu->tfu_TrackDataSize)
		{
			ASSERT( tfu->tfu_FilePosition >= 0 );

			tfu->tfu_FilePosition += num_track_bytes_read;
		}
	}
	#endif /* ENABLE_CACHE */

	if(num_track_bytes_read != tfu->tfu_TrackDataSize)
	{
		/* We probably don't know where we are now... */
		tfu->tfu_FilePosition = -1;

		/* The track buffer contents are no longer valid. */
		mark_track_buffer_as_invalid(tfu);

		/* Was this an actual read error? */
		if(num_track_bytes_read == -1)
		{
			error = IoErr();

			D(("that read didn't work (error=%ld)", error));

			/* Let's try and make some sense of the AmigaDOS error code.
			 * This may not be a reliable approach, though, since every
			 * file system or handler can pick its own error codes to
			 * match the situation.
			 */
			switch(error)
			{
				/* The disk has been removed. */
				case ERROR_DEVICE_NOT_MOUNTED:
				case ERROR_NO_DISK:

					SHOWMSG("disk has been removed -- closing the file");

					close_unit_file(tfu);
					turn_off_motor(tfu);

					error = TDERR_DiskChanged;
					break;

				default:

					error = TDERR_BadSecHdr;
					break;
			}
		}
		else
		{
			D(("that read didn't work: %ld bytes requested, read only %ld",
				tfu->tfu_TrackDataSize, num_track_bytes_read));

			error = TDERR_BadSecHdr;
		}

		goto out;
	}

	/* Remember the old track checksum, if necessary. */
	if(tfu->tfu_DiskChecksumTable != NULL && tfu->tfu_CurrentTrackNumber != -1)
	{
		ASSERT( 0 <= tfu->tfu_CurrentTrackNumber && tfu->tfu_CurrentTrackNumber < tfu->tfu_DiskChecksumTableLength );

		D(("OLD checksum for track %3ld = 0x%08lx%08lx",
			tfu->tfu_CurrentTrackNumber,
			tfu->tfu_TrackDataChecksum.f64c_high, tfu->tfu_TrackDataChecksum.f64c_low));

		tfu->tfu_DiskChecksumTable[tfu->tfu_CurrentTrackNumber] = tfu->tfu_TrackDataChecksum;
		tfu->tfu_ChecksumUpdated = TRUE;
	}

	/* There's new data in the buffer for a new track. */
	tfu->tfu_CurrentTrackNumber = tfu->tfu_Unit.tdu_CurrTrk = which_track;

	/* So we can verify the checksum later. */
	fletcher64_checksum(tfu->tfu_TrackData, tfu->tfu_TrackDataSize, &tfu->tfu_TrackDataChecksum);

	D(("NEW checksum for track %3ld = 0x%08lx%08lx",
		which_track,
		tfu->tfu_TrackDataChecksum.f64c_high, tfu->tfu_TrackDataChecksum.f64c_low));

	error = OK;

 out:

 	RETURN(error);
 	return(error);
}

/****************************************************************************/

/* If the track buffer has been modified, write its contents
 * back to the disk image file. This is used most prominently
 * by the CMD_UPDATE command.
 */
LONG
write_back_track_data(struct TrackFileUnit * tfu)
{
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	struct fletcher64_checksum new_track_checksum;
	LONG error;

	USE_EXEC(tfd);
	USE_DOS(tfd);

	ENTER();

	ASSERT( FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS );

	ASSERT( tfu->tfu_TrackDataChanged );
	ASSERT( NOT tfu->tfu_WriteProtected );
	ASSERT( 0 <= tfu->tfu_CurrentTrackNumber && tfu->tfu_CurrentTrackNumber < tfu->tfu_NumTracks );
	ASSERT( tfu->tfu_File != ZERO );
	ASSERT( NOT multiplication_overflows(tfu->tfu_CurrentTrackNumber, tfu->tfu_TrackDataSize) );
	ASSERT( tfu->tfu_CurrentTrackNumber * tfu->tfu_TrackDataSize >= 0 );

	D(("OLD checksum for track %3ld = 0x%08lx%08lx",
		tfu->tfu_CurrentTrackNumber,
		tfu->tfu_TrackDataChecksum.f64c_high, tfu->tfu_TrackDataChecksum.f64c_low));

	/* Let's see if the data really needs to be written back to the file. */
	fletcher64_checksum(tfu->tfu_TrackData, tfu->tfu_TrackDataSize, &new_track_checksum);

	D(("NEW checksum for track %3ld = 0x%08lx%08lx",
		tfu->tfu_CurrentTrackNumber,
		new_track_checksum.f64c_high, new_track_checksum.f64c_low));

	if(tfu->tfu_IgnoreTrackChecksum || compare_fletcher64_checksums(&tfu->tfu_TrackDataChecksum, &new_track_checksum) != SAME)
	{
		LONG new_position;

		/* Next time, do not ignore the old track checksum. */
		tfu->tfu_IgnoreTrackChecksum = FALSE;

		SHOWMSG("track contents have been changed, so we really need to write them back");

		new_position = tfu->tfu_CurrentTrackNumber * tfu->tfu_TrackDataSize;

		#if DEBUG
		{
			LONG current_file_position;

			current_file_position = Seek(tfu->tfu_File, 0, OFFSET_CURRENT);

			SHOWVALUE(tfu->tfu_FilePosition);
			SHOWVALUE(current_file_position);
			SHOWVALUE(new_position);

			ASSERT( tfu->tfu_FilePosition < 0 || tfu->tfu_FilePosition == current_file_position );
		}
		#endif /* DEBUG */

		if(new_position != tfu->tfu_FilePosition)
		{
			if(Seek(tfu->tfu_File, new_position, OFFSET_BEGINNING) == -1)
			{
				D(("that seek didn't work (error=%ld)", IoErr()));

				/* We probably don't know where we are now... */
				tfu->tfu_FilePosition = -1;

				error = TDERR_SeekError;
				goto out;
			}

			tfu->tfu_FilePosition = new_position;
		}

		ASSERT( tfu->tfu_FilePosition >= 0 );

		D(("writing to track %ld at file position %ld (%ld bytes are written from the track buffer at 0x%08lx)",
			tfu->tfu_CurrentTrackNumber, tfu->tfu_FilePosition, tfu->tfu_TrackDataSize, tfu->tfu_TrackData));

		ASSERT( tfu->tfu_TrackDataSize > 0 );

		if(Write(tfu->tfu_File, tfu->tfu_TrackData, tfu->tfu_TrackDataSize) == -1)
		{
			error = IoErr();

			/* We probably don't know where we are now... */
			tfu->tfu_FilePosition = -1;

			D(("that write didn't work (error=%ld)", error));

			/* Let's try and make some sense of the AmigaDOS error code.
			 * This may not be a reliable approach, though, since every
			 * file system or handler can pick its own error codes to
			 * match the situation.
			 */
			switch(error)
			{
				/* Disk or file is no longer writable. */
				case ERROR_DISK_NOT_VALIDATED:
				case ERROR_DISK_WRITE_PROTECTED:
				case ERROR_WRITE_PROTECTED:

					D(("obtaining unit %ld lock", tfu->tfu_UnitNumber));
					ObtainSemaphore(&tfu->tfu_Lock);

					tfu->tfu_WriteProtected = TRUE;

					D(("releasing unit %ld lock", tfu->tfu_UnitNumber));
					ReleaseSemaphore(&tfu->tfu_Lock);

					error = TDERR_WriteProt;
					break;

				/* The disk has been removed. */
				case ERROR_DEVICE_NOT_MOUNTED:
				case ERROR_NO_DISK:

					SHOWMSG("disk has been removed -- closing the file");

					close_unit_file(tfu);
					turn_off_motor(tfu);

					error = TDERR_DiskChanged;
					break;

				default:

					error = TDERR_SeekError;
					break;
			}

			goto out;
		}

		ASSERT( tfu->tfu_FilePosition >= 0 );

		tfu->tfu_FilePosition += tfu->tfu_TrackDataSize;

		/* If the cache is enabled, update the cache's idea
		 * of what should be stored in it.
		 */
		#if defined(ENABLE_CACHE)
		{
			SHOWPOINTER(tfd->tfd_CacheContext);
			SHOWVALUE(tfu->tfu_CacheEnabled);
			SHOWVALUE(tfu->tfu_DriveType);

			if(tfd->tfd_CacheContext != NULL &&
			   tfu->tfu_CacheEnabled &&
			   tfu->tfu_DriveType != DRIVE3_5_150RPM)
			{
				update_cache_contents(tfd->tfd_CacheContext,
					tfu, tfu->tfu_CurrentTrackNumber,
					tfu->tfu_TrackData, tfu->tfu_TrackDataSize,
					UDN_UpdateOnly);
			}
		}
		#endif /* ENABLE_CACHE */

		/* Is this the track which contains the reserved blocks,
		 * i.e. the boot block and the file system signature?
		 */
		if (tfu->tfu_CurrentTrackNumber == 0)
		{
			tfu->tfu_FileSystemSignature = *(ULONG *)tfu->tfu_TrackData;

			D(("file system signature = 0x%08lx", tfu->tfu_FileSystemSignature));

			tfu->tfu_BootBlockChecksum = calculate_boot_block_checksum(tfu->tfu_TrackData, TD_SECTOR * BOOTSECTS);

			D(("boot block checksum = 0x%08lx", tfu->tfu_BootBlockChecksum));
		}
		/* Is this the track which contains the root directory? */
		else if (tfu->tfu_CurrentTrackNumber == tfu->tfu_RootDirTrackNumber)
		{
			const struct RootDirBlock * rdb = (struct RootDirBlock *)&((UBYTE *)tfu->tfu_TrackData)[tfu->tfu_RootDirBlockOffset];

			SHOWMSG("updating the root directory information");

			tfu->tfu_RootDirValid = root_directory_is_valid(rdb);
			if(tfu->tfu_RootDirValid)
			{
				TEXT root_directory_name[32];
				size_t len;

				len = rdb->rdb_Name[0];

				/* Avoid unexpected buffer overflows. */
				if(len >= sizeof(root_directory_name))
					len = sizeof(root_directory_name)-1;

				CopyMem(&rdb->rdb_Name[1], root_directory_name, len);
				root_directory_name[len] = '\0';

				D(("volume name = \"%s\"", root_directory_name));
				D(("creation date and time = %ld/%ld/%ld",
					rdb->rdb_DiskInitialization.ds_Days,
					rdb->rdb_DiskInitialization.ds_Minute,
					rdb->rdb_DiskInitialization.ds_Tick));

				CopyMem(root_directory_name, tfu->tfu_RootDirName, len+1);
				tfu->tfu_RootDirDate = rdb->rdb_DiskInitialization;
			}
		}

		/* Update the track checksum, while we're at it. */
		tfu->tfu_TrackDataChecksum = new_track_checksum;

		if(tfu->tfu_DiskChecksumTable != NULL)
		{
			ASSERT( 0 <= tfu->tfu_CurrentTrackNumber && tfu->tfu_CurrentTrackNumber < tfu->tfu_DiskChecksumTableLength );

			tfu->tfu_DiskChecksumTable[tfu->tfu_CurrentTrackNumber] = new_track_checksum;
			tfu->tfu_ChecksumUpdated = TRUE;
		}

		/* The file data may have to be flushed to disk
		 * before the medium is ejected.
		 */
		tfu->tfu_ChangesMade = TRUE;
	}
	else
	{
		SHOWMSG("track contents are unchanged; no need to write them back");
	}

	tfu->tfu_TrackDataChanged = FALSE;

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/CMD_CLEAR *******************************************
*
*   NAME
*	CMD_CLEAR -- mark the track buffer as containing invalid data.
*
*   FUNCTION
*	This command marks the track buffer as invalid, forcing a
*	reread of the disk on the next operation. CMD_UPDATE
*	would be used to force data out to the disk before turning the motor
*	off. CMD_CLEAR will not do an update, nor will an update command
*	do a clear.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	CMD_CLEAR
*	io_Flags	0
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*
*   SEE ALSO
*	CMD_WRITE, CMD_UPDATE
*
******************************************************************************
*/

static LONG
cmd_clear(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	/* If this is an extended command, check if the I/O request
	 * is the right size and if it isn't yet stale.
	 */
	error = check_extended_command(io);
	if(error != OK)
		goto out;

	mark_track_buffer_as_invalid(tfu);

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/CMD_READ ********************************************
*
*   NAME
*	CMD_READ -- read sectors of data from a disk.
*
*   FUNCTION
*	This command transfers data from the track buffer to a supplied
*	buffer. If the desired sector is already in the track buffer, no disk
*	activity is initiated. If the desired sector is not in the buffer, the
*	track containing that sector is automatically read in. If the data in
*	the current track buffer has been modified, it is written out to the
*	disk before a new track is read.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	CMD_READ
*	io_Flags	0
*	io_Data		pointer to the buffer where the data should be put
*	io_Length	number of bytes to read, must be a multiple of
*			TD_SECTOR.
*	io_Offset	byte offset from the start of the disk describing
*			where to read data from, must be a multiple of
*			TD_SECTOR.
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*
*   SEE ALSO
*	CMD_WRITE
*
******************************************************************************
*/

static LONG
cmd_read(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG num_bytes_read = 0;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	/* We need to read from a file. */
	if(NOT unit_medium_is_present(tfu))
	{
		SHOWMSG("no disk present");

		error = TDERR_DiskChanged;
		goto out;
	}

	/* If this is an extended command, check if the I/O request
	 * is the right size and if it isn't yet stale.
	 */
	error = check_extended_command(io);
	if(error != OK)
		goto out;

	/* This should be a 'struct IOStdReq', or otherwise the
	 * io_Length, io_Actual and io_Data fields are
	 * unavailable.
	 */
	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	/* Is the io_Offset within bounds and a multiple
	 * of the sector size?
	 */
	error = check_offset(io);
	if(error != OK)
		goto out;

	/* Is the length a multiple of the sector size, and is
	 * the data WORD-aligned (16 bit word)?
	 */
	error = check_io_request_data_and_length(io, 0, TD_SECTOR, sizeof(WORD));
	if(error != OK)
		goto out;

	/* We know that the offset value is sound and that
	 * the requested length passes a simple test for
	 * validity, but how do they play together? Both the
	 * length itself and the position at which the data
	 * should be read must describe an interval which
	 * does not exceed the last readable byte of the
	 * medium.
	 */
	ASSERT( NOT addition_overflows(io->io_Offset, io->io_Length) && io->io_Offset + io->io_Length <= (ULONG)tfu->tfu_FileSize );

	if(addition_overflows(io->io_Offset, io->io_Length) ||
       io->io_Offset + io->io_Length > (ULONG)tfu->tfu_FileSize)
	{
		SHOWMSG("combined offset+length out of bounds");

		error = IOERR_BADLENGTH;
		goto out;
	}

	D(("read %ld bytes from offset %ld (track %ld, sector %ld) to 0x%08lx",
		io->io_Length,
		io->io_Offset,
		io->io_Offset / tfu->tfu_TrackDataSize, (io->io_Offset % tfu->tfu_TrackDataSize) / TD_SECTOR,
		io->io_Data));

	/* A read operation always enables the motor
	 * if it's not currently running.
	 */
	if(NOT tfu->tfu_MotorEnabled)
		SHOWMSG("turning the motor on");

	tfu->tfu_MotorEnabled = TRUE;

	/* Do we need to read anything at all? */
	if(io->io_Length > 0)
	{
		LONG which_track;
		LONG num_bytes_to_read = io->io_Length;
		LONG num_bytes_available;
		const BYTE * source = tfu->tfu_TrackData;
		BYTE * destination = io->io_Data;
		LONG source_position;
		LONG destination_position;
		LONG num_bytes;

		#if DEBUG
		check_stack_size_available(SysBase);
		#endif /* DEBUG */

		ASSERT( FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS );

		ASSERT( num_bytes_read + num_bytes_to_read == io->io_Length );
		ASSERT( tfu->tfu_TrackDataSize > 0 );

		/* Which track is the requested data stored on? */
		which_track = io->io_Offset / tfu->tfu_TrackDataSize;

		ASSERT( 0 <= which_track && which_track < tfu->tfu_NumTracks );
		ASSERT( which_track * tfu->tfu_TrackDataSize < tfu->tfu_FileSize );

		/* Where on that track does the read operation begin? */
		source_position = io->io_Offset % tfu->tfu_TrackDataSize;

		/* How much data can we read from the track? */
		num_bytes_available = tfu->tfu_TrackDataSize - source_position;

		ASSERT( num_bytes_available > 0 );

		destination_position = 0;

		while(TRUE)
		{
			D(("track = %ld, position = %ld, bytes to go = %ld, bytes available on track = %ld",
				which_track, source_position, num_bytes_to_read, num_bytes_available));

			ASSERT( which_track < tfu->tfu_NumTracks );
			ASSERT( which_track * tfu->tfu_TrackDataSize < tfu->tfu_FileSize );

			ASSERT( num_bytes_to_read > 0 );
			ASSERT( num_bytes_available > 0 );

			/* Don't read more data than the track provides. */
			num_bytes = num_bytes_to_read;
			if(num_bytes > num_bytes_available)
				num_bytes = num_bytes_available;

			ASSERT( num_bytes > 0 );
			ASSERT( source_position + num_bytes <= tfu->tfu_TrackDataSize );
			ASSERT( destination_position + num_bytes <= io->io_Length );

			/* Is the track data which we want to read not
			 * currently in memory?
			 */
			if(tfu->tfu_CurrentTrackNumber != which_track)
			{
				if(tfu->tfu_CurrentTrackNumber != -1)
					D(("currently keeping track %ld in the buffer, need to move to track %ld", tfu->tfu_CurrentTrackNumber, which_track));
				else
					D(("moving to track %ld", which_track));

				/* We may have to write back the changes currently
				 * in the track buffer first.
				 */
				if(tfu->tfu_TrackDataChanged)
				{
					D(("there is still data in the buffer that needs to be written back to track %ld",
						tfu->tfu_CurrentTrackNumber));

					error = write_back_track_data(tfu);
					if(error != OK)
					{
						D(("couldn't write back the track data, error=%ld", error));
						goto out;
					}
				}

				error = read_track_data(tfu, which_track);
				if(error != OK)
				{
					D(("couldn't the track data, error=%ld", error));
					goto out;
				}
			}

			ASSERT( destination == io->io_Data );

			CopyMem(&source[source_position], &destination[destination_position], num_bytes);

			ASSERT( num_bytes_to_read >= num_bytes );
			ASSERT( num_bytes_available >= num_bytes );

			destination_position	+= num_bytes;
			source_position			+= num_bytes;
			num_bytes_to_read		-= num_bytes;
			num_bytes_available		-= num_bytes;
			num_bytes_read			+= num_bytes;

			ASSERT( num_bytes_read + num_bytes_to_read == io->io_Length );

			ASSERT( num_bytes_to_read >= 0 );

			/* Are we finished yet? */
			if(num_bytes_to_read == 0)
			{
				ASSERT( num_bytes_read == io->io_Length );

				SHOWMSG("all done.");
				break;
			}

			ASSERT( source_position <= tfu->tfu_TrackDataSize );

			/* Is the next data to be read stored on the
			 * following track?
			 */
			if(source_position == tfu->tfu_TrackDataSize)
			{
				source_position = 0;
				num_bytes_available = tfu->tfu_TrackDataSize;
				which_track++;

				D(("moving from track %ld to track %ld, source position = %ld, bytes available = %ld",
					which_track-1,
					which_track,
					source_position,
					num_bytes_available));

				ASSERT( which_track <= tfu->tfu_NumTracks );
			}
		}

		ASSERT( num_bytes_read + num_bytes_to_read == io->io_Length );

		/* If this is an ETD_READ command, then we'll have to
		 * pretend to have read the sector label information,
		 * if this was requested.
		 */
		if(io->io_Command == ETD_READ)
		{
			struct IOExtTD * iotd = (struct IOExtTD *)io;
			APTR sector_label = (APTR)iotd->iotd_SecLabel;

			SHOWMSG("this is an extended read command");

			/* Are we required to return sector label data? */
			if(sector_label != NULL)
			{
				int num_sectors = io->io_Length / TD_SECTOR;

				D(("setting %ld sector(s) worth of label data to zero at 0x%08lx", num_sectors, sector_label));

				/* Somehow we could only read zeroes for the
				 * sector label information.
				 */
				memset(sector_label, 0, num_sectors * TD_LABELSIZE);
			}
			else
			{
				SHOWMSG("no sector label data was requested");
			}
		}
	}

	ASSERT( num_bytes_read == io->io_Length );

	io->io_Actual = io->io_Length;

	SHOWMSG("that went well");

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/* Restart I/O request processing for a unit process which is
 * currently stopped. Note that this must be an immediate
 * command since a stopped unit process cannot perform it (it no
 * longer listens to inbound I/O requests).
 *
 * Please note that trackdisk.device V37 implements this command
 * but lacks documentation for its purpose or use.
 */
static LONG
cmd_start(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	D(("obtaining unit %ld lock", tfu->tfu_UnitNumber));
	ObtainSemaphore(&tfu->tfu_Lock);

	Forbid();

	if(tfu->tfu_Stopped)
	{
		tfu->tfu_Stopped = FALSE;

		D(("unit %ld is no longer stopped", tfu->tfu_UnitNumber));

		if(tfu->tfu_Process != NULL)
		{
			D(("restarting process for unit %ld", tfu->tfu_UnitNumber));

			ASSERT( tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_SigTask == tfu->tfu_Process && tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Flags == PA_SIGNAL );
			ASSERT( 0 <= tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_SigBit && tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_SigBit <= 31 );

			/* Wake up the unit process so that it will resume
			 * processing the queued I/O requests.
			 */
			Signal(&tfu->tfu_Process->pr_Task, (1UL << tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_SigBit));
		}
		else
		{
			D(("unit %ld was stopped, but unit process is not currently active anyway", tfu->tfu_UnitNumber));
		}
	}
	else
	{
		D(("unit %ld has not been stopped; no need to restart it", tfu->tfu_UnitNumber));
	}

	Permit();

	D(("releasing unit %ld lock", tfu->tfu_UnitNumber));
	ReleaseSemaphore(&tfu->tfu_Lock);

	RETURN(OK);
	return(OK);
}

/****************************************************************************/

/* Stop I/O request processing for a unit process.
 *
 * Please note that trackdisk.device V37 implements this command
 * but lacks documentation for its purpose or use.
 */
static LONG
cmd_stop(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	D(("obtaining unit %ld lock", tfu->tfu_UnitNumber));
	ObtainSemaphore(&tfu->tfu_Lock);

	Forbid();

	if(NOT tfu->tfu_Stopped)
	{
		D(("stopping unit %ld", tfu->tfu_UnitNumber));

		tfu->tfu_Stopped = TRUE;
	}
	else
	{
		D(("unit %ld has already been stopped", tfu->tfu_UnitNumber));
	}

	Permit();

	D(("releasing unit %ld lock", tfu->tfu_UnitNumber));
	ReleaseSemaphore(&tfu->tfu_Lock);

	RETURN(OK);
	return(OK);
}

/****************************************************************************/

/****** trackfile.device/CMD_UPDATE ******************************************
*
*   NAME
*	CMD_UPDATE -- write out the track buffer if it is dirty.
*
*   FUNCTION
*	The trackfile device does not write data sectors unless it is
*	necessary (you request that a different track be used) or until the
*	user requests that an update be performed. This improves system speed
*	by caching disk operations. This command ensures that any
*	buffered data is flushed out to the disk. If the track buffer has not
*	been changed since the track was read in, this command does nothing.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	CMD_UPDATE
*	io_Flags	0
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*
*   SEE ALSO
*	CMD_WRITE
*
******************************************************************************
*/

static LONG
cmd_update(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	if(NOT unit_medium_is_present(tfu))
	{
		SHOWMSG("no disk present");

		error = TDERR_DiskChanged;
		goto out;
	}

	/* If this is an extended command, check if the I/O request
	 * is the right size and if it isn't yet stale.
	 */
	error = check_extended_command(io);
	if(error != OK)
		goto out;

	/* If necessary, write back the track data. */
	if(tfu->tfu_TrackDataChanged)
	{
		error = write_back_track_data((struct TrackFileUnit *)io->io_Unit);
		if(error != OK)
			goto out;
	}

	ASSERT( error == OK );

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/CMD_WRITE *******************************************
*
*   NAME
*	CMD_WRITE -- write sectors of data to a disk.
*
*   FUNCTION
*	This command transfers data from a supplied buffer to the track
*	buffer. If the track that contains this sector is already in the track
*	buffer, no disk activity is initiated. If the desired sector is not in
*	the buffer, the track containing that sector is automatically read in.
*	If the data in the current track buffer has been modified, it is
*	written out to the disk before the new track is read in for
*	modification.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	CMD_WRITE
*	io_Flags	0
*	io_Data		pointer to the buffer where the data should be put
*	io_Length	number of bytes to write, must be a multiple of
*			TD_SECTOR.
*	io_Offset	byte offset from the start of the disk describing
*			where to write data to, must be a multiple of
*			TD_SECTOR.
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h>
*
*   SEE ALSO
*	CMD_READ, TD_FORMAT
*
******************************************************************************
*/

static LONG
cmd_write(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG num_bytes_written = 0;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	if(NOT unit_medium_is_present(tfu))
	{
		SHOWMSG("no disk present");

		error = TDERR_DiskChanged;
		goto out;
	}

	if(tfu->tfu_WriteProtected)
	{
		SHOWMSG("disk is write-protected");

		error = TDERR_WriteProt;
		goto out;
	}

	error = check_extended_command(io);
	if(error != OK)
		goto out;

	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	error = check_offset(io);
	if(error != OK)
		goto out;

	error = check_io_request_data_and_length(io, 0, TD_SECTOR, sizeof(WORD));
	if(error != OK)
		goto out;

	ASSERT( NOT addition_overflows(io->io_Offset, io->io_Length) && io->io_Offset + io->io_Length <= (ULONG)tfu->tfu_FileSize );

	if(addition_overflows(io->io_Offset, io->io_Length) ||
	   io->io_Offset + io->io_Length > (ULONG)tfu->tfu_FileSize)
	{
		SHOWMSG("combined offset+length out of bounds");

		error = IOERR_BADLENGTH;
		goto out;
	}

	D(("write %ld bytes to offset %ld (track %ld, sector %ld) from 0x%08lx",
		io->io_Length,
		io->io_Offset,
		io->io_Offset / tfu->tfu_TrackDataSize, (io->io_Offset % tfu->tfu_TrackDataSize) / TD_SECTOR,
		io->io_Data));

	/* A write operation always enables the motor
	 * if it's not currently running.
	 */
	if(NOT tfu->tfu_MotorEnabled)
		SHOWMSG("turning the motor on");

	tfu->tfu_MotorEnabled = TRUE;

	/* Do we need to write anything at all? */
	if(io->io_Length > 0)
	{
		LONG which_track;
		LONG num_bytes_to_write = io->io_Length;
		LONG num_bytes_remaining;
		const BYTE * source = io->io_Data;
		BYTE * destination = tfu->tfu_TrackData;
		LONG source_position;
		LONG destination_position;
		LONG num_bytes;

		#if DEBUG
		check_stack_size_available(SysBase);
		#endif /* DEBUG */

		ASSERT( FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS );

		ASSERT( num_bytes_written + num_bytes_to_write == io->io_Length );
		ASSERT( tfu->tfu_TrackDataSize > 0 );

		/* Which track is the data stored on? */
		which_track = io->io_Offset / tfu->tfu_TrackDataSize;

		ASSERT( 0 <= which_track && which_track < tfu->tfu_NumTracks );
		ASSERT( which_track * tfu->tfu_TrackDataSize < tfu->tfu_FileSize );

		/* Where on that track does the write operation begin? */
		destination_position = io->io_Offset % tfu->tfu_TrackDataSize;

		/* How much data can we write to the track? */
		num_bytes_remaining = tfu->tfu_TrackDataSize - destination_position;

		ASSERT( num_bytes_remaining >= 0 );

		source_position = 0;

		while(TRUE)
		{
			D(("track = %ld, position = %ld, bytes to go = %ld, bytes remaining for track = %ld",
				which_track, destination_position, num_bytes_to_write, num_bytes_remaining));

			ASSERT( which_track < tfu->tfu_NumTracks );
			ASSERT( which_track * tfu->tfu_TrackDataSize < tfu->tfu_FileSize );

			ASSERT( num_bytes_to_write > 0 );
			ASSERT( num_bytes_remaining > 0 );

			/* Don't write more data than the track can hold. */
			num_bytes = num_bytes_to_write;
			if(num_bytes > num_bytes_remaining)
				num_bytes = num_bytes_remaining;

			ASSERT( num_bytes > 0 );
			ASSERT( source_position + num_bytes <= io->io_Length );
			ASSERT( destination_position + num_bytes <= tfu->tfu_TrackDataSize );

			/* Is the track data which we need to modify not
			 * currently in memory?
			 */
			if(tfu->tfu_CurrentTrackNumber != which_track)
			{
				if(tfu->tfu_CurrentTrackNumber != -1)
					D(("currently keeping track %ld in the buffer, need to move to track %ld", tfu->tfu_CurrentTrackNumber, which_track));
				else
					D(("moving to track %ld", which_track));

				/* We may have to write back the changes currently
				 * in the track buffer first.
				 */
				if(tfu->tfu_TrackDataChanged)
				{
					D(("there is still data in the buffer that needs to be written back to track %ld first",
						tfu->tfu_CurrentTrackNumber));

					error = write_back_track_data(tfu);
					if(error != OK)
					{
						D(("couldn't write back the track data, error=%ld", error));
						goto out;
					}
				}

				/* Will this write operation leave parts of the track
				 * unchanged? Then we will need to read the track
				 * data first.
				 */
				if(num_bytes < tfu->tfu_TrackDataSize)
				{
					D(("track %ld will be partly overwritten; we will have to read it...", which_track));

					error = read_track_data(tfu, which_track);
					if(error != OK)
					{
						D(("couldn't read the track data, error=%ld", error));
						goto out;
					}
				}
				/* Otherwise it doesn't matter. The contents of the track will
				 * be completely replaced.
				 */
				else
				{
					D(("entire track %ld will be overwritten; no need to read it.", which_track));

					/* Data will be written to this new track. */
					tfu->tfu_CurrentTrackNumber = tfu->tfu_Unit.tdu_CurrTrk = which_track;

					/* When writing back this track, do not compare the
					 * the old track checksum against the new one to
					 * determine whether the data must be written back
					 * to the file. We want the check to be skipped
					 * because we have no checksum of the data which
					 * we just chose not to read.
					 */
					tfu->tfu_IgnoreTrackChecksum = TRUE;
				}
			}

			ASSERT( source == io->io_Data );

			CopyMem(&source[source_position], &destination[destination_position], num_bytes);

			tfu->tfu_TrackDataChanged = TRUE;

			ASSERT( num_bytes_to_write >= num_bytes );
			ASSERT( num_bytes_remaining >= num_bytes );

			destination_position	+= num_bytes;
			source_position			+= num_bytes;
			num_bytes_to_write		-= num_bytes;
			num_bytes_remaining		-= num_bytes;
			num_bytes_written		+= num_bytes;

			ASSERT( num_bytes_written + num_bytes_to_write == io->io_Length );

			ASSERT( num_bytes_to_write >= 0 );

			/* Are we finished yet? */
			if(num_bytes_to_write == 0)
			{
				ASSERT( num_bytes_written == io->io_Length );

				SHOWMSG("all done.");
				break;
			}

			ASSERT( destination_position <= tfu->tfu_TrackDataSize );

			/* Is the next data to be written to stored on the
			 * following track?
			 */
			if(destination_position == tfu->tfu_TrackDataSize)
			{
				destination_position = 0;
				num_bytes_remaining = tfu->tfu_TrackDataSize;
				which_track++;

				ASSERT( which_track <= tfu->tfu_NumTracks );
			}
		}

		ASSERT( num_bytes_written + num_bytes_to_write == io->io_Length );
	}

	ASSERT( num_bytes_written == io->io_Length );

	io->io_Actual = io->io_Length;

	SHOWMSG("that went well");

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/TD_ADDCHANGEINT ************************************
*
*   NAME
*	TD_ADDCHANGEINT -- add a disk change software interrupt handler.
*
*   FUNCTION
*	This command lets you add a software interrupt handler to the
*	disk device that gets invoked whenever a disk insertion or removal
*	occurs.
*
*	You must pass in a properly initialized Exec Interrupt structure
*	and be prepared to deal with disk insertions/removals
*	immediately. From within the interrupt handler, you may only call the
*	status commands that can use IOF_QUICK.
*
*	To set up the handler, an Interrupt structure must be initialized.
*	This structure is supplied as the io_Data to the TD_ADDCHANGEINT
*	command. The handler then gets linked into the handler chain and
*	gets invoked whenever a disk change happens. You must eventually
*	remove the handler before you exit.
*
*	This command only returns when the handler is removed. That is,
*	the device holds onto the IO request until the TD_REMCHANGEINT command
*	is executed with that same IO request. Hence, you must use SendIO()
*	with this command.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_ADDCHANGEINT
*	io_Flags	0
*	io_Length	sizeof(struct Interrupt)
*	io_Data		pointer to Interrupt structure
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*
*   SEE ALSO
*	TD_REMCHANGEINT, <devices/trackdisk.h>, <exec/interrupts.h>,
*	<exec/errors.h>, exec.library/Cause()
*
******************************************************************************
*/

static LONG
td_addchangeint(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	error = check_io_request_data_and_length(io, sizeof(struct Interrupt), 0, 0);
	if(error != OK)
		goto out;

	SHOWPOINTER(io->io_Data);

	Forbid();

	/* Note that we keep the IOStdReq itself and not just the
	 * Interrupt data structure address stored in the io_Data field.
	 * This means that removing the Interrupt later requires that
	 * the client submits exactly the same IOStdReq which was used here.
	 *
	 * This procedure follows the trackdisk.device documentation
	 * and which is also why the TD_REMCHANGEINT command
	 * must be an "immediate" command executed on the context
	 * of the calling Task.
	 */
	AddTailMinList(&tfu->tfu_ChangeIntList, &io->io_Message.mn_Node);

	Permit();

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/TD_CHANGENUM ****************************************
*
*   NAME
*	TD_CHANGENUM -- return the current value of the disk-change counter.
*
*   FUNCTION
*	This command returns the current value of the disk-change counter (as
*	used by the enhanced commands). The disk change counter is incremented
*	each time a disk is inserted or removed from the trackdisk unit.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_CHANGENUM
*	io_Flags	0 or IOF_QUICK
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*	io_Actual - if io_Error is 0, this contains the current value of the
*		    disk-change counter.
*
******************************************************************************
*/

static LONG
td_changenum(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	/* The counter is updated under Forbid() conditions,
	 * so we can safely read it here without using
	 * Forbid()/Permit() guards.
	 */
	io->io_Actual = tfu->tfu_Unit.tdu_Counter;

	D(("change counter = %ld", io->io_Actual));

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/TD_CHANGESTATE **************************************
*
*   NAME
*	TD_CHANGESTATE -- check if a disk is currently in a drive.
*
*   FUNCTION
*	This command checks to see if there is currently a disk in a drive.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_CHANGESTATE
*	io_Flags	0 or IOF_QUICK
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*	io_Actual - if io_Error is 0, this tells you whether a disk is in
*	        the drive. 0 means there is a disk, while anything else
*	        indicates there is no disk.
*
******************************************************************************
*/

static LONG
td_changestate(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	/* The file pointer is updated under Forbid() conditions,
	 * so we can safely read it here without using
	 * Forbid()/Permit() guards.
	 */

	/* A value of 0 means that there is a disk present. */
	io->io_Actual = (tfu->tfu_File != ZERO) ? 0 : 1;

	D(("disk is gone = %ld (0 means no)", io->io_Actual));

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/TD_EJECT ********************************************
*
*   NAME
*	TD_EJECT -- eject the disk in the drive, if possible.
*
*   FUNCTION
*	This command causes the drive to attempt to eject the disk in
*	it, if any. We ignore io_Length because we could not load a
*	medium here if we wanted to and also because the V34 documentation
*	for the TD_EJECT command omitted the part of the io_Length
*	field, so developers may not even know that a load operation
*	may have been supported in the first place.
*
*	If there is still modified data in the write buffer,
*	trackfile.device will first attempt to write it back to
*	the disk image file. Should this attempt fail, then the
*	disk image will not be ejected.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_EJECT
*	io_Flags	0
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*
******************************************************************************
*/

static LONG
td_eject(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	if(NOT unit_medium_is_present(tfu))
	{
		SHOWMSG("no disk present");

		error = TDERR_DiskChanged;
		goto out;
	}

	error = eject_image_file(tfu);
	if(error != OK)
	{
		D(("ejection didn't work (error=%ld)", error));
		goto out;
	}

	trigger_change(tfu);

	SHOWMSG("medium ejected");

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/TD_FORMAT *******************************************
*
*   NAME
*	TD_FORMAT -- format a track on a disk.
*
*   FUNCTION
*	These commands are used to write data to a track that either
*	has not yet been formatted or has had a hard error on a standard write
*	command. TD_FORMAT completely ignores all data currently on a track and
*	does not check for disk change before performing the command. The
*	io_Data field must point to at least one track worth of data. The
*	io_Offset field must be track aligned, and the io_Length field must be
*	in units of track length (that is, TD_SECTOR * NUMSEC).
*
*	The device will format the requested tracks, filling each sector with
*	the contents of the buffer pointed to by io_Data. You
*	should do a read pass to verify the data.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_FORMAT
*	io_Flags	0
*	io_Data		points to a buffer containing the data to write to the
*			track, must be at least as large as io_Length.
*	io_Length	number of bytes to format, must be a multiple of
*			(TD_SECTOR * NUMSEC).
*	io_Offset	byte offset from the start of the disk for the track to
*			format, must be a multiple of (TD_SECTOR * NUMSEC).
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*
*   NOTES
*	The trackdisk.device TD_FORMAT documentation references the TD_SECTOR
*	and NUMSEC constants. In context, NUMSEC must be either 11 for a
*	double density floppy disk or 22 for a high density floppy disk.
*	TD_SECTOR must always be 512.
*
*   SEE ALSO
*	CMD_WRITE
*
******************************************************************************
*/

static LONG
td_format(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG num_bytes_written = 0;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	if(NOT unit_medium_is_present(tfu))
	{
		SHOWMSG("no disk present");

		error = TDERR_DiskChanged;
		goto out;
	}

	if(tfu->tfu_WriteProtected)
	{
		SHOWMSG("disk is write-protected");

		error = TDERR_WriteProt;
		goto out;
	}

	error = check_extended_command(io);
	if(error != OK)
		goto out;

	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	error = check_offset(io);
	if(error != OK)
		goto out;

	error = check_io_request_data_and_length(io, 0, tfu->tfu_TrackDataSize, sizeof(WORD));
	if(error != OK)
		goto out;

	ASSERT( NOT addition_overflows(io->io_Offset, io->io_Length) && io->io_Offset + io->io_Length <= (ULONG)tfu->tfu_FileSize );

	if(io->io_Offset + io->io_Length > (ULONG)tfu->tfu_FileSize ||
	   addition_overflows(io->io_Offset, io->io_Length))
	{
		SHOWMSG("combined offset+length out of bounds");

		error = IOERR_BADLENGTH;
		goto out;
	}

	/* Formatting only works on full tracks. */
	ASSERT( (io->io_Offset % tfu->tfu_TrackDataSize) == 0 );

	if((io->io_Offset % tfu->tfu_TrackDataSize) != 0)
	{
		D(("formatting only works on full tracks (offset=%ld, length=%ld, track size=%ld)",
			(io->io_Offset % tfu->tfu_TrackDataSize), io->io_Length, tfu->tfu_TrackDataSize));

		error = IOERR_BADLENGTH;
		goto out;
	}

	D(("write (format!) %ld bytes to offset %ld (track %ld) from 0x%08lx",
		io->io_Length,
		io->io_Offset,
		io->io_Offset / tfu->tfu_TrackDataSize,
		io->io_Data));

	/* A write operation always enables the motor
	 * if it's not currently running.
	 */
	if(NOT tfu->tfu_MotorEnabled)
		SHOWMSG("turning the motor on");

	tfu->tfu_MotorEnabled = TRUE;

	/* Do we need to format anything at all? */
	if(io->io_Length > 0)
	{
		LONG which_track;
		LONG num_bytes_to_write = io->io_Length;
		LONG num_bytes_remaining;
		const BYTE * source = io->io_Data;
		BYTE * destination = tfu->tfu_TrackData;
		LONG source_position;
		LONG destination_position;
		LONG num_bytes;

		#if DEBUG
		check_stack_size_available(SysBase);
		#endif /* DEBUG */

		ASSERT( num_bytes_written + num_bytes_to_write == io->io_Length );
		ASSERT( tfu->tfu_TrackDataSize > 0 );

		/* This must be a complete track. */
		destination_position = 0;

		/* Which track is the data stored on? */
		which_track = io->io_Offset / tfu->tfu_TrackDataSize;

		ASSERT( NOT multiplication_overflows(which_track, tfu->tfu_TrackDataSize) );
		ASSERT( 0 <= which_track && which_track < tfu->tfu_NumTracks );
		ASSERT( which_track * tfu->tfu_TrackDataSize < tfu->tfu_FileSize );

		/* How much data can we write to the track? */
		num_bytes_remaining = tfu->tfu_TrackDataSize - destination_position;

		ASSERT( num_bytes_remaining >= 0 );

		source_position = 0;

		/* We may have to write back the changes currently
		 * in the track buffer first if formatting will not
		 * overwrite this track anyway.
		 */
		if(tfu->tfu_TrackDataChanged)
		{
			LONG num_tracks = num_bytes_to_write / tfu->tfu_TrackDataSize;

			/* Are the contents of the track buffer not going to
			 * be overwritten by formatting?
			 */
			if(NOT (which_track <= tfu->tfu_CurrentTrackNumber && tfu->tfu_CurrentTrackNumber < which_track + num_tracks))
			{
				D(("there is still data in the buffer that needs to be written back to track %ld first",
					tfu->tfu_CurrentTrackNumber));

				ASSERT( NOT tfu->tfu_WriteProtected );

				error = write_back_track_data((struct TrackFileUnit *)io->io_Unit);
				if(error != OK)
				{
					D(("couldn't write back the track data, error=%ld", error));
					goto out;
				}
			}
		}

		/* Start the track buffer with a blank slate. */
		mark_track_buffer_as_invalid(tfu);

		memset(tfu->tfu_TrackData, 0, tfu->tfu_TrackDataSize);

		while(TRUE)
		{
			D(("track = %ld, position = %ld, bytes to go = %ld, bytes remaining for track = %ld",
				which_track, destination_position, num_bytes_to_write, num_bytes_remaining));

			D(("moving to track %ld", which_track));

			tfu->tfu_CurrentTrackNumber = tfu->tfu_Unit.tdu_CurrTrk = which_track;

			ASSERT( num_bytes_to_write > 0 );
			ASSERT( num_bytes_remaining > 0 );

			/* Don't write more data than the track can hold. */
			num_bytes = num_bytes_to_write;
			if(num_bytes > num_bytes_remaining)
				num_bytes = num_bytes_remaining;

			ASSERT( num_bytes > 0 );
			ASSERT( source_position + num_bytes <= io->io_Length );
			ASSERT( destination_position + num_bytes <= tfu->tfu_TrackDataSize );

			CopyMem(&source[source_position], &destination[destination_position], num_bytes);

			/* Make sure that the contents of this track go
			 * into the file.
			 */
			tfu->tfu_TrackDataChanged = TRUE;
			tfu->tfu_IgnoreTrackChecksum = TRUE;

			error = write_back_track_data((struct TrackFileUnit *)io->io_Unit);
			if(error != OK)
			{
				D(("couldn't write back the track data, error=%ld", error));
				goto out;
			}

			ASSERT( num_bytes_to_write >= num_bytes );
			ASSERT( num_bytes_remaining >= num_bytes );

			destination_position	+= num_bytes;
			source_position			+= num_bytes;
			num_bytes_to_write		-= num_bytes;
			num_bytes_remaining		-= num_bytes;
			num_bytes_written		+= num_bytes;

			ASSERT( num_bytes_written + num_bytes_to_write == io->io_Length );

			ASSERT( num_bytes_to_write >= 0 );

			/* Are we finished yet? */
			if(num_bytes_to_write == 0)
			{
				SHOWMSG("all done.");
				break;
			}

			ASSERT( destination_position <= tfu->tfu_TrackDataSize );

			/* Is the next data to be written to stored on the
			 * following track?
			 */
			if(destination_position == tfu->tfu_TrackDataSize)
			{
				destination_position = 0;
				num_bytes_remaining = tfu->tfu_TrackDataSize;
				which_track++;

				ASSERT( which_track <= tfu->tfu_NumTracks );
			}
		}

		ASSERT( num_bytes_written + num_bytes_to_write == io->io_Length );
	}

	ASSERT( num_bytes_written == io->io_Length );

	io->io_Actual = io->io_Length;

	SHOWMSG("that went well");

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/TD_GETDRIVETYPE *************************************
*
*   NAME
*	TD_GETDRIVETYPE -- return the type of disk drive for the unit that was
*			   opened.
*
*   FUNCTION
*	This command returns the type of the disk drive to the user.
*	This number will be a small integer and will come from the set of
*	DRIVEXXX constants defined in <devices/trackdisk.h>.
*
*	The only way you can actually use this command is if the trackfile
*	device understands the drive type of the hardware that is "plugged in".
*	This is because the OpenDevice() call will fail if the trackfile device
*	does not understand the drive type.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_GETDRIVETYPE
*	io_Flags	0 or IOF_QUICK
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*	io_Actual - if io_Error is 0 this contains the drive type connected to
*		    this unit.
*
*   SEE ALSO
*	TD_GETNUMTRACKS, <devices/trackdisk.h>, <exec/errors.h>
*
******************************************************************************
*/

static LONG
td_getdrivetype(struct IOStdReq * io)
{
	const struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	io->io_Actual = tfu->tfu_DriveType;

	if (tfu->tfu_DriveType == DRIVE3_5)
		SHOWMSG("drive type = DRIVE3_5");
	else if (tfu->tfu_DriveType == DRIVE3_5_150RPM)
		SHOWMSG("drive type = DRIVE3_5_150RPM");
	else
		SHOWMSG("drive type = DRIVE5_25");

	D(("drive type = %ld (%ld means DD 3.5\", %ld means HD 3.5\")",
		io->io_Actual, DRIVE3_5, DRIVE3_5_150RPM));

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/TD_GETGEOMETRY **************************************
*
*   NAME
*	TD_GETGEOMETRY -- return the geometry of the drive.
*
*   FUNCTION
*	This command returns a full set of information about the
*	layout of the drive. The information is returned in the
*	DriveGeometry structure pointed to by io_Data.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_GETGEOMETRY
*	io_Flags	0
*	io_Data		Pointer to a DriveGeometry structure
*	io_Length	sizeof(struct DriveGeometry)
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*
*   NOTE
*	This information may change when a disk is inserted.
*
*   SEE ALSO
*	TD_GETDRIVETYPE, TD_GETNUMTRACKS
*
******************************************************************************
*/

static LONG
td_getgeometry(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	struct DriveGeometry * dg;
	LONG num_sectors;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	error = check_io_request_data_and_length(io, sizeof(*dg), 0, 0);
	if(error != OK)
		goto out;

	dg = io->io_Data;

	memset(dg, 0, sizeof(*dg));

	/* For the moment we only support 3.5" drives. */
	if(tfu->tfu_DriveType == DRIVE3_5_150RPM)
		num_sectors = 2 * NUMSECS;
	else
		num_sectors = NUMSECS;

	if (tfu->tfu_DriveType == DRIVE3_5)
		SHOWMSG("drive type = DRIVE3_5");
	else if (tfu->tfu_DriveType == DRIVE3_5_150RPM)
		SHOWMSG("drive type = DRIVE3_5_150RPM");
	else
		SHOWMSG("drive type = DRIVE5_25");

	/* The following makes sense for both a 3.5" and 5.25"
	 * disk drive, provided of course that the number of
	 * tracks and number of sectors per track are both
	 * correct.
	 *
	 * The field values are initialized in exactly the same
	 * manner as is the case for a trackdisk.device unit.
	 */
	dg->dg_SectorSize	= TD_SECTOR;
	dg->dg_Cylinders	= tfu->tfu_NumCylinders;
	dg->dg_Heads		= tfu->tfu_NumHeads;
	dg->dg_TrackSectors	= num_sectors;
	dg->dg_CylSectors	= dg->dg_Heads * dg->dg_TrackSectors;
	dg->dg_TotalSectors	= dg->dg_CylSectors * dg->dg_Cylinders;
	dg->dg_BufMemType	= MEMF_PUBLIC;
	dg->dg_DeviceType	= DG_DIRECT_ACCESS;
	dg->dg_Flags		= DGF_REMOVABLE;

	D(("dg->dg_SectorSize   = %ld",     dg->dg_SectorSize));
	D(("dg->dg_TotalSectors = %ld",     dg->dg_TotalSectors));
	D(("dg->dg_Cylinders    = %ld",     dg->dg_Cylinders));
	D(("dg->dg_CylSectors   = %ld",     dg->dg_CylSectors));
	D(("dg->dg_Heads        = %ld",     dg->dg_Heads));
	D(("dg->dg_TrackSectors = %ld",     dg->dg_TrackSectors));
	D(("dg->dg_BufMemType   = 0x%08lx", dg->dg_BufMemType));
	D(("dg->dg_DeviceType   = %ld",     dg->dg_DeviceType));
	D(("dg->dg_Flags        = 0x%08lx", dg->dg_Flags));

	io->io_Actual = sizeof(*dg);

	D(("io->io_Actual = %ld", io->io_Actual));

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/TD_GETNUMTRACKS *************************************
*
*   NAME
*	TD_GETNUMTRACKS -- return the number of tracks for the type of disk
*			   drive for the unit that was opened.
*
*   FUNCTION
*	This command returns the number of tracks that are available
*	on the disk unit. Note that this command returns the number of
*	cylinders (80 for a 3.5" drive, 40 for some 5.25" drives)
*	and not the number of tracks. The number of tracks would be
*	twice the number of cylinders.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_GETNUMTRACKS
*	io_Flags	0 or IOF_QUICK
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*	io_Actual - if io_Error is 0 this contains the drive type connected to
*	           this unit.
*
*   SEE ALSO
*	TD_GETDRIVETYPE
*
******************************************************************************
*/

static LONG
td_getnumtracks(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	io->io_Actual = tfu->tfu_NumCylinders;

	D(("number of tracks = %ld (actually, that's the number of cylinders)", io->io_Actual));

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/TD_MOTOR ********************************************
*
*   NAME
*	TD_MOTOR -- control the on/off state of a drive motor.
*
*   FUNCTION
*	This command gives control over the disk motor. The motor may be
*	turned on or off.
*
*	Normally, turning the drive on is not necessary, the device does
*	this automatically if it receives a request when the motor is off.
*	However, turning the motor off is the programmer's responsibility.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_MOTOR
*	io_Flags	0
*	io_Length	the requested state of the motor, 0 to turn the motor
*			off, and 1 to turn the motor on.
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*	io_Actual - if io_Error is 0 this contains the previous state of the
*	           drive motor.
*
******************************************************************************
*/

static LONG
td_motor(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	BOOL motor_status_changed;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	if(NOT unit_medium_is_present(tfu))
	{
		SHOWMSG("no disk present");

		error = TDERR_DiskChanged;
		goto out;
	}

	error = check_extended_command(io);
	if(error != OK)
		goto out;

	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	motor_status_changed = (tfu->tfu_MotorEnabled != (io->io_Length != 0));

	io->io_Actual = tfu->tfu_MotorEnabled ? 1 : 0;

	if(io->io_Length != 0)
		tfu->tfu_MotorEnabled = TRUE;
	else
		turn_off_motor(tfu);

	if(motor_status_changed)
		D(("turning the motor %s (was turned %s)", tfu->tfu_MotorEnabled ? "on" : "off", io->io_Actual ? "on" : "off"));

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/TD_PROTSTATUS ***************************************
*
*   NAME
*	TD_PROTSTATUS -- return whether the current disk is write-protected.
*
*   FUNCTION
*	This command is used to determine whether the current disk is
*	write-protected.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_PROTSTATUS
*	io_Flags	0
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*	io_Actual - if io_Error is 0, this tells you whether the disk in the
*	           drive is write-protected. 0 means the disk is NOT write-
*	           protected, while any other value indicates it is.
*
******************************************************************************
*/

static LONG
td_protstatus(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	if(NOT unit_medium_is_present(tfu))
	{
		SHOWMSG("no disk present");

		error = TDERR_DiskChanged;
		goto out;
	}

	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	D(("obtaining unit %ld lock", tfu->tfu_UnitNumber));
	ObtainSemaphore(&tfu->tfu_Lock);

	io->io_Actual = tfu->tfu_WriteProtected;

	D(("releasing unit %ld lock", tfu->tfu_UnitNumber));
	ReleaseSemaphore(&tfu->tfu_Lock);

	D(("medium is write protected = %ld (0 means no)", io->io_Actual));

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/TD_RAWREAD ******************************************
*
*   NAME
*	TD_RAWREAD/ETD_RAWREAD -- read raw data from the disk.
*
*   FUNCTION
*	These commands will produce a track of raw MFM-encoded data from the
*	disk image file data as if it had been read from an Amiga floppy disk
*	drive. This has no real practical use beyond experimentation, testing
*	and quality assurance work.
*
*	The MFM-encoded data follows the specifications of the Amiga 1.0
*	disk format, as described in the 3rd edition "Amiga ROM Kernel
*	Reference Manual: Devices", Appendix C.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_RAWREAD or ETD_RAWREAD.
*	io_Flags	The IOTDB_INDEXSYNC and/or IOTDB_WORDSYNC bits may
*			be set.
*	io_Length	Length of buffer in bytes, with a maximum of 32768
*			bytes.
*	io_Data		Pointer to CHIP memory buffer where raw track data is
*			to be deposited.
*	io_Offset	The number of the track to read in.
*	iotd_Count	(ETD_RAWREAD only) maximum allowable change counter
*			value.
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h>
*
*   NOTES
*	The track buffer provided MUST be in CHIP memory.
*
*	Even if successful, these commands will not update the io_Actual field
*	to reflect the amount of data read.
*
*   SEE ALSO
*	trackdisk.device/TD_RAWREAD
*	trackdisk.device/TD_RAWWRITE
*
******************************************************************************
*/

#if defined(ENABLE_MFM_ENCODING)

/* This comes from the paper "Xorshift RNGs", by George Marsaglia.
 * It's an application of the 32 bit xor() algorithm which so
 * happens is pretty short and rather robust, provided the initial
 * state is not zero.
 */
static ULONG
xor_shift_32(ULONG x)
{
	ULONG old_x = x;

	/* Careful there: this algorithm only works if its
	 * input state is non-zero!
	 */
	if (x == 0)
		x = 1;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;

	D(("xor_shift_32(0x%08lx) -> 0x%08lx", old_x, x));

	return(x);
}

static LONG
td_rawread(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	/* We need to read from a file. */
	if(NOT unit_medium_is_present(tfu))
	{
		SHOWMSG("no disk present");

		error = TDERR_DiskChanged;
		goto out;
	}

	/* If this is an extended command, check if the I/O request
	 * is the right size and if it isn't yet stale.
	 */
	error = check_extended_command(io);
	if(error != OK)
		goto out;

	/* This should be a 'struct IOStdReq', or otherwise the
	 * io_Length, io_Actual and io_Data fields are
	 * unavailable.
	 */
	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	/* This must be a valid track. */
	if(io->io_Offset >= tfu->tfu_NumTracks)
	{
		D(("track %ld is out of bounds (number of tracks = %ld)", io->io_Offset, tfu->tfu_NumTracks));

		error = IOERR_BADLENGTH;
		goto out;
	}

	/* The amount of data to be read is limited by the
	 * capabilities of the Amiga hardware. The DSKLEN
	 * register allows for only 16384 16 bit words to be
	 * transferred, which yields 32768 bytes.
	 */
	if(io->io_Length > 32768)
	{
		D(("read length %ld is out of bounds (maximum is 32768)", io->io_Length));

		error = IOERR_BADLENGTH;
		goto out;
	}

	/* The specs call for this function to require chip
	 * memory to write into.
	 */
	if(FLAG_IS_CLEAR(TypeOfMem(io->io_Data), MEMF_CHIP))
	{
		D(("buffer to be filled at 0x%08lx does not lie in chip memory range", io->io_Data));

		error = IOERR_BADADDRESS;
		goto out;
	}

	D(("read (raw!) %ld bytes from offset %ld (track %ld) to 0x%08lx",
		io->io_Length,
		io->io_Offset,
		io->io_Offset / tfu->tfu_TrackDataSize,
		io->io_Data));

	/* A read operation always enables the motor
	 * if it's not currently running.
	 */
	if(NOT tfu->tfu_MotorEnabled)
		SHOWMSG("turning the motor on");

	tfu->tfu_MotorEnabled = TRUE;

	/* Do we need to read anything at all? */
	if(io->io_Length > 0)
	{
		LONG which_track = io->io_Offset;
		const BYTE * source = tfu->tfu_TrackData;
		BYTE * destination = io->io_Data;
		struct mfm_code_context * mcc = tfu->tfu_MFMCodeContext;
		int sector;
		int start_position;
		const BYTE * encoded_source = mcc->mcc_data;
		int from, to, from_length, to_length;

		ASSERT( FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS );

		ASSERT( NOT multiplication_overflows(which_track, tfu->tfu_TrackDataSize) );
		ASSERT( tfu->tfu_TrackDataSize > 0 );
		ASSERT( 0 <= which_track && which_track < tfu->tfu_NumTracks );
		ASSERT( which_track * tfu->tfu_TrackDataSize < tfu->tfu_FileSize );

		/* We may have to write back the changes currently
		 * in the track buffer first.
		 */
		if(which_track != tfu->tfu_CurrentTrackNumber)
		{
			error = read_track_data(tfu, which_track);
			if(error != OK)
			{
				D(("couldn't read the track data, error=%ld", error));
				goto out;
			}
		}

		/* Encode all the track sectors in sequence. */
		reset_mfm_code_context(mcc);

		ASSERT( mcc->mcc_num_sectors == tfu->tfu_TrackDataSize / TD_SECTOR );

		for(sector = 0 ; sector < mcc->mcc_num_sectors ; sector++)
		{
			mfm_encode_sector(mcc, which_track, sector, mcc->mcc_num_sectors - sector, source);

			source += TD_SECTOR;
		}

		/* How much data we have, and how much we should deliver
		 * to the user. If the user requests more data than we
		 * can provided, we'll just copy what we have again.
		 */
		from_length = mcc->mcc_data_size + mcc->mcc_sector_gap_size;
		to_length = io->io_Length;

		/* The length must be even since the Amiga hardware will
		 * transfer 16 bit words. So we round up to the next
		 * even number of bytes, if necessary.
		 */
		if((to_length % 2) > 0)
		{
			to_length++;
			if(to_length > 32768)
				to_length = 32768;
		}

		if(to_length <= from_length)
			D(("will copy %ld bytes of data", to_length));
		else
			D(("will copy %ld bytes of data; %ld bytes are requested, and the data will be repeated for the copy", from_length, to_length));

		/* If word sync is requested we start with a random sector. */
		if (FLAG_IS_SET(io->io_Flags, IOTDF_WORDSYNC))
		{
			/* If index sync is requested we'll start near the
			 * top of the track.
			 */
			if(FLAG_IS_SET(io->io_Flags, IOTDF_INDEXSYNC))
			{
				SHOWMSG("both word sync and index sync requested: we start with sector #0");

				start_position = 0;
			}
			/* Otherwise we pick a random sector number instead. */
			else
			{
				int start_sector;

				/* Actually, it's a pseudo-random sector number... */
				tfu->tfu_PRNGState = xor_shift_32(tfu->tfu_PRNGState);

				start_sector = tfu->tfu_PRNGState % mcc->mcc_num_sectors;

				D(("word sync requested: we start with sector #%ld", start_sector));

				start_position = mcc->mcc_sector_size * start_sector;

				/* Word sync means that the first 32 bit word of the
				 * sector will be skipped and reading begins with the
				 * sector header sync word.
				 */
				start_position += sizeof(ULONG);
			}
		}
		/* Otherwise we either obey the index sync requirement, if
		 * given, or we start with a random buffer position instead.
		 * For good measure, we also pick a random bit position at
		 * which the read operation will begin.
		 */
		else
		{
			/* If index sync is requested we'll start near the
			 * top of the track.
			 */
			if(FLAG_IS_SET(io->io_Flags, IOTDF_INDEXSYNC))
			{
				SHOWMSG("index sync requested: we start with sector #0");

				start_position = 0;
			}
			else
			{
				/* Make that a pseudo-random start position, which can
				 * lie within the sector gap.
				 */
				tfu->tfu_PRNGState = xor_shift_32(tfu->tfu_PRNGState);

				ASSERT( from_length > 0 );

				start_position = tfu->tfu_PRNGState % from_length;

				if(start_position < mcc->mcc_data_size)
					D(("we start with a random position around sector #%ld", start_position / mcc->mcc_sector_size));
				else
					SHOWMSG("we start with a random position in the sector gap");
			}

			/* Simulate the effect of the disk read head starting
			 * to look at the bit stream at a random position.
			 */
			tfu->tfu_PRNGState = xor_shift_32(tfu->tfu_PRNGState);

			D(("rotating the data by %ld bits", tfu->tfu_PRNGState % 32));

			mfm_encode_rotate_data(mcc, tfu->tfu_PRNGState);
		}

		D(("start position = %ld, track size in bytes = %ld", start_position, from_length));

		ASSERT( start_position < from_length );

		/* Copy the encoded data to the read buffer, with the data being
		 * repeated if more data is requested than is available.
		 */
		for(to = 0, from = start_position ; to < to_length ; to++)
		{
			destination[to] = encoded_source[from];

			/* Wrap around if we exhausted the data read so far. */
			if(++from == from_length)
				from = 0;
		}
	}

	SHOWMSG("that went well");

	error = OK;

 out:

	/* This command never provides a useful io_Actual value. */
	io->io_Actual = 0;

	RETURN(error);
	return(error);
}

#endif /* ENABLE_MFM_ENCODING */

/****************************************************************************/

/****** trackfile.device/TD_REMCHANGEINT *************************************
*
*   NAME
*	TD_REMCHANGEINT -- remove a disk change software interrupt handler.
*
*   FUNCTION
*	This command removes a disk change software interrupt added
*	by a previous use of TD_ADDCHANGEINT.
*
*   IO REQUEST INPUT
*	The same IO request used for TD_ADDCHANGEINT.
*
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_REMCHANGEINT
*	io_Flags	0 or IOF_QUICK
*	io_Length	sizeof(struct Interrupt)
*	io_Data		pointer to Interrupt structure
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*
*   SEE ALSO
*	TD_ADDCHANGEINT, <devices/trackdisk.h>, <exec/errors.h>
*
******************************************************************************
*/

static LONG
td_remchangeint(struct IOStdReq * which_io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)which_io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	struct IOStdReq * io;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	error = check_io_request_size(which_io);
	if(error != OK)
		goto out;

	error = check_io_request_data_and_length(which_io, sizeof(struct Interrupt), 0, 0);
	if(error != OK)
		goto out;

	Forbid();

	/* We need to remove the IOStdReq added by the TD_ADDCHANGEINT
	 * command, but we cannot use the easy way to just call Remove().
	 * So we check the entire list to find it there, and only if
	 * we know that it's there can we remove it.
	 *
	 * This procedure follows the trackdisk.device documentation
	 * and which is also why the TD_REMCHANGEINT command
	 * must be an "immediate" command executed on the context
	 * of the calling Task.
	 *
	 * Note that this "paranoid" approach to making sure that the
	 * I/O request is in fact known is not just a precaution, it
	 * actually guards against the possibility that the Task or
	 * Process which used TD_ADDCHANGEINT is blocked by a higher
	 * priority Task or Process, and TD_REMCHANGEINT cannot find
	 * that I/O request just yet.
	 */
	for(io = (struct IOStdReq *)tfu->tfu_ChangeIntList.mlh_Head ;
	    io->io_Message.mn_Node.ln_Succ != NULL ;
	    io = (struct IOStdReq *)io->io_Message.mn_Node.ln_Succ)
	{
		if(io == which_io)
		{
			Remove(&io->io_Message.mn_Node);

			/* Let's be paranoid. */
			io->io_Message.mn_Node.ln_Succ = NULL;
			io->io_Message.mn_Node.ln_Pred = NULL;

			break;
		}
	}

	Permit();

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/TD_REMOVE *************************************
*
*   NAME
*	TD_REMOVE - installs/removes the legacy disk change
*	        software interrupt handler.
*
*   FUNCTION
*	This command installs the legacy disk change software
*	interrupt if io_Data is not NULL and will remove it again
*	if io_Data is NULL. Note that there is only one single
*	legacy disk change software interrupt per drive.
*
*	The legacy disk change software interrupt coexists with
*	the disk change software interrupts installed/removed
*	by the TD_ADDCHANGEINT and TD_REMCHANGEINT commands,
*	respectively.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_REMOVE
*	io_Flags	0 or IOF_QUICK
*	io_Data		pointer to Interrupt structure or NULL
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*
*   NOTES
*	This is a legacy command, which can be used by only one
*	single client, first come, first serve. Prior to the
*	addition of the TD_ADDCHANGEINT and TD_REMCHANGEINT commands
*	this client was the AmigaDOS default file system.
*
*	Do not use TD_REMOVE. Always use the TD_ADDCHANGEINT and
*	TD_REMCHANGEINT commands instead.
*
*   SEE ALSO
*	TD_ADDCHANGEINT, TD_REMCHANGEINT, <devices/trackdisk.h>,
*	<exec/errors.h>
*
**********************************************************************
*/

static LONG
td_remove(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	/* Only change the legacy disk change interrupt if the
	 * I/O request actually contains the io_Data field.
	 * But don't complain about the I/O request being too
	 * short since the software which uses TD_REMOVE
	 * cannot be expected to check for errors, or handle
	 * those errors if they were flagged.
	 */

	/* Note: The v40 ffs will have io->io_Message.mn_Length=0 for all
	 *       quick and non-quick command, which is more than just
	 *       unfortunate...
	 */
	if(io->io_Message.mn_Length == 0 || io->io_Message.mn_Length >= sizeof(*io))
	{
		Forbid();

		D(("setting remove int to 0x%08lx (was 0x%08lx)", io->io_Data, tfu->tfu_RemoveInt));

		tfu->tfu_RemoveInt = io->io_Data;

		Permit();
	}

	RETURN(OK);
	return(OK);
}

/****************************************************************************/

/****** trackfile.device/TD_SEEK *********************************************
*
*   NAME
*	TD_SEEK -- control positioning of the drive heads.
*
*   FUNCTION
*	This command has no practical effect, even if it succeeds without
*	producing an error.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	TD_SEEK
*	io_Flags	0
*	io_Offset	byte offset from the start of the disk describing
*			where to move the head to.
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*
******************************************************************************
*/

static LONG
td_seek(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	if(NOT unit_medium_is_present(tfu))
	{
		SHOWMSG("no disk present");

		error = TDERR_DiskChanged;
		goto out;
	}

	error = check_extended_command(io);
	if(error != OK)
		goto out;

	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	error = check_offset(io);
	if(error != OK)
		goto out;

	if(NOT tfu->tfu_MotorEnabled)
		SHOWMSG("turning the motor on");

	tfu->tfu_MotorEnabled = TRUE;

	ASSERT( tfu->tfu_TrackDataSize != 0 );

	tfu->tfu_Unit.tdu_CurrTrk = io->io_Offset / tfu->tfu_TrackDataSize;

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/****** trackfile.device/NSCMD_DEVICEQUERY *********************************
*
*   NAME
*	NSCMD_DEVICEQUERY - Obtain information about the type and
*	        capabilities of an Amiga device driver.
*
*   FUNCTION
*	This command will provide information about the device
*	driver type and which commands it supports.
*
*   IO REQUEST INPUT
*	io_Device	preset by the call to OpenDevice()
*	io_Unit		preset by the call to OpenDevice()
*	io_Command	NSCMD_DEVICEQUERY
*	io_Length	sizeof(struct NSDeviceQueryResult)
*	io_Flags	0 or IOF_QUICK
*	io_Data		pointer to NSDeviceQueryResult structure
*
*   IO REQUEST RESULT
*	io_Error - 0 for success, or an error code as defined in
*	           <devices/trackdisk.h> or <exec/errors.h>
*
*   SEE ALSO
*	<devices/newstyle.h>, <devices/trackdisk.h>, <exec/errors.h>
*
**********************************************************************
*/

static LONG
nscmd_devicequery(struct IOStdReq * io)
{
	STATIC UWORD supported_commands[] =
	{
		CMD_CLEAR,
		CMD_READ,
		CMD_START,
		CMD_STOP,
		CMD_UPDATE,
		CMD_WRITE,
		ETD_CLEAR,
		ETD_FORMAT,
		ETD_MOTOR,
		ETD_READ,
		ETD_SEEK,
		ETD_UPDATE,
		ETD_WRITE,
		TD_ADDCHANGEINT,
		TD_CHANGENUM,
		TD_CHANGESTATE,
		TD_EJECT,
		TD_FORMAT,
		TD_GETDRIVETYPE,
		TD_GETGEOMETRY,
		TD_GETNUMTRACKS,
		TD_MOTOR,
		TD_PROTSTATUS,
	#if defined(ENABLE_MFM_ENCODING)

		TD_RAWREAD,
		ETD_RAWREAD,

	#endif /* ENABLE_MFM_ENCODING */
		TD_REMCHANGEINT,
		TD_REMOVE,
		TD_SEEK,
		NSCMD_DEVICEQUERY,

		0
	};

	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	struct NSDeviceQueryResult * qr;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	error = check_io_request_size(io);
	if(error != OK)
		goto out;

	error = check_io_request_data_and_length(io, sizeof(*qr), 0, 0);
	if(error != OK)
		goto out;

	qr = io->io_Data;

	memset(qr, 0, sizeof(*qr));

	qr->nsdqr_SizeAvailable		= sizeof(*qr);
	qr->nsdqr_DeviceType		= NSDEVTYPE_TRACKDISK;
	qr->nsdqr_DeviceSubType		= 0;
	qr->nsdqr_SupportedCommands	= supported_commands;

	io->io_Actual = qr->nsdqr_SizeAvailable;

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/* This invokes the actual device function which performs the
 * command given.
 */
VOID
perform_io(struct IOStdReq * io)
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	struct TrackFileDevice * tfd = tfu->tfu_Device;
	LONG error;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	switch(io->io_Command)
	{
		case CMD_CLEAR:
		case ETD_CLEAR:

			error = cmd_clear(io);
			break;

		case CMD_READ:
		case ETD_READ:

			error = cmd_read(io);
			break;

		case CMD_START:

			error = cmd_start(io);
			break;

		case CMD_STOP:

			error = cmd_stop(io);
			break;

		case CMD_UPDATE:
		case ETD_UPDATE:

			error = cmd_update(io);
			break;

		case CMD_WRITE:
		case ETD_WRITE:

			error = cmd_write(io);
			break;

		case TD_ADDCHANGEINT:

			error = td_addchangeint(io);
			break;

		case TD_CHANGENUM:

			error = td_changenum(io);
			break;

		case TD_CHANGESTATE:

			error = td_changestate(io);
			break;

		case TD_EJECT:

			error = td_eject(io);
			break;

		case TD_FORMAT:
		case ETD_FORMAT:

			error = td_format(io);
			break;

		case TD_GETDRIVETYPE:

			error = td_getdrivetype(io);
			break;

		case TD_GETGEOMETRY:

			error = td_getgeometry(io);
			break;

		case TD_GETNUMTRACKS:

			error = td_getnumtracks(io);
			break;

		case TD_MOTOR:
		case ETD_MOTOR:

			error = td_motor(io);
			break;

		case TD_PROTSTATUS:

			error = td_protstatus(io);
			break;

	#if defined(ENABLE_MFM_ENCODING)

		case TD_RAWREAD:
		case ETD_RAWREAD:

			error = td_rawread(io);
			break;

	#endif /* ENABLE_MFM_ENCODING */

		case TD_REMCHANGEINT:

			error = td_remchangeint(io);
			break;

		case TD_REMOVE:

			error = td_remove(io);
			break;

		case TD_SEEK:
		case ETD_SEEK:

			error = td_seek(io);
			break;

		case NSCMD_DEVICEQUERY:

			error = nscmd_devicequery(io);
			break;

		default:

			D(("unsupported command 0x%04lx", io->io_Command));

			error = IOERR_NOCMD;
			break;
	}

	io->io_Error = error;

	LEAVE();
}

/****************************************************************************/

/* Check if the command associated with the I/O request can be performed
 * on the context of the calling Task or Process. Returns TRUE if so,
 * FALSE otherwise. If this function returns FALSE, then the processing
 * of the command will have to be delegated to the unit Process via
 * PutMsg().
 */
BOOL
is_immediate_command(const struct IORequest * io)
{
	BOOL is_immediate;

	ASSERT( io != NULL );

	switch(io->io_Command)
	{
		/* No need to restrict the NSCMD_DEVICEQUERY command to
		 * non-immediate mode.
		 */
		case NSCMD_DEVICEQUERY:

		/* These six are exactly the same commands which trackdisk.device
		 * supports in immediate mode.
		 */
		case CMD_START:
		case TD_CHANGENUM:
		case TD_CHANGESTATE:
		case TD_GETDRIVETYPE:
		case TD_GETNUMTRACKS:
		case TD_REMCHANGEINT:

			is_immediate = TRUE;
			break;

		default:

			is_immediate = FALSE;
			break;
	}

	return(is_immediate);
}

/****************************************************************************/

/* Check if the command associated with the I/O request is known to
 * this device and can be performed. Returns TRUE if so, FALSE
 * otherwise.
 *
 * While you could leave this test to the perform_io() function, it is
 * more convenient to it early and quickly in the BeginIO() function.
 */
BOOL
is_known_command(const struct IORequest * io)
{
	BOOL is_known;

	ASSERT( io != NULL );

	switch(io->io_Command)
	{
		case CMD_CLEAR:
		case CMD_READ:
		case CMD_UPDATE:
		case CMD_WRITE:
		case ETD_CLEAR:
		case ETD_FORMAT:
		case ETD_MOTOR:
		case ETD_READ:
		case ETD_SEEK:
		case ETD_UPDATE:
		case ETD_WRITE:
		case TD_ADDCHANGEINT:
		case TD_CHANGENUM:
		case TD_CHANGESTATE:
		case TD_EJECT:
		case TD_FORMAT:
		case TD_GETDRIVETYPE:
		case TD_GETGEOMETRY:
		case TD_GETNUMTRACKS:
		case TD_MOTOR:
		case TD_PROTSTATUS:
		case TD_REMCHANGEINT:
		case TD_REMOVE:
		case TD_SEEK:
		case NSCMD_DEVICEQUERY:

	#if defined(ENABLE_MFM_ENCODING)

		case TD_RAWREAD:
		case ETD_RAWREAD:

	#endif /* ENABLE_MFM_ENCODING */

			is_known = TRUE;
			break;

		default:

			D(("unsupported command 0x%04lx", io->io_Command));

			is_known = FALSE;
			break;
	}

	return(is_known);
}

/****************************************************************************/

/* Mark both the buffer contents as invalid, as well as
 * the number of the track which the buffer's contents
 * used to be associated with.
 */
VOID
mark_track_buffer_as_invalid(struct TrackFileUnit * tfu)
{
	ASSERT( tfu != NULL );

	tfu->tfu_TrackDataChanged	= FALSE;
	tfu->tfu_CurrentTrackNumber	= -1;
}

/****************************************************************************/

/* Mark the motor as no longer running and also update the
 * track number in the public unit to read as invalid.
 */
VOID
turn_off_motor(struct TrackFileUnit * tfu)
{
	ASSERT( tfu != NULL );

	tfu->tfu_MotorEnabled		= FALSE;
	tfu->tfu_Unit.tdu_CurrTrk	= (UWORD)-1;
}
