//-----------------------------------------------------------------------------
//
// DOOM for the browser - video and input layer.
//
// Replaces linuxdoom-1.10/i_video.c (X11). The engine still renders
// into its own 320x200 8-bit buffer (screens[0]); here it is expanded
// through the current palette and handed to a <canvas>.
// Keyboard and mouse events arrive from JavaScript (doom.js) already
// translated to DOOM key codes.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>

#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"
#include "doomdef.h"

// After the DOOM headers: em_js.h defines true/false as macros.
#include <emscripten.h>

void I_FrameYield (void);
void I_CheckQuit (void);

// 32-bit RGBA version of the 8-bit screen, read by JavaScript.
static unsigned int	rgbabuffer[SCREENWIDTH*SCREENHEIGHT];
static unsigned int	palette32[256];

// Mouse motion is accumulated between tics, like the X11 version did
// with its pointer warping, and posted once per tic.
static int		mousedx;
static int		mousedy;
static int		mousebuttons;


void I_ShutdownGraphics(void)
{
}


//
// I_StartFrame
//
void I_StartFrame (void)
{
    I_CheckQuit ();
}


//
// Input entry points, called from JavaScript event handlers.
//
EMSCRIPTEN_KEEPALIVE
void I_WebKey (int down, int key)
{
    event_t	event;

    event.type = down ? ev_keydown : ev_keyup;
    event.data1 = key;
    event.data2 = event.data3 = 0;
    D_PostEvent (&event);
}

EMSCRIPTEN_KEEPALIVE
void I_WebMouseButtons (int buttons)
{
    event_t	event;

    mousebuttons = buttons;
    event.type = ev_mouse;
    event.data1 = buttons;
    event.data2 = event.data3 = 0;
    D_PostEvent (&event);
}

EMSCRIPTEN_KEEPALIVE
void I_WebMouseMove (int dx, int dy)
{
    mousedx += dx;
    mousedy += dy;
}


// Lets the page know whether losing the pointer lock should open the menu.
extern boolean	menuactive;

EMSCRIPTEN_KEEPALIVE
int I_WebMenuActive (void)
{
    return menuactive;
}


//
// I_StartTic
//
void I_StartTic (void)
{
    event_t	event;

    if (mousedx || mousedy)
    {
	event.type = ev_mouse;
	event.data1 = mousebuttons;
	event.data2 = mousedx << 2;
	event.data3 = -mousedy << 2;
	mousedx = mousedy = 0;
	D_PostEvent (&event);
    }
}


//
// I_UpdateNoBlit
//
void I_UpdateNoBlit (void)
{
}


EM_JS(void, JS_Blit, (const unsigned int *pixels, int width, int height), {
    Module.doomBlit(HEAPU8.subarray(pixels, pixels + width*height*4), width, height);
});

//
// I_FinishUpdate
//
void I_FinishUpdate (void)
{
    static int	lasttic;
    int		tics;
    int		i;
    byte*	src;
    unsigned int* dest;

    // draws little dots on the bottom of the screen
    if (devparm)
    {
	i = I_GetTime();
	tics = i - lasttic;
	lasttic = i;
	if (tics > 20) tics = 20;

	for (i=0 ; i<tics*2 ; i+=2)
	    screens[0][ (SCREENHEIGHT-1)*SCREENWIDTH + i] = 0xff;
	for ( ; i<20*2 ; i+=2)
	    screens[0][ (SCREENHEIGHT-1)*SCREENWIDTH + i] = 0x0;
    }

    src = screens[0];
    dest = rgbabuffer;
    for (i=0 ; i<SCREENWIDTH*SCREENHEIGHT ; i++)
	*dest++ = palette32[*src++];

    JS_Blit (rgbabuffer, SCREENWIDTH, SCREENHEIGHT);

    I_FrameYield ();
}


//
// I_ReadScreen
//
void I_ReadScreen (byte* scr)
{
    memcpy (scr, screens[0], SCREENWIDTH*SCREENHEIGHT);
}


//
// I_SetPalette
// Takes full 8 bit values, gamma corrected like the original.
//
void I_SetPalette (byte* palette)
{
    int		i;
    unsigned int r, g, b;

    for (i=0 ; i<256 ; i++)
    {
	r = gammatable[usegamma][*palette++];
	g = gammatable[usegamma][*palette++];
	b = gammatable[usegamma][*palette++];
	palette32[i] = 0xff000000u | (b << 16) | (g << 8) | r;	// RGBA, little endian
    }
}


void I_InitGraphics(void)
{
    static int	firsttime=1;

    if (!firsttime)
	return;
    firsttime = 0;
}
