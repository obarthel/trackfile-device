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

#ifndef DOS_DOS_H
#include <dos/dos.h>
#endif /* DOS_DOS_H */

extern struct Library * SysBase;
extern struct Library * DOSBase;

#if defined(__GNUC__)

#define __NOLIBBASE__

#include <proto/exec.h>
#include <proto/dos.h>

#else

#include <clib/exec_protos.h>
#include <pragmas/exec_sysbase_pragmas.h>

#include <clib/dos_protos.h>
#include <pragmas/dos_pragmas.h>

#endif /* __GNUC__ */

#include <string.h>

/****************************************************************************/

#ifndef _COMPILER_H
#include "compiler.h"
#endif /* _COMPILER_H */

/****************************************************************************/

#ifndef AbsExecBase
#define AbsExecBase (*(struct ExecBase **)4)
#endif /* AbsExecBase */

/****************************************************************************/

#ifdef __amigaos4__
#define kprintf(...) (((struct ExecIFace*)(*(struct ExecBase **)4)->MainInterface)->DebugPrintF)(__VA_ARGS__)
#define kputc(c) (((struct ExecIFace*)(*(struct ExecBase **)4)->MainInterface)->DebugPrintF)("%lc",c)

#include <dos/obsolete.h>
#else
extern void kprintf(const char *,...);
extern void STDARGS kputc(char c);
#endif

/****************************************************************************/

#include <stdarg.h>

/****************************************************************************/

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

/****************************************************************************/

#define DEBUGLEVEL_OnlyAsserts	0
#define DEBUGLEVEL_Reports		1
#define DEBUGLEVEL_CallTracing	2

/****************************************************************************/

static BPTR debug_file = (BPTR)NULL;
static int indent_level = 0;
//static int debug_level = DEBUGLEVEL_CallTracing;
static int debug_level = DEBUGLEVEL_OnlyAsserts;

static char program_name[40];
static int program_name_len = 0;

/****************************************************************************/

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

/****************************************************************************/

void
_SETDEBUGFILE(BPTR file)
{
	debug_file = file;
}

/****************************************************************************/

void
_SETPROGRAMNAME(char *name)
{
	if(name != NULL && name[0] != '\0')
	{
		program_name_len = strlen(name);
		if(program_name_len >= (int)sizeof(program_name))
			program_name_len = sizeof(program_name)-1;

		memmove(program_name,name,(size_t)program_name_len);
		program_name[program_name_len] = '\0';
	}
	else
	{
		program_name_len = 0;
	}
}

/****************************************************************************/

int
_SETDEBUGLEVEL(int level)
{
	int old_level;

	old_level = debug_level;
	debug_level = level;

	return(old_level);
}

/****************************************************************************/

int
_GETDEBUGLEVEL(void)
{
	return(debug_level);
}

/****************************************************************************/

static int previous_debug_level = -1;

void
_PUSHDEBUGLEVEL(int level)
{
	previous_debug_level = _SETDEBUGLEVEL(level);
}

void
_POPDEBUGLEVEL(void)
{
	if(previous_debug_level != -1)
	{
		_SETDEBUGLEVEL(previous_debug_level);

		previous_debug_level = -1;
	}
}

/****************************************************************************/

static void
_INDENT(void)
{
	if(program_name_len > 0)
	{
		if(debug_file != (BPTR)NULL)
			FPrintf(debug_file,"(%s) ",program_name);
		else
			kprintf("(%s) ",program_name);
	}

	if(debug_level >= DEBUGLEVEL_CallTracing)
	{
		int i;

		if(debug_file != (BPTR)NULL)
		{
			for(i = 0 ; i < indent_level ; i++)
				FPrintf(debug_file,"   ");
		}
		else
		{
			for(i = 0 ; i < indent_level ; i++)
				kprintf("   ");
		}
	}
}

/****************************************************************************/

void
_SHOWVALUE(
	unsigned long value,
	int size,
	const char *name,
	const char *file,
	int line)
{
	if(debug_level >= DEBUGLEVEL_Reports)
	{
		char *fmt;

		switch(size)
		{
			case 1:

				fmt = "%s:%ld:%s = %ld, 0x%02lx";
				break;

			case 2:

				fmt = "%s:%ld:%s = %ld, 0x%04lx";
				break;

			default:

				fmt = "%s:%ld:%s = %ld, 0x%08lx";
				break;
		}

		_INDENT();

		if(debug_file != (BPTR)NULL)
			FPrintf(debug_file,fmt,file,line,name,value,value);
		else
			kprintf(fmt,file,line,name,value,value);

		if(size == 1 && value < 256)
		{
			if(debug_file != (BPTR)NULL)
			{
				if(value < ' ' || (value >= 127 && value < 160))
					FPrintf(debug_file,", '\\x%02lx'",value);
				else
					FPrintf(debug_file,", '%lc'",value);
			}
			else
			{
				if(value < ' ' || (value >= 127 && value < 160))
					kprintf(", '\\x%02lx'",value);
				else
					kprintf(", '%lc'",value);
			}
		}

		if(debug_file != (BPTR)NULL)
		{
			FPrintf(debug_file,"\n");
			Flush(debug_file);
		}
		else
		{
			kprintf("\n");
		}
	}
}

/****************************************************************************/

void
_SHOWPOINTER(
	void *pointer,
	const char *name,
	const char *file,
	int line)
{
	if(debug_level >= DEBUGLEVEL_Reports)
	{
		char *fmt;

		_INDENT();

		if(pointer != NULL)
			fmt = "%s:%ld:%s = 0x%08lx\n";
		else
			fmt = "%s:%ld:%s = NULL\n";

		if(debug_file != (BPTR)NULL)
		{
			FPrintf(debug_file,fmt,file,line,name,pointer);
			Flush(debug_file);
		}
		else
		{
			kprintf(fmt,file,line,name,pointer);
		}
	}
}

/****************************************************************************/

void
_SHOWSTRING(
	const char *string,
	const char *name,
	const char *file,
	int line)
{
	if(debug_level >= DEBUGLEVEL_Reports)
	{
		_INDENT();

		if(debug_file != (BPTR)NULL)
		{
			FPrintf(debug_file,"%s:%ld:%s = 0x%08lx \"%s\"\n",file,line,name,string,string);
			Flush(debug_file);
		}
		else
		{
			kprintf("%s:%ld:%s = 0x%08lx \"%s\"\n",file,line,name,string,string);
		}
	}
}

/****************************************************************************/

void
_SHOWMSG(
	const char *string,
	const char *file,
	int line)
{
	if(debug_level >= DEBUGLEVEL_Reports)
	{
		_INDENT();

		if(debug_file != (BPTR)NULL)
		{
			FPrintf(debug_file,"%s:%ld:%s\n",file,line,string);
			Flush(debug_file);
		}
		else
		{
			kprintf("%s:%ld:%s\n",file,line,string);
		}
	}
}

/****************************************************************************/

void
_DPRINTF_HEADER(
	const char *file,
	int line)
{
	if(debug_level >= DEBUGLEVEL_Reports)
	{
		_INDENT();

		if(debug_file != (BPTR)NULL)
			FPrintf(debug_file,"%s:%ld:",file,line);
		else
			kprintf("%s:%ld:",file,line);
	}
}

/****************************************************************************/

static void ASM
putch(REG(d0,UBYTE c),REG(a3,APTR unused))
{
	if(c != '\0')
		kputc(c);
}

void __VAR_ARGS
_DPRINTF(const char *fmt,...)
{
	struct Library * SysBase = (struct Library *)AbsExecBase;

	if(debug_level >= DEBUGLEVEL_Reports)
	{
		va_list args;

		#if defined(__amigaos4__)
		{
			struct ExecIFace *IExec = (struct ExecIFace *)((struct ExecBase *)SysBase)->MainInterface;

			va_startlinear(args,fmt);

			if(debug_file != (BPTR)NULL)
				VFPrintf(debug_file,fmt,va_getlinearva(args, APTR));
			else
				RawDoFmt((char *)fmt,va_getlinearva(args, APTR),(VOID (*)())putch,NULL);

			va_end(args);
		}
		#else
		{
			va_start(args,fmt);

			if(debug_file != (BPTR)NULL)
				VFPrintf(debug_file,(STRPTR)fmt,args);
			else
				RawDoFmt((char *)fmt,args,(VOID (*)())putch,NULL);

			va_end(args);
		}
		#endif /* __amigaos4__ */

		if(debug_file != (BPTR)NULL)
		{
			FPrintf(debug_file,"\n");
			Flush(debug_file);
		}
		else
		{
			kprintf("\n");
		}
	}
}

void __VAR_ARGS
_DLOG(const char *fmt,...)
{
	struct Library * SysBase = (struct Library *)AbsExecBase;

	if(debug_level >= DEBUGLEVEL_Reports)
	{
		va_list args;

		#if defined(__amigaos4__)
		{
			struct ExecIFace *IExec = (struct ExecIFace *)((struct ExecBase *)SysBase)->MainInterface;

			va_startlinear(args,fmt);

			if(debug_file != (BPTR)NULL)
			{
				VFPrintf(debug_file,fmt,va_getlinearva(args, APTR));
				Flush(debug_file);
			}
			else
			{
				RawDoFmt((char *)fmt,va_getlinearva(args, APTR),(VOID (*)())putch,NULL);
			}

			va_end(args);
		}
		#else
		{
			va_start(args,fmt);

			if(debug_file != (BPTR)NULL)
			{
				VFPrintf(debug_file,(STRPTR)fmt,args);
				Flush(debug_file);
			}
			else
			{
				RawDoFmt((char *)fmt,args,(VOID (*)())putch,NULL);
			}

			va_end(args);
		}
		#endif /* __amigaos4__ */
	}
}

/****************************************************************************/

void
_ENTER(
	const char *file,
	int line,
	const char *function)
{
	if(debug_level >= DEBUGLEVEL_CallTracing)
	{
		_INDENT();

		if(debug_file != (BPTR)NULL)
		{
			FPrintf(debug_file,"%s:%ld:Entering %s\n",file,line,function);
			Flush(debug_file);
		}
		else
		{
			kprintf("%s:%ld:Entering %s\n",file,line,function);
		}
	}

	indent_level++;
}

void
_LEAVE(
	const char *file,
	int line,
	const char *function)
{
	indent_level--;

	if(debug_level >= DEBUGLEVEL_CallTracing)
	{
		_INDENT();

		if(debug_file != (BPTR)NULL)
		{
			FPrintf(debug_file,"%s:%ld: Leaving %s\n",file,line,function);
			Flush(debug_file);
		}
		else
		{
			kprintf("%s:%ld: Leaving %s\n",file,line,function);
		}
	}
}

void
_RETURN(
	const char *file,
	int line,
	const char *function,
	unsigned long result)
{
	indent_level--;

	if(debug_level >= DEBUGLEVEL_CallTracing)
	{
		_INDENT();

		if(debug_file != (BPTR)NULL)
		{
			FPrintf(debug_file,"%s:%ld: Leaving %s (result 0x%08lx, %ld)\n",file,line,function,result,result);
			Flush(debug_file);
		}
		else
		{
			kprintf("%s:%ld: Leaving %s (result 0x%08lx, %ld)\n",file,line,function,result,result);
		}
	}
}

/****************************************************************************/

void
_ASSERT(
	int x,
	const char *xs,
	const char *file,
	int line,
	const char *function)
{
	#ifdef CONFIRM
	{
		STATIC BOOL ScrollMode	= FALSE;
		STATIC BOOL BatchMode	= FALSE;

		if(BatchMode == FALSE && debug_file == (BPTR)NULL)
		{
			if(x == 0)
			{
				kprintf("%s:%ld:Expression '%s' failed assertion in %s().\n",
				        file,
				        line,
				        xs,
				        function);

				if(ScrollMode == FALSE)
				{
					ULONG Signals;

					SetSignal(0,SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E);

					kprintf(" ^C to continue, ^D to enter scroll mode, ^E to enter batch mode\r");

					Signals = Wait(SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E);

					if(Signals & SIGBREAKF_CTRL_D)
					{
						ScrollMode = TRUE;

						kprintf("Ok, entering scroll mode\033[K\n");
					}
					else if (Signals & SIGBREAKF_CTRL_E)
					{
						BatchMode = TRUE;

						kprintf("Ok, entering batch mode\033[K\n");
					}
					else
					{
						/* Continue */

						kprintf("\033[K\r");
					}
				}
			}
		}
	}
	#else
	{
		if(x == 0)
		{
			_INDENT();

			if(debug_file != (BPTR)NULL)
			{
				FPrintf(debug_file,"%s:%ld:Expression '%s' failed assertion in %s().\n",
				        file,
				        line,
				        xs,
				        function);

				Flush(debug_file);
			}
			else
			{
				kprintf("%s:%ld:Expression '%s' failed assertion in %s().\n",
				        file,
				        line,
				        xs,
				        function);
			}
		}
	}
	#endif	/* CONFIRM */
}
