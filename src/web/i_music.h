//-----------------------------------------------------------------------------
//
// DOOM for the browser - OPL2 music (MUS played through an emulated
// Yamaha YM3812, the way DMX drove an AdLib / Sound Blaster on DOS).
//
//-----------------------------------------------------------------------------

#ifndef __I_MUSIC__
#define __I_MUSIC__

void I_MusicInit (int samplerate);
void I_MusicSetVolume (int volume);	// 0..15, as the menu slider
int  I_MusicRegister (void *musdata);
void I_MusicPlay (int looping);
void I_MusicStop (void);
void I_MusicPause (int paused);

// Adds the music for the next 'frames' stereo frames into 'out'.
void I_MusicRender (float *out, int frames, int samplerate);

#endif
