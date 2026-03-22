// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//
//-----------------------------------------------------------------------------

//static const char
//rcsid[] = "$Id: m_bbox.c,v 1.1 1997/02/03 22:45:10 b1 Exp $";


#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <stdarg.h>
#include <sys/time.h>
#include <unistd.h>

#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"

#include "d_net.h"
#include "g_game.h"

#ifdef __GNUG__
#pragma implementation "i_system.h"
#endif
#include "i_system.h"


#ifdef COMBINE_SCREENS
unsigned char CombinedScreens[SCREENWIDTH*SCREENHEIGHT];
#else
unsigned char CombinedScreens[SCREENWIDTH*SCREENHEIGHT*4];
#endif


int	mb_used = 6;


void
I_Tactile
( int	on,
  int	off,
  int	total )
{
  // UNUSED.
  on = off = total = 0;
}

ticcmd_t	emptycmd;
ticcmd_t*	I_BaseTiccmd(void)
{
    return &emptycmd;
}

#if defined(THEOS_RUNTIME)
static unsigned char* DOOMHeap = NULL;
static int DOOMHeapSize = 0;

static int I_EnsureZoneHeap(void)
{
    int mib;

    if (DOOMHeap && DOOMHeapSize > 0)
        return DOOMHeapSize;

    // Prefer larger contiguous blocks first; fall back gradually.
    for (mib = 128; mib >= 16; mib -= 8)
    {
        size_t bytes = (size_t) mib * 1024U * 1024U;
        unsigned char* heap = (unsigned char*) malloc(bytes);
        if (!heap)
            continue;

        DOOMHeap = heap;
        DOOMHeapSize = (int) bytes;
        memset(DOOMHeap, 0, bytes);
        fprintf(stderr, "[DOOM] zone heap=%d MiB\n", mib);
        return DOOMHeapSize;
    }

    return 0;
}
#else
unsigned char DOOMHeap[FIXED_HEAP];
#endif

int  I_GetHeapSize (void)
{
#if defined(THEOS_RUNTIME)
    return I_EnsureZoneHeap();
#else
    return FIXED_HEAP;
#endif
}

byte* I_ZoneBase (int*	size)
{
#if defined(THEOS_RUNTIME)
    int heap_size = I_EnsureZoneHeap();
    if (heap_size <= 0 || !DOOMHeap)
    {
        *size = 0;
        return NULL;
    }

    *size = heap_size;
    return (byte*) DOOMHeap;
#else
    *size = FIXED_HEAP; //mb_used*1024*1024;
    return (byte *) DOOMHeap;
#endif
}



//
// I_GetTime
// returns time in 1/70th second tics
//
int  I_GetTime (void)
{
    struct timeval	tp;
    struct timezone	tzp;
    int			newtics;
    static int		basetime=0;
  
    gettimeofday(&tp, &tzp);
    if (!basetime)
	basetime = tp.tv_sec;
    newtics = (tp.tv_sec-basetime)*TICRATE + tp.tv_usec*TICRATE/1000000;
    return newtics;
}



//
// I_Init
//
void I_Init (void)
{
    I_InitSound();
    //  I_InitGraphics();
}

//
// I_Quit
//
void I_Quit (void)
{
    D_QuitNetGame ();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults ();
    I_ShutdownGraphics();
#if defined(THEOS_RUNTIME)
    if (DOOMHeap)
    {
        free(DOOMHeap);
        DOOMHeap = NULL;
        DOOMHeapSize = 0;
    }
#endif
    exit(0);
}

void I_WaitVBL(int count)
{
#ifdef SGI
    sginap(1);                                           
#else
#ifdef SUN
    sleep(0);
#else
    usleep (count * (1000000/700) );                                
#endif
#endif
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

byte*	I_AllocLow(int length)
{
    byte*	mem;
    mem = CombinedScreens;
    memset (mem,0,length);
    return mem;
}


//
// I_Error
//
extern boolean demorecording;

void I_Error (char *error, ...)
{
    va_list	argptr;

    // Message first.
    va_start (argptr,error);
    fprintf (stderr, "Error: ");
    vfprintf (stderr,error,argptr);
    fprintf (stderr, "\n");
    va_end (argptr);

    fflush( stderr );

    // Shutdown. Here might be other errors.
    if (demorecording)
	G_CheckDemoStatus();

    D_QuitNetGame ();
    I_ShutdownGraphics();
#if defined(THEOS_RUNTIME)
    if (DOOMHeap)
    {
        free(DOOMHeap);
        DOOMHeap = NULL;
        DOOMHeapSize = 0;
    }
#endif
    
    exit(-1);
}
