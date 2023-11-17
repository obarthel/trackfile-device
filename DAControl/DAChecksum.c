/*
 * :ts=4
 *
 * Example program for demonstrating how the trackfile.device
 * calculates the disk image file track and disk checksums.
 *
 * Written by Olaf Barthel (2020-07-11)
 * Public domain
 */

#include <exec/memory.h>

#include <dos/dosextens.h>

#include <devices/trackdisk.h>

/****************************************************************************/

#include <clib/exec_protos.h>
#include <clib/dos_protos.h>

#include <pragmas/exec_pragmas.h>
#include <pragmas/dos_pragmas.h>

/****************************************************************************/

extern struct Library * SysBase;
extern struct Library * DOSBase;

/****************************************************************************/

#include <stddef.h>
#include <string.h>

/****************************************************************************/

#define ZERO ((BPTR)NULL)
#define OK (0)

/****************************************************************************/

/* This should be a single unsigned 64 bit integer, but two unsigned
 * 32 bit integers will do just fine, too.
 */
struct fletcher64_checksum
{
	ULONG f64c_high;
	ULONG f64c_low;
};

/****************************************************************************/

/* Calculates the 64 bit checksum for a series of 32 bit words.
 *
 * The basic workings of this algorithm come from: J.G. Fletcher. An arithmetic
 * checksum for serial transmission; IEEE Transactions on Communications,
 * January 1982.
 */
void
fletcher64_checksum(APTR data, size_t size, struct fletcher64_checksum * checksum)
{
	const ULONG * block = (ULONG *)data;
	size_t count = size / sizeof(*block);
	ULONG sum1 = 0, sum2 = 0;

	/* Some loop unrolling may go a long way... */
	while(count >= 4)
	{
		sum1 += (*block++);
		sum2 += sum1;

		sum1 += (*block++);
		sum2 += sum1;

		sum1 += (*block++);
		sum2 += sum1;

		sum1 += (*block++);
		sum2 += sum1;

		count -= 4;
	}

	while(count-- > 0)
	{
		sum1 += (*block++);
		sum2 += sum1;
	}

	/* This should be a single unsigned 64 bit integer with 'sum2'
	 * being the most significant 32 bits.
	 */
	checksum->f64c_high	= sum2;
	checksum->f64c_low	= sum1;
}

/****************************************************************************/

/* Convert the track file checksum (a 64 bit integer) into a short
 * text representation. This will require a text buffer which can
 * hold at least 11 characters plus a terminating NUL byte.
 */
void
checksum_to_text(struct fletcher64_checksum * f64c, TEXT * text_form)
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

	ULONG high = f64c->f64c_high;
	ULONG low = f64c->f64c_low;
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

/* Calculate individual track checksums, pad them with the size of the
 * disk image file, then calculate the full disk checksum over this data.
 */
void
da_checksum(
	APTR							disk_data,
	LONG							file_size,
	struct fletcher64_checksum *	track_checksums,
	struct fletcher64_checksum *	disk_checksum)
{
	BYTE * track_data = disk_data;
	int bytes_per_track = file_size / 160;
	int i;

	for(i = 0 ; i < 160 ; i++)
		fletcher64_checksum(&track_data[i * bytes_per_track], bytes_per_track, &track_checksums[i]);

	track_checksums[160].f64c_high	= 0;
	track_checksums[160].f64c_low	= file_size;

	fletcher64_checksum(track_checksums, sizeof(*track_checksums) * (160+1), disk_checksum);
}

/****************************************************************************/

/* Read all Amiga disk image files, calculate their respective track and
 * disk checksums and print these along with the path names of these
 * files. Directories and soft links are ignored, as are files whose
 * sizes do not match those of Amiga 3.5" floppy disk image files
 * of double or high density disks.
 */
int
main(int argc, char ** argv)
{
	/* A 3.5" Amiga floppy disk has 80 cylinders, but
	 * it has twice as many tracks (160). Checksums are
	 * calculated per track, not per cylinder.
	 */
	int size_dd_disk = TD_SECTOR *     NUMSECS * 2 * 80;
	int size_hd_disk = TD_SECTOR * 2 * NUMSECS * 2 * 80;
	int max_path_name = 1024;

	struct fletcher64_checksum * track_checksums = NULL;
	struct fletcher64_checksum disk_checksum;
	struct RDArgs * rda = NULL;
	struct AnchorPath * ap = NULL;
	TEXT checksum_text[16];
	BOOL matched = FALSE;
	int result = RETURN_ERROR;
	STRPTR * files = NULL;
	BPTR file_handle = ZERO;
	APTR disk_data = NULL;
	LONG num_bytes_read;
	STRPTR file_name;
	BPTR old_dir;
	LONG error;

	/* Kickstart 2.04 or higher required. */
	if(SysBase->lib_Version < 37)
	{
		result = RETURN_FAIL;
		goto out;
	}

	/* Note that we allocate memory for reading the
	 * largest possible Amiga disk image file supported
	 * by trackfile.device.
	 */
	disk_data = AllocVec(size_hd_disk, MEMF_ANY|MEMF_PUBLIC);

	track_checksums = AllocVec(sizeof(*track_checksums) * (160 + 1), MEMF_ANY);
	
	ap = AllocVec(sizeof(*ap) + max_path_name, MEMF_ANY|MEMF_PUBLIC);
	if(ap != NULL)
		memset(ap, 0, sizeof(*ap));

	if(ap == NULL || disk_data == NULL || track_checksums == NULL)
	{
		PrintFault(ERROR_NO_FREE_STORE, "DAChecksum");
		goto out;
	}

	rda = ReadArgs("FILES/A/M", (LONG *)&files, NULL);
	if(rda == NULL)
	{
		PrintFault(IoErr(), "DAChecksum");
		goto out;
	}

	/* Process all the arguments as the "List" command would by
	 * apply pattern matching to them.
	 */
	while((file_name = (*files++)) != NULL)
	{
		/* Set up the directory scanner to stop on Ctrl+C and also
		 * make it build the full path to the respective disk image
		 * file.
		 */
		memset(ap, 0, sizeof(*ap));

		ap->ap_Strlen		= max_path_name;
		ap->ap_BreakBits	= SIGBREAKF_CTRL_C;

		for(error = MatchFirst(file_name, ap), matched = TRUE ;
		    error == OK ;
		    error = MatchNext(ap))
		{
			/* This should be a plain file and not a soft link. */
			if(ap->ap_Info.fib_DirEntryType >= 0 || ap->ap_Info.fib_DirEntryType == ST_SOFTLINK)
				continue;

			/* We only support 3.5" double density and
			 * high density disk image files.
			 */
			if(ap->ap_Info.fib_Size != size_dd_disk &&
			   ap->ap_Info.fib_Size != size_hd_disk)
			{
				continue;
			}

			/* Attempt to open the given file for reading. */
			old_dir = CurrentDir(ap->ap_Current->an_Lock);

			file_handle = Open(ap->ap_Info.fib_FileName, MODE_OLDFILE);
			error = IoErr();

			CurrentDir(old_dir);

			/* Abort if this didn't work out. */
			if(file_handle == NULL)
			{
				PrintFault(error, ap->ap_Buf);
				goto out;
			}

			/* Read the entire disk image file. */
			num_bytes_read = Read(file_handle, disk_data, ap->ap_Info.fib_Size);
			if(num_bytes_read == -1)
			{
				/* This was a read error. */
				PrintFault(error, ap->ap_Buf);
				goto out;
			}

			/* Make sure that we read exactly as much data
			 * as requested and that the file has not been
			 * truncated since we learned of its size.
			 */
			if(num_bytes_read != ap->ap_Info.fib_Size)
			{
				Printf("%s: File \"%s\" was truncated (expected %ld bytes, read only %ld)\n",
					"DAChecksum", ap->ap_Buf, ap->ap_Info.fib_Size, num_bytes_read);

				goto out;
			}

			Close(file_handle);
			file_handle = ZERO;

			da_checksum(disk_data, num_bytes_read, track_checksums, &disk_checksum);

			checksum_to_text(&disk_checksum, checksum_text);

			Printf("%s  %s\n", checksum_text, ap->ap_Buf);
		}

		if(error != OK && error != ERROR_NO_MORE_ENTRIES)
		{
			PrintFault(error, "DAChecksum");
			goto out;
		}

		MatchEnd(ap);
		matched = FALSE;
	}

	result = RETURN_OK;

 out:

	if(file_handle != ZERO)
		Close(file_handle);

	if(ap != NULL)
	{
		if(matched)
			MatchEnd(ap);

		FreeVec(ap);
	}

	if(track_checksums != NULL)
		FreeVec(track_checksums);

	if(disk_data != NULL)
		FreeVec(disk_data);

	if(rda != NULL)
		FreeArgs(rda);

	return(result);
}
