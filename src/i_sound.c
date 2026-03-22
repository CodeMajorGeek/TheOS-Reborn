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
//	System interface for sound.
//
//-----------------------------------------------------------------------------

//static const char
//rcsid[] = "$Id: i_unix.c,v 1.5 1997/02/03 22:45:10 b1 Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>

#include <sys/time.h>
#include <sys/types.h>


#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

// Linux voxware output.
#include <linux/soundcard.h>

// Timer stuff. Experimental.
#include <time.h>
#include <signal.h>

#include "z_zone.h"

#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"

#include "doomdef.h"

#ifndef STUB_SOUND


#ifndef LINUX
#include <sys/filio.h>
#endif

// UNIX hack, to be removed.
#ifdef SNDSERV
// Separate sound server process.
FILE*	sndserver=0;
char*	sndserver_filename = "./sndserver ";
#elif SNDINTR

// Update all 30 millisecs, approx. 30fps synchronized.
// Linux resolution is allegedly 10 millisecs,
//  scale is microseconds.
#define SOUND_INTERVAL     500

// Get the interrupt. Set duration in millisecs.
int I_SoundSetTimer( int duration_of_tick );
void I_SoundDelTimer( void );
#else
// None?
#endif


// A quick hack to establish a protocol between
// synchronous mix buffer updates and asynchronous
// audio writes. Probably redundant with gametic.
static int flag = 0;

// The number of internal mixing channels,
//  the samples calculated for each mixing step,
//  the size of the 16bit, 2 hardware channel (stereo)
//  mixing buffer, and the samplerate of the raw data.


// Needed for calling the actual sound output.
#define SAMPLECOUNT		512
#define NUM_CHANNELS		8
// It is 2 for 16bit, and 2 for two channels.
#define BUFMUL                  4
#define MIXBUFFERSIZE		(SAMPLECOUNT*BUFMUL)

#define SAMPLERATE		11025	// Hz
#define SAMPLESIZE		2   	// 16bit
#define AUDIO_FRAGMENT_SHIFT      8
#define AUDIO_FRAGMENT_COUNT      4
#define SFX_MENU_VOLUME_MAX       15

// The actual lengths of all sound effects.
int 		lengths[NUMSFX];

// The actual output device.
int	audio_fd = -1;

// The global mixing buffer.
// Basically, samples from all active internal channels
//  are modifed and added, and stored in the buffer
//  that is submitted to the audio device.
signed short	mixbuffer[MIXBUFFERSIZE];


// The channel step amount...
unsigned int	channelstep[NUM_CHANNELS];
// ... and a 0.16 bit remainder of last step.
unsigned int	channelstepremainder[NUM_CHANNELS];


// The channel data pointers, start and end.
unsigned char*	channels[NUM_CHANNELS];
unsigned char*	channelsend[NUM_CHANNELS];


// Time/gametic that the channel started playing,
//  used to determine oldest, which automatically
//  has lowest priority.
// In case number of active sounds exceeds
//  available channels.
int		channelstart[NUM_CHANNELS];

// The sound in channel handles,
//  determined on registration,
//  might be used to unregister/stop/modify,
//  currently unused.
int 		channelhandles[NUM_CHANNELS];

// SFX id of the playing sound effect.
// Used to catch duplicates (like chainsaw).
int		channelids[NUM_CHANNELS];			

// Pitch to stepping lookup, unused.
int		steptable[256];

// Volume lookups.
int		vol_lookup[128*256];

// Hardware left and right channel volume lookup.
int*		channelleftvol_lookup[NUM_CHANNELS];
int*		channelrightvol_lookup[NUM_CHANNELS];




//
// Safe ioctl, convenience.
//
int
myioctl
( int	fd,
  int	command,
  int*	arg )
{   
    int		rc;
    
    rc = ioctl(fd, command, arg);  
    if (rc < 0)
    {
	fprintf(stderr, "ioctl(dsp,%d,arg) failed\n", command);
	fprintf(stderr, "errno=%d\n", errno);
	return -1;
    }
    return 0;
}





//
// This function loads the sound data from the WAD lump,
//  for single sound.
//
void*
getsfx
( char*         sfxname,
  int*          len )
{
    unsigned char*      sfx;
    unsigned char*      paddedsfx;
    int                 i;
    int                 size;
    int                 paddedsize;
    char                name[20];
    int                 sfxlump;

    
    // Get the sound data from the WAD, allocate lump
    //  in zone memory.
    sprintf(name, "ds%s", sfxname);

    // Now, there is a severe problem with the
    //  sound handling, in it is not (yet/anymore)
    //  gamemode aware. That means, sounds from
    //  DOOM II will be requested even with DOOM
    //  shareware.
    // The sound list is wired into sounds.c,
    //  which sets the external variable.
    // I do not do runtime patches to that
    //  variable. Instead, we will use a
    //  default sound for replacement.
    if ( W_CheckNumForName(name) == -1 )
      sfxlump = W_GetNumForName("dspistol");
    else
      sfxlump = W_GetNumForName(name);
    
    size = W_LumpLength( sfxlump );

    // Debug.
    // fprintf( stderr, "." );
    //fprintf( stderr, " -loading  %s (lump %d, %d bytes)\n",
    //	     sfxname, sfxlump, size );
    //fflush( stderr );
    
    sfx = (unsigned char*)W_CacheLumpNum( sfxlump, PU_STATIC );

    // Pads the sound effect out to the mixing buffer size.
    // The original realloc would interfere with zone memory.
    paddedsize = ((size-8 + (SAMPLECOUNT-1)) / SAMPLECOUNT) * SAMPLECOUNT;

    // Allocate from zone memory.
    paddedsfx = (unsigned char*)Z_Malloc( paddedsize+8, PU_STATIC, 0 );
    // ddt: (unsigned char *) realloc(sfx, paddedsize+8);
    // This should interfere with zone memory handling,
    //  which does not kick in in the soundserver.

    // Now copy and pad.
    memcpy(  paddedsfx, sfx, size );
    for (i=size ; i<paddedsize+8 ; i++)
        paddedsfx[i] = 128;

    // Remove the cached lump.
    //Z_Free( sfx );
    
    // Preserve padded length.
    *len = paddedsize;

    // Return allocated padded data.
    return (void *) (paddedsfx + 8);
}

static int I_ScaleSfxVolumeToMixer(int volume)
{
  int scaled;

  if (volume <= 0)
    return 0;

  /* Original game logic uses menu volume 0..15, while the mixer table is 0..127. */
  scaled = (volume * 127 + (SFX_MENU_VOLUME_MAX / 2)) / SFX_MENU_VOLUME_MAX;
  if (scaled > 127)
    scaled = 127;
  return scaled;
}

static int I_CompressAndClamp16(int sample)
{
  const int knee = 24576;
  const int maxv = 32767;
  const int minv = -32768;

  if (sample > knee)
  {
    sample = knee + ((sample - knee) >> 2);
    if (sample > maxv)
      sample = maxv;
    return sample;
  }

  if (sample < -knee)
  {
    sample = -knee + ((sample + knee) >> 2);
    if (sample < minv)
      sample = minv;
    return sample;
  }

  return sample;
}





//
// This function adds a sound to the
//  list of currently active sounds,
//  which is maintained as a given number
//  (eight, usually) of internal channels.
// Returns a handle.
//
int
addsfx
( int		sfxid,
  int		volume,
  int		step,
  int		seperation )
{
    static unsigned short	handlenums = 0;
 
    int		i;
    int		rc = -1;
    
    int		oldest = gametic;
    int		oldestnum = 0;
    int		slot;

    int		rightvol;
    int		leftvol;

    // Chainsaw troubles.
    // Play these sound effects only one at a time.
    if ( sfxid == sfx_sawup
	 || sfxid == sfx_sawidl
	 || sfxid == sfx_sawful
	 || sfxid == sfx_sawhit
	 || sfxid == sfx_stnmov
	 || sfxid == sfx_pistol	 )
    {
	// Loop all channels, check.
	for (i=0 ; i<NUM_CHANNELS ; i++)
	{
	    // Active, and using the same SFX?
	    if ( (channels[i])
		 && (channelids[i] == sfxid) )
	    {
		// Reset.
		channels[i] = 0;
		// We are sure that iff,
		//  there will only be one.
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

    // Tales from the cryptic.
    // If we found a channel, fine.
    // If not, we simply overwrite the first one, 0.
    // Probably only happens at startup.
    if (i == NUM_CHANNELS)
	slot = oldestnum;
    else
	slot = i;

    // Okay, in the less recent channel,
    //  we will handle the new SFX.
    // Set pointer to raw data.
    channels[slot] = (unsigned char *) S_sfx[sfxid].data;
    // Set pointer to end of raw data.
    channelsend[slot] = channels[slot] + lengths[sfxid];

    // Reset current handle number, limited to 0..100.
    if (!handlenums)
	handlenums = 100;

    // Assign current handle number.
    // Preserved so sounds could be stopped (unused).
    channelhandles[slot] = rc = handlenums++;

    // Set stepping???
    // Kinda getting the impression this is never used.
    channelstep[slot] = step;
    // ???
    channelstepremainder[slot] = 0;
    // Should be gametic, I presume.
    channelstart[slot] = gametic;

    // Separation, that is, orientation/stereo.
    //  range is: 1 - 256
    seperation += 1;

    // Per left/right channel.
    //  x^2 seperation,
    //  adjust volume properly.
    leftvol =
	volume - ((volume*seperation*seperation) >> 16); ///(256*256);
    seperation = seperation - 257;
    rightvol =
	volume - ((volume*seperation*seperation) >> 16);	

    // Sanity check, clamp volume.
    if (rightvol < 0 || rightvol > 127)
	I_Error("rightvol out of bounds");
    
    if (leftvol < 0 || leftvol > 127)
	I_Error("leftvol out of bounds");

    leftvol = I_ScaleSfxVolumeToMixer(leftvol);
    rightvol = I_ScaleSfxVolumeToMixer(rightvol);
    
    // Get the proper lookup table piece
    //  for this volume level???
    channelleftvol_lookup[slot] = &vol_lookup[leftvol*256];
    channelrightvol_lookup[slot] = &vol_lookup[rightvol*256];

    // Preserve sound SFX id,
    //  e.g. for avoiding duplicates of chainsaw.
    channelids[slot] = sfxid;

    // You tell me.
    return rc;
}





//
// SFX API
// Note: this was called by S_Init.
// However, whatever they did in the
// old DPMS based DOS version, this
// were simply dummies in the Linux
// version.
// See soundserver initdata().
//
void I_SetChannels()
{
  // Init internal lookups (raw data, mixing buffer, channels).
  // This function sets up internal lookups used during
  //  the mixing process. 
  int		i;
  int		j;
    
  int*	steptablemid = steptable + 128;
  
  // Okay, reset internal mixing channels to zero.
  /*for (i=0; i<NUM_CHANNELS; i++)
  {
    channels[i] = 0;
  }*/

  // This table provides step widths for pitch parameters.
  // I fail to see that this is currently used.
  for (i=-128 ; i<128 ; i++)
  {
    // Avoid libm dependency (pow) in TheOS userland build.
    // Keep a monotonic pitch curve around unity (65536) using a lightweight linear approximation.
    int approx = 65536 + (i * 1024);
    if (approx < 4096)
      approx = 4096;
    steptablemid[i] = approx;
  }
  
  
  // Generates volume lookup tables
  //  which also turn the unsigned samples
  //  into signed samples.
  for (i=0 ; i<128 ; i++)
    for (j=0 ; j<256 ; j++)
      vol_lookup[i*256+j] = (i*(j-128)*256)/127;
}	

 
void I_SetSfxVolume(int volume)
{
  // Identical to DOS.
  // Basically, this should propagate
  //  the menu/config file setting
  //  to the state variable used in
  //  the mixing.
  snd_SfxVolume = volume;
}

enum
{
  MUS_TICKS_PER_SECOND = 140,
  MUS_MAX_CHANNELS = 16,
  MUS_MAX_VOICES = 24,
  MUS_DEFAULT_CHANNEL_VOLUME = 127,
  MUS_DEFAULT_EXPRESSION = 127,
  MUS_DEFAULT_PAN = 64,
  MUS_DEFAULT_NOTE_VOLUME = 100,
  MUS_CONTROLLER_PROGRAM = 0,
  MUS_CONTROLLER_VOLUME = 3,
  MUS_CONTROLLER_PAN = 4,
  MUS_CONTROLLER_EXPRESSION = 5,
  MUS_CONTROLLER_ALL_SOUNDS_OFF = 10,
  MUS_CONTROLLER_ALL_NOTES_OFF = 11,
  MUS_EVENT_SCORE_END = 6,
  MUS_MIX_SCALE_DENOM = 65548256
};

struct mus_channel_state
{
  unsigned char volume;
  unsigned char expression;
  unsigned char pan;
  signed char bend;
  unsigned char last_note_volume;
};

struct mus_voice_state
{
  int active;
  unsigned char channel;
  unsigned char note;
  unsigned char note_volume;
  unsigned int phase;
  unsigned int base_step;
  unsigned int step;
};

struct mus_runtime_state
{
  int registered_handle;
  const unsigned char* song_data;
  const unsigned char* score;
  const unsigned char* cursor;
  const unsigned char* score_end;
  int looping;
  int playing;
  int paused;
  unsigned int samples_until_next_event;
  unsigned int voice_alloc_index;
  struct mus_channel_state channels[MUS_MAX_CHANNELS];
  struct mus_voice_state voices[MUS_MAX_VOICES];
};

static struct mus_runtime_state I_music_state;
static int I_music_log_once = 0;

/* Precomputed 32-bit phase increments at 11025 Hz for MIDI notes 0..127. */
static const unsigned int I_music_note_steps[128] = {
  3185015U, 3374406U, 3575058U, 3787642U, 4012867U, 4251485U, 4504291U, 4772130U,
  5055896U, 5356535U, 5675051U, 6012507U, 6370030U, 6748811U, 7150117U, 7575285U,
  8025735U, 8502970U, 9008582U, 9544261U, 10111792U, 10713070U, 11350103U, 12025015U,
  12740059U, 13497623U, 14300233U, 15150569U, 16051469U, 17005939U, 18017165U, 19088521U,
  20223584U, 21426141U, 22700205U, 24050030U, 25480119U, 26995246U, 28600467U, 30301139U,
  32102938U, 34011878U, 36034330U, 38177043U, 40447168U, 42852281U, 45400411U, 48100060U,
  50960238U, 53990491U, 57200933U, 60602278U, 64205876U, 68023757U, 72068660U, 76354085U,
  80894335U, 85704563U, 90800821U, 96200119U, 101920476U, 107980983U, 114401866U, 121204555U,
  128411753U, 136047513U, 144137319U, 152708170U, 161788671U, 171409126U, 181601643U, 192400238U,
  203840952U, 215961966U, 228803732U, 242409110U, 256823506U, 272095026U, 288274639U, 305416341U,
  323577341U, 342818251U, 363203285U, 384800477U, 407681904U, 431923931U, 457607465U, 484818220U,
  513647012U, 544190053U, 576549277U, 610832681U, 647154683U, 685636503U, 726406571U, 769600953U,
  815363807U, 863847862U, 915214929U, 969636441U, 1027294024U, 1088380105U, 1153098554U, 1221665363U,
  1294309365U, 1371273005U, 1452813141U, 1539201906U, 1630727614U, 1727695724U, 1830429858U, 1939272882U,
  2054588048U, 2176760211U, 2306197109U, 2443330725U, 2588618730U, 2742546010U, 2905626283U, 3078403812U,
  3261455229U, 3455391449U, 3660859716U, 3878545763U, 4109176096U, 4294967295U, 4294967295U, 4294967295U
};

static unsigned short I_MusicReadLE16(const unsigned char* ptr)
{
  return (unsigned short) ((unsigned short) ptr[0] |
                           ((unsigned short) ptr[1] << 8));
}

static void I_MusicResetVoices(void)
{
  int i;
  for (i = 0; i < MUS_MAX_VOICES; i++)
  {
    I_music_state.voices[i].active = 0;
    I_music_state.voices[i].phase = 0U;
    I_music_state.voices[i].base_step = 0U;
    I_music_state.voices[i].step = 0U;
  }
}

static void I_MusicResetChannels(void)
{
  int i;
  for (i = 0; i < MUS_MAX_CHANNELS; i++)
  {
    I_music_state.channels[i].volume = MUS_DEFAULT_CHANNEL_VOLUME;
    I_music_state.channels[i].expression = MUS_DEFAULT_EXPRESSION;
    I_music_state.channels[i].pan = MUS_DEFAULT_PAN;
    I_music_state.channels[i].bend = 0;
    I_music_state.channels[i].last_note_volume = MUS_DEFAULT_NOTE_VOLUME;
  }
}

static void I_MusicStopPlayback(void)
{
  I_music_state.playing = 0;
  I_music_state.paused = 0;
  I_music_state.samples_until_next_event = 0U;
  I_MusicResetVoices();
}

static unsigned int I_MusicApplyPitchToStep(unsigned int base_step, signed char bend)
{
  int adjusted = (int) base_step;
  adjusted += (adjusted * (int) bend) / 1024;
  if (adjusted < 1)
    adjusted = 1;
  return (unsigned int) adjusted;
}

static void I_MusicRefreshChannelPitch(unsigned char channel)
{
  int i;
  signed char bend;
  if (channel >= MUS_MAX_CHANNELS)
    return;

  bend = I_music_state.channels[channel].bend;
  for (i = 0; i < MUS_MAX_VOICES; i++)
  {
    if (I_music_state.voices[i].active &&
        I_music_state.voices[i].channel == channel)
    {
      I_music_state.voices[i].step =
        I_MusicApplyPitchToStep(I_music_state.voices[i].base_step, bend);
    }
  }
}

static void I_MusicNoteOff(unsigned char channel, unsigned char note)
{
  int i;
  for (i = 0; i < MUS_MAX_VOICES; i++)
  {
    if (I_music_state.voices[i].active &&
        I_music_state.voices[i].channel == channel &&
        I_music_state.voices[i].note == note)
    {
      I_music_state.voices[i].active = 0;
    }
  }
}

static void I_MusicAllNotesOff(unsigned char channel)
{
  int i;
  for (i = 0; i < MUS_MAX_VOICES; i++)
  {
    if (I_music_state.voices[i].active &&
        I_music_state.voices[i].channel == channel)
    {
      I_music_state.voices[i].active = 0;
    }
  }
}

static int I_MusicAllocateVoice(void)
{
  int i;
  int idx;

  for (i = 0; i < MUS_MAX_VOICES; i++)
  {
    idx = (int) ((I_music_state.voice_alloc_index + (unsigned int) i) % MUS_MAX_VOICES);
    if (!I_music_state.voices[idx].active)
    {
      I_music_state.voice_alloc_index = (unsigned int) ((idx + 1) % MUS_MAX_VOICES);
      return idx;
    }
  }

  idx = (int) (I_music_state.voice_alloc_index % MUS_MAX_VOICES);
  I_music_state.voice_alloc_index = (unsigned int) ((idx + 1) % MUS_MAX_VOICES);
  return idx;
}

static void I_MusicNoteOn(unsigned char channel, unsigned char note, unsigned char note_volume)
{
  int idx;
  unsigned int base_step;
  struct mus_voice_state* voice;

  if (channel >= MUS_MAX_CHANNELS || note >= 128U)
    return;

  I_MusicNoteOff(channel, note);
  idx = I_MusicAllocateVoice();
  voice = &I_music_state.voices[idx];

  base_step = I_music_note_steps[note];
  voice->active = 1;
  voice->channel = channel;
  voice->note = note;
  voice->note_volume = (unsigned char) (note_volume & 0x7FU);
  voice->phase = 0U;
  voice->base_step = base_step;
  voice->step = I_MusicApplyPitchToStep(base_step, I_music_state.channels[channel].bend);
}

static unsigned int I_MusicTicksToSamples(unsigned int ticks)
{
  unsigned long long samples;

  if (ticks == 0U)
    return 0U;

  samples = ((unsigned long long) ticks * (unsigned long long) SAMPLERATE +
             (unsigned long long) (MUS_TICKS_PER_SECOND - 1)) /
            (unsigned long long) MUS_TICKS_PER_SECOND;
  if (samples == 0ULL)
    samples = 1ULL;
  if (samples > 0xFFFFFFFFULL)
    samples = 0xFFFFFFFFULL;
  return (unsigned int) samples;
}

static int I_MusicReadByte(unsigned char* out_value)
{
  if (!out_value || !I_music_state.cursor || !I_music_state.score_end)
    return 0;
  if (I_music_state.cursor >= I_music_state.score_end)
    return 0;

  *out_value = *I_music_state.cursor++;
  return 1;
}

static unsigned int I_MusicReadDelayTicks(void)
{
  unsigned int ticks = 0U;
  unsigned char value = 0U;
  int loops = 0;

  do
  {
    if (!I_MusicReadByte(&value))
    {
      I_MusicStopPlayback();
      return 0U;
    }
    ticks = (ticks << 7) | (unsigned int) (value & 0x7FU);
    loops++;
  } while ((value & 0x80U) != 0U && loops < 5);

  return ticks;
}

static void I_MusicApplyController(unsigned char channel, unsigned char controller, unsigned char value)
{
  if (channel >= MUS_MAX_CHANNELS)
    return;

  switch (controller)
  {
    case MUS_CONTROLLER_PROGRAM:
      break;

    case MUS_CONTROLLER_VOLUME:
      I_music_state.channels[channel].volume = (unsigned char) (value & 0x7FU);
      break;

    case MUS_CONTROLLER_PAN:
      I_music_state.channels[channel].pan = (unsigned char) (value & 0x7FU);
      break;

    case MUS_CONTROLLER_EXPRESSION:
      I_music_state.channels[channel].expression = (unsigned char) (value & 0x7FU);
      break;

    case MUS_CONTROLLER_ALL_SOUNDS_OFF:
    case MUS_CONTROLLER_ALL_NOTES_OFF:
      I_MusicAllNotesOff(channel);
      break;

    default:
      break;
  }
}

static void I_MusicHandleSongEnd(void)
{
  if (!I_music_state.looping || !I_music_state.score)
  {
    I_MusicStopPlayback();
    return;
  }

  I_music_state.cursor = I_music_state.score;
  I_music_state.samples_until_next_event = 0U;
  I_MusicResetChannels();
  I_MusicResetVoices();
}

static void I_MusicConsumeEvents(void)
{
  int safety = 0;

  while (I_music_state.playing &&
         !I_music_state.paused &&
         I_music_state.samples_until_next_event == 0U)
  {
    unsigned char descriptor = 0U;
    unsigned char event_type = 0U;
    unsigned char channel = 0U;
    unsigned char is_last = 0U;
    unsigned char value = 0U;

    if (++safety > 16384)
    {
      I_MusicStopPlayback();
      return;
    }

    if (!I_MusicReadByte(&descriptor))
    {
      I_MusicHandleSongEnd();
      return;
    }

    event_type = (unsigned char) ((descriptor >> 4) & 0x07U);
    channel = (unsigned char) (descriptor & 0x0FU);
    is_last = (unsigned char) ((descriptor & 0x80U) != 0U);

    switch (event_type)
    {
      case 0:
      {
        if (!I_MusicReadByte(&value))
        {
          I_MusicStopPlayback();
          return;
        }
        I_MusicNoteOff(channel, (unsigned char) (value & 0x7FU));
        break;
      }

      case 1:
      {
        unsigned char note = 0U;
        unsigned char note_volume = MUS_DEFAULT_NOTE_VOLUME;

        if (!I_MusicReadByte(&value))
        {
          I_MusicStopPlayback();
          return;
        }
        note = (unsigned char) (value & 0x7FU);
        note_volume = I_music_state.channels[channel].last_note_volume;
        if ((value & 0x80U) != 0U)
        {
          if (!I_MusicReadByte(&note_volume))
          {
            I_MusicStopPlayback();
            return;
          }
          note_volume = (unsigned char) (note_volume & 0x7FU);
          I_music_state.channels[channel].last_note_volume = note_volume;
        }
        I_MusicNoteOn(channel, note, note_volume);
        break;
      }

      case 2:
      {
        if (!I_MusicReadByte(&value))
        {
          I_MusicStopPlayback();
          return;
        }
        I_music_state.channels[channel].bend = (signed char) ((int) value - 128);
        I_MusicRefreshChannelPitch(channel);
        break;
      }

      case 3:
      {
        if (!I_MusicReadByte(&value))
        {
          I_MusicStopPlayback();
          return;
        }
        if (value == MUS_CONTROLLER_ALL_SOUNDS_OFF ||
            value == MUS_CONTROLLER_ALL_NOTES_OFF)
        {
          I_MusicAllNotesOff(channel);
        }
        break;
      }

      case 4:
      {
        unsigned char controller = 0U;
        unsigned char ctrl_value = 0U;

        if (!I_MusicReadByte(&controller) || !I_MusicReadByte(&ctrl_value))
        {
          I_MusicStopPlayback();
          return;
        }
        I_MusicApplyController(channel, controller, ctrl_value);
        break;
      }

      case 5:
        /* MUS event type 5 is reserved; ignore gracefully. */
        break;

      case MUS_EVENT_SCORE_END:
        I_MusicHandleSongEnd();
        break;

      default:
        I_MusicStopPlayback();
        return;
    }

    if (!I_music_state.playing)
      return;

    if (is_last)
    {
      unsigned int delay_ticks = I_MusicReadDelayTicks();
      if (!I_music_state.playing)
        return;
      I_music_state.samples_until_next_event = I_MusicTicksToSamples(delay_ticks);
    }
  }
}

static void I_MusicRenderSample(int* out_left, int* out_right)
{
  int i;
  int left = 0;
  int right = 0;

  if (!out_left || !out_right)
    return;

  if (!I_music_state.playing ||
      I_music_state.paused ||
      snd_MusicVolume <= 0)
  {
    *out_left = 0;
    *out_right = 0;
    return;
  }

  if (I_music_state.samples_until_next_event == 0U)
    I_MusicConsumeEvents();

  if (!I_music_state.playing ||
      I_music_state.paused ||
      snd_MusicVolume <= 0)
  {
    *out_left = 0;
    *out_right = 0;
    return;
  }

  for (i = 0; i < MUS_MAX_VOICES; i++)
  {
    struct mus_voice_state* voice;
    struct mus_channel_state* channel;
    unsigned int wave_phase;
    int tri;
    long long scaled;
    int sample;
    int pan;

    voice = &I_music_state.voices[i];
    if (!voice->active)
      continue;

    channel = &I_music_state.channels[voice->channel];
    voice->phase += voice->step;
    wave_phase = (voice->phase >> 16) & 0xFFFFU;
    if (wave_phase < 32768U)
      tri = (int) wave_phase - 16384;
    else
      tri = 49152 - (int) wave_phase;

    scaled = (long long) tri *
             (long long) voice->note_volume *
             (long long) channel->volume *
             (long long) channel->expression *
             (long long) snd_MusicVolume;
    sample = (int) (scaled / (long long) MUS_MIX_SCALE_DENOM);

    pan = (int) channel->pan;
    left += (sample * (127 - pan)) / 127;
    right += (sample * pan) / 127;
  }

  if (I_music_state.samples_until_next_event > 0U)
    I_music_state.samples_until_next_event--;

  *out_left = left;
  *out_right = right;
}

static int I_MusicLoadSongData(const void* data)
{
  const unsigned char* song;
  unsigned short score_len;
  unsigned short score_start;

  if (!data)
  {
    if (!I_music_log_once)
    {
      I_music_log_once = 1;
      fprintf(stderr, "I_Music: register failed (null song data)\n");
    }
    return 0;
  }

  song = (const unsigned char*) data;
  if (song[0] != 'M' || song[1] != 'U' || song[2] != 'S' || song[3] != 0x1A)
  {
    if (!I_music_log_once)
    {
      I_music_log_once = 1;
      fprintf(stderr, "I_Music: unsupported lump format (%02X %02X %02X %02X)\n",
              (unsigned) song[0], (unsigned) song[1], (unsigned) song[2], (unsigned) song[3]);
    }
    return 0;
  }

  score_len = I_MusicReadLE16(song + 4);
  score_start = I_MusicReadLE16(song + 6);
  if (score_len == 0U)
    return 0;
  if ((unsigned int) score_start + (unsigned int) score_len < (unsigned int) score_start)
    return 0;

  I_music_state.song_data = song;
  I_music_state.score = song + score_start;
  I_music_state.score_end = I_music_state.score + score_len;
  I_music_state.cursor = I_music_state.score;
  I_music_state.samples_until_next_event = 0U;
  I_music_state.voice_alloc_index = 0U;
  I_MusicResetChannels();
  I_MusicResetVoices();
  fprintf(stderr, "I_Music: MUS song loaded (score=%u bytes)\n", (unsigned) score_len);
  return 1;
}

void I_SetMusicVolume(int volume)
{
  if (volume < 0)
    volume = 0;
  else if (volume > 15)
    volume = 15;
  snd_MusicVolume = volume;
}


//
// Retrieve the raw data lump index
//  for a given SFX name.
//
int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

//
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback)
//  is set, but currently not used by mixing.
//
int
I_StartSound
( int		id,
  int		vol,
  int		sep,
  int		pitch,
  int		priority )
{

  // UNUSED
  priority = 0;
  
#ifdef SNDSERV 
    if (sndserver)
    {
	fprintf(sndserver, "p%2.2x%2.2x%2.2x%2.2x\n", id, pitch, vol, sep);
	fflush(sndserver);
    }
    // warning: control reaches end of non-void function.
    return id;
#else
    // Debug.
    //fprintf( stderr, "starting sound %d", id );
    
    // Returns a handle (not used).
    id = addsfx( id, vol, steptable[pitch], sep );

    // fprintf( stderr, "/handle is %d\n", id );
    
    return id;
#endif
}



void I_StopSound (int handle)
{
  // You need the handle returned by StartSound.
  // Would be looping all channels,
  //  tracking down the handle,
  //  an setting the channel to zero.
  
  // UNUSED.
  handle = 0;
}


int I_SoundIsPlaying(int handle)
{
    // Ouch.
    return gametic < handle;
}




//
// This function loops all active (internal) sound
//  channels, retrieves a given number of samples
//  from the raw sound data, modifies it according
//  to the current (internal) channel parameters,
//  mixes the per channel samples into the global
//  mixbuffer, clamping it to the allowed range,
//  and sets up everything for transferring the
//  contents of the mixbuffer to the (two)
//  hardware channels (left and right, that is).
//
// This function currently supports only 16bit.
//
void I_UpdateSound( void )
{
#ifdef SNDINTR
  // Debug. Count buffer misses with interrupt.
  static int misses = 0;
#endif

  
  // Mix current sound data.
  // Data, from raw sound, for right and left.
  register unsigned int	sample;
  register int		dl;
  register int		dr;
  
  // Pointers in global mixbuffer, left, right, end.
  signed short*		leftout;
  signed short*		rightout;
  signed short*		leftend;
  // Step in mixbuffer, left and right, thus two.
  int				step;

  // Mixing channel index.
  int				chan;
  int                           music_left;
  int                           music_right;
    
    // Left and right channel
    //  are in global mixbuffer, alternating.
    leftout = mixbuffer;
    rightout = mixbuffer+1;
    step = 2;

    // Determine end, for left channel only
    //  (right channel is implicit).
    leftend = mixbuffer + SAMPLECOUNT*step;

    // Mix sounds into the mixing buffer.
    // Loop over step*SAMPLECOUNT,
    //  that is 512 values for two channels.
    while (leftout != leftend)
    {
	// Reset left/right value. 
	dl = 0;
	dr = 0;

	// Love thy L2 chache - made this a loop.
	// Now more channels could be set at compile time
	//  as well. Thus loop those  channels.
	for ( chan = 0; chan < NUM_CHANNELS; chan++ )
	{
	    // Check channel, if active.
	    if (channels[ chan ])
	    {
		// Get the raw data from the channel. 
		sample = *channels[ chan ];
		// Add left and right part
		//  for this channel (sound)
		//  to the current data.
		// Adjust volume accordingly.
		dl += channelleftvol_lookup[ chan ][sample];
		dr += channelrightvol_lookup[ chan ][sample];
		// Increment index ???
		channelstepremainder[ chan ] += channelstep[ chan ];
		// MSB is next sample???
		channels[ chan ] += channelstepremainder[ chan ] >> 16;
		// Limit to LSB???
		channelstepremainder[ chan ] &= 65536-1;

		// Check whether we are done.
		if (channels[ chan ] >= channelsend[ chan ])
		    channels[ chan ] = 0;
	    }
	}

        // Mix software music on top of SFX stream.
        I_MusicRenderSample(&music_left, &music_right);
        dl += music_left;
        dr += music_right;

        dl = I_CompressAndClamp16(dl);
        dr = I_CompressAndClamp16(dr);
	
	// Clamp to range. Left hardware channel.
	// Has been char instead of short.
	// if (dl > 127) *leftout = 127;
	// else if (dl < -128) *leftout = -128;
	// else *leftout = dl;

	if (dl > 0x7fff)
	    *leftout = 0x7fff;
	else if (dl < -0x8000)
	    *leftout = -0x8000;
	else
	    *leftout = dl;

	// Same for right hardware channel.
	if (dr > 0x7fff)
	    *rightout = 0x7fff;
	else if (dr < -0x8000)
	    *rightout = -0x8000;
	else
	    *rightout = dr;

	// Increment current pointers in mixbuffer.
	leftout += step;
	rightout += step;
    }

#ifdef SNDINTR
    // Debug check.
    if ( flag )
    {
      misses += flag;
      flag = 0;
    }
    
    if ( misses > 10 )
    {
      fprintf( stderr, "I_SoundUpdate: missed 10 buffer writes\n");
      misses = 0;
    }
    
    // Increment flag for update.
    flag++;
#endif
}


// 
// This would be used to write out the mixbuffer
//  during each game loop update.
// Updates sound buffer and audio device at runtime. 
// It is called during Timer interrupt with SNDINTR.
// Mixing now done synchronous, and
//  only output be done asynchronous?
//
void
I_SubmitSound(void)
{
  // Write it to DSP device.
  if (audio_fd >= 0)
    write(audio_fd, mixbuffer, SAMPLECOUNT*BUFMUL);
}



void
I_UpdateSoundParams
( int	handle,
  int	vol,
  int	sep,
  int	pitch)
{
  // I fail too see that this is used.
  // Would be using the handle to identify
  //  on which channel the sound might be active,
  //  and resetting the channel parameters.

  // UNUSED.
  handle = vol = sep = pitch = 0;
}




void I_ShutdownSound(void)
{    
#ifdef SNDSERV
  if (sndserver)
  {
    // Send a "quit" command.
    fprintf(sndserver, "q\n");
    fflush(sndserver);
  }
#else
  // Wait till all pending sounds are finished.
  int done = 0;
  int i;
  

  // FIXME (below).
  fprintf( stderr, "I_ShutdownSound: NOT finishing pending sounds\n");
  fflush( stderr );
  
  while ( !done )
  {
    for( i=0 ; i<8 && !channels[i] ; i++);
    
    // FIXME. No proper channel output.
    //if (i==8)
    done=1;
  }
#ifdef SNDINTR
  I_SoundDelTimer();
#endif
  
  // Cleaning up -releasing the DSP device.
  if (audio_fd >= 0)
    close ( audio_fd );
  audio_fd = -1;
#endif

  // Done.
  return;
}






void
I_InitSound()
{ 
  I_InitMusic();
#ifdef SNDSERV
  char buffer[256];
  
  if (getenv("DOOMWADDIR"))
    sprintf(buffer, "%s/%s",
	    getenv("DOOMWADDIR"),
	    sndserver_filename);
  else
    sprintf(buffer, "%s", sndserver_filename);
  
  // start sound process
  if ( !access(buffer, X_OK) )
  {
    strcat(buffer, " -quiet");
    sndserver = popen(buffer, "w");
  }
  else
    fprintf(stderr, "Could not start sound server [%s]\n", buffer);
#else
    
  int i;
  
#ifdef SNDINTR
  fprintf( stderr, "I_SoundSetTimer: %d microsecs\n", SOUND_INTERVAL );
  I_SoundSetTimer( SOUND_INTERVAL );
#endif
    
  // Secure and configure sound device first.
  fprintf( stderr, "I_InitSound: ");
  
  audio_fd = open("/dev/dsp", O_WRONLY);
  if (audio_fd<0)
  {
    fprintf(stderr, "Could not open /dev/dsp\n");
    return;
  }
  
  i = AUDIO_FRAGMENT_SHIFT | (AUDIO_FRAGMENT_COUNT << 16);
  (void) myioctl(audio_fd, SNDCTL_DSP_SETFRAGMENT, &i);
  (void) myioctl(audio_fd, SNDCTL_DSP_RESET, &i);
  
  i=SAMPLERATE;
  
  (void) myioctl(audio_fd, SNDCTL_DSP_SPEED, &i);
  
  i=1;
  (void) myioctl(audio_fd, SNDCTL_DSP_STEREO, &i);
  
  (void) myioctl(audio_fd, SNDCTL_DSP_GETFMTS, &i);
  
  if (i&=AFMT_S16_LE)
    (void) myioctl(audio_fd, SNDCTL_DSP_SETFMT, &i);
  else
    fprintf(stderr, "Could not play signed 16 data\n");
  fprintf(stderr, " configured audio device\n" );

    
  // Initialize external data (all sounds) at start, keep static.
  fprintf( stderr, "I_InitSound: ");
  
  for (i=1 ; i<NUMSFX ; i++)
  { 
    // Alias? Example is the chaingun sound linked to pistol.
    if (!S_sfx[i].link)
    {
      // Load data from WAD file.
      S_sfx[i].data = getsfx( S_sfx[i].name, &lengths[i] );
    }	
    else
    {
      // Previously loaded already?
      S_sfx[i].data = S_sfx[i].link->data;
      lengths[i] = lengths[(S_sfx[i].link - S_sfx)/sizeof(sfxinfo_t)];
    }
  }

  fprintf( stderr, " pre-cached all sound data\n");
  
  // Now initialize mixbuffer with zero.
  for ( i = 0; i< MIXBUFFERSIZE; i++ )
    mixbuffer[i] = 0;
  
  // Finished initialization.
  fprintf(stderr, "I_InitSound: sound module ready\n");
    
#endif
}




//
// MUSIC API.
// Software MUS playback mixed with SFX.
//
void I_InitMusic(void)
{
  I_music_state.registered_handle = 0;
  I_music_state.song_data = 0;
  I_music_state.score = 0;
  I_music_state.cursor = 0;
  I_music_state.score_end = 0;
  I_music_state.looping = 0;
  I_music_state.playing = 0;
  I_music_state.paused = 0;
  I_music_state.samples_until_next_event = 0U;
  I_music_state.voice_alloc_index = 0U;
  I_MusicResetChannels();
  I_MusicResetVoices();
}

void I_ShutdownMusic(void)
{
  I_MusicStopPlayback();
  I_music_state.song_data = 0;
  I_music_state.score = 0;
  I_music_state.cursor = 0;
  I_music_state.score_end = 0;
  I_music_state.registered_handle = 0;
}

void I_PlaySong(int handle, int looping)
{
  if (handle <= 0 || handle != I_music_state.registered_handle)
  {
    if (!I_music_log_once)
    {
      I_music_log_once = 1;
      fprintf(stderr, "I_Music: play ignored (invalid handle=%d)\n", handle);
    }
    return;
  }
  if (!I_music_state.song_data || !I_music_state.score || !I_music_state.score_end)
  {
    if (!I_music_log_once)
    {
      I_music_log_once = 1;
      fprintf(stderr, "I_Music: play ignored (song not loaded)\n");
    }
    return;
  }

  I_music_state.looping = (looping != 0);
  I_music_state.playing = 1;
  I_music_state.paused = 0;
  I_music_state.cursor = I_music_state.score;
  I_music_state.samples_until_next_event = 0U;
  I_music_state.voice_alloc_index = 0U;
  I_MusicResetChannels();
  I_MusicResetVoices();
  fprintf(stderr, "I_Music: playback start (loop=%d)\n", (looping != 0));
}

void I_PauseSong (int handle)
{
  if (handle <= 0 || handle != I_music_state.registered_handle)
    return;
  if (I_music_state.playing)
    I_music_state.paused = 1;
}

void I_ResumeSong (int handle)
{
  if (handle <= 0 || handle != I_music_state.registered_handle)
    return;
  if (I_music_state.playing)
    I_music_state.paused = 0;
}

void I_StopSong(int handle)
{
  if (handle > 0 && handle != I_music_state.registered_handle)
    return;

  I_MusicStopPlayback();
  I_music_state.looping = 0;
}

void I_UnRegisterSong(int handle)
{
  if (handle <= 0 || handle != I_music_state.registered_handle)
    return;

  I_MusicStopPlayback();
  I_music_state.song_data = 0;
  I_music_state.score = 0;
  I_music_state.cursor = 0;
  I_music_state.score_end = 0;
  I_music_state.registered_handle = 0;
}

int I_RegisterSong(void* data)
{
  static int next_music_handle = 1;

  if (!I_MusicLoadSongData(data))
    return 0;

  if (next_music_handle <= 0)
    next_music_handle = 1;

  I_music_state.registered_handle = next_music_handle++;
  I_music_state.playing = 0;
  I_music_state.paused = 0;
  I_music_state.looping = 0;
  return I_music_state.registered_handle;
}

// Is the song playing?
int I_QrySongPlaying(int handle)
{
  if (handle <= 0 || handle != I_music_state.registered_handle)
    return 0;
  return I_music_state.playing;
}

#ifdef SNDINTR
//
// Experimental stuff.
// A Linux timer interrupt, for asynchronous
//  sound output.
// I ripped this out of the Timer class in
//  our Difference Engine, including a few
//  SUN remains...
//  
#ifdef sun
    typedef     sigset_t        tSigSet;
#else    
    typedef     int             tSigSet;
#endif


// We might use SIGVTALRM and ITIMER_VIRTUAL, if the process
//  time independend timer happens to get lost due to heavy load.
// SIGALRM and ITIMER_REAL doesn't really work well.
// There are issues with profiling as well.
static int /*__itimer_which*/  itimer = ITIMER_REAL;

static int sig = SIGALRM;

// Interrupt handler.
void I_HandleSoundTimer( int ignore )
{
  // Debug.
  //fprintf( stderr, "%c", '+' ); fflush( stderr );
  
  // Feed sound device if necesary.
  if ( flag )
  {
    // See I_SubmitSound().
    // Write it to DSP device.
    if (audio_fd >= 0)
      write(audio_fd, mixbuffer, SAMPLECOUNT*BUFMUL);

    // Reset flag counter.
    flag = 0;
  }
  else
    return;
  
  // UNUSED, but required.
  ignore = 0;
  return;
}

// Get the interrupt. Set duration in millisecs.
int I_SoundSetTimer( int duration_of_tick )
{
  // Needed for gametick clockwork.
  struct itimerval    value;
  struct itimerval    ovalue;
  struct sigaction    act;
  struct sigaction    oact;

  int res;
  
  // This sets to SA_ONESHOT and SA_NOMASK, thus we can not use it.
  //     signal( _sig, handle_SIG_TICK );
  
  // Now we have to change this attribute for repeated calls.
  act.sa_handler = I_HandleSoundTimer;
#ifndef sun    
  //ac	t.sa_mask = _sig;
#endif
  act.sa_flags = SA_RESTART;
  
  sigaction( sig, &act, &oact );

  value.it_interval.tv_sec    = 0;
  value.it_interval.tv_usec   = duration_of_tick;
  value.it_value.tv_sec       = 0;
  value.it_value.tv_usec      = duration_of_tick;

  // Error is -1.
  res = setitimer( itimer, &value, &ovalue );

  // Debug.
  if ( res == -1 )
    fprintf( stderr, "I_SoundSetTimer: interrupt n.a.\n");
  
  return res;
}


// Remove the interrupt. Set duration to zero.
void I_SoundDelTimer()
{
  // Debug.
  if ( I_SoundSetTimer( 0 ) == -1)
    fprintf( stderr, "I_SoundDelTimer: failed to remove interrupt. Doh!\n");
}

#endif
#endif
