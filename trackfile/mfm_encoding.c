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
 * What shall we do with all this useless beauty?
 */

#ifndef _SYSTEM_HEADERS_H
#include "system_headers.h"
#endif /* _SYSTEM_HEADERS_H */

/****************************************************************************/

#ifndef _TRACKFILE_DEVICE_H
#include "trackfile_device.h"
#endif /* _TRACKFILE_DEVICE_H */

/****************************************************************************/

#include "mfm_encoding.h"

/****************************************************************************/

#if defined(ENABLE_MFM_ENCODING)

/****************************************************************************/

#include "assert.h"

/****************************************************************************/

/* This releases the memory allocated for MFM encoding sector data. */
void
free_mfm_code_context(struct Library * SysBase, struct mfm_code_context * mcc)
{
	if(mcc != NULL)
		FreeVec(mcc);
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
	mcc->mcc_previous_byte			= MFM_ZERO & 0xFF;

	/* This initializes the sector gap to a well-defined state.
	 * It will contain MFM-encoded zeroes.
	 */
	memset(&mcc->mcc_data[mcc->mcc_data_size], (MFM_ZERO & 0xff), mcc->mcc_sector_gap_size);
}

/****************************************************************************/

/* Allocate memory for one or more sectors of data which will be
 * filled in by the sector encoding process.
 */
struct mfm_code_context *
create_mfm_code_context(struct Library * SysBase, int num_sectors)
{
	struct mfm_code_context * mcc;
	size_t sector_gap_size;

	ENTER();

	SHOWVALUE(num_sectors);

	ASSERT( num_sectors == 11 || num_sectors == 22 );

	/* The sector gap for high density disks is twice
	 * the size of the gap for double density disks.
	 */
	if(num_sectors == 22)
		sector_gap_size = 2 * MAXIMUM_SECTOR_GAP_SIZE;
	else
		sector_gap_size = MAXIMUM_SECTOR_GAP_SIZE;

	mcc = AllocVec(sizeof(*mcc) + sizeof(mcc->mcc_data) * (num_sectors - 1) + sector_gap_size, MEMF_ANY|MEMF_PUBLIC);
	if(mcc != NULL)
	{
		memset(mcc, 0, sizeof(*mcc));

		mcc->mcc_num_sectors		= num_sectors;
		mcc->mcc_sector_size		= sizeof(mcc->mcc_data);
		mcc->mcc_data_size			= mcc->mcc_sector_size * num_sectors;
		mcc->mcc_sector_gap_size	= sector_gap_size;

		SHOWVALUE(mcc->mcc_num_sectors);
		SHOWVALUE(mcc->mcc_sector_size);
		SHOWVALUE(mcc->mcc_data_size);
	}

	RETURN(mcc);
	return(mcc);
}

/****************************************************************************/

/* Save the current data encoding position in the buffer.
 * We will use that later when mfm_encode_restore_data_position()
 * is called prior to calculating the data area checksum.
 */
static void
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
static void
mfm_encode_restore_data_position(struct mfm_code_context * mcc)
{
	ASSERT( mcc->mcc_data_position > 0 );

	mcc->mcc_data_position = mcc->mcc_saved_data_position;
	mcc->mcc_previous_byte = mcc->mcc_data[mcc->mcc_data_position - 1];
}

/****************************************************************************/

/* Move on to the next track sector to be encoded and also
 * update the next data encoding position.
 */
static void
mfm_encode_advance_sector(struct mfm_code_context * mcc)
{
	ASSERT( mcc->mcc_sector_position + mcc->mcc_sector_size <= mcc->mcc_data_size );

	mcc->mcc_sector_position += mcc->mcc_sector_size;
	mcc->mcc_data_position = mcc->mcc_sector_position;
}

/****************************************************************************/

/* Store a single encoded word and advance the encoded data pointer. */
static void
mfm_encode_store_encoded_word(struct mfm_code_context * mcc, ULONG value)
{
	ASSERT( mcc->mcc_data_position + sizeof(value) <= mcc->mcc_data_size );

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
static void
mfm_encode_skip_encoded_words(struct mfm_code_context * mcc, int count)
{
	/* Note that mcc->mcc_previous_byte is not updated here.
	 * This is by design. mfm_encode_restore_data_position() will
	 * update mcc->mcc_previous_byte later.
	 */
	ASSERT( mcc->mcc_data_position + count * 2 * sizeof(ULONG) <= mcc->mcc_data_size );

	if(mcc->mcc_data_position + count * 2 * sizeof(ULONG) <= mcc->mcc_data_size)
		mcc->mcc_data_position += count * 2 * sizeof(ULONG);
}

/****************************************************************************/

/* MFM-encode half the bits of a single 32 bit word (either the odd
 * or the even ones).
 */
static void
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
	 * sequence are of the same value.
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
static void
mfm_encode_fix_clock_bit(struct mfm_code_context * mcc)
{
	ASSERT( mcc->mcc_data_position > 0 && mcc->mcc_data_position < mcc->mcc_data_size);

	if(mcc->mcc_data_position > 0)
	{
		UBYTE current_byte;

		/* Make sure that the clock bit is set correctly
		 * to follow the last bit stored.
		 */
		current_byte = mcc->mcc_data[mcc->mcc_data_position];
		if((current_byte & (1 << 6)) == 0)
		{
			/* Is the previous bit a zero? */
			if((mcc->mcc_previous_byte & 1) == 0)
				current_byte |=  (1 << 7);
			else
				current_byte &= ~(1 << 7);

			mcc->mcc_data[mcc->mcc_data_position] = current_byte;
		}
	}
}

/****************************************************************************/

/* Encode a single 32 bit word, storing the result in the buffer. The buffer
 * pointer will be updated accordingly.
 */
static void
mfm_encode_word(struct mfm_code_context * mcc, ULONG data)
{
	/* First the even bits, then the odd bits. */
	mfm_encode_half_the_bits(mcc, data >> 1);
	mfm_encode_half_the_bits(mcc, data);
}

/****************************************************************************/

/* Calculate the checksum for the sector buffer after it has been encoded. */
static ULONG
mfm_calculate_buffer_checksum(
	const struct mfm_code_context *	mcc,
	size_t							start_position,
	size_t							stop_position)
{
	ULONG sum = 0;

	ASSERT( start_position <= stop_position );
	ASSERT( start_position < mcc->mcc_sector_size );
	ASSERT( stop_position <= mcc->mcc_sector_size );

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
	const void *				_sector_data)
{
	const ULONG * sector_data = _sector_data;
	ULONG null_pattern;
	ULONG checksum;
	size_t i;

	ASSERT( track < 180 );
	ASSERT( sector < 22 );
	ASSERT( 1 <= sector_offset && sector_offset <= 22 );

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
	 * gap arrives.
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

	/* Proceed to encode the sector data. The odd bits are encoded first. */
	for(i = 0 ; i < (int)(512 / sizeof(ULONG)) ; i++)
		mfm_encode_half_the_bits(mcc, sector_data[i] >> 1);

	/* And encode the even bits which we left out before. */
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

/* Bit-rotate the entire track data to the right by 0..31 bits, simulating
 * the effect of the disk read head starting at a random bit position
 * in the recorded data stream.
 */
void
mfm_encode_rotate_data(struct mfm_code_context * mcc, int offset)
{
	offset %= 32;

	/* Any rotation necessary at all? */
	if(offset > 0)
	{
		ULONG * data = (ULONG *)mcc->mcc_data;
		int num_words = (mcc->mcc_data_size + mcc->mcc_sector_gap_size) / sizeof(*data);
		ULONG first, previous, shifted, d;
		int i;

		/* We'll adjust this later to make the rotation complete. */
		first = data[0];

		/* Remember which bits from the first word
		 * need to be copied into the second word,
		 * and so forth...
		 */
		previous = first << (32 - offset);

		for(i = 1 ; i < num_words ; i++)
		{
			d = data[i];

			/* Combine the bits left over from the
			 * previous word and the current word.
			 */
			shifted = previous | (d >> offset);

			/* These are the bits which need to be
			 * preserved so that they can be combined
			 * with the next following word.
			 */
			previous = d << (32 - offset);

			/* Store the combined word. */
			data[i] = shifted;
		}

		/* And this makes the rotation complete by
		 * combining the first word, with the
		 * leftover bits from the last word.
		 */
		data[0] = previous | (first >> offset);
	}
}

/****************************************************************************/

#endif /* ENABLE_MFM_ENCODING */
