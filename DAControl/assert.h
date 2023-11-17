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

#ifndef _ASSERT_H
#define _ASSERT_H

/****************************************************************************/

#if defined(DEBUG)
#undef DEBUG
#endif /* DEBUG */

/****************************************************************************/

/* IMPORTANT: If DEBUG is redefined, it must happen only here. This
 *            will cause all modules to depend upon it to be rebuilt
 *            by the smakefile (that is, provided the smakefile has
 *            all the necessary dependency lines in place).
 */

/*#define DEBUG*/

/****************************************************************************/

/* Profiling support code. */
#ifdef __SASC
#include <sprof.h>
#else
#ifdef PROFILE_OFF
#undef PROFILE_OFF
#endif
#define PROFILE_OFF() {}

#ifdef PROFILE_ON
#undef PROFILE_ON
#endif
#define PROFILE_ON()  {}
#endif /* __SASC */

/****************************************************************************/

#ifdef ASSERT
#undef ASSERT
#endif	/* ASSERT */

#define PUSH_ASSERTS()	PUSHDEBUGLEVEL(0)
#define PUSH_REPORTS()	PUSHDEBUGLEVEL(1)
#define PUSH_CALLS()	PUSHDEBUGLEVEL(2)
#define PUSH_ALL()	PUSHDEBUGLEVEL(2)
#define POP()		POPDEBUGLEVEL()

#if defined(DEBUG)

 #ifndef DOS_DOS_H
 #include <dos/dos.h>
 #endif /* DOS_DOS_H */

 #if defined(__amigaos4__)
  #define __VAR_ARGS __attribute__((linearvarargs))
 #else
  #define __VAR_ARGS
 #endif /* __amigaos4__ */

 #if defined(__GNUC__)
  #define __PRINTF_FORMAT __attribute__ ((format (printf, 1, 2)))
 #else
  #define __PRINTF_FORMAT
 #endif /* __GNUC__ */

 void _ASSERT(int x,const char *xs,const char *file,int line,const char *function);
 void _SHOWVALUE(unsigned long value,int size,const char *name,const char *file,int line);
 void _SHOWPOINTER(void *p,const char *name,const char *file,int line);
 void _SHOWSTRING(const char *string,const char *name,const char *file,int line);
 void _SHOWMSG(const char *msg,const char *file,int line);
 void _ENTER(const char *file,int line,const char *function);
 void _LEAVE(const char *file,int line,const char *function);
 void _RETURN(const char *file,int line,const char *function,unsigned long result);
 void _DPRINTF_HEADER(const char *file,int line);
 int  _SETDEBUGLEVEL(int level);
 void _PUSHDEBUGLEVEL(int level);
 void _POPDEBUGLEVEL(void);
 int  _GETDEBUGLEVEL(void);
 void _SETDEBUGFILE(BPTR file);
 void _SETPROGRAMNAME(char *name);

 void __VAR_ARGS _DPRINTF(const char *format,...) __PRINTF_FORMAT;
 void __VAR_ARGS _DLOG(const char *format,...) __PRINTF_FORMAT;

#if defined(__SASC)
#else
#if INCLUDE_VERSION >= 44
 #include <clib/debug_protos.h>
#endif
#endif

#if !defined(__SASC)
#define __FUNC__	__FUNCTION__
#endif

 #define ASSERT(x)		do { PROFILE_OFF(); _ASSERT((int)(x),#x,__FILE__,__LINE__,__FUNC__); PROFILE_ON(); } while(0)
 #define ENTER()		do { PROFILE_OFF(); _ENTER(__FILE__,__LINE__,__FUNC__); PROFILE_ON(); } while(0)
 #define LEAVE()		do { PROFILE_OFF(); _LEAVE(__FILE__,__LINE__,__FUNC__); PROFILE_ON(); } while(0)
 #define RETURN(r)		do { PROFILE_OFF(); _RETURN(__FILE__,__LINE__,__FUNC__,(unsigned long)r); PROFILE_ON(); } while(0)
 #define SHOWVALUE(v)		do { PROFILE_OFF(); _SHOWVALUE((unsigned long)(v),sizeof(v),#v,__FILE__,__LINE__); PROFILE_ON(); } while(0)
 #define SHOWPOINTER(p)		do { PROFILE_OFF(); _SHOWPOINTER(p,#p,__FILE__,__LINE__); PROFILE_ON(); } while(0)
 #define SHOWSTRING(s)		do { PROFILE_OFF(); _SHOWSTRING(s,#s,__FILE__,__LINE__); PROFILE_ON(); } while(0)
 #define SHOWMSG(s)		do { PROFILE_OFF(); _SHOWMSG(s,__FILE__,__LINE__); PROFILE_ON(); } while(0)
 #define D(s)			do { PROFILE_OFF(); _DPRINTF_HEADER(__FILE__,__LINE__); _DPRINTF s; PROFILE_ON(); } while(0)
 #define PRINTHEADER()		do { PROFILE_OFF(); _DPRINTF_HEADER(__FILE__,__LINE__); PROFILE_ON(); PROFILE_ON(); } while(0)
 #define PRINTF(s)		do { PROFILE_OFF(); _DLOG s; PROFILE_ON(); } while(0)
 #define LOG(s)			do { PROFILE_OFF(); _DPRINTF_HEADER(__FILE__,__LINE__); _DLOG("<%s()>:",__FUNC__); _DLOG s; PROFILE_ON(); } while(0)
 #define SETDEBUGLEVEL(l)	do { PROFILE_OFF(); _SETDEBUGLEVEL(l); PROFILE_ON(); } while(0)
 #define PUSHDEBUGLEVEL(l)	do { PROFILE_OFF(); _PUSHDEBUGLEVEL(l); PROFILE_ON(); } while(0)
 #define POPDEBUGLEVEL()	do { PROFILE_OFF(); _POPDEBUGLEVEL(); PROFILE_ON(); } while(0)
 #define SETDEBUGFILE(f)	_SETDEBUGFILE(f)
 #define SETPROGRAMNAME(n)	do { PROFILE_OFF(); _SETPROGRAMNAME(n); PROFILE_ON(); } while(0)
 #define GETDEBUGLEVEL()	_GETDEBUGLEVEL()

 #undef DEBUG
 #define DEBUG 1
#else
 #define ASSERT(x)		((void)0)
 #define ENTER()		((void)0)
 #define LEAVE()		((void)0)
 #define RETURN(r)		((void)0)
 #define SHOWVALUE(v)		((void)0)
 #define SHOWPOINTER(p)		((void)0)
 #define SHOWSTRING(s)		((void)0)
 #define SHOWMSG(s)		((void)0)
 #define D(s)			((void)0)
 #define PRINTHEADER()		((void)0)
 #define PRINTF(s)		((void)0)
 #define LOG(s)			((void)0)
 #define SETDEBUGLEVEL(l)	((void)0)
 #define PUSHDEBUGLEVEL(l)	((void)0)
 #define POPDEBUGLEVEL()	((void)0)
 #define SETDEBUGFILE(f)	((void)0)
 #define SETPROGRAMNAME(n)	((void)0)
 #define GETDEBUGLEVEL()	(0)

 #ifdef DEBUG
 #undef DEBUG
 #endif /* DEBUG */

 #define DEBUG 0
#endif /* DEBUG */

/****************************************************************************/

#endif /* _ASSERT_H */
