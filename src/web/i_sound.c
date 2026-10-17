//-----------------------------------------------------------------------------
//
// DOOM for the browser - sound effects layer.
//
// Based on the software mixer of linuxdoom-1.10/i_sound.c: 8 internal
// channels of 8-bit unsigned samples, mixed through per-volume lookup
// tables into a 16-bit stereo stream. Changes from the Linux version:
//
//  - Pull model: the Web Audio callback asks for N frames at the output
//    rate (I_WebMixAudio) instead of the game writing to /dev/dsp.
//  - Proper handles, so S_StopSound / I_SoundIsPlaying / positional
//    updates work as with the DOS DMX library (they were stubs on Linux).
//  - Each effect plays at the rate stored in its DMX header, resampled
//    nearest-neighbour like a Sound Blaster would.
//  - No random pitch shifting: DOS v1.9 didn't do it either.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "z_zone.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"
#include "doomdef.h"

#include "i_music.h"

// After the DOOM headers: em_js.h defines true/false as macros.
#include <emscripten.h>

#define NUM_CHANNELS		8

typedef struct
{
    unsigned char*	data;		// raw 8-bit unsigned samples
    int			length;		// in samples
    int			rate;		// Hz, from the DMX header
} sfxdata_t;

static sfxdata_t	sfxdata[NUMSFX];

// The channel data pointers, start and end.
static unsigned char*	channels[NUM_CHANNELS];
static unsigned char*	channelsend[NUM_CHANNELS];

// 16.16 fixed point position step, relative to the output rate.
static unsigned int	channelstep[NUM_CHANNELS];
static unsigned int	channelstepremainder[NUM_CHANNELS];

// Time/gametic that the channel started playing,
//  used to determine oldest, which automatically
//  has lowest priority.
static int		channelstart[NUM_CHANNELS];
static int		channelhandles[NUM_CHANNELS];
static int		channelids[NUM_CHANNELS];

// Pitch to stepping lookup, unused.
static int		steptable[256];

// Volume lookups.
static int		vol_lookup[128*256];

// Hardware left and right channel volume lookup.
static int*		channelleftvol_lookup[NUM_CHANNELS];
static int*		channelrightvol_lookup[NUM_CHANNELS];

static int		outputrate = 44100;


//
// This function loads the sound data from the WAD lump,
//  for single sound.
//
static void getsfx (int sfxid)
{
    char		name[20];
    int			sfxlump;
    int			size;
    int			length;
    unsigned char*	raw;

    // Now, there is a severe problem with the
    //  sound handling, in it is not (yet/anymore)
    //  gamemode aware. That means, sounds from
    //  DOOM II will be requested even with DOOM
    //  shareware.
    // We will use a default sound for replacement.
    sprintf(name, "ds%s", S_sfx[sfxid].name);
    if ( W_CheckNumForName(name) == -1 )
	sfxlump = W_GetNumForName("dspistol");
    else
	sfxlump = W_GetNumForName(name);

    size = W_LumpLength (sfxlump);
    raw = W_CacheLumpNum (sfxlump, PU_STATIC);

    // DMX header: format (3), sample rate, sample count.
    length = raw[4] | (raw[5]<<8) | (raw[6]<<16) | (raw[7]<<24);
    if (size < 8 || raw[0] != 3 || raw[1] != 0 || length > size - 8)
	length = size - 8;
    sfxdata[sfxid].rate = raw[2] | (raw[3]<<8);
    if (!sfxdata[sfxid].rate)
	sfxdata[sfxid].rate = 11025;

    // DMX skips the 16 padding bytes at each end of the sample.
    if (length > 48)
    {
	sfxdata[sfxid].data = malloc (length - 32);
	memcpy (sfxdata[sfxid].data, raw + 8 + 16, length - 32);
	sfxdata[sfxid].length = length - 32;
    }
    else
    {
	sfxdata[sfxid].data = malloc (length > 0 ? length : 1);
	memcpy (sfxdata[sfxid].data, raw + 8, length);
	sfxdata[sfxid].length = length;
    }

    Z_ChangeTag (raw, PU_CACHE);
}


//
// Volume and stereo separation to left/right lookup tables.
//
static void setchannelparams (int slot, int volume, int seperation)
{
    int		leftvol;
    int		rightvol;

    // DOS passes volume as 0..15, DMX scaled it to 0..127.
    volume *= 8;
    if (volume > 127)
	volume = 127;
    if (volume < 0)
	volume = 0;

    // Linear panning, as DMX does.
    leftvol = ((254 - seperation) * volume) / 127;
    rightvol = (seperation * volume) / 127;

    if (leftvol < 0) leftvol = 0;
    if (leftvol > 127) leftvol = 127;
    if (rightvol < 0) rightvol = 0;
    if (rightvol > 127) rightvol = 127;

    channelleftvol_lookup[slot] = &vol_lookup[leftvol*256];
    channelrightvol_lookup[slot] = &vol_lookup[rightvol*256];
}


//
// This function adds a sound to the
//  list of currently active sounds,
//  which is maintained as a given number
//  (eight, usually) of internal channels.
// Returns a handle.
//
static int addsfx (int sfxid, int volume, int seperation)
{
    static unsigned short	handlenums = 0;

    int		i;
    int		oldest = gametic;
    int		oldestnum = 0;
    int		slot;

    // Chainsaw troubles.
    // Play these sound effects only one at a time.
    if ( sfxid == sfx_sawup
	 || sfxid == sfx_sawidl
	 || sfxid == sfx_sawful
	 || sfxid == sfx_sawhit
	 || sfxid == sfx_stnmov
	 || sfxid == sfx_pistol	 )
    {
	for (i=0 ; i<NUM_CHANNELS ; i++)
	{
	    if ( (channels[i])
		 && (channelids[i] == sfxid) )
	    {
		channels[i] = 0;
		break;
	    }
	}
    }

    // Loop all channels to find oldest SFX.
    for (i=0; (i<NUM_CHANNELS) && (channels[i]); i++)
    {
	if (channelstart[i] < oldest)
	{
	    oldestnum = i;
	    oldest = channelstart[i];
	}
    }

    if (i == NUM_CHANNELS)
	slot = oldestnum;
    else
	slot = i;

    channels[slot] = sfxdata[sfxid].data;
    channelsend[slot] = channels[slot] + sfxdata[sfxid].length;

    if (!handlenums)
	handlenums = 100;
    channelhandles[slot] = handlenums++;

    channelstep[slot] = (unsigned int)(((long long)sfxdata[sfxid].rate << 16) / outputrate);
    channelstepremainder[slot] = 0;
    channelstart[slot] = gametic;
    channelids[slot] = sfxid;

    setchannelparams (slot, volume, seperation);

    return channelhandles[slot];
}


static int handletoslot (int handle)
{
    int		i;

    for (i=0 ; i<NUM_CHANNELS ; i++)
	if (channels[i] && channelhandles[i] == handle)
	    return i;
    return -1;
}


//
// SFX API
//
void I_SetChannels()
{
    int		i;
    int		j;
    int*	steptablemid = steptable + 128;

    // This table provides step widths for pitch parameters.
    for (i=-128 ; i<128 ; i++)
	steptablemid[i] = 65536;

    // Generates volume lookup tables
    //  which also turn the unsigned samples
    //  into signed samples.
    for (i=0 ; i<128 ; i++)
	for (j=0 ; j<256 ; j++)
	    vol_lookup[i*256+j] = (i*(j-128)*256)/127;
}


void I_SetSfxVolume(int volume)
{
    snd_SfxVolume = volume;
}


//
// Retrieve the raw data lump index
//  for a given SFX name.
//
int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    if (W_CheckNumForName(namebuf) == -1)
	return W_GetNumForName("dspistol");
    return W_GetNumForName(namebuf);
}


int
I_StartSound
( int		id,
  int		vol,
  int		sep,
  int		pitch,
  int		priority )
{
    return addsfx (id, vol, sep);
}


void I_StopSound (int handle)
{
    int slot = handletoslot (handle);

    if (slot >= 0)
	channels[slot] = 0;
}


int I_SoundIsPlaying(int handle)
{
    return handletoslot (handle) >= 0;
}


void
I_UpdateSoundParams
( int	handle,
  int	vol,
  int	sep,
  int	pitch)
{
    int slot = handletoslot (handle);

    if (slot >= 0)
	setchannelparams (slot, vol, sep);
}


//
// Mixing is pulled by the audio callback, see I_WebMixAudio.
//
void I_UpdateSound (void)
{
}

void I_SubmitSound (void)
{
}


//
// Called from the Web Audio callback with the stereo float buffer
// to fill. Mixes the effect channels exactly as the Linux mixer did,
// then adds the music.
//
EMSCRIPTEN_KEEPALIVE
void I_WebMixAudio (float* out, int frames)
{
    unsigned int	sample;
    int			dl;
    int			dr;
    int			chan;
    int			n;
    float*		o = out;

    for (n = 0 ; n < frames ; n++)
    {
	dl = 0;
	dr = 0;

	for ( chan = 0; chan < NUM_CHANNELS; chan++ )
	{
	    if (channels[ chan ])
	    {
		sample = *channels[ chan ];
		dl += channelleftvol_lookup[ chan ][sample];
		dr += channelrightvol_lookup[ chan ][sample];
		channelstepremainder[ chan ] += channelstep[ chan ];
		channels[ chan ] += channelstepremainder[ chan ] >> 16;
		channelstepremainder[ chan ] &= 65536-1;

		if (channels[ chan ] >= channelsend[ chan ])
		    channels[ chan ] = 0;
	    }
	}

	if (dl > 0x7fff) dl = 0x7fff;
	else if (dl < -0x8000) dl = -0x8000;
	if (dr > 0x7fff) dr = 0x7fff;
	else if (dr < -0x8000) dr = -0x8000;

	*o++ = dl * (1.0f/32768.0f);
	*o++ = dr * (1.0f/32768.0f);
    }

    I_MusicRender (out, frames, outputrate);
}


void I_ShutdownSound(void)
{
    int i;

    for (i=0 ; i<NUM_CHANNELS ; i++)
	channels[i] = 0;
}


EM_JS(int, JS_AudioRate, (void), {
    return Module.audioRate ? Module.audioRate() : 44100;
});

void
I_InitSound()
{
    int i;

    outputrate = JS_AudioRate ();
    fprintf( stderr, "I_InitSound: output at %d Hz\n", outputrate);

    // Initialize external data (all sounds) at start, keep static.
    for (i=1 ; i<NUMSFX ; i++)
    {
	// Alias? Example is the chaingun sound linked to pistol.
	if (!S_sfx[i].link)
	    getsfx (i);
    }
    for (i=1 ; i<NUMSFX ; i++)
    {
	if (S_sfx[i].link)
	    sfxdata[i] = sfxdata[S_sfx[i].link - S_sfx];
	S_sfx[i].data = sfxdata[i].data;
    }

    fprintf( stderr, "I_InitSound: pre-cached all sound data\n");
}


//
// MUSIC API, see i_music.c.
//
void I_InitMusic(void)		{ I_MusicInit (outputrate); }
void I_ShutdownMusic(void)	{ I_MusicStop (); }
void I_SetMusicVolume(int volume)
{
    snd_MusicVolume = volume;
    I_MusicSetVolume (volume);
}
void I_PauseSong (int handle)	{ I_MusicPause (1); }
void I_ResumeSong (int handle)	{ I_MusicPause (0); }
int  I_RegisterSong(void* data)	{ return I_MusicRegister (data); }
void I_PlaySong(int handle, int looping) { I_MusicPlay (looping); }
void I_StopSong(int handle)	{ I_MusicStop (); }
void I_UnRegisterSong(int handle) { I_MusicStop (); }
