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

#ifndef _TOOLS_H
#define _TOOLS_H

/****************************************************************************/

#ifndef _TRACKFILE_DEVICE_H
struct TrackFileDevice;
struct RootDirBlock;
#endif /* _TRACKFILE_DEVICE_H */

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

/* A 'C' version of the 'struct FileSysStartupMsg' from <dos/filehandler.h>. */
struct fs_startup_msg
{
	const TEXT *			fsm_device_name;	/* exec device name */
	ULONG					fsm_device_unit;	/* exec unit number of this device */
	ULONG					fsm_device_flags;	/* flags for OpenDevice() */
	const struct DosEnvec *	fsm_environment;	/* pointer to environment table */
};

/****************************************************************************/

/* Handle for a memory allocation aligned to a specific address
 * boundary to be be more cache-friendly.
 */
struct AlignedMemoryAllocation
{
	APTR ama_Allocated;	/* This is what was allocated */
	APTR ama_Aligned;	/* And this is the aligned version */
};

/****************************************************************************/

struct fletcher64_checksum * fletcher64_checksum(APTR data, size_t size, struct fletcher64_checksum * checksum);
int compare_fletcher64_checksums(const struct fletcher64_checksum * checksum1, const struct fletcher64_checksum * checksum2);
void init_msgport(struct MsgPort *mp, struct Task *signal_task, int signal_bit);
BOOL addition_overflows(ULONG x, ULONG y);
ULONG local_snprintf(struct TrackFileDevice *tfd, STRPTR buffer, ULONG buffer_size, const char *format_string, ...);
BOOL decode_file_sys_startup_msg(struct Library * SysBase, BPTR startup, struct fs_startup_msg * fsm);
void free_aligned_memory(struct TrackFileDevice * tfd, struct AlignedMemoryAllocation * ama);
LONG allocate_aligned_memory(struct TrackFileDevice * tfd, struct MsgPort * file_system, ULONG size, struct AlignedMemoryAllocation * ama);
LONG calculate_amiga_block_checksum(const void * block_data, int length);
ULONG calculate_boot_block_checksum(const ULONG * data, int length);
BOOL root_directory_is_valid(const struct RootDirBlock * rdb);

/****************************************************************************/

void check_stack_size_available(struct Library * SysBase);
BOOL multiplication_overflows(ULONG x, ULONG y);
BOOL node_is_in_list(const struct List * list, const struct Node * node);

/****************************************************************************/

#if defined(IsMinListEmpty)
#undef IsMinListEmpty
#endif /* IsMinListEmpty */

BOOL IsMinListEmpty(const struct MinList * list);

/****************************************************************************/

VOID NewMinList(struct MinList * list);
VOID InsertMinNode(struct MinList *list, struct MinNode *node, struct MinNode *list_node);
VOID AddHeadMinList(struct MinList *list, struct MinNode *node);
VOID AddTailMinList(struct MinList *list, struct MinNode *node);
VOID RemoveMinNode(struct MinNode *node);
struct MinNode * RemHeadMinList(struct MinList *list);
struct MinNode * RemTailMinList(struct MinList *list);

/****************************************************************************/

#define IsMinListEmpty(list) IsListEmpty((struct List *)(list))

#define NewMinList(list) NewList((struct List *)(list))

#define InsertMinNode(list, node, list_node) Insert((struct List *)(list), (struct Node *)(node), (struct Node *)(list_node))

#define AddHeadMinList(list, node) AddHead((struct List *)(list), (struct Node *)(node))

#define AddTailMinList(list, node) AddTail((struct List *)(list), (struct Node *)(node))

#define RemoveMinNode(node) Remove((struct Node *)(node))

#define RemHeadMinList(list) ((struct MinNode *)RemHead((struct List *)(list)))

#define RemTailMinList(list) ((struct MinNode *)RemTail((struct List *)(list)))

/****************************************************************************/

#endif /* _TOOLS_H */
