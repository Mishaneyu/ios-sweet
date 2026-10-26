/*
 * SPDX-License-Identifier: APSL-2.0
 *
 * libsweet_sysfix.dylib: non-intrusive boot & UI responsiveness fixes for
 * containermanagerd and SpringBoard on iOS 16 (RMX1931 / SWEET).
 * Adapted from HackiOS userland/src/inject.c & userland/src/inject_sb.c
 * (https://github.com/oosexgang/hackios-xnu-kernel).
 *
 * Loaded via kern.exec_inject (0200-rmx1931-hackios-peripherals.patch):
 *   - containermanagerd / containermanagerd_system:
 *       +[MCMUserIdentityCache personasAreSupported] -> NO
 *       Eliminates the 20 x 1 s persona lookup retry stall during boot while
 *       backboardd and app containers wait on containermanagerd.
 *   - SpringBoard:
 *       +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] -> JPEG / PNG
 *       Avoids multi-second CPU ASTC (.atx) texture compression when apps go to
 *       background, preventing 10 s scene-update watchdog terminations.
 *
 * Note: mobileactivationd and IOPMPowerSource (SweetBattery) work natively on
 * RMX1931 and are intentionally left untouched here.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
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

/* ---- containermanagerd: +[MCMUserIdentityCache personasAreSupported] ---- */

static BOOL
personas_are_supported(__unused id self, __unused SEL selector)
{
	return NO;
}

static bool cm_hooked;

static void
cm_image_added(__unused const struct mach_header *header, __unused intptr_t slide)
{
	Class cls;
	Method method;

	if (cm_hooked || (cls = objc_getClass("MCMUserIdentityCache")) == NULL) {
		return;
	}
	cm_hooked = true;
	method = class_getClassMethod(cls, sel_registerName("personasAreSupported"));
	if (method != NULL) {
		method_setImplementation(method, (IMP)personas_are_supported);
		say("sweet: %s: +[MCMUserIdentityCache personasAreSupported] -> NO\n", getprogname());
	}
}

/* ---- SpringBoard: +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] ---- */

#define SNAPSHOT_FORMAT_PNG  0L
#define SNAPSHOT_FORMAT_JPEG 3L

static long
snapshot_output_format(__unused id self, __unused SEL selector, id snapshot)
{
	SEL opaque = sel_registerName("isImageOpaque");

	if (snapshot != NULL &&
	    class_getInstanceMethod(object_getClass(snapshot), opaque) != NULL &&
	    ((BOOL (*)(id, SEL))objc_msgSend)(snapshot, opaque)) {
		return SNAPSHOT_FORMAT_JPEG;
	}
	return SNAPSHOT_FORMAT_PNG;
}

static bool sb_hooked;

static void
sb_image_added(__unused const struct mach_header *header, __unused intptr_t slide)
{
	Class cls;
	Method method;

	if (sb_hooked || (cls = objc_getClass("XBApplicationSnapshotManifestImpl")) == NULL) {
		return;
	}
	sb_hooked = true;
	method = class_getClassMethod(cls, sel_registerName("_outputFormatForSnapshot:"));
	if (method != NULL) {
		method_setImplementation(method, (IMP)snapshot_output_format);
		say("sweet: SpringBoard: +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] -> JPEG/PNG\n");
	}
}

__attribute__((constructor))
static void
sweet_sysfix_init(void)
{
	const char *name = getprogname();

	if (name == NULL) {
		return;
	}
	if (strcmp(name, "containermanagerd") == 0 ||
	    strcmp(name, "containermanagerd_system") == 0) {
		_dyld_register_func_for_add_image(cm_image_added);
	} else if (strcmp(name, "SpringBoard") == 0) {
		_dyld_register_func_for_add_image(sb_image_added);
	}
}
