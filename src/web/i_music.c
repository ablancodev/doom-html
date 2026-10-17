//-----------------------------------------------------------------------------
//
// DOOM for the browser - OPL2 music.
//
// Plays the MUS lumps directly (no MIDI conversion) through Nuked-OPL3,
// a cycle-accurate emulation of the Yamaha OPL chips, driven the way the
// DMX sound library of DOOM v1.9 drove an AdLib / Sound Blaster:
// 9 voices, the GENMIDI instrument bank of the WAD, DMX's frequency and
// volume tables and its voice allocation rules.
//
// The DMX behaviour (tables, voice stealing, volume maths) follows the
// reverse engineering done by the Chocolate Doom project
// (src/i_oplmusic.c, GPL v2+, Copyright (C) 2005-2014 Simon Howard).
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "doomtype.h"
#include "w_wad.h"
#include "z_zone.h"

#include "opl3.h"
#include "i_music.h"

#define OPL_NUM_VOICES		9
#define OPL_NUM_OPERATORS	21

#define OPL_REGS_TREMOLO	0x20
#define OPL_REGS_LEVEL		0x40
#define OPL_REGS_ATTACK		0x60
#define OPL_REGS_SUSTAIN	0x80
#define OPL_REGS_WAVEFORM	0xE0
#define OPL_REGS_FREQ_1		0xA0
#define OPL_REGS_FREQ_2		0xB0
#define OPL_REGS_FEEDBACK	0xC0

#define GENMIDI_NUM_INSTRS	128
#define GENMIDI_NUM_PERCUSSION	47
#define GENMIDI_HEADER_LEN	8
#define GENMIDI_FLAG_FIXED	0x0001	// fixed pitch
#define GENMIDI_FLAG_2VOICE	0x0004	// double voice

#define MUS_CHANNELS		16
#define MUS_PERCUSSION		15
#define MUS_TICRATE		140

typedef struct
{
    byte	tremolo;
    byte	attack;
    byte	sustain;
    byte	waveform;
    byte	scale;
    byte	level;
} __attribute__((packed)) genmidi_op_t;

typedef struct
{
    genmidi_op_t modulator;
    byte	feedback;
    genmidi_op_t carrier;
    byte	unused;
    short	base_note_offset;
} __attribute__((packed)) genmidi_voice_t;

typedef struct
{
    unsigned short flags;
    byte	fine_tuning;
    byte	fixed_note;
    genmidi_voice_t voices[2];
} __attribute__((packed)) genmidi_instr_t;

typedef struct
{
    genmidi_instr_t* instrument;
    int		volume;
    int		volume_base;
    int		pan;
    int		bend;
    int		velocity;	// last MUS note volume
} channel_t;

typedef struct
{
    int		index;
    int		op1, op2;
    genmidi_instr_t* current_instr;
    unsigned int current_instr_voice;
    channel_t*	channel;
    unsigned int key;
    unsigned int note;
    unsigned int freq;
    unsigned int note_volume;
    unsigned int car_volume;
    unsigned int mod_volume;
    unsigned int reg_pan;
    unsigned int priority;
} voice_t;

static const int voice_operators[2][OPL_NUM_VOICES] = {
    { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x10, 0x11, 0x12 },
    { 0x03, 0x04, 0x05, 0x0b, 0x0c, 0x0d, 0x13, 0x14, 0x15 }
};

// DMX frequency values for each 1/32 semitone.
static const unsigned short frequency_curve[] = {
    0x133, 0x133, 0x134, 0x134, 0x135, 0x136, 0x136, 0x137,
    0x137, 0x138, 0x138, 0x139, 0x139, 0x13a, 0x13b, 0x13b,
    0x13c, 0x13c, 0x13d, 0x13d, 0x13e, 0x13f, 0x13f, 0x140,
    0x140, 0x141, 0x142, 0x142, 0x143, 0x143, 0x144, 0x144,
    0x145, 0x146, 0x146, 0x147, 0x147, 0x148, 0x149, 0x149,
    0x14a, 0x14a, 0x14b, 0x14c, 0x14c, 0x14d, 0x14d, 0x14e,
    0x14f, 0x14f, 0x150, 0x150, 0x151, 0x152, 0x152, 0x153,
    0x153, 0x154, 0x155, 0x155, 0x156, 0x157, 0x157, 0x158,
    0x158, 0x159, 0x15a, 0x15a, 0x15b, 0x15b, 0x15c, 0x15d,
    0x15d, 0x15e, 0x15f, 0x15f, 0x160, 0x161, 0x161, 0x162,
    0x162, 0x163, 0x164, 0x164, 0x165, 0x166, 0x166, 0x167,
    0x168, 0x168, 0x169, 0x16a, 0x16a, 0x16b, 0x16c, 0x16c,
    0x16d, 0x16e, 0x16e, 0x16f, 0x170, 0x170, 0x171, 0x172,
    0x172, 0x173, 0x174, 0x174, 0x175, 0x176, 0x176, 0x177,
    0x178, 0x178, 0x179, 0x17a, 0x17a, 0x17b, 0x17c, 0x17c,
    0x17d, 0x17e, 0x17e, 0x17f, 0x180, 0x181, 0x181, 0x182,
    0x183, 0x183, 0x184, 0x185, 0x185, 0x186, 0x187, 0x188,
    0x188, 0x189, 0x18a, 0x18a, 0x18b, 0x18c, 0x18d, 0x18d,
    0x18e, 0x18f, 0x18f, 0x190, 0x191, 0x192, 0x192, 0x193,
    0x194, 0x194, 0x195, 0x196, 0x197, 0x197, 0x198, 0x199,
    0x19a, 0x19a, 0x19b, 0x19c, 0x19d, 0x19d, 0x19e, 0x19f,
    0x1a0, 0x1a0, 0x1a1, 0x1a2, 0x1a3, 0x1a3, 0x1a4, 0x1a5,
    0x1a6, 0x1a6, 0x1a7, 0x1a8, 0x1a9, 0x1a9, 0x1aa, 0x1ab,
    0x1ac, 0x1ad, 0x1ad, 0x1ae, 0x1af, 0x1b0, 0x1b0, 0x1b1,
    0x1b2, 0x1b3, 0x1b4, 0x1b4, 0x1b5, 0x1b6, 0x1b7, 0x1b8,
    0x1b8, 0x1b9, 0x1ba, 0x1bb, 0x1bc, 0x1bc, 0x1bd, 0x1be,
    0x1bf, 0x1c0, 0x1c0, 0x1c1, 0x1c2, 0x1c3, 0x1c4, 0x1c4,
    0x1c5, 0x1c6, 0x1c7, 0x1c8, 0x1c9, 0x1c9, 0x1ca, 0x1cb,
    0x1cc, 0x1cd, 0x1ce, 0x1ce, 0x1cf, 0x1d0, 0x1d1, 0x1d2,
    0x1d3, 0x1d3, 0x1d4, 0x1d5, 0x1d6, 0x1d7, 0x1d8, 0x1d8,
    0x1d9, 0x1da, 0x1db, 0x1dc, 0x1dd, 0x1de, 0x1de, 0x1df,
    0x1e0, 0x1e1, 0x1e2, 0x1e3, 0x1e4, 0x1e5, 0x1e5, 0x1e6,
    0x1e7, 0x1e8, 0x1e9, 0x1ea, 0x1eb, 0x1ec, 0x1ed, 0x1ed,
    0x1ee, 0x1ef, 0x1f0, 0x1f1, 0x1f2, 0x1f3, 0x1f4, 0x1f5,
    0x1f6, 0x1f6, 0x1f7, 0x1f8, 0x1f9, 0x1fa, 0x1fb, 0x1fc,
    0x1fd, 0x1fe, 0x1ff, 0x200, 0x201, 0x201, 0x202, 0x203,
    // First note of looped range used for all octaves:
    0x204, 0x205, 0x206, 0x207, 0x208, 0x209, 0x20a, 0x20b,
    0x20c, 0x20d, 0x20e, 0x20f, 0x210, 0x210, 0x211, 0x212,
    0x213, 0x214, 0x215, 0x216, 0x217, 0x218, 0x219, 0x21a,
    0x21b, 0x21c, 0x21d, 0x21e, 0x21f, 0x220, 0x221, 0x222,
    0x223, 0x224, 0x225, 0x226, 0x227, 0x228, 0x229, 0x22a,
    0x22b, 0x22c, 0x22d, 0x22e, 0x22f, 0x230, 0x231, 0x232,
    0x233, 0x234, 0x235, 0x236, 0x237, 0x238, 0x239, 0x23a,
    0x23b, 0x23c, 0x23d, 0x23e, 0x23f, 0x240, 0x241, 0x242,
    0x244, 0x245, 0x246, 0x247, 0x248, 0x249, 0x24a, 0x24b,
    0x24c, 0x24d, 0x24e, 0x24f, 0x250, 0x251, 0x252, 0x253,
    0x254, 0x256, 0x257, 0x258, 0x259, 0x25a, 0x25b, 0x25c,
    0x25d, 0x25e, 0x25f, 0x260, 0x262, 0x263, 0x264, 0x265,
    0x266, 0x267, 0x268, 0x269, 0x26a, 0x26c, 0x26d, 0x26e,
    0x26f, 0x270, 0x271, 0x272, 0x273, 0x275, 0x276, 0x277,
    0x278, 0x279, 0x27a, 0x27b, 0x27d, 0x27e, 0x27f, 0x280,
    0x281, 0x282, 0x284, 0x285, 0x286, 0x287, 0x288, 0x289,
    0x28b, 0x28c, 0x28d, 0x28e, 0x28f, 0x290, 0x292, 0x293,
    0x294, 0x295, 0x296, 0x298, 0x299, 0x29a, 0x29b, 0x29c,
    0x29e, 0x29f, 0x2a0, 0x2a1, 0x2a2, 0x2a4, 0x2a5, 0x2a6,
    0x2a7, 0x2a9, 0x2aa, 0x2ab, 0x2ac, 0x2ae, 0x2af, 0x2b0,
    0x2b1, 0x2b2, 0x2b4, 0x2b5, 0x2b6, 0x2b7, 0x2b9, 0x2ba,
    0x2bb, 0x2bd, 0x2be, 0x2bf, 0x2c0, 0x2c2, 0x2c3, 0x2c4,
    0x2c5, 0x2c7, 0x2c8, 0x2c9, 0x2cb, 0x2cc, 0x2cd, 0x2ce,
    0x2d0, 0x2d1, 0x2d2, 0x2d4, 0x2d5, 0x2d6, 0x2d8, 0x2d9,
    0x2da, 0x2dc, 0x2dd, 0x2de, 0x2e0, 0x2e1, 0x2e2, 0x2e4,
    0x2e5, 0x2e6, 0x2e8, 0x2e9, 0x2ea, 0x2ec, 0x2ed, 0x2ee,
    0x2f0, 0x2f1, 0x2f2, 0x2f4, 0x2f5, 0x2f6, 0x2f8, 0x2f9,
    0x2fb, 0x2fc, 0x2fd, 0x2ff, 0x300, 0x302, 0x303, 0x304,
    0x306, 0x307, 0x309, 0x30a, 0x30b, 0x30d, 0x30e, 0x310,
    0x311, 0x312, 0x314, 0x315, 0x317, 0x318, 0x31a, 0x31b,
    0x31c, 0x31e, 0x31f, 0x321, 0x322, 0x324, 0x325, 0x327,
    0x328, 0x329, 0x32b, 0x32c, 0x32e, 0x32f, 0x331, 0x332,
    0x334, 0x335, 0x337, 0x338, 0x33a, 0x33b, 0x33d, 0x33e,
    0x340, 0x341, 0x343, 0x344, 0x346, 0x347, 0x349, 0x34a,
    0x34c, 0x34d, 0x34f, 0x350, 0x352, 0x353, 0x355, 0x357,
    0x358, 0x35a, 0x35b, 0x35d, 0x35e, 0x360, 0x361, 0x363,
    0x365, 0x366, 0x368, 0x369, 0x36b, 0x36c, 0x36e, 0x370,
    0x371, 0x373, 0x374, 0x376, 0x378, 0x379, 0x37b, 0x37c,
    0x37e, 0x380, 0x381, 0x383, 0x384, 0x386, 0x388, 0x389,
    0x38b, 0x38d, 0x38e, 0x390, 0x392, 0x393, 0x395, 0x397,
    0x398, 0x39a, 0x39c, 0x39d, 0x39f, 0x3a1, 0x3a2, 0x3a4,
    0x3a6, 0x3a7, 0x3a9, 0x3ab, 0x3ac, 0x3ae, 0x3b0, 0x3b1,
    0x3b3, 0x3b5, 0x3b7, 0x3b8, 0x3ba, 0x3bc, 0x3bd, 0x3bf,
    0x3c1, 0x3c3, 0x3c4, 0x3c6, 0x3c8, 0x3ca, 0x3cb, 0x3cd,
    // The last note has an incomplete range, and loops round back to
    // the start. The last value is a buffer overrun in DMX.
    0x3cf, 0x3d1, 0x3d2, 0x3d4, 0x3d6, 0x3d8, 0x3da, 0x3db,
    0x3dd, 0x3df, 0x3e1, 0x3e3, 0x3e4, 0x3e6, 0x3e8, 0x3ea,
    0x3ec, 0x3ed, 0x3ef, 0x3f1, 0x3f3, 0x3f5, 0x3f6, 0x3f8,
    0x3fa, 0x3fc, 0x3fe, 0x36c,
};

// Mapping from MIDI volume level to OPL level value.
static const unsigned int volume_mapping_table[] = {
    0, 1, 3, 5, 6, 8, 10, 11,
    13, 14, 16, 17, 19, 20, 22, 23,
    25, 26, 27, 29, 30, 32, 33, 34,
    36, 37, 39, 41, 43, 45, 47, 49,
    50, 52, 54, 55, 57, 59, 60, 61,
    63, 64, 66, 67, 68, 69, 71, 72,
    73, 74, 75, 76, 77, 79, 80, 81,
    82, 83, 84, 84, 85, 86, 87, 88,
    89, 90, 91, 92, 92, 93, 94, 95,
    96, 96, 97, 98, 99, 99, 100, 101,
    101, 102, 103, 103, 104, 105, 105, 106,
    107, 107, 108, 109, 109, 110, 110, 111,
    112, 112, 113, 113, 114, 114, 115, 115,
    116, 117, 117, 118, 118, 119, 119, 120,
    120, 121, 121, 122, 122, 123, 123, 123,
    124, 124, 125, 125, 126, 126, 127, 127
};

static opl3_chip	opl;
static boolean		initialized = false;

static genmidi_instr_t*	main_instrs;
static genmidi_instr_t*	percussion_instrs;

static voice_t		voices[OPL_NUM_VOICES];
static voice_t*		voice_free_list[OPL_NUM_VOICES];
static voice_t*		voice_alloced_list[OPL_NUM_VOICES];
static int		voice_free_num;
static int		voice_alloced_num;

static channel_t	channels[MUS_CHANNELS];

static int		start_music_volume;
static int		current_music_volume;

// The song.
static byte*		musdata;
static byte*		musscore;	// first event
static byte*		musend;
static byte*		muspos;
static boolean		playing;
static boolean		paused;
static boolean		looping;

// Timing: samples until the next MUS event is due (16.16 fixed point).
static int		outrate;
static long long	samplesleft;


static void OPL_WriteRegister (int reg, int value)
{
    OPL3_WriteReg (&opl, (uint16_t) reg, (uint8_t) value);
}

static void OPL_InitRegisters (void)
{
    int r;

    for (r=OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r)
	OPL_WriteRegister(r, 0x3f);

    // These two loops write to registers that don't exist,
    // but this is what Doom does.
    for (r=OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r)
	OPL_WriteRegister(r, 0x00);
    for (r=1; r < OPL_REGS_LEVEL; ++r)
	OPL_WriteRegister(r, 0x00);

    OPL_WriteRegister(0x04, 0x60);	// reset both timers
    OPL_WriteRegister(0x04, 0x80);	// enable interrupts
    OPL_WriteRegister(0x01, 0x20);	// allow waveform selection
}


//
// Voice allocation, as DMX v1.9 does it.
//
static voice_t* GetFreeVoice (void)
{
    voice_t*	result;
    int		i;

    if (voice_free_num == 0)
	return NULL;

    result = voice_free_list[0];
    voice_free_num--;
    for (i = 0; i < voice_free_num; i++)
	voice_free_list[i] = voice_free_list[i + 1];

    voice_alloced_list[voice_alloced_num++] = result;
    return result;
}

static void VoiceKeyOff (voice_t* voice)
{
    OPL_WriteRegister(OPL_REGS_FREQ_2 + voice->index, voice->freq >> 8);
}

static void ReleaseVoice (int index)
{
    voice_t*	voice;
    int		i;

    if (index >= voice_alloced_num)
    {
	voice_alloced_num = 0;
	voice_free_num = 0;
	return;
    }

    voice = voice_alloced_list[index];
    VoiceKeyOff(voice);
    voice->channel = NULL;
    voice->note = 0;

    voice_alloced_num--;
    for (i = index; i < voice_alloced_num; i++)
	voice_alloced_list[i] = voice_alloced_list[i + 1];

    // Search to the end of the freelist (this is how Doom behaves).
    voice_free_list[voice_free_num++] = voice;
}

static void LoadOperatorData (int op, genmidi_op_t* data,
			      boolean max_level, unsigned int* volume)
{
    int level;

    // The scale and level fields must be combined for the level register.
    // For the carrier wave we always set the maximum level.
    level = data->scale;
    if (max_level)
	level |= 0x3f;
    else
	level |= data->level;

    *volume = level;

    OPL_WriteRegister(OPL_REGS_LEVEL + op, level);
    OPL_WriteRegister(OPL_REGS_TREMOLO + op, data->tremolo);
    OPL_WriteRegister(OPL_REGS_ATTACK + op, data->attack);
    OPL_WriteRegister(OPL_REGS_SUSTAIN + op, data->sustain);
    OPL_WriteRegister(OPL_REGS_WAVEFORM + op, data->waveform);
}

static void SetVoiceInstrument (voice_t* voice, genmidi_instr_t* instr,
				unsigned int instr_voice)
{
    genmidi_voice_t*	data;
    unsigned int	modulating;

    if (voice->current_instr == instr
	&& voice->current_instr_voice == instr_voice)
	return;

    voice->current_instr = instr;
    voice->current_instr_voice = instr_voice;

    data = &instr->voices[instr_voice];

    // Are we using modulated feedback mode?
    modulating = (data->feedback & 0x01) == 0;

    // Doom loads the second operator first, then the first.
    LoadOperatorData(voice->op2, &data->carrier, true, &voice->car_volume);
    LoadOperatorData(voice->op1, &data->modulator, !modulating, &voice->mod_volume);

    OPL_WriteRegister(OPL_REGS_FEEDBACK + voice->index,
		      data->feedback | voice->reg_pan);

    voice->priority = 0x0f - (data->carrier.attack >> 4)
		    + 0x0f - (data->carrier.sustain & 0x0f);
}

static void SetVoiceVolume (voice_t* voice, unsigned int volume)
{
    genmidi_voice_t*	opl_voice;
    unsigned int	midi_volume;
    unsigned int	full_volume;
    unsigned int	car_volume;
    unsigned int	mod_volume;

    voice->note_volume = volume;

    opl_voice = &voice->current_instr->voices[voice->current_instr_voice];

    // Multiply note volume and channel volume to get the actual volume.
    midi_volume = 2 * (volume_mapping_table[voice->channel->volume] + 1);
    full_volume = (volume_mapping_table[voice->note_volume] * midi_volume) >> 9;

    car_volume = 0x3f - full_volume;

    if (car_volume != (voice->car_volume & 0x3f))
    {
	voice->car_volume = car_volume | (voice->car_volume & 0xc0);
	OPL_WriteRegister(OPL_REGS_LEVEL + voice->op2, voice->car_volume);

	// If we are using non-modulated feedback mode, we must set the
	// volume for both voices.
	if ((opl_voice->feedback & 0x01) != 0
	    && opl_voice->modulator.level != 0x3f)
	{
	    mod_volume = opl_voice->modulator.level;
	    if (mod_volume < car_volume)
		mod_volume = car_volume;

	    mod_volume |= voice->mod_volume & 0xc0;

	    if (mod_volume != voice->mod_volume)
	    {
		voice->mod_volume = mod_volume;
		OPL_WriteRegister(OPL_REGS_LEVEL + voice->op1,
				  mod_volume | (opl_voice->modulator.scale & 0xc0));
	    }
	}
    }
}

static void InitVoices (void)
{
    int i;

    voice_free_num = OPL_NUM_VOICES;
    voice_alloced_num = 0;

    for (i = 0; i < OPL_NUM_VOICES; ++i)
    {
	memset (&voices[i], 0, sizeof(voices[i]));
	voices[i].index = i;
	voices[i].op1 = voice_operators[0][i];
	voices[i].op2 = voice_operators[1][i];
	voices[i].current_instr = NULL;
	voice_free_list[i] = &voices[i];
    }
}

static unsigned int FrequencyForVoice (voice_t* voice)
{
    genmidi_voice_t*	gm_voice;
    int			freq_index;
    unsigned int	octave;
    unsigned int	sub_index;
    int			note;

    note = voice->note;

    // Apply note offset, unless it is a fixed note instrument.
    gm_voice = &voice->current_instr->voices[voice->current_instr_voice];
    if ((voice->current_instr->flags & GENMIDI_FLAG_FIXED) == 0)
	note += gm_voice->base_note_offset;

    while (note < 0)
	note += 12;
    while (note > 95)
	note -= 12;

    freq_index = 64 + 32 * note + voice->channel->bend;

    // The second voice of a double voice instrument is detuned
    // by the fine tuning field.
    if (voice->current_instr_voice != 0)
	freq_index += (voice->current_instr->fine_tuning / 2) - 64;

    if (freq_index < 0)
	freq_index = 0;

    // The first 7 notes use the start of the table, while
    // consecutive notes loop around the latter part.
    if (freq_index < 284)
	return frequency_curve[freq_index];

    sub_index = (freq_index - 284) % (12 * 32);
    octave = (freq_index - 284) / (12 * 32);
    if (octave >= 7)
	octave = 7;

    return frequency_curve[sub_index + 284] | (octave << 10);
}

static void UpdateVoiceFrequency (voice_t* voice)
{
    unsigned int freq = FrequencyForVoice(voice);

    if (voice->freq != freq)
    {
	OPL_WriteRegister(OPL_REGS_FREQ_1 + voice->index, freq & 0xff);
	OPL_WriteRegister(OPL_REGS_FREQ_2 + voice->index, (freq >> 8) | 0x20);
	voice->freq = freq;
    }
}

static void VoiceKeyOn (channel_t* channel, genmidi_instr_t* instrument,
			unsigned int instrument_voice, unsigned int note,
			unsigned int key, unsigned int volume)
{
    voice_t* voice = GetFreeVoice();

    if (voice == NULL)
	return;

    voice->channel = channel;
    voice->key = key;

    if ((instrument->flags & GENMIDI_FLAG_FIXED) != 0)
	voice->note = instrument->fixed_note;
    else
	voice->note = note;

    voice->reg_pan = channel->pan;

    SetVoiceInstrument(voice, instrument, instrument_voice);
    SetVoiceVolume(voice, volume);

    voice->freq = 0;
    UpdateVoiceFrequency(voice);
}

// When all voices are in use, discard one: second voices of double
// voice instruments first, then the highest numbered channel.
static void ReplaceExistingVoice (void)
{
    int i;
    int result = 0;

    for (i = 0; i < voice_alloced_num; i++)
    {
	if (voice_alloced_list[i]->current_instr_voice != 0
	    || voice_alloced_list[i]->channel
	       >= voice_alloced_list[result]->channel)
	    result = i;
    }

    ReleaseVoice(result);
}


//
// MUS events.
//
static void KeyOff (channel_t* channel, unsigned int key)
{
    int i;

    for (i = 0; i < voice_alloced_num; i++)
    {
	if (voice_alloced_list[i]->channel == channel
	    && voice_alloced_list[i]->key == key)
	{
	    ReleaseVoice(i);
	    i--;
	}
    }
}

static void KeyOn (int chnum, unsigned int key, unsigned int volume)
{
    genmidi_instr_t*	instrument;
    channel_t*		channel = &channels[chnum];
    unsigned int	note = key;
    boolean		double_voice;

    if (volume == 0)
    {
	KeyOff (channel, key);
	return;
    }

    if (chnum == MUS_PERCUSSION)
    {
	if (key < 35 || key > 81)
	    return;
	instrument = &percussion_instrs[key - 35];
	note = 60;
    }
    else
	instrument = channel->instrument;

    double_voice = (instrument->flags & GENMIDI_FLAG_2VOICE) != 0;

    if (voice_free_num == 0)
	ReplaceExistingVoice();

    VoiceKeyOn(channel, instrument, 0, note, key, volume);
    if (double_voice)
	VoiceKeyOn(channel, instrument, 1, note, key, volume);
}

static void SetChannelVolume (channel_t* channel, unsigned int volume,
			      boolean clip_start)
{
    int i;

    channel->volume_base = volume;

    if (volume > current_music_volume)
	volume = current_music_volume;
    if (clip_start && volume > start_music_volume)
	volume = start_music_volume;

    channel->volume = volume;

    for (i = 0; i < OPL_NUM_VOICES; ++i)
	if (voices[i].channel == channel)
	    SetVoiceVolume(&voices[i], voices[i].note_volume);
}

static void AllNotesOff (channel_t* channel)
{
    int i;

    for (i = 0; i < voice_alloced_num; i++)
    {
	if (voice_alloced_list[i]->channel == channel)
	{
	    ReleaseVoice(i);
	    i--;
	}
    }
}

static void PitchBend (channel_t* channel, int bend)
{
    voice_t*	updated[OPL_NUM_VOICES];
    voice_t*	notupdated[OPL_NUM_VOICES];
    int		nupdated = 0;
    int		nnotupdated = 0;
    int		i;

    channel->bend = bend;

    // Updated voices move to the end of the allocated list.
    for (i = 0; i < voice_alloced_num; ++i)
    {
	if (voice_alloced_list[i]->channel == channel)
	{
	    UpdateVoiceFrequency(voice_alloced_list[i]);
	    updated[nupdated++] = voice_alloced_list[i];
	}
	else
	    notupdated[nnotupdated++] = voice_alloced_list[i];
    }
    for (i = 0; i < nnotupdated; i++)
	voice_alloced_list[i] = notupdated[i];
    for (i = 0; i < nupdated; i++)
	voice_alloced_list[i + nnotupdated] = updated[i];
}

static void InitChannel (channel_t* channel)
{
    channel->instrument = &main_instrs[0];
    channel->volume = current_music_volume;
    channel->volume_base = 100;
    if (channel->volume > channel->volume_base)
	channel->volume = channel->volume_base;
    channel->pan = 0x30;
    channel->bend = 0;
    channel->velocity = 127;
}

static void RestartSong (void)
{
    int i;

    muspos = musscore;
    start_music_volume = current_music_volume;
    for (i = 0; i < MUS_CHANNELS; ++i)
	InitChannel(&channels[i]);
}

static void AllVoicesOff (void)
{
    while (voice_alloced_num > 0)
	ReleaseVoice(0);
}

// Process MUS events until a delay is found. Returns the delay in
// MUS tics, or -1 when the song is over.
static int ProcessEvents (void)
{
    byte	desc;
    int		type;
    int		chnum;
    int		value;
    int		delay;
    channel_t*	channel;

    for (;;)
    {
	if (muspos >= musend)
	    return -1;

	desc = *muspos++;
	type = (desc >> 4) & 7;
	chnum = desc & 15;
	channel = &channels[chnum];

	switch (type)
	{
	  case 0:	// release note
	    KeyOff (channel, *muspos++ & 0x7f);
	    break;

	  case 1:	// play note
	    value = *muspos++;
	    if (value & 0x80)
		channel->velocity = *muspos++ & 0x7f;
	    KeyOn (chnum, value & 0x7f, channel->velocity);
	    break;

	  case 2:	// pitch wheel: only the MSB of the MIDI bend matters
	    value = *muspos++;
	    PitchBend (channel, ((value * 64) >> 7) - 64);
	    break;

	  case 3:	// system event
	    value = *muspos++ & 0x7f;
	    if (value == 11)	// all notes off
		AllNotesOff (channel);
	    break;

	  case 4:	// change controller
	    value = *muspos++;	// controller number
	    if (value == 0)
	    {
		// Instrument change.
		channel->instrument = &main_instrs[*muspos++ & 0x7f];
	    }
	    else
	    {
		int param = *muspos++;
		if (param & 0x80)
		    param = 0x7f;
		if (value == 3)
		    SetChannelVolume (channel, param, true);
		// Pan only matters in OPL3 mode.
	    }
	    break;

	  case 5:	// measure end, no data
	    break;

	  case 6:	// score end
	    return -1;

	  default:	// unused event types: skip a byte
	    muspos++;
	    break;
	}

	// The last event of a group has the top bit set,
	// followed by a variable length delay.
	if (desc & 0x80)
	{
	    delay = 0;
	    do
	    {
		value = *muspos++;
		delay = (delay << 7) | (value & 0x7f);
	    } while ((value & 0x80) && muspos < musend);
	    if (delay > 0)
		return delay;
	}
    }
}


//
// Interface.
//
void I_MusicInit (int samplerate)
{
    int lump;

    outrate = samplerate;
    OPL3_Reset (&opl, samplerate);
    OPL_InitRegisters ();
    InitVoices ();

    lump = W_CheckNumForName ("GENMIDI");
    if (lump < 0)
	return;

    main_instrs = (genmidi_instr_t *)
	((byte *) W_CacheLumpNum (lump, PU_STATIC) + GENMIDI_HEADER_LEN);
    percussion_instrs = main_instrs + GENMIDI_NUM_INSTRS;

    current_music_volume = 64;
    initialized = true;
}

void I_MusicSetVolume (int volume)
{
    int i;

    volume *= 8;
    if (volume > 127)
	volume = 127;

    if (!initialized || current_music_volume == volume)
	return;

    current_music_volume = volume;

    for (i = 0; i < MUS_CHANNELS; ++i)
    {
	if (i == MUS_PERCUSSION)
	    SetChannelVolume(&channels[i], volume, false);
	else
	    SetChannelVolume(&channels[i], channels[i].volume_base, false);
    }
}

int I_MusicRegister (void* data)
{
    byte*	d = data;
    int		scorelen;
    int		scorestart;

    if (!initialized || memcmp (d, "MUS\x1a", 4))
    {
	musdata = NULL;
	return 0;
    }

    scorelen = d[4] | (d[5] << 8);
    scorestart = d[6] | (d[7] << 8);

    musdata = d;
    musscore = d + scorestart;
    musend = musscore + scorelen;
    return 1;
}

void I_MusicPlay (int loop)
{
    if (!musdata)
	return;

    AllVoicesOff ();
    looping = loop;
    RestartSong ();
    samplesleft = 0;
    playing = true;
    paused = false;
}

void I_MusicStop (void)
{
    if (!initialized)
	return;
    AllVoicesOff ();
    playing = false;
}

void I_MusicPause (int p)
{
    int i;

    paused = p;

    // Turn off all main instrument voices (not percussion),
    // like DMX does.
    if (p)
    {
	for (i = 0; i < OPL_NUM_VOICES; ++i)
	    if (voices[i].channel != NULL
		&& voices[i].current_instr < percussion_instrs)
		VoiceKeyOff(&voices[i]);
    }
}

void I_MusicRender (float* out, int frames, int samplerate)
{
    int16_t	buf[2];
    int		delay;

    if (!initialized)
	return;

    while (frames--)
    {
	// Run the sequencer: one MUS tic is samplerate/140 samples.
	while (playing && !paused && samplesleft <= 0)
	{
	    delay = ProcessEvents ();
	    if (delay < 0)
	    {
		if (looping)
		{
		    RestartSong ();
		    // Wait 5ms before restarting, as Chocolate Doom does,
		    // so an empty song can't lock the player up.
		    samplesleft += ((long long) samplerate << 16) / 200;
		}
		else
		{
		    AllVoicesOff ();
		    playing = false;
		}
	    }
	    else
		samplesleft += (((long long) samplerate << 16) * delay) / MUS_TICRATE;
	}
	samplesleft -= 1 << 16;

	OPL3_GenerateResampled (&opl, buf);
	*out++ += buf[0] * (1.0f/32768.0f);
	*out++ += buf[1] * (1.0f/32768.0f);
    }
}
