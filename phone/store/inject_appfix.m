/*
 * SPDX-License-Identifier: APSL-2.0
 *
 * libsweet_appfix.dylib: runtime compatibility layer for third-party iOS apps
 * installed via SweetStore on RMX1931 / SWEET (iOS 26.5.2).
 *
 * Injected automatically into @userapps via kern.exec_inject (patch 0148):
 *   1. Isolated per-app Data container (/private/var/mobile/Containers/Data/Application/<bundleId>)
 *      via early CFFIXED_USER_HOME / HOME / TMPDIR setup + NSHomeDirectory interpose.
 *   2. App Group container fallback (-[NSFileManager containerURLForSecurityApplicationGroupIdentifier:])
 *      and -[NSUserDefaults initWithSuiteName:] fallback so force-unwrapped group.* URLs never SIGTRAP.
 *   3. Automatic Keychain -34018 (errSecMissingEntitlement) & -25243 recovery by retrying SecItem*
 *      without team-prefixed kSecAttrAccessGroup / kSecUseDataProtectionKeychain when rejected.
 *   4. Non-blocking StoreKit (+[SKPaymentQueue canMakePayments] -> NO, fast-fail SKProductsRequest /
 *      SKReceiptRefreshRequest) and APNs (-registerForRemoteNotifications fallback failure callback)
 *      so apps never stall on splash screens waiting for absent appstored / apsd hardware certs.
 *   5. Uncaught NSException logger -> /private/var/local/appfix.log.
 */
#import <Foundation/Foundation.h>
#import <CoreFoundation/CoreFoundation.h>
#import <Security/Security.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <os/lock.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
	const void *replacement;
	const void *replacee;
} sweet_interpose_t;

#define SWEET_INTERPOSE(_repl, _orig) \
	__attribute__((used)) static const sweet_interpose_t _interpose_##_orig \
	__attribute__((section("__DATA,__interpose"))) = { \
		(const void *)(uintptr_t)&_repl, \
		(const void *)(uintptr_t)&_orig \
	}

static bool g_active = false;
static char g_bundle_id[256];
static char g_app_container[512];
static os_unfair_lock g_hook_lock = OS_UNFAIR_LOCK_INIT;
static bool g_fm_hooked = false;
static bool g_sk_hooked = false;
static bool g_ui_hooked = false;
static NSUncaughtExceptionHandler *g_prev_exc_handler = NULL;

static void appfix_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void
appfix_log(const char *fmt, ...)
{
	char line[768];
	char msg[640];
	va_list ap;
	struct stat st;
	int fd, n;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	n = snprintf(line, sizeof(line), "[pid %d %s %s] %s\n",
	    (int)getpid(), getprogname() ? getprogname() : "?",
	    g_bundle_id[0] ? g_bundle_id : "-", msg);
	if (n <= 0) {
		return;
	}
	if (n > (int)sizeof(line)) {
		n = (int)sizeof(line);
	}
	fd = open("/private/var/local/appfix.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
	if (fd >= 0) {
		if (fstat(fd, &st) == 0 && st.st_size > 131072) {
			(void)ftruncate(fd, 0);
		}
		(void)write(fd, line, (size_t)n);
		close(fd);
	}
}

static void
sanitize_ident(const char *src, char *dst, size_t cap)
{
	size_t o = 0;

	if (!src || cap == 0) {
		if (cap > 0) dst[0] = '\0';
		return;
	}
	for (size_t i = 0; src[i] != '\0' && o + 1 < cap; i++) {
		unsigned char c = (unsigned char)src[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
			dst[o++] = (char)c;
		} else {
			dst[o++] = '_';
		}
	}
	if (o == 0 && cap > 4) {
		memcpy(dst, "app", 4);
		return;
	}
	dst[o] = '\0';
}

static void
mkdir_p(const char *path, mode_t mode)
{
	char tmp[512];
	size_t len;

	if (!path || !path[0]) {
		return;
	}
	len = strlcpy(tmp, path, sizeof(tmp));
	if (len >= sizeof(tmp)) {
		return;
	}
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			(void)mkdir(tmp, mode);
			*p = '/';
		}
	}
	(void)mkdir(tmp, mode);
}

/* Read CFBundleIdentifier directly from Info.plist before Foundation caches NSHomeDirectory(). */
static bool
read_bundle_id_early(const char *app_dir, char *out_bid, size_t cap)
{
	char plist_path[512];
	struct stat st;
	int fd;
	ssize_t rd;
	uint8_t *buf;
	CFDataRef data;
	CFPropertyListRef plist;
	bool ok = false;

	snprintf(plist_path, sizeof(plist_path), "%s/Info.plist", app_dir);
	fd = open(plist_path, O_RDONLY);
	if (fd < 0) {
		return false;
	}
	if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 1048576) {
		close(fd);
		return false;
	}
	buf = (uint8_t *)malloc((size_t)st.st_size);
	if (!buf) {
		close(fd);
		return false;
	}
	rd = read(fd, buf, (size_t)st.st_size);
	close(fd);
	if (rd != st.st_size) {
		free(buf);
		return false;
	}
	data = CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, buf, (CFIndex)rd, kCFAllocatorNull);
	if (data) {
		plist = CFPropertyListCreateWithData(kCFAllocatorDefault, data, kCFPropertyListImmutable, NULL, NULL);
		if (plist && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
			CFTypeRef val = CFDictionaryGetValue((CFDictionaryRef)plist, CFSTR("CFBundleIdentifier"));
			if (val && CFGetTypeID(val) == CFStringGetTypeID()) {
				ok = CFStringGetCString((CFStringRef)val, out_bid, (CFIndex)cap, kCFStringEncodingUTF8);
			}
		}
		if (plist) CFRelease(plist);
		CFRelease(data);
	}
	free(buf);
	return ok && out_bid[0] != '\0';
}

/* ---- 1. NSHomeDirectory Interposition ---- */

NSString *sweet_NSHomeDirectory(void);
NSString *
sweet_NSHomeDirectory(void)
{
	if (g_active && g_app_container[0] != '\0') {
		return [NSString stringWithUTF8String:g_app_container];
	}
	return NSHomeDirectory();
}
SWEET_INTERPOSE(sweet_NSHomeDirectory, NSHomeDirectory);

/* ---- 2. App Group & NSUserDefaults Suite Shims ---- */

static IMP g_orig_container_url = NULL;
static IMP g_orig_ud_suite = NULL;

static NSURL *
sw_containerURLForSecurityApplicationGroupIdentifier(id self, SEL _cmd, NSString *groupIdentifier)
{
	NSURL *u = NULL;
	if (g_orig_container_url) {
		u = ((NSURL *(*)(id, SEL, NSString *))g_orig_container_url)(self, _cmd, groupIdentifier);
	}
	if (u != nil || !g_active || groupIdentifier.length == 0) {
		return u;
	}
	char clean[192];
	char dir[512];
	char sub[512];
	sanitize_ident(groupIdentifier.UTF8String, clean, sizeof(clean));
	snprintf(dir, sizeof(dir), "/private/var/mobile/Containers/Shared/AppGroup/%s", clean);
	mkdir_p(dir, 0755);
	snprintf(sub, sizeof(sub), "%s/Documents", dir);
	mkdir_p(sub, 0755);
	snprintf(sub, sizeof(sub), "%s/Library/Preferences", dir);
	mkdir_p(sub, 0755);
	snprintf(sub, sizeof(sub), "%s/Library/Caches", dir);
	mkdir_p(sub, 0755);
	appfix_log("AppGroup fallback %s -> %s", groupIdentifier.UTF8String, dir);
	return [NSURL fileURLWithPath:[NSString stringWithUTF8String:dir] isDirectory:YES];
}

static id
sw_initWithSuiteName(id self, SEL _cmd, NSString *suitename)
{
	id res = nil;
	if (g_orig_ud_suite) {
		res = ((id (*)(id, SEL, NSString *))g_orig_ud_suite)(self, _cmd, suitename);
	}
	if (res != nil || !g_active || suitename.length == 0) {
		return res;
	}
	if (g_bundle_id[0] && strcmp(suitename.UTF8String, g_bundle_id) == 0) {
		return nil;
	}
	char clean[192];
	sanitize_ident(suitename.UTF8String, clean, sizeof(clean));
	NSString *alt = [NSString stringWithFormat:@"org.iosonpixel.suite.%s", clean];
	appfix_log("NSUserDefaults suite fallback %s -> %s", suitename.UTF8String, alt.UTF8String);
	if (g_orig_ud_suite) {
		/* Allocate a fresh NSUserDefaults since failed init may have released self */
		id fresh = [NSUserDefaults alloc];
		res = ((id (*)(id, SEL, NSString *))g_orig_ud_suite)(fresh, _cmd, alt);
	}
	return res;
}

/* ---- 3. Keychain -34018 / -25243 Auto-Recovery ---- */

static _Thread_local int g_in_secitem = 0;

static CFDictionaryRef
strip_sec_access_group(CFDictionaryRef src)
{
	if (!src || CFGetTypeID(src) != CFDictionaryGetTypeID()) {
		return NULL;
	}
	bool hasGrp = CFDictionaryContainsKey(src, kSecAttrAccessGroup);
	if (!hasGrp) {
		return NULL;
	}
	CFMutableDictionaryRef m = CFDictionaryCreateMutableCopy(kCFAllocatorDefault, 0, src);
	if (!m) {
		return NULL;
	}
	CFDictionaryRemoveValue(m, kSecAttrAccessGroup);
	return m;
}

OSStatus sweet_SecItemAdd(CFDictionaryRef attributes, CFTypeRef *result);
OSStatus
sweet_SecItemAdd(CFDictionaryRef attributes, CFTypeRef *result)
{
	OSStatus st = SecItemAdd(attributes, result);
	if (g_active && !g_in_secitem && (st == -34018 || st == -25243)) {
		CFDictionaryRef clean = strip_sec_access_group(attributes);
		if (clean) {
			g_in_secitem = 1;
			OSStatus st2 = SecItemAdd(clean, result);
			g_in_secitem = 0;
			appfix_log("SecItemAdd recovered %d -> %d", (int)st, (int)st2);
			CFRelease(clean);
			return st2;
		}
	}
	return st;
}
SWEET_INTERPOSE(sweet_SecItemAdd, SecItemAdd);

OSStatus sweet_SecItemCopyMatching(CFDictionaryRef query, CFTypeRef *result);
OSStatus
sweet_SecItemCopyMatching(CFDictionaryRef query, CFTypeRef *result)
{
	OSStatus st = SecItemCopyMatching(query, result);
	if (g_active && !g_in_secitem && (st == -34018 || st == -25243)) {
		CFDictionaryRef clean = strip_sec_access_group(query);
		if (clean) {
			g_in_secitem = 1;
			OSStatus st2 = SecItemCopyMatching(clean, result);
			g_in_secitem = 0;
			appfix_log("SecItemCopyMatching recovered %d -> %d", (int)st, (int)st2);
			CFRelease(clean);
			return st2;
		}
	}
	return st;
}
SWEET_INTERPOSE(sweet_SecItemCopyMatching, SecItemCopyMatching);

OSStatus sweet_SecItemUpdate(CFDictionaryRef query, CFDictionaryRef attributesToUpdate);
OSStatus
sweet_SecItemUpdate(CFDictionaryRef query, CFDictionaryRef attributesToUpdate)
{
	OSStatus st = SecItemUpdate(query, attributesToUpdate);
	if (g_active && !g_in_secitem && (st == -34018 || st == -25243)) {
		CFDictionaryRef cleanQ = strip_sec_access_group(query);
		CFDictionaryRef cleanA = strip_sec_access_group(attributesToUpdate);
		if (cleanQ || cleanA) {
			g_in_secitem = 1;
			OSStatus st2 = SecItemUpdate(cleanQ ? cleanQ : query, cleanA ? cleanA : attributesToUpdate);
			g_in_secitem = 0;
			appfix_log("SecItemUpdate recovered %d -> %d", (int)st, (int)st2);
			if (cleanQ) CFRelease(cleanQ);
			if (cleanA) CFRelease(cleanA);
			return st2;
		}
	}
	return st;
}
SWEET_INTERPOSE(sweet_SecItemUpdate, SecItemUpdate);

OSStatus sweet_SecItemDelete(CFDictionaryRef query);
OSStatus
sweet_SecItemDelete(CFDictionaryRef query)
{
	OSStatus st = SecItemDelete(query);
	if (g_active && !g_in_secitem && (st == -34018 || st == -25243)) {
		CFDictionaryRef clean = strip_sec_access_group(query);
		if (clean) {
			g_in_secitem = 1;
			OSStatus st2 = SecItemDelete(clean);
			g_in_secitem = 0;
			appfix_log("SecItemDelete recovered %d -> %d", (int)st, (int)st2);
			CFRelease(clean);
			return st2;
		}
	}
	return st;
}
SWEET_INTERPOSE(sweet_SecItemDelete, SecItemDelete);

/* ---- 4. Non-Blocking StoreKit & APNs Shims ---- */

static BOOL
sw_canMakePayments(__unused id self, __unused SEL _cmd)
{
	return NO;
}

static void
sw_skRequestStart(id self, __unused SEL _cmd)
{
	appfix_log("StoreKit request fast-fail: %s", class_getName([self class]));
	dispatch_async(dispatch_get_main_queue(), ^{
		SEL delSel = sel_registerName("delegate");
		if (![self respondsToSelector:delSel]) return;
		id del = ((id (*)(id, SEL))objc_msgSend)(self, delSel);
		SEL failSel = sel_registerName("request:didFailWithError:");
		if (del && [del respondsToSelector:failSel]) {
			NSError *err = [NSError errorWithDomain:@"SKErrorDomain"
			                                   code:2
			                               userInfo:@{NSLocalizedDescriptionKey: @"In-App Purchases unavailable on SWEET"}];
			((void (*)(id, SEL, id, id))objc_msgSend)(del, failSel, self, err);
		}
	});
}

static IMP g_orig_reg_push = NULL;

static void
sw_registerForRemoteNotifications(id self, SEL _cmd)
{
	if (g_orig_reg_push) {
		((void (*)(id, SEL))g_orig_reg_push)(self, _cmd);
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		SEL delSel = sel_registerName("delegate");
		if (![self respondsToSelector:delSel]) return;
		id del = ((id (*)(id, SEL))objc_msgSend)(self, delSel);
		SEL failSel = sel_registerName("application:didFailToRegisterForRemoteNotificationsWithError:");
		if (del && [del respondsToSelector:failSel]) {
			NSError *err = [NSError errorWithDomain:NSCocoaErrorDomain
			                                   code:3010
			                               userInfo:@{NSLocalizedDescriptionKey: @"APNs hardware certificate not present"}];
			((void (*)(id, SEL, id, id))objc_msgSend)(del, failSel, self, err);
		}
	});
}

static void
appfix_image_added(__unused const struct mach_header *mh, __unused intptr_t slide)
{
	os_unfair_lock_lock(&g_hook_lock);

	if (!g_fm_hooked) {
		Class fmCls = objc_getClass("NSFileManager");
		Class udCls = objc_getClass("NSUserDefaults");
		if (fmCls && udCls) {
			Method m1 = class_getInstanceMethod(fmCls, sel_registerName("containerURLForSecurityApplicationGroupIdentifier:"));
			if (m1) g_orig_container_url = method_setImplementation(m1, (IMP)sw_containerURLForSecurityApplicationGroupIdentifier);
			Method m2 = class_getInstanceMethod(udCls, sel_registerName("initWithSuiteName:"));
			if (m2) g_orig_ud_suite = method_setImplementation(m2, (IMP)sw_initWithSuiteName);
			g_fm_hooked = true;
		}
	}

	if (!g_sk_hooked) {
		Class pqCls = objc_getClass("SKPaymentQueue");
		if (pqCls) {
			Method cm = class_getClassMethod(pqCls, sel_registerName("canMakePayments"));
			if (cm) method_setImplementation(cm, (IMP)sw_canMakePayments);
			const char *reqClasses[] = { "SKProductsRequest", "SKReceiptRefreshRequest" };
			for (size_t i = 0; i < 2; i++) {
				Class rc = objc_getClass(reqClasses[i]);
				if (rc) {
					Method sm = class_getInstanceMethod(rc, sel_registerName("_start"));
					if (!sm) sm = class_getInstanceMethod(rc, sel_registerName("start"));
					if (sm) method_setImplementation(sm, (IMP)sw_skRequestStart);
				}
			}
			g_sk_hooked = true;
		}
	}

	if (!g_ui_hooked) {
		Class uiCls = objc_getClass("UIApplication");
		if (uiCls) {
			Method rm = class_getInstanceMethod(uiCls, sel_registerName("registerForRemoteNotifications"));
			if (rm) g_orig_reg_push = method_setImplementation(rm, (IMP)sw_registerForRemoteNotifications);
			g_ui_hooked = true;
		}
	}

	os_unfair_lock_unlock(&g_hook_lock);
}

static void
appfix_exception_handler(NSException *exc)
{
	appfix_log("UNCAUGHT EXCEPTION %s: %s\n%s",
	    exc.name ? exc.name.UTF8String : "?",
	    exc.reason ? exc.reason.UTF8String : "?",
	    exc.callStackSymbols ? exc.callStackSymbols.description.UTF8String : "");
	if (g_prev_exc_handler && g_prev_exc_handler != appfix_exception_handler) {
		g_prev_exc_handler(exc);
	}
}

__attribute__((constructor))
static void
sweet_appfix_init(void)
{
	char exec_path[1024];
	char real_exec[1024];
	uint32_t sz = sizeof(exec_path);

	if (_NSGetExecutablePath(exec_path, &sz) != 0) {
		return;
	}
	const char *ep = realpath(exec_path, real_exec) ? real_exec : exec_path;

	/* Locate enclosing .app bundle directory */
	const char *app_pos = strstr(ep, ".app/");
	if (!app_pos) {
		return;
	}
	size_t app_len = (size_t)((app_pos + 4) - ep);
	if (app_len >= 480) {
		return;
	}
	char app_dir[512];
	memcpy(app_dir, ep, app_len);
	app_dir[app_len] = '\0';

	/* Only activate inside user containers or .sweetstore_installed apps */
	bool is_user_container = (strncmp(app_dir, "/private/var/containers/Bundle/Application/", 43) == 0 ||
	                          strncmp(app_dir, "/var/containers/Bundle/Application/", 35) == 0);
	if (!is_user_container) {
		if (strcmp(app_dir, "/Applications/SweetStore.app") == 0 ||
		    strcmp(app_dir, "/Applications/SweetAuth.app") == 0) {
			return;
		}
		char marker[512];
		snprintf(marker, sizeof(marker), "%s/.sweetstore_installed", app_dir);
		if (access(marker, F_OK) != 0) {
			return;
		}
	}

	if (!read_bundle_id_early(app_dir, g_bundle_id, sizeof(g_bundle_id))) {
		sanitize_ident(getprogname(), g_bundle_id, sizeof(g_bundle_id));
	}

	char clean_bid[256];
	sanitize_ident(g_bundle_id, clean_bid, sizeof(clean_bid));

	/* Redirect HOME / CFFIXED_USER_HOME / TMPDIR to isolated Data container if currently shared /var/mobile */
	const char *cur_home = getenv("CFFIXED_USER_HOME");
	if (!cur_home || !cur_home[0]) {
		cur_home = getenv("HOME");
	}
	if (!cur_home || strcmp(cur_home, "/var/mobile") == 0 ||
	    strcmp(cur_home, "/private/var/mobile") == 0 ||
	    strcmp(cur_home, "/var/root") == 0 ||
	    strcmp(cur_home, "/") == 0) {
		char sub[576];
		snprintf(g_app_container, sizeof(g_app_container),
		    "/private/var/mobile/Containers/Data/Application/%s", clean_bid);
		mkdir_p(g_app_container, 0755);
		const char *dirs[] = {
			"Documents",
			"Library",
			"Library/Preferences",
			"Library/Caches",
			"Library/Application Support",
			"Library/SplashBoard",
			"SystemData",
			"tmp"
		};
		for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
			snprintf(sub, sizeof(sub), "%s/%s", g_app_container, dirs[i]);
			mkdir_p(sub, 0755);
		}
		snprintf(sub, sizeof(sub), "%s/tmp", g_app_container);
		setenv("CFFIXED_USER_HOME", g_app_container, 1);
		setenv("HOME", g_app_container, 1);
		setenv("TMPDIR", sub, 1);
	}

	g_active = true;
	g_prev_exc_handler = NSGetUncaughtExceptionHandler();
	NSSetUncaughtExceptionHandler(&appfix_exception_handler);

	_dyld_register_func_for_add_image(appfix_image_added);
	appfix_image_added(NULL, 0);

	appfix_log("initialized container=%s app=%s",
	    g_app_container[0] ? g_app_container : (cur_home ? cur_home : "?"), app_dir);
}
