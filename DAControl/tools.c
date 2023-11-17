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
#include <devices/trackfile.h>
#include <dos/filehandler.h>

/****************************************************************************/

#define __USE_SYSBASE
#include <proto/exec.h>

#include <proto/dos.h>
#include <proto/intuition.h>

#include <clib/alib_protos.h>

/****************************************************************************/

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

/****************************************************************************/

#include "compiler.h"
#include "macros.h"
#include "global_data.h"
#include "tools.h"

/****************************************************************************/

#include "assert.h"

/****************************************************************************/

/* This comes from the paper "Xorshift RNGs", by George Marsaglia.
 * It's an application of the 32 bit xor() algorithm which so
 * happens is pretty short and rather robust, provided the initial
 * state is not zero.
 */
ULONG
xor_shift_32(ULONG x)
{
	ULONG old_x = x;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;

	D(("xor_shift_32(0x%08lx) -> 0x%08lx", old_x, x));

	return(x);
}

/****************************************************************************/

/* Decode the data structure which the DeviceNode->dn_Startup
 * may point to. This is not a trivial affair because the value
 * stored in DeviceNode->dn_Startup is not necessarily a BCPL
 * pointer. It can also be a small integer, such as used by
 * the con-handler, newcon-handler and port-handler which would
 * store mode information there (-1, 0, 1, 2).
 *
 * We validate DeviceNode->dn_Startup and its contents. If the
 * validation fails, this function will return FALSE, otherwise
 * it will fill in the "struct fs_startup_msg" pointed to by
 * the "fsm" parameter and return TRUE.
 */
BOOL
decode_file_sys_startup_msg(struct GlobalData *gd, BPTR startup, struct fs_startup_msg * fsm)
{
	const struct FileSysStartupMsg * fssm;
	const TEXT * device_name;
	int device_name_len;
	const struct DosEnvec * de;
	BOOL success = FALSE;

	USE_EXEC(gd);

	ENTER();

	ASSERT( gd != NULL );

	/* Is this a small integer? We play is safe by allowing "small"
	 * to be any negative number, and any non-negative number up
	 * to 1024. A BCPL pointer can never be mistaken for a negative
	 * number because its most significant two bits are always
	 * zero.
	 */
	if((LONG)startup <= 1024)
	{
		D(("startup %08lx is a number, not a pointer", startup));
		goto out;
	}

	/* If this is truly a 32 bit BCPL pointer, then the two
	 * most significant bits will be zero.
	 */
	if(NOT IS_VALID_BPTR_ADDRESS(startup))
	{
		D(("startup %08lx is not even a BCPL pointer", startup));
		goto out;
	}

	/* This should be a valid RAM address. */
	fssm = BADDR(startup);
	if(TypeOfMem((APTR)fssm) == 0)
	{
		D(("fssm 0x%08lx is not a valid address", fssm));
		goto out;
	}

	/* The device name should be a NUL-terminated
	 * BCPL string and it should reside in RAM.
	 */
	if(NOT IS_VALID_BPTR_ADDRESS(fssm->fssm_Device))
	{
		D(("fssm device %08lx is not a valid BCPL pointer", fssm->fssm_Device));
		goto out;
	}

	device_name = BADDR(fssm->fssm_Device);
	if(device_name == NULL || TypeOfMem((APTR)device_name) == 0)
	{
		D(("device name 0x%08lx is not a valid address", device_name));
		goto out;
	}

	/* The device name should be suitable for passing
	 * to OpenDevice() as is, so it should not be
	 * empty.
	 */
	device_name_len = (*device_name++);
	if(device_name_len == 0)
	{
		SHOWMSG("device name is an empty string");
		goto out;
	}

	/* Is this really a NUL-terminated string?
	 * Note that some operating system components
	 * count the NUL character at the end of the
	 * string as being part of the BCPL string.
	 */
	if(device_name[device_name_len] != '\0' && device_name[device_name_len-1] != '\0')
		goto out;

	/* The environment vector should be a valid
	 * BCPL pointer and reside in RAM.
	 */
	if(NOT IS_VALID_BPTR_ADDRESS(fssm->fssm_Environ))
	{
		D(("fssm environ %08lx is not a valid BCPL pointer", fssm->fssm_Environ));
		goto out;
	}

	de = BADDR(fssm->fssm_Environ);
	if(de == NULL || TypeOfMem((APTR)de) == 0)
	{
		D(("environment 0x%08lx is not a valid address", de));
		goto out;
	}

	/* The environment table size should be > 0. The
	 * minimum actually is 11 which covers all fields
	 * up to and including 'de_NumBuffers'.
	 */
	if(de->de_TableSize < DE_NUMBUFFERS)
	{
		D(("environment table size %ld is < %ld (NumBuffers)", de->de_TableSize));
		goto out;
	}

	ASSERT( fsm != NULL );

	/* Now we put it all together. */
	fsm->fsm_device_name	= device_name;
	fsm->fsm_device_unit	= fssm->fssm_Unit;
	fsm->fsm_device_flags	= fssm->fssm_Flags;
	fsm->fsm_environment	= de;

	success = TRUE;

 out:

	RETURN(success);
	return(success);
}

/****************************************************************************/

/* Check if the given character is blank space. We are
 * only interested in the space and tabulator characters,
 * though.
 */
static BOOL
is_blank_space(TEXT c)
{
	return((BOOL)(c == ' ' || c == '\t'));
}

/****************************************************************************/

/* Check if the sum of two unsigned 32 bit integers will be larger than what
 * an unsigned 32 bit integer can hold.
 *
 * This algorithm comes from Henry S. Warren's book "Hacker's delight".
 */
static BOOL
addition_overflows(ULONG x, ULONG y)
{
	BOOL overflow;
	ULONG z;

	z = (x & y) | ((x | y) & ~(x + y));

	overflow = ((LONG)z) < 0;

	return(overflow);
}

/****************************************************************************/

/* Compute the number of leading zeros in a 32 bit word.
 *
 * This algorithm comes from Henry S. Warren's book "Hacker's delight".
 */
static int
nlz(ULONG x)
{
	int n;

	if (x == 0)
	{
		n = 32;
	}
	else
	{
		n = 0;

		if (x <= 0x0000FFFFUL)
		{
			n = n + 16;
			x = x << 16;
		}

		if (x <= 0x00FFFFFFUL)
		{
			n = n + 8;
			x = x << 8;
		}

		if (x <= 0x0FFFFFFFUL)
		{
			n = n + 4;
			x = x << 4;
		}

		if (x <= 0x3FFFFFFFUL)
		{
			n = n + 2;
			x = x << 2;
		}

		if (x <= 0x7FFFFFFFUL)
			n = n + 1;
	}

	return n;
}

/****************************************************************************/

/* Check if the product of two unsigned 32 bit integers will be
 * larger than what an unsigned 32 bit integer can hold. Note that
 * we are only interested in whether the multiplication will overflow,
 * and not in the product itself.
 *
 * This algorithm comes from Henry S. Warren's book "Hacker's delight".
 */
static BOOL
multiplication_overflows(ULONG x, ULONG y)
{
	BOOL overflow = FALSE;

	if(nlz(x) + nlz(y) <= 30)
	{
		overflow = TRUE;
	}
	else
	{
		ULONG t;

		t = x * (y >> 1);
		if((LONG)t < 0)
		{
			overflow = TRUE;
		}
		else if (y & 1)
		{
			ULONG z;

			z = t * 2 + x;

			overflow = (z < x);
		}
	}

	return(overflow);
}

/****************************************************************************/

/* Checks if a string is actually a number. Leading or trailing blank
 * spaces will be ignored. The remainder of the string should consist of
 * digits only. An empty string cannot represent a number.
 */
BOOL
string_is_number(const TEXT * str)
{
	BOOL success = FALSE;
	size_t len;

	ENTER();

	ASSERT( str != NULL );

	len = strlen(str);

	while(len > 0 && is_blank_space(str[0]))
	{
		str++;
		len--;
	}

	while(len > 0 && is_blank_space(str[len-1]))
		len--;

	if(len > 0)
	{
		size_t i;
		TEXT c;

		success = TRUE;

		for(i = 0 ; i < len ; i++)
		{
			c = str[i];

			if(NOT ('0' <= c && c <= '9'))
			{
				success = FALSE;
				break;
			}
		}
	}

	if(success)
		D(("'%s' is a number", str));
	else
		D(("'%s' is probably not a number", str));

	RETURN(success);
	return(success);
}

/****************************************************************************/

/* Convert a string to an unsigned 32 bit integer. Leading or trailing
 * blank spaces will be ignored. Returns TRUE if the remainder consists
 * of digits only, and the string can be converted into a decimal number
 * which does not exceed the range of an unsigned 32 bit integer.
 * Otherwise it's going to be FALSE...
 */
BOOL
convert_string_to_number(const TEXT * str, ULONG * value_ptr)
{
	BOOL success = FALSE;
	ULONG value;
	size_t len;
	size_t i;
	ULONG d;
	TEXT c;

	ENTER();

	ASSERT( str != NULL && value_ptr != NULL );

	len = strlen(str);

	while(len > 0 && is_blank_space(str[0]))
	{
		str++;
		len--;
	}

	while(len > 0 && is_blank_space(str[len-1]))
		len--;

	if(len == 0)
	{
		SHOWMSG("number string is empty");
		goto out;
	}

	value = 0;

	for(i = 0 ; i < len ; i++)
	{
		c = str[i];

		if(NOT ('0' <= c && c <= '9'))
			goto out;

		if(multiplication_overflows(value, 10))
		{
			SHOWMSG("numeric overflow");
			goto out;
		}

		value *= 10;

		d = c - '0';

		if(addition_overflows(value, d))
		{
			SHOWMSG("numeric overflow");
			goto out;
		}

		value += d;
	}

	D(("'%s' -> %ld", str, value));

	(*value_ptr) = value;

	success = TRUE;

 out:

	RETURN(success);
	return(success);
}

/****************************************************************************/

/* Return a string corresponding to a "trackfile.device" error code,
 * or NULL if no such code matches.
 */
STRPTR
get_tf_error_message(int error_code)
{
	static const struct { int code; STRPTR message; } error_message_table[] =
	{
		{ TFERROR_UnitBusy,			"unit is busy" },
		{ TFERROR_OutOfMemory,		"out of memory" },
		{ TFERROR_UnitNotFound,		"unit not found" },
		{ TFERROR_AlreadyInUse,		"unit is already in use" },
		{ TFERROR_UnitNotActive,	"unit is not active" },
		{ TFERROR_InvalidFile,		"disk file is not valid" },
		{ TFERROR_InvalidFileSize,	"disk file size is not supported" },
		{ TFERROR_NoFileGiven,		"no disk file was given" },
		{ TFERROR_Aborted,			"command was aborted" },
		{ TFERROR_ProcessFailed,	"unit process creation has failed" },
		{ TFERROR_NoMediumPresent,	"no medium is present" },
		{ TFERROR_ReadOnlyVolume,	"image file parent volume is not writable" },
		{ TFERROR_ReadOnlyFile,		"image file is not writable" },
		{ TFERROR_DuplicateDisk,	"contents of image file are duplicate of an active file" },
		{ TFERROR_DuplicateVolume,	"contents would likely crash the Amiga file system" },
		{ NULL, 0 },
	};

	STRPTR result = NULL;
	int i;

	ENTER();

	for(i = 0 ; error_message_table[i].message != NULL ; i++)
	{
		if(error_message_table[i].code == error_code)
		{
			result = error_message_table[i].message;
			break;
		}
	}

	RETURN(result);
	return(result);
}

/****************************************************************************/

/* Return an error string corresponding to a general I/O error code,
 * a trackdisk.device-specific error code or NULL if none matches.
 */
STRPTR
get_io_error_message(int error_code)
{
	static const struct { int code; STRPTR message; } error_message_table[] =
	{
		{ IOERR_OPENFAIL,		"device/unit failed to open" },
		{ IOERR_ABORTED,		"I/O request terminated early" },
		{ IOERR_NOCMD,			"command not supported by device" },
		{ IOERR_BADLENGTH,		"not a valid length" },
		{ IOERR_BADADDRESS,		"invalid address (misaligned or bad range)" },
		{ IOERR_UNITBUSY,		"device opened OK, but requested unit is busy" },
		{ IOERR_SELFTEST,		"hardware failed self-test" },

		{ TDERR_NotSpecified,	"unspecific error" },
		{ TDERR_NoSecHdr,		"sector header not found" },
		{ TDERR_BadSecPreamble,	"bad sector preamble" },
		{ TDERR_BadSecID,		"bad sector ID" },
		{ TDERR_BadHdrSum,		"bad header checksum" },
		{ TDERR_BadSecSum,		"bad sector checksum" },
		{ TDERR_TooFewSecs,		"too few sectors read" },
		{ TDERR_BadSecHdr,		"bad sector header" },
		{ TDERR_WriteProt,		"cannot write to a protected disk" },
		{ TDERR_DiskChanged,	"no disk in the drive" },
		{ TDERR_SeekError,		"could not find track 0" },
		{ TDERR_NoMem,			"ran out of memory" },
		{ TDERR_BadUnitNum,		"bad unit number" },
		{ TDERR_BadDriveType,	"bad drive type" },
		{ TDERR_DriveInUse,		"drive is already in use" },
		{ TDERR_PostReset,		"user hit reset" },

		{ NULL, 0 },
	};

	STRPTR result = NULL;
	int i;

	ENTER();

	for(i = 0 ; error_message_table[i].message != NULL ; i++)
	{
		if(error_message_table[i].code == error_code)
		{
			result = error_message_table[i].message;
			break;
		}
	}

	RETURN(result);
	return(result);
}

/****************************************************************************/

/* Return the error message text corresponding to an AmigaDOS error code,
 * a trackfile.device error code, a general I/O error code or
 * trackdisk.device-specific error code. The message text will be
 * stored in the provided buffer.
 */
STRPTR
get_error_message(struct GlobalData * gd, int error_code, TEXT * buffer, size_t buffer_size)
{
	STRPTR error_message;

	USE_DOS(gd);

	ENTER();

	ASSERT( buffer != NULL );

	SHOWVALUE(error_code);

	ASSERT( gd != NULL && buffer != NULL );

	error_message = get_tf_error_message(error_code);
	if(error_message == NULL)
		error_message = get_io_error_message(error_code);

	if(error_message != NULL)
		local_strlcpy(buffer, error_message, buffer_size);
	else
		Fault(error_code, NULL, buffer, buffer_size);

	RETURN(buffer);
	return(buffer);
}

/****************************************************************************/

/* The "Amiga ROM Kernel Reference Manual: Devices" (3rd edition) says
 * this in Appendix C: Floppy boot process and physical layout:
 *
 * "The first two sectors on each floppy disk contain special boot information.
 *  These sectors are read into the system at an arbitrary position; therefore,
 *  the code must be position independent. The first three longwords come from
 *  the include file <devices/bootblock.h>. The type must be BBID_DOS; the
 *  checksum must be correct (an additive carry wraparound sum of Oxffffffff).
 *  Execution starts at location 12 of the first sector read in."
 *
 * How the "additive carry wraparound sum" is not described at all, but it
 * works like this: all unsigned 32 bit words in the block are added up and
 * if the addition produces a carry, it is added to the sum.
 */
ULONG
calculate_boot_block_checksum(const ULONG * data, int length)
{
	ULONG sum, temp;
	int i;

	ENTER();

	ASSERT( data != NULL );

	sum = 0;

	for(i = 0 ; i < length / (int)sizeof(*data) ; i++)
	{
		temp = sum;

		sum += data[i];
		if(sum < temp) /* Check for overflow */
			sum++; /* Add carry */
	}

	RETURN(sum);
	return(sum);
}

/****************************************************************************/

/*
 * Appends src to string dst of size siz (unlike strncat, siz is the
 * full size of dst, not space left). At most siz-1 characters
 * will be copied. Always NUL terminates (unless siz <= strlen(dst)).
 * Returns strlen(src) + MIN(siz, strlen(initial dst)).
 * If retval >= siz, truncation occurred.
 */
size_t
local_strlcat(char *dst, const char *src, size_t siz)
{
	char *d = dst;
	const char *s = src;
	size_t n = siz;
	size_t dlen;
	size_t result;

	ENTER();

	ASSERT( dst != NULL && src != NULL );

	SHOWSTRING(dst);
	SHOWSTRING(src);
	SHOWVALUE(siz);

	/* Find the end of dst and adjust bytes left but don't go past end */
	while(n-- != 0 && (*d) != '\0')
		d++;

	dlen = d - dst;
	n = siz - dlen;

	if(n == 0)
	{
		result = dlen + strlen(s);
	}
	else
	{
		while((*s) != '\0')
		{
			if(n != 1)
			{
				(*d++) = (*s);
				n--;
			}

			s++;
		}

		(*d) = '\0';

		result = dlen + (s - src); /* count does not include NUL */
	}

	RETURN(result);
	return(result);
}

/****************************************************************************/

/*
 * Copy src to string dst of size siz. At most siz-1 characters
 * will be copied. Always NUL terminates (unless siz == 0).
 * Returns strlen(src); if retval >= siz, truncation occurred.
 */
size_t
local_strlcpy(char *dst, const char *src, size_t siz)
{
	char *d = dst;
	const char *s = src;
	size_t n = siz;
	size_t result;

	ENTER();

	ASSERT( dst != NULL && src != NULL );

	SHOWSTRING(src);
	SHOWVALUE(siz);

	/* Copy as many bytes as will fit */
	if(n != 0 && --n != 0)
	{
		do
		{
			if(((*d++) = (*s++)) == '\0')
				break;
		}
		while(--n != 0);
	}

	/* Not enough room in dst, add NUL and traverse rest of src */
	if(n == 0)
	{
		if(siz != 0)
			(*d) = '\0'; /* NUL-terminate dst */

		while((*s++) != '\0')
			;
	}

	result = s - src - 1; /* count does not include NUL */

	RETURN(result);
	return(result);
}

/****************************************************************************/

/* This is used by stuff_char() below. */
struct format_context
{
	STRPTR	fc_buffer;
	ULONG	fc_size;
	ULONG	fc_len;
};

/****************************************************************************/

/* This is used by local_vsnprintf() below. */
static void ASM
stuff_char(REG(d0, UBYTE c), REG(a3, struct format_context *fc))
{
	/* Still room in the buffer? */
	if(fc->fc_size > 0)
	{
		(*fc->fc_buffer++) = c;

		fc->fc_len++;

		fc->fc_size--;

		/* Just one more byte to go before the
		 * buffer runs out? Put a NUL byte there
		 * and refuse to store any further
		 * characters.
		 */
		if(fc->fc_size == 1)
		{
			(*fc->fc_buffer) = '\0';

			fc->fc_size = 0;
		}
	}
}

/****************************************************************************/

/* This is used by local_snprintf() below. */
static ULONG
local_vsnprintf(
	struct GlobalData *	gd,
	STRPTR				buffer,
	ULONG				buffer_size,
	const char *		format_string,
	va_list				args)
{
	USE_EXEC(gd);

	struct format_context fc;
	ULONG len;

	ENTER();

	ASSERT( gd != NULL && buffer != NULL && format_string != NULL );

	fc.fc_buffer	= buffer;
	fc.fc_size		= buffer_size;
	fc.fc_len		= 0;

	/* Any space left to store even a NUL byte? */
	if(fc.fc_size > 0)
	{
		/* Make sure that the output will be NUL-terminated. */
		if(fc.fc_size > 1)
			RawDoFmt((STRPTR)format_string, args, (VOID (*)())stuff_char, &fc);
		else
			(*buffer) = '\0';
	}

	len = fc.fc_len;

	RETURN(len);
	return(len);
}

/****************************************************************************/

/* Length-limited version of sprintf() which guarantees not to overrun the
 * buffer and to always NUL-terminate the buffer contents. If the buffer
 * size is smaller than 1, the buffer contents will not be modified.
 */
ULONG
local_snprintf(
	struct GlobalData *	gd,
	STRPTR				buffer,
	ULONG				buffer_size,
	const char *		format_string,
						...)
{
	va_list args;
	ULONG len;

	ENTER();

	ASSERT( gd != NULL && buffer != NULL && format_string != NULL );

	va_start(args, format_string);
	len = local_vsnprintf(gd, buffer, buffer_size, format_string, args);
	va_end(args);

	RETURN(len);
	return(len);
}

/****************************************************************************/

/* Use Inhibit() on an AmigaDOS file system device provided. A colon
 * character will be added to the file system name prior to calling the
 * Inhibit() function.
 */
LONG
inhibit_device(struct GlobalData * gd, STRPTR dos_device_name, BOOL on_or_off)
{
	TEXT dos_device_name_with_colon[260];
	LONG result;

	USE_DOS(gd);

	local_strlcpy(dos_device_name_with_colon, dos_device_name, sizeof(dos_device_name_with_colon));
	local_strlcat(dos_device_name_with_colon, ":", sizeof(dos_device_name_with_colon));

	result = Inhibit(dos_device_name_with_colon, on_or_off ? DOSTRUE : DOSFALSE);

	return(result);
}

/****************************************************************************/

/* Use Format() on an AmigaDOS file system device provided. A colon
 * character will be added to the file system name prior to calling the
 * Format() function.
 */
LONG
format_device(struct GlobalData * gd, STRPTR dos_device_name, STRPTR label, ULONG dos_type)
{
	TEXT dos_device_name_with_colon[260];
	LONG result;

	USE_DOS(gd);

	local_strlcpy(dos_device_name_with_colon, dos_device_name, sizeof(dos_device_name_with_colon));
	local_strlcat(dos_device_name_with_colon, ":", sizeof(dos_device_name_with_colon));

	result = Format(dos_device_name_with_colon, label, dos_type);

	return(result);
}

/****************************************************************************/

/* Print an error message, preferably using the standard error output
 * stream. It will always be prefixed by the name of the command,
 * similar to how PrintFault() works.
 *
 * If the program was started from Workbench, show an error requester
 * instead via intuition.library/EasyRequest().
 */
static int
ShowErrorCommon(struct GlobalData *gd, const char * choices, const char * fmt, va_list ap)
{
	USE_DOS(gd);
	USE_EXEC(gd);

	int result = 0;

	ENTER();

	ASSERT( gd != NULL && fmt != NULL );

	/* Error messages go to the shell window? */
	if(gd->gd_WBenchMsg == NULL)
	{
		BPTR fh;

		/* Try to use the standard error output stream,
		 * if possible. Otherwise fall back onto the
		 * standard output stream.
		 */
		fh = ((struct Process *)FindTask(NULL))->pr_CES;
		if(fh == ZERO)
			fh = Output();

		if(fh != ZERO && VFPrintf(fh, "DAControl: ", NULL) != -1)
		{
			if(VFPrintf(fh, (STRPTR)fmt, ap) != -1)
				VFPrintf(fh, "\n", NULL);
		}
	}
	/* We need to show an error requester. */
	else
	{
		struct Library * IntuitionBase;
		struct EasyStruct es;

		IntuitionBase = OpenLibrary("intuition.library", 37);
		if(IntuitionBase != NULL)
		{
			TEXT title[256];

			if(gd->gd_DiskImageFileName != NULL)
				local_snprintf(gd, title, sizeof(title), "Disk image file %s error", gd->gd_DiskImageFileName);
			else
				local_strlcpy(title, "Disk image file error", sizeof(title));

			memset(&es, 0, sizeof(es));

			es.es_StructSize	= sizeof(es);
			es.es_Title			= title;
			es.es_TextFormat	= (STRPTR)fmt;
			es.es_GadgetFormat	= (choices != NULL) ? choices : "OK";

			result = EasyRequestArgs(NULL, &es, NULL, ap);

			CloseLibrary(IntuitionBase);
		}
	}

	RETURN(result);
	return(result);
}

/****************************************************************************/

/* Show an error message either in the shell window or by displaying an
 * error requester window instead. If an error requester window is shown,
 * the user may choose how to proceed by answering "Continue" or "Stop".
 */
int
ShowError(struct GlobalData *gd, const char * choices, const char * fmt, ...)
{
	va_list ap;
	int result;

	va_start(ap, fmt);

	result = ShowErrorCommon(gd, choices, fmt, ap);

	va_end(ap);

	return(result);
}

/****************************************************************************/

/* Show an error message either in the shell window or by displaying an
 * error requester window instead.
 */
void
Error(struct GlobalData *gd, const char * fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	ShowErrorCommon(gd, NULL, fmt, ap);

	va_end(ap);
}

/****************************************************************************/

/* This is based upon Simon Tatham's "Sorting a linked list using Mergesort",
 * which can be found here:
 *
 * https://www.chiark.greenend.org.uk/~sgtatham/algorithms/listsort.html
 *
 * Adapted for use with Exec doubly-linked lists by Olaf Barthel,
 * 29 November 2019.
 */
void
SortList(struct List *list, int (*compare)(const struct Node *a, const struct Node *b))
{
	/* Only sort this list if contains at least two elements.
	 * The first test is for a non-empty list, the second is
	 * for a non-empty list with more than a single element.
	 */
	if(list->lh_Head->ln_Succ != NULL && list->lh_Head != list->lh_TailPred)
	{
		struct Node *head, *p, *q, *e, *tail;
		int insize, nmerges, psize, qsize, i;

		head = list->lh_Head;

		/* Make sure that the final list element has a NULL successor
		 * pointer. The first list item does not have to have its
		 * predecessor pointer set to NULL because the mergesort algorithm
		 * to follow only ever tests and changes the successor pointers.
		 */
		list->lh_TailPred->ln_Succ = NULL;

		/* Simon Tatham's algorithm follows, slightly modified so that it no
		 * longer allows for circular lists and to expect doubly-linked
		 * lists.
		 */
		insize = 1;

		while(TRUE)
		{
			p = head;

			head = NULL;
			tail = NULL;

			/* count number of merges we do in this pass */
			nmerges = 0;

			while(p != NULL)
			{
				/* there exists a merge to be done */
				nmerges++;

				/* step 'insize' places along from p */
				q = p;
				psize = 0;

				for(i = 0 ; i < insize ; i++)
				{
					psize++;

					q = q->ln_Succ;
					if(q == NULL)
						break;
				}

				/* if q hasn't fallen off end, we have two lists to merge */
				qsize = insize;

				/* now we have two lists; merge them */
				while(psize > 0 || (qsize > 0 && q != NULL))
				{
					/* decide whether next element of merge comes from p or q */
					if (psize == 0)
					{
						/* p is empty; e must come from q. */
						e = q;
						q = q->ln_Succ;
						qsize--;
					}
					else if (qsize == 0 || q == NULL)
					{
						/* q is empty; e must come from p. */
						e = p;
						p = p->ln_Succ;
						psize--;
					}
					else if ((*compare)(p, q) <= 0)
					{
						/* First element of p is lower (or same);
						 * e must come from p.
						 */
						e = p;
						p = p->ln_Succ;
						psize--;
					}
					else
					{
						/* First element of q is lower; e must come from q. */
						e = q;
						q = q->ln_Succ;
						qsize--;
					}

					/* add the next element to the merged list */
					if(tail != NULL)
						tail->ln_Succ = e;
					else
						head = e;

					/* Maintain reverse pointers in a doubly linked list. */
					e->ln_Pred = tail;
					tail = e;
				}

				/* now p has stepped `insize' places along, and q has too */
				p = q;
			}

			tail->ln_Succ = NULL;

			/* If we have done only one merge, we're finished. */
			if(nmerges <= 1) /* allow for nmerges==0, the empty list case */
				break;

			/* Otherwise repeat, merging lists twice the size */
			insize *= 2;
		}

		/* Hook up the head and tail elements with the standard
		 * Exec list layout.
		 */
		list->lh_Head = head;
		head->ln_Pred = (struct Node *)&list->lh_Head;

		list->lh_TailPred = tail;
		tail->ln_Succ = (struct Node *)&list->lh_Tail;
	}
}

/****************************************************************************/

/* Convert the track file checksum (a 64 bit integer) into a short
 * text representation. This will require a text buffer which can
 * hold at least 11 characters plus a terminating NUL byte.
 */
void
tf_checksum_to_text(const struct TrackFileChecksum * tfc, TEXT * text_form)
{
	/* We break down the 64 bit integer into groups of six bits
	 * each, with the last four bits having two zero bits.
	 * Each 6 bit value then gets mapped to a character from
	 * the following table. Note that the 0 and O are both
	 * replaced with different characters to as to avoid mistaking
	 * one for the other.
	 */
	static const char mapping[64+1] = \
		".abcdefghijklmnopqrstuvwxyz%123456789ABCDEFGHIJKLMN/PQRSTUVWXYZ:";

	ULONG high = tfc->tfc_high;
	ULONG low = tfc->tfc_low;
	int i;

	for(i = 0 ; i < 11 ; i++)
	{
		(*text_form++) = mapping[low % 64];

		/* Shift the 64 bit integer right by 6 bits. */
		low = (high << (32 - 6)) | (low >> 6);
		high = high >> 6;
	}

	/* And that would be the NUL-termination for an
	 * 11 character string.
	 */
	(*text_form) = '\0';
}

/****************************************************************************/

/* Just like strtok(), but reentrant... */
char *
local_strtok_r(char *str, const char *separator_set, char ** state_ptr)
{
	char * result = NULL;
	char * last;
	size_t size;

	ENTER();

	ASSERT( separator_set != NULL && state_ptr != NULL );

	SHOWSTRING(str);
	SHOWSTRING(separator_set);
	SHOWPOINTER(state_ptr);

	last = (*state_ptr);

	/* Did we get called before? Restart at the last valid position. */
	if(str == NULL)
	{
		str = last;

		/* However, we may have hit the end of the
		 * string already.
		 */
		if(str == NULL)
			goto out;
	}

	last = NULL;

	/* Skip the characters which count as
	 * separators.
	 */
	str += strspn(str, separator_set);
	if((*str) == '\0')
		goto out;

	/* Count the number of characters which aren't
	 * separators.
	 */
	size = strcspn(str, separator_set);
	if(size == 0)
		goto out;

	/* This is where the search can resume later. */
	last = &str[size];

	/* If we didn't hit the end of the string already,
	 * skip the separator.
	 */
	if((*last) != '\0')
		last++;

	/* This is the token we found; make sure that
	 * it looks like a valid string.
	 */
	str[size] = '\0';

	result = str;

 out:

	if(state_ptr != NULL)
		(*state_ptr) = last;

	RETURN(result);
	return(result);
}

/****************************************************************************/

#if DEBUG

/****************************************************************************/

/* Try to find a "$VER: <name> <version>.<revision> (<day>.<month>.<year>)"
 * format version string in a segment list, as returned by LoadSeg(). Will
 * return the length of the string found or 0. If the length is > 0, then
 * it will be copied into the provided buffer and, if it is too long, to be
 * truncated.
 */
size_t
find_version_string(
	BPTR	segment_list,
	STRPTR	string_buffer,
	size_t	string_buffer_size)
{
	size_t result = 0;
	const ULONG * segment_data;
	BPTR next_segment;
	BPTR segment;
	const UBYTE * data_bytes;
	size_t segment_data_size;
	size_t i, j;
	size_t len;

	ENTER();

	ASSERT( string_buffer != NULL && string_buffer_size > 0 );

	for(segment = segment_list ;
	    result == 0 && segment != ZERO ;
	    segment = next_segment)
	{
		/* The segment begins with a BCPL pointer to
		 * the next following segment, which may be ZERO.
		 * Directly preceding that pointer is the total
		 * memory allocated for this segment, including
		 * the size field. This means that the size given
		 * is 8 bytes larger than the allocation size.
		 */
		segment_data = BADDR(segment);
		segment_data_size = segment_data[-1];

		next_segment = segment_data[0];

		/* The standard version string begins with
		 * six characters ("$VER: "). We skip these
		 * and expect that something meaningful will
		 * follow them.
		 */
		if(segment_data_size > 2 * sizeof(*segment_data) + 6)
		{
			segment_data_size -= 2 * sizeof(*segment_data);

			data_bytes = (UBYTE *)&segment_data[1];

			/* Look for the prefix. */
			for(i = 0 ; i < segment_data_size - 6 ; i++)
			{
				if(data_bytes[i] != '$' || strncmp(&data_bytes[i+1], "VER: ", 5) != SAME)
					continue;

				/* Make a worst case estimate how long a
				 * non-NUL-terminated string could be.
				 */
				len = segment_data_size - (i + 6);

				/* If there's a NUL in the string to
				 * follow, adjust the expected
				 * string length.
				 */
				for(j = i + 6 ; j < segment_data_size ; j++)
				{
					if(data_bytes[j] == '\0')
					{
						len = j- (i + 6);
						break;
					}
				}

				/* Did we find anything useful? */
				if(len > 0)
				{
					/* This is the full expected length. */
					result = len;

					/* But we may have to truncate the
					 * string to be copied, if necessary.
					 */
					if(len > string_buffer_size-1)
						len = string_buffer_size-1;

					memmove(string_buffer, &data_bytes[i+6], len);
					string_buffer[len] = '\0';

					break;
				}

				i += 5;
			}
		}
	}

	RETURN(result);
	return(result);
}

/****************************************************************************/

/* Try to find a Resident structure in a segment list, as returned by LoadSeg().
 * Will return the address of it, or NULL if none could be found.
 */
struct Resident *
find_rom_tag(BPTR segment_list)
{
	struct Resident * result = NULL;
	struct Resident * rt;
	const ULONG * segment_data;
	size_t segment_data_size;
	BPTR next_segment;
	BPTR segment;
	const UWORD * data_words;
	size_t num_data_words;
	size_t i;

	ENTER();

	for(segment = segment_list ;
	    result == NULL && segment != ZERO ;
	    segment = next_segment)
	{
		/* The segment begins with a BCPL pointer to
		 * the next following segment, which may be ZERO.
		 * Directly preceding that pointer is the total
		 * memory allocated for this segment, including
		 * the size field. This means that the size given
		 * is 8 bytes larger than the allocation size.
		 */
		segment_data = BADDR(segment);
		segment_data_size = segment_data[-1];

		next_segment = segment_data[0];

		if(segment_data_size > 2 * sizeof(*segment_data))
		{
			segment_data_size -= 2 * sizeof(*segment_data);

			data_words = (UWORD *)&segment_data[1];
			num_data_words = segment_data_size / sizeof(*data_words);

			for(i = 0 ; i < num_data_words ; i++)
			{
				if(data_words[i] != RTC_MATCHWORD)
					continue;

				rt = (struct Resident *)&data_words[i];
				if(rt->rt_MatchTag == rt)
				{
					result = rt;
					break;
				}
			}
		}
	}

	RETURN(result);
	return(result);
}

/****************************************************************************/

#endif /* DEBUG */
