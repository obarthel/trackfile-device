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
 * sc link debug=sf addsym mfm_encode_decode.c
 */

#include <exec/memory.h>
#include <proto/exec.h>

/****************************************************************************/

#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>

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

/* This is for encoding 11 or 22 (high density disks) sectors in MFM
 * format. Note that the buffer will have room for exactly that many
 * sectors, each 1088 bytes in size. On disk this will have to be
 * padded with 1660 bytes filled with the value 0xAA, producing what
 * known as the "sector gap". To read this data back, you need a
 * buffer with at least 1660 bytes plus 11+1 or 22+1 sectors worth of
 * data so that you will be able to process all the sectors regardless
 * of where the "sector gap" turns up.
 *
 * Note: When writing encoded data back to the medium, trackdisk.device
 *       begins by writing the sector gap (1660 bytes filled with the
 *       MFM pattern 0xAA, meaning 0), then follows this up with the
 *       11 or 22 sectors, back to back. When the entire buffer has
 *       been written, the last sector may have overwritten part of the
 *       sector gap data which came before it. This is what the gap is
 *       for: make up for hardware tolerances.
 */
struct mfm_code_context
{
	int		mcc_num_sectors;
	size_t	mcc_sector_size;
	size_t	mcc_sector_position;
	size_t	mcc_data_size;
	size_t	mcc_data_position;
	size_t	mcc_saved_data_position;
	UBYTE	mcc_previous_byte;
	UBYTE	mcc_pad[3];
	UBYTE	mcc_data[2 * 544];
};

/****************************************************************************/

/* This releases the memory allocated for MFM encoding sector data. */
void
free_mfm_code_context(struct mfm_code_context * mcc)
{
	if(mcc != NULL)
		FreeMem(mcc, sizeof(*mcc) + sizeof(mcc->mcc_data) * (mcc->mcc_num_sectors - 1));
}

/****************************************************************************/

/* Call this function before you encode the first sector
 * of a track.
 */
void
reset_mfm_code_context(struct mfm_code_context * mcc)
{
	mcc->mcc_sector_position		= 0;
	mcc->mcc_saved_data_position	= 0;
	mcc->mcc_data_position			= 0;
	mcc->mcc_previous_byte			= 0;
}

/****************************************************************************/

/* Allocate memory for one or more sectors of data which will be
 * filled in by the sector encoding process.
 */
struct mfm_code_context *
create_mfm_code_context(int num_sectors)
{
	struct mfm_code_context * mcc;

	mcc = AllocMem(sizeof(*mcc) + sizeof(mcc->mcc_data) * (num_sectors - 1), MEMF_ANY|MEMF_PUBLIC);
	if(mcc != NULL)
	{
		mcc->mcc_num_sectors	= num_sectors;
		mcc->mcc_sector_size	= sizeof(mcc->mcc_data);
		mcc->mcc_data_size		= mcc->mcc_sector_size * num_sectors;

		reset_mfm_code_context(mcc);
	}

	return(mcc);
}

/****************************************************************************/

/* Save the current data encoding position in the buffer.
 * We will use that later when mfm_encode_restore_data_position()
 * is called prior to calculating the data area checksum.
 */
void
mfm_encode_save_data_position(struct mfm_code_context * mcc)
{
	mcc->mcc_saved_data_position = mcc->mcc_data_position;
}

/****************************************************************************/

/* Restore the data encoding position in the buffer, as stored
 * by mfm_encode_save_data_position() and also make sure that the
 * byte preceding that position is read, too. This is to make
 * sure that the mfm_encode_half_the_bits() function will correctly
 * set the most significant bit of the MFM-encoded word.
 */
void
mfm_encode_restore_data_position(struct mfm_code_context * mcc)
{
	mcc->mcc_data_position = mcc->mcc_saved_data_position;
	mcc->mcc_previous_byte = mcc->mcc_data[mcc->mcc_data_position - 1];
}

/****************************************************************************/

/* Move on to the next track sector to be encoded and also
 * update the next data encoding position.
 */
void
mfm_encode_advance_sector(struct mfm_code_context * mcc)
{
	mcc->mcc_sector_position += mcc->mcc_sector_size;
	mcc->mcc_data_position = mcc->mcc_sector_position;
}

/****************************************************************************/

/* Store a single encoded word and advance the encoded data pointer. */
void
mfm_encode_store_encoded_word(struct mfm_code_context * mcc, ULONG value)
{
	if(mcc->mcc_data_position + sizeof(value) <= mcc->mcc_data_size)
	{
		ULONG * buffer = (ULONG *)&mcc->mcc_data[mcc->mcc_data_position];

		(*buffer) = value;

		/* Remember this for the next mfm_encode_half_the_bits() call. */
		mcc->mcc_previous_byte = (UBYTE)value;

		mcc->mcc_data_position += sizeof(value);
	}
}

/****************************************************************************/

/* Advance the encoded data pointer by the size of a number
 * of encoded words.
 */
void
mfm_encode_skip_encoded_words(struct mfm_code_context * mcc, int count)
{
	/* Note that mcc->mcc_previous_byte is not updated here.
	 * This is by design. mfm_encode_restore_data_position() will
	 * update mcc->mcc_previous_byte later.
	 */
	if(mcc->mcc_data_position + count * 2 * sizeof(ULONG) <= mcc->mcc_data_size)
		mcc->mcc_data_position += count * 2 * sizeof(ULONG);
}

/****************************************************************************/

/* MFM-encode half the bits of a single 32 bit word (either the odd
 * or the even ones).
 */
void
mfm_encode_half_the_bits(struct mfm_code_context * mcc, ULONG d0)
{
	ULONG d2;

	/* Clear the even bits. These will be replaced by
	 * the clock/fill bits.
	 */
	d0 &= MFM_DATA_BIT_MASK;

	/* Example:
	 * d0:    11 11 00 00 11 11 00 00
	 * d0: -> 01 01 00 00 01 01 00 00
	 */

	/* Flip the odd bits. */
	d2 = d0 ^ MFM_DATA_BIT_MASK;

	/* Example:
	 * d0:    01 01 00 00 01 01 00 00
	 * d2: -> 00 00 01 01 00 00 01 01
	 */

	/* Add the clock/fill bits, producing the
	 * MFM encoding pattern which has the effect
	 * that no more than two consecutive bits in
	 * sequence have all the same value.
	 *
	 * The most significant bit is set to 1,
	 * assuming that the bit preceding it will be
	 * a zero.
	 */

	/* Example:
	 * d2:            00 00 01 01 00 00 01 01
	 * d2 (left):  -> 00 00 00 10 10 00 00 10
	 * d2 (left):  -> 10 00 00 10 10 00 00 10
	 *
	 * d2:            00 00 01 01 00 00 01 01
	 * d2 (right): -> 00 00 10 10 00 00 10 10
	 *
	 * d2 (left):     10 00 00 10 10 00 00 10
	 * d2 (right):    00 00 10 10 00 00 10 10
	 * d2          -> 00 00 00 10 00 00 00 10
	 *
	 * d0:    01 01 00 00 01 01 00 00
	 * d2:    00 00 00 10 00 00 00 10
	 * d0: -> 01 01 00 10 01 01 00 10
	 */
	d0 |= ((d2 >> 1) | (1UL << 31)) & (d2 << 1);

	/* Note: the Amiga Unix "flopf.c" code performs the
	 *       the same encoding in two steps instead of
	 *       the three operations adapted from the
	 *       trackdisk.device V33 code.
	 *
	 *       Why do these different algorithms produce
	 *       the same effect?
	 */
	/*
	d0 &= MFM_DATA_BIT_MASK;
	d0 |= ~((d0 << 1) | (d0 >> 1) | MFM_DATA_BIT_MASK);
	*/

	/* Clear the clock bit which we set above
	 * if the bit immediately preceding it
	 * was a one.
	 */
	if(mcc->mcc_data_position > 0 && (mcc->mcc_previous_byte & 1) != 0)
		d0 &= ~(1UL << 31);

	mfm_encode_store_encoded_word(mcc, d0);
}

/****************************************************************************/

/* After the encoding of a 32 bit word has been performed, make sure that the
 * clock bit following the last data bit of the preceding MFM-encoded word
 * is set correctly.
 */
void
mfm_encode_fix_clock_bit(struct mfm_code_context * mcc)
{
	if(mcc->mcc_data_position > 0)
	{
		UBYTE previous_byte = mcc->mcc_previous_byte;

		/* Is the previous bit a zero? */
		if((previous_byte & 1) == 0)
		{
			/* Make sure that the clock bit is set. */
			if((previous_byte & (1 << 6)) == 0)
				mcc->mcc_data[mcc->mcc_data_position] = previous_byte | (1 << 7);
		}
		else
		{
			/* The previous bit was a one. Clear the clock bit. */
			mcc->mcc_data[mcc->mcc_data_position] &= ~(1 << 7);
		}
	}
}

/****************************************************************************/

/* Encode a single 32 bit word, storing the result in the buffer. The buffer
 * pointer will be updated accordingly.
 */
void
mfm_encode_word(struct mfm_code_context * mcc, ULONG data)
{
	/* First the even bits, then the odd bits. */
	mfm_encode_half_the_bits(mcc, data >> 1);
	mfm_encode_half_the_bits(mcc, data);
}

/****************************************************************************/

/* Retrieve a single MFM-encoded 32 bit word, returning the result with the
 * clock/fill bits set to zero. The buffer pointer will be updated
 * accordingly. Returns 0 if more data is read than is available.
 */
ULONG
mfm_encode_get_word(struct mfm_code_context * mcc)
{
	ULONG data;

	if(mcc->mcc_data_position + sizeof(data) <= mcc->mcc_data_size)
	{
		const ULONG * buffer = (ULONG *)&mcc->mcc_data[mcc->mcc_data_position];

		data = buffer[0] & MFM_DATA_BIT_MASK;

		mcc->mcc_data_position += sizeof(*buffer);
	}
	else
	{
		data = 0;
	}

	return(data);
}

/****************************************************************************/

/* Decode a single 32 bit word, returning the result. The buffer pointer will
 * be updated accordingly. Returns 0 if more data is read than is available.
 */
ULONG
mfm_decode_word(struct mfm_code_context * mcc)
{
	ULONG data;

	if(mcc->mcc_data_position + 2 * sizeof(data) <= mcc->mcc_data_size)
	{
		const ULONG * buffer = (ULONG *)&mcc->mcc_data[mcc->mcc_data_position];

		data = ((buffer[0] & MFM_DATA_BIT_MASK) << 1) | (buffer[1] & MFM_DATA_BIT_MASK);

		mcc->mcc_data_position += 2 * sizeof(*buffer);
	}
	else
	{
		data = 0;
	}

	return(data);
}

/****************************************************************************/

/* Calculate the checksum for the sector buffer after it has been encoded. */
ULONG
mfm_calculate_buffer_checksum(
	const struct mfm_code_context *	mcc,
	size_t							start_position,
	size_t							stop_position)
{
	ULONG sum = 0;

	if(start_position <= stop_position &&
	   start_position < mcc->mcc_sector_size &&
	   stop_position <= mcc->mcc_sector_size)
	{
		const ULONG * encoded_data = (ULONG *)&mcc->mcc_data[mcc->mcc_sector_position + start_position];
		size_t pos;

		for(pos = start_position ; pos < stop_position ; pos += sizeof(*encoded_data))
			sum ^= (*encoded_data++);

		/* Keep only the data bits, removing the clock/fill bits. */
		sum &= MFM_DATA_BIT_MASK;
	}

	return(sum);
}

/****************************************************************************/

/* Encode a single sector, with the given track number, sector number and
 * sector offset.
 */
void
mfm_encode_sector(
	struct mfm_code_context *	mcc,
	int							track,
	int							sector,
	int							sector_offset,
	const ULONG *				sector_data)
{
	ULONG null_pattern;
	ULONG checksum;
	size_t i;

	/* This takes care of the sector header's leading 0x0000 word. We need to
	 * set the most significant bit to zero if the bit preceding it was one.
	 */
	null_pattern = MFM_ZERO;
	if(mcc->mcc_data_position > 0 && (mcc->mcc_previous_byte & 1) != 0)
		null_pattern &= ~(1UL << 31);

	mfm_encode_store_encoded_word(mcc, null_pattern);

	/* Store the sector's signature value, which comes out as something which
	 * correct MFM encoding cannot create.
	 */
	mfm_encode_store_encoded_word(mcc, MFM_SPECIAL_A1);

	/* This is the sector header information. It is in "Amiga 1.0 format",
	 * identifies the track number, the sector number and indicates how
	 * many sectors (including this one) will appear before the sector
	 * gap appears.
	 */
	mfm_encode_word(mcc,
		(AMIGA_10_FORMAT << 24) |
		(track << 16) |
		(sector << 8) |
		sector_offset
	);

	/* This is the sector label information. We store zeroes. */
	for(i = 0 ; i < 4 ; i++)
		mfm_encode_word(mcc, 0);

	/* Calculate the sector header checksum and store it. */
	checksum = mfm_calculate_buffer_checksum(mcc,
		offsetof(struct sec2, sec2_fmt),
		offsetof(struct sec2, sec2_fmt.fmt2_header_checksum)
	);

	mfm_encode_word(mcc, checksum);

	/* The buffer pointer is now referring to the
	 * data checksum, which will be filled in after
	 * we have calculated it.
	 */
	mfm_encode_save_data_position(mcc);
	mfm_encode_skip_encoded_words(mcc, 1);

	/* Now encode the sector data. The odd bits are encoded first. */
	for(i = 0 ; i < (int)(512 / sizeof(ULONG)) ; i++)
		mfm_encode_half_the_bits(mcc, sector_data[i] >> 1);

	/* Now encode the even bits. */
	for(i = 0 ; i < (int)(512 / sizeof(ULONG)) ; i++)
		mfm_encode_half_the_bits(mcc, sector_data[i]);

	/* Finally, calculate the checksum of the data. */
	mfm_encode_restore_data_position(mcc);

	checksum = mfm_calculate_buffer_checksum(mcc,
		offsetof(struct sec2, sec2_data),
		sizeof(struct sec2)
	);

	/* Store the checksum and fix up the MFM-encoded
	 * long word which follows it, too.
	 */
	mfm_encode_word(mcc, checksum);
	mfm_encode_fix_clock_bit(mcc);

	/* And move on to the next sector. */
	mfm_encode_advance_sector(mcc);
}

/****************************************************************************/

/* This is used for decoding 11 or 22 (high density disks) sectors
 * which have been encoded in the Amiga 1.0 MFM disk format. The
 * decoded data will be stored in the individual sectors for which
 * memory was allocated. A single decoded sector yields 544 bytes
 * of data, as described by 'struct sec'. This includes the sector
 * header, the format information, sector label, sector header
 * checksum, data checksum and sector data.
 */
struct mfm_decode_context
{
	int		mdc_num_sectors;
	size_t	mdc_sector_size;
	size_t	mdc_data_size;
	size_t	mdc_data_position;
	ULONG	mdc_header_checksum;
	ULONG	mdc_data_checksum;
	UBYTE	mdc_data[544];
};

/****************************************************************************/

/* Release the memory allocated for MFM-decoding. */
void
free_mfm_decode_context(struct mfm_decode_context * mdc)
{
	if(mdc != NULL)
		FreeMem(mdc, sizeof(*mdc) + sizeof(mdc->mdc_data) * (mdc->mdc_num_sectors - 1));
}

/****************************************************************************/

/* Allocate memory for decoding Amiga 1.0 disk format sectors which
 * were MFM-encoded.
 */
struct mfm_decode_context *
create_mfm_decode_context(int num_sectors)
{
	struct mfm_decode_context * mdc;

	mdc = AllocMem(sizeof(*mdc) + sizeof(mdc->mdc_data) * (num_sectors - 1), MEMF_ANY|MEMF_PUBLIC);
	if(mdc != NULL)
	{
		mdc->mdc_num_sectors	= num_sectors;
		mdc->mdc_sector_size	= sizeof(mdc->mdc_data);
		mdc->mdc_data_position	= 0;
		mdc->mdc_data_size		= mdc->mdc_sector_size * num_sectors;
	}

	return(mdc);
}

/****************************************************************************/

/* The header and data checksums combine the odd and the even bits of a 32
 * bit word using exclusive-or bit operations. For the header checksum the
 * data is already available in combined form, we only have to take it
 * apart again.
 */
ULONG
mfm_get_bit_checksum_value(ULONG value)
{
	ULONG result;

	result = ((value >> 1) & MFM_DATA_BIT_MASK) ^ (value & MFM_DATA_BIT_MASK);

	return(result);
}

/****************************************************************************/

/* Decode a complete track, with all its sector data. Also produce the
 * respective sector header and data checksums of the decoded data.
 *
 * Note that the decoding process makes the following assumptions:
 *
 * 1. The buffer begins with the 0xAAAAAAAA,0x44894489 pattern of the first
 *    encoded MFM sector.
 *
 * 2. There are no gaps between the sectors. Each sector directly follows the
 *    previous one and there are no additional 0xAAAAAAAA words between
 *    the previous and the following sectors.
 */
void
mfm_decode_track(struct mfm_code_context * mcc, struct mfm_decode_context * mdc)
{
	ULONG header_checksum;
	ULONG data_checksum;
	struct sec * sec;
	ULONG data;
	int sector;
	int i;

	/* Start reading from the very first byte of the encoded data. */
	reset_mfm_code_context(mcc);

	/* Process all the encoded sector data, decode and store it. */
	for(sector = 0 ;
	    sector < mcc->mcc_num_sectors && mdc->mdc_data_position + sizeof(*sec) <= mdc->mdc_data_size ;
	    sector++, mdc->mdc_data_position += mdc->mdc_sector_size)
	{
		sec = (struct sec *)&mdc->mdc_data[mdc->mdc_data_position];

		/* Begin by skipping the header. We cannot decode it anyway
		 * because its purpose is to produce a reliably recognizable
		 * pattern in the MFM data which introduces each sector.
		 */
		mfm_encode_skip_encoded_words(mcc, 1);

		/* Here is where the sector data begins for real with the
		 * format byte, the track number, the sector number
		 * and the sector offset bytes.
		 */
		data = mfm_decode_word(mcc);

		header_checksum = mfm_get_bit_checksum_value(data);

		sec->sec_fmt.fmt_type_track_sector_sector_off = data;

		/* Now for the sector label information which is covered by
		 * the same checksum which also covers the format, etc.
		 * information that preceded it.
		 */
		for(i = 0 ; i < 4 ; i++)
		{
			data = mfm_decode_word(mcc);

			header_checksum ^= mfm_get_bit_checksum_value(data);

			sec->sec_fmt.fmt_sector_label[i] = data;
		}

		mdc->mdc_header_checksum = header_checksum;

		/* Finally, the checksum which covers the preceding
		 * five 32 bit words.
		 */
		sec->sec_fmt.fmt_header_checksum = mfm_decode_word(mcc);

		/* This is the checksum for the data which will follow it. */
		sec->sec_data_checksum = mfm_decode_word(mcc);

		/* The sector data for the odd and even bits is stored
		 * separately. The odd bits, with the clock/fill bits set to
		 * zero, are retrieved first and stored until we combine
		 * them with the even bits.
		 */
		data_checksum = 0;

		for(i = 0 ; i < (int)(512 / sizeof(ULONG)) ; i++)
		{
			data = mfm_encode_get_word(mcc);

			data_checksum ^= data;

			sec->sec_data[i] = data << 1;
		}

		/* Go over the same data and combine the already present
		 * odd bits with the even bits.
		 */
		for(i = 0 ; i < (int)(512 / sizeof(ULONG)) ; i++)
		{
			data = mfm_encode_get_word(mcc);

			data_checksum ^= data;

			sec->sec_data[i] |= data;
		}

		mdc->mdc_data_checksum = data_checksum;
	}
}

/****************************************************************************/

int
main(int argc, char ** argv)
{
	struct mfm_code_context * mcc;
	struct mfm_decode_context * mdc;
	UBYTE sector_data[512];
	int i;

	for(i = 0 ; i < 512 ; i++)
		sector_data[i] = 'A' + (i % 26);

	mcc = create_mfm_code_context(1);
	if(mcc != NULL)
	{
		mdc = create_mfm_decode_context(1);
		if(mdc != NULL)
		{
			mfm_encode_sector(mcc, 21, 5, 0, (ULONG *)sector_data);

			mfm_decode_track(mcc, mdc);
		}
	}

	return(EXIT_SUCCESS);
}
