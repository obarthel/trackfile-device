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

#include <exec/types.h>

#include <string.h>

/****************************************************************************/

#ifndef _COMPILER_H
#include "compiler.h"
#endif /* _COMPILER_H */

/****************************************************************************/

extern void RawPutChar(UBYTE c);
extern APTR RawDoFmt( const char *formatString, APTR dataStream, VOID (*putChProc)(), APTR putChData );

#if defined(__SASC)
#pragma syscall RawDoFmt 20a BA9804
#pragma syscall RawPutChar 204 001
#endif /* __SASC */

/****************************************************************************/

#include <stdarg.h>

/****************************************************************************/

#if defined(__GNUC__)
 #define __PRINTF_FORMAT __attribute__ ((format (printf, 1, 2)))
 #define __PRINTF_FORMAT
#endif /* __GNUC__ */

/****************************************************************************/

#define DEBUGLEVEL_OnlyAsserts	0
#define DEBUGLEVEL_Reports		1
#define DEBUGLEVEL_CallTracing	2

/****************************************************************************/

static int indent_level = 0;
//static int debug_level = DEBUGLEVEL_CallTracing;
static int debug_level = DEBUGLEVEL_OnlyAsserts;

static char program_name[40];
static int program_name_len = 0;

/****************************************************************************/

void _ASSERT(int x,const char *xs,const char *file,int line,const char *function);
void _SHOWVALUE(unsigned long value,int size,const char *name,const char *file,int line);
void _SHOWPOINTER(const void *p,const char *name,const char *file,int line);
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
void _SETPROGRAMNAME(char *name);

/****************************************************************************/

static void ASM
putch(REG(d0,UBYTE c))
{
	RawPutChar(c);
}

static void
local_kprintf(const char *fmt,...)
{
	va_list args;

	va_start(args,fmt);
	RawDoFmt(fmt,args,(VOID (*)())putch,NULL);
	va_end(args);
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
		local_kprintf("(%s) ",program_name);

	if(debug_level >= DEBUGLEVEL_CallTracing)
	{
		int i;

		for(i = 0 ; i < indent_level ; i++)
			local_kprintf("   ");
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

		local_kprintf(fmt,file,line,name,value,value);

		if(size == 1 && value < 256)
		{
			if(value < ' ' || (value >= 127 && value < 160))
				local_kprintf(", '\\x%02lx'",value);
			else
				local_kprintf(", '%lc'",value);
		}

		local_kprintf("\n");
	}
}

/****************************************************************************/

void
_SHOWPOINTER(
	const void *pointer,
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

		local_kprintf(fmt,file,line,name,pointer);
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

		local_kprintf("%s:%ld:%s = 0x%08lx \"%s\"\n",file,line,name,string,string);
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

		local_kprintf("%s:%ld:%s\n",file,line,string);
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

		local_kprintf("%s:%ld:",file,line);
	}
}

/****************************************************************************/

void
_DPRINTF(const char *fmt,...)
{
	if(debug_level >= DEBUGLEVEL_Reports)
	{
		va_list args;

		va_start(args,fmt);
		RawDoFmt((char *)fmt,args,(VOID (*)())putch,NULL);
		va_end(args);

		local_kprintf("\n");
	}
}

void
_DLOG(const char *fmt,...)
{
	if(debug_level >= DEBUGLEVEL_Reports)
	{
		va_list args;

		va_start(args,fmt);
		RawDoFmt((char *)fmt,args,(VOID (*)())putch,NULL);
		va_end(args);
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

		local_kprintf("%s:%ld:Entering %s\n",file,line,function);
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

		local_kprintf("%s:%ld: Leaving %s\n",file,line,function);
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

		local_kprintf("%s:%ld: Leaving %s (result 0x%08lx, %ld)\n",file,line,function,result,result);
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
	if(x == 0)
	{
		_INDENT();

		local_kprintf("%s:%ld:Expression \"%s\" failed assertion in %s().\n",
			file,
			line,
			xs,
			function);
	}
}
