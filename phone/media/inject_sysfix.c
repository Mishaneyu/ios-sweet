/*
 * SPDX-License-Identifier: APSL-2.0
 *
 * libsweet_sysfix.dylib: non-intrusive boot & UI responsiveness fixes for
 * containermanagerd and SpringBoard on RMX1931 / SWEET (iOS 16 & iOS 26).
 * Adapted from HackiOS userland/src/inject.c & userland/src/inject_sb.c
 * (https://github.com/oosexgang/hackios-xnu-kernel).
 *
 * Loaded via kern.exec_inject (0200-rmx1931-hackios-peripherals.patch):
 *   - containermanagerd / containermanagerd_system:
 *       * iOS 16 & iOS 26+:
 *           +[MCMUserIdentityCache personasAreSupported] -> NO
 *           Eliminates the 20 x 1 s usermanagerd persona lookup retry stall
 *           and bypasses strict XPC PersonaKernelID propagation checks on
 *           single-volume HFS+ (where containermanagerd proxies system/bundle
 *           containers to containermanagerd_system).
 *       * iOS 26+ (Darwin >= 25):
 *           -[MCMUserIdentity init] creates _defaultUserIdentity with
 *           personaType = 2 (unspecific, isNoSpecificPersona == YES), which
 *           causes legacy/system lookups (mobileactivationd) and container
 *           cache queries (lsd) to fail with MCMError 76 / 115 when
 *           personasAreSupported == NO. In addition, both containermanagerd
 *           (--user-container-mode=fixed, defaultUser=mobile/501) and
 *           containermanagerd_system (--user-container-mode=none, defaultUser=root/0)
 *           have userContainerMode != 2, so userIdentityForClient:error: assigns
 *           defaultUser while userIdentityForPersonalPersonaWithPOSIXUser: uses
 *           client.euid, rejecting cross-UID clients (mobileactivationd uid 0 ->
 *           containermanagerd, lsd uid 501 -> containermanagerd_system) with
 *           MCMError 76. Keep lock_userIdentityForUnspecificPersona as
 *           personaType = 2 while returning defaultUser's personaType = 0
 *           (Personal) identity from defaultUserIdentity,
 *           lock_userIdentityForPersonalPersona, and
 *           userIdentityForPersonalPersonaWithPOSIXUser:.
 *   - backboardd (iOS 26+):
 *       -[CBBacklightNode getBacklightCalibration:] fallback for 625-nit
 *       AppleARMBacklight so _DisplayCreateWithCapabilitiesAndID does not call
 *       __DisplaySliderToLogicalBrightnessInternal before copyMarketingTable
 *       allocates the slider luminance table.
 *   - SpringBoard:
 *       +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] -> JPEG / PNG
 *       Avoids multi-second CPU ASTC (.atx) texture compression when apps go to
 *       background, preventing 10 s scene-update watchdog terminations.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <notify.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <os/lock.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
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

static int mcm_log_fd = -1;

static void
mcm_logf(const char *format, ...)
{
	struct stat st;
	char buf[384];
	va_list ap;
	int n;

	if (mcm_log_fd < 0) {
		return;
	}
	if (fstat(mcm_log_fd, &st) == 0 && st.st_size < 65536) {
		va_start(ap, format);
		n = vsnprintf(buf, sizeof(buf), format, ap);
		va_end(ap);
		if (n > 0) {
			if (n > (int)sizeof(buf) - 1) {
				n = (int)sizeof(buf) - 1;
			}
			(void)write(mcm_log_fd, buf, (size_t)n);
		}
	}
}

static int
darwin_major(void)
{
	char rel[32];
	size_t len = sizeof(rel) - 1;

	memset(rel, 0, sizeof(rel));
	if (sysctlbyname("kern.osrelease", rel, &len, NULL, 0) != 0) {
		return 0;
	}
	return atoi(rel);
}

/* ---- containermanagerd: +[MCMUserIdentityCache personasAreSupported] ---- */

static BOOL
personas_are_supported(__unused id self, __unused SEL selector)
{
	return NO;
}

/* ---- containermanagerd (iOS 26+): Personal vs Unspecific identity split ---- */

static Class cls_user_identity;
static IMP orig_default_identity;
static IMP orig_persona_id;
static IMP orig_mcm_error_init;
static IMP orig_mcm_error_init_cat;
static SEL sel_default_identity;
static SEL sel_alloc;
static SEL sel_release;
static SEL sel_posix_user;
static SEL sel_home_dir_url;
static SEL sel_kernel_persona_id;
static SEL sel_init_posix;
static SEL sel_utf8_string;
static SEL sel_path_str;
static os_unfair_lock personal_identity_lock = OS_UNFAIR_LOCK_INIT;
static const char personal_identity_key;

static id
cm26_default_identity(id self, __unused SEL selector)
{
	id raw, personal, posix_user, home_url, obj;
	uint32_t kpid;

	if (self == NULL || orig_default_identity == NULL) {
		return NULL;
	}
	raw = ((id (*)(id, SEL))orig_default_identity)(self, sel_default_identity);
	if (raw == NULL || cls_user_identity == NULL) {
		return raw;
	}
	personal = objc_getAssociatedObject(raw, &personal_identity_key);
	if (personal != NULL) {
		return personal;
	}

	os_unfair_lock_lock(&personal_identity_lock);
	personal = objc_getAssociatedObject(raw, &personal_identity_key);
	if (personal == NULL) {
		posix_user = ((id (*)(id, SEL))objc_msgSend)(raw, sel_posix_user);
		home_url = ((id (*)(id, SEL))objc_msgSend)(raw, sel_home_dir_url);
		kpid = ((uint32_t (*)(id, SEL))objc_msgSend)(raw, sel_kernel_persona_id);
		if (posix_user != NULL && home_url != NULL) {
			obj = ((id (*)(id, SEL))objc_msgSend)((id)cls_user_identity, sel_alloc);
			if (obj != NULL) {
				personal = ((id (*)(id, SEL, id, id, id, uint64_t, uint32_t))objc_msgSend)(
				    obj, sel_init_posix, posix_user, home_url, NULL, 0ULL, kpid);
				if (personal != NULL) {
					objc_setAssociatedObject(raw, &personal_identity_key,
					    personal, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
					((void (*)(id, SEL))objc_msgSend)(personal, sel_release);
				}
			}
		}
	}
	os_unfair_lock_unlock(&personal_identity_lock);

	return personal != NULL ? personal : raw;
}

static id
cm26_lock_personal(id self, __unused SEL selector)
{
	return cm26_default_identity(self, sel_default_identity);
}

static id
cm26_personal_with_posix_user(id self, __unused SEL selector, __unused id posix_user)
{
	return cm26_default_identity(self, sel_default_identity);
}

static id
cm26_lock_unspecific(id self, __unused SEL selector)
{
	if (self == NULL || orig_default_identity == NULL) {
		return NULL;
	}
	return ((id (*)(id, SEL))orig_default_identity)(self, sel_default_identity);
}

static id
cm26_persona_id(id self, SEL selector, uint32_t persona_id)
{
	id res = ((id (*)(id, SEL, uint32_t))orig_persona_id)(self, selector, persona_id);

	if (res != NULL) {
		return res;
	}
	return cm26_default_identity(self, sel_default_identity);
}

static const char *
nsstr_cstr(id obj)
{
	if (obj == NULL || sel_utf8_string == NULL) {
		return "-";
	}
	if (class_getInstanceMethod(object_getClass(obj), sel_utf8_string) != NULL) {
		const char *s = ((const char *(*)(id, SEL))objc_msgSend)(obj, sel_utf8_string);
		return s != NULL ? s : "-";
	}
	if (sel_path_str != NULL &&
	    class_getInstanceMethod(object_getClass(obj), sel_path_str) != NULL) {
		id p = ((id (*)(id, SEL))objc_msgSend)(obj, sel_path_str);
		if (p != NULL && class_getInstanceMethod(object_getClass(p), sel_utf8_string) != NULL) {
			const char *s = ((const char *(*)(id, SEL))objc_msgSend)(p, sel_utf8_string);
			return s != NULL ? s : "-";
		}
	}
	return "?";
}

static id
cm26_error_init(id self, SEL selector, uint64_t type, uint64_t category,
    id path, int posix_errno)
{
	id res = ((id (*)(id, SEL, uint64_t, uint64_t, id, int))orig_mcm_error_init)(
	    self, selector, type, category, path, posix_errno);

	if (res != NULL && type > 1) {
		mcm_logf("%s[%d]: MCMError type=%llu cat=%llu errno=%d path=%s\n",
		    getprogname(), (int)getpid(),
		    (unsigned long long)type, (unsigned long long)category,
		    posix_errno, nsstr_cstr(path));
	}
	return res;
}

static id
cm26_error_init_cat(id self, SEL selector, uint64_t type, uint64_t category)
{
	id res = ((id (*)(id, SEL, uint64_t, uint64_t))orig_mcm_error_init_cat)(
	    self, selector, type, category);

	if (res != NULL && type > 1) {
		mcm_logf("%s[%d]: MCMError type=%llu cat=%llu\n",
		    getprogname(), (int)getpid(),
		    (unsigned long long)type, (unsigned long long)category);
	}
	return res;
}

/*
 * iOS 26+ HFS+ APFS-fsctl & POSIX.1e extended-ACL bypasses:
 *   - -[MCMFileHandle registerAppContainerForProtectionWithError:] calls APFS
 *     container-protection fsctl and only accepts 0 or EEXIST (not ENOTSUP),
 *     causing -[MCMContainerFactory _createContainerForContainerIdentity:error:]
 *     to roll back and delete every newly created container on HFS+.
 *   - -[MCMFileHandle checkAppContainerProtection:error:] fails with ENOTSUP
 *     (MCMError 144 / 161) and discards queried containers on HFS+.
 *   - -[MCMFileManager enableFastDiskUsageForURL:error:] calls APFSIOC_DIR_STATS_OP
 *     (0xc1104a71), failing with ENOTSUP on HFS+.
 *   - System (class 12) and SystemGroup (class 13) containers call
 *     -[MCMFileManager setTopLevelSystemContainerACLAtURL:error:] (acl_set_link_np)
 *     which fails with ENOTSUP on HFS+ and rolls back every staged system/group
 *     container. In addition, MCMContainerPath sets schemaPOSIXMode = 0000 and
 *     schemaPOSIXOwner = nobody (-2) when systemContainerMode == 1, relying on
 *     inherited APFS ACLs for multi-UID access (lsd uid=501, mobileactivationd
 *     uid=0, installd uid=33). Bypass the unsupported POSIX.1e ACL setters and
 *     assign 0777 + container owner on System/SystemGroup paths so HFS+ UNIX DAC
 *     matches Apple's inherited system-container ACL semantics under Seatbelt.
 */
static IMP orig_root_posix_mode;
static IMP orig_data_posix_mode;
static IMP orig_schema_posix_mode;
static IMP orig_subdir_posix_owner;
static IMP orig_create_dir;
static SEL sel_container_class;

#ifndef DT_DIR
#define DT_DIR 4
#endif

static void
chmod_sys_dirs_recursive(const char *path, int depth)
{
	DIR *dir;
	struct dirent *de;
	char sub[1024];
	int n;

	if (path == NULL || path[0] != '/' || depth > 4) {
		return;
	}
	(void)chmod(path, 0777);
	if (depth == 4) {
		return;
	}
	dir = opendir(path);
	if (dir == NULL) {
		return;
	}
	while ((de = readdir(dir)) != NULL) {
		if (de->d_type != DT_DIR) {
			continue;
		}
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0 ||
		    strcmp(de->d_name, "DiagnosticReports") == 0 ||
		    strcmp(de->d_name, "MemoryLimitViolations") == 0) {
			continue;
		}
		n = snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
		if (n > 0 && n < (int)sizeof(sub)) {
			chmod_sys_dirs_recursive(sub, depth + 1);
		}
	}
	(void)closedir(dir);
}

static void
repair_existing_sys_containers(void)
{
	static const char *const roots[] = {
		"/private/var/containers/Data/System",
		"/private/var/containers/Shared/SystemGroup",
	};
	DIR *dir;
	struct dirent *de;
	char sub[1024];
	size_t i;
	int n;

	for (i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		dir = opendir(roots[i]);
		if (dir == NULL) {
			continue;
		}
		while ((de = readdir(dir)) != NULL) {
			if (de->d_type != DT_DIR ||
			    strcmp(de->d_name, ".") == 0 ||
			    strcmp(de->d_name, "..") == 0) {
				continue;
			}
			n = snprintf(sub, sizeof(sub), "%s/%s", roots[i], de->d_name);
			if (n > 0 && n < (int)sizeof(sub)) {
				chmod_sys_dirs_recursive(sub, 0);
			}
		}
		(void)closedir(dir);
	}
}

static BOOL
cm26_register_protection(__unused id self, __unused SEL selector, __unused id *out_error)
{
	return YES;
}

static BOOL
cm26_check_protection(__unused id self, __unused SEL selector,
    BOOL *out_protected, __unused id *out_error)
{
	if (out_protected != NULL) {
		*out_protected = YES;
	}
	return YES;
}

static BOOL
cm26_enable_fast_disk_usage(__unused id self, __unused SEL selector,
    __unused id url, __unused id *out_error)
{
	return YES;
}

static BOOL
cm26_set_top_level_sys_acl(__unused id self, __unused SEL selector,
    id url, __unused id *out_error)
{
	const char *p = nsstr_cstr(url);

	if (p != NULL && p[0] == '/') {
		chmod_sys_dirs_recursive(p, 0);
	}
	return YES;
}

static BOOL
cm26_std_sys_acl(__unused id self, __unused SEL selector,
    id url, __unused id *out_error)
{
	const char *p = nsstr_cstr(url);

	if (p != NULL && p[0] == '/') {
		chmod_sys_dirs_recursive(p, 0);
	}
	return YES;
}

static BOOL
cm26_url_acl_noop(__unused id self, __unused SEL selector,
    __unused id url, __unused id *out_error)
{
	return YES;
}

static BOOL
cm26_standardize_acl_noop(__unused id self, __unused SEL selector,
    id url, BOOL is_sys, __unused id *out_error)
{
	const char *p;

	if (is_sys) {
		p = nsstr_cstr(url);
		if (p != NULL && p[0] == '/') {
			chmod_sys_dirs_recursive(p, 0);
		}
	}
	return YES;
}

static BOOL
cm26_create_dir(id self, SEL selector, id url, BOOL with_inter, uint16_t mode,
    id owner, int dp_class, void *fs_node, id *out_error)
{
	BOOL ok;
	const char *p;

	ok = ((BOOL (*)(id, SEL, id, BOOL, uint16_t, id, int, void *, id *))orig_create_dir)(
	    self, selector, url, with_inter, mode, owner, dp_class, fs_node, out_error);
	if (ok && (mode & 0777) == 0777) {
		p = nsstr_cstr(url);
		if (p != NULL && p[0] == '/') {
			(void)chmod(p, 0777);
		}
	}
	return ok;
}

static BOOL
cm26_fix_acl_on_fd(__unused id self, __unused SEL selector,
    __unused int fd, __unused void *rem_fsec, __unused void *dd_fsec,
    __unused const char *dd_text, __unused const char *path,
    __unused id *out_error)
{
	return YES;
}

static bool
cm26_is_system_or_group_path(id self)
{
	uint64_t ccls;

	if (self == NULL || sel_container_class == NULL) {
		return false;
	}
	ccls = ((uint64_t (*)(id, SEL))objc_msgSend)(self, sel_container_class);
	return (ccls & ~1ULL) == 12ULL;
}

static uint16_t
cm26_root_posix_mode(id self, SEL selector)
{
	if (cm26_is_system_or_group_path(self)) {
		return 0777;
	}
	return orig_root_posix_mode != NULL ?
	    ((uint16_t (*)(id, SEL))orig_root_posix_mode)(self, selector) : 0755;
}

static uint16_t
cm26_data_posix_mode(id self, SEL selector)
{
	if (cm26_is_system_or_group_path(self)) {
		return 0777;
	}
	return orig_data_posix_mode != NULL ?
	    ((uint16_t (*)(id, SEL))orig_data_posix_mode)(self, selector) : 0755;
}

static uint16_t
cm26_schema_posix_mode(id self, SEL selector)
{
	if (cm26_is_system_or_group_path(self)) {
		return 0777;
	}
	return orig_schema_posix_mode != NULL ?
	    ((uint16_t (*)(id, SEL))orig_schema_posix_mode)(self, selector) : 0755;
}

static id
cm26_subdir_posix_owner(id self, SEL selector, uint64_t container_class, id user)
{
	if ((container_class & ~1ULL) == 12ULL && user != NULL) {
		return user;
	}
	return orig_subdir_posix_owner != NULL ?
	    ((id (*)(id, SEL, uint64_t, id))orig_subdir_posix_owner)(
	        self, selector, container_class, user) : user;
}

static bool cm_hooked;

static void
cm_image_added(__unused const struct mach_header *header, __unused intptr_t slide)
{
	Class cls, fh_cls, fm_cls, cp_cls, err_cls;
	Method m;
	int dmaj;

	if (cm_hooked || (cls = objc_getClass("MCMUserIdentityCache")) == NULL) {
		return;
	}
	cm_hooked = true;
	dmaj = darwin_major();

	m = class_getClassMethod(cls, sel_registerName("personasAreSupported"));
	if (m != NULL) {
		method_setImplementation(m, (IMP)personas_are_supported);
		say("sweet: %s: +[MCMUserIdentityCache personasAreSupported] -> NO\n",
		    getprogname());
	}
	if (dmaj < 25) {
		return;
	}
	if (mcm_log_fd < 0) {
		mcm_log_fd = open("/private/var/local/mcm.log",
		    O_WRONLY | O_APPEND | O_CLOEXEC);
	}

	cls_user_identity = objc_getClass("MCMUserIdentity");
	sel_default_identity = sel_registerName("defaultUserIdentity");
	sel_alloc = sel_registerName("alloc");
	sel_release = sel_registerName("release");
	sel_posix_user = sel_registerName("posixUser");
	sel_home_dir_url = sel_registerName("homeDirectoryURL");
	sel_kernel_persona_id = sel_registerName("kernelPersonaID");
	sel_init_posix = sel_registerName(
	    "initWithPOSIXUser:homeDirectoryURL:personaUniqueString:personaType:kernelPersonaID:");
	sel_utf8_string = sel_registerName("UTF8String");
	sel_path_str = sel_registerName("path");
	sel_container_class = sel_registerName("containerClass");

	m = class_getInstanceMethod(cls, sel_default_identity);
	if (m != NULL) {
		orig_default_identity = method_setImplementation(m, (IMP)cm26_default_identity);
	}
	m = class_getInstanceMethod(cls, sel_registerName("lock_userIdentityForPersonalPersona"));
	if (m != NULL) {
		(void)method_setImplementation(m, (IMP)cm26_lock_personal);
	}
	m = class_getInstanceMethod(cls,
	    sel_registerName("userIdentityForPersonalPersonaWithPOSIXUser:"));
	if (m != NULL) {
		(void)method_setImplementation(m, (IMP)cm26_personal_with_posix_user);
	}
	m = class_getInstanceMethod(cls, sel_registerName("lock_userIdentityForUnspecificPersona"));
	if (m != NULL) {
		(void)method_setImplementation(m, (IMP)cm26_lock_unspecific);
	}
	m = class_getInstanceMethod(cls, sel_registerName("userIdentityWithPersonaID:"));
	if (m != NULL) {
		orig_persona_id = method_setImplementation(m, (IMP)cm26_persona_id);
	}

	fh_cls = objc_getClass("MCMFileHandle");
	if (fh_cls != NULL) {
		m = class_getInstanceMethod(fh_cls,
		    sel_registerName("registerAppContainerForProtectionWithError:"));
		if (m != NULL) {
			(void)method_setImplementation(m, (IMP)cm26_register_protection);
		}
		m = class_getInstanceMethod(fh_cls,
		    sel_registerName("checkAppContainerProtection:error:"));
		if (m != NULL) {
			(void)method_setImplementation(m, (IMP)cm26_check_protection);
		}
	}
	fm_cls = objc_getClass("MCMFileManager");
	if (fm_cls != NULL) {
		m = class_getInstanceMethod(fm_cls,
		    sel_registerName("enableFastDiskUsageForURL:error:"));
		if (m != NULL) {
			(void)method_setImplementation(m, (IMP)cm26_enable_fast_disk_usage);
		}
		m = class_getInstanceMethod(fm_cls,
		    sel_registerName("setTopLevelSystemContainerACLAtURL:error:"));
		if (m != NULL) {
			(void)method_setImplementation(m, (IMP)cm26_set_top_level_sys_acl);
		}
		m = class_getInstanceMethod(fm_cls,
		    sel_registerName("standardizeACLsForSystemContainerAtURL:error:"));
		if (m != NULL) {
			(void)method_setImplementation(m, (IMP)cm26_std_sys_acl);
		}
		m = class_getInstanceMethod(fm_cls,
		    sel_registerName("standardizeACLsAtURL:isSystemContainer:error:"));
		if (m != NULL) {
			(void)method_setImplementation(m, (IMP)cm26_standardize_acl_noop);
		}
		m = class_getInstanceMethod(fm_cls,
		    sel_registerName("standardizeAllSystemContainerACLsAtURL:error:"));
		if (m != NULL) {
			(void)method_setImplementation(m, (IMP)cm26_std_sys_acl);
		}
		m = class_getInstanceMethod(fm_cls,
		    sel_registerName("stripACLFromURL:error:"));
		if (m != NULL) {
			(void)method_setImplementation(m, (IMP)cm26_url_acl_noop);
		}
		m = class_getInstanceMethod(fm_cls,
		    sel_registerName("_fixACLOnFD:removeACLFilesec:denyDeleteFilesec:denyDeleteText:path:error:"));
		if (m != NULL) {
			(void)method_setImplementation(m, (IMP)cm26_fix_acl_on_fd);
		}
		m = class_getInstanceMethod(fm_cls,
		    sel_registerName("createDirectoryAtURL:withIntermediateDirectories:mode:owner:dataProtectionClass:fsNode:error:"));
		if (m != NULL) {
			orig_create_dir = method_setImplementation(m, (IMP)cm26_create_dir);
		}
	}
	if (geteuid() == 0) {
		repair_existing_sys_containers();
	}
	cp_cls = objc_getClass("MCMContainerPath");
	if (cp_cls != NULL) {
		m = class_getInstanceMethod(cp_cls, sel_registerName("rootPOSIXMode"));
		if (m != NULL) {
			orig_root_posix_mode = method_setImplementation(m,
			    (IMP)cm26_root_posix_mode);
		}
		m = class_getInstanceMethod(cp_cls, sel_registerName("dataPOSIXMode"));
		if (m != NULL) {
			orig_data_posix_mode = method_setImplementation(m,
			    (IMP)cm26_data_posix_mode);
		}
		m = class_getInstanceMethod(cp_cls, sel_registerName("schemaPOSIXMode"));
		if (m != NULL) {
			orig_schema_posix_mode = method_setImplementation(m,
			    (IMP)cm26_schema_posix_mode);
		}
		m = class_getClassMethod(cp_cls,
		    sel_registerName("_posixOwnerForContainerSubdirectories:user:"));
		if (m != NULL) {
			orig_subdir_posix_owner = method_setImplementation(m,
			    (IMP)cm26_subdir_posix_owner);
		}
	}

	err_cls = objc_getClass("MCMError");
	if (err_cls != NULL) {
		m = class_getInstanceMethod(err_cls,
		    sel_registerName("initWithErrorType:category:path:POSIXerrno:"));
		if (m != NULL) {
			orig_mcm_error_init = method_setImplementation(m, (IMP)cm26_error_init);
		}
		m = class_getInstanceMethod(err_cls,
		    sel_registerName("initWithErrorType:category:"));
		if (m != NULL) {
			orig_mcm_error_init_cat = method_setImplementation(m,
			    (IMP)cm26_error_init_cat);
		}
	}
	mcm_logf("sweet: %s[%d]: hooked MCMUserIdentityCache + HFS+ protection + ACLs (darwin=%d uid=%u)\n",
	    getprogname(), (int)getpid(), dmaj, (unsigned)geteuid());
	say("sweet: %s: MCMUserIdentityCache + HFS+ protection & ACL hooks installed\n",
	    getprogname());
}

/* ---- SpringBoard: snapshot format & iOS 26+ CloudConfiguration bypass ---- */

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

static BOOL
sb26_return_no(__unused id self, __unused SEL selector)
{
	return NO;
}

static bool sb_xb_hooked;
static bool sb_mc_hooked;

static void
sb_image_added(__unused const struct mach_header *header, __unused intptr_t slide)
{
	Class cls;
	Method method;

	if (!sb_xb_hooked &&
	    (cls = objc_getClass("XBApplicationSnapshotManifestImpl")) != NULL) {
		sb_xb_hooked = true;
		method = class_getClassMethod(cls,
		    sel_registerName("_outputFormatForSnapshot:"));
		if (method != NULL) {
			method_setImplementation(method, (IMP)snapshot_output_format);
			say("sweet: SpringBoard: +[XBApplicationSnapshotManifestImpl _outputFormatForSnapshot:] -> JPEG/PNG\n");
		}
	}
	if (!sb_mc_hooked && darwin_major() >= 25 &&
	    (cls = objc_getClass("MCProfileConnection")) != NULL) {
		sb_mc_hooked = true;
		method = class_getInstanceMethod(cls,
		    sel_registerName("shouldShowSetupAssistant"));
		if (method != NULL) {
			method_setImplementation(method, (IMP)sb26_return_no);
		}
		method = class_getInstanceMethod(cls,
		    sel_registerName("shouldShowCloudConfigurationUI"));
		if (method != NULL) {
			method_setImplementation(method, (IMP)sb26_return_no);
		}
		say("sweet: SpringBoard: -[MCProfileConnection shouldShowSetupAssistant/CloudConfigurationUI] -> NO\n");
	}
}

static int sb_ringer_token = -1;

static void *
sb_ringer_unmute_thread(__unused void *arg)
{
	int tok = -1;

	sleep(2);
	if (notify_register_check("com.apple.springboard.ringerstate", &tok) == 0) {
		sb_ringer_token = tok;
		(void)notify_set_state(tok, 1);
	}
	(void)notify_post("com.apple.springboard.ringerstate");
	say("sweet: SpringBoard: com.apple.springboard.ringerstate -> 1 (+2s)\n");

	sleep(8);
	if (sb_ringer_token >= 0) {
		(void)notify_set_state(sb_ringer_token, 1);
	}
	(void)notify_post("com.apple.springboard.ringerstate");
	say("sweet: SpringBoard: com.apple.springboard.ringerstate -> 1 (+10s)\n");
	return NULL;
}

static void
sb_start_ringer_unmute(void)
{
	pthread_t thr;

	if (pthread_create(&thr, NULL, sb_ringer_unmute_thread, NULL) == 0) {
		(void)pthread_detach(thr);
	}
}

/* ---- backboardd (iOS 26+): -[CBBacklightNode getBacklightCalibration:] ---- */

static IMP orig_get_backlight_calib;
static bool bb_hooked;

static BOOL
bb26_get_backlight_calib(id self, SEL selector, float *out)
{
	if (orig_get_backlight_calib != NULL &&
	    ((BOOL (*)(id, SEL, float *))orig_get_backlight_calib)(self, selector, out)) {
		return YES;
	}
	if (out == NULL) {
		return NO;
	}
	/* Matches AppleARMBacklight (625 nits max, 155 nits mid, 2 nits min, 100 mA max) */
	out[0] = 100.0f;	/* I_max */
	out[1] = 25.0f;		/* I_mid */
	out[2] = 100.0f;	/* Ihw_max */
	out[3] = 625.0f;	/* Lmax_product */
	out[4] = 155.0f;	/* Lmid_product */
	out[5] = 2.0f;		/* Lmin_product */
	out[6] = 625.0f;	/* Lmax_panel */
	out[7] = 155.0f;	/* Lmid_panel */
	out[8] = 2.0f;		/* Lmin_panel */
	out[9] = 625.0f;	/* milliAmps2NitsScaleFactor */
	return YES;
}

static void
bb_image_added(__unused const struct mach_header *header, __unused intptr_t slide)
{
	Class cls;
	Method method;

	if (bb_hooked || (cls = objc_getClass("CBBacklightNode")) == NULL) {
		return;
	}
	bb_hooked = true;
	method = class_getInstanceMethod(cls, sel_registerName("getBacklightCalibration:"));
	if (method != NULL) {
		orig_get_backlight_calib = method_setImplementation(method,
		    (IMP)bb26_get_backlight_calib);
		say("sweet: backboardd: -[CBBacklightNode getBacklightCalibration:] -> 625-nit fallback\n");
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
		cm_image_added(NULL, 0);
	} else if (strcmp(name, "backboardd") == 0) {
		if (darwin_major() >= 25) {
			_dyld_register_func_for_add_image(bb_image_added);
			bb_image_added(NULL, 0);
		}
	} else if (strcmp(name, "SpringBoard") == 0) {
		_dyld_register_func_for_add_image(sb_image_added);
		sb_image_added(NULL, 0);
		sb_start_ringer_unmute();
	}
}
