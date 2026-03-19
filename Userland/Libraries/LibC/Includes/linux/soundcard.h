#ifndef _LINUX_SOUNDCARD_H
#define _LINUX_SOUNDCARD_H

/*
 * Minimal OSS compatibility shim for ports expecting <linux/soundcard.h>.
 * The OS currently provides no audio backend; these constants are kept
 * Linux-compatible so source code can compile unchanged.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Audio sample formats. */
#ifndef AFMT_QUERY
#define AFMT_QUERY   0x00000000
#endif
#ifndef AFMT_U8
#define AFMT_U8      0x00000008
#endif
#ifndef AFMT_S16_LE
#define AFMT_S16_LE  0x00000010
#endif
#ifndef AFMT_S16_BE
#define AFMT_S16_BE  0x00000020
#endif

/* OSS DSP ioctls (Linux ABI values). */
#ifndef SNDCTL_DSP_RESET
#define SNDCTL_DSP_RESET        0x00005000UL
#endif
#ifndef SNDCTL_DSP_SYNC
#define SNDCTL_DSP_SYNC         0x00005001UL
#endif
#ifndef SNDCTL_DSP_SPEED
#define SNDCTL_DSP_SPEED        0xC0045002UL
#endif
#ifndef SNDCTL_DSP_STEREO
#define SNDCTL_DSP_STEREO       0xC0045003UL
#endif
#ifndef SNDCTL_DSP_SETFMT
#define SNDCTL_DSP_SETFMT       0xC0045005UL
#endif
#ifndef SNDCTL_DSP_SETFRAGMENT
#define SNDCTL_DSP_SETFRAGMENT  0xC004500AUL
#endif
#ifndef SNDCTL_DSP_GETFMTS
#define SNDCTL_DSP_GETFMTS      0x8004500BUL
#endif

#ifdef __cplusplus
}
#endif

#endif
