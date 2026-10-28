/*
 * SPDX-License-Identifier: MIT
 *
 * sweet-audiotest.m: End-to-end CoreAudio -> mediaserverd -> HakiosAudio.driver
 * -> /tmp/sweet-audio.shm -> adspd -> Q6ASM -> MAX98927 stereo verification CLI.
 *
 * Usage:
 *   sweet-audiotest [duration_ms] [freq_left_hz] [freq_right_hz]
 */
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <objc/message.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../audio/hakios.audio.h"

#define NUM_AQ_BUFFERS  4
#define CHUNK_FRAMES    2048u

struct tone_ctx {
	double   freq_l;
	double   freq_r;
	uint64_t total_frames;
	uint64_t cursor;
	bool     finished;
};

static void
aq_output_cb(void *user_data, AudioQueueRef aq, AudioQueueBufferRef buf)
{
	struct tone_ctx *ctx = user_data;
	int16_t *pcm = buf->mAudioData;
	uint32_t cap_frames = buf->mAudioDataBytesCapacity / HAKIOS_AUDIO_FRAME;

	if (ctx->cursor >= ctx->total_frames) {
		memset(pcm, 0, buf->mAudioDataBytesCapacity);
		buf->mAudioDataByteSize = buf->mAudioDataBytesCapacity;
		if (!ctx->finished) {
			ctx->finished = true;
			AudioQueueStop(aq, false);
		}
		return;
	}

	uint64_t rem = ctx->total_frames - ctx->cursor;
	uint32_t n = rem < cap_frames ? (uint32_t)rem : cap_frames;
	for (uint32_t i = 0; i < n; i++) {
		uint64_t pos = ctx->cursor + i;
		double env = 1.0;
		if (pos < 960ULL) {
			env = 0.5 * (1.0 - cos(M_PI * (double)pos / 960.0));
		} else if (ctx->total_frames - pos < 2400ULL) {
			env = 0.5 * (1.0 - cos(M_PI * (double)(ctx->total_frames - pos) / 2400.0));
		}
		double t = (double)pos / (double)HAKIOS_AUDIO_RATE;
		pcm[i * 2]     = (int16_t)lrint(sin(2.0 * M_PI * ctx->freq_l * t) * 12000.0 * env);
		pcm[i * 2 + 1] = (int16_t)lrint(sin(2.0 * M_PI * ctx->freq_r * t) * 12000.0 * env);
	}
	if (n < cap_frames) {
		memset(&pcm[n * 2], 0, (size_t)(cap_frames - n) * HAKIOS_AUDIO_FRAME);
	}
	ctx->cursor += n;
	buf->mAudioDataByteSize = cap_frames * HAKIOS_AUDIO_FRAME;
	(void)AudioQueueEnqueueBuffer(aq, buf, 0, NULL);
}

static void
activate_playback_session_if_present(void)
{
	(void)dlopen("/System/Library/Frameworks/AVFAudio.framework/AVFAudio", RTLD_NOW | RTLD_GLOBAL);
	(void)dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_NOW | RTLD_GLOBAL);

	Class cls = NSClassFromString(@"AVAudioSession");
	SEL sel_shared = NSSelectorFromString(@"sharedInstance");
	SEL sel_cat_opts = NSSelectorFromString(@"setCategory:withOptions:error:");
	SEL sel_cat = NSSelectorFromString(@"setCategory:error:");
	SEL sel_act = NSSelectorFromString(@"setActive:error:");

	if (cls == nil || ![cls respondsToSelector:sel_shared]) {
		printf("  avsession: AVAudioSession class unavailable\n");
		return;
	}
	id sess = ((id (*)(id, SEL))objc_msgSend)((id)cls, sel_shared);
	if (sess == nil) {
		return;
	}
	NSError *err = nil;
	BOOL ok_cat = NO;
	if ([sess respondsToSelector:sel_cat_opts]) {
		/* AVAudioSessionCategoryOptionMixWithOthers = 1 allows non-foreground CLI playback */
		ok_cat = ((BOOL (*)(id, SEL, NSString *, NSUInteger, NSError **))objc_msgSend)(
		    sess, sel_cat_opts, @"AVAudioSessionCategoryPlayback", (NSUInteger)1, &err);
	} else if ([sess respondsToSelector:sel_cat]) {
		ok_cat = ((BOOL (*)(id, SEL, NSString *, NSError **))objc_msgSend)(
		    sess, sel_cat, @"AVAudioSessionCategoryPlayback", &err);
	}
	NSError *act_err = nil;
	BOOL ok_act = NO;
	if ([sess respondsToSelector:sel_act]) {
		ok_act = ((BOOL (*)(id, SEL, BOOL, NSError **))objc_msgSend)(
		    sess, sel_act, YES, &act_err);
	}
	printf("  avsession: cat=%d(%ld) active=%d(%ld)\n",
	    (int)ok_cat, err ? (long)err.code : 0L,
	    (int)ok_act, act_err ? (long)act_err.code : 0L);
}

static void
read_shm_snapshot(uint32_t *out_w, uint32_t *out_r, uint32_t *out_vol, bool *out_ok)
{
	int fd = open(SWEET_AUDIO_SHM_PATH, O_RDONLY | O_CLOEXEC);

	*out_w = *out_r = *out_vol = 0;
	*out_ok = false;
	if (fd < 0) {
		return;
	}
	struct sweet_audio_shm *s = mmap(NULL, sizeof(*s), PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (s == MAP_FAILED) {
		return;
	}
	if (s->magic == SWEET_AUDIO_SHM_MAGIC) {
		*out_w = __atomic_load_n(&s->write_frames, __ATOMIC_ACQUIRE);
		*out_r = __atomic_load_n(&s->read_frames, __ATOMIC_ACQUIRE);
		*out_vol = __atomic_load_n(&s->volume_q16, __ATOMIC_ACQUIRE);
		*out_ok = true;
	}
	munmap(s, sizeof(*s));
}

int
main(int argc, const char *argv[])
{
	@autoreleasepool {
		unsigned duration_ms = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 600u;
		double freq_l = argc > 2 ? strtod(argv[2], NULL) : 523.25;
		double freq_r = argc > 3 ? strtod(argv[3], NULL) : 659.25;

		if (duration_ms < 100u) duration_ms = 100u;
		if (duration_ms > 10000u) duration_ms = 10000u;

		activate_playback_session_if_present();

		uint32_t w0 = 0, r0 = 0, vol0 = 0;
		bool shm_ok0 = false;
		read_shm_snapshot(&w0, &r0, &vol0, &shm_ok0);
		printf("sweet-audiotest: %u ms stereo (L=%.2f Hz, R=%.2f Hz) @ 48 kHz S16_LE\n",
		    duration_ms, freq_l, freq_r);
		printf("  before: shm=%s write_frames=%u read_frames=%u vol_q16=%u\n",
		    shm_ok0 ? "attached" : "missing", w0, r0, vol0);

		AudioStreamBasicDescription fmt = {
			.mSampleRate       = HAKIOS_AUDIO_RATE,
			.mFormatID         = kAudioFormatLinearPCM,
			.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
			.mBytesPerPacket   = HAKIOS_AUDIO_FRAME,
			.mFramesPerPacket  = 1,
			.mBytesPerFrame    = HAKIOS_AUDIO_FRAME,
			.mChannelsPerFrame = HAKIOS_AUDIO_CHANNELS,
			.mBitsPerChannel   = 16,
		};
		struct tone_ctx ctx = {
			.freq_l       = freq_l,
			.freq_r       = freq_r,
			.total_frames = ((uint64_t)duration_ms * HAKIOS_AUDIO_RATE) / 1000ULL,
			.cursor       = 0,
			.finished     = false,
		};

		AudioQueueRef aq = NULL;
		OSStatus st = AudioQueueNewOutput(&fmt, aq_output_cb, &ctx,
		    CFRunLoopGetCurrent(), kCFRunLoopCommonModes, 0, &aq);
		if (st != noErr || aq == NULL) {
			fprintf(stderr, "sweet-audiotest: AudioQueueNewOutput failed (OSStatus=%d)\n", (int)st);
			return 1;
		}
		(void)AudioQueueSetParameter(aq, kAudioQueueParam_Volume, 1.0f);

		AudioQueueBufferRef bufs[NUM_AQ_BUFFERS];
		for (int i = 0; i < NUM_AQ_BUFFERS; i++) {
			st = AudioQueueAllocateBuffer(aq, CHUNK_FRAMES * HAKIOS_AUDIO_FRAME, &bufs[i]);
			if (st != noErr) {
				fprintf(stderr, "sweet-audiotest: AudioQueueAllocateBuffer failed (%d)\n", (int)st);
				AudioQueueDispose(aq, true);
				return 1;
			}
			aq_output_cb(&ctx, aq, bufs[i]);
		}

		st = AudioQueueStart(aq, NULL);
		if (st != noErr) {
			printf("  AudioQueueStart returned OSStatus=%d (headless CLI fallback -> AudioServicesPlaySystemSound)\n",
			    (int)st);
			AudioQueueDispose(aq, true);
			AudioServicesPlaySystemSound(1007);
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.75, false);
		} else {
			NSTimeInterval deadline = [NSDate timeIntervalSinceReferenceDate] +
			    ((double)duration_ms / 1000.0) + 1.0;
			while (!ctx.finished && [NSDate timeIntervalSinceReferenceDate] < deadline) {
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, false);
			}
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.15, false);
			AudioQueueStop(aq, true);
			AudioQueueDispose(aq, true);
		}

		uint32_t w1 = 0, r1 = 0, vol1 = 0;
		bool shm_ok1 = false;
		read_shm_snapshot(&w1, &r1, &vol1, &shm_ok1);
		printf("  after:  shm=%s write_frames=%u (+%u) read_frames=%u (+%u) vol_q16=%u\n",
		    shm_ok1 ? "attached" : "missing", w1, w1 - w0, r1, r1 - r0, vol1);

		NSString *statusJson = [NSString stringWithContentsOfFile:@"/var/local/adsp-status.json"
		    encoding:NSUTF8StringEncoding error:nil];
		if (statusJson != nil) {
			printf("=== /var/local/adsp-status.json ===\n%s", [statusJson UTF8String]);
		}
		return 0;
	}
}
