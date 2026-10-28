/*
 * SPDX-License-Identifier: APSL-2.0
 *
 * /dev/hakios-audio & /tmp/hot-audio.shm: shared PCM, volume & state ABI
 * between bsd/dev/rmx1931_audio.c, phone/audio/audio_plugin.c
 * (HotAudio.driver), and phone/adsp/adspd.
 * Adapted from HackiOS userland/src/hakios.audio.h (https://github.com/oosexgang/hackios-xnu-kernel).
 */
#ifndef _HOT_AUDIO_H_
#define _HOT_AUDIO_H_

#include <stdint.h>
#include <sys/ioccom.h>

#define HAKIOS_AUDIO_DEVICE         "/dev/hakios-audio"

/* 16-bit signed little-endian samples, left then right, at 48 kHz. */
#define HAKIOS_AUDIO_RATE           48000
#define HAKIOS_AUDIO_CHANNELS       2
#define HAKIOS_AUDIO_FRAME          4       /* bytes */

struct hakios_audio_volume {
	uint32_t below;                /* eighths of a dB below max */
	uint32_t muted;
};
#define HAKIOS_AUDIO_SET_VOLUME     _IOW('r', 1, struct hakios_audio_volume)

/*
 * Lockless shared-memory ring buffer between HotAudio.driver (mediaserverd)
 * and userland SM8150 LPASS ADSP daemon (/var/local/bin/adspd).
 */
#define HOT_AUDIO_SHM_PATH        "/tmp/hot-audio.shm"
#define HOT_AUDIO_SHM_MAGIC       0x48544155u /* 'HTAU' */
#define HOT_AUDIO_SHM_VERSION     1u
#define HOT_AUDIO_RING_FRAMES     32768u      /* ~682 ms stereo S16_LE @ 48 kHz */
#define HOT_AUDIO_MIC_RING_FRAMES 8192u       /* ~170 ms 4-ch S16_LE @ 48 kHz */

struct hot_audio_shm {
	uint32_t magic;             /* HOT_AUDIO_SHM_MAGIC */
	uint32_t version;           /* HOT_AUDIO_SHM_VERSION */
	uint32_t write_frames;      /* monotonic stereo frames written by HAL */
	uint32_t read_frames;       /* monotonic stereo frames consumed by adspd */
	uint32_t volume_q16;        /* 0..65536 linear gain (0 when muted) */
	uint32_t speaker_running;   /* 1 while Speaker IO is active */
	uint32_t baseband_voice_io; /* 1 while Baseband Voice IO is active */
	uint32_t codec_mic_source;  /* active selector ('mc01'..'mc04') */
	uint32_t mic_write_frames;  /* monotonic 4-ch frames written by adspd */
	uint32_t reserved[7];
	int16_t  pcm[HOT_AUDIO_RING_FRAMES * HAKIOS_AUDIO_CHANNELS];
	int16_t  mic_pcm[HOT_AUDIO_MIC_RING_FRAMES * 4];
};

#endif /* _HOT_AUDIO_H_ */
