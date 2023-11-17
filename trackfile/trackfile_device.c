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

#include "trackfile.device_rev.h"

/****************************************************************************/

#include "commands.h"
#include "functions.h"
#include "unit.h"

/****************************************************************************/

/* Local function prototypes only. */
static struct TrackFileDevice *ASM dev_init(REG (d0, struct TrackFileDevice *tfd ), REG (a0, BPTR segment_list ), REG (a6, struct Library *SysBase ));
static VOID dev_exit(struct TrackFileDevice *tfd);
static LONG ASM dev_open(REG (a1, struct IORequest *io ), REG (d0, LONG unit_number ), REG (d1, ULONG flags ), REG (a6, struct TrackFileDevice *tfd ));
static BPTR ASM dev_close(REG (a1, struct IORequest *io ), REG (a6, struct TrackFileDevice *tfd ));
static BPTR ASM dev_expunge(REG (a6, struct TrackFileDevice *tfd ));
static ULONG dev_reserved(VOID);
static LONG ASM dev_abort_io(REG (a1, struct IORequest *which_io ), REG (a6, struct TrackFileDevice *tfd ));
static VOID ASM dev_begin_io(REG (a1, struct IORequest *io ), REG (a6, struct TrackFileDevice *tfd ));

/****************************************************************************/

/* Device entry point; should always return -1 to discourage
 * starting it from the shell. This must not be a static
 * function!
 */
LONG
_start(void)
{
	return(-1);
}

/***********************************************************************/

/* Function table definition required for the device initialization table */
static const APTR function_table[] =
{
	/* Standard system routines */
	dev_open,
	dev_close,
	dev_expunge,
	dev_reserved,

	/* Mandatory device functions */
	dev_begin_io,
	dev_abort_io,

	/* This device's peculiar functionality */
	tf_start_unit_taglist,
	tf_stop_unit_taglist,
	tf_insert_media_taglist,
	tf_eject_media_taglist,
	tf_get_unit_data,
	tf_free_unit_data,
	tf_change_unit_taglist,
	tf_examine_file_size,

	/* Function table end marker */
	(APTR)-1
};

/***********************************************************************/

/* The "rom_tag" below specifies that we are "RTF_AUTOINIT". This means
 * that the RT_INIT structure member points to one of these tables below.
 */
static const struct InitTable init_table =
{
	sizeof(struct TrackFileDevice),
	(APTR)function_table,
	NULL, /* unused */
	dev_init
};

/***********************************************************************/

/* NOTE: This cannot be declared as static, since otherwise the
 *       compiler will optimize it out. It must be visible for ramlib
 *       to find the Resident tag during the OpenDevice() call.
 */
const struct Resident rom_tag =
{
	RTC_MATCHWORD,					/* (UWORD) rt_MatchWord (Magic cookie) */
	(struct Resident *)&rom_tag,	/* (struct Resident *) rt_MatchTag (Back pointer) */
	(struct Resident *)&rom_tag+1,	/* (struct Resident *) rt_EndSkip (Points behind ROM tag) */
	RTF_AUTOINIT|RTF_AFTERDOS,		/* (UBYTE) rt_Flags (Magic--see dev_init() function) */
	VERSION,						/* (UBYTE) rt_Version */
	NT_DEVICE,						/* (UBYTE) rt_Type (must be correct) */
	0,								/* (BYTE) rt_Pri */
	"trackfile.device",				/* (char *) rt_Name (Exec device name) */
	VSTRING,						/* (char *) rt_IDString (text string) */
	(APTR)&init_table				/* (APTR) rt_Init */
};

/***********************************************************************/

/* Note: the device initialization function may be called with as
 *       little as 2K of stack space. Careful how you use that!
 */
static struct TrackFileDevice * ASM
dev_init(
	REG(d0, struct TrackFileDevice *	tfd),
	REG(a0, BPTR						segment_list),
	REG(a6, struct Library *			SysBase))
{
	struct TrackFileDevice * result = NULL;

	/* Because of rom_tag.rt_Flags having the RTF_AUTOINIT flag
	 * set we can use the ready-made device base. Otherwise
	 * we would have had to call MakeLibrary() here before
	 * initializing the device fields.
	 */
	tfd->tfd_SysBase = SysBase;
	tfd->tfd_SegList = segment_list;

	/* This enables full debug output. */
	SETDEBUGLEVEL(2);

	SETPROGRAMNAME(tfd->tfd_Device.dd_Library.lib_Node.ln_Name);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	/* The library/device initialization will automaticaly fill
	 * in the version number, but the revision number is our
	 * responsibility to fill in.
	 */
	tfd->tfd_Device.dd_Library.lib_Revision = REVISION;

	InitSemaphore(&tfd->tfd_Lock);

	NewMinList(&tfd->tfd_UnitList);

	/* Kickstart 2.04 or higher required. */
	tfd->tfd_DOSBase = OpenLibrary("dos.library", 37);
	if(tfd->tfd_DOSBase == NULL)
		goto out;

	tfd->tfd_UtilityBase = OpenLibrary("utility.library", 37);
	if(tfd->tfd_UtilityBase == NULL)
		goto out;

	result = tfd;

 out:

	if(result == NULL)
		dev_exit(tfd);

	RETURN(result);
	return(result);
}

/***********************************************************************/

static VOID
dev_exit(struct TrackFileDevice *tfd)
{
	USE_EXEC(tfd);

	ENTER();

	if(tfd->tfd_DOSBase != NULL)
		CloseLibrary(tfd->tfd_DOSBase);

	if(tfd->tfd_UtilityBase != NULL)
		CloseLibrary(tfd->tfd_UtilityBase);

	FreeMem(((BYTE *)tfd) - tfd->tfd_Device.dd_Library.lib_NegSize,
		tfd->tfd_Device.dd_Library.lib_NegSize + tfd->tfd_Device.dd_Library.lib_PosSize);

	LEAVE();
}

/***********************************************************************/

/****** trackfile.device/--background-- **************************************
*
*   FUNCTION
*	The trackfile device is a "virtual" trackdisk.device which provides
*	access (read-only and read-write) to disk image files (ADF). Its chief
*	goal is to emulate the behaviour and the functionality of
*	trackdisk.device as well as possible for maximum compatibility.
*
*	This emulation includes the behaviour of commands such as
*	TD_ADDCHANGEINT and TD_REMCHANGEINT and their respective
*	idiosyncrasies. Software which fails to use these correctly with
*	trackfile.device will also fail to work correctly with
*	trackdisk.device or scsi.device, for example.
*
*	That said, trackdisk.device performs additional sanity checks and
*	validation on the commands it processes which trackdisk.device or
*	scsi.device do not. If such commands are rejected (with error) by
*	trackfile.device then chances are that trackdisk.device or scsi.device
*	may exhibit undefined behaviour under the same circumstances. For
*	example, the trackdisk.device command TD_RAWREAD is documented to
*	require that the destination buffer has to reside in chip memory but
*	trackdisk.device never checks if this requirement is satisfied.
*
*   UNITS
*	Whereas trackdisk.device was limited to up to 4 units in operation, as
*	set at power-up time, virtual drives can be added to the trackfile
*	device as needed. Other than the available memory, there is no limit
*	to how many units can be in use at a time.
*
*	A unit supports ADF disk image files corresponding to an Amiga floppy
*	disk with 880 KBytes of storage space (3.5", double density) or 1760 KBytes
*	of storage space (3.5", high density). 40 track floppy disks (5.25")
*	are not currently supported. The disk format itself is fixed to the
*	Amiga 1.0 format and other formats, such as the IBM 720 KByte or 1440
*	KByte formats, are unsupported.
*
*   LIBRARY
*	In addition to the normal device calls, the trackfile device also
*	supports several direct, library like calls. These are used for
*	loading, ejecting and managing the ADF image files, for example.
*
*	In order to use the library calls, trackfile.device must be
*	opened with unit TFUNIT_CONTROL.
*
*****************************************************************************
*/

/* Note: the device open function may be called with as little
 *       as 2K of stack space. Careful how you use that!
 */
static LONG ASM
dev_open(
	REG(a1, struct IORequest *			io),
	REG(d0, LONG						unit_number),
	REG(d1, ULONG						flags),
	REG(a6, struct TrackFileDevice *	tfd))
{
	USE_EXEC(tfd);

	struct TrackFileUnit * which_tfu = NULL;
	LONG error;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	SHOWVALUE(unit_number);
	SHOWVALUE(flags);

	/* IMPORTANT: Mark IORequest as "complete" or otherwise CheckIO() may
	 *            consider it as "in use" in spite of never having been
	 *            used at all. This also avoids that WaitIO() will hang
	 *            on an IORequest which has never been used.
	 */
	io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;

	/* Subtle point: Any AllocMem() call can cause a call to this device's
	 *               expunge vector. If lib_OpenCnt is zero, the device
	 *               might get expunged.
	 */
	tfd->tfd_Device.dd_Library.lib_OpenCnt++; /* Fake an opener for duration of call */

	/* Let's see if the unit number requested is valid.
	 * The number -1 (TFUNIT_CONTROL) stands for the
	 * control unit which provides access to the API
	 * functions of this device and is therefore always
	 * permitted.
	 */
	if(unit_number != TFUNIT_CONTROL)
	{
		SHOWMSG("trying to find a specific unit");

		/* Is the requested unit not currently in operation? */
		which_tfu = find_unit_by_number(tfd, unit_number);
		if(which_tfu == NULL)
		{
			SHOWMSG("didn't find it");

			error = IOERR_OPENFAIL;
			goto out;
		}
	}

	io->io_Unit = (struct Unit *)which_tfu;

	/* Mark us as having another opener. */
	tfd->tfd_Device.dd_Library.lib_OpenCnt++;

	/* If this is a real unit, update its usage count, etc. */
	if(which_tfu != NULL)
		which_tfu->tfu_Unit.tdu_Unit.unit_OpenCnt++;

	/* Prevent delayed expunges. We're open! */
	CLEAR_FLAG(tfd->tfd_Device.dd_Library.lib_Flags, LIBF_DELEXP);

	SHOWMSG("that went well");

	/* Success */
	error = OK;

 out:

	/* Did the unit open? */
	if(error != OK)
	{
		/* IMPORTANT: Invalidate io_Device on open failure. */
		io->io_Device = NULL;
	}

	/* Save the error code. */
	io->io_Error = error;

	/* End of expunge protection. */
	tfd->tfd_Device.dd_Library.lib_OpenCnt--;

	SHOWVALUE(tfd->tfd_Device.dd_Library.lib_OpenCnt);

	RETURN(error);
	return(error);
}

/***********************************************************************/

/* If possible, call the expunge function through the
 * library vector instead of calling the local
 * function directly.
 */
#if defined(__SASC)
#pragma libcall TrackFileBase DeviceExpunge 12 00
extern BPTR DeviceExpunge(VOID);
#endif /* __SASC */

static BPTR ASM
dev_close(REG(a1, struct IORequest *io), REG(a6, struct TrackFileDevice *tfd))
{
	USE_EXEC(tfd);

	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	BPTR result = ZERO;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	/* IMPORTANT: Make sure the IORequest is not used again with a NULL in
	 *            io_Device. Any BeginIO() attempt will immediately halt,
	 *            which is better than a subtle corruption that will lead
	 *            to hard-to-trace crashes.
	 */
	io->io_Unit		= NULL;
	io->io_Device	= NULL;

	/* This may be NULL for the control unit. */
	if(tfu != NULL)
	{
		/* If this is a real unit, update its usage count, etc. */
		if(tfu->tfu_Unit.tdu_Unit.unit_OpenCnt > 0)
			tfu->tfu_Unit.tdu_Unit.unit_OpenCnt--;

		SHOWVALUE(tfu->tfu_Unit.tdu_Unit.unit_OpenCnt);

		/* Write back any track buffer changes and turn the
		 * motor off, just in case this user forgot to
		 * do either.
		 */
		tfu->tfu_TurnMotorOff = TRUE;
	}

	/* Mark us as having one fewer opener. */
	if(tfd->tfd_Device.dd_Library.lib_OpenCnt > 0)
		tfd->tfd_Device.dd_Library.lib_OpenCnt--;

	SHOWVALUE(tfd->tfd_Device.dd_Library.lib_OpenCnt);

	/* See if there is anyone left with us open. */
	if(tfd->tfd_Device.dd_Library.lib_OpenCnt == 0)
	{
		/* See if we have a delayed expunge pending, so that
		 * we may finally get to act upon it.
		 */
		if(FLAG_IS_SET(tfd->tfd_Device.dd_Library.lib_Flags, LIBF_DELEXP))
		{
			SHOWMSG("we have a delayed expunge pending");

			/* If possible, call the expunge function through the
			 * library vector instead of calling the local
			 * function directly.
			 */
			#if defined(__SASC)
			{
				struct TrackFileDevice * TrackFileBase = tfd;

				result = DeviceExpunge();
			}
			#else
			{
				result = dev_expunge(tfd);
			}
			#endif /* __SASC */

			D(("expunge returned 0x%08lx", result));
		}
		else
		{
			SHOWMSG("still can't quit yet (expunge not pending)");
		}
	}

	/* MUST return either zero or the SegList!!! */
	RETURN(result);
	return(result);
}

/***********************************************************************/

static BPTR ASM
dev_expunge(REG(a6, struct TrackFileDevice *tfd))
{
	USE_EXEC(tfd);

	BPTR result = ZERO;
	BOOL can_quit;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	/* See if anyone has us open. */
	can_quit = (tfd->tfd_Device.dd_Library.lib_OpenCnt == 0);

	/* Looks good so far. Check if one of the units is still active. */
	if(can_quit)
	{
		/* Note: The expunge function must not break the Forbid()
		 *       state which means that we cannot call ObtainSemaphore()
		 *       here. So we need to try with AttemptSemaphore() instead,
		 *       and if that does not work out, we'll just assume that
		 *       we cannot quit.
		 */
		SHOWMSG("trying to obtain device lock");

		if(AttemptSemaphore(&tfd->tfd_Lock))
		{
			struct TrackFileUnit * tfu;

			/* We can't quit until the last user has closed the unit
			 * and we also need to be careful not to close before
			 * the last disk change interrupt has been safely removed.
			 */
			for(tfu = (struct TrackFileUnit *)tfd->tfd_UnitList.mlh_Head ;
				tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ != NULL ;
				tfu = (struct TrackFileUnit *)tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ)
			{
				if(tfu->tfu_Unit.tdu_Unit.unit_OpenCnt > 0 ||		/* Unit still in use */
				   NOT IsMinListEmpty(&tfu->tfu_ChangeIntList) ||	/* Change notification still present */
				   tfu->tfu_RemoveInt != NULL)						/* Legacy change notification still present */
				{
					can_quit = FALSE;
					break;
				}
			}

			SHOWMSG("releasing device lock");
			ReleaseSemaphore(&tfd->tfd_Lock);
		}
		else
		{
			SHOWMSG("could not obtain device lock; will assume that we cannot quit yet");

			can_quit = FALSE;
		}
	}

	if(can_quit)
	{
		SHOWMSG("shutting down the device");

		#if defined(ENABLE_CACHE)
		{
			if(tfd->tfd_CacheContext == NULL)
				delete_cache_context(tfd->tfd_CacheContext);
		}
		#endif /* ENABLE_CACHE */

		/* Go ahead and get rid of us. */
		result = tfd->tfd_SegList;

		/* Unlink from device list. */
		Remove(&tfd->tfd_Device.dd_Library.lib_Node);

		dev_exit(tfd);
	}
	else
	{
		SHOWMSG("cannot quit yet: device is still active");

		/* It is still in use. Set the delayed expunge flag. */
		SET_FLAG(tfd->tfd_Device.dd_Library.lib_Flags, LIBF_DELEXP);
	}

	RETURN(result);
	return(result);
}

/***********************************************************************/

static ULONG
dev_reserved(VOID)
{
	ENTER();

	SHOWMSG("who's this?");

	RETURN(0);
	return(0);
}

/***********************************************************************/

static LONG ASM
dev_abort_io(
	REG(a1, struct IORequest *			which_io),
	REG(a6, struct TrackFileDevice *	tfd))
{
	USE_EXEC(tfd);

	struct TrackFileUnit * tfu = (struct TrackFileUnit *)which_io->io_Unit;
	LONG error = OK;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	SHOWPOINTER(which_io);

	#if DEBUG
	{
		struct TrackFileUnit * which_tfu;
		BOOL unit_found = FALSE;

		Forbid();

		for(which_tfu = (struct TrackFileUnit *)tfd->tfd_UnitList.mlh_Head ;
			which_tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ != NULL ;
			which_tfu = (struct TrackFileUnit *)which_tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ)
		{
			if(which_tfu == tfu)
			{
				unit_found = TRUE;
				break;
			}
		}

		Permit();

		if(NOT unit_found)
			D(("unit 0x%08lx seems to be invalid", tfu));
	}
	#endif /* DEBUG */

	/* Sanity first... */
	if(which_io->io_Device == (struct Device *)tfd && tfu != NULL)
	{
		struct IORequest * io;

		/* Check if the request is still in the queue, waiting to be
		 * processed; this *must* be done under Disable() conditions
		 * because interrupt code can end up attaching I/O requests
		 * to the unit port.
		 *
		 * Note that an active TD_ADDCHANGEINT command which has
		 * been processed and has its I/O request queued in the
		 * tfu->tfu_ChangeIntList will not be "aborted" here.
		 * Neither trackdisk.device nor scsi.device even permit
		 * aborting I/O requests in the first place. Question is
		 * whether the TD_ADDCHANGEINT command should be subject
		 * to AbortIO().
		 */
		Disable();

		for(io = (struct IORequest *)tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_MsgList.lh_Head ;
		    io->io_Message.mn_Node.ln_Succ != NULL ;
		    io = (struct IORequest *)io->io_Message.mn_Node.ln_Succ)
		{
			if(io == which_io)
			{
				/* Remove it from the queue and tag it as aborted. */
				Remove(&io->io_Message.mn_Node);

				ASSERT( (io->io_Flags & IOF_QUICK) == 0 );

				error = io->io_Error = IOERR_ABORTED;

				/* Reply the message, as usual */
				ReplyMsg(&io->io_Message);
				break;
			}
		}

		Enable();
	}
	else
	{
		SHOWMSG("I/O request doesn't look good");
	}

	RETURN(error);
	return(error);
}

/***********************************************************************/

/* Note: the device begin I/O function may be called with as little
 *       as 2K of stack space. Careful how you use that!
 */
static VOID ASM
dev_begin_io(
	REG(a1, struct IORequest *			io),
	REG(a6, struct TrackFileDevice *	tfd))
{
	struct TrackFileUnit * tfu = (struct TrackFileUnit *)io->io_Unit;
	const char * command_name = "(unknown)";
	const char * sender_name = "(unknown)";

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	SHOWPOINTER(io);

	#if DEBUG
	{
		struct TrackFileUnit * which_tfu;
		BOOL unit_found = FALSE;

		Forbid();

		for(which_tfu = (struct TrackFileUnit *)tfd->tfd_UnitList.mlh_Head ;
			which_tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ != NULL ;
			which_tfu = (struct TrackFileUnit *)which_tfu->tfu_Unit.tdu_Unit.unit_MsgPort.mp_Node.ln_Succ)
		{
			if(which_tfu == tfu)
			{
				unit_found = TRUE;
				break;
			}
		}

		Permit();

		if(NOT unit_found)
			D(("unit 0x%08lx seems to be invalid", tfu));
	}
	#endif /* DEBUG */

	#if DEBUG
	{
		static const struct { char * name; UWORD command; } tab[] =
		{
			{ "CMD_CLEAR",			CMD_CLEAR },
			{ "CMD_READ",			CMD_READ },
			{ "CMD_UPDATE",			CMD_UPDATE },
			{ "CMD_WRITE",			CMD_WRITE },
			{ "ETD_CLEAR",			ETD_CLEAR },
			{ "ETD_FORMAT",			ETD_FORMAT },
			{ "ETD_MOTOR",			ETD_MOTOR },
			{ "ETD_READ",			ETD_READ },
			{ "ETD_SEEK",			ETD_SEEK },
			{ "ETD_UPDATE",			ETD_UPDATE },
			{ "ETD_WRITE",			ETD_WRITE },
			{ "HD_SCSICMD",			HD_SCSICMD },
			{ "TD_ADDCHANGEINT",	TD_ADDCHANGEINT },
			{ "TD_CHANGENUM",		TD_CHANGENUM },
			{ "TD_CHANGESTATE",		TD_CHANGESTATE },
			{ "TD_EJECT",			TD_EJECT },
			{ "TD_FORMAT",			TD_FORMAT },
			{ "TD_GETDRIVETYPE",	TD_GETDRIVETYPE },
			{ "TD_GETGEOMETRY",		TD_GETGEOMETRY },
			{ "TD_GETNUMTRACKS",	TD_GETNUMTRACKS },
			{ "TD_MOTOR",			TD_MOTOR },
			{ "TD_PROTSTATUS",		TD_PROTSTATUS },
			{ "TD_RAWREAD",			TD_RAWREAD },
			{ "TD_RAWWRITE",		TD_RAWWRITE },
			{ "TD_REMCHANGEINT",	TD_REMCHANGEINT },
			{ "TD_REMOVE",			TD_REMOVE },
			{ "TD_SEEK",			TD_SEEK },
			{ "NSCMD_DEVICEQUERY",	NSCMD_DEVICEQUERY },
			{ NULL,					0 },
		};

		int i;

		for(i = 0 ; tab[i].name != NULL ; i++)
		{
			if(io->io_Command == tab[i].command)
			{
				command_name = tab[i].name;
				break;
			}
		}

		if(io->io_Message.mn_ReplyPort != NULL &&
		   io->io_Message.mn_ReplyPort->mp_SigTask != NULL &&
		   io->io_Message.mn_ReplyPort->mp_Flags == PA_SIGNAL)
		{
			sender_name = ((struct Node *)io->io_Message.mn_ReplyPort->mp_SigTask)->ln_Name;
		}
	}
	#endif /* DEBUG */

	D(("io->io_Message.mn_Length = %ld (sizeof(struct IOStdReq) == %ld)", io->io_Message.mn_Length, sizeof(struct IOStdReq)));
	D(("io->io_Command           = 0x%04lx->%s (from \"%s\")", io->io_Command, command_name, sender_name));

	/* This makes sure that WaitIO() is guaranteed to work and
	 * will not hang.
	 */
	io->io_Message.mn_Node.ln_Type = NT_MESSAGE;

	/* Is this a command for an actual unit, and is the
	 * command supported?
	 */
	if(io->io_Device == (struct Device *)tfd && tfu != NULL && is_known_command(io))
	{
		#if DEBUG
		{
			if (io->io_Command == TD_REMCHANGEINT)
			{
				SHOWMSG("the TD_REMCHANGEINT command must always get processed as quick");
			}
			else if (io->io_Command == CMD_START)
			{
				SHOWMSG("the CMD_START command must always get processed as quick");
			}
			else if (FLAG_IS_SET(io->io_Flags, IOF_QUICK))
			{
				if(NOT is_immediate_command(io))
					SHOWMSG("client asked for quick I/O, but command does not support it");
			}
			else
			{
				if(is_immediate_command(io))
					SHOWMSG("client did not ask for quick I/O, but command would have supported it");
			}
		}
		#endif /* DEBUG */

		/* Process this command on the context of the caller?
		 *
		 * Note: In order to make TD_REMCHANGEINT safer to use, we
		 * always process it as if IOF_QUICK had been set for it.
		 * The caller might just reuse the same I/O request which
		 * he had used for TD_ADDCHANGEINT, and that presents a
		 * problem because trackdisk.device queued that particular
		 * I/O request already. This is an old wound and,
		 * unfortunately, baked into the overall trackdisk.device
		 * API design :-(
		 *
		 * Forcing CMD_START to be an immediate command fixes a
		 * design bug in the undocumented CMD_STOP/CMD_START
		 * support in trackdisk.device. If you stop a unit, you
		 * will not succeed in starting it again unless you use
		 * CMD_START in quick mode: the start command would just
		 * get added to the queue...
		 */
		if((io->io_Command == TD_REMCHANGEINT || io->io_Command == CMD_START) ||
		   (FLAG_IS_SET(io->io_Flags, IOF_QUICK) && is_immediate_command(io)))
		{
			D(("BEGIN: performing this command directly (io=0x%08lx)", io));

			perform_io((struct IOStdReq *)io);

			D(("END: performing this command directly (io=0x%08lx)", io));
		}
		else
		{
			Forbid();

			/* Is this unit still online? */
			if(tfu->tfu_Process != NULL)
			{
				D(("sending this command to unit #%ld for processing (io=0x%08lx)", tfu->tfu_UnitNumber, io));

				/* Mark the IORequest has having been queued. */
				CLEAR_FLAG(io->io_Flags, IOF_QUICK);

				PutMsg(&tfu->tfu_Unit.tdu_Unit.unit_MsgPort, &io->io_Message);

				/* The unit Process is now responsible for it. */
				io = NULL;

				Permit();
			}
			/* Otherwise we'll have to cover for it. */
			else
			{
				Permit();

				D(("BEGIN: performing this command by proxy (io=0x%08lx)", io));

				/* We pretend that this command was processed
				 * by the unit process. It will be returned
				 * via ReplyMsg().
				 */
				CLEAR_FLAG(io->io_Flags, IOF_QUICK);

				/* Every command which requires the unit to
				 * be online or have a medium attached
				 * will return an error.
				 */
				perform_io((struct IOStdReq *)io);

				D(("END: performing this command by proxy (io=0x%08lx)", io));
			}
		}
	}
	else
	{
		SHOWMSG("can't do this");

		io->io_Error = IOERR_NOCMD;
	}

	/* Reply the IO request if we have to. */
	if(io != NULL && FLAG_IS_CLEAR(io->io_Flags, IOF_QUICK))
	{
		D(("replying io=0x%08lx", io));

		ReplyMsg(&io->io_Message);
	}

	LEAVE();
}
