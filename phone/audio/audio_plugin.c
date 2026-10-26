/*
 * SPDX-License-Identifier: APSL-2.0
 *
 * HakiosAudio.driver: CoreAudio HAL AudioServerPlugIn for iOS 16 on RMX1931 / SWEET.
 * Adapted from HackiOS userland/src/audio_plugin.c (https://github.com/oosexgang/hackios-xnu-kernel).
 *
 * Presents the four built-in devices expected by D22AP's VirtualAudio.plugin routing tables:
 *   - "Speaker"   stereo 48 kHz output -> /dev/hakios-audio (+ HAKIOS_AUDIO_SET_VOLUME ioctl),
 *                 plus silent stereo reference input
 *   - "Hawking"   silent mono input required by D22AP's 'vhaw' companion route
 *   - "Actuator"  mono 48 kHz Taptic Engine waveform output -> /dev/hakios-haptic (AW8697 LRA)
 *   - "Codec"     mono 48 kHz built-in microphone ('imic') -> /dev/hakios-mic (or silence)
 */
#include <CoreFoundation/CoreFoundation.h>
#if __has_include(<CoreAudioTypes/CoreAudioTypes.h>)
#include <CoreAudioTypes/CoreAudioTypes.h>
#else
#include <CoreAudio/CoreAudioTypes.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/ioctl.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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
	OBJECT_LAST = OBJECT_CODEC_SOURCE
};

#define VOLUME_RANGE_DB   48.0

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
	{ .object = OBJECT_CODEC, .uid = CFSTR("Codec"), .input = OBJECT_CODEC_INPUT, .input_channels = 1 },
};
#define DEVICES             (sizeof(devices) / sizeof(devices[0]))

#define TRANSPORT_BUILT_IN  'bltn'
#define MIC_SOURCE          'imic'
#define MANUFACTURER        CFSTR("sweet")
#define TIMESTAMP_PERIOD    16384

static AudioServerPlugInHostRef host;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int sink = -1;
static unsigned sink_open_tries;
static Float64 ticks_per_frame;
static UInt64 written, refused;
static Boolean inactive[OBJECT_LAST + 1];
static Float32 volume = 1.0f;

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

static int
ensure_sink_fd(void)
{
	if (sink >= 0) {
		return sink;
	}
	if (sink_open_tries < 16) {
		sink_open_tries++;
		sink = open(HAKIOS_AUDIO_DEVICE, O_WRONLY);
	}
	return sink;
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

static void
volume_to_kernel(void)
{
	struct hakios_audio_volume v = { (uint32_t)lround(-scalar_to_db(volume) * 8), volume <= 0 };
	int fd = ensure_sink_fd();

	if (fd < 0 || ioctl(fd, HAKIOS_AUDIO_SET_VOLUME, &v) < 0) {
		say("sweet: audio: volume %.3f ioctl failed (%s)\n", volume, strerror(errno));
		return;
	}
	say("sweet: audio: volume %.3f (%s-%.1f dB)\n", volume, v.muted ? "muted, " : "", v.below / 8.0);
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
	sink = open(HAKIOS_AUDIO_DEVICE, O_WRONLY);
	sink_open_tries = 1;
	say("sweet: audio: HakiosAudio initialized (%s: %s)\n",
	    HAKIOS_AUDIO_DEVICE, sink < 0 ? strerror(errno) : "open");
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
		UInt32 words[4];
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
#define LIST(x)   (v.u.words[v.size / sizeof(UInt32)] = (x), v.size += sizeof(UInt32))
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
		case kAudioSelectorControlPropertyAvailableItems:
			WORD(MIC_SOURCE);
			break;
		case kAudioSelectorControlPropertyItemName:
			if (qualifier_size == sizeof(UInt32) && qualifier != NULL &&
			    *(const UInt32 *)qualifier == MIC_SOURCE) {
				STRING(CFSTR("Bottom"));
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
			WORD(d->output != 0);
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
			v.u.words[1] = 2;
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
		    address->mSelector != kAudioPlugInPropertyDeviceList) {
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
		return size == sizeof(UInt32) ?
		    (*(const UInt32 *)data == MIC_SOURCE ? 0 : kAudioHardwareIllegalOperationError) :
		    kAudioHardwareBadPropertySizeError;
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

/* ---- Codec microphone (/dev/hakios-mic, optional) ---- */

static int mic = -1;
static UInt64 mic_frames, mic_heard;

static void
codec_open(void)
{
	mic = open("/dev/hakios-mic", O_RDONLY | O_NONBLOCK);
	mic_frames = mic_heard = 0;
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
	ssize_t got = mic >= 0 ? read(mic, samples, (size_t)frames * 2) : -1;
	UInt32 have = got > 0 ? (UInt32)got / 2 : 0;

	if (have < frames) {
		memset(samples + have, 0, (size_t)(frames - have) * 2);
	}
	mic_frames += frames;
	mic_heard += have;
}

static OSStatus
start_io(__unused AudioServerPlugInDriverRef drv, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running++ == 0) {
		d->anchor = mach_absolute_time();
		d->periods = 0;
		if (d->output != 0) {
			written = refused = 0;
		}
		if (d->object == OBJECT_CODEC) {
			codec_open();
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

static OSStatus
stop_io(__unused AudioServerPlugInDriverRef drv, AudioObjectID object, __unused UInt32 client)
{
	struct device *d = device_of(object);

	if (d == NULL || object != d->object) {
		return kAudioHardwareBadObjectError;
	}
	pthread_mutex_lock(&lock);
	if (d->running != 0 && --d->running == 0) {
		if (d->object == OBJECT_CODEC) {
			codec_close();
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
		haptic = open("/dev/hakios-haptic", O_WRONLY | O_NONBLOCK);
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
	} else if (operation == kAudioServerPlugInIOOperationWriteMix && stream == d->output) {
		int fd = ensure_sink_fd();
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
