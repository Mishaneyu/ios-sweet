/*
 * SPDX-License-Identifier: APSL-2.0
 *
 * /dev/hakios-audio: shared PCM & volume ioctl definitions between
 * bsd/dev/rmx1931_audio.c and phone/audio/audio_plugin.c (HakiosAudio.driver).
 * Adapted from HackiOS userland/src/hakios.audio.h (https://github.com/oosexgang/hackios-xnu-kernel).
 */
#ifndef _SWEET_HAKIOS_AUDIO_H_
#define _SWEET_HAKIOS_AUDIO_H_

#include <stdint.h>
#include <sys/ioccom.h>

#define HAKIOS_AUDIO_DEVICE    "/dev/hakios-audio"

/* 16-bit signed little-endian samples, left then right, at 48 kHz. */
#define HAKIOS_AUDIO_RATE      48000
#define HAKIOS_AUDIO_CHANNELS  2
#define HAKIOS_AUDIO_FRAME     4       /* bytes */

struct hakios_audio_volume {
	uint32_t below;                /* eighths of a dB below max */
	uint32_t muted;
};
#define HAKIOS_AUDIO_SET_VOLUME _IOW('r', 1, struct hakios_audio_volume)

#endif /* _SWEET_HAKIOS_AUDIO_H_ */
