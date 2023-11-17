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

#ifndef _FUNCTIONS_H
#define _FUNCTIONS_H

/****************************************************************************/

LONG ASM tf_start_unit_taglist(REG (d0, LONG which_unit ), REG (a0, struct TagItem *tags ), REG (a6, struct TrackFileDevice *tfd ));
LONG ASM tf_stop_unit_taglist(REG (d0, LONG which_unit ), REG (a0, struct TagItem *tags ), REG (a6, struct TrackFileDevice *tfd ));
LONG ASM tf_insert_media_taglist(REG (d0, LONG which_unit ), REG (a0, struct TagItem *tags ), REG (a6, struct TrackFileDevice *tfd ));
LONG ASM tf_eject_media_taglist(REG (d0, LONG which_unit ), REG (a0, struct TagItem *tags ), REG (a6, struct TrackFileDevice *tfd ));
LONG ASM tf_change_unit_taglist(REG (d0, LONG which_unit ), REG (a0, struct TagItem *tags ), REG (a6, struct TrackFileDevice *tfd ));
struct TrackFileUnitData *ASM tf_get_unit_data(REG (d0, LONG which_unit ), REG (a6, struct TrackFileDevice *tfd ));
VOID ASM tf_free_unit_data(REG (a0, struct TrackFileUnitData *first_tfud ), REG (a6, struct TrackFileDevice *tfd ));
LONG ASM tf_examine_file_size(REG (d0, LONG file_size), REG (a6, struct TrackFileDevice *tfd ));

/****************************************************************************/

#endif /* _FUNCTIONS_H */
