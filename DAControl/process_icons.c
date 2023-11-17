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

#include <exec/memory.h>
#include <exec/errors.h>

#include <resources/filesysres.h>

#include <dos/filehandler.h>

#include <workbench/icon.h>

/****************************************************************************/

#define __USE_SYSBASE
#include <proto/exec.h>

#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/icon.h>

#include <proto/trackfile.h>

/****************************************************************************/

#include <string.h>

/****************************************************************************/

#include "macros.h"
#include "global_data.h"
#include "start_unit.h"
#include "insert_media_by_name.h"
#include "process_icons.h"
#include "cache.h"
#include "tools.h"

/****************************************************************************/

#include "assert.h"

/****************************************************************************/

/* A somewhat simpler, specialized function for starting units and loading
 * disk image files via Workbench project icons.
 */
LONG ASM
process_icons(REG(a0, struct GlobalData * gd))
{
	const LONG num_args = gd->gd_WBenchMsg->sm_NumArgs;
	const struct WBArg * args = gd->gd_WBenchMsg->sm_ArgList;
	D_S(struct FileInfoBlock, fib);
	struct Library * IconBase = NULL;
	struct Library * TrackFileBase = NULL;
	struct DiskObject * icon = NULL;
	STRPTR usechecksums_system_option;
	STRPTR write_protect_option;
	STRPTR file_system_option;
	TEXT error_message[256];
	BOOL write_protected;
	BOOL enable_cache = FALSE;
	BOOL prefill_cache = FALSE;
	LONG cache_size = 0;
	STRPTR choices;
	BPTR file = ZERO;
	LONG error;
	LONG unit;
	LONG i;

	/* The default disk type is an Amiga 3.5" double density disk. */
	int num_cylinders = NUMCYLS;
	int num_sectors = NUMSECS;

	USE_EXEC(gd);
	USE_DOS(gd);
	USE_UTILITY(gd);

	ENTER();

	D(("launched from Workbench, %ld argument(s)", num_args));

	/* Open trackfile.device now. */
	error = OpenDevice(TRACKFILENAME, TFUNIT_CONTROL, (struct IORequest *)&gd->gd_TrackFileDevice, 0);
	if(error != OK)
	{
		D(("that didn't work (error=%ld)", error));

		Error(gd, "Could not open %s.", TRACKFILENAME);

		goto out;
	}

	/* Version 2.15 of trackfile.device introduced the TFChangeUnitTagList()
	 * and TFExamineFileSize() functions, which we need.
	 */
	TrackFileBase = gd->gd_TrackFileBase = &gd->gd_TrackFileDevice.io_Device->dd_Library;
	if(TrackFileBase->lib_Version < 2 || (TrackFileBase->lib_Version == 2 && TrackFileBase->lib_Revision < 15))
	{
		Error(gd, "\"%s\" version 2 or higher required.", TrackFileBase->lib_Node.ln_Name);

		error = IOERR_OPENFAIL;
		goto out;
	}

	/* We have to have icon.library for everything that happens next. */
	IconBase = OpenLibrary("icon.library", 0);
	if(IconBase == NULL)
	{
		SHOWMSG("could not open icon.library");

		Error(gd, "Could not open icon.library.");

		error = ERROR_INVALID_RESIDENT_LIBRARY;
		goto out;
	}

	/* We look at each project icon. */
	for(i = 1 ; i < num_args ; i++)
	{
		/* If an error message needs to be displayed, the user
		 * may want to stop processing any further icons or
		 * just continue on. If this is the only icon there is,
		 * or the last one left, don't pretend that there is a
		 * choice to be made.
		 */
		if(i+1 == num_args)
			choices = "Stop";
		else
			choices = "Continue|Stop";

		/* In case we need to show an error message, try to
		 * show the name of the disk image file in the
		 * error requester title.
		 */
		gd->gd_DiskImageFileName = FilePart(args[i].wa_Name);

		/* Clean up from last round... */
		if(file != ZERO)
		{
			SHOWMSG("closing old file");

			Close(file);
			file = ZERO;
		}

		if(icon != NULL)
		{
			SHOWMSG("freeing old icon");

			FreeDiskObject(icon);
		}

		if(gd->gd_LoadedFileSystem != ZERO)
		{
			/* Release the file system loaded for the
			 * previous icon, if there is one, if
			 * it hasn't been used.
			 */
			if(NOT gd->gd_LoadedFileSystemUsed)
			{
				SHOWMSG("unloading unused file system");

				UnLoadSeg(gd->gd_LoadedFileSystem);
			}

			/* Get read to load another file system. */
			gd->gd_LoadedFileSystem = ZERO;
			gd->gd_LoadedFileSystemUsed = FALSE;
		}

		gd->gd_LoadedFileSystemName = NULL;

		/* The default is not to use disk image file checksums. */
		gd->gd_UseChecksums = FALSE;

		/* The disk image starts out as write-protected. */
		write_protected = TRUE;

		#if defined(ENABLE_CACHE)
		{
			/* Reset the cache options to defaults (no cache
			 * enabled, do not prefill the cache, no cache size
			 * preferences).
			 */
			enable_cache = FALSE;
			prefill_cache = 0;
			cache_size = 0;
		}
		#endif /* ENABLE_CACHE */

		CurrentDir(args[i].wa_Lock);

		D(("checking icon '%s'", args[i].wa_Name));

		/* We'd like to see the icon, please. */
		if(IconBase->lib_Version >= 44)
		{
			SHOWMSG("will try to get a default icon if necessary");

			/* This will produce a default icon, which hopefully
			 * will be specific to the Amiga disk image file type
			 * and feature relevant tool types.
			 */
			icon = GetIconTags(args[i].wa_Name,
				ICONGETA_FailIfUnavailable, FALSE,
			TAG_DONE);
		}
		else
		{
			icon = GetDiskObject(args[i].wa_Name);
		}

		if(icon != NULL)
		{
			/* This should be a project icon, really. */
			if(icon->do_Type != WBPROJECT)
			{
				D(("'%s' isn't a project icon", args[i].wa_Name));

				if(ShowError(gd, choices, "Disk image data does not appear to be a file."))
				{
					continue;
				}
				else
				{
					error = ERROR_OBJECT_WRONG_TYPE;
					goto out;
				}
			}

			#if defined(ENABLE_CACHE)
			{
				STRPTR enable_cache_option;
				STRPTR prefill_cache_option;
				STRPTR cache_size_option;

				/* Enable caching for this disk image? */
				enable_cache_option = FindToolType(icon->do_ToolTypes, "ENABLECACHE");
				if(enable_cache_option != NULL)
				{
					D(("enable cache option = '%s'", enable_cache_option));

					if(Stricmp(enable_cache_option, "yes") == SAME)
					{
						if(NOT enable_cache)
						{
							SHOWMSG("enabling cache");

							enable_cache = TRUE;
						}
						else
						{
							SHOWMSG("cache remains unchanged");
						}
					}
					else
					{
						SHOWMSG("cache remains unchanged");
					}
				}
				else
				{
					SHOWMSG("cache remains unchanged");
				}

				/* Prefill the cache for this disk image? Note that if the prefill
				 * option is enabled caching will be enabled for this disk image
				 * as well.
				 */
				prefill_cache_option = FindToolType(icon->do_ToolTypes, "PREFILLCACHE");
				if(prefill_cache_option != NULL)
				{
					D(("enable cache option = '%s'", prefill_cache_option));

					if(Stricmp(prefill_cache_option, "yes") == SAME)
					{
						SHOWMSG("enabling both cache and cache prefill");

						prefill_cache = enable_cache = TRUE;
					}
					else
					{
						SHOWMSG("cache prefill remains disabled");
					}
				}
				else
				{
					SHOWMSG("cache prefill remains disabled");
				}

				/* Request a maximum shared cache size? */
				cache_size_option = FindToolType(icon->do_ToolTypes, "CACHESIZE");
				if(cache_size_option != NULL)
				{
					LONG value;

					D(("cache size option = '%s'", cache_size_option));

					if(StrToLong(cache_size_option, &value) > 0 && value >= TF_MINIMUM_CACHE_SIZE)
					{
						cache_size = value;

						D(("cache size is now %ld bytes", cache_size));
					}
					else
					{
						SHOWMSG("cache size remains unchanged");
					}
				}
				else
				{
					SHOWMSG("cache size remains unchanged");
				}
			}
			#endif /* ENABLE_CACHE */

			/* You can request the disk to be write-enabled. */
			write_protect_option = FindToolType(icon->do_ToolTypes, "WRITEPROTECTED");
			if(write_protect_option == NULL)
				write_protect_option = FindToolType(icon->do_ToolTypes, "PROTECTED");

			if(write_protect_option != NULL)
			{
				D(("write protect option = '%s'", write_protect_option));

				/* We check with Stricmp() because MatchToolValue() is
				 * unsuitable: it would return TRUE if "yes | no" were
				 * set as the tool type value.
				 */
				if(Stricmp(write_protect_option, "no") == SAME)
				{
					SHOWMSG("disabling write protection");

					write_protected = FALSE;
				}
				else
				{
					SHOWMSG("write protection remains unchanged");
				}
			}
			else
			{
				SHOWMSG("write protection remains unchanged");
			}

			/* Use disk image file checksums? */
			usechecksums_system_option = FindToolType(icon->do_ToolTypes, "USECHECKSUMS");
			if(usechecksums_system_option != NULL)
			{
				D(("use checksums option = '%s'", usechecksums_system_option));

				if(Stricmp(usechecksums_system_option, "yes") == SAME)
				{
					SHOWMSG("enabling checksums");

					gd->gd_UseChecksums = TRUE;
				}
				else
				{
					SHOWMSG("checksum use remains unchanged");
				}
			}
			else
			{
				SHOWMSG("checksum use remains unchanged");
			}

			/* You can also ask for a specific file system to be
			 * used for this disk image.
			 */
			file_system_option = FindToolType(icon->do_ToolTypes, "FILESYSTEM");
			if(file_system_option != NULL)
			{
				D(("file system option = '%s'", file_system_option));

				if(file_system_option[0] != '\0')
				{
					gd->gd_LoadedFileSystem = LoadSeg(file_system_option);
					if(gd->gd_LoadedFileSystem == ZERO)
					{
						error = IoErr();

						D(("that didn't work (error=%ld)", error));

						if(ShowError(gd, choices, "Could not load \"%s\" file system (%s).",
							file_system_option,
							get_error_message(gd, error, error_message, sizeof(error_message))))
						{
							continue;
						}
						else
						{
							goto out;
						}
					}

					gd->gd_LoadedFileSystemName = file_system_option;
				}
				else
				{
					SHOWMSG("file system remains unchanged");
				}
			}
			else
			{
				SHOWMSG("file system remains unchanged");
			}
		}
		else
		{
			SHOWMSG("didn't get the icon");
		}

		D(("opening file '%s'", args[i].wa_Name));

		/* There should be an actual file to go along with the icon. */
		file = Open(args[i].wa_Name, MODE_OLDFILE);
		if(file == ZERO)
		{
			error = IoErr();

			D(("file didn't open for reading (error=%ld)", error));

			if(ShowError(gd, choices, "Could not open file for reading (%s).",
				get_error_message(gd, error, error_message, sizeof(error_message))))
			{
				continue;
			}
			else
			{
				goto out;
			}
		}

		/* That file better be the right size. */
		if(CANNOT ExamineFH(file, fib))
		{
			/* ExamineFH() may fail on an old file system, such
			 * as the CDTV ISO 9660 file system. So we try again,
			 * if we can...
			 */
			error = IoErr();
			if(error == ERROR_ACTION_NOT_KNOWN)
			{
				BPTR file_lock;

				file_lock = Lock(args[i].wa_Name, SHARED_LOCK);
				if(file_lock != ZERO)
				{
					if(Examine(file_lock, fib))
						error = OK;
					else
						error = IoErr();

					UnLock(file_lock);
				}
				else
				{
					error = IoErr();
				}
			}

			if(error != OK)
			{
				D(("cannot examine file (error=%ld)", error));

				if(ShowError(gd, choices, "Could not examine file (%s).",
					get_error_message(gd, error, error_message, sizeof(error_message))))
				{
					continue;
				}
				else
				{
					goto out;
				}
			}
		}

		/* Either a 3.5" DD or 3.5" DD disk, but nothing else. */
		if(TFExamineFileSize(fib->fib_Size) == TFEFS_Unsupported)
		{
			D(("file size %ld is not suitable", fib->fib_Size));

			if(ShowError(gd, choices, "Disk image file size is not supported."))
			{
				continue;
			}
			else
			{
				error = ERROR_OBJECT_WRONG_TYPE;
				goto out;
			}
		}

		SHOWMSG("starting the next available unit");

		/* Find a suitable unit for it. */
		error = start_unit(gd,
			FALSE,	/* <- No verbose output */
			-1,		/* <- Invalid unit number */
			TRUE,	/* <- Use the next available unit */
			cache_size,
			num_cylinders,
			num_sectors,
			&unit,
			NULL);	/* <- No AmigaDOS device name provided */

		if(error != OK)
		{
			D(("that didn't work (error=%ld)", error));

			if(ShowError(gd, choices, "Could not start disk device (%s).",
				get_error_message(gd, error, error_message, sizeof(error_message))))
			{
				continue;
			}
			else
			{
				goto out;
			}
		}

		SHOWMSG("inserting medium");

		/* And insert this medium. */
		error = TFInsertMediaTags(unit,
			TF_ImageFileHandle,		file,
			TF_WriteProtected,		write_protected,

			#if defined(ENABLE_CACHE)
				TF_EnableUnitCache,		enable_cache,
				TF_PrefillUnitCache,	prefill_cache,
			#endif /* ENABLE_CACHE */
		TAG_DONE);

		if(error != OK && error != ERROR_OBJECT_IN_USE)
		{
			D(("that didnt't work, and we have bigger problems now (error=%ld)", error));

			if(ShowError(gd, choices, "Could not mount disk image file (%s).",
				get_error_message(gd, error, error_message, sizeof(error_message))))
			{
				continue;
			}
			else
			{
				goto out;
			}

			goto out;
		}

		/* The file is now in use. */
		if(error == OK)
		{
			SHOWMSG("file is now in use");
			file = ZERO;
		}
	}

	error = OK;

 out:

	if(gd->gd_LoadedFileSystem != ZERO && NOT gd->gd_LoadedFileSystemUsed)
	{
		SHOWMSG("unloading unused file system");

		UnLoadSeg(gd->gd_LoadedFileSystem);
	}

	if(file != ZERO)
		Close(file);

	if(icon != NULL)
		FreeDiskObject(icon);

	if(TrackFileBase != NULL)
		CloseDevice((struct IORequest *)&gd->gd_TrackFileDevice);

	if(IconBase != NULL)
		CloseLibrary(IconBase);

	RETURN(error);
	return(error);
}
