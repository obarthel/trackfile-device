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

/*
 * This is scaffolding for building "better" Amiga floppy disk data
 * recovery functionality which Disk Doctor can make use of.
 *
 * The Amiga floppy disk hardware will read/write a complete track
 * in one single revolution of the disk. Even if you only asked for a
 * single sector, the entire track will have to be processed in order to
 * find the sector which the data you asked for resides in.
 *
 * Because the hardware will start reading/writing data as soon as it gets
 * going, the order in which the track sectors appear is unpredictable.
 * You might get lucky and see the first sector at the beginning of the
 * track, but more likely one of the other 10 sectors (or 21 sectors for
 * a high density disk) will be there instead, followed by the next
 * sectors in ascending order. Or you might start with sector 11 and the
 * next to follow will be sector 0.
 *
 * Should any of the track sectors get corrupted, it's possible that the
 * damage may spread to neighbouring sectors, but it's also very well
 * possible that the damage may be limited to only a handful of sectors with
 * the remainder being intact and recoverable.
 *
 * So, why is this scaffolding code useful in building better Amiga floppy
 * disk recovery functionality? The reason lies with "trackdisk.device" which
 * for reasons likely involving ROM space constraints has a very limited
 * approach to recovering partly damaged tracks.
 *
 * Because the entire track has to be read, decoded and checked for errors,
 * even one single defect found in the track data can cause "trackdisk.device"
 * to return the read request with an error code set even if the sector you
 * wanted to read was not defective.
 *
 * Of course, "trackdisk.device" will first try to remedy the read error
 * problem by rereading the track (some errors are spurious, such as caused
 * by bus bandwidth limitations). But if rereading the track fails to produce
 * better results then you won't see the data you asked for, even if it is
 * in perfect order.
 *
 * This scaffolding code goes the extra mile to decode as many sectors as
 * possible in the tracks it reads, regardless of how many others are
 * damaged.
 */

/****************************************************************************/

#include <devices/trackdisk.h>
#include <exec/memory.h>

#include <dos/filehandler.h>
#include <dos/rdargs.h>
#include <dos/dosasl.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <clib/alib_protos.h>

/****************************************************************************/

#include <string.h>
#include <stdlib.h>
#include <stddef.h>

/****************************************************************************/

#define OK (0)
#define NOT !
#define CANNOT !
#define ZERO ((BPTR)NULL)

/****************************************************************************/

/* This is a simple test to check if a BPTR may point
 * to a valid address. The test involves looking at the
 * two most significant bits of the BPTR which must be 0.
 */
#define IS_VALID_BPTR_ADDRESS(ptr) (((ptr) & 0xC0000000) == 0)

/****************************************************************************/

/* This defines the "Amiga 1.0 format" for the Commodore-Amiga disk
 * format (see below).
 */
#define AMIGA_10_FORMAT 0xFF

/* Raw sector header, according to the RKM Devices, 3rd edition, appendix C,
 * section "Commodore-Amiga disk format". Note that the encoding of the
 * "16 bytes of OS recovery info" as documented ("treated as a block of
 * 16 bytes for encoding") is incorrect with respect to how trackdisk.device
 * encodes it.
 */
struct sector_header
{
	/* This marks the beginning of the sector. We don't actually care about
	 * the contents after decoding. The MFM-encoding applies to two blocks
	 * of two bytes each. These four bytes (zero and sync) are not encoded
	 * as a single 32 bit word. The disk driver just stores these values
	 * in the header when the sector data is to be written back to disk.
	 */
	UBYTE	zero[2];		/* Stored as 0x00,0x00; MFM-encoded as 0xAAAA,0xAAAA */
	UBYTE	sync[2];		/* Stored as 0xA1,0xA1; encoded as 0x4489,0x4489 */

	/* Format and track/sector information. The MFM-encoding treats these
	 * as a single 32 bit word.
	 */
	UBYTE	format;			/* Amiga 1.0 format = 0xFF */
	UBYTE	track_number;	/* 0..79 */
	UBYTE	sector_number;	/* 0..10 or 0..21 for high density floppy disk */

	/* The sector offset indicates how many further sectors will
	 * follow this particular sector. In sector 0 this would be the
	 * highest valid sector number (e.g. 11 or 22 for high density
	 * floppy disks) and each sector in sequence would decrement
	 * this value. The last valid sector will have 1 in the
	 * sector offset field.
	 */
	UBYTE	sector_offset;	/* 1..11 or 1..22 for high density floppy disk */

	/* The sector label information, which is of no practical relevance
	 * here, except for it being part of the header checksum calculation.
	 * The MFM-encoding treats this data as four consecutive 32 bit words.
	 *
	 * Note that the label "OS recovery info" is misleading as this
	 * feature was never put to use by the Amiga operating system.
	 * In the "trackdisk.device" documentation these 16 bytes are
	 * referred to as the "sector label". Usually, all of these bytes
	 * will be set to 0.
	 */
	UBYTE	os_recovery_info[16];

	/* Checksum for the format/sector information and the sector label
	 * information. MFM-encoding treats this as a single 32 bit word.
	 */
	ULONG	header_checksum;

	/* Checksum for the data which follows the header. MFM-encoding treats
	 * this as a single 32 bit word.
	 */
	ULONG	data_area_checksum;
};

/****************************************************************************/

/* Try to find the next MFM-encoded Amiga format sector in the track
 * data, starting at a specific position in the track buffer. If
 * successful, fills in the position where the sector header showed
 * up and at which bit position (0..15) the sector header appeared.
 *
 * Ideally, this function would always pick up the next sector
 * directly following the previous sector. But it might just encounter
 * a bigger sector gap which follows the previous sector or which
 * precedes the first sector to be read. Both cases are handled here.
 */
static BOOL
find_next_sector(
	const void *	_track_data,
	int				track_data_size,
	int				track_data_start,
	int *			sector_header_offset_ptr,
	int *			bit_offset_ptr)
{
	const UWORD * track_data = (UWORD *)_track_data;
	int track_data_size_in_words = track_data_size / sizeof(UWORD);
	BOOL sync_marker_found = FALSE;
	ULONG pattern, mask;
	UWORD data;
	int sync_marker_offset;
	int bit_offset;

	/* The sector must start with two 0 bytes, in MFM-encoded
	 * form. This will come out as either a 0xAAAA or 0x5555
	 * 16 bit word. We are looking at a bit stream, which is why
	 * the MFM-encoded two 0 bytes may start at any bit offset.
	 *
	 * The two MFM-encoded 0 bytes are followed by a sync marker
	 * which is stored as the two 16 bit words 0x4489,0x4489.
	 *
	 * What we are going to do here is look for the 0xAAAA
	 * 16 bit word, which might show up as 0x5555. If found,
	 * we skip it and any padding that might follow it (the
	 * sector gap).
	 *
	 * The 0xAAAA 16 bit words must be followed by the sync
	 * marker. Because we are looking at a bit stream, the
	 * sync marker need not be perfectly aligned to the
	 * most significant bit of each word we look at. Hence,
	 * we must compare the data against a search pattern
	 * bit string which is shifted around until we find the
	 * right bit position at which the pattern matches.
	 *
	 * Once the right bit position is found, we take note
	 * of the word and bit position at which we found the sync
	 * marker. Decoding the sector header will make use of it.
	 */
	sync_marker_offset = track_data_start / sizeof(UWORD);

	/* Don't bother searching for a sync marker in the last
	 * few words of the track.
	 */
	while(sync_marker_offset < track_data_size_in_words - 4)
	{
		/* We have to find the pattern which introduces
		 * the sector header, or which might be part of
		 * the sector gap padding. Whether we find the
		 * first or the second of these markers is of
		 * no importance.
		 */
		data = track_data[sync_marker_offset++];

		if(data != 0xAAAA && data != 0x5555)
		{
			/* Didn't find it just yet. */
			continue;
		}

		/* Skip any padding in the form of a sector gap. */
		while(track_data[sync_marker_offset] == data)
		{
			if(sync_marker_offset + 2 >= track_data_size_in_words)
				break;

			sync_marker_offset++;
		}

		/* We need to find the bit string 0xAAAA 0x4489 0x4489
		 * in the track data to follow. If we're lucky, then the
		 * bit string will appear as 0x4489 0x4489, but if not, then
		 * bits of the leading 0xAAAA will precede the sync
		 * marker.
		 */
		data = track_data[sync_marker_offset];

		/* The sync marker and what precedes it may appear
		 * at any bit position. Hence we test them all.
		 */
		for(bit_offset = 0 ; bit_offset < 16 ; bit_offset++)
		{
			/* First sync word and maybe bits of the preceding
			 * 0xAAAA word leading up to it.
			 */
			pattern = ((0xAAAA << (16 - bit_offset)) | (0x4489 >> bit_offset)) & 0xFFFF;
			if(pattern == data && sync_marker_offset + 1 < track_data_size_in_words)
			{
				/* Second sync word and maybe bits of the preceding
				 * sync word leading up to it.
				 */
				pattern = ((0x4489 << (16 - bit_offset) | (0x4489 >> bit_offset))) & 0xFFFF;
				if(pattern == track_data[sync_marker_offset + 1])
				{
					/* Pattern is aligned to the most significant bit
					 * of the track data word?
					 */
					if (bit_offset == 0)
					{
						/* And we're done here. */
						sync_marker_found = TRUE;
						break;
					}
					/* Check the remaining bits of the second sync word. */
					else if (sync_marker_offset + 2 < track_data_size_in_words)
					{
						mask = (0xFFFF << (16 - bit_offset)) & 0xFFFF;

						/* We reuse the search pattern and just shave off
						 * the bits which we don't need to look at.
						 */
						if((pattern & mask) == (track_data[sync_marker_offset + 2] & mask))
						{
							/* And we're done here. */
							sync_marker_found = TRUE;
							break;
						}
					}
				}
			}
		}

		/* Are we finished yet? */
		if(sync_marker_found)
		{
			/* The two 0xAAAA words precede the sync marker.
			 * The sector decoding needs these to be in the right
			 * position.
			 */
			(*sector_header_offset_ptr)	= (sync_marker_offset - 2) * sizeof(UWORD);
			(*bit_offset_ptr)			= bit_offset;

			break;
		}
	}

	return(sync_marker_found);
}

/****************************************************************************/

/* Retrieve a 32 bit word which might not be perfectly aligned to the
 * most significant bit of the memory it is stored in. Ugly stuff,
 * but we're not exactly optimizing for speed here.
 */
static ULONG
get_realigned_word(const ULONG * source_buffer, int offset, int bit_offset)
{
	ULONG result;

	if(bit_offset == 0)
		result = source_buffer[offset];
	else
		result = (source_buffer[offset] << bit_offset) | (source_buffer[offset+1] >> (32 - bit_offset));

	return(result);
}

/****************************************************************************/

/* Decode a single MFM-encoded sector, including its header
 * and data. The sector data may not be perfectly aligned to
 * a 32 bit word boundary, and we need to adjust for that.
 */
static void
decode_sector_data(
	const void *			_source_buffer,
	struct sector_header *	_sector_header,
	void *					_data_area,
	int						num_bytes_per_sector,
	int						bit_offset,
	ULONG *					header_checksum_ptr,
	ULONG *					data_area_checksum_ptr)
{
	const ULONG * source_buffer = (ULONG *)_source_buffer;
	ULONG * sector_header = (ULONG *)_sector_header;
	ULONG * data_area = _data_area;
	const int num_longs_per_sector = (int)(num_bytes_per_sector / sizeof(ULONG));
	int from, to, i;
	ULONG odd, even;
	ULONG header_checksum, data_area_checksum;

	/* Decode the sector header (32 bytes). This is broken up into
	 * three 32 bit words which are encoded individually, a block
	 * of 16 bytes and two more 32 bit words encoded individually.
	 *
	 * We start with the sync pattern and the format/track/sector
	 * information. Each of these is encoded as a single 32 bit word,
	 * which means that the two parts of the MFM-encoded data are
	 * stored as two 32 bit words, with the "odd" bits in the first
	 * word and the "even" bits in the second word following the
	 * first word.
	 *
	 * MFM encoding stores each data bit as two bits. On the Amiga
	 * this is accomplished by storing the odd-numbered and even-numbered
	 * bits separately (the most significant bit of a word is considered
	 * an odd-numbered bit, and the least significant bit of a word is
	 * considered an even-numbered bit).
	 *
	 * This is why the decoding has to read two different words and
	 * put the respective odd and even bits back together again. What
	 * is removed before the two bit sets are combined are the so-called
	 * "clock bits" or "fill bits" (this is what the 0x55555555 bit mask
	 * accomplishes).
	 *
	 * The "clock bits" are inserted as part of the MFM-encoding process
	 * and have the effect of introducing magnetic flux changes into the
	 * encoded data which the floppy read head can detect. Detection is
	 * much more reliable for a signal which changes than for one which
	 * is constant if you have no common external clock source. These
	 * "clock bits" provide this mechanism: when they are added to the
	 * even and odd bit patterns, they make sure that every 1 bit is
	 * followed by a 0 bit and the other way round.
	 *
	 * The exception to this encoding scheme is the sync pattern
	 * which is stored as the two words 0x4489 0x4489 in the sector
	 * header: 0x4489 = 0100010010001001. In the sync pattern there
	 * are two zero bits with no "clock bits" to compensate for the
	 * long sequence of zeroes. Because this is not valid MFM encoded
	 * data the sync pattern cannot appear in the sector header or
	 * the sector data area. This is what makes it special, and
	 * suitable as the sector synchronization pattern.
	 *
	 * MFM-encoding Amiga-format disk data has a handful more rules
	 * which are only relevant for the encoding process, and luckily
	 * we do not have to deal with them here...
	 */
	for(i = from = to = 0 ; i < 2 ; i++, to++, from += 2)
	{
		odd		= get_realigned_word(source_buffer, from, bit_offset);
		even	= get_realigned_word(source_buffer, from + 1, bit_offset);

		sector_header[to] = ((0x55555555 & odd) << 1) | (0x55555555 & even);
	}

	/* The header checksum includes the format/track/sector information,
	 * which we just read.
	 *
	 * Both the sector header and data area checksums are calculated by
	 * XOR'ing the previous checksum value (initial value = 0) with both
	 * the odd/even bits of each 32 bit word as they were before MFM-encoding
	 * added the "clock bits". You might say that the checksums are a by-product
	 * of the MFM-encoding process.
	 */
	header_checksum = (0x55555555 & odd) ^ (0x55555555 & even);

	/* This takes care of the sector label information. We do not actually
	 * need this data, but it must be decoded for the header checksum
	 * test to be performed correctly.
	 */
	for(i = 2 ; i < 6 ; i++, to++, from += 2)
	{
		odd		= get_realigned_word(source_buffer, from, bit_offset);
		even	= get_realigned_word(source_buffer, from + 1, bit_offset);

		sector_header[to] = ((0x55555555 & odd) << 1) | (0x55555555 & even);

		/* The sector label information is part of the header
		 * checksum, too.
		 */
		header_checksum ^= (0x55555555 & odd) ^ (0x55555555 & even);
	}

	/* And these are the checksums (for the sector header and data area,
	 * respectively).
	 */
	for(i = 6 ; i < 8 ; i++, to++, from += 2)
	{
		odd		= get_realigned_word(source_buffer, from, bit_offset);
		even	= get_realigned_word(source_buffer, from + 1, bit_offset);

		sector_header[to] = ((0x55555555 & odd) << 1) | (0x55555555 & even);
	}

	/* Decode the data area of the sector. */
	data_area_checksum = 0;

	for(i = 0 ; i < num_longs_per_sector ; i++, from++)
	{
		odd		= get_realigned_word(source_buffer, from, bit_offset);
		even	= get_realigned_word(source_buffer, from + num_longs_per_sector, bit_offset);

		data_area[i] = ((0x55555555 & odd) << 1) | (0x55555555 & even);

		/* Same checksum method as used for the sector header. */
		data_area_checksum ^= (0x55555555 & odd) ^ (0x55555555 & even);
	}

	(*header_checksum_ptr)		= header_checksum;
	(*data_area_checksum_ptr)	= data_area_checksum;
}

/****************************************************************************/

/* Check the sector header for validity and consistency (what little there can
 * be done).
 */
static BOOL
sector_header_is_valid(
	const struct sector_header *	header,
	int								track,
	int								num_sectors,
	ULONG							header_checksum)
{
	BOOL is_valid;

	is_valid = (
		header->format			== AMIGA_10_FORMAT &&
		header->track_number	== track &&
		header->sector_number	<  num_sectors &&
		header->header_checksum	== header_checksum
	);

	return(is_valid);
}

/****************************************************************************/

/* These are used in the definition of the command line template below.
 * Each type is the same size as a LONG, which is what ReadArgs() expects.
 * The typedefs add a little bit of information to each parameter
 * definition.
 */
typedef LONG	SWITCH;
typedef STRPTR	KEY;
typedef LONG *	NUMBER;

/****************************************************************************/

int
main(int argc, char **argv)
{
	const TEXT template_string[] = "DRIVE/A,RETRIES/K/N,TO/K,DEBUG=DIAGNOSTICS/S";

	struct
	{
		KEY		Drive;
		NUMBER	Retries;
		KEY		To;
		SWITCH	Debug;
	} opts;

	struct MsgPort * disk_port = NULL;
	struct IOExtTD * disk_request = NULL;
	UBYTE * encoded_track_data = NULL;
	UBYTE * raw_sector = NULL;
	UBYTE * track_data = NULL;
	int encoded_track_data_size = 0;
	int track_data_size = 0;
	struct DriveGeometry geometry;
	LONG error;
	int num_tracks = 2 * 80, num_sectors = NUMSECS;
	int safety_margin;
	const int num_bytes_per_sector = 512;
	const int raw_sector_size = sizeof(struct sector_header) + num_bytes_per_sector;
	const int encoded_raw_sector_size = sizeof(UWORD) * raw_sector_size;
	int track;
	ULONG set_of_sectors_found;
	ULONG set_of_valid_sector_data;
	int num_valid_sector_header;
	int num_valid_sector_data;
	BOOL motor_is_on = FALSE;
	int search_offset, bit_offset;
	int sector_header_offset;
	int num_sectors_decoded;
	int result = RETURN_ERROR;
	struct sector_header * sector_header;
	UBYTE * data_area;
	ULONG header_checksum;
	ULONG data_area_checksum;
	BOOL file_system_is_inhibited = FALSE;
	struct DevProc * dvp = NULL;
	const struct DosList * dol_head;
	const struct DeviceNode * dn;
	BOOL dos_list_locked = FALSE;
	const struct FileSysStartupMsg * fssm;
	const struct DosEnvec * de;
	const TEXT * drive_name;
	size_t len;
	TEXT dos_device_name[256];
	TEXT * colon;
	const TEXT * device_name;
	TEXT device_name_copy[256];
	ULONG device_unit;
	BOOL retry_reading_track;
	int max_retries = 10;
	int num_retries;
	int num_readable_sector_data_area = 0;
	BOOL print_diagnostic_info = FALSE;
	BOOL drive_parameters_acceptable = TRUE;
	struct RDArgs * rda = NULL;
	const TEXT * program_name = (TEXT *)argv[0];
	const TEXT * output_file_name = NULL;
	BPTR output_file = ZERO;
	LONG output_file_size = 0;

	if(((struct Library *)DOSBase)->lib_Version < 36)
		goto out;

	memset(&opts, 0, sizeof(opts));

	rda = ReadArgs(template_string, (LONG *)&opts, NULL);
	if(rda == NULL)
	{
		TEXT error_message[256];

		Fault(IoErr(), NULL, error_message, sizeof(error_message));

		Printf("%s: %s\n", program_name, error_message);

		result = RETURN_ERROR;
		goto out;
	}

	/* This should be an Amiga floppy disk device name. */
	drive_name = opts.Drive;

	/* Enable per-sector diagnostic output? */
	if(opts.Debug)
		print_diagnostic_info = TRUE;

	/* If the track data cannot be decoded, how many times
	 * to try again to read and decode it?
	 */
	if(opts.Retries != NULL)
	{
		LONG number = (*opts.Retries);

		if(number < 1)
		{
			Printf("%s: Number of retries must be 1 or higher.\n", program_name);

			result = RETURN_ERROR;
			goto out;
		}

		max_retries = number;
	}

	/* Write the decoded track data to a file? */
	if(opts.To != NULL)
		output_file_name = opts.To;

	disk_port = CreateMsgPort();
	if(disk_port == NULL)
	{
		Printf("%s: Could not create message port.\n", program_name);
		goto out;
	}

	disk_request = (struct IOExtTD *)CreateIORequest(disk_port, sizeof(*disk_request));
	if(disk_request == NULL)
	{
		Printf("%s: Could not create I/O request.\n", program_name);
		goto out;
	}

	/* The following code is an exercise in figuring out if a file system
	 * device matches the properties of a "trackdisk.device"-like device,
	 * right down to the number of cylinders, heads and number of sectors.
	 * We key off the device name, e.g. "df0:" and then figure out the
	 * rest from there.
	 */

	/* This will mount the file system, if necessary. */
	dvp = GetDeviceProc(drive_name, NULL);
	if(dvp == NULL)
	{
		TEXT error_message[256];

		Fault(IoErr(), NULL, error_message, sizeof(error_message));

		Printf("%s: Could not access drive \"%s\" (%s).\n", program_name, drive_name, error_message);
		goto out;
	}

	/* This is for translating the drive name into
	 * something suitable for the Dos list access
	 * API in dos.library.
	 */
	len = strlen(drive_name);
	if(len >= sizeof(dos_device_name))
		len = sizeof(dos_device_name)-1;

	memmove(dos_device_name, drive_name, len);
	dos_device_name[len] = '\0';

	colon = (TEXT *)strchr((char *)dos_device_name, ':');
	if(colon != NULL)
		(*colon) = '\0';

	/* Now that the file system is definitely around, let's access
	 * the file system parameters. It is safe to both have called
	 * GetDeviceProc and to lock the Dos list for read access.
	 */
	dol_head = LockDosList(LDF_DEVICES|LDF_READ);
	dos_list_locked = TRUE;

	/* This should be a device name. */
	dn = (struct DeviceNode *)FindDosEntry(dol_head, dos_device_name, LDF_DEVICES);
	if(dn == NULL)
	{
		Printf("%s: Could not find drive \"%s\".\n", program_name, drive_name);
		goto out;
	}

	/* Note: The startup field can contain a small integer, such as used
	 *       by newcon-handler to indicate its mode of operation. A small
	 *       integer satisfies the same criteria as the test performed
	 *       by IS_VALID_BPTR_ADDRESS(). This is why this test includes a
	 *       check for the startup field being an integer.
	 */
	if(dn->dn_Startup > 1024 && IS_VALID_BPTR_ADDRESS(dn->dn_Startup))
		fssm = BADDR(dn->dn_Startup);
	else
		fssm = NULL;

	if(fssm == NULL || TypeOfMem((APTR)fssm) == 0)
	{
		UnLockDosList(LDF_DEVICES|LDF_READ);
		dos_list_locked = FALSE;

		Printf("%s: Drive \"%s\" parameters are inconsistent.\n", program_name, drive_name);
		goto out;
	}

	device_unit = fssm->fssm_Unit;

	if(IS_VALID_BPTR_ADDRESS(fssm->fssm_Device))
		device_name = ((TEXT *)BADDR(fssm->fssm_Device)) + 1;
	else
		device_name = NULL;

	if(IS_VALID_BPTR_ADDRESS(fssm->fssm_Environ))
		de = BADDR(fssm->fssm_Environ);
	else
		de = NULL;

	if(device_name == NULL ||
	   TypeOfMem((APTR)device_name) == 0 ||
	   de == NULL ||
	   TypeOfMem((APTR)de) == 0)
	{
		UnLockDosList(LDF_DEVICES|LDF_READ);
		dos_list_locked = FALSE;

		Printf("%s: Drive \"%s\" parameters are inconsistent.\n", program_name, drive_name);
		goto out;
	}

	/* Check if the disk parameters match what a 3.5" or
	 * 5.25" floppy disk drive should have.
	 */
	if(de->de_LowCyl != 0)
		drive_parameters_acceptable = FALSE;

	if(de->de_SizeBlock * sizeof(LONG) != num_bytes_per_sector)
		drive_parameters_acceptable = FALSE;

	/* Is this a 5.25" floppy disk drive? */
	if (de->de_Surfaces * (de->de_HighCyl + 1) == 80)
	{
		if(de->de_BlocksPerTrack != 11)
			drive_parameters_acceptable = FALSE;
	}
	/* Is this a 3.5" floppy disk drive? */
	else if (de->de_Surfaces * (de->de_HighCyl + 1) == 160)
	{
		/* This should be a standard or high density 3.5"
		 * floppy disk drive.
		 */
		if(de->de_BlocksPerTrack != 11 && de->de_BlocksPerTrack != 22)
			drive_parameters_acceptable = FALSE;
	}
	else
	{
		drive_parameters_acceptable = FALSE;
	}

	if(NOT drive_parameters_acceptable)
	{
		UnLockDosList(LDF_DEVICES|LDF_READ);
		dos_list_locked = FALSE;

		Printf("%s: Drive \"%s\" does not appear to be an Amiga floppy disk device.\n", program_name, drive_name);
		goto out;
	}

	len = strlen(device_name);
	if(len >= sizeof(device_name_copy))
		len = sizeof(device_name_copy)-1;

	memmove(device_name_copy, device_name, len);
	device_name_copy[len] = '\0';

	UnLockDosList(LDF_DEVICES|LDF_READ);
	dos_list_locked = FALSE;

	/* Unless the ALLOW_NON_3_5 flag is set "trackdisk.device" will
	 * refuse to access 5.25" floppy disk drives or high density
	 * 3.5" disk drives.
	 */
	error = OpenDevice(device_name_copy, device_unit, (struct IORequest *)disk_request, TDF_ALLOW_NON_3_5);
	if(error != OK)
	{
		Printf("%s: Could not open \"%s\", unit %ld (error=%ld).\n", program_name, device_name_copy, device_unit, error);
		goto out;
	}

	/* Tell the file system to let go of the volume. We want
	 * to read from the disk and not see any interference
	 * from other writers.
	 */
	if(DoPkt1(dvp->dvp_Port, ACTION_INHIBIT, DOSTRUE) == DOSFALSE)
	{
		int error_code = IoErr();

		/* The file system may not have mounted this disk in the
		 * first place, which means that it does not need to be
		 * inhibited. This is indicated by the error code
		 * ERROR_DEVICE_NOT_MOUNTED, which we will ignore.
		 */
		if(error_code != ERROR_DEVICE_NOT_MOUNTED)
		{
			TEXT error_message[256];

			Fault(error_code, NULL, error_message, sizeof(error_message));

			Printf("%s: Could not inhibit drive \"%s\" (%s).\n", program_name, drive_name, error_message);
			goto out;
		}
	}
	else
	{
		file_system_is_inhibited = TRUE;
	}

	/* Let's see if there is a disk in the drive. */
	disk_request->iotd_Req.io_Command = TD_CHANGESTATE;

	DoIO((struct IORequest *)disk_request);

	if(disk_request->iotd_Req.io_Actual != 0)
	{
		Printf("%s: No disk in drive \"%s\".\n", program_name, drive_name);
		goto out;
	}

	/* Make an attempt to support both standard 3.5" and 5.25"
	 * disks, as well as high density 3.5" disks.
	 */
	memset(&geometry, 0, sizeof(geometry));

	disk_request->iotd_Req.io_Command	= TD_GETGEOMETRY;
	disk_request->iotd_Req.io_Data		= &geometry;
	disk_request->iotd_Req.io_Length	= sizeof(geometry);

	/* If the device supports the TD_GETGEOMETRY command and
	 * neither crashes nor ignore it, we'll have to take a quick
	 * look at the geometry data to see if it's in any way
	 * helpful.
	 */
	if(DoIO((struct IORequest *)disk_request) == OK &&
	   geometry.dg_SectorSize > 0 &&
	   geometry.dg_Cylinders > 0 &&
	   geometry.dg_Heads > 0 &&
	   geometry.dg_TrackSectors > 0)
	{
		num_tracks	= geometry.dg_Heads * geometry.dg_Cylinders;
		num_sectors	= geometry.dg_TrackSectors;

		if(geometry.dg_SectorSize != num_bytes_per_sector ||
		   num_tracks != 160 ||
		   (num_sectors != 11 && num_sectors != 22))
		{
			Printf("%s: Drive \"%s\" does not appear to be an Amiga floppy disk device.\n", program_name, drive_name);
			goto out;
		}
	}

	/* We are going to read complete tracks of data and this requires
	 * a bit more preparation than one might expect or find in the
	 * official documentation...
	 *
	 * Individual tracks are separated by a gap, and within the tracks
	 * the individual sectors are not separated by any gaps. The track
	 * data we may be able to retrieve may start with the gap because
	 * the hardware begins reading MFM-encoded data into the buffer
	 * as soon as it can. Hence what ends up in the buffer may not even
	 * begin with the first bit of the first sector header. So, how large
	 * can the gap between first sector header and the beginning of the
	 * buffer be?
	 *
	 * According to internal Commodore documentation, the "safety margin"
	 * (that's what the "gap" actually is) comes down to about 830 bytes.
	 * For high density floppy disks it should be twice this size.
	 */
	if(num_sectors == 22)
		safety_margin = 2 * 830;
	else
		safety_margin = 830;

	/* We will read raw MFM-encoded data, which encodes each
	 * byte as a word. Or rather, it encodes each original
	 * bit as two bits.
	 *
	 * We must be able to read one complete track, including the
	 * sector gap which introduces it. We might not be able to
	 * fully read the first sector, worst case being that we narrowly
	 * miss the opportunity to read the first sector which follows
	 * the sector gap. This is why we have to be able to read the
	 * sector gap again and the complete first sector. Also
	 * included are 4 bytes of padding.
	 */
	encoded_track_data_size = sizeof(UWORD) * (safety_margin + 4 + (num_sectors+1) * raw_sector_size + safety_margin);

	/* The track buffer must use chip memory. */
	encoded_track_data = AllocMem(encoded_track_data_size, MEMF_CHIP);
	if(encoded_track_data == NULL)
	{
		Printf("%s: Could not allocate track memory.\n", program_name);
		goto out;
	}

	/* This is filled in by the MFM decoding process and will
	 * provide a sector header and the data area associated
	 * with the sector.
	 */
	raw_sector = AllocMem(raw_sector_size, MEMF_ANY);
	if(raw_sector == NULL)
	{
		Printf("%s: Could not allocate sector header+data memory.\n", program_name);
		goto out;
	}

	/* The raw sector data starts with a 32 byte header, followed by
	 * 512 bytes of sector data.
	 */
	sector_header = (struct sector_header *)raw_sector;
	data_area = (UBYTE *)&sector_header[1];

	/* This is where the decoded sector data for the entire track will go. */
	if(output_file_name != NULL)
	{
		BPTR output_file_lock;
		LONG error_code;

		track_data_size = num_sectors * num_bytes_per_sector;

		track_data = AllocMem(track_data_size, MEMF_ANY);
		if(track_data == NULL)
		{
			Printf("%s: Could not allocate sector memory.\n", program_name);
			goto out;
		}

		/* Does a file or directory by this name already exist? */
		output_file_lock = Lock(output_file_name, SHARED_LOCK);
		if(output_file_lock != ZERO)
		{
			Printf("%s: \"%s\" already exists.\n", program_name, output_file_name);

			UnLock(output_file_lock);

			result = RETURN_ERROR;
			goto out;
		}

		error_code = IoErr();

		if(error_code != ERROR_OBJECT_NOT_FOUND)
		{
			TEXT error_message[256];

			Fault(error_code, NULL, error_message, sizeof(error_message));

			Printf("%s: Cannot create file \"%s\" (%s).\n", program_name, output_file_name, error_message);

			result = RETURN_ERROR;
			goto out;
		}

		output_file = Open(output_file_name, MODE_NEWFILE);
		if(output_file == ZERO)
		{
			TEXT error_message[256];

			Fault(IoErr(), NULL, error_message, sizeof(error_message));

			Printf("%s: Cannot create file \"%s\" (%s).\n", program_name, output_file_name, error_message);

			result = RETURN_ERROR;
			goto out;
		}
	}

	/* Now read each MFM-encoded track and break it down into
	 * individual Amiga format sectors, if possible.
	 */
	for(track = 0 ; track < num_tracks ; track++)
	{
		/* Stop this before we continue? */
		if(CheckSignal(SIGBREAKF_CTRL_C))
		{
			PrintFault(IoErr(), argv[0]);
			goto out;
		}

		Printf("Reading track #%ld...\n", track);

		/* It's possible, but not very likely that we might see the same
		 * sector fly by the read head twice. This is how we try to make
		 * sure we won't store it more than once.
		 */
		set_of_sectors_found = 0;
		num_valid_sector_header = 0;

		/* This is for keeping track of valid sector data. We might
		 * find a valid sector header but the sector data itself
		 * might not be in good shape.
		 */
		set_of_valid_sector_data = 0;
		num_valid_sector_data = 0;

		retry_reading_track = FALSE;
		num_retries = 0;

		/* Start with a clean slate. Unreadable sectors will
		 * be set to a predefined pattern.
		 */
		if(encoded_track_data != NULL)
			memset(encoded_track_data, 0xAA, encoded_track_data_size);

		do
		{
			/* Note that a "track" in the context of the TD_RAWREAD
			 * command is not the same thing as a "cylinder". That is,
			 * for a standard double density 3.5" floppy disk there are
			 * 11 sectors per track and not 22: you cannot read more
			 * than 11 individual sectors, and if you try, the hardware
			 * will just keep rereading the same data.
			 *
			 * Rereading the same data may have merit for data recovery,
			 * as subsequent read passes over the same magnetic media
			 * may succeed in retrieving different bit patterns, which
			 * make the difference between corrupted and sound data.
			 *
			 * Anyway, a standard double density 3.5" floppy disk
			 * features 160 tracks of 11 sectors each. The high
			 * density floppy format features the same number of
			 * tracks, but uses 22 sectors instead.
			 */
			disk_request->iotd_Req.io_Command	= TD_RAWREAD;
			disk_request->iotd_Req.io_Data		= encoded_track_data;
			disk_request->iotd_Req.io_Length	= encoded_track_data_size;
			disk_request->iotd_Req.io_Offset	= track;

			/* We assume that trackdisk.device will turn on the
			 * motor when the TD_RAWREAD command is executed.
			 */
			motor_is_on = TRUE;

			/* "trackdisk.device" will read as much data as you asked
			 * for (up to 32768 bytes, but not more) unless something
			 * goes wrong and an error is flagged. Don't bother checking
			 * the io_Actual field to verify how much data you got.
			 * Success of the TD_RAWREAD command will not even result in
			 * updating the field.
			 */
			/*error = DoIO((struct IORequest *)disk_request);*/

			disk_request->iotd_Req.io_Flags = IOTDF_INDEXSYNC | IOTDF_WORDSYNC;
			BeginIO((struct IORequest *)disk_request);
			error = WaitIO((struct IORequest *)disk_request);

			if(error != OK)
			{
				Printf("%s: Cannot read track #%ld (error=%ld).\n", program_name, track, error);
				goto out;
			}

			/* Check every sector which might be stored in the MFM-encoded
			 * track data just filled. We have no expectations of finding
			 * anything, mind you.
			 *
			 * If we do find something, though, each loop iteration should
			 * decode the MFM data and pick up searching for the next
			 * sector where it left off.
			 *
			 * Note that the order in which the decoder finds the individual
			 * sectors is essentially unpredictable. If the track data is
			 * sound, all the expected sectors should be present, though.
			 *
			 * We do not store the decoded sector data in any ordered form
			 * here. This would require checking the header data for
			 * consistency, so that the sector numbers could be trusted.
			 */
			for(num_sectors_decoded = 0, search_offset = 0 ;
			    num_sectors_decoded < num_sectors && search_offset < encoded_track_data_size - encoded_raw_sector_size ;
			    num_sectors_decoded++, search_offset = sector_header_offset + encoded_raw_sector_size)
			{
				/* Stop this before we continue? */
				if(CheckSignal(SIGBREAKF_CTRL_C))
				{
					PrintFault(IoErr(), argv[0]);
					goto out;
				}

				/* Try our best to find the next sector header in the track buffer. */
				if(CANNOT find_next_sector(
					encoded_track_data,
					encoded_track_data_size,
					search_offset,
					&sector_header_offset,
					&bit_offset))
				{
					break;
				}

				/* Decode the sector header and the data area contents. */
				decode_sector_data(
					&encoded_track_data[sector_header_offset],
					sector_header,
					data_area,
					num_bytes_per_sector,
					bit_offset,
					&header_checksum,
					&data_area_checksum
				);

				/* Did we see this sector before? First off, we need to make sure that the
				 * sector header contents appear to be valid.
				 */
				if(sector_header_is_valid(sector_header, track, num_sectors, header_checksum))
				{
					/* Is this the first time we're seeing this sector appear? */
					if((set_of_sectors_found & (1UL << sector_header->sector_number)) == 0)
					{
						/* Take note of it. */
						set_of_sectors_found |= (1UL << sector_header->sector_number);
						num_valid_sector_header++;

						/* Remember if the sector data is valid. */
						if(sector_header->data_area_checksum == data_area_checksum)
						{
							set_of_valid_sector_data |= (1UL << sector_header->sector_number);
							num_valid_sector_data++;
						}

						/* Store this track's sector data for later use. */
						if(track_data != NULL)
							memmove(&track_data[sector_header->sector_number * num_bytes_per_sector], data_area, num_bytes_per_sector);
					}
				}

				if(print_diagnostic_info)
				{
					TEXT printable[17], c;
					int i, j;

					Printf("    Sector %ld @ %ld:%ld", num_sectors_decoded, sector_header_offset, bit_offset);

					Printf("        format = 0x%02lx", sector_header->format);

					if(sector_header->format == AMIGA_10_FORMAT)
						Printf(" (OK)\n");
					else
						Printf(" (NOT OK -- should be 0x%02lx)\n", AMIGA_10_FORMAT);

					Printf("        track = %ld", sector_header->track_number);

					if(sector_header->track_number == track)
						Printf(" (OK)\n");
					else
						Printf(" (NOT OK -- should be %ld)\n", track);

					Printf("        sector = %ld (%ld more to go)", sector_header->sector_number, sector_header->sector_offset);

					if(sector_header->sector_number < num_sectors)
						Printf(" (OK)\n");
					else
						Printf(" (NOT OK -- should be < %ld)\n", num_sectors);

					Printf("        header checksum = 0x%08lx", sector_header->header_checksum);

					if(sector_header->header_checksum == header_checksum)
						Printf(" (OK)\n");
					else
						Printf(" (NOT OK -- should be 0x%08lx)\n", header_checksum);

					Printf("        data area checksum = 0x%08lx", sector_header->data_area_checksum);

					if(sector_header->data_area_checksum == data_area_checksum)
						Printf(" (OK)\n");
					else
						Printf(" (NOT OK -- should be 0x%08lx)\n", data_area_checksum);

					/* Print the contents of this sector "type hex" style :-) */
					for(i = 0 ; i < num_bytes_per_sector ; i += 4 * sizeof(ULONG))
					{
						/* Stop this before we continue? */
						if(CheckSignal(SIGBREAKF_CTRL_C))
						{
							PrintFault(IoErr(), argv[0]);
							goto out;
						}

						Printf("        %04lx: %08lx %08lx %08lx %08lx    ",
							i,
							((ULONG *)&data_area[i])[0],
							((ULONG *)&data_area[i])[1],
							((ULONG *)&data_area[i])[2],
							((ULONG *)&data_area[i])[3]
						);

						for(j = 0 ; j < 16 ; j++)
						{
							c = data_area[i+j];

							if((c & 0x7F) < ' ')
								c = '.';

							printable[j] = (c & 0x7F) >= ' ' ? c : '.';
						}

						printable[j] = '\0';

						Printf("    %s\n", printable);
					}

					Printf("\n");
				}
			}

			/* Are any of the sectors in this track missing or damaged? */
			if(num_valid_sector_header < num_sectors)
			{
				int num_missing_sectors;
				int i;

				Printf("Track #%ld is missing these sectors: ", track);

				for(i = num_missing_sectors = 0 ; i < num_sectors ; i++)
				{
					if((set_of_sectors_found & (1UL << i)) == 0)
						Printf("%s%ld", (num_missing_sectors++ == 0) ? "" : ", ", i);
				}

				Printf("\n");

				/* Let's try that again. */
				retry_reading_track = TRUE;
			}

			/* Is any of the sector data damaged? */
			if(num_valid_sector_data < num_sectors)
			{
				int num_damaged_sectors;
				int i;

				Printf("Track #%ld has damaged sector data: ", track);

				for(i = num_damaged_sectors = 0 ; i < num_sectors ; i++)
				{
					if((set_of_sectors_found & (1UL << i)) != 0 && (set_of_valid_sector_data & (1UL << i)) == 0)
						Printf("%s%ld", (num_damaged_sectors++ == 0) ? "" : ", ", i);
				}

				Printf("\n");

				/* Let's try that again. */
				retry_reading_track = TRUE;
			}

			/* If there's reason to try again, check if we hit
			 * the retry limit or the user has decided to skip
			 * rereading this particular track.
			 */
			if(retry_reading_track)
			{
				num_retries++;

				/* Did we hit the retry limit? */
				if (num_retries == max_retries)
				{
					Printf("Giving up after %ld read attempts.\n", num_retries);

					retry_reading_track = FALSE;
				}
				/* Does the user want to skip the remainder of this track? */
				else if (CheckSignal(SIGBREAKF_CTRL_D))
				{
					retry_reading_track = FALSE;
				}
				/* Otherwise, just say what we are trying to do. */
				else
				{
					Printf("Rereading track #%ld (attempt %ld of %ld)...%s\n",
						track, num_retries, max_retries,
						(num_retries == 1) ? " ([Ctrl+D] to skip this track)" : "");
				}
			}
		}
		while(retry_reading_track);

		/* Let's look at how we did reading an recovering
		 * data from this track.
		 */
		if (num_valid_sector_data == num_sectors)
		{
			/* Perfect :-) */
			Printf("Track #%ld appears to be correct. No physical damage was found.\n\n", track);
		}
		else if (num_valid_sector_header == num_sectors)
		{
			/* The sector header information appears to be correct,
			 * but the sector data area contents or the sector
			 * header data area checksum are damaged for some
			 * sectors.
			 */
			Printf("Track #%ld is damaged: the data stored in %ld sector(s) appears to have been corrupted.\n\n",
				track, num_sectors - num_valid_sector_data);

			result = RETURN_WARN;
		}
		else if (num_valid_sector_header == 0)
		{
			/* None of the sector header checksums appears to be
			 * correct. This could be an unformatted track (how likely
			 * is that?) or maybe even the entire disk has been
			 * formatted for use with IBM-PC compatibles ;-)
			 */
			Printf("Track #%ld appears to be unformatted; no Amiga-format data could be found.\n\n", track);

			result = RETURN_WARN;
		}
		else
		{
			/* Most, if not all sector header checksums are incorrect,
			 * and so is likely the associated sector data area.
			 */
			Printf("Track #%ld is damaged: the format information and/or data of %ld sector(s) appears to have been corrupted.\n\n",
				track, num_sectors - num_valid_sector_header);
		}

		if(output_file != ZERO)
		{
			if(Write(output_file, track_data, track_data_size) == -1)
			{
				TEXT error_message[256];

				Fault(IoErr(), NULL, error_message, sizeof(error_message));

				Printf("%s: Cannot write to \"%s\" (%s).\n", program_name, output_file_name, error_message);

				result = RETURN_ERROR;
				goto out;
			}

			output_file_size += track_data_size;
		}

		num_readable_sector_data_area += num_valid_sector_data;
	}

	if (num_readable_sector_data_area == num_sectors * num_tracks)
	{
		Printf("The disk appears to be correct.\n");

		result = RETURN_OK;
	}
	else if (num_readable_sector_data_area == 0)
	{
		Printf("The disk is completely unreadable. It may not be in Amiga format.\n");

		result = RETURN_WARN;
	}
	else
	{
		int percent;

		percent = (10000 * num_readable_sector_data_area) / (num_sectors * num_tracks);

		Printf("The disk appears to be damaged. About %ld.%02ld%% of its data may be recoverable.\n",
			percent / 100, percent % 100);

		result = RETURN_WARN;
	}

 out:

	/* Spin down the disk before we quit. */
	if(motor_is_on)
	{
		disk_request->iotd_Req.io_Command	= TD_MOTOR;
		disk_request->iotd_Req.io_Length	= 0;

		DoIO((struct IORequest *)disk_request);
	}

	if(file_system_is_inhibited)
		DoPkt1(dvp->dvp_Port, ACTION_INHIBIT, DOSFALSE);

	if(dvp != NULL)
		FreeDeviceProc(dvp);

	if(dos_list_locked)
		UnLockDosList(LDF_DEVICES|LDF_READ);

	if(encoded_track_data != NULL)
		FreeMem(encoded_track_data, encoded_track_data_size);

	if(track_data != NULL)
		FreeMem(track_data, track_data_size);

	if(raw_sector != NULL)
		FreeMem(raw_sector, raw_sector_size);

	if(disk_request != NULL)
	{
		if(disk_request->iotd_Req.io_Device != NULL)
			CloseDevice((struct IORequest *)disk_request);

		DeleteIORequest((struct IORequest *)disk_request);
	}

	if(disk_port != NULL)
		DeleteMsgPort(disk_port);

	if(output_file != ZERO)
	{
		Close(output_file);

		if(output_file_size != 0)
			SetProtection(output_file_name, FIBF_EXECUTE);
		else
			DeleteFile(output_file_name);
	}

	if(rda != NULL)
		FreeArgs(rda);

	return(result);
}
