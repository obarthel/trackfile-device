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

#include <resources/filesysres.h>

#include <dos/filehandler.h>

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

/****************************************************************************/

#include "assert.h"

/****************************************************************************/

/* Mount a file system in the manner of the "strap" module, which is
 * responsible for setting up the Amiga floppy disk drives. The
 * mount process is intended to be compatible with this particular
 * use case as far as the data structures are concerned. However, we
 * are using dos.library functions for all of the APIs rather than
 * the expansion.library/AddBootNode function which serves a different
 * purpose (auto-booting).
 *
 * Note that this function is called with the global AmigaDOS list
 * of devices locked for writing. Hence you cannot safely use any
 * AmigaDOS functions in it which would result in a file access.
 */
LONG
mount_floppy_file(
	struct GlobalData *		gd,
	STRPTR					dos_device_name,
	LONG					unit_number,
	int						num_cylinders,
	int						num_sectors_per_track,
	struct DeviceNode **	dn_ptr)
{
	const struct FileSysResource * fsr;
	struct DeviceNode * dn;
	struct FileSysStartupMsg * fssm = NULL;
	STRPTR fssm_device_name = NULL;
	int fssm_device_name_len;
	struct DosEnvec * de = NULL;
	LONG error = ERROR_NO_FREE_STORE;

	USE_EXEC(gd);
	USE_DOS(gd);
	USE_TRACKFILE(gd);

	ENTER();

	ASSERT( gd != NULL );
	ASSERT( unit_number >= 0 );
	ASSERT( num_cylinders > 0 && num_sectors_per_track > 0);
	ASSERT( dos_device_name != NULL );
	ASSERT( strchr(dos_device_name, '/') == NULL );
	ASSERT( strchr(dos_device_name, ':') == NULL );
	ASSERT( strlen(dos_device_name) <= 255 );

	/* Note that the DOS device name must not contain
	 * '/' or ':' characters and cannot be longer than
	 * 255 characters total.
	 */
	dn = (struct DeviceNode *)MakeDosEntry(dos_device_name, DLT_DEVICE);
	if(dn == NULL)
	{
		D(("could not create dos device %s", dos_device_name));

		error = IoErr();
		goto out;
	}

	/* Note that we must use AllocVec() for this public data structure. */
	fssm = AllocVec(sizeof(*fssm), MEMF_ANY|MEMF_PUBLIC|MEMF_CLEAR);
	if(fssm == NULL)
	{
		SHOWMSG("could not allocate memory for file system startup message");
		goto out;
	}

	fssm_device_name_len = strlen(TrackFileBase->lib_Node.ln_Name);

	/* This is for a NUL-terminated BCPL string, with a leading
	 * length byte, followed by the string and the NUL-termination.
	 * That pointer will go into the FileSysStartupMsg->fssm_Device
	 * field. Note that we must use AllocVec() for this public
	 * data structure.
	 */
	fssm_device_name = AllocVec(1 + fssm_device_name_len + 1, MEMF_ANY|MEMF_PUBLIC);
	if(fssm_device_name == NULL)
	{
		SHOWMSG("could not allocate memory for device name");
		goto out;
	}

	de = AllocVec((1+DE_BOOTBLOCKS) * sizeof(LONG), MEMF_ANY|MEMF_PUBLIC|MEMF_CLEAR);
	if(de == NULL)
	{
		SHOWMSG("could not allocate memory for DOS environment vector");
		goto out;
	}

	/* This must be a NUL-terminated BCPL string. */
	fssm_device_name[0] = fssm_device_name_len;
	strcpy(&fssm_device_name[1], TrackFileBase->lib_Node.ln_Name);

	fssm->fssm_Unit		= unit_number;
	fssm->fssm_Device	= MKBADDR(fssm_device_name);
	fssm->fssm_Environ	= MKBADDR(de);
	fssm->fssm_Flags	= 0;

	/* NOTE: We need to set this up exactly like it's shown here,
	 *       with the GlobalVec=0 and StackSize=600 or strange
	 *       things will happen. The default AmigaDOS file system
	 *       is supposed to use the Tripos startup method, which means
	 *       that using dn_GlobVec=-1 and dn_StackSize=2400 will
	 *       crash it because this will force the 'C' interface
	 *       for the file system to be used (which isn't there).
	 */
	dn->dn_StackSize	= 600;	/* <- Number of 32 bit words that make up the stack space. */
	dn->dn_Priority		= 10;
	dn->dn_Startup		= MKBADDR(fssm);
	dn->dn_SegList		= ((struct DosLibrary *)DOSBase)->dl_Root->rn_FileHandlerSegment;

	/* Use a custom file system instead of the ROM default
	 * file system?
	 *
	 * Why use the segment pointer of a file system loaded
	 * from disk rather than setting up the dn->dn_Handler
	 * to hold the name of that file system? This only works
	 * well if the file system name is given as an absolute
	 * path such as "L:FastFileSystem". This is not necessarily
	 * possible if you load the file system from the local
	 * directory, for example. Also, it complicates matters in
	 * that when the file system is supposed to be started and
	 * the named file system file cannot be loaded. In this case
	 * the file system would remain unusable and there would be
	 * no good way to detect and report why it failed.
	 */
	if(gd->gd_LoadedFileSystem != ZERO)
	{
		/* We assume that the file system is not using
		 * the BCPL rules and is written in 'C' or
		 * assembly language.
		 */
		dn->dn_GlobalVec	= -1; /* <- 'C'/assembly language file system, not BCPL. */
		dn->dn_StackSize	= dn->dn_StackSize * sizeof(LONG); /* <- Bytes, not the number of 32 bit words. */
		dn->dn_SegList		= gd->gd_LoadedFileSystem;
	}

	ASSERT( num_cylinders == 80 );
	ASSERT( num_sectors_per_track == 11 || num_sectors_per_track == 22 );

	/* Standard configuration for a floppy disk drive, as you would
	 * find it in the 'strap' module which is responsible for
	 * letting the operating system boot from floppy disk,
	 * PCMCIA card or an auto-booting mass storage device.
	 */
	de->de_TableSize		= DE_BOOTBLOCKS;
	de->de_SizeBlock		= TD_SECTOR / sizeof(LONG);
	de->de_SecOrg			= 0;
	de->de_Surfaces			= NUMHEADS; /* 2 surfaces for a standard 3.5" disk. */
	de->de_SectorPerBlock	= 1;
	de->de_BlocksPerTrack	= num_sectors_per_track; /* 11 sectors for a standard 3.5" DD disk. */
	de->de_Reserved			= 2;
	de->de_PreAlloc			= 0;
	de->de_Interleave		= 0;
	de->de_LowCyl			= 0;
	de->de_HighCyl			= num_cylinders - 1; /* 80 cylinders for a standard 3.5" disk. */
	de->de_NumBuffers		= 5;
	de->de_BufMemType		= MEMF_ANY|MEMF_PUBLIC;
	de->de_MaxTransfer		= 0x200000; /* True for trackdisk.device but not necessarily for trackfile.device */
	de->de_Mask				= 0x7FFFFFFE; /* True for trackdisk.device but not necessarily for trackfile.device */
	de->de_BootPri			= -128;
	de->de_DosType			= ID_DOS_DISK;
	de->de_BootBlocks		= de->de_Reserved;

	/* Final touches: check with FileSystem.resource if the file system
	 * type we want to use has been configured to use different DeviceNode
	 * options after all.
	 */
	fsr = OpenResource(FSRNAME);
	if(fsr != NULL)
	{
		const struct FileSysEntry * fse_best = NULL;
		const struct FileSysEntry * fse;

		Forbid();

		/* We need a file system entry which matches the default file
		 * system type for the mount information. Furthermore, if there
		 * is more than one matching entry, we will use the one with
		 * the highest version and revision number.
		 */
		for(fse = (struct FileSysEntry *)fsr->fsr_FileSysEntries.lh_Head ;
		    fse->fse_Node.ln_Succ != NULL ;
		    fse = (struct FileSysEntry *)fse->fse_Node.ln_Succ)
		{
			if(fse->fse_DosType == de->de_DosType)
			{
				if(fse_best != NULL && fse->fse_Version < fse_best->fse_Version)
					continue;

				fse_best = fse;
			}
		}

		if(fse_best != NULL)
		{
			const ULONG * from;
			ULONG * to;
			int flag;

			D(("found a FileSystem.resource record with patch flags = 0x%08lx and version = %ld.%ld",
				fse_best->fse_PatchFlags,
				fse_best->fse_Version >> 16, fse_best->fse_Version & 0xFFFF
			));

			/* Copy some or all of the file system options into
			 * the DeviceNode we just created.
			 */
			from	= (ULONG *)&fse_best->fse_Type;
			to		= (ULONG *)&dn->dn_Type;

			/* This covers 9 entries only:
			 *
			 * 0 (0x001) fse_Type
			 * 1 (0x002) fse_Task
			 * 2 (0x004) fse_Lock
			 * 3 (0x008) fse_Handler
			 * 4 (0x010) fse_StackSize
			 * 5 (0x020) fse_Priority
			 * 6 (0x040) fse_Startup
			 * 7 (0x080) fse_SegList
			 * 8 (0x100) fse_GlobalVec
			 */
			for(flag = 0 ; flag < 9 ; flag++)
			{
				if(FLAG_IS_SET(fse_best->fse_PatchFlags, (1 << flag)))
				{
					switch(flag)
					{
						case 0:

							D(("setting dn_Type = %ld", from[flag]));
							break;

						case 1:

							D(("setting dn_Task = 0x%08lx", from[flag]));
							break;

						case 2:

							D(("setting dn_Lock = %lx (= 0x%08lx)", from[flag], BADDR(from[flag])));
							break;

						case 3:

							D(("setting dn_Handler = %lx (= \"%b\")", from[flag], from[flag]));
							break;

						case 4:

							D(("setting dn_StackSize = %ld", from[flag]));
							break;

						case 5:

							D(("setting dn_Priority = %ld", from[flag]));
							break;

						case 6:

							D(("setting dn_Startup = %lx (= 0x%08lx)", from[flag], BADDR(from[flag])));
							break;

						case 7:

							D(("setting dn_SegList = %lx (= 0x%08lx)", from[flag], BADDR(from[flag])));
							break;

						case 8:

							if(from[flag] < 0)
								D(("setting dn_GlobVec = %ld", from[flag]));
							else
								D(("setting dn_GlobVec = %lx (= 0x%08lx)", from[flag], BADDR(from[flag])));

							break;
					}

					to[flag] = from[flag];
				}
			}
		}
		else
		{
			D(("did not find FileSystem.resource record with fse_DosType=0x%08lx", de->de_DosType));
		}

		Permit();
	}

	/* And that should be the end of that little exercise.
	 * With AddDosEntry() we may now be able to activate
	 * the file system device properly.
	 */
	if(CANNOT AddDosEntry((struct DosList *)dn))
	{
		SHOWMSG("could not add device node");

		error = IoErr();
		goto out;
	}

	/* Remember the DeviceNode, if possible. */
	if(dn_ptr != NULL)
		(*dn_ptr) = dn;

	/* These are now all in use and must not be released. */
	dn = NULL;
	fssm = NULL;
	fssm_device_name = NULL;
	de = NULL;

	/* Do not unload the file system now. */
	if(gd->gd_LoadedFileSystem != ZERO)
		gd->gd_LoadedFileSystemUsed = TRUE;

	error = OK;

 out:

	if(dn != NULL)
		FreeDosEntry((struct DosList *)dn);

	FreeVec(fssm);
	FreeVec(fssm_device_name);
	FreeVec(de);

	RETURN(error);
	return(error);
}
