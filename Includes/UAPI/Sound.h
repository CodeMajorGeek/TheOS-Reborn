#ifndef _UAPI_SOUND_H
#define _UAPI_SOUND_H

/* OSS-compatible PCM format bits. */
#define AFMT_QUERY   0x00000000U
#define AFMT_U8      0x00000008U
#define AFMT_S16_LE  0x00000010U
#define AFMT_S16_BE  0x00000020U

/* OSS DSP ioctl numbers (Linux-compatible ABI values). */
#define SNDCTL_DSP_RESET        0x00005000UL
#define SNDCTL_DSP_SYNC         0x00005001UL
#define SNDCTL_DSP_SPEED        0xC0045002UL
#define SNDCTL_DSP_STEREO       0xC0045003UL
#define SNDCTL_DSP_SETFMT       0xC0045005UL
#define SNDCTL_DSP_SETFRAGMENT  0xC004500AUL
#define SNDCTL_DSP_GETFMTS      0x8004500BUL

#endif
