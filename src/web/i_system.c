//-----------------------------------------------------------------------------
//
// DOOM for the browser - system layer.
//
// Replaces linuxdoom-1.10/i_system.c. Timing, memory, quitting and
// error handling, mapped onto the browser through Emscripten.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "d_net.h"
#include "g_game.h"
#include "w_wad.h"
#include "z_zone.h"

#include "i_system.h"

// After the DOOM headers: em_js.h defines true/false as macros.
#include <emscripten.h>

// Zone memory. DOS gave the zone ~8MB; extra room lets big PWADs load.
int	mb_used = 16;

// I_WaitVBL can't block inside an indirect call (asyncify), so the
// quit delay of M_QuitResponse is turned into a deferred quit.
static double	waituntil = 0;
static boolean	quitpending = false;

static boolean	yielded = false;


void I_Tactile (int on, int off, int total)
{
    // UNUSED.
    on = off = total = 0;
}

ticcmd_t	emptycmd;
ticcmd_t*	I_BaseTiccmd(void)
{
    return &emptycmd;
}


int  I_GetHeapSize (void)
{
    return mb_used*1024*1024;
}

byte* I_ZoneBase (int*	size)
{
    *size = mb_used*1024*1024;
    return (byte *) malloc (*size);
}


//
// I_GetTime
// returns time in 1/35th second tics
//
int  I_GetTime (void)
{
    static double	basetime = 0;
    double		now = emscripten_get_now ();

    if (!basetime)
	basetime = now;
    return (int)((now - basetime) * TICRATE / 1000.0);
}


//
// I_Yield
// Unwinds back to the browser event loop and resumes on the next turn.
//
void I_Yield (void)
{
    yielded = true;
    emscripten_sleep (1);
}

// Called once per displayed frame: make sure the browser gets
// at least one turn per frame, even when the game is falling behind.
void I_FrameYield (void)
{
    if (!yielded)
	emscripten_sleep (0);
    yielded = false;
}


//
// I_Init
//
void I_Init (void)
{
    I_InitSound();
    I_InitMusic();
}


EM_JS(void, JS_Quit, (const byte *endoom), {
    if (Module.onDoomQuit)
	Module.onDoomQuit(endoom ? HEAPU8.slice(endoom, endoom + 4000) : null);
});

EM_JS(void, JS_SyncFS, (void), {
    if (Module.syncFS)
	Module.syncFS();
});

static void I_DoQuit (void)
{
    int		lump;

    D_QuitNetGame ();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults ();
    I_ShutdownGraphics();
    JS_SyncFS ();

    // DOS showed the ENDOOM text screen on exit.
    lump = W_CheckNumForName ("ENDOOM");
    JS_Quit (lump >= 0 ? W_CacheLumpNum (lump, PU_CACHE) : NULL);

    emscripten_force_exit (0);
}

//
// I_Quit
//
void I_Quit (void)
{
    if (waituntil > emscripten_get_now ())
    {
	// Let the quit sound play first (see I_WaitVBL).
	quitpending = true;
	return;
    }
    I_DoQuit ();
}

// Called every frame by I_StartFrame.
void I_CheckQuit (void)
{
    if (quitpending && emscripten_get_now () >= waituntil)
	I_DoQuit ();
}

void I_WaitVBL(int count)
{
    waituntil = emscripten_get_now () + count * (1000.0/70.0);
}

// Called from JavaScript when the page is hidden or closed, so menu
// settings survive even without quitting through the menu.
EMSCRIPTEN_KEEPALIVE
void I_WebSaveDefaults (void)
{
    M_SaveDefaults ();
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

    mem = (byte *)malloc (length);
    memset (mem,0,length);
    return mem;
}


//
// I_Error
//
extern boolean demorecording;

EM_JS(void, JS_Error, (const char *msg), {
    if (Module.onDoomError)
	Module.onDoomError(UTF8ToString(msg));
});

void I_Error (char *error, ...)
{
    static boolean	inerror = false;
    char		msg[512];
    va_list		argptr;

    va_start (argptr,error);
    vsnprintf (msg, sizeof(msg), error, argptr);
    va_end (argptr);

    fprintf (stderr, "Error: %s\n", msg);

    // Shutdown. Here might be other errors.
    if (!inerror)
    {
	inerror = true;
	if (demorecording)
	    G_CheckDemoStatus();
	D_QuitNetGame ();
	I_ShutdownGraphics();
    }

    JS_Error (msg);
    emscripten_force_exit (-1);
}
