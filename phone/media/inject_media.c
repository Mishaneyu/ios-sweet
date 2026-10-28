/*
 * SPDX-License-Identifier: APSL-2.0
 *
 * libhot_media.dylib: mediaserverd companion for iOS 16 on RMX1931 / SWEET.
 * Adapted from HackiOS userland/src/inject_media.c (https://github.com/oosexgang/hackios-xnu-kernel).
 *
 * Loaded into mediaserverd via kern.exec_inject (0200-rmx1931-hackios-peripherals.patch):
 *   1. Skips +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:]
 *      so CMCapture does not jump through a null OpenGL ES context on startup, keeping
 *      mediaserverd and VideoToolbox's software HEVC/HEIC decoder (VCPHEVC.videocodec) alive.
 *   2. Registers VCPHEVC.videocodec's software HEVC encoder (HEVCVideoEncoder_CreateInstance)
 *      with VideoToolbox for 'hvc1' so HEIC/HEVC encoding works without Apple AVE hardware.
 *   3. Bridges FigFlashlight (Control Center torch & AVFlashlight) to /dev/hakios-torch
 *      (PM8150L SID 5 0xd300 on RMX1931) via a synthetic BWFigCaptureDeviceVendor returned
 *      exclusively to FigFlashlight callers (verified 20H392 & 20F66 unslid ranges + dladdr).
 *   4. Prepares CMCapture graph hooks (no ISP video stabilization, swallow missing capture-audio
 *      broker 'nope', single-frame speed quality prioritization, bypass BWGNRNode, drop NULL
 *      sample buffers from silent BWAudioSourceNode).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
}

/* ---- Flashlight (/dev/hakios-torch) & future /dev/hakios-camera hookup ---- */

static int torch_fd = -1;
static unsigned torch_open_tries;
static bool camera_present;
static Class vendor_class, device_class, stream_class;
static IMP original_shared_vendor;
static uintptr_t cmcapture_slide;
static unsigned torch_said;

struct addr_range {
	uintptr_t from, to;
};

/*
 * Verified unslid FigFlashlight caller ranges in CMCapture (dyld_shared_cache_arm64):
 *   - iOS 16.7.16 (20H392, D22AP): sharedCaptureDeviceVendor = 0x1a1f40c84
 *       _flashlight_IsAvailable .. _flashlight_setLevelInternal: 0x1a20007b0 .. 0x1a2000c88
 *       _flashlight_teardownDevice:                              0x1a1f3e540 .. 0x1a1f3e668
 *   - iOS 16.5    (20F66,  D22AP): sharedCaptureDeviceVendor = 0x1a1ef0d04
 *       _flashlight_IsAvailable .. _flashlight_setupDevice:      0x1a1fb0830 .. 0x1a1fb0c5c
 *       _flashlight_teardownDevice:                              0x1a1eee5c0 .. 0x1a1eee6e8
 */
static const struct addr_range flashlight_20h392[] = {
	{ 0x1a20007b0UL, 0x1a2000c88UL },
	{ 0x1a1f3e540UL, 0x1a1f3e668UL },
};

static const struct addr_range flashlight_20f66[] = {
	{ 0x1a1fb0830UL, 0x1a1fb0c5cUL },
	{ 0x1a1eee5c0UL, 0x1a1eee6e8UL },
};

static int
ensure_torch_fd(void)
{
	if (torch_fd >= 0) {
		return torch_fd;
	}
	if (torch_open_tries < 16) {
		torch_open_tries++;
		torch_fd = open("/dev/hakios-torch", O_WRONLY);
		if (torch_fd >= 0) {
			say("hot: mediaserverd: opened /dev/hakios-torch\n");
		}
	}
	return torch_fd;
}

static bool
caller_is_flashlight(const void *ret_addr)
{
	uintptr_t caller = (uintptr_t)ret_addr - cmcapture_slide;
	uintptr_t unslid_vendor = original_shared_vendor ? ((uintptr_t)original_shared_vendor - cmcapture_slide) : 0;
	const struct addr_range *ranges = NULL;
	size_t count = 0;

	if (unslid_vendor == 0x1a1f40c84UL) {
		ranges = flashlight_20h392;
		count = sizeof(flashlight_20h392) / sizeof(flashlight_20h392[0]);
	} else if (unslid_vendor == 0x1a1ef0d04UL) {
		ranges = flashlight_20f66;
		count = sizeof(flashlight_20f66) / sizeof(flashlight_20f66[0]);
	}
	for (size_t i = 0; i < count; i++) {
		if (caller >= ranges[i].from && caller < ranges[i].to) {
			return true;
		}
	}
	Dl_info info;
	if (dladdr(ret_addr, &info) != 0 && info.dli_sname != NULL &&
	    strstr(info.dli_sname, "flashlight") != NULL) {
		return true;
	}
	return false;
}

/* +[BWFigCaptureDeviceVendor sharedCaptureDeviceVendor] */
static id
shared_vendor(id self, SEL selector)
{
	const void *ret_addr = __builtin_return_address(0);
	id vendor = original_shared_vendor ? ((id (*)(id, SEL))original_shared_vendor)(self, selector) : NULL;
	static id ours, camera_vendor;
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

	if (vendor != NULL) {
		return vendor;
	}
	if (!camera_present && access("/dev/hakios-camera", F_OK) == 0) {
		camera_present = true;
		say("hot: mediaserverd: /dev/hakios-camera detected\n");
	}
	if (camera_present) {
		void *create_fn = dlsym(RTLD_DEFAULT, "hakios_camera_device_create");
		if (create_fn != NULL) {
			pthread_mutex_lock(&lock);
			if (camera_vendor == NULL) {
				id made = ((id (*)(Class, SEL))objc_msgSend)(
				    objc_getClass("BWFigCaptureDeviceVendor"), sel_registerName("alloc"));
				camera_vendor = ((id (*)(id, SEL, void *))objc_msgSend)(
				    made, sel_registerName("initWithDeviceCreateFunction:"), create_fn);
				say("hot: mediaserverd: camera device vendor %s\n",
				    camera_vendor ? "created" : "FAILED");
			}
			pthread_mutex_unlock(&lock);
			return camera_vendor;
		}
	}
	if (caller_is_flashlight(ret_addr) && ensure_torch_fd() >= 0) {
		pthread_mutex_lock(&lock);
		if (ours == NULL && vendor_class != NULL) {
			ours = class_createInstance(vendor_class, 0);
			say("hot: mediaserverd: providing HotFlashlightVendor to FigFlashlight\n");
		}
		pthread_mutex_unlock(&lock);
		return ours;
	}
	return NULL;
}

static void
flashlight_set(uint32_t level)
{
	int fd = ensure_torch_fd();
	ssize_t written = fd >= 0 ? write(fd, &level, sizeof(level)) : -1;

	if (written != (ssize_t)sizeof(level) || torch_said < 40) {
		torch_said++;
		say("hot: mediaserverd: flashlight %u/1000%s\n", level,
		    written == (ssize_t)sizeof(level) ? "" : " (write failed)");
	}
}

static int
flashlight_register(__unused id self, __unused SEL selector, __unused int pid, __unused id description,
    __unused int priority, __unused BOOL steal, __unused BOOL share, __unused id handler)
{
	return 1;
}

static id
flashlight_copy_device(__unused id self, __unused SEL selector, __unused int client, __unused BOOL inform, int *error)
{
	if (error != NULL) {
		*error = 0;
	}
	return class_createInstance(device_class, 0);
}

static id
flashlight_copy_stream(__unused id self, __unused SEL selector, __unused int position, __unused int type, id device)
{
	return device != NULL && object_getClass(device) == device_class ? class_createInstance(stream_class, 0) : NULL;
}

static void
flashlight_take_back(__unused id self, __unused SEL selector, __unused id device, __unused int client, __unused BOOL inform)
{
	flashlight_set(0);
}

static BOOL
flashlight_active_equals(__unused id self, __unused SEL selector, id device)
{
	return device == NULL || object_getClass(device) == device_class;
}

static id
flashlight_stream_properties(__unused id self, __unused SEL selector)
{
	static CFDictionaryRef none;

	if (none == NULL) {
		none = CFDictionaryCreate(NULL, NULL, NULL, 0,
		    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	return (id)none;
}

static int
flashlight_stream_set(__unused id self, __unused SEL selector, id key, id value)
{
	float level = 0.0f;

	if (key != NULL && CFEqual((CFTypeRef)key, CFSTR("TorchLevel"))) {
		if (value != NULL && CFGetTypeID((CFTypeRef)value) == CFNumberGetTypeID()) {
			CFNumberGetValue((CFNumberRef)value, kCFNumberFloatType, &level);
		}
		level = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level;
		flashlight_set((uint32_t)(level * 1000.0f + 0.5f));
	}
	return 0;
}

/* ---- CMCapture graph shims ---- */

static int
video_stabilization_method(__unused id self, __unused SEL selector)
{
	return 0;
}

static int
video_stabilization_type(__unused id self, __unused SEL selector)
{
	return 0;
}

#define ERROR_NO_SUCH_THING 1852797029L /* 'nope' */

static IMP original_audio_source_start;

static BOOL
audio_source_start(id self, SEL selector, id *error)
{
	BOOL started = ((BOOL (*)(id, SEL, id *))original_audio_source_start)(self, selector, error);
	id made = error != NULL ? *error : NULL;
	static bool said;

	if (started || made == NULL ||
	    ((long (*)(id, SEL))objc_msgSend)(made, sel_registerName("code")) != ERROR_NO_SUCH_THING) {
		return started;
	}
	*error = NULL;
	if (!said) {
		said = true;
		say("hot: mediaserverd: capture audio broker absent; continuing session silently\n");
	}
	return true;
}

static IMP original_quality_prioritization;

static int
quality_prioritization(id self, SEL selector)
{
	int asked = ((int (*)(id, SEL))original_quality_prioritization)(self, selector);
	static bool said;

	if (asked > 1 && !said) {
		said = true;
		say("hot: mediaserverd: clamping still qualityPrioritization %d -> 1 (speed)\n", asked);
	}
	return asked > 1 ? 1 : asked;
}

static void
gnr_render(id self, __unused SEL selector, void *sample_buffer, __unused id input)
{
	id output = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("output"));
	static bool said;

	if (!said) {
		said = true;
		say("hot: mediaserverd: bypassing BWGNRNode\n");
	}
	if (output != nil) {
		((void (*)(id, SEL, void *))objc_msgSend)(output, sel_registerName("emitSampleBuffer:"), sample_buffer);
	}
}

static bool
is_node(id node, const char *name)
{
	return node != nil && strcmp(object_getClassName(node), name) == 0;
}

static void
time_photo_encoder(id output, CMSampleBufferRef sample_buffer)
{
	CVImageBufferRef image = CMSampleBufferGetImageBuffer(sample_buffer);
	static uint64_t began;
	static mach_timebase_info_data_t timebase;
	id connection, input;

	if (image == NULL || CVPixelBufferGetWidth(image) < 2000) {
		return;
	}
	if (is_node(((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("node")), "BWPhotoEncoderNode")) {
		if (began != 0) {
			if (timebase.denom == 0) {
				mach_timebase_info(&timebase);
			}
			say("hot: mediaserverd: encoded %zux%zu photo in %.0f ms\n",
			    CVPixelBufferGetWidth(image), CVPixelBufferGetHeight(image),
			    (double)(mach_absolute_time() - began) * timebase.numer / timebase.denom / 1e6);
		}
		began = 0;
		return;
	}
	connection = ((id (*)(id, SEL))objc_msgSend)(output, sel_registerName("connection"));
	input = connection != nil ? ((id (*)(id, SEL))objc_msgSend)(connection, sel_registerName("input")) : nil;
	if (input != nil && is_node(((id (*)(id, SEL))objc_msgSend)(input, sel_registerName("node")), "BWPhotoEncoderNode")) {
		began = mach_absolute_time();
	}
}

static IMP original_emit_sample_buffer;

static void
emit_sample_buffer(id self, SEL selector, CMSampleBufferRef sample_buffer)
{
	static bool said;

	if (sample_buffer == NULL) {
		if (!said) {
			said = true;
			say("hot: mediaserverd: dropping NULL sampleBuffer on BWNodeOutput\n");
		}
		return;
	}
	time_photo_encoder(self, sample_buffer);
	((void (*)(id, SEL, CMSampleBufferRef))original_emit_sample_buffer)(self, selector, sample_buffer);
}

/* ---- Software HEVC encoder registration (VCPHEVC.videocodec) ---- */

static void
register_hevc_encoder(void)
{
	void *codec = dlopen("/System/Library/VideoCodecs/VCPHEVC.videocodec", RTLD_NOW | RTLD_LOCAL);
	void *vt = dlopen("/System/Library/Frameworks/VideoToolbox.framework/VideoToolbox", RTLD_NOW | RTLD_GLOBAL);
	void *create = codec != NULL ? dlsym(codec, "HEVCVideoEncoder_CreateInstance") : NULL;
	int (*register_encoder)(uint32_t, CFDictionaryRef, void *) =
	    vt != NULL ? dlsym(vt, "VTRegisterVideoEncoderWithInfo") : NULL;
	CFStringRef *id_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderID") : NULL;
	CFStringRef *name_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_EncoderName") : NULL;
	CFStringRef *hardware_key = vt != NULL ? dlsym(vt, "kVTVideoEncoderList_IsHardwareAccelerated") : NULL;

	if (create == NULL || register_encoder == NULL || id_key == NULL ||
	    name_key == NULL || hardware_key == NULL) {
		return;
	}
	const void *keys[] = { *id_key, *name_key, *hardware_key };
	const void *values[] = {
		CFSTR("com.apple.videotoolbox.videoencoder.hevc.vcp"),
		CFSTR("HEVC (VCPHEVC, software)"),
		kCFBooleanFalse,
	};
	CFDictionaryRef info = CFDictionaryCreate(NULL, keys, values, 3,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (info != NULL) {
		int status = register_encoder(0x68766331u /* 'hvc1' */, info, create);
		CFRelease(info);
		say("hot: mediaserverd: registered VCPHEVC software HEVC encoder (%d)\n", status);
	}
}

struct method_spec {
	const char *selector, *types;
	IMP imp;
};

static Class
make_class(const char *name, const struct method_spec *methods, size_t count)
{
	Class cls = objc_allocateClassPair(objc_getClass("NSObject"), name, 0);
	if (cls == NULL) {
		return objc_getClass(name);
	}
	for (size_t i = 0; i < count; i++) {
		class_addMethod(cls, sel_registerName(methods[i].selector), methods[i].imp, methods[i].types);
	}
	objc_registerClassPair(cls);
	return cls;
}

static void
flashlight_classes(void)
{
	static const struct method_spec vendor[] = {
		{ "registerClientWithPID:clientDescription:clientPriority:canStealFromClientsWithSamePriority:"
		  "deviceSharingWithOtherClientsAllowed:deviceAvailabilityChangedHandler:",
		  "i@:i@iBB@", (IMP)flashlight_register },
		{ "copyDeviceForClient:informClientWhenDeviceAvailableAgain:error:",
		  "@@:iB^i", (IMP)flashlight_copy_device },
		{ "copyStreamForFlashlightWithPosition:deviceType:forDevice:",
		  "@@:ii@", (IMP)flashlight_copy_stream },
		{ "takeBackDevice:forClient:informClientWhenDeviceAvailableAgain:",
		  "v@:@iB", (IMP)flashlight_take_back },
		{ "activeDeviceEquals:", "B@:@", (IMP)flashlight_active_equals },
	};
	static const struct method_spec stream[] = {
		{ "supportedProperties", "@@:", (IMP)flashlight_stream_properties },
		{ "setProperty:value:", "i@:@@", (IMP)flashlight_stream_set },
	};

	vendor_class = make_class("HotFlashlightVendor", vendor, sizeof(vendor) / sizeof(vendor[0]));
	device_class = make_class("HotFlashlightDevice", NULL, 0);
	stream_class = make_class("HotFlashlightStream", stream, sizeof(stream) / sizeof(stream[0]));
}

static struct replacement {
	const char *class_name, *selector;
	IMP replacement, *original;
	bool instance;
	bool capture_slide;
	bool done;
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:",
	  (IMP)prewarm_portrait_filter, NULL, false, false, false },
	{ "BWFigCaptureDeviceVendor", "sharedCaptureDeviceVendor",
	  (IMP)shared_vendor, &original_shared_vendor, false, true, false },
	{ "BWAudioSourceNode", "start:",
	  (IMP)audio_source_start, &original_audio_source_start, true, false, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationMethod",
	  (IMP)video_stabilization_method, NULL, true, false, false },
	{ "FigVideoCaptureConnectionConfiguration", "videoStabilizationType",
	  (IMP)video_stabilization_type, NULL, true, false, false },
	{ "FigCaptureStillImageSettings", "qualityPrioritization",
	  (IMP)quality_prioritization, &original_quality_prioritization, true, false, false },
	{ "BWGNRNode", "renderSampleBuffer:forInput:",
	  (IMP)gnr_render, NULL, true, false, false },
	{ "BWNodeOutput", "emitSampleBuffer:",
	  (IMP)emit_sample_buffer, &original_emit_sample_buffer, true, false, false },
};

static void
image_added(__unused const struct mach_header *header, intptr_t image_slide)
{
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == NULL) {
			continue;
		}
		r->done = true;
		method = r->instance ? class_getInstanceMethod(cls, sel_registerName(r->selector)) :
		    class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		if (original == NULL) {
			say("hot: mediaserverd: %c[%s %s] missing\n",
			    r->instance ? '-' : '+', r->class_name, r->selector);
			continue;
		}
		if (r->capture_slide) {
			cmcapture_slide = (uintptr_t)image_slide;
		}
		if (r->original != NULL) {
			*r->original = original;
		}
		method_setImplementation(method, r->replacement);
		say("hot: mediaserverd: %c[%s %s] hooked (unslid %#lx)\n",
		    r->instance ? '-' : '+', r->class_name, r->selector,
		    (unsigned long)((uintptr_t)original - (uintptr_t)image_slide));
	}
}

__attribute__((constructor))
static void
hot_media_init(void)
{
	const char *name = getprogname();

	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		return;
	}
	flashlight_classes();
	(void)ensure_torch_fd();
	camera_present = access("/dev/hakios-camera", F_OK) == 0;
	register_hevc_encoder();
	_dyld_register_func_for_add_image(image_added);
}
