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

#include "unit.h"

/****************************************************************************/

#include "cache.h"

/****************************************************************************/

#if defined(ENABLE_CACHE)

/****************************************************************************/

#include "swap_stack.h"

/****************************************************************************/

#include <stddef.h>
#include <string.h>

/****************************************************************************/

#include "assert.h"

/****************************************************************************/

/* The splay tree code comes from Daniel D. Sleator's anonymous FTP directory
 * at https://www.link.cs.cmu.edu/link/ftp-site, originally from 1995, and
 * was adapted for use on the Amiga. The original splay tree implementation
 * can be found here:
 *
 *     https://www.link.cs.cmu.edu/link/ftp-site/splaying/top-down-splay.c
 *
 * Splay trees are described in the paper "Self-adjusting binary search trees"
 * by Daniel Dominic Sleator and Robert Endre Tarjan, as published in the
 * Journal of the Association for Computing Machinery, Vol. 32, No. 3,
 * July 1985.
 */

/* Simple top down splay, not requiring the key to be in the tree. */
static struct SplayNode *
splay(struct SplayNode * t, ULONG key)
{
	struct SplayNode N, *l, *r, *y;

	ASSERT( t != NULL );

	memset(&N, 0, sizeof(N));

	l = r = &N;

	while(key != t->sn_Key)
	{
		if(key < t->sn_Key)
		{
			if(t->sn_Left == NULL)
				break;

			if(key < t->sn_Left->sn_Key)
			{
				/* rotate right */
				y = t->sn_Left;
				t->sn_Left = y->sn_Right;
				y->sn_Right = t;
				t = y;

				if(t->sn_Left == NULL)
					break;
			}

			/* link right */
			r->sn_Left = t;
			r = t;
			t = t->sn_Left;
		}
		else
		{
			if(t->sn_Right == NULL)
				break;

			if(key > t->sn_Right->sn_Key)
			{
				/* rotate left */
				y = t->sn_Right;
				t->sn_Right = y->sn_Left;
				y->sn_Left = t;
				t = y;

				if(t->sn_Right == NULL)
					break;
			}

			/* link left */
			l->sn_Right = t;
			l = t;
			t = t->sn_Right;
		}
	}

	/* assemble */
	l->sn_Right	= t->sn_Left;
	r->sn_Left	= t->sn_Right;
	t->sn_Left	= N.sn_Right;
	t->sn_Right	= N.sn_Left;

	return(t);
}

/****************************************************************************/

/* Insert a node into the splay tree, unless there is already a node present
 * in it which shares the same key value. Returns TRUE if the new node could
 * be inserted and FALSE if there was already a node in the tree with the
 * same key value as used by the new node.
 */
static BOOL
insert_splay_node_into_tree(struct SplayTree * tree, struct SplayNode * new)
{
	BOOL success = TRUE;

	ASSERT( tree != NULL && new != NULL );

	if(tree->st_Root != NULL)
	{
		struct SplayNode * t;

		t = splay(tree->st_Root, new->sn_Key);

		/* Insert the new node in the right place. */
		if (new->sn_Key < t->sn_Key)
		{
			new->sn_Left	= t->sn_Left;
			new->sn_Right	= t;

			t->sn_Left = NULL;
		}
		else if (new->sn_Key > t->sn_Key)
		{
			new->sn_Right	= t->sn_Right;
			new->sn_Left	= t;

			t->sn_Right = NULL;
		}
		/* Oops! We do not support duplicates in the tree. */
		else
		{
			/* Splaying the tree changed the root, which we
			 * will have to update, or even attempting to
			 * add a duplicate key will destroy the tree.
			 */
			tree->st_Root = t;

			success = FALSE;

			D(("key 0x%08lx already present in tree", new->sn_Key));
		}
	}
	else
	{
		new->sn_Left = new->sn_Right = NULL;
	}

	if(success)
		tree->st_Root = new;

	return(success);
}

/****************************************************************************/

/* Removes a node from the tree if it's there. Returns a pointer to the node
 * if it is found, NULL otherwise. Note that even a failed attempt to find a
 * node for the given key will change the tree!
 */
static struct SplayNode *
remove_node_from_splay_tree(struct SplayTree * tree, ULONG key)
{
	struct SplayNode * result = NULL;

	ASSERT( tree != NULL );

	if(tree->st_Root != NULL)
	{
		struct SplayNode * t;

		t = splay(tree->st_Root, key);

		if(t->sn_Key == key)
		{
			struct SplayNode * x;

			result = t;

			if(t->sn_Left == NULL)
			{
				x = t->sn_Right;
			}
			else
			{
				x = splay(t->sn_Left, key);

				x->sn_Right = t->sn_Right;
			}

			t = x;
		}

		tree->st_Root = t;
	}

	return(result);
}

/****************************************************************************/

/* Attempt to find a node in the splay tree whose key matches the one given.
 * Will return a pointer to the splay node if found and otherwise returns
 * NULL. The layout of the tree will not be changed even if the node is not
 * found. We just perform a "normal" binary tree traversal here.
 */
static struct SplayNode *
find_splay_node(const struct SplayTree * tree, ULONG key)
{
	struct SplayNode * found = NULL;
	const struct SplayNode * t;

	ASSERT( tree != NULL );

	t = tree->st_Root;
	while(t != NULL)
	{
		if (key < t->sn_Key)
		{
			/* Key is smaller than the current node; move left
			 * to a smaller node value.
			 */
			t = t->sn_Left;
		}
		else if (key > t->sn_Key)
		{
			/* Key is greater than the current node; move right
			 * to a greater node value.
			 */
			t = t->sn_Right;
		}
		else
		{
			/* Found the key. */
			found = (struct SplayNode *)t;
			break;
		}
	}

	return(found);
}

/****************************************************************************/

/* Try to find a key in the tree, then splay the tree if a matching node is
 * found. Returns a pointer to the splay node found or NULL if no such splay
 * node could be found. If no splay node could be found then the splay tree
 * will remain unchanged.
 */
static struct SplayNode *
find_node_and_splay_tree(struct SplayTree * tree, ULONG key)
{
	struct SplayNode * found;

	ASSERT( tree != NULL );

	found = find_splay_node(tree, key);
	if(found != NULL && found != tree->st_Root)
		tree->st_Root = splay(tree->st_Root, key);

	return(found);
}

/****************************************************************************/

/* Initialize a splay tree to be empty. Very simple in operation. */
static void
initialize_splay_tree(struct SplayTree *tree)
{
	ENTER();

	ASSERT( tree != NULL );

	tree->st_Root = NULL;

	NewMinList(&tree->st_List);

	LEAVE();
}

/****************************************************************************/

/* The number of protected segment entries is limited. As more entries are
 * moved from the probationary segment into the protected segment over time,
 * they may have to be moved into the probationary segment again.
 *
 * This function removes the least recently-used entries from the probationary
 * segment and adds them to the beginning of the probationary segment in exactly
 * the same order as they were in the probationary segment. Only as many entries
 * will be moved as are needed to bring the size of the probationary segment
 * back to its maximum size.
 */
static void
adjust_protected_cache_size(struct CacheContext * cc)
{
	USE_EXEC(cc->cc_TrackFileBase);

	struct CacheNode * cn;
	struct CacheNode * cn_removed;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	ASSERT( cc != NULL) ;

	if(cc->cc_ProtectedCacheSize > cc->cc_ProtectedCacheMax)
	{
		D(("protected segment now contains %lu entries; %lu entries need to be moved to probationary segment",
			cc->cc_ProtectedCacheSize, cc->cc_ProtectedCacheSize - cc->cc_ProtectedCacheMax));
	}

	while(cc->cc_ProtectedCacheSize > cc->cc_ProtectedCacheMax &&
		(cn = (struct CacheNode *)RemTailMinList(&cc->cc_ProtectedCacheTree.st_List)) != NULL)
	{
		cn_removed = (struct CacheNode *)remove_node_from_splay_tree(&cc->cc_ProtectedCacheTree, cn->cn_SplayNode.sn_Key);

		ASSERT( cn_removed == cn && cn_removed != NULL && "THIS SHOULD NEVER HAPPEN" );

		cc->cc_ProtectedCacheSize--;

		if(insert_splay_node_into_tree(&cc->cc_ProbationCacheTree, &cn->cn_SplayNode))
		{
			AddHeadMinList(&cc->cc_ProbationCacheTree.st_List, &cn->cn_SplayNode.sn_Node);
		}
		else
		{
			SHOWMSG("THIS SHOULD NEVER HAPPEN: Found duplicate in protected cache tree");

			RemoveMinNode(&cn->cn_UnitNode);

			AddHeadMinList(&cc->cc_SpareList, &cn->cn_SplayNode.sn_Node);
		}
	}

	LEAVE();
}

/****************************************************************************/

/* Calculate a checksum for the given data, very much like the Amiga
 * "Install" shell command does for the disk boot block.
 */
static ULONG
calculate_cache_data_checksum(const void * _data, ULONG num_bytes)
{
	const ULONG * data = _data;
	ULONG next_sum;
	ULONG sum;
	size_t num_longs;

	sum = 0;

	num_longs = num_bytes / sizeof(*data);

	while(num_longs-- > 0)
	{
		next_sum = sum + (*data++);
		if(next_sum < sum)
			next_sum++;

		sum = next_sum;
	}

	return(sum);
}

/****************************************************************************/

/* Try to find data corresponding to the given key in the cache. If found,
 * copies it to the client-supplied buffer and returns TRUE, otherwise
 * nothing is copied and FALSE is returned. Accessing the cache will likely
 * update its state.
 */
BOOL
read_cache_contents(
	struct CacheContext *	cc,
	struct TrackFileUnit *	tfu,
	LONG					track_number,
	void *					data,
	ULONG					data_size)
{
	USE_EXEC(cc->cc_TrackFileBase);

	BOOL success = FALSE;

	ENTER();

	ASSERT( cc != NULL );
	ASSERT( tfu != NULL );
	ASSERT( 0 <= track_number && track_number < tfu->tfu_NumTracks );

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	ObtainSemaphore(&cc->cc_Lock);

	D(("cache read unit %ld/track %ld: data = 0x%08lx, data_size = %ld",
		tfu->tfu_UnitNumber, track_number, data, data_size));

	if(data_size == cc->cc_DataSize)
	{
		ULONG key = CACHE_KEY(tfu->tfu_UnitNumber, track_number);
		struct CacheNode * cn;

		/* We try to find an existing cache node with the same
		 * key in use in the protected and probationary cache
		 * segments.
		 */
		cn = (struct CacheNode *)find_node_and_splay_tree(&cc->cc_ProtectedCacheTree, key);
		if(cn != NULL)
		{
			/* Seems we got lucky. Move up the cache node to the
			 * beginning of the list to reflect that it has
			 * been reused more frequently than other nodes.
			 */
			if(cc->cc_ProtectedCacheTree.st_List.mlh_Head != &cn->cn_SplayNode.sn_Node)
			{
				RemoveMinNode(&cn->cn_SplayNode.sn_Node);

				AddHeadMinList(&cc->cc_ProtectedCacheTree.st_List, &cn->cn_SplayNode.sn_Node);
			}
		}
		else
		{
			/* If we can find the node in the probationary segment, it will
			 * be promoted to the protected segment.
			 */
			cn = (struct CacheNode *)remove_node_from_splay_tree(&cc->cc_ProbationCacheTree, key);
			if(cn != NULL)
			{
				RemoveMinNode(&cn->cn_SplayNode.sn_Node);

				if(insert_splay_node_into_tree(&cc->cc_ProtectedCacheTree, &cn->cn_SplayNode))
				{
					cc->cc_ProtectedCacheSize++;

					AddHeadMinList(&cc->cc_ProtectedCacheTree.st_List, &cn->cn_SplayNode.sn_Node);

					/* If there are now more entries in the protected segment
					 * than there should be, move the least frequently-used
					 * entries over to the beginning of the probationary segment.
					 */
					adjust_protected_cache_size(cc);
				}
				else
				{
					SHOWMSG("THIS SHOULD NEVER HAPPEN: Found duplicate in probation cache tree");

					RemoveMinNode(&cn->cn_UnitNode);

					AddHeadMinList(&cc->cc_SpareList, &cn->cn_SplayNode.sn_Node);
					cn = NULL;
				}
			}
		}

		/* If we found the cache node and the data checksum matches,
		 * copy its contents into the client's buffer.
		 */
		if(cn != NULL)
		{
			ULONG checksum;

			checksum = calculate_cache_data_checksum(&cn[1], cc->cc_DataSize);
			if(checksum == cn->cn_Checksum)
			{
				CopyMem(&cn[1], data, cc->cc_DataSize);

				success = TRUE;
			}
			else
			{
				D(("checksum mismatch for key 0x%08lx: got 0x%08lx, expected 0x%08lx",
					key, checksum, cn->cn_Checksum));

				invalidate_cache_entry(cc, key);
			}
		}
	}
	else
	{
		D(("data size mismatch: got %ld but expected %ld", data_size, cc->cc_DataSize));
	}

	ReleaseSemaphore(&cc->cc_Lock);

	RETURN(success);
	return(success);
}

/****************************************************************************/

/* Translate the address of the CacheNode->cn_UnitNode field into the
 * address of the CacheNode itself.
 */
static struct CacheNode *
cache_node_from_unit_node(const struct MinNode * mn)
{
	struct SplayNode * sn;

	sn = (struct SplayNode *)mn;

	ASSERT( offsetof(struct CacheNode, cn_UnitNode) == sizeof(struct SplayNode) );

	return((struct CacheNode *)&sn[-1]);
}

/****************************************************************************/

/* Invalidate all cache entries associated with a specific unit number, which
 * is needed when a disk image is ejected. We wouldn't want the old cache
 * entries for that disk to persist until it's reinserted, would we?
 */
void
invalidate_cache_entries_for_unit(struct CacheContext * cc, struct TrackFileUnit * tfu)
{
	USE_EXEC(cc->cc_TrackFileBase);

	ULONG num_entries_removed = 0;

	struct CacheNode * cn;
	struct CacheNode * cn_removed;
	struct MinNode * mn;

	ENTER();

	ASSERT( cc != NULL );
	ASSERT( tfu != NULL );

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	D(("invalidating cache entries for unit #%ld", tfu->tfu_UnitNumber));

	ObtainSemaphore(&cc->cc_Lock);

	/* All the cache nodes associated with this particular unit
	 * are stored in a list so that the invalidation could be
	 * made as fast as possible. The alternatives would have been
	 * to search for each key that belongs to a unit which
	 * may scale poorly...
	 */
	while((mn = RemHeadMinList(&tfu->tfu_CacheNodeList)) != NULL)
	{
		num_entries_removed++;

		/* This node is embedded in the CacheNode, following the
		 * SplayNode, which is why we need to translate the address
		 * back to the beginning of the CacheNode.
		 */
		cn = cache_node_from_unit_node(mn);

		/* That node may be in the probationary segment. */
		cn_removed = (struct CacheNode *)remove_node_from_splay_tree(&cc->cc_ProbationCacheTree, cn->cn_SplayNode.sn_Key);
		if(cn_removed == NULL)
		{
			/* If it's not there, it should be in the protected segment. */
			cn_removed = (struct CacheNode *)remove_node_from_splay_tree(&cc->cc_ProtectedCacheTree, cn->cn_SplayNode.sn_Key);
			if(cn_removed != NULL)
			{
				cc->cc_ProtectedCacheSize--;
			}
			else
			{
				D(("THIS SHOULD NEVER HAPPEN: CacheNode 0x%08lx is not stored in the splay tree!", cn));
				continue;
			}
		}

		ASSERT( cn == cn_removed && "THIS SHOULD NEVER HAPPEN" );

		ASSERT( node_is_in_list((struct List *)&cc->cc_ProbationCacheTree.st_List, (struct Node *)&cn->cn_SplayNode.sn_Node) ||
		        node_is_in_list((struct List *)&cc->cc_ProtectedCacheTree.st_List, (struct Node *)&cn->cn_SplayNode.sn_Node));

		RemoveMinNode(&cn->cn_SplayNode.sn_Node);

		ASSERT( NOT node_is_in_list((struct List *)&cc->cc_SpareList, (struct Node *)&cn->cn_SplayNode.sn_Node) );

		AddTailMinList(&cc->cc_SpareList, &cn->cn_SplayNode.sn_Node);
	}

	ReleaseSemaphore(&cc->cc_Lock);

	D(("%lu cache entries removed", num_entries_removed));

	LEAVE();
}

/****************************************************************************/

/* Invalidate a cache entry, such as may be necessary after a read error was
 * detected. The cache entry will be moved into the list of unused entries
 * to be reused later, perhaps.
 */
void
invalidate_cache_entry(struct CacheContext * cc, ULONG key)
{
	USE_EXEC(cc->cc_TrackFileBase);

	struct CacheNode * cn;

	ENTER();

	ASSERT( cc != NULL );

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	ObtainSemaphore(&cc->cc_Lock);

	/* Try to find a cache node in the probationary segment, and if
	 * that fails, try again with the protected segment.
	 * If the node is found in the protected segment, update the
	 * size of the protected segment, too!
	 */
	cn = (struct CacheNode *)remove_node_from_splay_tree(&cc->cc_ProbationCacheTree, key);
	if(cn == NULL)
	{
		cn = (struct CacheNode *)remove_node_from_splay_tree(&cc->cc_ProtectedCacheTree, key);
		if(cn != NULL)
			cc->cc_ProtectedCacheSize--;
	}

	/* If we found the cache node, move it over to the list of
	 * unused spares.
	 */
	if(cn != NULL)
	{
		RemoveMinNode(&cn->cn_UnitNode);

		RemoveMinNode(&cn->cn_SplayNode.sn_Node);
		AddTailMinList(&cc->cc_SpareList, &cn->cn_SplayNode.sn_Node);
	}

	ReleaseSemaphore(&cc->cc_Lock);

	LEAVE();
}

/****************************************************************************/

/* Try to update the cache, either by replacing data in an already  existing
 * cache node or by creating a new cache mode. Whether this function will
 * limit itself to updating existing cache nodes, or recycling existing ones,
 * is controlled through the mode parameter.
 *
 * mode == UDN_Allocate will try to allocate new nodes if none are available
 * for updating, and mode == UDN_UpdateOnly will update existing entries or
 * recycle entries only.
 */
void
update_cache_contents(
	struct CacheContext *	cc,
	struct TrackFileUnit *	tfu,
	LONG					track_number,
	const void *			data,
	ULONG					data_size,
	enum UDN_Mode			mode)
{
	USE_EXEC(cc->cc_TrackFileBase);

	struct CacheNode * cn;
	struct CacheNode * cn_removed;
	ULONG key;

	ENTER();

	ASSERT( cc != NULL );
	ASSERT( tfu != NULL );
	ASSERT( 0 <= track_number && track_number < tfu->tfu_NumTracks );

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	key = CACHE_KEY(tfu->tfu_UnitNumber, track_number);

	D(("update unit %ld/track %ld: data = 0x%08lx, data_size = %ld, mode = %s",
		tfu->tfu_UnitNumber, track_number, data, data_size, mode == UDN_Allocate ? "allocate" : "update only"));

	ObtainSemaphore(&cc->cc_Lock);

	if(data_size == cc->cc_DataSize)
	{
		/* We try to find an existing cache node with the same
		 * key in use in the probationary and protected cache
		 * segments first.
		 */
		cn = (struct CacheNode *)find_splay_node(&cc->cc_ProbationCacheTree, key);
		if(cn == NULL)
			cn = (struct CacheNode *)find_splay_node(&cc->cc_ProtectedCacheTree, key);
		else
			ASSERT( find_splay_node(&cc->cc_ProtectedCacheTree, key) == NULL && "THIS SHOULD NEVER HAPPEN" );

		/* If that didn't work, we may try to allocate memory
		 * for a new cache node or reuse an unused node instead.
		 */
		if(mode == UDN_Allocate && cn == NULL)
		{
			size_t allocation_size = sizeof(*cn) + cc->cc_DataSize;

			SHOWVALUE(allocation_size);

			/* Try to reuse an unused cache node first, and if
			 * that fails, allocate memory for a new node.
			 */
			cn = (struct CacheNode *)RemHeadMinList(&cc->cc_SpareList);
			if(cn == NULL)
			{
				D(("number of bytes allocated (%lu) + allocation size (%lu) > maximum (%lu)? %s",
					cc->cc_NumBytesAllocated,
					allocation_size,
					cc->cc_MaxCacheSize,
					cc->cc_NumBytesAllocated + allocation_size > cc->cc_MaxCacheSize ? "yes" : "no"));

				/* Is there still room for more nodes? */
				if(cc->cc_NumBytesAllocated + allocation_size < cc->cc_MaxCacheSize)
				{
					cn = AllocMem(allocation_size, MEMF_ANY);
					if(cn != NULL)
					{
						D(("0x%08lx = AllocMem(%lu, MEMF_ANY)", cn, allocation_size));

						cc->cc_NumBytesAllocated += allocation_size;
						if(cc->cc_NumBytesAllocated == cc->cc_MaxCacheSize)
						{
							D(("cache now contains %lu bytes and has reached its maximum size",
								cc->cc_NumBytesAllocated));
						}
						else
						{
							D(("cache now contains %lu bytes of %lu and is %lu%% full",
								cc->cc_NumBytesAllocated, cc->cc_MaxCacheSize,
								(100 * cc->cc_NumBytesAllocated) / cc->cc_MaxCacheSize));
						}
					}
					else
					{
						SHOWMSG("failed to allocate memory for another cache entry");
					}
				}
				else
				{
					SHOWMSG("no such luck: we already use as much memory as we can");
				}
			}

			/* If this still didn't work out, we'll try to recycle
			 * a cache node which is currently stored in the probationary
			 * or protected segments.
			 */
			if(cn == NULL)
			{
				/* Always try the probationary segment first. We will reuse
				 * the least recently-used node.
				 */
				cn = (struct CacheNode *)RemTailMinList(&cc->cc_ProbationCacheTree.st_List);
				if(cn != NULL)
				{
					RemoveMinNode(&cn->cn_UnitNode);

					cn_removed = (struct CacheNode *)remove_node_from_splay_tree(&cc->cc_ProbationCacheTree, cn->cn_SplayNode.sn_Key);

					ASSERT( cn_removed == cn && cn_removed != NULL && "THIS SHOULD NEVER HAPPEN" );
				}
				/* And if that didn't work, we'll try to reuse the least recently-used
				 * protected segment node.
				 */
				else
				{
					cn = (struct CacheNode *)RemTailMinList(&cc->cc_ProtectedCacheTree.st_List);
					if(cn != NULL)
					{
						RemoveMinNode(&cn->cn_UnitNode);

						cn_removed = (struct CacheNode *)remove_node_from_splay_tree(&cc->cc_ProtectedCacheTree, cn->cn_SplayNode.sn_Key);

						ASSERT( cn_removed == cn && cn_removed != NULL && "THIS SHOULD NEVER HAPPEN" );

						cc->cc_ProtectedCacheSize--;
					}
				}
			}

			/* Update the cache node to use a new key and put it
			 * into the probationary segment.
			 */
			if(cn != NULL)
			{
				cn->cn_SplayNode.sn_Key = key;

				if(insert_splay_node_into_tree(&cc->cc_ProbationCacheTree, &cn->cn_SplayNode))
				{
					AddHeadMinList(&cc->cc_ProbationCacheTree.st_List, &cn->cn_SplayNode.sn_Node);

					/* This cache node now belongs to this unit. */
					ASSERT( NOT node_is_in_list((struct List *)&tfu->tfu_CacheNodeList, (struct Node *)&cn->cn_UnitNode) );

					AddTailMinList(&tfu->tfu_CacheNodeList, &cn->cn_UnitNode);
				}
				else
				{
					SHOWMSG("THIS SHOULD NEVER HAPPEN: Found duplicate in cache tree");

					/* This goes back into the spare list. */
					AddHeadMinList(&cc->cc_SpareList, &cn->cn_SplayNode.sn_Node);
					cn = NULL;
				}
			}
		}

		/* If we actually managed to obtain a cache node,
		 * update the data it keeps.
		 */
		if(cn != NULL)
		{
			CopyMem(data, &cn[1], cc->cc_DataSize);

			cn->cn_Checksum = calculate_cache_data_checksum(&cn[1], cc->cc_DataSize);

			D(("data checksum for key 0x%08lx is 0x%08lx", key, cn->cn_Checksum));
		}
	}
	else
	{
		D(("data size mismatch: got %ld but expected %ld", data_size, cc->cc_DataSize));
	}

	ReleaseSemaphore(&cc->cc_Lock);

	LEAVE();
}

/****************************************************************************/

/* If the cache is larger than the maximum given memory usage permitted,
 * reduce it as far as necessary by purging the unused and least
 * recently-used cache entries. This may result in the entire cache
 * getting purged.
 */
static ULONG
reduce_cache_size_memory_usage(struct CacheContext * cc, ULONG max_memory_usage)
{
	USE_EXEC(cc->cc_TrackFileBase);

	struct CacheNode * cn;
	struct CacheNode * cn_removed;
	const size_t allocation_size = sizeof(*cn) + cc->cc_DataSize;
	ULONG total_memory_freed = 0;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	SHOWVALUE(allocation_size);

	SHOWVALUE(max_memory_usage);
	SHOWVALUE(cc->cc_NumBytesAllocated);

	/* Drop unused entries from the list of spare cache nodes first. */
	while(cc->cc_NumBytesAllocated > max_memory_usage &&
	      (cn = (struct CacheNode *)RemHeadMinList(&cc->cc_SpareList)) != NULL)
	{
		D(("FreeMem(0x%08lx, %lu)", cn, allocation_size));

		FreeMem(cn, allocation_size);
		total_memory_freed += allocation_size;

		cc->cc_NumBytesAllocated -= allocation_size;
	}

	/* Drop the least recently-used entries from the probationary segment. */
	while(cc->cc_NumBytesAllocated > max_memory_usage &&
	      (cn = (struct CacheNode *)RemTailMinList(&cc->cc_ProbationCacheTree.st_List)) != NULL)
	{
		RemoveMinNode(&cn->cn_UnitNode);

		cn_removed = (struct CacheNode *)remove_node_from_splay_tree(&cc->cc_ProbationCacheTree, cn->cn_SplayNode.sn_Key);

		ASSERT( cn_removed == cn && cn_removed != NULL && "THIS SHOULD NEVER HAPPEN" );

		D(("FreeMem(0x%08lx, %lu)", cn, allocation_size));

		FreeMem(cn, allocation_size);
		total_memory_freed += allocation_size;

		cc->cc_NumBytesAllocated -= allocation_size;
	}

	/* If we still haven't suceeded in meeting the requirements
	 * for the cache nodes to be freed, proceed to drop the least
	 * recently-used entries from the protected segment.
	 */
	while(cc->cc_NumBytesAllocated > max_memory_usage &&
	      (cn = (struct CacheNode *)RemTailMinList(&cc->cc_ProtectedCacheTree.st_List)) != NULL)
	{
		RemoveMinNode(&cn->cn_UnitNode);

		cn_removed = (struct CacheNode *)remove_node_from_splay_tree(&cc->cc_ProtectedCacheTree, cn->cn_SplayNode.sn_Key);

		ASSERT( cn_removed == cn && cn_removed != NULL && "THIS SHOULD NEVER HAPPEN" );

		cc->cc_ProtectedCacheSize--;

		D(("FreeMem(0x%08lx, %lu)", cn, allocation_size));

		FreeMem(cn, allocation_size);
		total_memory_freed += allocation_size;

		cc->cc_NumBytesAllocated -= allocation_size;
	}

	RETURN(total_memory_freed);
	return(total_memory_freed);
}

/****************************************************************************/

/* Attempt to release cache memory so that a memory allocation which
 * previously failed may succeed. Returns whether it was successful
 * in releasing memory.
 *
 * This function is called under Forbid() condition and must never
 * call Wait() at any time, whether directly or indirectly.
 */
STATIC LONG ASM
memory_cleanup(
	REG(a0, struct CacheContext *	cc),
	REG(a6, struct Library *		SysBase))
{
	const struct MemHandlerData * memh = cc->cc_MemHandlerData;
	LONG result = MEM_DID_NOTHING;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	if(cc->cc_NumBytesAllocated > 0)
	{
		if(AttemptSemaphore(&cc->cc_Lock))
		{
			/* Try to free some of the cache. */
			if (memh->memh_RequestSize < cc->cc_NumBytesAllocated)
			{
				ULONG total_bytes_freed;

				D(("trying to free %ld bytes of %ld currently allocated",
					memh->memh_RequestSize, cc->cc_NumBytesAllocated));

				total_bytes_freed = reduce_cache_size_memory_usage(cc, cc->cc_NumBytesAllocated - memh->memh_RequestSize);

				D(("freed %ld bytes, %ld were called for", total_bytes_freed, memh->memh_RequestSize));

				/* If there is still more to free, if necessary,
				 * we can try this again. Otherwise, we're done
				 * here...
				 */
				if(cc->cc_NumBytesAllocated > 0)
					result = MEM_TRY_AGAIN;
				else
					result = MEM_ALL_DONE;
			}
			/* Everything must go... */
			else
			{
				ULONG total_bytes_freed;

				SHOWMSG("freeing the entire cache");

				total_bytes_freed = reduce_cache_size_memory_usage(cc, 0);

				D(("freed %ld bytes", total_bytes_freed));

				result = MEM_ALL_DONE;
			}

			ReleaseSemaphore(&cc->cc_Lock);
		}
		else
		{
			SHOWMSG("couldn't release any data: global lock is blocked");
		}
	}
	else
	{
		SHOWMSG("couldn't release any data...");
	}

	RETURN(result);
	return(result);
}

/****************************************************************************/

/* Starting with version 39, exec.library may invoke a function such as the
 * one below when memory becomes tight so that non-essential memory may be
 * released by applications, libraries and device drivers.
 *
 * This function has very little stack space to work with. The API
 * description even goes so far as to claim that it must be able to
 * work with as little as 64 bytes of stack space.
 */
STATIC LONG ASM
mem_handler(
	REG(a0, const struct MemHandlerData *	memh),
	REG(a1, struct CacheContext *			cc),
	REG(a6, struct Library *				SysBase))
{
	LONG result;

	/* This will get passed to the memory_cleanup() function. */
	cc->cc_MemHandlerData = memh;

	/* Use a larger stack so that the memory_cleanup() function
	 * may succeed and not crash due to stack overflow.
	 */
	result = swap_stack_and_call(cc, (stack_swapped_func_t)memory_cleanup, cc->cc_StackSwap, SysBase);

	return(result);
}

/****************************************************************************/

/* Change the upper limit for the amount of memory which may be used by the
 * cache. This figure is given in bytes and will be broken down into the
 * number of cache nodes which may be used at a time. Note that if the figure
 * is too small, then the segmented LRU purge scheme will no longer work,
 * which has the effect of turning off the cache entirely.
 *
 * The cache purge scheme is described in the paper "Caching strategies to
 * improve disk system performance", by Ramakrishna Karedla,
 * J. Spencer Love & Bradley G. Wherry as published in "IEEE Computer",
 * March 1994.
 */
void
change_cache_size(
	struct CacheContext *	cc,
	ULONG					max_cache_size)
{
	USE_EXEC(cc->cc_TrackFileBase);

	const size_t allocation_size = sizeof(struct CacheNode) + cc->cc_DataSize;
	ULONG remainder;
	BOOL disable_cache;
	ULONG max_cache_nodes, one_third;

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	SHOWVALUE(max_cache_size);

	ObtainSemaphore(&cc->cc_Lock);

	/* Round up the maximum cache size to a multiple
	 * of the cache node size, unless it 0 or
	 * too small.
	 */
	remainder = max_cache_size % allocation_size;

	max_cache_size -= remainder;

	if(remainder >= allocation_size / 2)
		max_cache_size += allocation_size;

	if(max_cache_size > 0)
	{
		cc->cc_MaxCacheSize = max_cache_size;

		max_cache_nodes = cc->cc_MaxCacheSize / allocation_size;

		SHOWVALUE(max_cache_nodes);

		/* The suggested maximum size of the protected cache
		 * is about 60-80% of the total cache size. We pick
		 * two thirds here.
		 */
		one_third = max_cache_nodes / 3;
		if(one_third < max_cache_nodes)
			cc->cc_ProtectedCacheMax = max_cache_nodes - one_third;
		else
			cc->cc_ProtectedCacheMax = 0;

		SHOWVALUE(cc->cc_ProtectedCacheMax);

		/* In order to be useful, the cache ought to have some
		 * room in the protected segment...
		 */
		disable_cache = (BOOL)(cc->cc_ProtectedCacheMax < 8);
		if(disable_cache)
			D(("protected cache size (%ld) is too small to be useful", cc->cc_ProtectedCacheMax));

		/* If necessary, adjust how much memory is currently
		 * allocated for the cache.
		 */
		if(NOT disable_cache)
			reduce_cache_size_memory_usage(cc, cc->cc_MaxCacheSize);
	}
	else
	{
		disable_cache = TRUE;
	}

	if(disable_cache)
	{
		reduce_cache_size_memory_usage(cc, 0);

		cc->cc_ProtectedCacheMax	= 0;
		cc->cc_MaxCacheSize			= 0;
	}

	ReleaseSemaphore(&cc->cc_Lock);

	LEAVE();
}

/****************************************************************************/

/* Free all the memory allocated by create_cache_context(). */
void
delete_cache_context(struct CacheContext * cc)
{
	USE_EXEC(cc->cc_TrackFileBase);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	if(cc != NULL)
	{
		if(SysBase->lib_Version >= 39)
			RemMemHandler(&cc->cc_MemHandler);

		reduce_cache_size_memory_usage(cc, 0);

		FreeVec(cc->cc_StackSwap);

		FreeMem(cc, sizeof(*cc));
	}

	LEAVE();
}

/****************************************************************************/

/* Allocate memory for the management data structures used by the cache. How
 * much memory the cache may use is set up through the change_cache_size()
 * function.
 */
struct CacheContext *
create_cache_context(struct TrackFileDevice * tfd, ULONG data_size)
{
	struct CacheContext * result = NULL;
	struct CacheContext * cc;

	USE_EXEC(tfd);

	ENTER();

	#if DEBUG
	check_stack_size_available(SysBase);
	#endif /* DEBUG */

	ASSERT( data_size > 0 );

	SHOWVALUE(data_size);

	cc = AllocMem(sizeof(*cc), MEMF_ANY|MEMF_PUBLIC|MEMF_CLEAR);
	if(cc == NULL)
		goto out;

	cc->cc_DataSize = data_size;

	cc->cc_TrackFileBase = tfd;

	InitSemaphore(&cc->cc_Lock);

	initialize_splay_tree(&cc->cc_ProbationCacheTree);
	initialize_splay_tree(&cc->cc_ProtectedCacheTree);

	NewMinList(&cc->cc_SpareList);

	/* Kickstart 3.0 and higher feature a mechanism by which
	 * failed memory allocation attempts may result in asking
	 * clients to release memory, so that the allocation
	 * attempt may be retried. This can be helpful to keep the
	 * size of the cache in check when memory becomes tight.
	 */
	if(SysBase->lib_Version >= 39)
	{
		const ULONG stack_size = 2000;

		/* The memory cleanup operation has very little stack space
		 * available (64 bytes perhaps!), so we need to make sure
		 * that once it is called, it won't crash. We will use
		 * a custom stack setup when the time comes.
		 */
		cc->cc_StackSwap = AllocVec(sizeof(*cc->cc_StackSwap) + stack_size, MEMF_ANY|MEMF_PUBLIC);
		if(cc->cc_StackSwap == NULL)
			goto out;

		cc->cc_StackSwap->stk_Lower		= &cc->cc_StackSwap[1];
		cc->cc_StackSwap->stk_Upper		= (ULONG)cc->cc_StackSwap->stk_Lower + stack_size;
		cc->cc_StackSwap->stk_Pointer	= (APTR)(cc->cc_StackSwap->stk_Upper - 4 * sizeof(APTR));

		cc->cc_MemHandler.is_Node.ln_Name	= tfd->tfd_Device.dd_Library.lib_Node.ln_Name;
		cc->cc_MemHandler.is_Node.ln_Pri	= 50;
		cc->cc_MemHandler.is_Data			= cc;
		cc->cc_MemHandler.is_Code			= (VOID (*)())mem_handler;

		AddMemHandler(&cc->cc_MemHandler);
	}

	result = cc;
	cc = NULL;

 out:

	if(cc != NULL)
		delete_cache_context(cc);

	RETURN(result);
	return(result);
}

/****************************************************************************/

#endif /* ENABLE_CACHE */
