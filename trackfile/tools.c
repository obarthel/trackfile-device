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

#ifndef _TRACKFILE_DEVICE_H
#include "trackfile_device.h"
#endif /* _TRACKFILE_DEVICE_H */

/****************************************************************************/

#include "tools.h"

/****************************************************************************/

#include "assert.h"

/****************************************************************************/

/* Calculates the 64 bit checksum for a series of 32 bit words.
 *
 * The basic workings of this algorithm come from: J.G. Fletcher. An arithmetic
 * checksum for serial transmission; IEEE Transactions on Communications,
 * January 1982.
 */
struct fletcher64_checksum *
fletcher64_checksum(const APTR data, size_t size, struct fletcher64_checksum * checksum)
{
	const ULONG * block = (ULONG *)data;
	size_t count = size / sizeof(*block);
	ULONG sum1 = 0, sum2 = 0;

	ASSERT( size == 0 || ((size % sizeof(*block)) == 0 && data != NULL) );
	ASSERT( checksum != NULL );

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

	return(checksum);
}

/****************************************************************************/

/* Compare two Fletcher 64 checksums. Returns 0 if identical, otherwise
 * not identical.
 */
int
compare_fletcher64_checksums(
	const struct fletcher64_checksum * checksum1,
	const struct fletcher64_checksum * checksum2)
{
	int result;

	ASSERT( checksum1 != NULL && checksum2 != NULL );

	result = (checksum1->f64c_low == checksum2->f64c_low && checksum1->f64c_high == checksum2->f64c_high) ? 0 : 1;

	return(result);
}

/****************************************************************************/

/* Initialize a MsgPort to use signal notification for a specific
 * Task or Process, with a specific signal bit to be used. Note
 * that the MsgPort.mp_Node contents will remain unchanged by
 * this function, so it's safe to use even if the MsgPort is part
 * of a List.
 */
void
init_msgport(struct MsgPort * mp, struct Task * signal_task, int signal_bit)
{
	ASSERT( mp != NULL && signal_task != NULL && 0 <= signal_bit && signal_bit <= 31 );

	mp->mp_Flags	= PA_SIGNAL;
	mp->mp_SigBit	= signal_bit;
	mp->mp_SigTask	= signal_task;

	NewList(&mp->mp_MsgList);
}

/****************************************************************************/

/* This is used by local_snprintf() below. */
struct format_context
{
	STRPTR	fc_buffer;	/* where to store the next character */
	ULONG	fc_size;	/* number of bytes left in the buffer */
	ULONG	fc_len;		/* length of string produced so far */
};

/****************************************************************************/

/* This is used by the local_vsnprintf() function below, and specifically
 * the exec.library/RawDoFmt() function it uses.
 */
static void ASM
stuff_char(REG(d0, UBYTE c), REG(a3, struct format_context *fc))
{
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

/* This is used by the local_snprintf() function below. */
static ULONG
local_vsnprintf(
	struct TrackFileDevice *	tfd,
	STRPTR						buffer,
	ULONG						buffer_size,
	const char *				format_string,
	va_list						args)
{
	USE_EXEC(tfd);

	struct format_context fc;
	ULONG len;

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

	return(len);
}

/****************************************************************************/

/* Length-limited version of sprintf() which guarantees not to overrun the
 * buffer and to always NUL-terminate the buffer contents.
 *
 * If the buffer size is smaller than 1, the buffer contents will not be
 * modified.
 *
 * Returns the length of the string produced so far, excluding the terminating
 * NUL character.
 *
 * Unlike the C99 snprintf() function you cannot detect buffer overflows just
 * by looking at the result and comparing it against the buffer size.
 */
ULONG
local_snprintf(
	struct TrackFileDevice *	tfd,
	STRPTR						buffer,
	ULONG						buffer_size,
	const char *				format_string,
								...)
{
	va_list args;
	ULONG len;

	ASSERT( tfd != NULL && buffer != NULL && format_string != NULL );

	va_start(args, format_string);
	len = local_vsnprintf(tfd, buffer, buffer_size, format_string, args);
	va_end(args);

	return(len);
}

/****************************************************************************/

/* Check if the sum of two unsigned 32 bit integers will be larger than what
 * an unsigned 32 bit integer can hold.
 *
 * This algorithm comes from Henry S. Warren's book "Hacker's delight".
 */
BOOL
addition_overflows(ULONG x, ULONG y)
{
	BOOL overflow;
	ULONG z;

	z = (x & y) | ((x | y) & ~(x + y));

	overflow = ((LONG)z) < 0;

	return(overflow);
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
decode_file_sys_startup_msg(struct Library * SysBase, BPTR startup, struct fs_startup_msg * fsm)
{
	const struct FileSysStartupMsg * fssm;
	const TEXT * device_name;
	int device_name_len;
	const struct DosEnvec * de;
	BOOL success = FALSE;

	ENTER();

	ASSERT( SysBase != NULL );

	/* Is this a small integer? We play is safe by allowing "small"
	 * to be any negative number, and any non-negative number up
	 * to 1024. A BCPL pointer can never be mistaken for a negative
	 * number because its most significant two bits are always
	 * zero.
	 */
	if((LONG)startup <= 1024)
		goto out;

	/* If this is truly a 32 bit BCPL pointer, then the two
	 * most significant bits will be zero.
	 */
	if(NOT IS_VALID_BPTR_ADDRESS(startup))
		goto out;

	/* This should be a valid RAM address. */
	fssm = BADDR(startup);
	if(TypeOfMem(fssm) == 0)
		goto out;

	/* The device name should be a NUL-terminated
	 * BCPL string and it should reside in RAM.
	 */
	if(NOT IS_VALID_BPTR_ADDRESS(fssm->fssm_Device))
		goto out;

	device_name = BADDR(fssm->fssm_Device);
	if(device_name == NULL || TypeOfMem(device_name) == 0)
		goto out;

	/* The device name should be suitable for passing
	 * to OpenDevice() as is, so it should not be
	 * empty.
	 */
	device_name_len = (*device_name++);
	if(device_name_len == 0)
		goto out;

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
		goto out;

	de = BADDR(fssm->fssm_Environ);
	if(de == NULL || TypeOfMem(de) == 0)
		goto out;

	/* The environment table size should be > 0. The
	 * minimum actually is 11 which covers all fields
	 * up to and including 'de_NumBuffers'.
	 */
	if(de->de_TableSize < DE_NUMBUFFERS)
		goto out;

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

/* Release memory allocated by allocate_aligned_memory(). */
void
free_aligned_memory(struct TrackFileDevice * tfd, struct AlignedMemoryAllocation * ama)
{
	USE_EXEC(tfd);

	ENTER();

	ASSERT( tfd != NULL );

	if(ama != NULL)
	{
		if(ama->ama_Allocated != NULL)
			FreeVec(ama->ama_Allocated);

		ama->ama_Allocated	= NULL;
		ama->ama_Aligned	= NULL;
	}

	LEAVE();
}

/****************************************************************************/

/* Allocate memory best suited for a specific file system, and also
 * align it to an address renders it cache-friendly. Fills in the
 * 'struct AlignedMemoryAllocation' data pointed to by the 'ama'
 * parameter. Returns 0 for success or otherwise an error code.
 */
LONG
allocate_aligned_memory(
	struct TrackFileDevice *			tfd,
	struct MsgPort *					file_system,
	ULONG								size,
	struct AlignedMemoryAllocation *	ama)
{
	USE_EXEC(tfd);
	USE_DOS(tfd);

	ULONG buffer_memory_type = MEMF_ANY|MEMF_PUBLIC;
	LONG error = ERROR_NO_FREE_STORE;
	struct DosList * dol;

	ENTER();

	ASSERT( FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS );

	ASSERT( tfd != NULL && file_system != NULL && size > 0 && ama != NULL );

	memset(ama, 0, sizeof(*ama));

	/* Try to find the file system device associated with the
	 * file system process. If that file system turns out to
	 * offer information on which memory allocation type is
	 * best suited for it, remember these settings.
	 */
	for(dol = NextDosEntry(LockDosList(LDF_DEVICES|LDF_READ), LDF_DEVICES) ;
	    dol != NULL ;
	    dol = NextDosEntry(dol, LDF_DEVICES))
	{
		if(dol->dol_Task == file_system)
		{
			struct fs_startup_msg fsm;

			SHOWMSG("found the file system device");

			if(decode_file_sys_startup_msg(SysBase, dol->dol_misc.dol_handler.dol_Startup, &fsm))
			{
				if(fsm.fsm_environment->de_TableSize >= DE_BUFMEMTYPE)
				{
					D(("will use allocation flags 0x%08lx (previously: 0x%08lx)",
						fsm.fsm_environment->de_BufMemType, buffer_memory_type));

					buffer_memory_type = fsm.fsm_environment->de_BufMemType;
				}
			}

			break;
		}
	}

	UnLockDosList(LDF_DEVICES|LDF_READ);

	ASSERT( NOT addition_overflows(size, 15) );

	/* We will try to align the allocation to a cache line
	 * size, as used by the MC68040. That's 16 bytes or 128 bits
	 * in the new money.
	 */
	ama->ama_Allocated = AllocVec(size + 15, buffer_memory_type | MEMF_PUBLIC);
	if(ama->ama_Allocated == NULL)
		goto out;

	ama->ama_Aligned = (APTR)( (((ULONG)ama->ama_Allocated) + 15) & ~15UL );

	error = OK;

 out:

	RETURN(error);
	return(error);
}

/****************************************************************************/

/* Calculate the checksum for an Amiga file system block. Assumes that the
 * block size is already known.
 */
LONG
calculate_amiga_block_checksum(const void * block_data, int length)
{
	const LONG * data = block_data;
	int longs_per_block = length / sizeof(*data);
	LONG sum = 0;

	while(longs_per_block >= 8)
	{
		sum += (*data++);
		sum += (*data++);
		sum += (*data++);
		sum += (*data++);
		sum += (*data++);
		sum += (*data++);
		sum += (*data++);
		sum += (*data++);

		longs_per_block -= 8;
	}

	while(longs_per_block-- > 0)
		sum += (*data++);

	return(sum);
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

/* Examine the contents of what is a 512 byte root directory block or maybe
 * not. We perform all the consistency checks which the Amiga default file
 * system uses (primary and secondary block types, own key, block count, hash
 * table size and parent directory block number tested) and also verify that
 * the volume name is sound.
 */
BOOL
root_directory_is_valid(const struct RootDirBlock * rdb)
{
	BOOL is_inconsistent = FALSE;
	BOOL is_valid = FALSE;
	size_t len, i;
	TEXT c;

	/* Is the checksum of the block contents unsound? */
	if(calculate_amiga_block_checksum(rdb, TD_SECTOR) != 0)
	{
		SHOWMSG("root block checksum appears to be invalid");
		goto out;
	}

	/* Is this not a root directory? */
	if(rdb->rdb_PrimaryType != T_SHORT || rdb->rdb_SecondaryType != ST_ROOT)
	{
		D(("root block type information does not seem to match (primary=%ld, secondary=%ld)",
			rdb->rdb_PrimaryType, rdb->rdb_SecondaryType));

		goto out;
	}

	len = rdb->rdb_Name[0];

	/* Is the length of the name sound? */
	if (len == 0)
	{
		SHOWMSG("root directory name is empty");

		is_inconsistent = TRUE;
	}
	else if (len >= sizeof(rdb->rdb_Name))
	{
		D(("root directory name claims to be %ld characters long, %ld are the maximum",
			len, sizeof(rdb->rdb_Name)-1));

		is_inconsistent = TRUE;
	}

	/* Does the name contain characters which are not
	 * permitted for a volume name?
	 */
	for(i = 0 ; i < len && i < sizeof(rdb->rdb_Name)-1 ; i++)
	{
		c = rdb->rdb_Name[i+1];
		if(c == ':' || c == '/' || (c < ' ' && c != '\t') || (128 <= c && c < 160))
		{
			D(("volume name contains invalid characters, e.g. %ld (%lc)", c, c));

			is_inconsistent = TRUE;
			break;
		}
	}

	/* A file, link or user directory header contains its
	 * own block number. For the root directory it must
	 * be zero.
	 */
	if(rdb->rdb_OwnKey != 0)
	{
		D(("root directory 'own key' should be zero, but is 0x%08lx", rdb->rdb_OwnKey));

		is_inconsistent = TRUE;
	}

	/* A file or file extension header contains the number of
	 * entries which are used in the block table. For a root
	 * directory that count must be zero.
	 */
	if(rdb->rdb_BlockCount != 0)
	{
		D(("root directory 'block count' should be zero, but is 0x%08lx", rdb->rdb_BlockCount));

		is_inconsistent = TRUE;
	}

	/* A directory hash table should have at least 72
	 * entries.
	 */
	if(rdb->rdb_HashTableSize < 72)
	{
		D(("root directory hash table size should be 72 or higher, but is %lu", rdb->rdb_HashTableSize));

		is_inconsistent = TRUE;
	}

	/* The root directory has no parent directory. */
	if(rdb->rdb_Parent != 0)
	{
		D(("root directory 'parent directory key' should be zero, but is 0x%08lx", rdb->rdb_Parent));

		is_inconsistent = TRUE;
	}

	if(is_inconsistent)
		goto out;

	is_valid = TRUE;

 out:

	return(is_valid);
}

/****************************************************************************/

#if DEBUG

/****************************************************************************/

/* The check_stack_size_available() function needs to be able to access
 * the current stack pointer address in a sort of portable manner
 * through the get_sp() macro.
 */
#if defined(__SASC)

#include <dos.h>

#define get_sp() ((BYTE *)getreg(REG_A7))

#else

#define get_sp() ((BYTE *)0)

#endif /* __SASC */

/****************************************************************************/

void
check_stack_size_available(struct Library * SysBase)
{
	if((GetCC() & 0x2000) != 0)
	{
		SHOWMSG("Warning: called from interrupt/exception state!");
	}
	else
	{
		const struct Task * tc = FindTask(NULL);
		const BYTE * lower = (BYTE *)tc->tc_SPLower;
		const BYTE * upper = (BYTE *)tc->tc_SPUpper;
		const BYTE * current_sp = get_sp();

		if (lower <= current_sp && current_sp < upper)
		{
			LONG size = upper - lower;
			LONG available = current_sp - lower;

			if(size < 500)
				D(("Warning: almost out of stack space -- stack size available = %ld bytes (of %ld bytes)", available, size));
			else
				D(("stack size available = %ld bytes (of %ld bytes)", available, size));
		}
		else if (current_sp < lower && current_sp < upper)
		{
			D(("Warning: stack overflow of %ld bytes! (or stack is out of bounds)", lower - current_sp));
		}
		else if (lower <= current_sp && upper <= current_sp)
		{
			D(("Warning: stack underflow of %ld bytes! (or stack is out of bounds)", current_sp - (upper - sizeof(LONG))));
		}
	}
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
BOOL
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

/* Examine all the nodes in a list and try to find a specific node. If that
 * node is part of the list return TRUE, otherwise return FALSE.
 */
BOOL
node_is_in_list(const struct List * list, const struct Node * node)
{
	BOOL result = FALSE;
	struct Node * ln;

	ASSERT( list != NULL );

	for(ln = list->lh_Head ; ln->ln_Succ != NULL ; ln = ln->ln_Succ)
	{
		if(ln == node)
		{
			result = TRUE;
			break;
		}
	}

	return(result);
}

/****************************************************************************/

/*
BOOL
IsMinListEmpty(const struct MinList * list)
{
	BOOL is_empty;

	is_empty = (BOOL)(list->mlh_TailPred == (struct MinNode *)list);

	return(is_empty);
}

VOID
NewMinList(struct MinList * list)
{
	list->mlh_Tail		= NULL;
	list->mlh_Head		= (struct MinNode *)&list->mlh_Tail;
	list->mlh_TailPred	= (struct MinNode *)list;
}

VOID
InsertMinNode(struct MinList *list, struct MinNode *node, struct MinNode *list_node)
{
	if (list_node == NULL)
	{
		AddHeadMinList(list, node);
	}
	else if (node->mln_Succ == NULL)
	{
		struct MinNode * pred;

		list_node->mln_Succ	= node;
		pred				= node->mln_Pred;
		list_node->mln_Pred	= pred;
		node->mln_Pred		= list_node;
		pred->mln_Succ		= list_node;
	}
	else
	{
		struct MinNode * succ;

		succ				= node->mln_Succ;
		list_node->mln_Succ	= succ;
		list_node->mln_Pred	= node;
		succ->mln_Pred		= list_node;
		node->mln_Succ		= list_node;
	}
}

VOID
AddHeadMinList(struct MinList *list, struct MinNode *node)
{
	struct MinNode * head;

	head			= list->mlh_Head;
	list->mlh_Head	= node;
	node->mln_Succ	= head;
	node->mln_Pred	= (struct MinNode *)list;
	head->mln_Pred	= node;
}

VOID
AddTailMinList(struct MinList *list, struct MinNode *node)
{
	struct MinNode * tail;
	struct MinNode * tail_pred;

	tail				= (struct MinNode *)&list->mlh_Tail;
	tail_pred			= tail->mln_Pred;
	tail->mln_Pred		= node;
	node->mln_Succ		= tail;
	node->mln_Pred		= tail_pred;
	tail_pred->mln_Succ	= node;
}

VOID
RemoveMinNode(struct MinNode *node)
{
	struct MinNode * next;
	struct MinNode * pred;

	next			= node->mln_Succ;
	pred			= node->mln_Pred;
	pred->mln_Succ	= next;
	next->mln_Pred	= pred;
}

struct MinNode *
RemHeadMinList(struct MinList *list)
{
	struct MinNode * result;

	if(!IsMinListEmpty(list))
	{
		result = list->mlh_Head;

		RemoveMinNode(result);
	}
	else
	{
		result = NULL;
	}

	return(result);
}

struct MinNode *
RemTailMinList(struct MinList *list)
{
	struct MinNode * result;

	if(!IsMinListEmpty(list))
	{
		result = list->mlh_TailPred;

		RemoveMinNode(result);
	}
	else
	{
		result = NULL;
	}

	return(result);
}
*/

/****************************************************************************/

#endif /* DEBUG */

/****************************************************************************/
