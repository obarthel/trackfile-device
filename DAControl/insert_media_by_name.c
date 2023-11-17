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

#include <resources/filesysres.h>

#include <dos/dos.h>

/****************************************************************************/

#define __USE_SYSBASE
#include <proto/exec.h>

#include <proto/dos.h>

#include <proto/trackfile.h>

/****************************************************************************/

#include <string.h>

/****************************************************************************/

#include "macros.h"
#include "global_data.h"
#include "mount_floppy_file.h"
#include "insert_media_by_name.h"
#include "start_unit.h"
#include "cache.h"
#include "tools.h"

/****************************************************************************/

#include "assert.h"

/****************************************************************************/

/* Open the given disk image file, then if necessary spin up a unit
 * and finally mount the file system, if necessary. Returns an
 * error code (error message will have been printed), if things
 * don't work out.
 */
static LONG
open_and_mount_disk_image_file(
	struct GlobalData *	gd,
	BOOL				verbose,
	BOOL				write_protected,
	BOOL				enable_cache,
	BOOL				prefill_cache,
	LONG				cache_size,
	STRPTR				file_name,
	LONG				unit,
	BOOL				use_next_available_unit,
	int					num_cylinders,
	int					num_sectors,
	STRPTR				dos_device_name)
{
	USE_DOS(gd);
	USE_TRACKFILE(gd);

	TEXT error_message[256];
	LONG error;
	BPTR file;

	ENTER();

	ASSERT( gd != NULL && file_name != NULL );

	file = Open(file_name, MODE_OLDFILE);
	if(file == ZERO)
	{
		error = IoErr();

		Error(gd, "Could not open \"%s\" (%s).",
			file_name, get_error_message(gd, error, error_message, sizeof(error_message)));

		goto out;
	}

	ASSERT( use_next_available_unit || unit >= 0 );

	/* We'll need a new unit to insert the medium. */
	if(use_next_available_unit)
	{
		SHOWMSG("use the next unit available to start a unit");

		error = start_unit(gd,
			verbose,
			-1,		/* <- Invalid unit number */
			TRUE,	/* <- Use the next available unit */
			cache_size,
			num_cylinders,
			num_sectors,
			&unit,
			dos_device_name);

		if(error != OK)
		{
			SHOWMSG("could not start unit");
			goto out;
		}

		D(("will use unit %ld", unit));
	}

	if(dos_device_name != NULL)
		D(("AmigaDOS device name for unit %ld = %s", unit, dos_device_name));
	else
		D(("no AmigaDOS device name for unit %ld was provided", unit));

	if(verbose)
	{
		if(dos_device_name != NULL && dos_device_name[0] != '\0')
			Printf("Inserting disk image file \"%s\" into \"%s:\" (unit %ld).\n", file_name, dos_device_name, unit);
		else
			Printf("Inserting disk image file \"%s\" into unit %ld.\n", file_name, unit);
	}

	error = TFInsertMediaTags(unit,
		TF_ImageFileHandle,	file,
		TF_WriteProtected,	write_protected,

		#if defined(ENABLE_CACHE)
			TF_EnableUnitCache,		enable_cache,
			TF_PrefillUnitCache,	prefill_cache,
		#endif /* ENABLE_CACHE */
	TAG_DONE);

	if(error != OK)
	{
		SHOWMSG("could not insert disk image file");

		Error(gd, "Could not insert disk image file \"%s\" (%s).",
			file_name, get_error_message(gd, error, error_message, sizeof(error_message)));

		goto out;
	}

	/* The trackfile.device unit is now in control of the file. */
	file = ZERO;

 out:

	if(file != ZERO)
		Close(file);

	RETURN(error);
	return(error);
}

/****************************************************************************/

/* Find media files which are suitable for use with trackfile.device
 * and load/mount these as needed.
 */
LONG
insert_media_by_name(
	struct GlobalData *	gd,
	BOOL				quiet,
	BOOL				verbose,
	BOOL				ignore,
	BOOL				write_protected,
	BOOL				enable_cache,
	BOOL				prefill_cache,
	LONG				cache_size,
	struct AnchorPath *	ap,
	STRPTR *			files,
	LONG				unit,
	BOOL				use_next_available_unit,
	int					num_cylinders,
	int					num_sectors,
	STRPTR				dos_device_name,	/* <- This may get modified */
	LONG				max_matches)
{
	USE_DOS(gd);
	USE_EXEC(gd);
	USE_TRACKFILE(gd);

	struct Process * this_process = (struct Process *)FindTask(NULL);
	APTR old_window_pointer = this_process->pr_WindowPtr;
	STRPTR file_name;
	BOOL matched = FALSE;
	LONG num_matches = 0;
	TEXT error_message[256];
	LONG error;
	BPTR old_cur_dir;
	BPTR lock = ZERO;

	ENTER();

	ASSERT( gd != NULL && ap != NULL && files != NULL );

	while((file_name = (*files++)) != NULL)
	{
		if(CheckSignal(SIGBREAKF_CTRL_C))
		{
			SHOWMSG("aborted.");

			error = ERROR_BREAK;
			goto out;
		}

		D(("next file name = '%s'", file_name));

		/* Let's be (sort of) clever: if the user gave the
		 * name of a file which so happens to feature wildcard
		 * characters in its name or path, and we can actually
		 * access it as such, then don't even try to interpret
		 * it as a wildcard pattern.
		 */
		SHOWMSG("checking if this directory entry exists");

		lock = Lock(file_name, SHARED_LOCK);
		if(lock != ZERO)
		{
			SHOWMSG("name matches an existing directory entry");

			/* Is this a file? */
			if(CANNOT Examine(lock, &ap->ap_Info))
			{
				error = IoErr();

				if(NOT quiet)
				{
					Error(gd, "Could not examine \"%s\" (%s).",
						file_name, get_error_message(gd, error, error_message, sizeof(error_message)));
				}

				goto out;
			}

			/* We no longer need this. */
			UnLock(lock);
			lock = ZERO;

			if(NOT FIB_IS_FILE(&ap->ap_Info))
			{
				if(NOT quiet)
					Error(gd, "\"%s\" is not a disk image file.", file_name);

				if(ignore)
					continue;

				error = ERROR_OBJECT_WRONG_TYPE;
				goto out;
			}

			/* Is the size acceptable for a disk image file? */
			if(TFExamineFileSize(ap->ap_Info.fib_Size) == TFEFS_Unsupported)
			{
				D(("file size %ld is unsuitable", ap->ap_Info.fib_Size));

				if(NOT quiet)
					Error(gd, "\"%s\" is not a suitable disk image file (size not supported).", file_name);

				if(ignore)
					continue;

				error = ERROR_OBJECT_WRONG_TYPE;
				goto out;
			}

			/* Let's try and open it, then put it to good use. */
			error = open_and_mount_disk_image_file(gd,
				verbose,
				write_protected,
				enable_cache,
				prefill_cache,
				cache_size,
				file_name,
				unit,
				use_next_available_unit,
				num_cylinders,
				num_sectors,
				dos_device_name);

			if(error != OK)
			{
				if(error == ERROR_OBJECT_NOT_FOUND && ignore)
					continue;

				goto out;
			}

			/* If this was invoked through the START/CREATE option,
			 * then we have only one single unit to deal
			 * with.
			 */
			num_matches++;
			if(num_matches == max_matches)
			{
				D(("reached the maximum number of matches allowed (%ld)", num_matches));
				break;
			}

			/* For the next match we'll need another unit. */
			use_next_available_unit = TRUE;

			/* Move on to the next file. */
			continue;
		}
		else
		{
			error = IoErr();

			/* If we cannot find what is called for, we leave the
			 * rest to MatchFirst()/MatchNext(). Otherwise we
			 * better stop here.
			 */
			if(error != ERROR_OBJECT_NOT_FOUND)
			{
				D(("that didn't work, and we seem to have bigger problems (error=%ld)", error));

				Error(gd, "Could not examine \"%s\" (%s).",
					file_name, get_error_message(gd, error, error_message, sizeof(error_message)));

				goto out;
			}
		}

		/* And now we look for wildcard matches. */
		SHOWMSG("try that again with wildcard matching");

		if(matched)
		{
			MatchEnd(ap);

			/* Reinitialize this, since we are going to reuse
			 * the AnchorPath.
			 */
			memset(ap, 0, sizeof(*ap));

			ap->ap_BreakBits = SIGBREAKF_CTRL_C;
		}

		/* The first attempt to match something needs special
		 * attention because failure to find at least one single
		 * match (indicated by ERROR_NO_MORE_ENTRIES) needs to
		 * be reported.
		 */
		error = MatchFirst(file_name, ap);

		matched = TRUE;

		/* If we are in trouble, report the problem and abort. */
		if(error != OK)
		{
			if(NOT quiet)
			{
				TEXT error_message[256];

				Error(gd, "Could not examine \"%s\" (%s).",
					file_name,
					get_error_message(gd, error, error_message, sizeof(error_message)));
			}

			if(error == ERROR_OBJECT_NOT_FOUND && ignore)
				error = OK;

			goto out;
		}

		do
		{
			D(("found '%s' as a match", ap->ap_Info.fib_FileName));

			/* Is this a soft linked object? */
			if(ap->ap_Info.fib_DirEntryType == ST_SOFTLINK)
			{
				SHOWMSG("this is a soft linked object");

				/* Turn off DOS requesters. */
				this_process->pr_WindowPtr = (APTR)-1;

				/* Try to get a lock on the object, which will hopefully
				 * resolve the link.
				 */
				old_cur_dir = CurrentDir(ap->ap_Current->an_Lock);

				lock = Lock(ap->ap_Info.fib_FileName, SHARED_LOCK);
				if(lock != ZERO)
				{
					D_S(struct FileInfoBlock, fib);

					/* Have a closer look at the object. */
					if(Examine(lock, fib))
					{
						/* Copy the type and the file size, if any.
						 * Both are relevant.
						 */
						ap->ap_Info.fib_DirEntryType	= fib->fib_DirEntryType;
						ap->ap_Info.fib_Size			= fib->fib_Size;
					}

					UnLock(lock);
					lock = ZERO;
				}

				CurrentDir(old_cur_dir);

				/* This turns the DOS requesters back on. */
				this_process->pr_WindowPtr = old_window_pointer;
			}

			/* This should not be a directory, or something like it. */
			if(NOT FIB_IS_FILE(&ap->ap_Info))
			{
				SHOWMSG("this is not a file");

				if(NOT quiet)
					Error(gd, "\"%s\" is not a disk image file.", ap->ap_Info.fib_FileName);

				if(ignore)
					continue;

				error = ERROR_OBJECT_WRONG_TYPE;
				goto out;
			}

			/* Is the size not acceptable for a disk image file? */
			if(TFExamineFileSize(ap->ap_Info.fib_Size) == TFEFS_Unsupported)
			{
				D(("file size %ld is unsuitable", ap->ap_Info.fib_Size));

				if(NOT quiet)
					Error(gd, "\"%s\" is not a suitable disk image file (size is wrong).", ap->ap_Info.fib_FileName);

				if(ignore)
					continue;

				error = ERROR_OBJECT_WRONG_TYPE;
				goto out;
			}

			old_cur_dir = CurrentDir(ap->ap_Current->an_Lock);

			error = open_and_mount_disk_image_file(gd,
				verbose,
				write_protected,
				enable_cache,
				prefill_cache,
				cache_size,
				ap->ap_Info.fib_FileName,
				unit,
				use_next_available_unit,
				num_cylinders,
				num_sectors,
				dos_device_name);

			CurrentDir(old_cur_dir);

			if(error != OK)
			{
				if(error == ERROR_OBJECT_NOT_FOUND && ignore)
					continue;

				goto out;
			}

			/* If this was invoked through the START/CREATE option,
			 * then we have only one single unit to deal
			 * with.
			 */
			num_matches++;
			if(num_matches == max_matches)
			{
				D(("reached the maximum number of matches allowed (%ld)", num_matches));
				break;
			}

			/* For the next match we'll need another unit. */
			use_next_available_unit = TRUE;
		}
		while((error = MatchNext(ap)) == OK);

		if(error == ERROR_NO_MORE_ENTRIES)
			error = OK;

		if(error != OK)
		{
			if(error != ERROR_BREAK)
				D(("that didn't work, and we seem to have bigger problems (IoErr=%ld)", error));

			goto out;
		}

		if(num_matches == max_matches)
			break;
	}

	SHOWMSG("finished so far...");

	error = OK;

 out:

	if(matched)
		MatchEnd(ap);

	if(lock != ZERO)
		UnLock(lock);

	RETURN(error);
	return(error);
}
