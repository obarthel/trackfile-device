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

#ifndef _COMPILER_H
#define _COMPILER_H

/****************************************************************************/

/* This header file contains definitions and macros which may make it
 * easier to write machine-specific code for various compilers (Lattice 'C',
 * Aztec 'C', SAS/C, DICE, GCC) which use different methods and keywords for
 * Amiga-specific code generation features and data properties.
 */

/****************************************************************************/

#ifndef EXEC_TYPES_H
#include <exec/types.h>
#endif /* EXEC_TYPES_H */

/****************************************************************************/

#if defined(__amigaos4__)

/****************************************************************************/

#include <amiga_compiler.h>

/* This can cause trouble with "amiga_compiler.h", which
 * does not define SAVE_DS.
 */
#define SAVE_DS

/****************************************************************************/

#else

/****************************************************************************/

/* The following definitions are redundant in the V50 AmigaOS header files. */
#ifndef AMIGA_COMPILER_H

#if defined(__SASC)

#define ASM __asm
#define REG(r, p) register __##r p
#define INLINE __inline
#define INTERRUPT __interrupt
#define STDARGS __stdargs
#define SAVE_DS __saveds
#define VARARGS68K

#elif defined(__GNUC__) && defined(AMIGA)

#define ASM
#define REG(r, p) p __asm(#r)
#define INLINE __inline__
#define INTERRUPT __attribute__((__interrupt__))
#define STDARGS __attribute__((__stkparm__))
#define SAVE_DS __attribute__((__saveds__))
#define VARARGS68K

#else

#define ASM
#define REG(r, p) p
#define INLINE
#define INTERRUPT
#define STDARGS
#define SAVE_DS
#define VARARGS68K

#endif /* __SASC */

#endif /* AMIGA_COMPILER_H */

/****************************************************************************/

#endif /* __amigaos4__ */

/****************************************************************************/

#endif /* _COMPILER_H */
