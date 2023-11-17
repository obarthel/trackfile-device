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

#ifndef _MFM_ENCODING_H
#define _MFM_ENCODING_H

/****************************************************************************/

/* This enables the use of the MFM track encoding for experimentation,
 * testing and quality assurance work. It is not particularly useful beyond
 * these areas.
 */
/*#define ENABLE_MFM_ENCODING*/

/****************************************************************************/

#if defined(ENABLE_MFM_ENCODING)

/****************************************************************************/

#ifndef EXEC_LIBRARIES_H
#include <exec/libraries.h>
#endif /* EXEC_LIBRARIES_H */

/****************************************************************************/

#include <stddef.h>

/****************************************************************************/

/* Constants used in the sector header (in MFM format). */
#define MFM_ZERO		0xAAAAAAAA	/* The value 0 in MFM-encoded form */
#define MFM_SPECIAL_A1	0x44894489	/* Magic value identifying the sector header */

/****************************************************************************/

/* In the MFM-encoded form only the odd bits of a 32 bit word
 * carry the information.
 */
#define MFM_DATA_BIT_MASK 0x55555555

/****************************************************************************/

/* Size of the sector gap in bytes, according to what trackdisk.device
 * would use for a double density disk. Note that the gap will be partly
 * overwritten by the track data, owing to angular velocity differences
 * resulting from the read/write head position.
 *
 * Please note that for a high density disk the sector gap size must be
 * twice this size.
 */
#define MAXIMUM_SECTOR_GAP_SIZE 1660

/****************************************************************************/

/* Part of the sector header, identifying the Amiga 1.0 format */
#define AMIGA_10_FORMAT 0xFF

/****************************************************************************/

/* Sector header information, prior to MFM-encoding. */
struct fmt
{
	ULONG	fmt_type_track_sector_sector_off;	/* Format type, track number, sector number
												 * and sector offset
												 */
	ULONG	fmt_sector_label[4];				/* 16 bytes of sector label data */
	ULONG	fmt_header_checksum;				/* Checksum for the 5 preceding 32 bit words */
};

/* Complete sector data, prior to MFM-encoding. The size of this data
 * structure should be exactly 544 bytes: 4 (sector sync pattern) +
 * 4 (format type, track number, sector number, sector offset) +
 * 16 (sector label data) + 4 (sector header checksum) +
 * 4 (sector data checksum) + 512 (sector data).
 */
struct sec
{
	ULONG		sec_zeros_special_a1;			/* Space for sector header; no useful
												 * data here on account that the header
												 * is only relevant in MFM-encoded
												 * form.
												 */
	struct fmt	sec_fmt;						/* The sector header information */
	ULONG		sec_data_checksum;				/* Checksum for the sector data */
	ULONG		sec_data[512 / sizeof(ULONG)];	/* The sector data itself */
};

/****************************************************************************/

/* Sector header information, after MFM-encoding. Note that compared
 * to the previous data structure definitions everything is now
 * twice as large.
 */
struct fmt2
{
	ULONG	fmt2_type_track_sector_sector_off[2];
	ULONG	fmt2_sector_label[2*4];
	ULONG	fmt2_header_checksum[2];
};

/* Complete sector data, after MFM-encoding. The size of this data
 * structure should be exactly 1088 bytes.
 */
struct sec2
{
	ULONG		sec2_zeros_a1[2];
	struct fmt2	sec2_fmt;
	ULONG		sec2_data_checksum[2];
	ULONG		sec2_data[(2*512) / sizeof(ULONG)];
};

/****************************************************************************/

/* This is for encoding 11 or 22 (high density disks) sectors in MFM format.
 *
 * Note that the buffer will have room for exactly that many sectors, each
 * 1088 bytes in size. On disk this will have to be padded with 1660 bytes
 * filled with the value 0xAA, producing what is known as the "sector gap".
 *
 * To read this data back, you need a buffer with at least 1660 bytes
 * plus 11+1 or 22+1 sectors worth of data so that you will be able to
 * process all the sectors regardless of where the "sector gap" turns up.
 * Note that for a high density disk (22 sectors per track) the sector gap
 * needs to be twice the size of a double density (11 sectors per
 * track) disk.
 *
 * Note: When writing encoded data back to the medium, trackdisk.device
 *       begins by writing the sector gap (1660 bytes filled with the MFM
 *       pattern 0xAA, which decodes as 0), then follows this up with the
 *       11 or 22 sectors, back to back. When the entire buffer has been
 *       written, the last sector may have overwritten part of the sector gap
 *       data which came before it. This is what the gap is for: make up for
 *       how much data can be recorded on the medium on the different tracks
 *       of the disk.
 */
struct mfm_code_context
{
	int		mcc_num_sectors;
	size_t	mcc_sector_size;
	size_t	mcc_sector_position;
	size_t	mcc_sector_gap_size;
	size_t	mcc_data_size;
	size_t	mcc_data_position;
	size_t	mcc_saved_data_position;
	UBYTE	mcc_previous_byte;
	UBYTE	mcc_pad[3];
	UBYTE	mcc_data[2 * 544];
};

/****************************************************************************/

void free_mfm_code_context(struct Library * sysbase, struct mfm_code_context * mcc);
void reset_mfm_code_context(struct mfm_code_context * mcc);
struct mfm_code_context * create_mfm_code_context(struct Library * sysbase, int num_sectors);
void mfm_encode_sector(struct mfm_code_context *mcc, int track, int sector, int sector_offset, const void * sector_data);
void mfm_encode_rotate_data(struct mfm_code_context * mcc, int offset);

/****************************************************************************/

#endif /* ENABLE_MFM_ENCODING */

/****************************************************************************/

#endif /* _MFM_ENCODING_H */
