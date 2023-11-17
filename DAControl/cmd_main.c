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

#include <exec/errors.h>
#include <exec/resident.h>

#include <dos/dosextens.h>
#include <dos/dosasl.h>

#include <devices/timer.h>

#include <workbench/startup.h>

#include <devices/bootblock.h>

/****************************************************************************/

#include <clib/alib_protos.h>

/****************************************************************************/

#define __USE_SYSBASE
#include <proto/exec.h>

#include <proto/dos.h>
#include <proto/utility.h>
#include <proto/icon.h>
#include <proto/timer.h>

#include <proto/trackfile.h>

/****************************************************************************/

#include <string.h>

/****************************************************************************/

#include "compiler.h"
#include "macros.h"
#include "global_data.h"
#include "insert_media_by_name.h"
#include "mount_floppy_file.h"
#include "start_unit.h"
#include "tools.h"
#include "cache.h"
#include "cmd_main.h"

/****************************************************************************/

#include "assert.h"

/****************************************************************************/

#include "DAControl_rev.h"

/****************************************************************************/

/* These are semantic sugar for the ReadArgs() command
 * line processing.
 */
typedef STRPTR	KEY;
typedef LONG	SWITCH;
typedef LONG *	NUMBER;

/****************************************************************************/

static int compare_by_unit_number(const struct Node *a, const struct Node *b);

/****************************************************************************/

/****** commands/DACONTROL ***************************************************
*
*   NAME
*	DACONTROL - Load and mount, eject or create an Amiga Disk File (ADF).
*
*   FORMAT
*	DACONTROL [[LOAD|EJECT|CHANGE] [START|STOP] [DEVICE <unit or device>]]
*	[TIMEOUT <number of seconds>] [PROTECT|WRITEPROTECTED {<YES|NO>}]
*	[USECHECKSUMS {<YES|NO>}] [SAFEEJECT {<YES|NO>}]
*	[CREATE [BOOTABLE] [DISKTYPE <DD|HD>] [LABEL <name>] [OVERWRITE]
*	[FILESYSTEM <name>] [FILESYSTEMTYPE [<OFS|FFS>][+INTERNATIONAL]
*	[+<LONGNAME|DIRCACHE>]]] [INFO [SHOWCHECKSUMS] [SHOWVOLUMES]
*	[SHOWBOOTBLOCKS]] [SETENV] [SETVAR] [QUIET|VERBOSE] [IGNORE]
*	[[FILE] {<name|pattern>}]
*
*   TEMPLATE
*	LOAD/S,EJECT/S,CHANGE/S,TIMEOUT/K/N,START/S,STOP/S,CREATE/S,
*	USECHECKSUMS/K,SAFEEJECT/K,BOOTABLE=INSTALL/S,FILESYSTEM/K,
*	FILESYSTEMTYPE/K,OVERWRITE/S,DISKTYPE/K,LABEL/K,
*	PROTECT=WRITEPROTECTED/K,UNIT=DEVICE/K,INFO/S,SHOWCHECKSUMS/S,
*	SHOWVOLUMES/S,SHOWBOOTBLOCKS/S,SETENV/S,SETVAR/S,QUIET/S,
*	VERBOSE/S,IGNORE/S,FILE/M
*
*   PATH
*	C/DACONTROL
*
*   FUNCTION
*	The DACONTROL command loads ADF (Amiga Disk File) image files and
*	makes them available to the system in a very similar way on how
*	physical floppy disks are treated. It needs "trackfile.device" to
*	perform its duty.
*
*	Only ADF image files of certain sizes are supported, such as for
*	double density and high density 3.5" floppy disks, as well as
*	for 5.25" double density disks. These disks must be "formatted"
*	for use with the Amiga, i.e. they must be either 880 KBytes
*	or 1760 KBytes in size.
*
*	Like a floppy disk drive, "trackfile.device" supports multiple
*	"drives", each corresponding to a unit number. For example, unit 0
*	corresponds to device name "DA0:", similiar to how "DF0:" corresponds
*	to floppy disk unit 0.
*
*	The number of such units is limited only by available memory. You can
*	either request a specific unit number to be used or let DACONTROL
*	reuse an unused or empty unit whose medium has just been ejected.
*
*	DACONTROL can load, eject or change a medium, i.e. a disk image file.
*	You can tell it which unit to use, or you just say which device you
*	want, e.g. "DA0:" if you want to eject what's in "DA0:". Changing a
*	medium implies ejecting a medium already present first, then loading
*	the new medium. Changing a medium is safe even if there is currently
*	no medium loaded.
*
*	The default for disk images is the read-only state. If you need to
*	write to the media, you need to specifically say so.
*
*	You can add a project icon with "C:DAControl" as its default tool to
*	have for example, a particular ADF file mounted when you double-click
*	on its icon in Workbench. Use the WRITEPROTECTED=NO tool type to have
*	the file mounted as a writable disk. PROTECT may be used in place of
*	WRITEPROTECTED. Also supported are the FILESYSTEM and USECHECKSUMS
*	tool types.
*
*	Available options:
*
*	LOAD
*	    Loads a specific ADF file and mounts it as a file system device.
*	    Wildcards are supported if you want to load more than a ADF file.
*	    The LOAD option implies that a unit is started or reused for this
*	    file first.
*
*	    Once the ADF file is loaded, it cannot be deleted or moved. You
*	    would need to eject it first.
*
*	EJECT
*	    Ejects a medium, i.e. an ADF file. You need to state which unit or
*	    device name it is associated with. Once the medium is ejected, you
*	    can move or delete the file.
*
*	CHANGE
*	    Removes an ADF file from a unit, if present, and then loads a new
*	    ADF file for it. You need to say which unit or device name is
*	    associated with the ADF file that should be ejected. It is safe to
*	    try and eject an ADF file from a unit which has none loaded yet.
*
*	TIMEOUT
*	    Once an ADF file is mounted, AmigaDOS may keep it busy when it
*	    reads from it or changes are made to its contents. If you use the
*	    EJECT or CHANGE option, it may not immediately have the desired
*	    effect. Use the TIMEOUT option to state for how long the DACONTROL
*	    command should wait for the medium to be safely ejected. The
*	    minimum time to wait are 5 seconds.
*
*	    If you do not want to wait any longer while DACONTROL is trying to
*	    eject a medium, press the [Ctrl]+C keys or use the BREAK shell
*	    command instead.
*
*	WRITEPROTECTED
*	    If you use the LOAD or CHANGE options, the disk image file will be
*	    mounted as read-only. To make the image writable instead, use the
*	    WRITEPROTECTED=NO option.
*
*	CREATE
*	    Creates a "blank" disk image file when it starts up and has it
*	    formatted and mounted. This image file will always be writable,
*	    overriding the WRITEPROTECTED=YES option.
*
*	    You can only create a disk image file if this would not result in
*	    an existing file getting overwritten.
*
*	BOOTABLE
*	    This option will make the image file created through the CREATE
*	    option bootable, as if the INSTALL command had been used on it.
*
*	USECHECKSUMS
*	    No two disk images with the same contents should be active at
*	    the same time, or otherwise mounting the disk image may crash
*	    as a result. To guard against this possibility, you can enable
*	    disk and track checksums which reduce the risk of a crash at
*	    the expense of the mount operation taking slightly longer.
*	    It will take slightly longer because the entire disk image file
*	    will have to be read first. The USECHECKSUMS=YES option enables
*	    the use of disk image checksums.
*
*	SAFEEJECT
*	    Ejecting or changing the disk image file associated with a device or
*	    unit requires that the image file is not currently in use. The removal
*	    of the image file therefore has to wait until the file system has
*	    stopped using. A floppy disk drive LED will be turned off when the
*	    drive's motor has stopped, indicating that pressing the eject button
*	    is now possible. The DAControl command will likewise wait for the same
*	    type of signal before removing the image file.
*
*	    This approach is not as robust as the alternative, namely asking the
*	    file system which tends to a disk drive to let go of the medium,
*	    writing back any buffered data first, and only then ejecting the disk
*	    image file. This is the safer approach and also much faster than
*	    waiting for the file system to spin down the drive motor. Use the
*	    SAFEEJECT=YES option to enable this faster eject operation. Note that
*	    the SAFEEJECT=YES option may not be supported well by some software
*	    which may have trouble detecting that a volume is no longer present.
*
*	FILESYSTEM
*	    DAControl will use the same filesystem software which the disk drives
*	    DF0: through DF3: and even RAD: would use. You can use a different
*	    file system of your choice which serves the same purpose, such as the
*	    "L:FileSystem" which is used mostly by hard disk partitioning
*	    software. This can be useful for the purpose of testing software with
*	    older or newer Amiga filesystems, rather than the filesystem software
*	    your system starts with.
*
*	    Please note that DAControl will load this filesystem software every
*	    time you use the FILESYSTEM option, even if you used it before with
*	    the same option and filesystem name. You may want to keep an eye on
*	    how much memory this will consume!
*
*	FILESYSTEMTYPE
*	    This option will change the file system type or mode of operation
*	    for the image file created through the CREATE option. The type
*	    must be one of either OFS or FFS, which can be combined with the
*	    modes INTERNATIONAL, DIRCACHE and LONGNAMES. Not all combinations
*	    are permitted. Use the "+" sign to combine type and modes, such
*	    as in OFS+INTERNATIONAL or FFS+LONGNAMES.
*
*	OVERWRITE
*	    Unless you use the OVERWRITE option, the CREATE option will first
*	    check if there already is a file, directory or link by the given
*	    file name, and abort the CREATE operation immediately. With the
*	    OVERWRITE option in use, any existing file will be overwritten
*	    without a prior check to avoid this.
*
*	LABEL
*	    Tells DACONTROL which label to use for a newly-created empty disk
*	    image file. If you omit the LABEL options, DACONTROL will assign a
*	    default label all by itself instead of just using "Empty".
*
*	DISKTYPE
*	    This option has an effect on the size of the image file to be
*	    created through the CREATE option. It can either be a DD (880 KB
*	    double density) or HD (1.76 MB high density) image file.
*
*	UNIT/DEVICE
*	    Some of the options need to know which AmigaDOS file system
*	    device, e.g. "DA0:" or which corresponding unit number is
*	    involved. DACONTROL automatically determines whether you provided
*	    a unit number or an AmigaDOS file system device name.
*
*	    Instead of using a unit number or device name, you can also use
*	    the keywords ANY or LAST. ANY will pick the next unused unit and
*	    reuse it, if necessary, or start a new unit instead. LAST will
*	    consult the "DA_LASTDEVICE" environment variable and use what it
*	    found there.
*
*	START
*	    Each time you use the LOAD or CREATE options, DACONTROL will
*	    either reuse an unused unit or it will start a new unit for you,
*	    followed by mounting a disk image file. You can also choose to
*	    directly start a new unit with the START option, without attaching
*	    a disk image file to it yet.
*
*	    You can combine the START option with the LOAD and CHANGE options
*	    in that the unit will first be started, followed by a single disk
*	    image file being loaded.
*
*	STOP
*	    You can save memory by stopping a unit which you no longer need.
*	    While it can reactivated later, in the mean time between 12-17
*	    KBytes of RAM can be released. Please note that the AmigaDOS
*	    device attached to the unit, such as "DA0:" for unit 0, will
*	    remain active with the unit shut down. It will appear like an
*	    empty disk drive.
*
*	    Stopping a unit requires that the medium associated with it has
*	    been ejected. You can combine the EJECT and STOP options to the
*	    effect that the medium will be ejected first, and then the unit
*	    will be stopped.
*
*	INFO
*	    Show which units have been started, whether they are active, have
*	    a medium loaded and which further information is available on
*	    them.
*
*	SHOWCHECKSUMS
*	    Information on the currently active units does not include the
*	    disk checksum information by default. Use the SHOWCHECKSUMS
*	    options to show it. Please note that a unit has to have the
*	    checksum feature enabled in order for a disk checksum to be
*	    displayed (it is disabled by default).
*
*	    Checksum information is updated in real time as the contents
*	    of a disk image are modified.
*
*	SHOWVOLUMES
*	    When using the INFO option, show the volume names and creation
*	    dates associated with the contents of the disk image files.
*	    Please note that this information may not be available for all
*	    disk images and at all times. If the volume creation date and
*	    time is unavailable, so will the be the volume name.
*
*	    Volume information is updated in real time as the contents
*	    of a disk image are modified.
*
*	SHOWBOOTBLOCKS
*	    When using the INFO option, show the file system signature and
*	    whether or not the disk is supposed to be bootable. Please note
*	    that this information may not be available for all disk images.
*
*	    Boot block and file system signature information is updated in
*	    real time as the contents of a disk image are modified.
*
*	SETVAR and SETENV
*	    If you use one of these options, then DACONTROL will store the
*	    name of the last AmigaDOS device it used in the environment
*	    variable "DA_LASTDEVICE". You can use this variable in AmigaDOS
*	    scripts for example, or you can tell DACONTROL the next time you
*	    start it with DEVICE=LAST to consult the is environment variable.
*
*	    SETVAR will create a local variable which is only visible to the
*	    shell or script in which DACONTROL was used. SETENV will create an
*	    environment variable which is visible to all shells and scripts.
*
*	QUIET
*	    Performs the instructed operation without displaying any progress
*	    messages on the screen, unless error messages need to be printed.
*
*	VERBOSE
*	    Performs the instructed operation displaying more detail on what
*	    is being done.
*
*	IGNORE
*	    If the files to load or change are missing, or if such files are
*	    present but unsuitable, DACONTROL will print an error message,
*	    stop and exit. Use the IGNORE option to make DACONTROL continue
*	    instead of stopping and exiting.
*
*	    Please note that error messages will still be printed if the
*	    IGNORE option is used. DACONTROL will still stop and exit if a
*	    problem is more serious than missing files or unsuitable
*	    files.
*
*	FILE
*	    The LOAD and CREATE options require the name of a disk image file
*	    which should be loaded or created. You may use wildcards when
*	    loading disk image files.
*
*   EXAMPLES
*	1> DACONTROL load #?.adf
*
*	will load all ADF files in the current directory, You will quickly see
*	all the volumes pop up on your Workbench backdrop
*
*	1> DACONTROL load disk.adf device da0:
*
*	will load an ADF file and make it available through DA0:, if this
*	is possible.
*
*	1> DACONTROL load adf/test1.adf writeprotected=no
*
*	will give you a write-enabled volume.
*
*	1> DACONTROL create label=empty adf/empty.adf
*
*	will create an empty disk image file, make it writable and format it
*	using "empty" as the volume name.
*
*	1> DACONTROL create fstype=ffs+intl adf/empty.adf
*
*	will create an empty disk image file, make it writable, and format it
*	in FFS international mode.
*
*	1> DACONTROL info
*	Device  Type     Active  Access     File
*	DA0     3.5" DD  Yes     read-only  Archive:Demo 1.adf
*	DA1     3.5" DD  Yes     read-only  Archive:Demo 2.adf
*	DA2     3.5" DD  Yes *   read-only  Archive:Mindwalker.adf
*	DA3     3.5" DD  Yes     read-only  Archive:Workbench.adf
*	DA4     3.5" DD  Yes     read-only  Archive:Amiga Extras.adf
*	DA5     3.5" DD  No      -          -
*
*	will show the units which have been started so far. Each unit
*	corresponds to an AmigaDOS file system device, such as 0 to "DA0:". In
*	this example unit 5 has been stopped. Unit 2 is currently busy
*	indicate by the "Yes *" in the "Active" column. Being busy means that
*	the "disk motor" is spinning and the disk image file is being read or
*	written to.
*
*   1> DACONTROL info showchecksums
*	Device  Type     Checksum     Active  Access      File
*	DA0     3.5" DD  yaz8H2pEvGi  Yes     read-only   AR:Demo1.adf
*
*	If a unit is keeping checksums for tracks and the disk, then these
*	may be shown as an 11 character representation.
*
*	1> DACONTROL eject timeout=5 stop unit 0
*
*	will first try to eject the medium loaded in unit 0 and wait for
*	up to 5 seconds to succeed. Once the medium is ejected, the unit
*	will be stopped.
*
*   SEE ALSO
*	MOUNT
*	FORMAT
*	INSTALL
**********************************************************************
*/

long ASM
cmd_main(REG(a0, struct GlobalData * gd))
{
	/* This is the ReadArgs() template for this command.
	 * The corresponding data structure filled in by
	 * ReadArgs() follows directly below.
	 */
	static const TEXT template[] =
		"LOAD/S,"
		"EJECT/S,"
		"CHANGE/S,"
		"TIMEOUT/K/N,"
		"START/S,"
		"STOP/S,"
		"CREATE/S,"
		"INSTALL=BOOTABLE/S,"
		"USECHECKSUMS/K,"
	#if defined(ENABLE_CACHE)
		"ENABLECACHE/K,"
		"PREFILLCACHE/K,"
		"CACHESIZE/K/N,"
	#endif /* ENABLE_CACHE */
		"SAFEEJECT/K,"
		"FILESYSTEM/K,"
		"FILESYSTEMTYPE=FSTYPE/K,"
		"OVERWRITE/S,"
		"DISKTYPE/K,"
		"LABEL/K,"
		"PROTECT=WRITEPROTECTED/K,"
		"UNIT=DEVICE/K,"
		"INFO/S,"
		"SHOWCHECKSUMS/S,"
		"SHOWVOLUMES/S,"
		"SHOWBOOTBLOCKS/S,"
	#if defined(ENABLE_CACHE)
		"SHOWCACHES/S,"
	#endif /* ENABLE_CACHE */
		"SETENV/S,"
		"SETVAR/S,"
		"QUIET/S,"
		"VERBOSE/S,"
		"IGNORE/S,"
		"FILE/M"
		VERSTAG;

	struct
	{
		SWITCH	Load;
		SWITCH	Eject;
		SWITCH	Change;
		NUMBER	Timeout;

		SWITCH	Start;
		SWITCH	Stop;

		SWITCH	Create;
		SWITCH	Bootable;
		KEY		UseChecksums;
	#if defined(ENABLE_CACHE)
		KEY		EnableCache;
		KEY		PrefillCache;
		NUMBER	CacheSize;
	#endif /* ENABLE_CACHE */
		KEY		SafeEject;
		KEY		FileSystem;
		KEY		FileSystemType;
		SWITCH	Overwrite;
		KEY		DiskType;
		KEY		Label;

		KEY		WriteProtected;

		KEY		Device;

		SWITCH	Info;
		SWITCH	ShowChecksums;
		SWITCH	ShowVolumes;
		SWITCH	ShowBootblocks;
	#if defined(ENABLE_CACHE)
		SWITCH	ShowCaches;
	#endif /* ENABLE_CACHE */

		SWITCH	SetEnv;
		SWITCH	SetVar;

		SWITCH	Quiet;
		SWITCH	Verbose;
		SWITCH	Ignore;

		KEY *	File;
	} options;

	const TEXT variable_name[] = "DA_LASTDEVICE";
	LONG rc = RETURN_FAIL;
	LONG error;
	struct Library * TrackFileBase = NULL;
	struct RDArgs * rda = NULL;
	struct AnchorPath * ap = NULL;
	BOOL use_next_available_unit = FALSE;
	TEXT dos_device_name[260]; /* <- Large enough for a BCPL string, plus a ":" character and NUL termination. */
	BOOL dos_device_name_is_valid = FALSE;
	LONG requested_unit = -1;
	LONG unit = -1;
	BOOL unit_is_valid = FALSE;
	BOOL write_protected = TRUE;
	BOOL safe_eject = FALSE;
	LONG timeout = 0;
	ULONG * cylinder_data = NULL;
	BPTR cylinder_file = ZERO;
	TEXT error_message[256];
	struct IOStdReq * io = NULL;
	ULONG * boot_block = NULL;
	const int boot_block_size = BOOTSECTS * TD_SECTOR;
	ULONG file_system_signature = 0;
	BOOL enable_cache = FALSE;
	BOOL prefill_cache = FALSE;
	LONG cache_size = 0;
	BOOL requirements_satisfied;
	/* The default disk type is an Amiga 3.5" double density disk. */
	int num_cylinders = NUMCYLS, num_sectors = NUMSECS;

	USE_EXEC(gd);
	USE_DOS(gd);
	USE_UTILITY(gd);

	/* This may be used later. */
	dos_device_name[0] = '\0';

	memset(&options, 0, sizeof(options));

	/* Open the control unit for "trackfile.device",
	 * which is unit -1 (TFUNIT_CONTROL).
	 */
	error = OpenDevice(TRACKFILENAME, TFUNIT_CONTROL, (struct IORequest *)&gd->gd_TrackFileDevice, 0);
	if(error != OK)
	{
		Error(gd, "Cannot open \"%s\".", TRACKFILENAME);
		goto out;
	}

	/* Version 2.15 of trackfile.device introduced the TFChangeUnitTagList()
	 * and TFExamineFileSize() functions, which we need.
	 */
	TrackFileBase = gd->gd_TrackFileBase = &gd->gd_TrackFileDevice.io_Device->dd_Library;
	if(TrackFileBase->lib_Version < 2 || (TrackFileBase->lib_Version == 2 && TrackFileBase->lib_Revision < 15))
	{
		error = IOERR_OPENFAIL;

		Error(gd, "\"%s\" version 2.15 or higher required.", TrackFileBase->lib_Node.ln_Name);
		goto out;
	}

	/* This is for the LOAD and CHANGE options and how they
	 * collect suitable files via MatchFirst() and MatchNext().
	 */
	ap = AllocVec(sizeof(*ap), MEMF_ANY | MEMF_PUBLIC | MEMF_CLEAR);
	if(ap == NULL)
	{
		error = ERROR_NO_FREE_STORE;

		PrintFault(error, "DAControl");
		goto out;
	}

	ap->ap_BreakBits = SIGBREAKF_CTRL_C;

	rda = ReadArgs((STRPTR)template, (LONG *)&options, NULL);
	if(rda == NULL)
	{
		error = IoErr();

		PrintFault(error, "DAControl");
		goto out;
	}

	/* The QUIET option always overrides the VERBOSE option. */
	if(options.Quiet)
		options.Verbose = FALSE;

	/* We need to know what to do, and that either
	 * requires an action to perform (load, eject or
	 * change a medium, start or stop a unit, create
	 * and load a new file) or show the current state
	 * of things.
	 */
	if(NOT options.Load &&
	   NOT options.Eject &&
	   NOT options.Change &&
	   NOT options.Start &&
	   NOT options.Stop &&
	   NOT options.Create &&
	   NOT options.Info)
	{
		error = ERROR_REQUIRED_ARG_MISSING;

		PrintFault(error, "DAControl");
		goto out;
	}

	/* Disable the write protection for media? */
	if(options.WriteProtected != NULL)
	{
		if(Stricmp(options.WriteProtected, "yes") == SAME)
		{
			if(options.Create)
			{
				Error(gd, "You cannot use both the CREATE and WRITEPROTECTED=YES options at the same time.");

				error = ERROR_TOO_MANY_ARGS;
				goto out;
			}

			write_protected = TRUE;
		}
		else if (Stricmp(options.WriteProtected, "no") == SAME)
		{
			write_protected = FALSE;
		}
		else
		{
			Error(gd, "The WRITEPROTECTED option must be either YES or NO.");

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}
	}

	/* A few quick checks to verify that combining
	 * options makes sense in the first place.
	 */

	if(options.Load && options.Eject)
	{
		Error(gd, "You cannot use both the LOAD and EJECT options at the same time.");

		error = ERROR_TOO_MANY_ARGS;
		goto out;
	}

	if(options.Load && options.Change)
	{
		Error(gd, "You cannot use both the LOAD and CHANGE options at the same time.");

		error = ERROR_TOO_MANY_ARGS;
		goto out;
	}

	if(options.Eject && options.Change)
	{
		Error(gd, "You cannot use both the EJECT and CHANGE options at the same time.");

		error = ERROR_TOO_MANY_ARGS;
		goto out;
	}

	if(options.Eject && options.Create)
	{
		Error(gd, "You cannot use both the EJECT and CREATE options at the same time.");

		error = ERROR_TOO_MANY_ARGS;
		goto out;
	}

	if(options.Stop && options.Create)
	{
		Error(gd, "You cannot use both the STOP and CREATE options at the same time.");

		error = ERROR_TOO_MANY_ARGS;
		goto out;
	}

	if(options.Start && options.Stop)
	{
		Error(gd, "You cannot use both the START and STOP options at the same time.");

		error = ERROR_TOO_MANY_ARGS;
		goto out;
	}

	if(options.Start && options.Eject)
	{
		Error(gd, "You cannot use both the START and EJECT options at the same time.");

		error = ERROR_TOO_MANY_ARGS;
		goto out;
	}

	if(options.Load && options.File == NULL)
	{
		Error(gd, "The LOAD option needs the name of the file/files to use.");

		error = ERROR_REQUIRED_ARG_MISSING;
		goto out;
	}

	if(options.Change && options.File == NULL)
	{
		requirements_satisfied = FALSE;

		if(options.WriteProtected != NULL)
			requirements_satisfied = TRUE;

		#if defined(ENABLE_CACHE)
		{
			if(options.EnableCache != NULL || options.CacheSize != NULL)
				requirements_satisfied = TRUE;
		}
		#endif /* ENABLE_CACHE */

		if(NO requirements_satisfied)
		{
			#if defined(ENABLE_CACHE)
			{
				Error(gd, "The CHANGE option needs the name of the file/files to use or the WRITEPROTECTED, ENABLECACHE or CACHESIZE options.");
			}
			{
				Error(gd, "The CHANGE option needs the name of the file/files to use or the WRITEPROTECTED option.");
			}
			#endif /* ENABLE_CACHE */

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}
	}

	if(options.FileSystemType != NULL && NOT options.Create)
	{
		Error(gd, "The FILESYSTEMTYPE option only works together with the CREATE option.");

		error = ERROR_REQUIRED_ARG_MISSING;
		goto out;
	}

	#if defined(ENABLE_CACHE)
	{
		/* Enable the cache for a disk image file, once it's loaded? */
		if(options.EnableCache != NULL)
		{
			if (Stricmp(options.EnableCache, "yes") == SAME)
			{
				enable_cache = TRUE;
			}
			else if (Stricmp(options.EnableCache, "no") == SAME)
			{
				enable_cache = FALSE;
			}
			else
			{
				Error(gd, "The ENABLECACHE option must be either YES or NO.");

				error = ERROR_REQUIRED_ARG_MISSING;
				goto out;
			}
		}

		/* Prefill the cache for a disk image file? Note that requesting the
		 * cache to be prefilled also enables caching for that disk image
		 * once it's loaded.
		 */
		if(options.PrefillCache != NULL)
		{
			if (Stricmp(options.PrefillCache, "yes") == SAME)
			{
				prefill_cache = enable_cache = TRUE;
			}
			else if (Stricmp(options.PrefillCache, "no") == SAME)
			{
				prefill_cache = FALSE;
			}
			else
			{
				Error(gd, "The PREFILLCACHE option must be either YES or NO.");

				error = ERROR_REQUIRED_ARG_MISSING;
				goto out;
			}
		}

		/* Request that the shared unit cache should be a specific
		 * size? This can result in the existing cache to be resized
		 * if needed.
		 */
		if(options.CacheSize != NULL)
		{
			cache_size = (*options.CacheSize);

			if(cache_size != 0 && cache_size < TF_MINIMUM_CACHE_SIZE && NOT options.Ignore)
			{
				if(NOT options.Quiet)
					Error(gd, "The minimum cache size must be %ld or greater.", TF_MINIMUM_CACHE_SIZE);

				error = ERROR_BAD_NUMBER;
				goto out;
			}
		}
	}
	#endif /* ENABLE_CACHE */

	/* Use disk and track checksums to detect disks with
	 * the same contents?
	 */
	if(options.UseChecksums != NULL)
	{
		if(Stricmp(options.UseChecksums, "yes") == SAME)
		{
			gd->gd_UseChecksums = TRUE;
		}
		else if (Stricmp(options.UseChecksums, "no") != SAME)
		{
			Error(gd, "The USECHECKSUMS option must be either YES or NO.");

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}
	}

	/* Use the safer and faster disk image removal
	 * option instead of waiting for the drive
	 * motor to spin down?
	 */
	if(options.SafeEject != NULL)
	{
		if(Stricmp(options.SafeEject, "yes") == SAME)
		{
			safe_eject = TRUE;
		}
		else if (Stricmp(options.SafeEject, "no") != SAME)
		{
			Error(gd, "The SAFEEJECT option must be either YES or NO.");

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}
	}

	/* Use a specific file system, to be loaded from disk,
	 * instead of the ROM default file system?
	 */
	if(options.FileSystem != NULL)
	{
		if(options.FileSystem[0] == '\0')
		{
			Error(gd, "The FILESYSTEM option needs the path and name of the file system to use.");

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}

		D(("trying to load file system '%s'", options.FileSystem));

		gd->gd_LoadedFileSystem = LoadSeg(options.FileSystem);
		if(gd->gd_LoadedFileSystem == ZERO)
		{
			error = IoErr();

			D(("that didn't work (error=%ld)", error));

			Error(gd, "Could not load file system \"%s\" (%s)",
				options.FileSystem,
				get_error_message(gd, error, error_message, sizeof(error_message)));

			goto out;
		}

		#if DEBUG
		{
			const struct Resident * rt;
			TEXT version_string[256];
			size_t version_string_len;

			rt = find_rom_tag(gd->gd_LoadedFileSystem);
			if(rt != NULL && rt->rt_IdString != NULL)
			{
				const TEXT * id_string = rt->rt_IdString;
				TEXT c;

				version_string_len = strlen(id_string);

				while(version_string_len > 0)
				{
					c = id_string[version_string_len - 1];
					if(c != '\r' && c != '\n')
						break;

					version_string_len--;
				}

				if(version_string_len > sizeof(version_string)-1)
					version_string_len = sizeof(version_string)-1;

				memmove(version_string, id_string, version_string_len);
				version_string[version_string_len] = '\0';

				D(("file system version = \"%s\"", version_string));
			}
			else
			{
				SHOWMSG("did not find a resident tag in the file system");
			}

			version_string_len = find_version_string(gd->gd_LoadedFileSystem, version_string, sizeof(version_string));
			if(version_string_len > 0)
			{
				TEXT c;

				while(version_string_len > 0)
				{
					c = version_string[version_string_len - 1];
					if(c != '\r' && c != '\n')
						break;

					version_string_len--;
				}

				version_string[version_string_len] = '\0';

				D(("file system version = \"%s\"", version_string));
			}
			else
			{
				SHOWMSG("did not find a version string in the file system");
			}
		}
		#endif /* DEBUG */

		gd->gd_LoadedFileSystemName = options.FileSystem;
	}

	/* The user wants to create a new file and load it? */
	if(options.Create)
	{
		/* Use a specific file system type? */
		if(options.FileSystemType != NULL)
		{
			const TEXT * fs_type = options.FileSystemType;
			size_t fs_type_len = strlen(fs_type);
			STRPTR fs_type_copy;
			const char separators[] = ",+ \t";
			const char * key;
			char * state;
			ULONG fs_signature_base = 0;
			const TEXT * flavour;
			BOOL international = FALSE;
			BOOL long_names = FALSE;
			BOOL dircache = FALSE;

			/* Make a copy of the file system type option
			 * text since local_strtok_r() will modify the string
			 * it processes.
			 */
			fs_type_copy = AllocVec(fs_type_len+1, MEMF_ANY);
			if(fs_type_copy == NULL)
			{
				SHOWMSG("not enough memory");

				error = ERROR_NO_FREE_STORE;

				Error(gd, "%s", get_error_message(gd, error, error_message, sizeof(error_message)));
				goto out;
			}

			CopyMem((APTR)fs_type, fs_type_copy, fs_type_len+1);

			error = OK;

			D(("fs_type_copy = '%s'", fs_type_copy));

			/* Now break it up into single keywords. Keywords may be
			 * separated by ",", "+" or blank space characters.
			 */
			for(key = local_strtok_r(fs_type_copy, separators, &state) ;
			    key != NULL ;
			    key = local_strtok_r(NULL, separators, &state))
			{
				D(("key = '%s'", key));

				/* Fast file system as the base? */
				if (Stricmp(key, "FFS") == SAME)
				{
					/* Nothing set yet? Then we just use it. */
					if (fs_signature_base == 0)
					{
						SHOWMSG("will use FFS");

						fs_signature_base = ID_FFS_DISK;
					}
					/* Something different was already set? */
					else if (fs_signature_base == ID_DOS_DISK)
					{
						SHOWMSG("already picked FFS");

						/* We complain about it. */
						error = ERROR_TOO_MANY_ARGS;

						Error(gd, "The FILESYSTEMTYPE option does not support both OFS and FFS at the same time.");
						break;
					}
				}
				/* Original file system as the base? */
				else if (Stricmp(key, "OFS") == SAME)
				{
					if (fs_signature_base == 0)
					{
						SHOWMSG("will use DOS");

						fs_signature_base = ID_DOS_DISK;
					}
					else if (fs_signature_base == ID_FFS_DISK)
					{
						SHOWMSG("already picked DOS");

						error = ERROR_TOO_MANY_ARGS;

						Error(gd, "The FILESYSTEMTYPE option does not support both OFS and FFS at the same time.");
						break;
					}
				}
				/* Now for the variants. */
				else
				{
					/* International mode (ISO 8859-1 character set hashing instead of US-ASCII). */
					if (Stricmp(key, "INTERNATIONAL") == SAME || Stricmp(key, "INTL") == SAME)
					{
						SHOWMSG("will use INTERNATIONAL option");

						international = TRUE;
					}
					/* Directory cache mode, also known as "fastdir mode". Nobody should be using this. */
					else if (Stricmp(key, "DIRCACHE") == SAME)
					{
						SHOWMSG("will use DIRCACHE option");

						dircache = TRUE;
					}
					/* Long name mode (up to 107 characters). */
					else if (Stricmp(key, "LONGNAMES") == SAME)
					{
						SHOWMSG("will use LONGNAMES option");

						long_names = TRUE;
					}
					/* Everything else we complain about. */
					else
					{
						error = ERROR_TOO_MANY_ARGS;

						Error(gd, "The FILESYSTEMTYPE option does not support \"%s\".", key);
						break;
					}
				}
			}

			FreeVec(fs_type_copy);

			if(error != OK)
				goto out;

			/* These variants are incompatible. */
			if(dircache && long_names)
			{
				error = ERROR_TOO_MANY_ARGS;

				Error(gd, "The FILESYSTEMTYPE option does not support both DIRCACHE and LONGNAMES at the same time.");
				goto out;
			}

			/* Unless specified otherwise, we stick with the
			 * original file system. It's better suited for
			 * floppy disks because the data blocks are
			 * protected by checksums which are more robust
			 * than the low-level disk format's checksum
			 * algorithm.
			 */
			if(fs_signature_base == 0)
			{
				SHOWMSG("no specific file system signature picked; will use OFS");

				fs_signature_base = ID_DOS_DISK;
			}

			/* Now put the base and the variations together. */
			if (dircache)
			{
				if(fs_signature_base == ID_DOS_DISK)
				{
					file_system_signature = ID_FASTDIR_DOS_DISK;

					flavour = "OFS directory cache";
				}
				else
				{
					file_system_signature = ID_FASTDIR_FFS_DISK;

					flavour = "FFS directory cache";
				}
			}
			else if (long_names)
			{
				if(fs_signature_base == ID_DOS_DISK)
				{
					file_system_signature = ID_LONG_DOS_DISK;

					flavour = "OFS long name";
				}
				else
				{
					file_system_signature = ID_LONG_FFS_DISK;

					flavour = "FFS long name";
				}
			}
			else if (international)
			{
				if(fs_signature_base == ID_DOS_DISK)
				{
					file_system_signature = ID_INTER_DOS_DISK;

					flavour = "OFS international mode";
				}
				else
				{
					file_system_signature = ID_INTER_FFS_DISK;

					flavour = "FFS international mode";
				}
			}
			else
			{
				file_system_signature = fs_signature_base;

				if(fs_signature_base == ID_DOS_DISK)
					flavour = "OFS";
				else
					flavour = "FFS";
			}

			D(("file_system_signature = 0x%08lx", file_system_signature));

			if(options.Verbose)
				Printf("Disk image will use file system type %s.\n", flavour);
		}

		if(options.File == NULL)
		{
			Error(gd, "If you want to create a disk file you also need to state its name.");

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}

		/* If the user didn't say otherwise, we imply LOAD if CREATE is used. */
		if(NOT options.Load && NOT options.Change)
			options.Start = options.Load = TRUE;

		/* If the medium's not writable, we can't format it... */
		write_protected = FALSE;
	}

	/* The disk type works in conjunction with the CREATE option. */
	if(options.DiskType)
	{
		if (Stricmp(options.DiskType, "DD") == SAME)
		{
			num_sectors = NUMSECS;
		}
		else if (Stricmp(options.DiskType, "HD") == SAME)
		{
			num_sectors = 2 * NUMSECS;
		}
		else
		{
			Error(gd, "The TYPE option must be either \"DD\" or \"HD\".");

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}
	}

	/* If a disk image file is to be created, it should get a
	 * volume label. The user can provide it, but it should
	 * be suitable as such. No path separator characters and
	 * no unprintable characters may be part of it.
	 */
	if(options.Label)
	{
		STRPTR label = options.Label;
		size_t len = strlen(label);
		BOOL label_is_valid = TRUE;
		size_t i;
		TEXT c;

		for(i = 0 ; i < len ; i++)
		{
			c = label[i];

			if(c == '/' || c == ':' || (c & 0x7F) < ((TEXT)' '))
			{
				label_is_valid = FALSE;
				break;
			}
		}

		if(NOT label_is_valid || len == 0)
		{
			Error(gd, "\"%s\" is not a valid volume label.", label);

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}

		/* We assume that the AmigaDOS default file system is
		 * being used, e.g. OFS/FFS, etc.
		 */
		if(len > MAX_ROOT_DIRECTORY_NAME_LEN)
		{
			Error(gd, "Volume label \"%s\" is too long (only up to %ld characters are supported).",
				label, MAX_ROOT_DIRECTORY_NAME_LEN);

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}
	}

	/* The TIMEOUT option works in conjunction with the
	 * EJECT/CHANGE options.
	 */
	if(options.Timeout != NULL)
	{
		timeout = (*options.Timeout);

		if(timeout < 5)
		{
			Error(gd, "The TIMEOUT must be at least 5 seconds long.");

			error = ERROR_BAD_NUMBER;
			goto out;
		}
	}

	/* The user provided a file system device name which
	 * should indicate the unit number to be used? Or
	 * maybe it's a unit number all along.
	 */
	if(options.Device != NULL)
	{
		struct DosList * dol;
		size_t len, i;

		/* Pick the next available unit? */
		if (Stricmp(options.Device, "ANY") == SAME)
		{
			use_next_available_unit = TRUE;

			SHOWMSG("will use the next available unit");

			if(options.Verbose)
				Printf("Using the next available unit, or will create one first.\n");
		}
		/* Is this a unit number? */
		else if (string_is_number(options.Device))
		{
			struct fs_startup_msg fsm;
			ULONG number;

			if(CANNOT convert_string_to_number(options.Device, &number) || ((LONG)number) < 0)
			{
				Error(gd, "The unit number \"%s\" is invalid.", options.Device);

				error = ERROR_BAD_NUMBER;
				goto out;
			}

			unit = (LONG)number;

			D(("using unit %ld", unit));

			unit_is_valid = TRUE;
			use_next_available_unit = FALSE;

			/* Let's try to match an AmigaDOS device name to
			 * that unit number.
			 */
			for(dol = NextDosEntry(LockDosList(LDF_DEVICES|LDF_READ), LDF_DEVICES) ;
			    dol != NULL ;
			    dol = NextDosEntry(dol, LDF_DEVICES))
			{
				if(decode_file_sys_startup_msg(gd, dol->dol_misc.dol_handler.dol_Startup, &fsm))
				{
					if(strcmp(fsm.fsm_device_name, TrackFileBase->lib_Node.ln_Name) == SAME &&
					   fsm.fsm_device_unit == unit)
					{
						const TEXT * device_name = (TEXT *)BADDR(dol->dol_Name);
						int device_name_len;

						device_name_len = (*device_name++);
						if(device_name_len > 0)
						{
							ASSERT( device_name_len <= sizeof(dos_device_name) );

							CopyMem((APTR)device_name, dos_device_name, device_name_len);
							dos_device_name[device_name_len] = '\0';

							dos_device_name_is_valid = TRUE;

							D(("unit %ld corresponds to '%s:'", unit, dos_device_name));
						}

						break;
					}
				}
			}

			UnLockDosList(LDF_DEVICES|LDF_READ);
		}
		/* It's either a keyword (e.g. "LAST") or a
		 * device name.
		 */
		else
		{
			BOOL name_found = FALSE;

			/* Check an environment variable which might
			 * hold the name of the last device used?
			 */
			if(Stricmp(options.Device, "LAST") == SAME)
			{
				BOOL name_is_valid = TRUE;
				TEXT device_text[260];

				SHOWMSG("will try to get the device name from env");

				if(GetVar((STRPTR)variable_name, device_text, sizeof(device_text), 0) <= 0)
				{
					error = IoErr();

					D(("that didn't work (error=%ld)", error));

					Error(gd, "The environment variable \"%s\" is not set.", variable_name);

					goto out;
				}

				len = strlen(device_text);

				for(i = 0 ; i < len ; i++)
				{
					if(device_text[i] == '/')
					{
						name_is_valid = FALSE;
						break;
					}

					if(device_text[i] == ':')
					{
						len = i;
						break;
					}
				}

				if(len > 255 || NOT name_is_valid || len == 0)
				{
					D(("cannot use that device name: %s", device_text));

					Error(gd, "The environment variable \"%s\" value is not valid.", variable_name);

					error = ERROR_REQUIRED_ARG_MISSING;
					goto out;
				}

				CopyMem(device_text, dos_device_name, len);
				dos_device_name[len] = '\0';

				D(("using device name %s", dos_device_name));

				if(options.Verbose)
					Printf("Using device \"%s:\" (from environment variable \"%s\").\n", dos_device_name, variable_name);
			}
			/* So this better be a device name, e.g. "DA0:". */
			else
			{
				BOOL name_is_valid = TRUE;

				D(("will try to use device '%s'", options.Device));

				/* The device name should be valid, which means that
				 * it ought to contain a colon character and it should
				 * not contain a file name separator character.
				 *
				 * We also make sure that it's not longer than 255
				 * characters since that's the limit for a BCPL
				 * string.
				 */
				len = strlen(options.Device);

				for(i = 0 ; i < len ; i++)
				{
					if(options.Device[i] == '/')
					{
						name_is_valid = FALSE;
						break;
					}

					if(options.Device[i] == ':')
					{
						len = i;
						break;
					}
				}

				if(NOT name_is_valid || len == 0)
				{
					SHOWMSG("that didn't work");

					Error(gd, "The device name \"%s\" is not valid.", options.Device);

					error = ERROR_OBJECT_WRONG_TYPE;;
					goto out;
				}

				if(len > 255)
				{
					SHOWMSG("name is too long");

					Error(gd, "The device name \"%s\" is too long.", options.Device);

					error = ERROR_BUFFER_OVERFLOW;
					goto out;
				}

				/* Let's see if we may be able to extract
				 * the unit number from the device name.
				 * We may find this useful later.
				 */
				if(2 < len && len < 255)
				{
					D(("'%s' could be a device name with embedded unit number", options.Device));

					/* This should be something like "DA0". */
					if(Strnicmp(options.Device, "DA", 2) == SAME)
					{
						TEXT device_name_copy[254];
						ULONG value;

						/* Keep what could be the unit number. */
						CopyMem(&options.Device[2], device_name_copy, len - 2);
						device_name_copy[len - 2] = '\0';

						if(convert_string_to_number(device_name_copy, &value))
						{
							requested_unit = (LONG)value;

							D(("'%s' looks like a number: %ld", device_name_copy, requested_unit));
						}
						else
						{
							D(("'%s' does not look like a number", device_name_copy));
						}
					}
					else
					{
						SHOWMSG("name does not begin with \"DA\", so ignoring it...");
					}
				}
				else
				{
					D(("'%s' is not a suitable device name with embedded unit number", options.Device));
				}

				CopyMem(options.Device, dos_device_name, len);
				dos_device_name[len] = '\0';

				D(("using device name %s", dos_device_name));
			}

			/* Let's see if this device name matches anything... */
			dol = FindDosEntry(LockDosList(LDF_DEVICES|LDF_READ), dos_device_name, LDF_DEVICES);
			if(dol != NULL)
			{
				struct fs_startup_msg fsm;

				name_found = TRUE;

				/* Now figure out if the exec device name matches
				 * what we expect.
				 */
				if(decode_file_sys_startup_msg(gd, dol->dol_misc.dol_handler.dol_Startup, &fsm))
				{
					if(strcmp(fsm.fsm_device_name, TrackFileBase->lib_Node.ln_Name) == SAME &&
					   fsm.fsm_device_unit >= 0)
					{
						unit = fsm.fsm_device_unit;

						unit_is_valid = TRUE;
						use_next_available_unit = FALSE;

						D(("will use device %s with unit %ld", dos_device_name, unit));

						dos_device_name_is_valid = TRUE;
					}
				}
			}

			UnLockDosList(LDF_DEVICES|LDF_READ);

			/* If we have a unit number to work with which
			 * is derived from the AmigaDOS device name,
			 * can we actually make good use of it? This
			 * option is really useful only with the
			 * LOAD and START command line options.
			 */
			if(requested_unit != -1 && NOT name_found && (options.Load || options.Start))
			{
				D(("will use unit %ld, derived from device name", requested_unit));

				/* Let's use the unit number we picked up. */
				unit = requested_unit;
				unit_is_valid = TRUE;

				if(options.Load)
					options.Start = TRUE;
			}
			else
			{
				if(NOT name_found)
				{
					SHOWMSG("didn't find the device name");

					Error(gd, "File system device \"%s:\" not found.", dos_device_name);

					error = ERROR_OBJECT_NOT_FOUND;
					goto out;
				}

				if(NOT dos_device_name_is_valid)
				{
					SHOWMSG("device name is not valid");

					Error(gd, "Cannot use file system device \"%s:\".", dos_device_name);

					error = ERROR_OBJECT_WRONG_TYPE;
					goto out;
				}
			}
		}
	}
	/* With no device/unit specified, try to pick the
	 * next available one.
	 */
	else
	{
		use_next_available_unit = TRUE;

		SHOWMSG("will use the next available unit");

		if(options.Verbose)
			Printf("Using the next available unit, or will create one first.\n");
	}

	if(options.Device == NULL || Stricmp(options.Device, "ANY") == SAME)
	{
		if(options.Start && NOT options.Load && NOT options.Change)
		{
			Error(gd, "The START option needs a DEVICE to work with.");

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}
	}

	if(options.Change && options.WriteProtected != NULL && options.File == NULL)
	{
		if(options.Create || options.Start || options.Stop || options.Load || options.Eject)
		{
			Error(gd, "Changing the write protection of an active unit cannot be combined with other actions.");

			error = ERROR_TOO_MANY_ARGS;
			goto out;
		}

		if(NOT unit_is_valid)
		{
			Error(gd, "To change the write protection of an active unit you need a DEVICE or UNIT to work with.");

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}
	}

	requirements_satisfied = FALSE;

	#if defined(ENABLE_CACHE)
	{
		/* Change whether the unit cache is enabled or the
		 * maximum cache size?
		 */
		if(options.Change && options.File == NULL)
		{
			if(options.EnableCache != NULL && unit_is_valid)
				requirements_satisfied = TRUE;

			if(options.EnableCache == NULL && options.CacheSize != NULL)
				requirements_satisfied = TRUE;
		}
	}
	#endif /* ENABLE_CACHE */

	if((options.Change || options.Eject) && NOT requirements_satisfied && (NOT unit_is_valid || use_next_available_unit))
	{
		Error(gd, "The EJECT and CHANGE options need a DEVICE or UNIT to work with.");

		error = ERROR_REQUIRED_ARG_MISSING;
		goto out;
	}

	/* Create a new disk image file? */
	if(options.Create)
	{
		/* This assumes an 80 cylinder, 2 heads, 3.5" floppy disk.
		 * The number of sectors follows from the type of disk,
		 * which is either double density (11) or high density (22).
		 */
		const LONG cylinder_size = NUMHEADS * num_sectors * TD_SECTOR;
		STRPTR file_name;
		int cyl, i;

		ASSERT( options.File != NULL );

		/* Just to be sure: the name should not be empty. */
		file_name = options.File[0];
		if(file_name[0] == '\0')
		{
			Error(gd, "The CREATE option needs a valid file name.");

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}

		/* Just like the "Format" command we will try to write
		 * complete "cylinders".
		 */
		cylinder_data = AllocVec(cylinder_size, MEMF_ANY|MEMF_PUBLIC);
		if(cylinder_data == NULL)
		{
			SHOWMSG("not enough memory");

			error = ERROR_NO_FREE_STORE;

			Error(gd, "%s", get_error_message(gd, error, error_message, sizeof(error_message)));
			goto out;
		}

		/* Try not to overwrite a file which already exists. */
		ASSERT(options.File != NULL);

		D(("will try to create file '%s'", file_name));

		/* Unless the user wants to overwrite the file, we
		 * check first if the file already exists (or something
		 * else exists that is not a file).
		 */
		if(NOT options.Overwrite)
		{
			BPTR lock;

			lock = Lock(file_name, SHARED_LOCK);
			if(lock != ZERO)
			{
				UnLock(lock);

				Error(gd, "You cannot overwrite \"%s\", which already exists.", file_name);

				error = ERROR_OBJECT_EXISTS;
				goto out;
			}
		}

		if(options.Verbose)
			Printf("Creating disk image file \"%s\".", file_name);

		SHOWMSG("trying to create that file");

		cylinder_file = Open(file_name, MODE_NEWFILE);
		if(cylinder_file == ZERO)
		{
			error = IoErr();

			D(("that didn't work (error=%ld)", error));

			Error(gd, "Could not create file \"%s\" (%s).",
				file_name, get_error_message(gd, error, error_message, sizeof(error_message)));

			goto out;
		}

		for(cyl = 0 ; cyl < num_cylinders ; cyl++)
		{
			/* Fill the track with the same pattern which
			 * the "Format" command would have used. Its
			 * original purpose is to create a different pattern
			 * for each cylinder so that the verification
			 * performed after a cylinder has been formatted
			 * can detect if it's a pattern for this cylinder
			 * or something else.
			 */
			for(i = 0 ; i < (int)(cylinder_size / sizeof(LONG)) ; i++)
				cylinder_data[i] = (cyl << 16) | i | ID_DOS_DISK;

			/* Until it's been formatted, the disk will
			 * remain unusable. This is accomplished by setting
			 * the file system signature to "BAD\0" in the very
			 * first sector of the disk file.
			 */
			if(cyl == 0)
				cylinder_data[0] = ID_BAD_DISK;

			if(Write(cylinder_file, cylinder_data, cylinder_size) == -1)
			{
				error = IoErr();

				D(("formatting failed on cylinder %ld with error %ld", cyl, error));

				Error(gd, "Could not write to file \"%s\" (%s).",
					file_name, get_error_message(gd, error, error_message, sizeof(error_message)));

				goto out;
			}
		}

		/* This is no longer needed to proceed. */
		FreeVec(cylinder_data);
		cylinder_data = NULL;

		Close(cylinder_file);
		cylinder_file = ZERO;

		/* This is a data file after all, not a program. */
		SetProtection(file_name, FIBF_EXECUTE);
	}

	/* Start a unit, or restart an unused one. */
	if (options.Start)
	{
		LONG new_unit;

		SHOWMSG("start a unit, or restart an unused one");

		/* Unless we already know which unit to use,
		 * we default to asking for the next
		 * available one.
		 */
		if(NOT unit_is_valid)
		{
			SHOWMSG("unit number is not valid; will try the next available unit");

			use_next_available_unit = TRUE;
			unit = -1;
		}

		/* Note: starting the unit will fill in the "dos_device_name"
		 *       variable, which will contain the name of the AmigaDOS
		 *       file system device which was started along with the
		 *       unit, e.g. "DA0".
		 */
		error = start_unit(gd,
			options.Verbose,
			unit,
			use_next_available_unit,
			cache_size,
			num_cylinders,
			num_sectors,
			&new_unit, /* <- This will be filled in */
			dos_device_name); /* <- This will be filled in */

		if(error != OK)
			goto out;

		dos_device_name_is_valid = (dos_device_name[0] != '\0');

		/* With the unit started, let's see if we
		 * should insert a medium, too.
		 */
		if(options.Load || options.Change)
		{
			ASSERT( new_unit >= 0 );

			if(NOT dos_device_name_is_valid)
			{
				D(("didn't obtain file system device name for unit %ld", new_unit));

				Error(gd, "File system device for unit %ld not found.", new_unit);

				error = ERROR_OBJECT_NOT_FOUND;
				goto out;
			}

			SHOWMSG("we need to insert a medium into that unit");

			/* Only insert a single medium. */
			error = insert_media_by_name(gd,
				options.Quiet,
				options.Verbose,
				options.Ignore,
				write_protected,
				enable_cache,
				prefill_cache,
				cache_size,
				ap,
				options.File,
				new_unit,	/* <- This was filled in above */
				FALSE,	/* <- Do not use the next available unit */
				num_cylinders,
				num_sectors,
				dos_device_name,	/* <- This was filled in above */
				1);	/* <- This calls for only 1 medium to be inserted */

			if(error != OK)
				goto out;
		}

		unit = new_unit;
	}
	/* Stop a currently active unit. */
	else if (options.Stop)
	{
		D(("stop a currently active unit; unit=%ld", unit));

		if(NOT unit_is_valid || use_next_available_unit)
		{
			Error(gd, "The STOP option needs a DEVICE or UNIT to work with.");

			error = ERROR_REQUIRED_ARG_MISSING;
			goto out;
		}

		/* Remove a medium first, if one is present. */
		if(options.Eject)
		{
			BOOL file_system_inhibited = FALSE;

			SHOWMSG("try to eject the medium first, if any");

			if(NOT unit_is_valid || use_next_available_unit)
			{
				Error(gd, "The EJECT option needs a DEVICE or UNIT to work with.");

				error = ERROR_REQUIRED_ARG_MISSING;
				goto out;
			}

			if(options.Verbose)
			{
				if(dos_device_name_is_valid)
					Printf("Ejecting medium from \"%s:\" (unit %ld) with timeout %lds.\n", dos_device_name, unit, timeout);
				else
					Printf("Ejecting medium from unit %ld with timeout %lds.\n", unit, timeout);
			}

			/* Try to tell the file system to shut down,
			 * so we may remove the medium safely.
			 */
			if(safe_eject)
			{
				if(dos_device_name_is_valid)
				{
					SHOWMSG("using safe eject");

					if(CANNOT inhibit_device(gd, dos_device_name, TRUE))
					{
						error = IoErr();

						Error(gd, "Could not eject medium from \"%s:\" (unit %ld) (%s).", dos_device_name, unit,
							get_error_message(gd, error, error_message, sizeof(error_message)));

						goto out;
					}

					file_system_inhibited = TRUE;
				}
				else
				{
					SHOWMSG("file system device name is not known; have to do without Inhibit(.., TRUE)");
				}
			}

			D(("eject unit %ld with timeout %ld", unit, timeout));

			error = TFEjectMediaTags(unit,
				TF_Timeout, timeout,
			TAG_DONE);

			if(file_system_inhibited)
				inhibit_device(gd, dos_device_name, FALSE);

			if(error != OK)
			{
				get_error_message(gd, error, error_message, sizeof(error_message));

				if(dos_device_name_is_valid)
					Error(gd, "The disk image file could not be ejected from \"%s:\" (unit %ld) (%s).", dos_device_name, unit, error_message);
				else
					Error(gd, "The disk image file could not be ejected from unit %ld (%s).", unit, error_message);

				goto out;
			}
		}

		if(options.Verbose)
		{
			if(dos_device_name_is_valid)
				Printf("Stopping unit %ld (\"%s:\").\n", unit, dos_device_name);
			else
				Printf("Stopping unit %ld.\n", unit);
		}

		error = TFStopUnitTags(unit, TAG_DONE);
		if(error != OK)
		{
			get_error_message(gd, error, error_message, sizeof(error_message));

			if(dos_device_name_is_valid)
				Error(gd, "Unit %ld (\"%s:\") could not be stopped (%s).", unit, dos_device_name, error_message);
			else
				Error(gd, "Unit %ld could not be stopped (%s).", unit, error_message);

			goto out;
		}
	}
	/* Change something about an active unit. */
	else if (options.Change && options.File == NULL)
	{
		/* We should change the write protection? */
		if(options.WriteProtected != NULL)
		{
			if(NOT dos_device_name_is_valid)
			{
				Error(gd, "File system device for unit %ld is not known.", unit);

				error = ERROR_REQUIRED_ARG_MISSING;
				goto out;
			}

			if(options.Verbose)
			{
				Printf("Changing medium in \"%s:\" (unit %ld) to be %s.\n",
					dos_device_name, unit,
					write_protected ? "write-protected" : "writable");
			}

			/* Start by taking the file system off the job, so that
			 * we may tinker with the disk write protection state.
			 */
			if(CANNOT inhibit_device(gd, dos_device_name, TRUE))
			{
				error = IoErr();

				Error(gd, "Could not inhibit file system on \"%s:\" (unit %ld) (%s).", dos_device_name, unit,
					get_error_message(gd, error, error_message, sizeof(error_message)));

				goto out;
			}

			/* Ask for the change to be made. */
			error = TFChangeUnitTags(unit,
				TF_WriteProtected, write_protected,
			TAG_DONE);

			/* Tell the file system to drop the needle on the record again. */
			inhibit_device(gd, dos_device_name, FALSE);

			/* The change may not have taken... */
			if(error != OK)
			{
				get_error_message(gd, error, error_message, sizeof(error_message));

				Error(gd, "Could not change write protection on \"%s:\" (unit %ld) (%s).", dos_device_name, unit, error_message);
				goto out;
			}
		}

		#if defined(ENABLE_CACHE)
		{
			/* Enable/disable the unit cache? */
			if(options.EnableCache != NULL)
			{
				if(options.Verbose)
				{
					Printf("%s cache on \"%s:\" (unit %ld).\n",
						enable_cache ? "Enabling" : "Disabling",
						dos_device_name, unit);
				}

				/* Ask for the change to be made. */
				error = TFChangeUnitTags(unit,
					TF_EnableUnitCache, enable_cache,
				TAG_DONE);

				if(error != OK)
				{
					get_error_message(gd, error, error_message, sizeof(error_message));

					Error(gd, "Could not %s cache on \"%s:\" (unit %ld) (%s).",
						enable_cache ? "enable" : "disable", dos_device_name, unit, error_message);

					goto out;
				}
			}

			/* Change the size of the shared unit cache or disable
			 * the cache altogether?
			 */
			if(options.CacheSize != NULL)
			{
				if(options.Verbose)
				{
					if(cache_size > 0)
						Printf("Changing the maximum cache size to %ld bytes.\n", cache_size);
					else
						Printf("Releasing the cache and turning it off.\n");
				}

				/* Ask for the change to be made. */
				error = TFChangeUnitTags(TFUNIT_CONTROL,
					TF_MaxCacheMemory, cache_size,
				TAG_DONE);

				if(error != OK)
				{
					get_error_message(gd, error, error_message, sizeof(error_message));

					Error(gd, "Could not change cache size (%s).", error_message);
					goto out;
				}
			}
		}
		#endif /* ENABLE_CACHE */
	}
	else
	{
		/* Remove a medium, if one is present. */
		if(options.Eject || options.Change)
		{
			BOOL file_system_inhibited = FALSE;

			D(("eject medium from unit %ld, if possible", unit));

			if(NOT unit_is_valid || use_next_available_unit)
			{
				Error(gd, "The EJECT and CHANGE options need a DEVICE or UNIT to work with.");

				error = ERROR_REQUIRED_ARG_MISSING;
				goto out;
			}

			if(options.Verbose)
			{
				if(dos_device_name_is_valid)
					Printf("Ejecting medium from \"%s:\" (unit %ld) with timeout %lds.\n", dos_device_name, unit, timeout);
				else
					Printf("Ejecting medium from unit %ld with timeout %lds.\n", unit, timeout);
			}

			/* Try to tell the file system to shut down,
			 * so we may remove the medium safely.
			 */
			if(safe_eject)
			{
				if(dos_device_name_is_valid)
				{
					SHOWMSG("using safe eject");

					if(CANNOT inhibit_device(gd, dos_device_name, TRUE))
					{
						error = IoErr();

						Error(gd, "Could not eject medium from \"%s:\" (unit %ld) (%s).", dos_device_name, unit,
							get_error_message(gd, error, error_message, sizeof(error_message)));

						goto out;
					}

					file_system_inhibited = TRUE;
				}
				else
				{
					SHOWMSG("file system device name is not known; have to do without Inhibit(.., TRUE)");
				}
			}

			D(("eject unit %ld with timeout %ld", unit, timeout));

			error = TFEjectMediaTags(unit,
				TF_Timeout, timeout,
			TAG_DONE);

			if(file_system_inhibited)
				inhibit_device(gd, dos_device_name, FALSE);

			if(error != OK)
			{
				get_error_message(gd, error, error_message, sizeof(error_message));

				if(dos_device_name_is_valid)
					Error(gd, "The disk image file could not be ejected from \"%s:\" (unit %ld) (%s).", dos_device_name, unit, error_message);
				else
					Error(gd, "The disk image file could not be ejected from unit %ld (%s).", unit, error_message);

				goto out;
			}
		}

		if(options.Load || options.Change)
		{
			LONG number_of_units;

			D(("insert and mount one or more disk image files"));

			/* If a disk file was created, we only want to
			 * load or change this one.
			 *
			 * Otherwise, all file names will be processed
			 * and the respective files will be used.
			 */
			if(options.Create)
			{
				SHOWMSG("we created this one file to use");
				number_of_units = 1;
			}
			else
			{
				SHOWMSG("we insert/mount as many files as we can");
				number_of_units = -1;
			}

			/* Insert as many media as provided, starting a
			 * new unit as needed.
			 */
			error = insert_media_by_name(gd,
				options.Quiet,
				options.Verbose,
				options.Ignore,
				write_protected,
				enable_cache,
				prefill_cache,
				cache_size,
				ap,
				options.File,
				unit,
				use_next_available_unit,
				num_cylinders,
				num_sectors,
				dos_device_name,	/* <- This may get overwritten */
				number_of_units);

			if(error != OK)
				goto out;
		}
	}

	/* If we created a disk image file, it's time to format it. */
	if(options.Create)
	{
		BOOL formatting_successful = FALSE;
		TEXT label[32];

		SHOWMSG("we created a new disk image file");

		/* Do we have a disk label ready to go? */
		if(options.Label != NULL)
		{
			local_strlcpy(label, options.Label, sizeof(label));
		}
		/* Make up a label which isn't likely to be
		 * currently in use right now.
		 */
		else
		{
			struct Device * TimerBase;
			struct timerequest tr;
			struct EClockVal ev;
			struct timeval now;
			ULONG key[2];

			/* We open timer.device with a minimum timerequest
			 * just for the sake of using ReadEClock() and
			 * GetSysTime().
			 */
			memset(&tr, 0, sizeof(tr));

			error = OpenDevice(TIMERNAME, UNIT_ECLOCK, (struct IORequest *)&tr, 0);
			if(error != OK)
			{
				Error(gd, "Could not open \"timer.device\" (%s).",
					get_error_message(gd, error, error_message, sizeof(error_message)));

				goto out;
			}

			TimerBase = tr.tr_node.io_Device;

			/* Time is fleeting... */
			ReadEClock(&ev);
			GetSysTime(&now);

			/* We combine the EClock counter and seconds information to avoid
			 * leaving the most significant 32 bit word of the EClock value
			 * zero after the system has just rebooted. For good measure, we
			 * then shake up the bits a bit more through a pseudo-random
			 * number generator.
			 */
			key[0] = xor_shift_32(ev.ev_hi ^ now.tv_secs);
			key[1] = xor_shift_32(ev.ev_lo ^ now.tv_micro);

			CloseDevice((struct IORequest *)&tr);

			/* This should produce a somewhat random label which
			 * hopefully will be unique. Better than naming all
			 * these disks "Empty", I suppose.
			 */
			local_snprintf(gd, label, sizeof(label), "Disk %04lx-%04lx-%04lx",
				key[0] & 0xffff,
				key[1] >> 16,
				key[1] & 0xffff);
		}

		D(("will use '%s' for the disk label", label));

		/* If the file system signature has not been
		 * specified, we fall back onto OFS. It's the
		 * traditional thing.
		 */
		if(file_system_signature == 0)
		{
			SHOWMSG("will format this disk image as OFS");

			file_system_signature = ID_DOS_DISK;
		}

		/* Shut down access to the file system for a bit. */
		if(inhibit_device(gd, dos_device_name, TRUE))
		{
			SHOWMSG("file system inhibited");

			if(options.Verbose)
				Printf("Disk image file \"%s\" is 'formatted' as \"%s\".\n", options.File[0], label);

			D(("formatting the volume as \"%s\"", label));

			/* Initialize both the file system signature in the reserved
			 * blocks as well as the root directory and the bitmap.
			 */
			if(format_device(gd, dos_device_name, label, file_system_signature))
			{
				error = OK;

				SHOWMSG("that seems to have worked");

				formatting_successful = TRUE;
			}
			else
			{
				error = IoErr();

				D(("formatting failed (error=%ld)", error));
			}

			/* And restart the file system. */
			inhibit_device(gd, dos_device_name, FALSE);
		}
		else
		{
			error = IoErr();

			D(("file system inhibit operation failed (error=%ld)", error));
		}

		if(NOT formatting_successful)
		{
			Error(gd, "Could not format disk image \"%s\" (%s).", options.File[0],
				get_error_message(gd, error, error_message, sizeof(error_message)));

			goto out;
		}

		/* Make the disk bootable? */
		if(options.Bootable)
		{
			struct Process * this_process = (struct Process *)FindTask(NULL);
			struct MsgPort * io_port = &this_process->pr_MsgPort;
			BOOL install_successful = FALSE;

			/* We can't just write to the file created. It must be
			 * done through the respective trackfile unit.
			 */
			io = (struct IOStdReq *)CreateIORequest(io_port, sizeof(*io));
			if(io == NULL)
			{
				SHOWMSG("out of memory");

				error = ERROR_NO_FREE_STORE;

				Error(gd, "Could not create I/O request (%s).",
					get_error_message(gd, error, error_message, sizeof(error_message)));

				goto out;
			}

			ASSERT( boot_block_size <= 2 * TD_SECTOR );

			/* Be nice and allocate memory for the boot block. */
			boot_block = AllocMem(boot_block_size, MEMF_ANY|MEMF_PUBLIC);
			if(boot_block == NULL)
			{
				SHOWMSG("out of memory");

				error = ERROR_NO_FREE_STORE;

				Error(gd, "Could not allocate memory for the boot block (%s).",
					get_error_message(gd, error, error_message, sizeof(error_message)));

				goto out;
			}

			D(("opening unit %ld", unit));

			error = OpenDevice(TrackFileBase->lib_Node.ln_Name, unit, (struct IORequest *)io, NULL);
			if(error != NULL)
			{
				D(("unit %ld didn't open (error=%ld)", unit, error));

				Error(gd, "Could not open \"%s\" unit %ld (%s).", TrackFileBase->lib_Node.ln_Name, unit,
					get_error_message(gd, error, error_message, sizeof(error_message)));

				goto out;
			}

			/* Read the two boot blocks, if possible. We want to be
			 * sure that we can, and that the file system signature
			 * which was used during formatting has persisted.
			 */
			io->io_Command		= CMD_READ;
			io->io_Offset		= 0;
			io->io_Length		= boot_block_size;
			io->io_Data			= boot_block;

			D(("boot block size = %ld bytes", boot_block_size));

			error = DoIO((struct IORequest *)io);
			if(error == OK)
			{
				/* That should be the file system signature
				 * we placed there.
				 */
				if(boot_block[0] == file_system_signature)
				{
					/* Image of the BootBlock of a 2.0 bootable disk. */
					static const UBYTE boot_block_code[] =
						"\0\0\3\x70\x43\xFA\0\x3E\x70\x25\x4E\xAE\xFD\xD8\x4A\x80\x67\xC\x22"
						"\x40\x8\xE9\0\6\0\x22\x4E\xAE\xFE\x62\x43\xFA\0\x18\x4E\xAE\xFF\xA0"
						"\x4A\x80\x67\xA\x20\x40\x20\x68\0\x16\x70\0\x4E\x75\x70\xFF\x4E\x75"
						"dos.library\0"
						"expansion.library";

					D(("boot block code size = %ld", sizeof(boot_block_code)));

					/* Initialize the boot block just like the
					 * "Install" command would do.
					 */
					memset(boot_block, 0, boot_block_size);

					/* Restore the file system signature. */
					boot_block[0] = file_system_signature;

					CopyMem((APTR)boot_block_code, &boot_block[2], sizeof(boot_block_code));

					/* This updates the boot block checksum, which will render
					 * the boot block bootable at last...
					 */
					boot_block[1] = ~calculate_boot_block_checksum(boot_block, boot_block_size);

					/* And write it back. */
					io->io_Command		= CMD_WRITE;
					io->io_Offset		= 0;
					io->io_Length		= boot_block_size;
					io->io_Data			= boot_block;

					error = DoIO((struct IORequest *)io);
					if(error == OK)
					{
						/* Make sure that the change will stick. */
						io->io_Command = CMD_UPDATE;

						error = DoIO((struct IORequest *)io);
						if(error == OK)
							install_successful = TRUE;
						else
							D(("could not write back boot block (error=%ld)", error));
					}
					else
					{
						D(("could not write back boot block (error=%ld)", error));
					}
				}
				else
				{
					D(("reserved block file system signature 0x%08lx does not match expected 0x%08lx",
						boot_block[0], file_system_signature));

					/* Sort of correct :-/ */
					error = ERROR_OBJECT_WRONG_TYPE;
				}
			}
			else
			{
				D(("could not read the boot block (error=%ld)", error));
			}

			/* Turn the motor back off. */
			io->io_Command	= TD_MOTOR;
			io->io_Length	= 0;

			DoIO((struct IORequest *)io);

			/* Say something if this didn't work out. */
			if(NOT install_successful)
			{
				if(dos_device_name_is_valid)
					Error(gd, "Could not make \"%s:\" (unit %ld) bootable (%s).", dos_device_name, unit, error_message);
				else
					Error(gd, "Could not make unit %ld bootable (%s).", unit, error_message);

				goto out;
			}
		}
	}

	/* Show the current state of the units. */
	if(options.Info)
	{
		struct TrackFileUnitData * first_tfud;
		BOOL header_printed = FALSE;

		/* We want to hear about all the currently known units. */
		first_tfud = TFGetUnitData(TFGUD_AllUnits);
		if(first_tfud != NULL)
		{
			struct TrackFileUnitData * tfud;
			TEXT checksum_text[16];

			struct Node * unit_nodes;
			struct Node * list_node;
			TEXT is_active[20];
			STRPTR is_writable;
			struct List unit_list;
			int num_units, i;
			BOOL is_busy;

			/* We need to present the units in ascending
			 * unit number order. So that needs a sorting
			 * data structure such as a table or a list.
			 *
			 * The order in which the unit data records
			 * are presented may be affected by how
			 * frequently the units are accessed. Hence
			 * the need to bring them into a well-defined
			 * order for display.
			 */
			num_units = 0;

			for(tfud = first_tfud ; tfud != NULL ; tfud = tfud->tfud_Next)
			{
				/* A size == 0 indicates that information about this
				 * unit is currently not available. For example, it could
				 * have been locked or shut down at the time the
				 * information was collected. This can happen in a
				 * multitasking system...
				 */
				if(tfud->tfud_Size > 0)
					num_units++;
			}

			unit_nodes = AllocVec(sizeof(*unit_nodes) * num_units, MEMF_ANY|MEMF_PUBLIC|MEMF_CLEAR);
			if(unit_nodes == NULL)
			{
				/* Don't forget to free the memory allocated
				 * for the unit data!
				 */
				TFFreeUnitData(first_tfud);

				error = ERROR_NO_FREE_STORE;

				Error(gd, "Could not obtain unit information (%s).",
					get_error_message(gd, error, error_message, sizeof(error_message)));

				goto out;
			}

			/* Each node in the table links to a unit. */
			NewList(&unit_list);

			for(tfud = first_tfud, i = 0 ;
			    i < num_units && tfud != NULL ;
			    tfud = tfud->tfud_Next, i++)
			{
				/* Only use units which currently have data to offer. */
				if(tfud->tfud_Size > 0)
				{
					/* Link the node to the unit data, then hook it
					 * up with the list, so that all nodes can be
					 * sorted.
					 */
					unit_nodes[i].ln_Name = (APTR)tfud;
					AddTail(&unit_list, &unit_nodes[i]);
				}
			}

			/* Sort by ascending unit number. */
			SortList(&unit_list, compare_by_unit_number);

			for(list_node = unit_list.lh_Head ;
			    list_node->ln_Succ != NULL ;
			    list_node = list_node->ln_Succ)
			{
				if(CheckSignal(SIGBREAKF_CTRL_C))
				{
					error = ERROR_BREAK;
					break;
				}

				tfud = (struct TrackFileUnitData *)list_node->ln_Name;

				if(NOT header_printed)
				{
					Printf("%-06s  %-7s  ",
						"Device",
						"Type"
					);

					if(options.ShowChecksums)
						Printf("%-11s  ", "Checksum");

					if(options.ShowVolumes)
					{
						Printf("%-31s  ", "Volume name");
						Printf("%-19s  ", "Volume date");
					}

					if(options.ShowBootblocks)
					{
						Printf("%-12s  ", "File system");
						Printf("%-8s  ", "Bootable");
					}

					#if defined(ENABLE_CACHE)
					{
						if(options.ShowCaches)
						{
							Printf("%-11s  ", "Caching");
							Printf("%-11s  ", "Cache rate");
						}
					}
					#endif /* ENABLE_CACHE */

					Printf("%-6s  %-10s  %s\n",
						"Active",
						"Access",
						"File"
					);

					header_printed = TRUE;
				}

				if(tfud->tfud_MediumIsPresent && tfud->tfud_IsActive)
				{
					is_writable	= tfud->tfud_IsWritable ? "read/write" : "read-only";
					is_busy		= tfud->tfud_IsBusy;
				}
				else
				{
					is_writable	= "-";
					is_busy		= FALSE;
				}

				/* The "is active" column combines information on whether
				 * the unit process is still operational as well if the
				 * unit's motor is enabled and the track buffer contains
				 * data that needs to be written back. The latter is
				 * indicated by the '*' added to the column.
				 */
				local_snprintf(gd, is_active, sizeof(is_active), "%s %lc",
					tfud->tfud_IsActive ? "Yes" : "No",
					is_busy ? '*' : ' '
				);

				if(tfud->tfud_MediumIsPresent && tfud->tfud_ChecksumsEnabled)
					tf_checksum_to_text(&tfud->tfud_Checksum, checksum_text);
				else
					strcpy(checksum_text, "-");

				Printf("%-06s  %-7s  ",
					tfud->tfud_DeviceName != NULL ? tfud->tfud_DeviceName : (STRPTR)"-",
					tfud->tfud_DriveType == DRIVE3_5 ? "3.5\" DD" : "3.5\" HD"
				);

				if(options.ShowChecksums)
					Printf("%-11s  ", checksum_text);

				if(tfud->tfud_MediumIsPresent && tfud->tfud_Size > offsetof(struct TrackFileUnitData, tfud_VolumeValid))
				{
					if(options.ShowVolumes)
					{
						Printf("%-31s  ", tfud->tfud_VolumeValid ? tfud->tfud_VolumeName : (STRPTR)"-" );

						if(tfud->tfud_VolumeValid)
						{
							TEXT date[LEN_DATSTRING], time[LEN_DATSTRING];
							struct DateTime dat;

							memset(&dat, 0, sizeof(dat));

							dat.dat_Stamp	= tfud->tfud_VolumeDate;
							dat.dat_Format	= FORMAT_DOS;
							dat.dat_StrDate	= date;
							dat.dat_StrTime	= time;

							if(DateToStr(&dat))
								Printf("%-9s %-8s  ", date, time);
							else
								Printf("%-19s  ", "-");
						}
						else
						{
							Printf("%-19s  ", "-");
						}
					}

					if(options.ShowBootblocks)
					{
						TEXT file_sys_signature[10];

						local_snprintf(gd, file_sys_signature, sizeof(file_sys_signature),
							"%08lx", tfud->tfud_FileSysSignature);

						Printf("%-12s  ", file_sys_signature);
						Printf("%-8s  ", tfud->tfud_BootBlockChecksum == 0xffffffff ? "Yes" : "No");
					}

					#if defined(ENABLE_CACHE)
					{
						if(options.ShowCaches)
						{
							if(tfud->tfud_Size > offsetof(struct TrackFileUnitData, tfud_CacheEnabled))
							{
								Printf("%-11s  ", tfud->tfud_CacheEnabled ? "Yes" : "No");

								if(tfud->tfud_CacheAccesses > 0)
								{
									ULONG hit_count = tfud->tfud_CacheAccesses - tfud->tfud_CacheMisses;
									ULONG hit_percent;
									TEXT percent[10];

									hit_percent = (10000 * hit_count) / tfud->tfud_CacheAccesses;

									local_snprintf(gd, percent, sizeof(percent), "%3ld.%02ld%%",
										hit_percent / 100, hit_percent % 100);

									Printf("%-11s  ", percent);
								}
								else
								{
									Printf("%-11s  ", "-");
								}
							}
							else
							{
								Printf("%-11s  ", "-");
								Printf("%-11s  ", "-");
							}
						}
					}
					#endif /* ENABLE_CACHE */
				}
				else
				{
					if(options.ShowVolumes)
					{
						Printf("%-31s  ", "-");
						Printf("%-19s  ", "-");
					}

					if(options.ShowBootblocks)
					{
						Printf("%-12s  ", "-");
						Printf("%-8s  ", "-");
					}

					#if defined(ENABLE_CACHE)
					{
						if(options.ShowCaches)
						{
							Printf("%-11s  ", "-");
							Printf("%-11s  ", "-");
						}
					}
					#endif /* ENABLE_CACHE */
				}

				Printf("%-6s  %-10s  %s\n",
					is_active,
					is_writable,
					tfud->tfud_FileName != NULL ? tfud->tfud_FileName : (STRPTR)"-"
				);
			}

			FreeVec(unit_nodes);

			TFFreeUnitData(first_tfud);
		}
		else
		{
			/* Did TFGetUnitData() return NULL because there is no unit
			 * information available, or because of an actual error during
			 * unit data collection?
			 */
			error = IoErr();
			if(error != OK)
			{
				Error(gd, "Could not obtain unit information (%s).",
					get_error_message(gd, error, error_message, sizeof(error_message)));

				goto out;
			}
		}

		if(error == OK && NOT header_printed)
			Printf("No units have been started yet.\n");
	}

	/* If requested, save the AmigaDOS device
	 * name in a dedicated environment variable.
	 */
	if(dos_device_name[0] != '\0')
	{
		/* The AmigaDOS device name does not contain a ":"
		 * which would serve as a path separator. We
		 * add it here to the environment variables because
		 * it's more useful in script files, for example,
		 * to be able to combine device and directory
		 * names.
		 */
		local_strlcat(dos_device_name, ":", sizeof(dos_device_name));

		if(options.SetEnv)
		{
			D(("storing global env var %s=%s", variable_name, dos_device_name));
			SetVar((STRPTR)variable_name, dos_device_name, strlen(dos_device_name), GVF_GLOBAL_ONLY);
		}

		if(options.SetVar)
		{
			D(("storing local env var %s=%s", variable_name, dos_device_name));
			SetVar((STRPTR)variable_name, dos_device_name, strlen(dos_device_name), GVF_LOCAL_ONLY);
		}
	}

	rc = RETURN_OK;

 out:

	if(error == ERROR_BREAK)
	{
		PrintFault(error, "DAControl");

		rc = RETURN_WARN;
	}

	if(gd->gd_DevProc != NULL)
		FreeDeviceProc(gd->gd_DevProc);

	if(cylinder_data != NULL)
		FreeVec(cylinder_data);

	if(cylinder_file != ZERO)
	{
		Close(cylinder_file);

		DeleteFile(options.File[0]);
	}

	if(boot_block != NULL)
		FreeMem(boot_block, boot_block_size);

	if(gd->gd_LoadedFileSystem != ZERO && NOT gd->gd_LoadedFileSystemUsed)
	{
		SHOWMSG("unloading unused file system");

		UnLoadSeg(gd->gd_LoadedFileSystem);
	}

	if(io != NULL)
	{
		if(io->io_Device != NULL)
			CloseDevice((struct IORequest *)io);

		DeleteIORequest((struct IORequest *)io);
	}

	if(TrackFileBase != NULL)
		CloseDevice((struct IORequest *)&gd->gd_TrackFileDevice);

	if(rda != NULL)
		FreeArgs(rda);

	if(ap != NULL)
		FreeVec(ap);

	if(rc != RETURN_OK && error != OK)
		SetIoErr(error);

	return(rc);
}

/****************************************************************************/

/* This is used by the SortList() function, for the DAControl
 * "INFO" option to show the trackfile.device units in
 * ascending order.
 */
static int
compare_by_unit_number(const struct Node *a, const struct Node *b)
{
	ULONG unit_a = ((struct TrackFileUnitData *)a->ln_Name)->tfud_UnitNumber;
	ULONG unit_b = ((struct TrackFileUnitData *)b->ln_Name)->tfud_UnitNumber;
	int result;

	if (unit_a < unit_b)
		result = -1;
	else if (unit_a == unit_b)
		result = 0;
	else
		result = 1;

	return(result);
}
