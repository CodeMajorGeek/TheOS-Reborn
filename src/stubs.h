#if defined(THEOS_RUNTIME)
#define TUNE_MAXVISPLANES 128
#define TUNE_MAXOPENINGS (SCREENWIDTH * 64)
#else
#define TUNE_MAXVISPLANES 32  // At 16, frequently causes visual glitches, but size is critical.
#define TUNE_MAXOPENINGS (SCREENWIDTH * 16)
#define STUB_NET
#endif

#ifndef SET_MEMORY_DEBUG
#define SET_MEMORY_DEBUG 1 // For printing free RAM.
#endif

#if !defined(THEOS_RUNTIME)
#define COMBINE_SCREENS // Overlap memory for screens #0-3 on constrained embedded targets.

// Disable screen wipes on constrained embedded targets.
#define DISABLE_WIPES
#endif

//We can create tables for things like the textures and maps... You can set the flags here to produce these.
//Don't do this on target hardware!!!

#ifndef FIXED_HEAP
#if defined(THEOS_RUNTIME)
#define FIXED_HEAP (64 * 1024 * 1024)
#elif defined(GENERATE_BAKED)
#define FIXED_HEAP 40000000 //Heap size when running on computer to store full size.
#else
#define FIXED_HEAP (384*1024)   //Actual heap for embedded device.
#endif
#endif


// Keep sound stubbed on constrained embedded targets, but enable real audio on TheOS runtime.
#if !defined(THEOS_RUNTIME)
#define STUB_SOUND
#endif



#ifdef STUB_SOUND
#define __S_SOUND__
#define S_StartSound( dev, sound )
#define S_ChangeMusic( musnum, id)
#define I_UpdateSound(x)
#define I_ShutdownSound(x)
#define I_ShutdownMusic(x)
#define S_PauseSound(x)
#define S_ResumeSound(x)
#define S_StartMusic(x)
#define S_SetMusicVolume(v)
#define S_UpdateSounds( listener )
#define S_SetSfxVolume(iv)
#define I_SubmitSound(vod)
#define I_InitSound()
#define S_Init(x,y)
#define S_Start(x)
#define  S_StopSound(x)
#endif


