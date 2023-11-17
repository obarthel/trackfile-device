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

#ifndef _INSERT_MEDIA_BY_NAME_H
#define _INSERT_MEDIA_BY_NAME_H

/****************************************************************************/

#ifndef _GLOBAL_DATA_H
#include "global_data.h"
#endif /* _GLOBAL_DATA_H */

/****************************************************************************/

#ifndef DOS_DOSASL_H
#include <dos/dosasl.h>
#endif /* DOS_DOSASL_H */

/****************************************************************************/

LONG insert_media_by_name(struct GlobalData * gd, BOOL quiet, BOOL verbose, BOOL ignore, BOOL write_protected, BOOL enable_cache, BOOL prefill_cache, LONG cache_size, struct AnchorPath * ap, STRPTR * files, LONG unit, BOOL use_next_available_unit, int num_cylinders, int num_sectors, STRPTR dos_device_name, LONG max_matches);

/****************************************************************************/

#endif /* _INSERT_MEDIA_BY_NAME_H */
