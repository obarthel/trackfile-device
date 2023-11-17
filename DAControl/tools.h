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

#ifndef DOS_DOSEXTENS_H
#include <dos/dosextens.h>
#endif /* DOS_DOSEXTENS_H */

/****************************************************************************/

#ifndef _GLOBAL_DATA_H
#include "global_data.h"
#endif /* _GLOBAL_DATA_H */

/****************************************************************************/

#include <stddef.h>

/****************************************************************************/

/* A 'C' version of the 'struct FileSysStartupMsg' from <dos/filehandler.h>.
 * Used by the decode_file_sys_startup_msg() function.
 */
struct fs_startup_msg
{
	const TEXT *			fsm_device_name;	/* exec device name */
	ULONG					fsm_device_unit;	/* exec unit number of this device */
	ULONG					fsm_device_flags;	/* flags for OpenDevice() */
	const struct DosEnvec *	fsm_environment;	/* pointer to environment table */
};

/****************************************************************************/

extern ULONG xor_shift_32(ULONG x);
extern BOOL decode_file_sys_startup_msg(struct GlobalData *gd, BPTR startup, struct fs_startup_msg * fsm);
extern BOOL string_is_number(const TEXT * str);
extern BOOL convert_string_to_number(const TEXT * str, ULONG * value_ptr);
extern STRPTR get_tf_error_message(int error_code);
extern STRPTR get_io_error_message(int error_code);
extern STRPTR get_error_message(struct GlobalData * gd, int error_code, TEXT * buffer, size_t buffer_size);
extern ULONG calculate_boot_block_checksum(const ULONG * data, int length);
extern size_t local_strlcat(char *dst, const char *src, size_t siz);
extern size_t local_strlcpy(char *dst, const char *src, size_t siz);
extern ULONG local_snprintf(struct GlobalData * gd, STRPTR buffer, ULONG buffer_size, const char *format_string, ...);
extern LONG inhibit_device(struct GlobalData * gd, STRPTR dos_device_name, BOOL on_or_off);
extern LONG format_device(struct GlobalData * gd, STRPTR dos_device_name, STRPTR label, ULONG dos_type);
extern int ShowError(struct GlobalData *gd, const char *choices, const char * fmt, ...);
extern void Error(struct GlobalData *gd, const char * fmt, ...);
extern void SortList(struct List *list, int (*compare)(const struct Node *a, const struct Node *b));
extern void tf_checksum_to_text(const struct TrackFileChecksum * tfc, TEXT * text_form);
extern size_t find_version_string(BPTR segment_list, STRPTR string_buffer, size_t string_buffer_size);
extern struct Resident * find_rom_tag(BPTR segment_list);

/****************************************************************************/

/* This C99 reentrant version of strtok() is not part of the SAS/C 6
 * compiler runtime library.
 */
extern char * local_strtok_r(char *str, const char *separator_set, char ** state_ptr);

/****************************************************************************/

#endif /* _TOOLS_H */
