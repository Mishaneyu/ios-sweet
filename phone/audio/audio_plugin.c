/*
 * SPDX-License-Identifier: APSL-2.0
 *
 * HakiosAudio.driver: CoreAudio HAL AudioServerPlugIn for iOS 16 & iOS 26
 * (D22AP / D421AP) on RMX1931 / SWEET.
 * Adapted from HackiOS userland/src/audio_plugin.c (https://github.com/oosexgang/hackios-xnu-kernel).
 *
 * Presents the five built-in devices expected by D421AP's VirtualAudio.plugin
 * routing tables:
 *   - "Speaker"        stereo 48 kHz output -> /tmp/sweet-audio.shm (adspd) +
 *                      /dev/hakios-audio (+ HAKIOS_AUDIO_SET_VOLUME ioctl),
 *                      plus silent stereo reference input
 *   - "Hawking"        silent mono input required by 'vhaw' companion route
 *   - "Actuator"       mono 48 kHz Taptic Engine waveform -> /dev/hakios-haptic
 *   - "Codec"          4-ch 48 kHz built-in microphone ('mc01'..'mc04') ->
 *                      /tmp/sweet-audio.shm mic_pcm or /dev/hakios-mic
 *   - "Baseband Voice" mono 48 kHz cellular call route (signals adspd MVM/CVS/CVP)
 */
#include <CoreFoundation/CoreFoundation.h>
#if __has_include(<CoreAudioTypes/CoreAudioTypes.h>)
#include <CoreAudioTypes/CoreAudioTypes.h>
#else
#include <CoreAudio/CoreAudioTypes.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hakios.audio.h"

/* CFPlugIn COM types (omitted from iPhoneOS.sdk headers on newer Xcode). */
typedef SInt32 HRESULT;
typedef UInt32 ULONG;
typedef void *LPVOID;
typedef CFUUIDBytes REFIID;
#ifndef S_OK
#define S_OK            ((HRESULT)0x00000000L)
#endif
#ifndef E_NOINTERFACE
#define E_NOINTERFACE   ((HRESULT)0x80004002L)
#endif
#ifndef IUnknownUUID
#define IUnknownUUID CFUUIDGetConstantUUIDWithBytes(NULL, \
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
	0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46)
#endif

/* ---- AudioServerPlugIn interface (macOS CoreAudio/AudioServerPlugIn.h) ---- */

typedef UInt32 AudioObjectID;
typedef struct {
	UInt32 mSelector, mScope, mElement;
} AudioObjectPropertyAddress;
typedef struct {
	AudioStreamBasicDescription mFormat;
	AudioValueRange mSampleRateRange;
} AudioStreamRangedDescription;
typedef struct {
	UInt32 mClientID;
	pid_t mProcessID;
	Boolean mIsNativeEndian;
	CFStringRef mBundleID;
} AudioServerPlugInClientInfo;
typedef struct {
	UInt64 mIOCycleCounter;
	UInt32 mNominalIOBufferFrameSize;
	AudioTimeStamp mCurrentTime, mInputTime, mOutputTime;
	Float64 mMasterHostTicksPerFrame, mDeviceHostTicksPerFrame;
} AudioServerPlugInIOCycleInfo;

typedef struct AudioServerPlugInHostInterface AudioServerPlugInHostInterface;
typedef const AudioServerPlugInHostInterface *AudioServerPlugInHostRef;
struct AudioServerPlugInHostInterface {
	OSStatus (*PropertiesChanged)(AudioServerPlugInHostRef, AudioObjectID, UInt32, const AudioObjectPropertyAddress *);
	OSStatus (*CopyFromStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef *);
	OSStatus (*WriteToStorage)(AudioServerPlugInHostRef, CFStringRef, CFPropertyListRef);
	OSStatus (*DeleteFromStorage)(AudioServerPlugInHostRef, CFStringRef);
	OSStatus (*RequestDeviceConfigurationChange)(AudioServerPlugInHostRef, AudioObjectID, UInt64, void *);
};

typedef struct AudioServerPlugInDriverInterface AudioServerPlugInDriverInterface;
typedef AudioServerPlugInDriverInterface **AudioServerPlugInDriverRef;
struct AudioServerPlugInDriverInterface {
	void *_reserved;
	HRESULT (*QueryInterface)(void *, REFIID, LPVOID *);
	ULONG (*AddRef)(void *);
	ULONG (*Release)(void *);
	OSStatus (*Initialize)(AudioServerPlugInDriverRef, AudioServerPlugInHostRef);
	OSStatus (*CreateDevice)(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
	OSStatus (*DestroyDevice)(AudioServerPlugInDriverRef, AudioObjectID);
	OSStatus (*AddDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*RemoveDeviceClient)(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
	OSStatus (*PerformDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	OSStatus (*AbortDeviceConfigurationChange)(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
	Boolean (*HasProperty)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
	OSStatus (*IsPropertySettable)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
	OSStatus (*GetPropertyDataSize)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32 *);
	OSStatus (*GetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, UInt32 *, void *);
	OSStatus (*SetPropertyData)(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32,
	    const void *, UInt32, const void *);
	OSStatus (*StartIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*StopIO)(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
	OSStatus (*GetZeroTimeStamp)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
	OSStatus (*WillDoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
	OSStatus (*BeginIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
	OSStatus (*DoIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *, void *, void *);
	OSStatus (*EndIOOperation)(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32,
	    const AudioServerPlugInIOCycleInfo *);
};

#define kAudioServerPlugInTypeUUID CFUUIDGetConstantUUIDWithBytes(NULL, \
	0x44, 0x3A, 0xBA, 0xB8, 0xE7, 0xB3, 0x49, 0x1A, \
	0xB9, 0x85, 0xBE, 0xB9, 0x18, 0x70, 0x30, 0xDB)
#define kAudioServerPlugInDriverInterfaceUUID CFUUIDGetConstantUUIDWithBytes(NULL, \
	0xEE, 0xA5, 0x77, 0x3D, 0xCC, 0x43, 0x49, 0xF1, \
	0x8E, 0x00, 0x8F, 0x96, 0xE7, 0xD2, 0x3B, 0x17)

enum {
	kAudioHardwareUnknownPropertyError = 'who?', kAudioHardwareBadPropertySizeError = '!siz',
	kAudioHardwareIllegalOperationError = 'nope', kAudioHardwareBadObjectError = '!obj',
	kAudioHardwareUnsupportedOperationError = 'unop', kAudioDeviceUnsupportedFormatError = '!dat',

	kAudioObjectPropertyScopeGlobal = 'glob', kAudioObjectPropertyScopeInput = 'inpt', kAudioObjectPropertyScopeOutput = 'outp',

	kAudioObjectClassID = 'aobj', kAudioPlugInClassID = 'aplg', kAudioDeviceClassID = 'adev', kAudioStreamClassID = 'astr',

	kAudioObjectPropertyBaseClass = 'bcls', kAudioObjectPropertyClass = 'clas', kAudioObjectPropertyOwner = 'stdv',
	kAudioObjectPropertyName = 'lnam', kAudioObjectPropertyManufacturer = 'lmak', kAudioObjectPropertyOwnedObjects = 'ownd',

	kAudioPlugInPropertyBoxList = 'box#', kAudioPlugInPropertyTranslateUIDToBox = 'uidb',
	kAudioPlugInPropertyDeviceList = 'dev#', kAudioPlugInPropertyTranslateUIDToDevice = 'uidd',
	kAudioPlugInPropertyResourceBundle = 'rsrc',

	kAudioDevicePropertyDeviceUID = 'uid ', kAudioDevicePropertyModelUID = 'muid', kAudioDevicePropertyTransportType = 'tran',
	kAudioDevicePropertyRelatedDevices = 'akin', kAudioDevicePropertyClockDomain = 'clkd',
	kAudioDevicePropertyDeviceIsAlive = 'livn', kAudioDevicePropertyDeviceIsRunning = 'goin',
	kAudioDevicePropertyDeviceCanBeDefaultDevice = 'dflt', kAudioDevicePropertyDeviceCanBeDefaultSystemDevice = 'sflt',
	kAudioDevicePropertyLatency = 'ltnc', kAudioDevicePropertyStreams = 'stm#', kAudioObjectPropertyControlList = 'ctrl',
	kAudioDevicePropertySafetyOffset = 'saft', kAudioDevicePropertyNominalSampleRate = 'nsrt',
	kAudioDevicePropertyAvailableNominalSampleRates = 'nsr#', kAudioDevicePropertyIsHidden = 'hidn',
	kAudioDevicePropertyPreferredChannelsForStereo = 'dch2', kAudioDevicePropertyZeroTimeStampPeriod = 'ring',
	kAudioSelectorControlClassID = 'slct', kAudioDataSourceControlClassID = 'dsrc',
	kAudioSelectorControlPropertyCurrentItem = 'scci', kAudioSelectorControlPropertyAvailableItems = 'scai',
	kAudioSelectorControlPropertyItemName = 'scin', kAudioSelectorControlPropertyItemKind = 'clkk',

	kAudioStreamPropertyIsActive = 'sact', kAudioStreamPropertyDirection = 'sdir', kAudioStreamPropertyTerminalType = 'term',
	kAudioStreamPropertyStartingChannel = 'schn', kAudioStreamPropertyLatency = 'ltnc',
	kAudioStreamPropertyVirtualFormat = 'sfmt', kAudioStreamPropertyAvailableVirtualFormats = 'sfma',
	kAudioStreamPropertyPhysicalFormat = 'pft ', kAudioStreamPropertyAvailablePhysicalFormats = 'pfta',

	kAudioStreamTerminalTypeSpeaker = 'spkr', kAudioStreamTerminalTypeMicrophone = 'micr',

	kAudioServerPlugInIOOperationReadInput = 'read', kAudioServerPlugInIOOperationWriteMix = 'rite',

	kAudioLevelControlClassID = 'levl', kAudioVolumeControlClassID = 'vlme',
	kAudioControlPropertyScope = 'cscp', kAudioControlPropertyElement = 'celm',
	kAudioLevelControlPropertyScalarValue = 'lcsv', kAudioLevelControlPropertyDecibelValue = 'lcdv',
	kAudioLevelControlPropertyDecibelRange = 'lcdr', kAudioLevelControlPropertyConvertScalarToDecibels = 'lcsd',
	kAudioLevelControlPropertyConvertDecibelsToScalar = 'lcds',
};

/* ---- Devices ---- */

enum {
	OBJECT_PLUGIN = 1,
	OBJECT_SPEAKER, OBJECT_SPEAKER_OUTPUT, OBJECT_SPEAKER_INPUT,
	OBJECT_HAWKING, OBJECT_HAWKING_INPUT,
	OBJECT_ACTUATOR, OBJECT_ACTUATOR_OUTPUT,
	OBJECT_CODEC, OBJECT_CODEC_INPUT,
	OBJECT_SPEAKER_VOLUME,
	OBJECT_CODEC_SOURCE,
	OBJECT_BASEBAND_VOICE, OBJECT_BASEBAND_VOICE_OUTPUT, OBJECT_BASEBAND_VOICE_INPUT,
	OBJECT_LAST = OBJECT_BASEBAND_VOICE_INPUT
};

#define VOLUME_RANGE_DB   48.0
#define CODEC_CHANNELS    4

struct device {
	AudioObjectID object;
	CFStringRef uid;
	AudioObjectID output, input;
	UInt32 output_channels, input_channels;
	unsigned running;
	UInt64 anchor;
	UInt64 periods;
};

static struct device devices[] = {
	{ .object = OBJECT_SPEAKER, .uid = CFSTR("Speaker"), .output = OBJECT_SPEAKER_OUTPUT,
	  .input = OBJECT_SPEAKER_INPUT, .output_channels = HAKIOS_AUDIO_CHANNELS, .input_channels = 2 },
	{ .object = OBJECT_HAWKING, .uid = CFSTR("Hawking"), .input = OBJECT_HAWKING_INPUT, .input_channels = 1 },
	{ .object = OBJECT_ACTUATOR, .uid = CFSTR("Actuator"), .output = OBJECT_ACTUATOR_OUTPUT, .output_channels = 1 },
	{ .object = OBJECT_CODEC, .uid = CFSTR("Codec"), .input = OBJECT_CODEC_INPUT, .input_channels = CODEC_CHANNELS },
	{ .object = OBJECT_BASEBAND_VOICE, .uid = CFSTR("Baseband Voice"),
	  .output = OBJECT_BASEBAND_VOICE_OUTPUT, .input = OBJECT_BASEBAND_VOICE_INPUT,
	  .output_channels = 1, .input_channels = 1 },
};
#define DEVICES             (sizeof(devices) / sizeof(devices[0]))

#define TRANSPORT_BUILT_IN  'bltn'
#define MIC_SOURCE_LEGACY   'imic'
#define MIC_SOURCE_1        'mc01'
#define MIC_SOURCE_2        'mc02'
#define MIC_SOURCE_3        'mc03'
#define MIC_SOURCE_4        'mc04'
#define MANUFACTURER        CFSTR("sweet")
#define TIMESTAMP_PERIOD    16384
#define VALUE_MAX_WORDS     16

static AudioServerPlugInHostRef host;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int sink = -1;
static unsigned sink_open_tries;
static struct sweet_audio_shm *shm_ptr;
static UInt64 shm_last_try;
static UInt64 retry_interval_ticks;
static Float64 ticks_per_frame;
static UInt64 written, refused;
static Boolean inactive[OBJECT_LAST + 1];
static Float32 volume = 1.0f;
static UInt32 mic_source = MIC_SOURCE_1;
static uint32_t mic_read_pos;

static void
say(const char *format, ...)
{
	char line[256];
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
	}
	(void)write(2, line, (size_t)n);
}

static Float32
scalar_to_db(Float32 scalar)
{
	scalar = scalar < 0 ? 0 : scalar > 1 ? 1 : scalar;
	return (Float32)(-VOLUME_RANGE_DB * (1.0 - scalar));
}

static Float32
db_to_scalar(Float32 db)
{
	Float32 scalar = (Float32)(1.0 + db / VOLUME_RANGE_DB);
	return scalar < 0 ? 0 : scalar > 1 ? 1 : scalar;
}

static uint32_t
volume_to_q16(Float32 scalar)
{
	Float32 db;
	long q16;

	if (scalar <= 0.0f) {
		return 0;
	}
	if (scalar >= 1.0f) {
		return 65536u;
	}
	db = scalar_to_db(scalar);
	q16 = lroundf(powf(10.0f, db / 20.0f) * 65536.0f);
	if (q16 < 0) {
		return 0;
	}
	if (q16 > 65536L) {
		return 65536u;
	}
	return (uint32_t)q16;
}

static struct sweet_audio_shm *
ensure_shm(void)
{
	struct sweet_audio_shm *mapped;
	struct stat st;
	UInt64 now;
	int fd;

	mapped = __atomic_load_n(&shm_ptr, __ATOMIC_ACQUIRE);
	if (mapped != NULL) {
		if (__atomic_load_n(&mapped->magic, __ATOMIC_ACQUIRE) == SWEET_AUDIO_SHM_MAGIC &&
		    mapped->version == SWEET_AUDIO_SHM_VERSION) {
			return mapped;
		}
		return NULL;
	}

	now = mach_absolute_time();
	if (shm_last_try != 0 && now - shm_last_try < retry_interval_ticks) {
		return NULL;
	}
	shm_last_try = now;

	fd = open(SWEET_AUDIO_SHM_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		return NULL;
	}
	if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(struct sweet_audio_shm)) {
		close(fd);
		return NULL;
	}
	mapped = mmap(NULL, sizeof(struct sweet_audio_shm), PROT_READ | PROT_WRITE,
	    MAP_SHARED, fd, 0);
	close(fd);
	if (mapped == MAP_FAILED) {
		return NULL;
	}
	if (__atomic_load_n(&mapped->magic, __ATOMIC_ACQUIRE) != SWEET_AUDIO_SHM_MAGIC ||
	    mapped->version != SWEET_AUDIO_SHM_VERSION) {
		munmap(mapped, sizeof(struct sweet_audio_shm));
		return NULL;
	}

	__atomic_store_n(&mapped->volume_q16, volume_to_q16(volume), __ATOMIC_RELEASE);
	__atomic_store_n(&mapped->speaker_running, devices[0].running != 0 ? 1u : 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&mapped->baseband_voice_io, devices[4].running != 0 ? 1u : 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&mapped->codec_mic_source, mic_source, __ATOMIC_RELEASE);
	__atomic_store_n(&shm_ptr, mapped, __ATOMIC_RELEASE);
	say("sweet: audio: attached %s (vol_q16=%u)\n", SWEET_AUDIO_SHM_PATH,
	    (unsigned)volume_to_q16(volume));
	return mapped;
}

static void
shm_write_speaker(const int16_t *samples, UInt32 frames)
{
	struct sweet_audio_shm *s = ensure_shm();
	uint32_t w, r, mask, i;

	if (s == NULL || samples == NULL || frames == 0) {
		return;
	}
	mask = SWEET_AUDIO_RING_FRAMES - 1u;
	w = __atomic_load_n(&s->write_frames, __ATOMIC_RELAXED);
	r = __atomic_load_n(&s->read_frames, __ATOMIC_ACQUIRE);
	if (w - r > SWEET_AUDIO_RING_FRAMES) {
		w = r;
	}
	if (frames > SWEET_AUDIO_RING_FRAMES) {
		samples += (size_t)(frames - SWEET_AUDIO_RING_FRAMES) * HAKIOS_AUDIO_CHANNELS;
		frames = SWEET_AUDIO_RING_FRAMES;
	}
	for (i = 0; i < frames; i++) {
		uint32_t idx = ((w + i) & mask) * HAKIOS_AUDIO_CHANNELS;
		s->pcm[idx] = samples[i * 2];
		s->pcm[idx + 1] = samples[i * 2 + 1];
	}
	__atomic_store_n(&s->write_frames, w + frames, __ATOMIC_RELEASE);
}

static int
ensure_sink_fd(void)
{
	if (sink >= 0) {
		return sink;
	}
	if (sink_open_tries < 16) {
		sink_open_tries++;
		sink = open(HAKIOS_AUDIO_DEVICE, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	}
	return sink;
}

static void
volume_to_kernel(void)
{
	struct hakios_audio_volume v = { (uint32_t)lround(-scalar_to_db(volume) * 8), volume <= 0 };
	struct sweet_audio_shm *s = ensure_shm();
	uint32_t q16 = volume_to_q16(volume);
	int fd = ensure_sink_fd();

	if (s != NULL) {
		__atomic_store_n(&s->volume_q16, q16, __ATOMIC_RELEASE);
	}
	if (fd >= 0 && ioctl(fd, HAKIOS_AUDIO_SET_VOLUME, &v) == 0) {
		say("sweet: audio: volume %.3f (%s-%.1f dB, q16=%u)\n",
		    volume, v.muted ? "muted, " : "", v.below / 8.0, (unsigned)q16);
		return;
	}
	say("sweet: audio: volume %.3f (%s-%.1f dB, q16=%u, shm=%s)\n",
	    volume, v.muted ? "muted, " : "", v.below / 8.0, (unsigned)q16,
	    s != NULL ? "ok" : "pending");
}

static struct device *
device_of(AudioObjectID object)
{
	for (unsigned i = 0; i < DEVICES; i++) {
		if (object == devices[i].object ||
		    (object != 0 && (object == devices[i].output || object == devices[i].input))) {
			return &devices[i];
		}
	}
	return NULL;
}

static AudioStreamBasicDescription
format_of(AudioObjectID stream)
{
	const struct device *d = device_of(stream);
	UInt32 channels = stream == d->output ? d->output_channels : d->input_channels;

	return (AudioStreamBasicDescription) {
		.mSampleRate = HAKIOS_AUDIO_RATE,
		.mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked,
		.mBytesPerPacket = 2 * channels,
		.mFramesPerPacket = 1,
		.mBytesPerFrame = 2 * channels,
		.mChannelsPerFrame = channels,
		.mBitsPerChannel = 16,
	};
}

static bool
valid_mic_source(UInt32 item)
{
	return item == MIC_SOURCE_1 || item == MIC_SOURCE_2 ||
	    item == MIC_SOURCE_3 || item == MIC_SOURCE_4 ||
	    item == MIC_SOURCE_LEGACY;
}

static HRESULT
query_interface(void *drv, REFIID uuid, LPVOID *iface)
{
	CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, uuid);
	Boolean ours = CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID);

	CFRelease(requested);
	if (iface == NULL || !ours) {
		return E_NOINTERFACE;
	}
	*iface = drv;
	return S_OK;
}

static ULONG
add_ref(__unused void *drv)
{
	return 1;
}

static ULONG
release(__unused void *drv)
{
	return 1;
}

static OSStatus
initialize(__unused AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef the_host)
{
	mach_timebase_info_data_t timebase;

	host = the_host;
	mach_timebase_info(&timebase);
	ticks_per_frame = 1e9 * timebase.denom / timebase.numer / HAKIOS_AUDIO_RATE;
	retry_interval_ticks = (UInt64)(0.25e9 * timebase.denom / timebase.numer);
	sink = open(HAKIOS_AUDIO_DEVICE, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	sink_open_tries = 1;
	(void)ensure_shm();
	say("sweet: audio: HakiosAudio initialized (D421AP 5-device HAL, %s: %s, shm: %s)\n",
	    HAKIOS_AUDIO_DEVICE, sink < 0 ? strerror(errno) : "open",
	    shm_ptr != NULL ? "attached" : "waiting");
	return 0;
}

static OSStatus
create_device(__unused AudioServerPlugInDriverRef drv, __unused CFDictionaryRef description,
    __unused const AudioServerPlugInClientInfo *client, __unused AudioObjectID *object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
destroy_device(__unused AudioServerPlugInDriverRef drv, __unused AudioObjectID object)
{
	return kAudioHardwareUnsupportedOperationError;
}

static OSStatus
device_client(__unused AudioServerPlugInDriverRef drv, __unused AudioObjectID object,
    __unused const AudioServerPlugInClientInfo *client)
{
	return 0;
}

static OSStatus
configuration_change(__unused AudioServerPlugInDriverRef drv, __unused AudioObjectID object,
    __unused UInt64 action, __unused void *info)
{
	return 0;
}

struct value {
	UInt32 size;
	bool empty;
	bool string;
	union {
		UInt32 words[VALUE_MAX_WORDS];
		Float64 number;
		Float32 decimal;
		CFStringRef string;
		AudioValueRange range;
		AudioStreamBasicDescription format;
		AudioStreamRangedDescription ranged;
	} u;
};

static struct value
property(AudioObjectID object, const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier)
{
	UInt32 selector = address->mSelector, scope = address->mScope;
	const struct device *d = device_of(object);
	struct value v;

	memset(&v, 0, sizeof(v));
#define WORD(x)   (v.u.words[0] = (x), v.size = sizeof(UInt32))
#define LIST(x)   do { \
	if (v.size / sizeof(UInt32) < VALUE_MAX_WORDS) { \
		v.u.words[v.size / sizeof(UInt32)] = (x); \
		v.size += sizeof(UInt32); \
	} \
} while (0)
#define STRING(x) (v.u.string = (x), v.string = true, v.size = sizeof(CFStringRef))
#define FLOAT(x)  (v.u.decimal = (x), v.size = sizeof(Float32))
	if (object == OBJECT_SPEAKER_VOLUME) {
		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(kAudioLevelControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(kAudioVolumeControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_SPEAKER);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeOutput);
			break;
		case kAudioControlPropertyElement:
			WORD(0);
			break;
		case kAudioLevelControlPropertyScalarValue:
			FLOAT(volume);
			break;
		case kAudioLevelControlPropertyDecibelValue:
			FLOAT(scalar_to_db(volume));
			break;
		case kAudioLevelControlPropertyDecibelRange:
			v.u.range.mMinimum = -VOLUME_RANGE_DB;
			v.u.range.mMaximum = 0;
			v.size = sizeof(AudioValueRange);
			break;
		case kAudioLevelControlPropertyConvertScalarToDecibels:
		case kAudioLevelControlPropertyConvertDecibelsToScalar:
			FLOAT(0);
			break;
		}
		return v;
	}
	if (object == OBJECT_CODEC_SOURCE) {
		switch (selector) {
		case kAudioObjectPropertyBaseClass:
			WORD(kAudioSelectorControlClassID);
			break;
		case kAudioObjectPropertyClass:
			WORD(kAudioDataSourceControlClassID);
			break;
		case kAudioObjectPropertyOwner:
			WORD(OBJECT_CODEC);
			break;
		case kAudioObjectPropertyOwnedObjects:
			v.empty = true;
			break;
		case kAudioControlPropertyScope:
			WORD(kAudioObjectPropertyScopeInput);
			break;
		case kAudioControlPropertyElement:
			WORD(0);
			break;
		case kAudioSelectorControlPropertyCurrentItem:
			WORD(mic_source);
			break;
		case kAudioSelectorControlPropertyAvailableItems:
			LIST(MIC_SOURCE_1);
			LIST(MIC_SOURCE_2);
			LIST(MIC_SOURCE_3);
			LIST(MIC_SOURCE_4);
			break;
		case kAudioSelectorControlPropertyItemName:
			if (qualifier_size == sizeof(UInt32) && qualifier != NULL) {
				switch (*(const UInt32 *)qualifier) {
				case MIC_SOURCE_1:
				case MIC_SOURCE_LEGACY:
					STRING(CFSTR("Bottom"));
					break;
				case MIC_SOURCE_2:
					STRING(CFSTR("Front"));
					break;
				case MIC_SOURCE_3:
					STRING(CFSTR("Back"));
					break;
				case MIC_SOURCE_4:
					STRING(CFSTR("Bottom 2"));
					break;
				default:
					STRING(CFSTR(""));
					break;
				}
			} else {
				STRING(CFSTR(""));
			}
			break;
		}
		return v;
	}
	if (object != OBJECT_PLUGIN && d == NULL) {
		return v;
	}
	switch (selector) {
	case kAudioObjectPropertyBaseClass:
		WORD(kAudioObjectClassID);
		return v;
	case kAudioObjectPropertyClass:
		WORD(object == OBJECT_PLUGIN ? kAudioPlugInClassID :
		     object == d->object ? kAudioDeviceClassID : kAudioStreamClassID);
		return v;
	case kAudioObjectPropertyOwner:
		WORD(object == OBJECT_PLUGIN ? 0 : object == d->object ? OBJECT_PLUGIN : d->object);
		return v;
	}
	if (object == OBJECT_PLUGIN) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer:
			STRING(MANUFACTURER);
			break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioPlugInPropertyDeviceList:
			for (unsigned i = 0; i < DEVICES; i++) {
				LIST(devices[i].object);
			}
			break;
		case kAudioPlugInPropertyBoxList:
			v.empty = true;
			break;
		case kAudioPlugInPropertyTranslateUIDToBox:
			WORD(0);
			break;
		case kAudioPlugInPropertyTranslateUIDToDevice:
			WORD(0);
			for (unsigned i = 0; i < DEVICES; i++) {
				if (qualifier_size == sizeof(CFStringRef) && qualifier != NULL &&
				    *(const CFStringRef *)qualifier != NULL &&
				    CFEqual(*(const CFStringRef *)qualifier, devices[i].uid)) {
					WORD(devices[i].object);
				}
			}
			break;
		case kAudioPlugInPropertyResourceBundle:
			STRING(CFSTR(""));
			break;
		}
	} else if (object == d->object) {
		switch (selector) {
		case kAudioObjectPropertyManufacturer:
			STRING(MANUFACTURER);
			break;
		case kAudioObjectPropertyName:
		case kAudioDevicePropertyDeviceUID:
		case kAudioDevicePropertyModelUID:
			STRING(d->uid);
			break;
		case kAudioDevicePropertyTransportType:
			WORD(TRANSPORT_BUILT_IN);
			break;
		case kAudioDevicePropertyRelatedDevices:
			WORD(d->object);
			break;
		case kAudioDevicePropertyClockDomain:
		case kAudioDevicePropertyLatency:
		case kAudioDevicePropertySafetyOffset:
		case kAudioDevicePropertyIsHidden:
			WORD(0);
			break;
		case kAudioDevicePropertyDeviceIsAlive:
			WORD(1);
			break;
		case kAudioDevicePropertyDeviceCanBeDefaultDevice:
		case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
			WORD(d->object == OBJECT_SPEAKER ? 1 : 0);
			break;
		case kAudioDevicePropertyDeviceIsRunning:
			WORD(d->running != 0);
			break;
		case kAudioObjectPropertyOwnedObjects:
		case kAudioDevicePropertyStreams:
			if (scope != kAudioObjectPropertyScopeInput && d->output != 0) {
				LIST(d->output);
			}
			if (scope != kAudioObjectPropertyScopeOutput && d->input != 0) {
				LIST(d->input);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_SPEAKER) {
				LIST(OBJECT_SPEAKER_VOLUME);
			}
			if (selector == kAudioObjectPropertyOwnedObjects && d->object == OBJECT_CODEC &&
			    scope != kAudioObjectPropertyScopeOutput) {
				LIST(OBJECT_CODEC_SOURCE);
			}
			v.empty = v.size == 0;
			break;
		case kAudioObjectPropertyControlList:
			if (d->object == OBJECT_SPEAKER) {
				LIST(OBJECT_SPEAKER_VOLUME);
			}
			if (d->object == OBJECT_CODEC) {
				LIST(OBJECT_CODEC_SOURCE);
			}
			v.empty = v.size == 0;
			break;
		case kAudioDevicePropertyNominalSampleRate:
			v.u.number = HAKIOS_AUDIO_RATE;
			v.size = sizeof(Float64);
			break;
		case kAudioDevicePropertyAvailableNominalSampleRates:
			v.u.range.mMinimum = v.u.range.mMaximum = HAKIOS_AUDIO_RATE;
			v.size = sizeof(AudioValueRange);
			break;
		case kAudioDevicePropertyPreferredChannelsForStereo:
			v.u.words[0] = 1;
			v.u.words[1] = (d->output_channels >= 2 || d->input_channels >= 2) ? 2 : 1;
			v.size = 2 * sizeof(UInt32);
			break;
		case kAudioDevicePropertyZeroTimeStampPeriod:
			WORD(TIMESTAMP_PERIOD);
			break;
		}
	} else {
		switch (selector) {
		case kAudioStreamPropertyIsActive:
			WORD(!inactive[object]);
			break;
		case kAudioStreamPropertyDirection:
			WORD(object == d->input);
			break;
		case kAudioStreamPropertyTerminalType:
			WORD(object == d->input ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker);
			break;
		case kAudioStreamPropertyStartingChannel:
			WORD(1);
			break;
		case kAudioStreamPropertyLatency:
			WORD(0);
			break;
		case kAudioStreamPropertyVirtualFormat:
		case kAudioStreamPropertyPhysicalFormat:
			v.u.format = format_of(object);
			v.size = sizeof(AudioStreamBasicDescription);
			break;
		case kAudioStreamPropertyAvailableVirtualFormats:
		case kAudioStreamPropertyAvailablePhysicalFormats:
			v.u.ranged.mFormat = format_of(object);
			v.u.ranged.mSampleRateRange.mMinimum = v.u.ranged.mSampleRateRange.mMaximum = HAKIOS_AUDIO_RATE;
			v.size = sizeof(AudioStreamRangedDescription);
			break;
		}
	}
#undef WORD
#undef LIST
#undef STRING
#undef FLOAT
	return v;
}

static Boolean
has_property(__unused AudioServerPlugInDriverRef drv, AudioObjectID object, __unused pid_t client,
    const AudioObjectPropertyAddress *address)
{
	struct value v;

	if (address == NULL) {
		return false;
	}
	v = property(object, address, 0, NULL);
	return v.size != 0 || v.empty;
}

static OSStatus
is_property_settable(AudioServerPlugInDriverRef drv, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, Boolean *settable)
{
	const struct device *d = device_of(object);

	if (address == NULL || settable == NULL || !has_property(drv, object, client, address)) {
		return kAudioHardwareUnknownPropertyError;
	}
	if (object == OBJECT_SPEAKER_VOLUME) {
		*settable = address->mSelector == kAudioLevelControlPropertyScalarValue ||
		    address->mSelector == kAudioLevelControlPropertyDecibelValue;
	} else if (object == OBJECT_CODEC_SOURCE) {
		*settable = address->mSelector == kAudioSelectorControlPropertyCurrentItem;
	} else if (object == OBJECT_PLUGIN) {
		*settable = false;
	} else if (object == d->object) {
		*settable = address->mSelector == kAudioDevicePropertyNominalSampleRate;
	} else {
		*settable = address->mSelector == kAudioStreamPropertyIsActive ||
		    address->mSelector == kAudioStreamPropertyVirtualFormat ||
		    address->mSelector == kAudioStreamPropertyPhysicalFormat;
	}
	return 0;
}

static OSStatus
get_property_data_size(AudioServerPlugInDriverRef drv, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier, UInt32 *size)
{
	if (address == NULL || size == NULL || !has_property(drv, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ?
		    kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	*size = property(object, address, qualifier_size, qualifier).size;
	return 0;
}

static OSStatus
get_property_data(AudioServerPlugInDriverRef drv, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, UInt32 qualifier_size, const void *qualifier,
    UInt32 room, UInt32 *size, void *data)
{
	struct value v;

	if (address == NULL || size == NULL || data == NULL || !has_property(drv, object, client, address)) {
		return object < OBJECT_PLUGIN || object > OBJECT_LAST ?
		    kAudioHardwareBadObjectError : kAudioHardwareUnknownPropertyError;
	}
	if (object == OBJECT_SPEAKER_VOLUME &&
	    (address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ||
	     address->mSelector == kAudioLevelControlPropertyConvertDecibelsToScalar)) {
		if (room < sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		*(Float32 *)data = address->mSelector == kAudioLevelControlPropertyConvertScalarToDecibels ?
		    scalar_to_db(*(Float32 *)data) : db_to_scalar(*(Float32 *)data);
		*size = sizeof(Float32);
		return 0;
	}
	pthread_mutex_lock(&lock);
	v = property(object, address, qualifier_size, qualifier);
	pthread_mutex_unlock(&lock);
	if (v.size > room) {
		if (address->mSelector != kAudioObjectPropertyOwnedObjects &&
		    address->mSelector != kAudioDevicePropertyStreams &&
		    address->mSelector != kAudioPlugInPropertyDeviceList &&
		    address->mSelector != kAudioSelectorControlPropertyAvailableItems) {
			return kAudioHardwareBadPropertySizeError;
		}
		v.size = room - room % sizeof(UInt32);
	}
	memcpy(data, &v.u, v.size);
	if (v.string) {
		CFRetain(v.u.string);
	}
	*size = v.size;
	return 0;
}

static OSStatus
set_property_data(AudioServerPlugInDriverRef drv, AudioObjectID object, pid_t client,
    const AudioObjectPropertyAddress *address, __unused UInt32 qualifier_size, __unused const void *qualifier,
    UInt32 size, const void *data)
{
	Boolean settable = false;

	if (address == NULL || data == NULL || is_property_settable(drv, object, client, address, &settable) != 0) {
		return kAudioHardwareUnknownPropertyError;
	}
	if (!settable) {
		return kAudioHardwareIllegalOperationError;
	}
	if (object == OBJECT_SPEAKER_VOLUME) {
		AudioObjectPropertyAddress changed[2] = {
			{ kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, 0 },
			{ kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, 0 },
		};
		Float32 wanted;

		if (size != sizeof(Float32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		wanted = *(const Float32 *)data;
		wanted = address->mSelector == kAudioLevelControlPropertyDecibelValue ? db_to_scalar(wanted) :
		    wanted < 0 ? 0 : wanted > 1 ? 1 : wanted;
		pthread_mutex_lock(&lock);
		volume = wanted;
		volume_to_kernel();
		pthread_mutex_unlock(&lock);
		if (host != NULL) {
			host->PropertiesChanged(host, OBJECT_SPEAKER_VOLUME, 2, changed);
		}
		return 0;
	}
	if (object == OBJECT_CODEC_SOURCE) {
		AudioObjectPropertyAddress changed = {
			kAudioSelectorControlPropertyCurrentItem, kAudioObjectPropertyScopeGlobal, 0
		};
		struct sweet_audio_shm *s;
		UInt32 wanted;

		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		wanted = *(const UInt32 *)data;
		if (!valid_mic_source(wanted)) {
			return kAudioHardwareIllegalOperationError;
		}
		if (wanted == MIC_SOURCE_LEGACY) {
			wanted = MIC_SOURCE_1;
		}
		pthread_mutex_lock(&lock);
		mic_source = wanted;
		s = ensure_shm();
		if (s != NULL) {
			__atomic_store_n(&s->codec_mic_source, mic_source, __ATOMIC_RELEASE);
		}
		pthread_mutex_unlock(&lock);
		if (host != NULL) {
			host->PropertiesChanged(host, OBJECT_CODEC_SOURCE, 1, &changed);
		}
		return 0;
	}
	if (address->mSelector == kAudioStreamPropertyIsActive) {
		if (size != sizeof(UInt32)) {
			return kAudioHardwareBadPropertySizeError;
		}
		pthread_mutex_lock(&lock);
		inactive[object] = *(const UInt32 *)data == 0;
		pthread_mutex_unlock(&lock);
		return 0;
	}
	if (address->mSelector == kAudioDevicePropertyNominalSampleRate) {
		return size == sizeof(Float64) && *(const Float64 *)data == HAKIOS_AUDIO_RATE ?
		    0 : kAudioDeviceUnsupportedFormatError;
	}
	if (size != sizeof(AudioStreamBasicDescription)) {
		return kAudioHardwareBadPropertySizeError;
	} else {
		const AudioStreamBasicDescription *wanted = data, format = format_of(object);
		return wanted->mSampleRate == format.mSampleRate &&
		       wanted->mFormatID == format.mFormatID &&
		       wanted->mChannelsPerFrame == format.mChannelsPerFrame &&
		       wanted->mBitsPerChannel == format.mBitsPerChannel &&
		       (wanted->mFormatFlags & kAudioFormatFlagIsSignedInteger) ?
		       0 : kAudioDeviceUnsupportedFormatError;
	}
}

/* ---- Codec 4-channel microphone (/tmp/sweet-audio.shm or /dev/hakios-mic) ---- */

static int mic = -1;
static UInt64 mic_frames, mic_heard;

static void
codec_open(void)
{
	struct sweet_audio_shm *s = ensure_shm();

	mic = open("/dev/hakios-mic", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	mic_frames = mic_heard = 0;
	if (s != NULL) {
		mic_read_pos = __atomic_load_n(&s->mic_write_frames, __ATOMIC_ACQUIRE);
	}
}

static void
codec_close(void)
{
	if (mic >= 0) {
		close(mic);
	}
	mic = -1;
}

static void
codec_read(int16_t *samples, UInt32 frames)
{
	struct sweet_audio_shm *s = ensure_shm();
	UInt32 have = 0;

	if (s != NULL) {
		uint32_t mw = __atomic_load_n(&s->mic_write_frames, __ATOMIC_ACQUIRE);
		uint32_t avail = mw - mic_read_pos;
		if (avail > SWEET_AUDIO_MIC_RING_FRAMES) {
			mic_read_pos = mw;
			avail = 0;
		}
		if (avail > 0) {
			uint32_t take = avail < frames ? avail : frames;
			uint32_t mask = SWEET_AUDIO_MIC_RING_FRAMES - 1u;
			for (uint32_t i = 0; i < take; i++) {
				uint32_t idx = ((mic_read_pos + i) & mask) * CODEC_CHANNELS;
				memcpy(&samples[i * CODEC_CHANNELS], &s->mic_pcm[idx],
				    CODEC_CHANNELS * sizeof(int16_t));
			}
			mic_read_pos += take;
			have = take;
		}
	}
	if (have == 0 && mic >= 0) {
		int16_t mono[512];
		UInt32 want = frames < 512 ? frames : 512;
		ssize_t got = read(mic, mono, (size_t)want * sizeof(int16_t));
		if (got > 0) {
			have = (UInt32)got / sizeof(int16_t);
			for (UInt32 i = 0; i < have; i++) {
				for (UInt32 ch = 0; ch < CODEC_CHANNELS; ch++) {
					samples[i * CODEC_CHANNELS + ch] = mono[i];
				}
			}
		}
	}
	if (have < frames) {
		memset(&samples[have * CODEC_CHANNELS], 0,
		    (size_t)(frames - have) * CODEC_CHANNELS * sizeof(int16_t));
	}
	mic_frames += frames;
	mic_heard += have;
}

static OSStatus
start_io(__unused AudioServerPlugInDriverRef drv, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);
	struct sweet_audio_shm *s;

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running++ == 0) {
		d->anchor = mach_absolute_time();
		d->periods = 0;
		if (d->object == OBJECT_SPEAKER) {
			written = refused = 0;
			s = ensure_shm();
			if (s != NULL) {
				__atomic_store_n(&s->speaker_running, 1u, __ATOMIC_RELEASE);
			}
		} else if (d->object == OBJECT_CODEC) {
			codec_open();
		} else if (d->object == OBJECT_BASEBAND_VOICE) {
			s = ensure_shm();
			if (s != NULL) {
				__atomic_store_n(&s->baseband_voice_io, 1u, __ATOMIC_RELEASE);
			}
			say("sweet: audio: Baseband Voice IO started\n");
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
stop_io(__unused AudioServerPlugInDriverRef drv, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);
	struct sweet_audio_shm *s;

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running != 0 && --d->running == 0) {
		if (d->object == OBJECT_SPEAKER) {
			s = ensure_shm();
			if (s != NULL) {
				__atomic_store_n(&s->speaker_running, 0u, __ATOMIC_RELEASE);
			}
		} else if (d->object == OBJECT_CODEC) {
			codec_close();
		} else if (d->object == OBJECT_BASEBAND_VOICE) {
			s = ensure_shm();
			if (s != NULL) {
				__atomic_store_n(&s->baseband_voice_io, 0u, __ATOMIC_RELEASE);
			}
			say("sweet: audio: Baseband Voice IO stopped\n");
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
get_zero_time_stamp(__unused AudioServerPlugInDriverRef drv, AudioObjectID object, __unused UInt32 client,
    Float64 *sample_time, UInt64 *host_time, UInt64 *seed)
{
	Float64 ticks_per_period = ticks_per_frame * TIMESTAMP_PERIOD;
	UInt64 now = mach_absolute_time();
	struct device *d = device_of(object);

	if (d == NULL) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	while (now >= d->anchor + (UInt64)((Float64)(d->periods + 1) * ticks_per_period)) {
		d->periods++;
	}
	*sample_time = (Float64)d->periods * TIMESTAMP_PERIOD;
	*host_time = d->anchor + (UInt64)((Float64)d->periods * ticks_per_period);
	*seed = 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
will_do_io_operation(__unused AudioServerPlugInDriverRef drv, __unused AudioObjectID object, __unused UInt32 client,
    UInt32 operation, Boolean *will_do, Boolean *in_place)
{
	*will_do = operation == kAudioServerPlugInIOOperationReadInput ||
	    operation == kAudioServerPlugInIOOperationWriteMix;
	*in_place = true;
	return 0;
}

static OSStatus
begin_end_io_operation(__unused AudioServerPlugInDriverRef drv, __unused AudioObjectID object, __unused UInt32 client,
    __unused UInt32 operation, __unused UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle)
{
	return 0;
}

/* ---- Taptic Engine actuator (/dev/hakios-haptic -> AW8697 LRA) ---- */

static int haptic = -1;
static unsigned haptic_open_tries;
static unsigned haptic_said;

static void
actuator_write(const int16_t *samples, UInt32 frames)
{
	int peak = 0;

	for (UInt32 i = 0; i < frames; i++) {
		int v = samples[i] < 0 ? -samples[i] : samples[i];
		peak = v > peak ? v : peak;
	}
	if (haptic < 0 && haptic_open_tries < 16) {
		haptic_open_tries++;
		haptic = open("/dev/hakios-haptic", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	}
	if (haptic >= 0) {
		(void)write(haptic, samples, (size_t)frames * 2);
	}
	if (peak > 0 && haptic_said < 12) {
		haptic_said++;
		say("sweet: audio: actuator %u frames, peak %d/32768%s\n", (unsigned)frames, peak,
		    haptic >= 0 ? "" : " (no /dev/hakios-haptic)");
	}
}

static OSStatus
do_io_operation(__unused AudioServerPlugInDriverRef drv, __unused AudioObjectID object, AudioObjectID stream,
    __unused UInt32 client, UInt32 operation, UInt32 frames, __unused const AudioServerPlugInIOCycleInfo *cycle,
    void *buffer, __unused void *secondary)
{
	const struct device *d = device_of(stream);

	if (d == NULL) {
		return kAudioHardwareBadObjectError;
	}
	if (operation == kAudioServerPlugInIOOperationReadInput && stream == OBJECT_CODEC_INPUT) {
		codec_read(buffer, frames);
	} else if (operation == kAudioServerPlugInIOOperationReadInput) {
		memset(buffer, 0, (size_t)frames * 2 * d->input_channels);
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == OBJECT_ACTUATOR_OUTPUT) {
		actuator_write(buffer, frames);
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == OBJECT_SPEAKER_OUTPUT) {
		int fd = ensure_sink_fd();

		shm_write_speaker(buffer, frames);
		if (fd < 0 || write(fd, buffer, (size_t)frames * HAKIOS_AUDIO_FRAME) < 0) {
			refused += frames;
		}
		written += frames;
	}
	return 0;
}

static AudioServerPlugInDriverInterface interface_vtable = {
	NULL, query_interface, add_ref, release, initialize, create_device, destroy_device,
	device_client, device_client, configuration_change, configuration_change,
	has_property, is_property_settable, get_property_data_size, get_property_data,
	set_property_data, start_io, stop_io, get_zero_time_stamp, will_do_io_operation,
	begin_end_io_operation, do_io_operation, begin_end_io_operation,
};
static AudioServerPlugInDriverInterface *driver_instance = &interface_vtable;

void *
HakiosAudio_Create(__unused CFAllocatorRef allocator, CFUUIDRef type)
{
	return CFEqual(type, kAudioServerPlugInTypeUUID) ? &driver_instance : NULL;
}
