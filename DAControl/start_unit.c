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

#include <dos/dosextens.h>

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
#include "start_unit.h"
#include "cache.h"
#include "tools.h"

/****************************************************************************/

#include "assert.h"

/****************************************************************************/

/* Start a unit, if necessary mounting it as well. This will
 * fill in the name of AmigaDOS file system device and
 * the file system process address.
 */
LONG
start_unit(
	struct GlobalData *	gd,
	BOOL				verbose,
	LONG				unit,
	BOOL				use_next_available_unit,
	LONG				cache_size,
	int					num_cylinders,
	int					num_sectors_per_track,
	LONG *				which_unit_ptr,
	STRPTR				dos_device_name)	/* <- This may be modified if not NULL */
{
	struct DosList * first_dol;
	struct DosList * dol;
	LONG new_unit;
	struct fs_startup_msg fsm;
	BOOL found_device = FALSE;
	BOOL dos_list_is_locked = FALSE;
	LONG drive_type;
	TEXT error_message[256];
	LONG error;

	USE_EXEC(gd);
	USE_DOS(gd);
	USE_TRACKFILE(gd);

	ENTER();

	ASSERT( use_next_available_unit || unit >= 0 );
	ASSERT( num_cylinders > 0 );
	ASSERT( num_sectors_per_track > 0 );

	if(dos_device_name != NULL)
		SHOWMSG("need to return the name of the file system device, if possible");
	else
		SHOWMSG("no need to return the name of the file system device");

	/* We don't know the AmigaDOS device name yet, so
	 * we provide a blank just in case.
	 */
	if(use_next_available_unit && dos_device_name != NULL)
		(*dos_device_name) = '\0';

	/* Note that we support only 3.5" disks here. */
	drive_type = (num_sectors_per_track == 22) ? DRIVE3_5_150RPM : DRIVE3_5;

	if (drive_type == DRIVE3_5)
		SHOWMSG("drive type = DRIVE3_5");
	else if (drive_type == DRIVE3_5_150RPM)
		SHOWMSG("drive type = DRIVE3_5_150RPM");
	else
		SHOWMSG("drive type = DRIVE5_25");

	/* Ask the device to rustle up a unit and get
	 * it ready for use.
	 */
	new_unit = TFStartUnitTags(use_next_available_unit ? TFSU_NextAvailableUnit : unit,
		TF_DriveType,		drive_type,
		TF_EnableChecksums,	gd->gd_UseChecksums,

		#if defined(ENABLE_CACHE)
			TF_MaxCacheMemory,	cache_size,
		#endif /* ENABLE_CACHE */
	TAG_DONE);

	if(new_unit < 0)
	{
		/* The "unit number" is actually an error code. */
		error = new_unit;

		get_error_message(gd, error, error_message, sizeof(error_message));

		if(use_next_available_unit)
		{
			Error(gd, "Could not start a new unit (%s).", error_message);
		}
		else
		{
			if(dos_device_name != NULL && (*dos_device_name) != '\0')
				Error(gd, "Could not start unit %ld (\"%s\") (%s).", unit, dos_device_name, error_message);
			else
				Error(gd, "Could not start unit %ld (%s).", unit, error_message);
		}

		goto out;
	}

	/* Is this unit already used by an active mount?
	 * If so, we're going to reuse it. Otherwise we will
	 * have to make our own mount.
	 */
	first_dol = dol = LockDosList(LDF_DEVICES|LDF_READ);
	dos_list_is_locked = TRUE;

	while((dol = NextDosEntry(dol, LDF_DEVICES)) != NULL)
	{
		if(CANNOT decode_file_sys_startup_msg(gd, dol->dol_misc.dol_handler.dol_Startup, &fsm))
			continue;

		if(strcmp(fsm.fsm_device_name, TrackFileBase->lib_Node.ln_Name) != SAME)
			continue;

		/* Is this the unit we are supposed to start?
		 * Then our job is already done...
		 */
		if(fsm.fsm_device_unit == new_unit)
		{
			/* Remember the AmigaDOS device name. */
			if(dos_device_name != NULL)
			{
				const TEXT * device_name = BADDR(dol->dol_Name);
				int device_name_len;

				device_name_len = device_name[0];

				CopyMem((APTR)&device_name[1], dos_device_name, device_name_len);
				dos_device_name[device_name_len] = '\0';

				D(("file system device name = '%s'", dos_device_name));
			}

			found_device = TRUE;
			break;
		}
	}

	/* We may have to mount the AmigaDOS device all on our own. */
	if(NOT found_device)
	{
		TEXT new_dos_device_name[260];
		size_t new_dos_device_name_len;
		struct DeviceNode * dn = NULL;
		BOOL device_name_already_in_use;

		SHOWMSG("didn't find a suitable AmigaDOS file system device");

		/* The name of the file system device must correspond
		 * to the unit number.
		 */
		local_snprintf(gd, new_dos_device_name, sizeof(new_dos_device_name), "DA%ld", new_unit);
		new_dos_device_name_len = strlen(new_dos_device_name);

		D(("trying to create new AmigaDOS file system device as \"%s\".", new_dos_device_name));

		/* Is this AmigaDOS file system device name
		 * already in use?
		 */
		device_name_already_in_use = (FindDosEntry(first_dol, new_dos_device_name, LDF_DEVICES) != NULL);

		UnLockDosList(LDF_DEVICES|LDF_READ);
		dos_list_is_locked = FALSE;

		if(device_name_already_in_use)
		{
			SHOWMSG("cannot find an unused DA<xxxx>: device name");

			error = ERROR_DEVICE_NOT_MOUNTED;

			Error(gd, "The unit could not be mounted because no device name was available for use.");
			goto out;
		}

		/* This adds the file system mount information, but it
		 * does not start the file system just yet. Note that
		 * we do not need to keep the DOS list locked for the
		 * mount operation to succeeed.
		 */
		error = mount_floppy_file(gd, new_dos_device_name, new_unit, num_cylinders, num_sectors_per_track, &dn);
		if(error != OK)
		{
			SHOWMSG("mount attempt failed");

			Error(gd, "The unit could not be mounted as \"%s:\" (%s).",
				new_dos_device_name, get_error_message(gd, error, error_message, sizeof(error_message)));

			goto out;
		}

		/* Make sure that we do not unload the
		 * custom file system for this file system
		 * device by mistake.
		 *
		 * Also say a word if the custom file
		 * system could not be used.
		 */
		if(gd->gd_LoadedFileSystem != ZERO)
		{
			if(gd->gd_LoadedFileSystem == dn->dn_SegList)
			{
				if(verbose)
				{
					ASSERT( gd->gd_WBenchMsg == NULL );

					Printf("Mounting device %s: using custom file system \"%s\".\n",
						new_dos_device_name, gd->gd_LoadedFileSystemName);
				}
			}
			else
			{
				if(verbose)
				{
					ASSERT( gd->gd_WBenchMsg == NULL );

					Printf("Cannot mount device %s: using custom file system \"%s\" (overridden by %s).\n",
						new_dos_device_name, gd->gd_LoadedFileSystemName, FSRNAME);
				}
			}
		}

		/* To wrap this up, we now start the file system. We do this
		 * while we still have "trackfile.device" opened as otherwise
		 * the device may get expunged when we exit. This way the
		 * file system will open the unit and keep the device in
		 * business.
		 */
		local_strlcat(new_dos_device_name, ":", sizeof(new_dos_device_name));

		if(DeviceProc(new_dos_device_name) == NULL)
		{
			SHOWMSG("could not start file system");

			error = IoErr();
			goto out;
		}

		/* Retain the device name, forget about the colon character
		 * just added to make DeviceProc() work.
		 */
		new_dos_device_name[new_dos_device_name_len] = '\0';

		/* Remember the AmigaDOS file system device name. */
		if(dos_device_name != NULL)
			local_strlcpy(dos_device_name, new_dos_device_name, sizeof(new_dos_device_name));

		D(("file system device name = '%s'", new_dos_device_name));
	}

	/* Remember which unit we ended up using. */
	if(which_unit_ptr != NULL)
		(*which_unit_ptr) = new_unit;

	error = OK;

 out:

	if(dos_list_is_locked)
		UnLockDosList(LDF_DEVICES|LDF_READ);

	/* Don't return a unit number which might be
	 * mistaken for something valid in case an
	 * error occured.
	 */
	if(error != OK && which_unit_ptr != NULL)
		(*which_unit_ptr) = -1;

	return(error);
}
