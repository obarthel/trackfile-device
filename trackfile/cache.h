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

#ifndef _CACHE_H
#define _CACHE_H

/****************************************************************************/

/* This enables the use of the global and unit caches for experimentation. */
#define ENABLE_CACHE

/****************************************************************************/

#if defined(ENABLE_CACHE)

/****************************************************************************/

#ifndef _UNIT_H
#include "unit.h"
#endif /* _UNIT_H */

/****************************************************************************/

/* Combine unit number, track number (0..159: 8 bits) and a possible way to
 * support high density disks by allocating two cache entries per track for
 * which a single bit is reserved.
 *
 * This leaves 32 - (8 + 1) = 23 bits, allowing for only up to a meagre
 * 8,388,608 units to be used at a time.
 */
#define CACHE_KEY(unit_number, track_number) \
	(((unit_number) << 9) | ((track_number) << 1))

#define CACHE_KEY_UNIT_MASK ((~0UL << 9) & 0xFFFFFFFFUL)

/****************************************************************************/

/* A combination of a balanced binary tree with a doubly-linked list.
 * This is used to manage the cache LRU scheme.
 */
struct SplayNode
{
	struct MinNode		sn_Node;		/* This goes into the list */
	struct SplayNode *	sn_Left;		/* Left child */
	struct SplayNode *	sn_Right;		/* Right child */
	ULONG				sn_Key;			/* Unique identifier */
};

/* The root of the splay tree. */
struct SplayTree
{
	struct SplayNode *	st_Root;		/* The tree root node */
	struct MinList		st_List;		/* All nodes are stored here, too */
};

/****************************************************************************/

/* A single cache node which also contains size and checksum information for
 * the data. The data directly follows the CacheNode structure.
 */
struct CacheNode
{
	struct SplayNode	cn_SplayNode;	/* This is part of the splay tree */
	struct MinNode		cn_UnitNode;	/* This is associated with the unit which uses the cache node */
	ULONG				cn_Checksum;	/* Checksum for the data which follows the CacheNode */
};

/****************************************************************************/

struct CacheContext
{
	struct TrackFileDevice *		cc_TrackFileBase;		/* Very handy... */

	struct SignalSemaphore			cc_Lock;				/* Arbitration for accessing this data structure. */
	UWORD							cc_Pad1;

	ULONG							cc_DataSize;			/* How much memory is spent for each cache payload */

	ULONG							cc_MaxCacheSize;		/* Maximum amount of memory to spend on caching */
	ULONG							cc_NumBytesAllocated;	/* Total number of bytes allocated for cache nodes */

	struct SplayTree				cc_ProtectedCacheTree;	/* Protected segment of the LRU scheme */
	struct SplayTree				cc_ProbationCacheTree;	/* Probationary segment of the LRU scheme */

	struct MinList					cc_SpareList;			/* Unused cache nodes go here. */

	ULONG							cc_ProtectedCacheMax;	/* How many nodes may be in the protected section? */
	ULONG							cc_ProtectedCacheSize;	/* How many nodes are currently in the protected section? */

	struct StackSwapStruct *		cc_StackSwap;			/* Used by the exec.library memory handler */
	const struct MemHandlerData *	cc_MemHandlerData;		/* Passed to the memory cleanup function */
	struct Interrupt				cc_MemHandler;			/* Called by exec when memory becomes tight */
};

/****************************************************************************/

/* This is used by update_cache_contents() and indicates whether
 * updating the cache is restricted to existing entries only (UDN_UpdateOnly)
 * or whether the data in the cache should always be updated by either
 * allocating a new cache entry or reusing an existing one.
 */
enum UDN_Mode
{
	UDN_Allocate,
	UDN_UpdateOnly
};

/****************************************************************************/

extern BOOL read_cache_contents(struct CacheContext *cc, struct TrackFileUnit *	tfu, LONG track_number, void *data, ULONG data_size);
extern void invalidate_cache_entries_for_unit(struct CacheContext * cc, struct TrackFileUnit * tfu);
extern void invalidate_cache_entry(struct CacheContext * cc, ULONG key);
extern void update_cache_contents(struct CacheContext *cc, struct TrackFileUnit * tfu, LONG track_number, const void * data, ULONG data_size, enum UDN_Mode mode);
extern void change_cache_size(struct CacheContext *cc, ULONG max_cache_size);
extern void delete_cache_context(struct CacheContext * cc);
extern struct CacheContext * create_cache_context(struct TrackFileDevice * tfd, ULONG data_size);

/****************************************************************************/

#endif /* ENABLE_CACHE */

/****************************************************************************/

#endif /* _CACHE_H */
