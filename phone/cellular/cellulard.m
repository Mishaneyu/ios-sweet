/*
 * cellulard: iOS 26.5.2 cellular & SIM bring-up daemon for SM8150 (RMX1931).
 *
 * Bridges Qualcomm MPSS QMI services (DMS, UIM, NAS, WDA, WDS) over wland's
 * /var/run/sweet-qrtr.sock multiplexer into iOS 26:
 *   1. SystemStatus (STTelephonyStatusDomainPublisher) -> Lock/Home status bar
 *   2. com.apple.commcenter.coretelephony.xpc (NSXPCListener, CTXPCServiceInterface)
 *      -> Settings -> Cellular, Settings -> General -> About, systemstatusd
 *   3. com.apple.commcenter.xpc (libxpc Mach service)
 *      -> CTServerConnection dictionary queries from SpringBoard & systemstatusd
 *
 * PRIVACY: Never requests, stores, logs, or returns IMEI, IMEISV, IMSI, ICCID,
 * or subscriber phone numbers (MSISDN).
 */
#import <Foundation/Foundation.h>
#import <CoreTelephony/CTTelephonyNetworkInfo.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <os/lock.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "qmi_cellular.h"

#define CELLULARD_LOG		"/var/local/cellulard.log"
#define CELLULARD_STAMP		"/var/local/cellulard.boot"
#define CELLULARD_JSON		"/var/local/cellular-status.json"
#define CELLULARD_CONF		"/var/local/cellular.conf"
#define RADIOS_PLIST		"/private/var/preferences/SystemConfiguration/com.apple.radios.plist"
#define SLOT1_UUID_STR		"00000001-0000-4000-8000-000000000001"

/* ---- Typed SPI interfaces (resolved dynamically via NSClassFromString) ---- */

@interface SweetXPCInterfaceSPI : NSObject
+ (instancetype)interfaceWithProtocol:(Protocol *)protocol;
- (void)setClasses:(NSSet<Class> *)classes forSelector:(SEL)sel
     argumentIndex:(NSUInteger)arg ofReply:(BOOL)ofReply;
@end

@interface SweetXPCConnectionSPI : NSObject
@property (nonatomic, strong) id exportedInterface;
@property (nonatomic, strong) id exportedObject;
@property (nonatomic, strong) id remoteObjectInterface;
@property (nonatomic, copy) void (^invalidationHandler)(void);
@property (nonatomic, copy) void (^interruptionHandler)(void);
@property (nonatomic, readonly) int processIdentifier;
- (id)remoteObjectProxyWithErrorHandler:(void (^)(NSError *error))handler;
- (void)resume;
- (void)invalidate;
@end

@interface SweetXPCListenerSPI : NSObject
@property (nonatomic, weak) id delegate;
- (instancetype)initWithMachServiceName:(NSString *)name;
- (void)resume;
@end

/* SystemStatus private classes */
@interface SweetSTMutableSIMInfo : NSObject
- (void)setSIMPresent:(BOOL)v;
- (void)setBootstrap:(BOOL)v;
- (void)setLabel:(NSString *)v;
- (void)setShortLabel:(NSString *)v;
- (void)setSignalStrengthBars:(NSUInteger)v;
- (void)setMaxSignalStrengthBars:(NSUInteger)v;
- (void)setServiceState:(NSUInteger)v;
- (void)setCellularServiceState:(NSUInteger)v;
- (void)setServiceDescription:(NSString *)v;
- (void)setSecondaryServiceDescription:(NSString *)v;
- (void)setDataNetworkType:(NSUInteger)v;
- (void)setProvidingDataConnection:(BOOL)v;
- (void)setPreferredForDataConnections:(BOOL)v;
- (void)setRegisteredWithoutCellular:(BOOL)v;
- (void)setCallForwardingEnabled:(BOOL)v;
@end

@interface SweetSTMutableTelephonyData : NSObject
- (void)setCellularRadioCapabilityEnabled:(BOOL)v;
- (void)setDualSIMEnabled:(BOOL)v;
- (void)setRadioModuleDead:(BOOL)v;
- (void)setInactiveSOSEnabled:(BOOL)v;
- (void)setUsingStewieForSOS:(BOOL)v;
- (void)setUsingStewieConnection:(BOOL)v;
- (void)updateSIMOneInfoWithBlock:(void (^)(SweetSTMutableSIMInfo *sim))block;
- (void)updateSIMTwoInfoWithBlock:(void (^)(SweetSTMutableSIMInfo *sim))block;
@end

@interface SweetSTTelephonyPublisher : NSObject
- (void)updateDataWithBlock:(void (^)(SweetSTMutableTelephonyData *data))block;
@end

/* CoreTelephony private constructors & delegate notification methods */
@interface SweetCTContextInfo : NSObject
+ (instancetype)contextWithUUID:(NSUUID *)uuid andAccountID:(NSString *)acct andSlot:(NSInteger)slot;
@end

@interface SweetCTServiceDescriptor : NSObject
+ (instancetype)telephonyDescriptorWithInstance:(NSNumber *)inst;
@end

@interface SweetCTServiceDescriptorContainer : NSObject
- (instancetype)initWithDescriptors:(NSArray *)descs;
@end

@interface SweetCTSimLabel : NSObject
- (instancetype)initWithId:(NSString *)uid andText:(NSString *)text andDidSetup:(BOOL)setup;
@end

@interface SweetCTSimHardwareInfo : NSObject
- (instancetype)initWithHardwareInfo:(NSInteger)hwType simLocation:(NSInteger)loc;
@end

@interface SweetCTDisplayPlanList : NSObject
- (instancetype)initWithDisplayPlans:(NSArray *)plans;
@end

@interface SweetCTSupportedMaxDataRates : NSObject
- (instancetype)initWithRates:(NSArray *)rates;
@end

@interface SweetCTEncryptionStatusInfo : NSObject
- (instancetype)initWithStatus:(NSInteger)st;
@end

/* Distinct typed response wrappers so ARC never sees ambiguous initWithResult: */
@interface SweetCTBoolResp : NSObject
- (instancetype)initWithResult:(BOOL)r;
@end

@interface SweetCTObjResp : NSObject
- (instancetype)initWithResult:(id)r;
@end

@interface SweetCTEnabledResp : NSObject
- (instancetype)initWithIsEnabled:(BOOL)en;
@end

@interface SweetCTInfoResp : NSObject
- (instancetype)initWithInfo:(id)info;
@end

@interface SweetCTStatusResp : NSObject
- (instancetype)initWithStatus:(id)st;
@end

@interface SweetCTSigInfoResp : NSObject
- (instancetype)initWithSignalStrengthInfo:(id)info;
@end

@interface SweetCTSigMeasResp : NSObject
- (instancetype)initWithSignalStrengthMeasurements:(id)m;
@end

@interface SweetCTOpNameResp : NSObject
- (instancetype)initWithOperatorName:(NSString *)name;
@end

@interface SweetCTRoamingResp : NSObject
- (instancetype)initWithRoamingStatus:(id)st;
@end

@interface SweetCTCurRatResp : NSObject
- (instancetype)initWithRat:(NSString *)rat;
@end

@interface SweetCTRegRatResp : NSObject
- (instancetype)initWithRAT:(NSString *)rat;
@end

@interface SweetCTTechResp : NSObject
- (instancetype)initWithTechnology:(NSString *)tech;
@end

@interface SweetCTRatSelResp : NSObject
- (instancetype)initWithRATSelection:(id)sel;
@end

@interface SweetCTDataAttResp : NSObject
- (instancetype)initWithIsDataAttached:(BOOL)att;
@end

@interface SweetCTHomeResp : NSObject
- (instancetype)initWithIsHome:(BOOL)home;
@end

@interface SweetCTAvailResp : NSObject
- (instancetype)initWithIsAvailable:(BOOL)avail;
@end

@interface SweetCTReselResp : NSObject
- (instancetype)initWithNeedsReselection:(BOOL)need;
@end

@interface SweetCTMccResp : NSObject
- (instancetype)initWithMcc:(NSString *)mcc;
@end

@interface SweetCTLastMccResp : NSObject
- (instancetype)initWithLastMcc:(NSString *)mcc;
@end

@interface SweetCTMncResp : NSObject
- (instancetype)initWithMnc:(NSString *)mnc;
@end

@interface SweetCTPlmnResp : NSObject
- (instancetype)initWithPlmn:(NSString *)plmn;
@end

@interface SweetCTLabelResp : NSObject
- (instancetype)initWithLabel:(id)lbl;
@end

@interface SweetCTHwResp : NSObject
- (instancetype)initWithHardwareInfo:(id)hw;
@end

@interface SweetCTNRResp : NSObject
- (instancetype)initWithNRStatus:(id)nr;
@end

@interface SweetCTSupportResp : NSObject
- (instancetype)initWithSupport:(BOOL)sup;
@end

@interface SweetCTDataRateResp : NSObject
- (instancetype)initWithDataRate:(NSInteger)rate;
@end

@interface SweetCTSupRatesResp : NSObject
- (instancetype)initWithSupportedRates:(id)rates;
@end

@interface SweetCTBandResp : NSObject
- (instancetype)initWithBandInfo:(id)bi;
@end

@interface SweetCTCtxResp : NSObject
- (instancetype)initWithContext:(id)ctx;
@end

@interface SweetCTNetSelInfoResp : NSObject
- (instancetype)initWithNetworkSelectionInfo:(id)info;
@end

@interface SweetCTNetResp : NSObject
- (instancetype)initWithNetwork:(id)net;
@end

@interface SweetCTNetListResp : NSObject
- (instancetype)initWithNetworkList:(id)list;
@end

@interface SweetCTIMSResp : NSObject
- (instancetype)initWithIMSRegistrationStatus:(id)ims;
@end

@interface SweetCTIMSTransResp : NSObject
- (instancetype)initWithIMSRegistrationInfo:(id)ims;
@end

@interface SweetCTEncResp : NSObject
- (instancetype)initWithEncryptionStatusInfo:(id)enc;
@end

@interface SweetCTCauseResp : NSObject
- (instancetype)initWithCauseCode:(NSNumber *)cc;
@end

@interface SweetCTPlanListResp : NSObject
- (instancetype)initWithPlanList:(id)list;
@end

@interface SweetCTListResp : NSObject
- (instancetype)initWithList:(id)list;
@end

@interface SweetCTDevsResp : NSObject
- (instancetype)initWithDevices:(id)devs;
@end

@interface SweetCTTransferPlansResp : NSObject
- (instancetype)initWithRemtoeDevices:(id)devs isFlexPolicyOn:(BOOL)flex;
@end

@interface SweetCTMeInfoResp : NSObject
- (instancetype)initWithEquipmentInfoList:(id)list;
@end

@interface SweetCTFwInfoResp : NSObject
- (instancetype)initWithFimwareUpdateInfo:(id)dict;
@end

@interface SweetCTCallCapResp : NSObject
- (instancetype)initWithCapabilities:(id)caps;
@end

/* CallKit dynamic SPI interfaces (loaded via dlopen, zero build-time framework link needed) */
@interface SweetCXHandleSPI : NSObject
- (instancetype)initWithType:(NSInteger)type value:(NSString *)value;
@end

@interface SweetCXAccountSPI : NSObject
- (instancetype)initWithUUID:(NSUUID *)UUID
		 description:(NSString *)description
		 serviceName:(NSString *)serviceName
	      isoCountryCode:(NSString *)isoCountryCode
		      handle:(id)handle
		  shortLabel:(NSString *)shortLabel;
@end

@interface SweetCXSenderIdentitySPI : NSObject
- (instancetype)initWithUUID:(NSUUID *)UUID account:(id)account;
- (instancetype)initWithUUID:(NSUUID *)UUID handle:(id)handle localizedName:(NSString *)localizedName;
@end

@interface SweetCXProviderConfigSPI : NSObject
- (instancetype)initWithLocalizedName:(NSString *)localizedName;
- (void)setLocalizedName:(NSString *)localizedName;
- (void)setSupportedHandleTypes:(NSSet *)types;
- (void)setSupportsAudioOnly:(BOOL)v;
- (void)setSupportsVideo:(BOOL)v;
- (void)setSupportsEmergency:(BOOL)v;
- (void)setSupportsVoicemail:(BOOL)v;
- (void)setSupportsCurrentPlatform:(BOOL)v;
- (void)setSupportsRinging:(BOOL)v;
- (void)setSupportsDynamicSystemUI:(BOOL)v;
- (void)setSupportsAudioTranslation:(BOOL)v;
- (void)setIncludesCallsInRecents:(BOOL)v;
- (void)setMaximumCallGroups:(NSUInteger)v;
- (void)setMaximumCallsPerCallGroup:(NSUInteger)v;
- (void)setPrioritizedSenderIdentities:(NSOrderedSet *)identities;
- (void)setSenderIdentities:(NSSet *)identities;
- (void)setEmergencyHandles:(NSArray *)handles;
@end

@interface SweetCXProviderSPI : NSObject
- (instancetype)initWithConfiguration:(id)configuration;
- (id)configuration;
- (void)setConfiguration:(id)configuration;
- (void)setDelegate:(id)delegate queue:(dispatch_queue_t)queue;
- (void)reportNewIncomingCallWithUUID:(NSUUID *)UUID
			       update:(id)update
			   completion:(void (^)(NSError *error))completion;
- (void)reportCallWithUUID:(NSUUID *)UUID updated:(id)update;
- (void)reportOutgoingCallWithUUID:(NSUUID *)UUID sentInvitationAtDate:(NSDate *)dateSentInvitation;
- (void)reportOutgoingCallWithUUID:(NSUUID *)UUID startedConnectingAtDate:(NSDate *)dateStartedConnecting;
- (void)reportOutgoingCallWithUUID:(NSUUID *)UUID connectedAtDate:(NSDate *)dateConnected;
- (void)reportCallWithUUID:(NSUUID *)UUID endedAtDate:(NSDate *)dateEnded reason:(NSInteger)endedReason;
- (void)reportCallWithUUID:(NSUUID *)UUID endedAtDate:(NSDate *)dateEnded privateReason:(NSInteger)privateReason;
@end

@interface SweetCXActionSPI : NSObject
- (NSUUID *)callUUID;
- (void)fulfill;
- (void)fail;
@end

/* Client delegate push interface */
@interface SweetCTDelegateProxy : NSObject
- (void)subscriptionInfoDidChange;
- (void)activeSubscriptionsDidChange;
- (void)simStatusDidChange:(id)ctx status:(NSString *)status;
- (void)signalStrengthChanged:(id)ctx info:(id)info;
- (void)operatorNameChanged:(id)ctx name:(NSString *)name;
- (void)displayStatusChanged:(id)ctx status:(id)status;
- (void)dataStatus:(id)ctx dataStatusInfo:(id)dataStatus;
- (void)servingNetworkChanged:(id)ctx;
@end

/* ---- Logging & runtime C symbol resolution (libxpc + notify + blocks) ---- */

static FILE *g_logf;
static os_unfair_lock g_state_lock = OS_UNFAIR_LOCK_INIT;
static struct cellular_state g_state;
static bool g_cellular_data_enabled = true;
static bool g_airplane_mode = false;
static bool g_enable_wds_data = false;
static bool g_publish_system_status = true;
static uint16_t g_rat_mode_pref = 0x001cu; /* GSM|WCDMA|LTE */
static char g_configured_apn[64] = "";

typedef void *sweet_xpc_t;
static sweet_xpc_t (*fn_xpc_conn_create_mach)(const char *, dispatch_queue_t, uint64_t);
static void (*fn_xpc_conn_set_target_queue)(sweet_xpc_t, dispatch_queue_t);
static void (*fn_xpc_conn_set_handler)(sweet_xpc_t, void (^)(sweet_xpc_t));
static void (*fn_xpc_conn_resume)(sweet_xpc_t);
static void (*fn_xpc_conn_send)(sweet_xpc_t, sweet_xpc_t);
static sweet_xpc_t (*fn_xpc_dict_create)(const char * const *, const sweet_xpc_t *, size_t);
static sweet_xpc_t (*fn_xpc_dict_create_reply)(sweet_xpc_t);
static void (*fn_xpc_dict_set_int64)(sweet_xpc_t, const char *, int64_t);
static void (*fn_xpc_dict_set_string)(sweet_xpc_t, const char *, const char *);
static void (*fn_xpc_dict_set_bool)(sweet_xpc_t, const char *, bool);
static void (*fn_xpc_dict_set_value)(sweet_xpc_t, const char *, sweet_xpc_t);
static const char *(*fn_xpc_dict_get_string)(sweet_xpc_t, const char *);
static sweet_xpc_t (*fn_xpc_dict_get_value)(sweet_xpc_t, const char *);
static const void *(*fn_xpc_get_type)(sweet_xpc_t);
static sweet_xpc_t (*fn_xpc_retain)(sweet_xpc_t);
static void (*fn_xpc_release)(sweet_xpc_t);
static const void *sym_xpc_type_connection;
static const void *sym_xpc_type_dictionary;
static const void *sym_xpc_error_conn_invalid;
static uint32_t (*fn_notify_post)(const char *);
static const char *(*fn_Block_signature)(void *);

static void clog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void clog(const char *fmt, ...)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm;
	localtime_r(&ts.tv_sec, &tm);

	char msg[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	FILE *out = g_logf ? g_logf : stderr;
	fprintf(out, "%02d:%02d:%02d.%03ld [cellulard] %s\n",
		tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, msg);
	fflush(out);
}

static void resolve_runtime_symbols(void)
{
	fn_xpc_conn_create_mach = dlsym(RTLD_DEFAULT, "xpc_connection_create_mach_service");
	fn_xpc_conn_set_target_queue = dlsym(RTLD_DEFAULT, "xpc_connection_set_target_queue");
	fn_xpc_conn_set_handler = dlsym(RTLD_DEFAULT, "xpc_connection_set_event_handler");
	fn_xpc_conn_resume = dlsym(RTLD_DEFAULT, "xpc_connection_resume");
	fn_xpc_conn_send = dlsym(RTLD_DEFAULT, "xpc_connection_send_message");
	fn_xpc_dict_create = dlsym(RTLD_DEFAULT, "xpc_dictionary_create");
	fn_xpc_dict_create_reply = dlsym(RTLD_DEFAULT, "xpc_dictionary_create_reply");
	fn_xpc_dict_set_int64 = dlsym(RTLD_DEFAULT, "xpc_dictionary_set_int64");
	fn_xpc_dict_set_string = dlsym(RTLD_DEFAULT, "xpc_dictionary_set_string");
	fn_xpc_dict_set_bool = dlsym(RTLD_DEFAULT, "xpc_dictionary_set_bool");
	fn_xpc_dict_set_value = dlsym(RTLD_DEFAULT, "xpc_dictionary_set_value");
	fn_xpc_dict_get_string = dlsym(RTLD_DEFAULT, "xpc_dictionary_get_string");
	fn_xpc_dict_get_value = dlsym(RTLD_DEFAULT, "xpc_dictionary_get_value");
	fn_xpc_get_type = dlsym(RTLD_DEFAULT, "xpc_get_type");
	fn_xpc_retain = dlsym(RTLD_DEFAULT, "xpc_retain");
	fn_xpc_release = dlsym(RTLD_DEFAULT, "xpc_release");
	sym_xpc_type_connection = dlsym(RTLD_DEFAULT, "_xpc_type_connection");
	sym_xpc_type_dictionary = dlsym(RTLD_DEFAULT, "_xpc_type_dictionary");
	sym_xpc_error_conn_invalid = dlsym(RTLD_DEFAULT, "_xpc_error_connection_invalid");
	fn_notify_post = dlsym(RTLD_DEFAULT, "notify_post");
	fn_Block_signature = dlsym(RTLD_DEFAULT, "_Block_signature");
}

static void load_config(void)
{
	/* Check iOS AirplaneMode preference first */
	NSDictionary *radios = [NSDictionary dictionaryWithContentsOfFile:@RADIOS_PLIST];
	if ([radios isKindOfClass:[NSDictionary class]]) {
		NSNumber *ap = radios[@"AirplaneMode"];
		if ([ap respondsToSelector:@selector(boolValue)])
			g_airplane_mode = [ap boolValue];
	}

	FILE *f = fopen(CELLULARD_CONF, "r");
	if (!f)
		return;
	char line[160];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "enable_data=", 12))
			g_enable_wds_data = (atoi(line + 12) != 0);
		else if (!strncmp(line, "cellular_data=", 14))
			g_cellular_data_enabled = (atoi(line + 14) != 0);
		else if (!strncmp(line, "airplane_mode=", 14))
			g_airplane_mode = (atoi(line + 14) != 0);
		else if (!strncmp(line, "publish_system_status=", 22))
			g_publish_system_status = (atoi(line + 22) != 0);
		else if (!strncmp(line, "apn=", 4))
			snprintf(g_configured_apn, sizeof(g_configured_apn), "%s", line + 4);
		else if (!strncmp(line, "rat_mode=", 9)) {
			const char *rm = line + 9;
			if (!strcmp(rm, "3g2g") || !strcmp(rm, "cs"))
				g_rat_mode_pref = 0x000cu; /* GSM|WCDMA */
			else if (!strcmp(rm, "gsm") || !strcmp(rm, "2g"))
				g_rat_mode_pref = 0x0004u; /* GSM */
			else if (!strcmp(rm, "wcdma") || !strcmp(rm, "3g"))
				g_rat_mode_pref = 0x0008u; /* WCDMA */
			else if (!strcmp(rm, "lte") || !strcmp(rm, "4g"))
				g_rat_mode_pref = 0x0010u; /* LTE */
			else
				g_rat_mode_pref = 0x001cu; /* GSM|WCDMA|LTE */
		}
	}
	fclose(f);
}

static struct cellular_state snapshot_state(void)
{
	os_unfair_lock_lock(&g_state_lock);
	struct cellular_state cp = g_state;
	os_unfair_lock_unlock(&g_state_lock);
	return cp;
}

static void ct_set(id obj, NSString *key, id val)
{
	if (!obj || !key)
		return;
	@try {
		[obj setValue:val forKey:key];
	} @catch (id exc) {
		(void)exc;
	}
}

static id ct_get(id obj, NSString *key)
{
	if (!obj || !key)
		return nil;
	@try {
		return [obj valueForKey:key];
	} @catch (id exc) {
		return nil;
	}
}

static void wipe_ram(void *p, size_t n)
{
	volatile uint8_t *vp = (volatile uint8_t *)p;
	while (n--)
		*vp++ = 0;
}

static bool is_emergency_cstr(const char *s)
{
	if (!s || !s[0])
		return false;
	char digits[24];
	size_t o = 0;
	wipe_ram(digits, sizeof(digits));
	for (size_t i = 0; s[i] && o + 1 < sizeof(digits); i++) {
		if ((s[i] >= '0' && s[i] <= '9') || s[i] == '+' || s[i] == '*' || s[i] == '#')
			digits[o++] = s[i];
	}
	bool m = (!strcmp(digits, "112") || !strcmp(digits, "911") || !strcmp(digits, "100") ||
		  !strcmp(digits, "101") || !strcmp(digits, "102") || !strcmp(digits, "108") ||
		  !strcmp(digits, "999") || !strcmp(digits, "000"));
	wipe_ram(digits, sizeof(digits));
	return m;
}

/* ---- CoreTelephony Model Object Builders ---- */

static NSUUID *slot1_uuid(void)
{
	static NSUUID *u;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		u = [[NSUUID alloc] initWithUUIDString:@SLOT1_UUID_STR];
	});
	return u;
}

static id build_slot1_context(const struct cellular_state *st)
{
	Class cls = NSClassFromString(@"CTXPCServiceSubscriptionContext");
	if (!cls)
		return nil;
	id ctx = [[cls alloc] init];
	BOOL good = st->sim_present && (strcmp(st->sim_state, "present") == 0);
	ct_set(ctx, @"slotID", @(1));
	ct_set(ctx, @"uuid", slot1_uuid());
	ct_set(ctx, @"labelID", @SLOT1_UUID_STR);
	ct_set(ctx, @"label", @"Primary");
	ct_set(ctx, @"phoneNumber", @"");
	ct_set(ctx, @"isSimPresent", @(st->sim_present));
	ct_set(ctx, @"isSimGood", @(good));
	ct_set(ctx, @"isSimHidden", @(NO));
	ct_set(ctx, @"isSimDataOnly", @(NO));
	ct_set(ctx, @"userDataPreferred", @(1));
	ct_set(ctx, @"userDefaultVoice", @(1));
	return ctx;
}

static id build_subscription_info(const struct cellular_state *st)
{
	Class cls = NSClassFromString(@"CTXPCServiceSubscriptionInfo");
	if (!cls)
		return nil;
	id info = [[cls alloc] init];
	id ctx1 = build_slot1_context(st);
	NSArray *all = ctx1 ? @[ctx1] : @[];
	NSArray *active = (ctx1 && st->sim_present) ? @[ctx1] : @[];
	ct_set(info, @"subscriptions", all);
	ct_set(info, @"subscriptionsInUse", active);
	ct_set(info, @"subscriptionsValid", active);
	return info;
}

static id build_active_contexts(const struct cellular_state *st)
{
	Class cls = NSClassFromString(@"CTXPCContexts");
	Class icls = NSClassFromString(@"CTXPCContextInfo");
	if (!cls || !icls)
		return nil;
	id contexts = [[cls alloc] init];
	if (st->sim_present) {
		id ci = [(id)icls contextWithUUID:slot1_uuid()
				     andAccountID:@SLOT1_UUID_STR
					  andSlot:1];
		ct_set(ci, @"label", @"Primary");
		ct_set(ci, @"labelID", @SLOT1_UUID_STR);
		ct_set(ci, @"phoneNumber", @"");
		NSArray *arr = ci ? @[ci] : @[];
		ct_set(contexts, @"subscriptions", arr);
		ct_set(contexts, @"existingUserSubscriptions", arr);
		ct_set(contexts, @"dataPreferred", slot1_uuid());
		ct_set(contexts, @"voicePreferred", slot1_uuid());
	} else {
		ct_set(contexts, @"subscriptions", @[]);
		ct_set(contexts, @"existingUserSubscriptions", @[]);
	}
	return contexts;
}

static id build_slot1_descriptor(void)
{
	Class cls = NSClassFromString(@"CTServiceDescriptor");
	if (!cls)
		return nil;
	return [(id)cls telephonyDescriptorWithInstance:@(1)];
}

static NSString *sim_status_string(const struct cellular_state *st)
{
	if (!st->sim_present)
		return @"kCTSIMSupportSIMStatusNotInserted";
	if (!strcmp(st->sim_state, "pin_locked"))
		return @"kCTSIMSupportSIMStatusPINLocked";
	if (!strcmp(st->sim_state, "puk_locked"))
		return @"kCTSIMSupportSIMStatusPUKLocked";
	if (!strcmp(st->sim_state, "network_locked"))
		return @"kCTSIMSupportSIMStatusNetworkLocked";
	if (!strcmp(st->app_state, "ready") || !strcmp(st->app_state, "subscription_ready"))
		return @"kCTSIMSupportSIMStatusReady";
	return @"kCTSIMSupportSIMStatusInserted";
}

static BOOL is_radio_off(const struct cellular_state *st)
{
	if (g_airplane_mode)
		return YES;
	if (st->dms_mode_raw == 0)
		return NO;
	if (st->dms_mode_raw == 0xff && st->reg_state_raw == 1)
		return NO;
	if (st->dms_mode_raw != 0xff)
		return YES;
	return NO;
}

static NSString *reg_status_string(const struct cellular_state *st)
{
	if (!st->sim_present || is_radio_off(st))
		return @"kCTRegistrationStatusNotRegistered";
	switch (st->reg_state_raw) {
	case 1:
		return st->roaming ? @"kCTRegistrationStatusRegisteredRoaming"
				   : @"kCTRegistrationStatusRegisteredHome";
	case 2:
		return @"kCTRegistrationStatusSearching";
	case 3:
		return @"kCTRegistrationStatusDenied";
	default:
		return @"kCTRegistrationStatusNotRegistered";
	}
}

static NSString *public_rat_string(const struct cellular_state *st)
{
	if (!strcmp(st->rat, "5g_nr")) return @"CTRadioAccessTechnologyNR";
	if (!strcmp(st->rat, "lte"))   return @"CTRadioAccessTechnologyLTE";
	if (!strcmp(st->rat, "umts"))  return @"CTRadioAccessTechnologyWCDMA";
	if (!strcmp(st->rat, "gsm"))   return @"CTRadioAccessTechnologyEdge";
	return @"";
}

static NSString *reg_rat_string(const struct cellular_state *st)
{
	if (!strcmp(st->rat, "5g_nr")) return @"kCTRegistrationRadioAccessTechnologyNR";
	if (!strcmp(st->rat, "lte"))   return @"kCTRegistrationRadioAccessTechnologyLTE";
	if (!strcmp(st->rat, "umts"))  return @"kCTRegistrationRadioAccessTechnologyUTRAN";
	if (!strcmp(st->rat, "gsm"))   return @"kCTRegistrationRadioAccessTechnologyGSM";
	return @"kCTRegistrationRadioAccessTechnologyUnknown";
}

static id build_signal_info(const struct cellular_state *st)
{
	Class cls = NSClassFromString(@"CTSignalStrengthInfo");
	if (!cls)
		return nil;
	id info = [[cls alloc] init];
	int bars = (st->sim_present && !is_radio_off(st) && st->reg_state_raw == 1) ? st->bars_0_to_4 : 0;
	ct_set(info, @"bars", @(bars));
	ct_set(info, @"displayBars", @(bars));
	ct_set(info, @"maxDisplayBars", @(4));
	return info;
}

static id build_signal_measurements(const struct cellular_state *st)
{
	Class cls = NSClassFromString(@"CTSignalStrengthMeasurements");
	if (!cls)
		return nil;
	id m = [[cls alloc] init];
	if (st->has_rssi) ct_set(m, @"rssi", @(st->rssi_dbm));
	if (st->has_rsrp) ct_set(m, @"rsrp", @(st->rsrp_dbm));
	if (st->has_rsrq) ct_set(m, @"rsrq", @(st->rsrq_db));
	if (st->has_snr)  ct_set(m, @"snr", @(st->snr_tenth_db / 10));
	return m;
}

static id build_display_status(const struct cellular_state *st)
{
	Class cls = NSClassFromString(@"CTRegistrationDisplayStatus");
	if (!cls)
		return nil;
	id ds = [[cls alloc] init];
	ct_set(ds, @"registrationDisplayStatus", reg_status_string(st));
	ct_set(ds, @"isInHomeCountry", @(!st->roaming));
	ct_set(ds, @"changedDueToSimRemoval", @(!st->sim_present));
	ct_set(ds, @"isRegistrationForcedHome", @(NO));
	ct_set(ds, @"isSatelliteSystem", @(NO));
	return ds;
}

static id build_data_status(const struct cellular_state *st)
{
	Class cls = NSClassFromString(@"CTDataStatus");
	if (!cls)
		return nil;
	id ds = [[cls alloc] init];
	BOOL attached = st->sim_present && !is_radio_off(st) && st->reg_state_raw == 1 && st->ps_attached;
	/* CoreTelephony indicator table: 0=none, 2=EDGE, 3=3G, 7=LTE, 11=5G */
	int ind = 0, rtech = 0;
	if (attached) {
		if (!strcmp(st->rat, "5g_nr"))     { ind = 11; rtech = 9; }
		else if (!strcmp(st->rat, "lte"))  { ind = 7;  rtech = 6; }
		else if (!strcmp(st->rat, "umts")) { ind = 3;  rtech = 2; }
		else if (!strcmp(st->rat, "gsm"))  { ind = 2;  rtech = 1; }
	}
	ct_set(ds, @"attached", @(attached));
	ct_set(ds, @"dataSim", @(st->sim_present));
	ct_set(ds, @"roamAllowed", @(YES));
	ct_set(ds, @"cellularDataPossible", @(st->sim_present && g_cellular_data_enabled));
	ct_set(ds, @"inHomeCountry", @(!st->roaming));
	ct_set(ds, @"indicator", @(ind));
	ct_set(ds, @"indicatorOverride", @(0));
	ct_set(ds, @"radioTechnology", @(rtech));
	ct_set(ds, @"activeContexts", @(st->data_connected ? 1 : 0));
	ct_set(ds, @"totalActiveContexts", @(st->data_connected ? 1 : 0));
	return ds;
}

static id build_data_status_basic(const struct cellular_state *st)
{
	Class cls = NSClassFromString(@"CTDataStatusBasic");
	if (!cls)
		return nil;
	id ds = [[cls alloc] init];
	BOOL attached = st->sim_present && !is_radio_off(st) && st->reg_state_raw == 1 && st->ps_attached;
	ct_set(ds, @"attached", @(attached));
	ct_set(ds, @"hasIndicator", @(attached));
	ct_set(ds, @"inHomeCountry", @(!st->roaming));
	ct_set(ds, @"roamAllowed", @(YES));
	ct_set(ds, @"cellularDataPossible", @(st->sim_present && g_cellular_data_enabled));
	ct_set(ds, @"newRadioCoverage", @(strcmp(st->rat, "5g_nr") == 0));
	return ds;
}

/* ---- SystemStatus Publisher (Status Bar Bars + Carrier + LTE) ---- */

static SweetSTTelephonyPublisher *g_st_publisher;

static void init_system_status_publisher(void)
{
	if (!g_publish_system_status)
		return;
	@try {
		void *h = dlopen("/System/Library/PrivateFrameworks/SystemStatus.framework/SystemStatus", RTLD_NOW);
		if (!h) {
			clog("SystemStatus dlopen unavailable: %s", dlerror());
			return;
		}
		Class pubCls = NSClassFromString(@"STTelephonyStatusDomainPublisher");
		if (pubCls) {
			g_st_publisher = [[pubCls alloc] init];
			clog("STTelephonyStatusDomainPublisher initialized (%p)", (__bridge void *)g_st_publisher);
		}
	} @catch (id exc) {
		clog("SystemStatus init exception: %s", [[exc description] UTF8String]);
	}
}

static void publish_system_status(const struct cellular_state *st)
{
	if (!g_st_publisher)
		return;
	@try {
		NSString *op = st->operator_name[0] ? [NSString stringWithUTF8String:st->operator_name] : @"Carrier";
		BOOL simPresent = st->sim_present;
		BOOL radioOff = is_radio_off(st);
		NSUInteger bars = (simPresent && !radioOff && st->reg_state_raw == 1)
				  ? (NSUInteger)st->bars_0_to_4 : 0;

		/* STTelephonyStatusDomainServiceState: 0=No Service, 1=Searching, 2=Service, 3=SOS */
		NSUInteger svcState = 0;
		if (simPresent && !radioOff) {
			if (st->reg_state_raw == 1)
				svcState = 2;
			else if (st->dms_mode_raw == 0 && st->reg_state_raw == 2)
				svcState = 1;
			else if (st->reg_state_raw == 2)
				svcState = 1;
		}

		/* STTelephonyStatusDomainDataNetworkType: 0=Unknown, 3=EDGE, 5=HSDPA, 7=LTE, 11=5G */
		NSUInteger netType = 0;
		if (svcState == 2 && st->ps_attached) {
			if (!strcmp(st->rat, "5g_nr"))     netType = 11;
			else if (!strcmp(st->rat, "lte"))  netType = 7;
			else if (!strcmp(st->rat, "umts")) netType = 5;
			else if (!strcmp(st->rat, "gsm"))  netType = 3;
		}

		BOOL qrtrAlive = st->qrtr_connected;
		BOOL psUp = (svcState == 2 && st->ps_attached && g_cellular_data_enabled);

		[g_st_publisher updateDataWithBlock:^(SweetSTMutableTelephonyData *data) {
			if (!data)
				return;
			[data setCellularRadioCapabilityEnabled:!radioOff];
			[data setDualSIMEnabled:NO];
			[data setRadioModuleDead:!qrtrAlive];
			[data setInactiveSOSEnabled:NO];
			[data setUsingStewieForSOS:NO];
			[data setUsingStewieConnection:NO];
			[data updateSIMOneInfoWithBlock:^(SweetSTMutableSIMInfo *sim) {
				if (!sim)
					return;
				[sim setSIMPresent:simPresent];
				[sim setBootstrap:NO];
				[sim setLabel:@"Primary"];
				[sim setShortLabel:@"P"];
				[sim setMaxSignalStrengthBars:4];
				[sim setSignalStrengthBars:bars];
				[sim setServiceState:svcState];
				[sim setCellularServiceState:svcState];
				[sim setServiceDescription:(svcState == 2 ? op : @"")];
				[sim setSecondaryServiceDescription:@""];
				[sim setDataNetworkType:netType];
				[sim setPreferredForDataConnections:simPresent];
				[sim setProvidingDataConnection:psUp];
				[sim setRegisteredWithoutCellular:NO];
				[sim setCallForwardingEnabled:NO];
			}];
		}];
	} @catch (id exc) {
		clog("SystemStatus publish exception: %s", [[exc description] UTF8String]);
	}
}

/* ---- Recursive Protocol Method Lookup for NSXPCInterface Fallback ---- */

static struct objc_method_description find_proto_method(Protocol *proto, SEL sel, NSMutableSet *visited)
{
	struct objc_method_description empty = { NULL, NULL };
	if (!proto || !sel)
		return empty;
	NSString *pname = [NSString stringWithUTF8String:protocol_getName(proto)];
	if ([visited containsObject:pname])
		return empty;
	[visited addObject:pname];

	struct objc_method_description d = protocol_getMethodDescription(proto, sel, YES, YES);
	if (!d.types)
		d = protocol_getMethodDescription(proto, sel, NO, YES);
	if (d.types)
		return d;

	unsigned int count = 0;
	Protocol * __unsafe_unretained *sub = protocol_copyProtocolList(proto, &count);
	if (sub) {
		for (unsigned int i = 0; i < count; i++) {
			d = find_proto_method(sub[i], sel, visited);
			if (d.types) {
				free(sub);
				return d;
			}
		}
		free(sub);
	}
	return empty;
}

static NSSet<Class> *build_allowed_ct_classes(void)
{
	static NSSet<Class> *cached;
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		NSMutableSet<Class> *set = [NSMutableSet setWithObjects:
			[NSObject class], [NSArray class], [NSMutableArray class],
			[NSDictionary class], [NSMutableDictionary class],
			[NSSet class], [NSMutableSet class],
			[NSString class], [NSMutableString class],
			[NSNumber class], [NSData class], [NSMutableData class],
			[NSDate class], [NSUUID class], [NSURL class],
			[NSError class], [NSNull class], nil];
		unsigned int count = 0;
		Class *list = objc_copyClassList(&count);
		if (list) {
			Protocol *secProto = objc_getProtocol("NSSecureCoding");
			for (unsigned int i = 0; i < count; i++) {
				const char *nm = class_getName(list[i]);
				if (nm && nm[0] == 'C' && (nm[1] == 'T' || nm[1] == 'N')) {
					if (!secProto || class_conformsToProtocol(list[i], secProto))
						[set addObject:list[i]];
				}
			}
			free(list);
		}
		cached = [set copy];
	});
	return cached;
}

/* ---- CoreTelephony XPC Exported Object & Listener ---- */

@interface SweetCellularXPCService : NSObject
@property (nonatomic, weak) SweetXPCConnectionSPI *connection;
@property (nonatomic, assign) BOOL registeredNotifications;
@end

static os_unfair_lock g_clients_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableArray<SweetCellularXPCService *> *g_xpc_clients;
static NSMutableSet<NSString *> *g_logged_unhandled;
static void trigger_qmi_radio_power(bool online);

@implementation SweetCellularXPCService

/* -- Base Interface -- */

- (void)ping:(void (^)(NSError *))completion
{
	if (completion)
		completion(nil);
}

- (void)registerForNotifications:(NSArray *)notifications completion:(void (^)(NSError *))completion
{
	(void)notifications;
	self.registeredNotifications = YES;
	if (completion)
		completion(nil);
}

- (void)getSubscriptionInfo:(void (^)(id, NSError *))completion
{
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(build_subscription_info(&st), nil);
}

- (void)getActiveContexts:(void (^)(id, NSError *))completion
{
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(build_active_contexts(&st), nil);
}

- (void)getDescriptorsForDomain:(NSInteger)domain completion:(void (^)(id, NSError *))completion
{
	(void)domain;
	Class ccls = NSClassFromString(@"CTServiceDescriptorContainer");
	id d = build_slot1_descriptor();
	id cont = ccls ? [(SweetCTServiceDescriptorContainer *)[ccls alloc]
			  initWithDescriptors:(d ? @[d] : @[])] : nil;
	if (completion)
		completion(cont, nil);
}

- (void)getDualSimCapability:(void (^)(NSInteger, NSError *))completion
{
	if (completion)
		completion(1, nil); /* Single active SIM */
}

- (void)getEvolvedSubscriptionLabelID:(id)ctx withCallback:(void (^)(NSString *, NSError *))cb
{
	(void)ctx;
	if (cb)
		cb(@SLOT1_UUID_STR, nil);
}

/* -- Subscriber / SIM Interface -- */

- (void)getSIMStatus:(id)ctx completion:(void (^)(NSString *, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(sim_status_string(&st), nil);
}

- (void)getSIMTrayStatus:(void (^)(NSString *, NSError *))completion
{
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(st.sim_present ? @"kCTSIMSupportSIMTrayInsertedWithSIM"
					  : @"kCTSIMSupportSIMTrayInsertedNoSIM", nil);
}

- (void)getShortLabels:(id)ctx completion:(void (^)(NSDictionary *, NSError *))completion
{
	(void)ctx;
	if (completion)
		completion(@{ @SLOT1_UUID_STR: @"P" }, nil);
}

- (void)isSimLocked:(id)ctx completion:(void (^)(BOOL, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	BOOL locked = (!strcmp(st.sim_state, "pin_locked") || !strcmp(st.sim_state, "puk_locked"));
	if (completion)
		completion(locked, nil);
}

- (void)simHardwareInfo:(id)ctx completion:(void (^)(id, NSError *))completion
{
	(void)ctx;
	Class cls = NSClassFromString(@"CTSimHardwareInfo");
	id hw = cls ? [(SweetCTSimHardwareInfo *)[cls alloc] initWithHardwareInfo:1 simLocation:1] : nil;
	if (completion)
		completion(hw, nil);
}

/* -- Registration & Signal Interface -- */

- (void)copyOperatorName:(id)ctx completion:(void (^)(NSString *, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	NSString *op = st.operator_name[0] ? [NSString stringWithUTF8String:st.operator_name] : @"";
	if (completion)
		completion(op, nil);
}

- (void)copyRegistrationStatus:(id)ctx completion:(void (^)(NSString *, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(reg_status_string(&st), nil);
}

- (void)copyRegistrationDisplayStatus:(id)ctx completion:(void (^)(id, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(build_display_status(&st), nil);
}

- (void)getSignalStrengthInfo:(id)ctx completion:(void (^)(id, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(build_signal_info(&st), nil);
}

- (void)getPublicSignalStrength:(id)desc completion:(void (^)(id, NSError *))completion
{
	(void)desc;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(build_signal_info(&st), nil);
}

- (void)getSignalStrengthMeasurements:(id)desc completion:(void (^)(id, NSError *))completion
{
	(void)desc;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(build_signal_measurements(&st), nil);
}

- (void)copyMobileCountryCode:(id)ctx completion:(void (^)(NSString *, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	NSString *mcc = st.mcc[0] ? [NSString stringWithUTF8String:st.mcc] : @"";
	if (completion)
		completion(mcc, nil);
}

- (void)copyLastKnownMobileCountryCode:(id)ctx completion:(void (^)(NSString *, NSError *))completion
{
	[self copyMobileCountryCode:ctx completion:completion];
}

- (void)copyMobileNetworkCode:(id)ctx completion:(void (^)(NSString *, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	NSString *mnc = st.mnc[0] ? [NSString stringWithUTF8String:st.mnc] : @"";
	if (completion)
		completion(mnc, nil);
}

- (void)copyServingPlmn:(id)ctx completion:(void (^)(NSString *, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	NSString *plmn = (st.mcc[0] && st.mnc[0])
			 ? [NSString stringWithFormat:@"%s%s", st.mcc, st.mnc] : @"";
	if (completion)
		completion(plmn, nil);
}

- (void)copyRadioAccessTechnology:(id)ctx completion:(void (^)(NSString *, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(reg_rat_string(&st), nil);
}

- (void)copyIsDataAttached:(id)ctx completion:(void (^)(BOOL, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(st.sim_present && st.reg_state_raw == 1 && st.ps_attached, nil);
}

- (void)copyIsInHomeCountry:(id)ctx completion:(void (^)(BOOL, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(!st.roaming, nil);
}

- (void)isNetworkSelectionMenuAvailable:(id)ctx completion:(void (^)(BOOL, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(st.sim_present, nil);
}

- (void)copyNetworkSelectionMode:(id)ctx completion:(void (^)(NSString *, NSError *))completion
{
	(void)ctx;
	if (completion)
		completion(@"kCTRegistrationNetworkSelectionModeAutomatic", nil);
}

- (void)copyNetworkSelectionState:(id)ctx completion:(void (^)(NSString *, NSError *))completion
{
	(void)ctx;
	if (completion)
		completion(@"kCTRegistrationNetworkSelectionStateIdle", nil);
}

- (void)copyNetworkSelectionInfo:(id)ctx completion:(void (^)(id, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	Class cls = NSClassFromString(@"CTNetworkSelectionInfo");
	id info = cls ? [[cls alloc] init] : nil;
	ct_set(info, @"selectionState", @(1));
	ct_set(info, @"selectionMode", @(0)); /* Automatic */
	Class ncls = NSClassFromString(@"CTNetwork");
	if (ncls && st.operator_name[0]) {
		id net = [[ncls alloc] init];
		ct_set(net, @"name", [NSString stringWithUTF8String:st.operator_name]);
		ct_set(net, @"plmn", [NSString stringWithFormat:@"%s%s", st.mcc, st.mnc]);
		ct_set(net, @"rat", @"LTE");
		ct_set(info, @"selection", net);
	}
	if (completion)
		completion(info, nil);
}

- (void)copyRatSelection:(id)ctx completion:(void (^)(id, NSError *))completion
{
	(void)ctx;
	Class cls = NSClassFromString(@"CTRatSelection");
	id rs = cls ? [[cls alloc] init] : nil;
	ct_set(rs, @"mask", @(7)); /* GSM | UMTS | LTE */
	ct_set(rs, @"selection", @"kCTRegistrationRATSelectionLTE");
	ct_set(rs, @"preferred", @"kCTRegistrationRATSelectionLTE");
	if (completion)
		completion(rs, nil);
}

/* -- Data & Carrier Bundle Interface -- */

- (void)getDataStatus:(id)ctx completion:(void (^)(id, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(build_data_status(&st), nil);
}

- (void)getDataStatusBasic:(id)ctx completion:(void (^)(id, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(build_data_status_basic(&st), nil);
}

- (void)getCurrentDataSubscriptionContext:(void (^)(id, NSError *))completion
{
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(st.sim_present ? build_slot1_context(&st) : nil, nil);
}

- (void)getPreferredDataSubscriptionContext:(void (^)(id, NSError *))completion
{
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(st.sim_present ? build_slot1_context(&st) : nil, nil);
}

- (void)getCurrentDataServiceDescriptor:(void (^)(id, NSError *))completion
{
	if (completion)
		completion(build_slot1_descriptor(), nil);
}

- (void)getPreferredDataServiceDescriptor:(void (^)(id, NSError *))completion
{
	if (completion)
		completion(build_slot1_descriptor(), nil);
}

- (void)getCellularDataIsEnabled:(void (^)(BOOL, NSError *))completion
{
	if (completion)
		completion(g_cellular_data_enabled, nil);
}

- (void)setCellularDataIsEnabled:(BOOL)enabled completion:(void (^)(NSError *))completion
{
	g_cellular_data_enabled = enabled;
	clog("Settings toggled Cellular Data -> %d", enabled ? 1 : 0);
	struct cellular_state st = snapshot_state();
	publish_system_status(&st);
	if (completion)
		completion(nil);
}

- (void)getDataSettings:(void (^)(id, NSError *))completion
{
	Class cls = NSClassFromString(@"CTDataSettings");
	id ds = cls ? [[cls alloc] init] : nil;
	ct_set(ds, @"cellularDataEnabled", @(g_cellular_data_enabled));
	ct_set(ds, @"airplaneModeEnabled", @(g_airplane_mode));
	ct_set(ds, @"cellularDataCapable", @(YES));
	if (completion)
		completion(ds, nil);
}

- (void)getSupports5G:(void (^)(BOOL, NSError *))completion
{
	if (completion)
		completion(NO, nil); /* SM8150 / SDX24/internal LTE modem */
}

- (void)getNREnabled:(id)ctx completion:(void (^)(BOOL, NSError *))completion
{
	(void)ctx;
	if (completion)
		completion(NO, nil);
}

- (void)getInternationalDataAccessStatus:(id)ctx completion:(void (^)(BOOL, NSError *))completion
{
	(void)ctx;
	if (completion)
		completion(YES, nil);
}

- (void)setInternationalDataAccessStatus:(id)ctx status:(BOOL)roam completion:(void (^)(NSError *))completion
{
	(void)ctx; (void)roam;
	if (completion)
		completion(nil);
}

- (void)getConfiguredApns:(id)ctx completion:(void (^)(NSArray *, NSError *))completion
{
	(void)ctx;
	struct cellular_state st = snapshot_state();
	const char *fb = qmi_resolve_fallback_apn(&st);
	NSString *apn = st.data_apn[0] ? [NSString stringWithUTF8String:st.data_apn]
			: (g_configured_apn[0] ? [NSString stringWithUTF8String:g_configured_apn]
			   : (fb ? [NSString stringWithUTF8String:fb] : @"internet"));
	if (completion)
		completion(@[ @{ @"apn": apn } ], nil);
}

/* -- Stewie, Capabilities, PNR, Private Network & Voicemail Interfaces -- */

- (void)getStewieSupport:(void (^)(id, NSError *))completion
{
	Class cls = NSClassFromString(@"CTStewieSupport");
	id obj = cls ? [[cls alloc] init] : nil;
	ct_set(obj, @"status", @(NO));
	ct_set(obj, @"hwSupport", @(NO));
	if (completion)
		completion(obj, nil);
}

- (void)getPhoneServicesDeviceInfo:(void (^)(NSDictionary *, NSError *))completion
{
	if (completion)
		completion(@{}, nil);
}

- (void)getPNRSupportStatus:(id)ctx completion:(void (^)(id, NSError *))completion
{
	(void)ctx;
	Class cls = NSClassFromString(@"CTPNRSupportStatus");
	id obj = cls ? [[cls alloc] init] : nil;
	ct_set(obj, @"supportType", @(0));
	if (completion)
		completion(obj, nil);
}

- (void)getPrivateNetworkCapabilitiesForContext:(id)ctx completion:(void (^)(id, NSError *))completion
{
	(void)ctx;
	Class cls = NSClassFromString(@"CTPrivateNetworkCapabilities");
	id obj = cls ? [[cls alloc] init] : nil;
	ct_set(obj, @"isPrivateNetworkSIM", @(NO));
	ct_set(obj, @"isPrivateNetworkPreferredOverWifi", @(NO));
	ct_set(obj, @"isPrivateNetworkModeEnabled", @(NO));
	ct_set(obj, @"hideDataRoaming", @(NO));
	if (completion)
		completion(obj, nil);
}

- (void)getVoicemailCallInDetails:(id)ctx completion:(void (^)(NSDictionary *, NSError *))completion
{
	(void)ctx;
	if (completion)
		completion(@{}, nil);
}

- (void)context:(id)ctx getCapability:(NSString *)cap completion:(void (^)(BOOL, NSDictionary *, NSError *))completion
{
	(void)ctx;
	BOOL enabled = ([cap isEqualToString:@"kCTCapabilityPhoneServices"] ||
			[cap isEqualToString:@"kCTCapabilityVoLTE"]);
	NSDictionary *info = [cap isEqualToString:@"kCTCapabilityPhoneServices"] ? @{
		@"kCTCapabilityAgent": @YES,
		@"kCTCapabilityAgent2": @YES,
		@"kCTCallManagementIsPhoneServicesActive": @YES,
		@"kCTCallManagementPhoneServicesCSCallingAvailable": @YES,
		@"kCTCallManagementPhoneServicesVoLTECallingAvailable": @YES,
		@"kCTCallManagementEmergencyWifiAllowed": @NO,
		@"kCTCallManagementEmergencyWifiCallCapable": @NO,
	} : @{};
	if (completion)
		completion(enabled, info, nil);
}

- (void)getVoicemailInfo:(id)ctx completion:(void (^)(BOOL, NSNumber *, NSString *, NSError *))completion
{
	(void)ctx;
	if (completion)
		completion(NO, @(0), @"", nil);
}

- (void)copyCarrierBundleVersion:(id)ctx completion:(void (^)(NSString *, NSError *))completion
{
	(void)ctx;
	if (completion)
		completion(@"64.0", nil);
}

static id build_call_capabilities_obj(void)
{
	Class cls = NSClassFromString(@"CTCallCapabilities");
	id caps = cls ? [[cls alloc] init] : nil;
	ct_set(caps, @"isCSCallingAvailable", @YES);
	ct_set(caps, @"isVoLTECallingAvailable", @YES);
	ct_set(caps, @"isWiFiCallingAvailable", @NO);
	ct_set(caps, @"isEmergencyCallingOnWifiAllowed", @NO);
	ct_set(caps, @"isEmergencyCallingOnWifiAvailable", @NO);
	ct_set(caps, @"isCarrierSupportsWiFiCallingOnOtherDevices", @NO);
	return caps;
}

static NSDictionary *build_system_capabilities_dict(void)
{
	return @{
		@"kCTCapabilityPhoneServices": @{
			@"kCTCapabilityAgent": @YES,
			@"kCTCapabilityAgent2": @YES,
			@"kCTCallManagementIsPhoneServicesActive": @YES,
			@"kCTCallManagementPhoneServicesCSCallingAvailable": @YES,
			@"kCTCallManagementPhoneServicesVoLTECallingAvailable": @YES,
			@"kCTCallManagementEmergencyWifiAllowed": @NO,
			@"kCTCallManagementEmergencyWifiCallCapable": @NO,
		},
	};
}

static id resolve_carrier_bundle_val(const struct cellular_state *st, id keyHierarchy, id defaultVal)
{
	NSString *k = @"";
	if ([keyHierarchy isKindOfClass:[NSArray class]] && [(NSArray *)keyHierarchy count] > 0)
		k = [[(NSArray *)keyHierarchy lastObject] description];
	else if ([keyHierarchy isKindOfClass:[NSString class]])
		k = (NSString *)keyHierarchy;

	id val = nil;
	if ([k isEqualToString:@"CarrierName"] || [k isEqualToString:@"MVNOCarrierName"]) {
		val = st->operator_name[0] ? [NSString stringWithUTF8String:st->operator_name] : @"Carrier";
	} else if ([k isEqualToString:@"StatusBarImages"]) {
		NSString *nm = st->operator_name[0] ? [NSString stringWithUTF8String:st->operator_name] : @"";
		val = @[ @{ @"StatusBarCarrierName": nm, @"AllowPrefixMatching": @YES } ];
	} else if ([k isEqualToString:@"Show3GSwitch"] || [k isEqualToString:@"LTEConnectionShows4G"]) {
		val = @NO;
	} else if ([k isEqualToString:@"Show4GSwitch"] || [k isEqualToString:@"AllowEDGEEditing"] ||
		   [k isEqualToString:@"VoLTEEnabled"] || [k isEqualToString:@"SupportsCallsOverIMS"] ||
		   [k isEqualToString:@"ShowCallForwarding"] || [k isEqualToString:@"ShowCallWaiting"]) {
		val = @YES;
	}
	return val ? val : defaultVal;
}

- (void)copyCarrierBundleValue:(id)ctx key:(NSArray *)keyHierarchy
		    bundleType:(id)btype completion:(void (^)(id, NSError *))completion
{
	[self copyCarrierBundleValueWithDefault:ctx key:keyHierarchy bundleType:btype completion:completion];
}

- (void)copyCarrierBundleValueWithDefault:(id)ctx key:(NSArray *)keyHierarchy
			       bundleType:(id)btype completion:(void (^)(id, NSError *))completion
{
	(void)ctx; (void)btype;
	struct cellular_state st = snapshot_state();
	if (completion)
		completion(resolve_carrier_bundle_val(&st, keyHierarchy, nil), nil);
}

/* -- Unified CTXPCRequestMessage Dispatcher (sendRequest:completionHandler:) -- */

- (void)sendRequest:(id)request completionHandler:(void (^)(id response, NSError *error))completionHandler
{
	if (!completionHandler)
		return;
	if (!request) {
		completionHandler(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:EINVAL userInfo:nil]);
		return;
	}

	@try {
		NSString *cname = NSStringFromClass([request class]);
		struct cellular_state st = snapshot_state();
		id resp = nil;

		if ([cname isEqualToString:@"CTXPCGetSignalStrengthInfoRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetSignalStrengthInfoResponse");
			resp = rcls ? [(SweetCTSigInfoResp *)[rcls alloc]
				       initWithSignalStrengthInfo:build_signal_info(&st)] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetSignalStrengthMeasurementsRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetSignalStrengthMeasurementsResponse");
			resp = rcls ? [(SweetCTSigMeasResp *)[rcls alloc]
				       initWithSignalStrengthMeasurements:build_signal_measurements(&st)] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetOperatorNameRequest"] ||
			   [cname isEqualToString:@"CTXPCSubscriptionNameRequest"]) {
			NSString *op = st.operator_name[0] ? [NSString stringWithUTF8String:st.operator_name] : @"";
			if ([cname isEqualToString:@"CTXPCGetOperatorNameRequest"]) {
				Class rcls = NSClassFromString(@"CTXPCGetOperatorNameResponse");
				resp = rcls ? [(SweetCTOpNameResp *)[rcls alloc] initWithOperatorName:op] : nil;
			} else {
				Class rcls = NSClassFromString(@"CTXPCSimCommonResultResponse");
				resp = rcls ? [(SweetCTObjResp *)[rcls alloc] initWithResult:(op.length ? op : @"Primary")] : nil;
			}
		} else if ([cname isEqualToString:@"CTXPCGetRegistrationStatusRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetRegistrationStatusResponse");
			resp = rcls ? [(SweetCTStatusResp *)[rcls alloc] initWithStatus:reg_status_string(&st)] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetRegistrationDisplayStatusRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetRegistrationDisplayStatusResponse");
			resp = rcls ? [(SweetCTStatusResp *)[rcls alloc] initWithStatus:build_display_status(&st)] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetRoamingStatusRequest"]) {
			Class mcls = NSClassFromString(@"CTRoamingStatus");
			id rm = mcls ? [[mcls alloc] init] : nil;
			ct_set(rm, @"isVoiceRoaming", @(st.roaming));
			ct_set(rm, @"isDataRoaming", @(st.roaming));
			Class rcls = NSClassFromString(@"CTXPCGetRoamingStatusResponse");
			resp = rcls ? [(SweetCTRoamingResp *)[rcls alloc] initWithRoamingStatus:rm] : nil;
		} else if ([cname isEqualToString:@"CTXPCSimStatusRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCSimCommonResultResponse");
			resp = rcls ? [(SweetCTObjResp *)[rcls alloc] initWithResult:sim_status_string(&st)] : nil;
		} else if ([cname isEqualToString:@"CTXPCSimTrayStatusRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCSimCommonResultResponse");
			NSString *tray = st.sim_present ? @"kCTSIMSupportSIMTrayInsertedWithSIM"
							: @"kCTSIMSupportSIMTrayInsertedNoSIM";
			resp = rcls ? [(SweetCTObjResp *)[rcls alloc] initWithResult:tray] : nil;
		} else if ([cname isEqualToString:@"CTXPCSimHardwareInfoRequest"]) {
			Class hcls = NSClassFromString(@"CTSimHardwareInfo");
			id hw = hcls ? [(SweetCTSimHardwareInfo *)[hcls alloc] initWithHardwareInfo:1 simLocation:1] : nil;
			Class rcls = NSClassFromString(@"CTXPCSimHardwareInfoResponse");
			resp = rcls ? [(SweetCTHwResp *)[rcls alloc] initWithHardwareInfo:hw] : nil;
		} else if ([cname isEqualToString:@"CTXPCSimLockStateRequest"]) {
			/* 0 = unlocked, 1 = PIN locked, 2 = PUK locked */
			int lstate = 0;
			if (!strcmp(st.sim_state, "pin_locked")) lstate = 1;
			else if (!strcmp(st.sim_state, "puk_locked")) lstate = 2;
			Class rcls = NSClassFromString(@"CTXPCSimCommonNumberResultResponse");
			resp = rcls ? [(SweetCTObjResp *)[rcls alloc] initWithResult:@(lstate)] : nil;
		} else if ([cname isEqualToString:@"CTXPCRemainingPINRetriesRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCSimCommonNumberResultResponse");
			resp = rcls ? [(SweetCTObjResp *)[rcls alloc] initWithResult:@(st.pin1_retries)] : nil;
		} else if ([cname isEqualToString:@"CTXPCRemainingPUKRetriesRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCSimCommonNumberResultResponse");
			resp = rcls ? [(SweetCTObjResp *)[rcls alloc] initWithResult:@(st.puk1_retries)] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetSimLabelRequest"]) {
			Class lcls = NSClassFromString(@"CTSimLabel");
			id lbl = lcls ? [(SweetCTSimLabel *)[lcls alloc]
					 initWithId:@SLOT1_UUID_STR andText:@"Primary" andDidSetup:YES] : nil;
			Class rcls = NSClassFromString(@"CTXPCGetSimLabelResponse");
			resp = rcls ? [(SweetCTLabelResp *)[rcls alloc] initWithLabel:lbl] : nil;
		} else if ([cname isEqualToString:@"CTXPCShortLabelRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCSimCommonResultResponse");
			resp = rcls ? [(SweetCTObjResp *)[rcls alloc] initWithResult:@"P"] : nil;
		} else if ([cname isEqualToString:@"CTXPCIsAnySimReadyRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCBooleanResponseMessage");
			BOOL rdy = st.sim_present &&
				   (!strcmp(st.app_state, "ready") || !strcmp(st.app_state, "subscription_ready"));
			resp = rcls ? [(SweetCTBoolResp *)[rcls alloc] initWithResult:rdy] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetIsDataAttachedRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetIsDataAttachedResponse");
			BOOL att = st.sim_present && st.reg_state_raw == 1 && st.ps_attached;
			resp = rcls ? [(SweetCTDataAttResp *)[rcls alloc] initWithIsDataAttached:att] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetIsInHomeCountryRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetIsInHomeCountryResponse");
			resp = rcls ? [(SweetCTHomeResp *)[rcls alloc] initWithIsHome:!st.roaming] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetMobileCountryCodeRequest"] ||
			   [cname isEqualToString:@"CTXPCMobileSubscriberCountryCodeRequest"] ||
			   [cname isEqualToString:@"CTXPCLastKnownMobileSubscriberCountryCodeRequest"]) {
			NSString *mcc = st.mcc[0] ? [NSString stringWithUTF8String:st.mcc] : @"";
			if ([cname isEqualToString:@"CTXPCGetMobileCountryCodeRequest"]) {
				Class rcls = NSClassFromString(@"CTXPCGetMobileCountryCodeResponse");
				resp = rcls ? [(SweetCTMccResp *)[rcls alloc] initWithMcc:mcc] : nil;
			} else {
				Class rcls = NSClassFromString(@"CTXPCSimCommonResultResponse");
				resp = rcls ? [(SweetCTObjResp *)[rcls alloc] initWithResult:mcc] : nil;
			}
		} else if ([cname isEqualToString:@"CTXPCGetLastKnownMobileCountryCodeRequest"]) {
			NSString *mcc = st.mcc[0] ? [NSString stringWithUTF8String:st.mcc] : @"";
			Class rcls = NSClassFromString(@"CTXPCGetLastKnownMobileCountryCodeResponse");
			resp = rcls ? [(SweetCTLastMccResp *)[rcls alloc] initWithLastMcc:mcc] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetMobileNetworkCodeRequest"] ||
			   [cname isEqualToString:@"CTXPCMobileSubscriberNetworkCodeRequest"]) {
			NSString *mnc = st.mnc[0] ? [NSString stringWithUTF8String:st.mnc] : @"";
			if ([cname isEqualToString:@"CTXPCGetMobileNetworkCodeRequest"]) {
				Class rcls = NSClassFromString(@"CTXPCGetMobileNetworkCodeResponse");
				resp = rcls ? [(SweetCTMncResp *)[rcls alloc] initWithMnc:mnc] : nil;
			} else {
				Class rcls = NSClassFromString(@"CTXPCSimCommonResultResponse");
				resp = rcls ? [(SweetCTObjResp *)[rcls alloc] initWithResult:mnc] : nil;
			}
		} else if ([cname isEqualToString:@"CTXPCGetServingPlmnRequest"]) {
			NSString *plmn = (st.mcc[0] && st.mnc[0])
					 ? [NSString stringWithFormat:@"%s%s", st.mcc, st.mnc] : @"";
			Class rcls = NSClassFromString(@"CTXPCGetServingPlmnResponse");
			resp = rcls ? [(SweetCTPlmnResp *)[rcls alloc] initWithPlmn:plmn] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetCurrentRatRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetCurrentRatResponse");
			resp = rcls ? [(SweetCTCurRatResp *)[rcls alloc] initWithRat:public_rat_string(&st)] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetRadioAccessTechnologyRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetRadioAccessTechnologyResponse");
			resp = rcls ? [(SweetCTRegRatResp *)[rcls alloc] initWithRAT:reg_rat_string(&st)] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetWirelessTechnologyRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetWirelessTechnologyResponse");
			resp = rcls ? [(SweetCTTechResp *)[rcls alloc] initWithTechnology:@"UMTS"] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetRatSelectionRequest"]) {
			Class scls = NSClassFromString(@"CTRatSelection");
			id rs = scls ? [[scls alloc] init] : nil;
			ct_set(rs, @"mask", @(7));
			ct_set(rs, @"selection", @"kCTRegistrationRATSelectionLTE");
			ct_set(rs, @"preferred", @"kCTRegistrationRATSelectionLTE");
			Class rcls = NSClassFromString(@"CTXPCGetRatSelectionResponse");
			resp = rcls ? [(SweetCTRatSelResp *)[rcls alloc] initWithRATSelection:rs] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetMaxDataRateRequest"]) {
			/* 3 = kCTRegistrationDataRate4G / LTE */
			Class rcls = NSClassFromString(@"CTXPCGetMaxDataRateResponse");
			resp = rcls ? [(SweetCTDataRateResp *)[rcls alloc] initWithDataRate:3] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetSupportedDataRatesRequest"]) {
			Class scls = NSClassFromString(@"CTSupportedMaxDataRates");
			id sr = scls ? [(SweetCTSupportedMaxDataRates *)[scls alloc] initWithRates:@[@2, @3]] : nil;
			Class rcls = NSClassFromString(@"CTXPCGetSupportedDataRatesResponse");
			resp = rcls ? [(SweetCTSupRatesResp *)[rcls alloc] initWithSupportedRates:sr] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetNRStatusRequest"]) {
			Class ncls = NSClassFromString(@"CTNRStatus");
			id nr = ncls ? [[ncls alloc] init] : nil;
			ct_set(nr, @"saSwitchVisible", @(NO));
			ct_set(nr, @"saSwitchConfigurable", @(NO));
			ct_set(nr, @"saDisabled", @(YES));
			ct_set(nr, @"nsaDisabled", @(YES));
			Class rcls = NSClassFromString(@"CTXPCGetNRStatusResponse");
			resp = rcls ? [(SweetCTNRResp *)[rcls alloc] initWithNRStatus:nr] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetSupports5GStandaloneRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetSupports5GStandaloneResponse");
			resp = rcls ? [(SweetCTSupportResp *)[rcls alloc] initWithSupport:NO] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetBandInfoRequest"]) {
			Class bcls = NSClassFromString(@"CTBandInfo");
			id bi = bcls ? [[bcls alloc] init] : nil;
			Class rcls = NSClassFromString(@"CTXPCGetBandInfoResponse");
			resp = rcls ? [(SweetCTBandResp *)[rcls alloc] initWithBandInfo:bi] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetDefaultVoiceRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetDefaultVoiceResponse");
			resp = rcls ? [(SweetCTCtxResp *)[rcls alloc]
				       initWithContext:(st.sim_present ? build_slot1_context(&st) : nil)] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetIsNetworkSelectionMenuAvailableRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetIsNetworkSelectionMenuAvailableResponse");
			resp = rcls ? [(SweetCTAvailResp *)[rcls alloc] initWithIsAvailable:st.sim_present] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetIsNetworkReselectionNeededRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetIsNetworkReselectionNeededResponse");
			resp = rcls ? [(SweetCTReselResp *)[rcls alloc] initWithNeedsReselection:NO] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetNetworkSelectionInfoRequest"]) {
			Class icls = NSClassFromString(@"CTNetworkSelectionInfo");
			id info = icls ? [[icls alloc] init] : nil;
			ct_set(info, @"selectionState", @(1));
			ct_set(info, @"selectionMode", @(0));
			Class rcls = NSClassFromString(@"CTXPCGetNetworkSelectionInfoResponse");
			resp = rcls ? [(SweetCTNetSelInfoResp *)[rcls alloc] initWithNetworkSelectionInfo:info] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetNetworkSelectionRequest"]) {
			Class ncls = NSClassFromString(@"CTNetwork");
			id net = ncls ? [[ncls alloc] init] : nil;
			if (st.operator_name[0])
				ct_set(net, @"name", [NSString stringWithUTF8String:st.operator_name]);
			Class rcls = NSClassFromString(@"CTXPCGetNetworkSelectionResponse");
			resp = rcls ? [(SweetCTNetResp *)[rcls alloc] initWithNetwork:net] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetNetworkListRequest"] ||
			   [cname isEqualToString:@"CTXPCFetchNetworkListRequest"]) {
			Class lcls = NSClassFromString(@"CTNetworkList");
			id nl = lcls ? [[lcls alloc] init] : nil;
			ct_set(nl, @"networks", [NSMutableArray array]);
			ct_set(nl, @"moreComing", @(NO));
			Class rcls = NSClassFromString(@"CTXPCGetNetworkListResponse");
			resp = rcls ? [(SweetCTNetListResp *)[rcls alloc] initWithNetworkList:nl] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetIMSRegistrationStatusRequest"]) {
			Class icls = NSClassFromString(@"CTIMSRegistrationStatus");
			id ims = icls ? [[icls alloc] init] : nil;
			Class rcls = NSClassFromString(@"CTXPCGetIMSRegistrationStatusResponse");
			resp = rcls ? [(SweetCTIMSResp *)[rcls alloc] initWithIMSRegistrationStatus:ims] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetRegistrationIMSTransportInfoRequest"]) {
			Class icls = NSClassFromString(@"CTIMSRegistrationTransportInfo");
			id ims = icls ? [[icls alloc] init] : nil;
			Class rcls = NSClassFromString(@"CTXPCGetRegistrationIMSTransportInfoResponse");
			resp = rcls ? [(SweetCTIMSTransResp *)[rcls alloc] initWithIMSRegistrationInfo:ims] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetEncryptionStatusRequest"]) {
			Class ecls = NSClassFromString(@"CTEncryptionStatusInfo");
			id enc = ecls ? [(SweetCTEncryptionStatusInfo *)[ecls alloc] initWithStatus:0] : nil;
			Class rcls = NSClassFromString(@"CTXPCGetEncryptionStatusResponse");
			resp = rcls ? [(SweetCTEncResp *)[rcls alloc] initWithEncryptionStatusInfo:enc] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetRejectCauseCodeRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCGetRejectCauseCodeResponse");
			resp = rcls ? [(SweetCTCauseResp *)[rcls alloc] initWithCauseCode:@(st.last_reject_cause)] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetCallCapabilitiesRequest"]) {
			id caps = build_call_capabilities_obj();
			Class rcls = NSClassFromString(@"CTXPCGetCallCapabilitiesResponse");
			if (rcls) {
				if ([rcls instancesRespondToSelector:@selector(initWithCapabilities:)])
					resp = [(SweetCTCallCapResp *)[rcls alloc] initWithCapabilities:caps];
				else {
					resp = [[rcls alloc] init];
					ct_set(resp, @"capabilities", caps);
				}
			}
		} else if ([cname isEqualToString:@"CTXPCSupportEmbeddedSimRequest"] ||
			   [cname isEqualToString:@"CTXPCEmbeddedSIMOnlyConfigRequest"] ||
			   [cname isEqualToString:@"CTXPCNeedToLaunchSetUpeSIMRequest"] ||
			   [cname isEqualToString:@"CTXPCUsingBootstrapDataServiceRequest"] ||
			   [cname isEqualToString:@"CTXPCSupportsHydraRequest"] ||
			   [cname isEqualToString:@"CTXPCSupportsLimitedUseSIMsRequest"] ||
			   [cname isEqualToString:@"CTXPCShouldShoweSIMTravelTipRequest"] ||
			   [cname isEqualToString:@"CTXPCIsMemoryConstrainedRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCBooleanResponseMessage");
			resp = rcls ? [(SweetCTBoolResp *)[rcls alloc] initWithResult:NO] : nil;
		} else if ([cname isEqualToString:@"CTXPCEmbeddedSIMHealthRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCBooleanResponseMessage");
			resp = rcls ? [(SweetCTBoolResp *)[rcls alloc] initWithResult:YES] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetPendingInstallPlansRequest"]) {
			Class dcls = NSClassFromString(@"CTDisplayPlanList");
			id empty = dcls ? [(SweetCTDisplayPlanList *)[dcls alloc] initWithDisplayPlans:@[]] : nil;
			Class rcls = NSClassFromString(@"CTXPCGetPendingInstallPlansResponse");
			resp = rcls ? [(SweetCTPlanListResp *)[rcls alloc] initWithPlanList:empty] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetCarrierSetupItemsRequest"] ||
			   [cname isEqualToString:@"CTXPCGetHiddenTransferPlansRequest"] ||
			   [cname isEqualToString:@"CTXPCGetPlansPendingCrossPlatformTransferRequest"]) {
			Class dcls = NSClassFromString(@"CTDisplayPlanList");
			id empty = dcls ? [(SweetCTDisplayPlanList *)[dcls alloc] initWithDisplayPlans:@[]] : nil;
			Class rcls = NSClassFromString([cname stringByReplacingOccurrencesOfString:@"Request"
											withString:@"Response"]);
			resp = rcls ? [(SweetCTListResp *)[rcls alloc] initWithList:empty] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetRemoteDevicesRequest"]) {
			Class lcls = NSClassFromString(@"CTRemoteDeviceList");
			id empty = lcls ? [[lcls alloc] init] : nil;
			ct_set(empty, @"devices", [NSMutableArray array]);
			Class rcls = NSClassFromString(@"CTXPCGetRemoteDevicesResponse");
			resp = rcls ? [(SweetCTListResp *)[rcls alloc] initWithList:empty] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetRemoteDevicesForTransferRequest"]) {
			Class lcls = NSClassFromString(@"CTRemoteDeviceList");
			id empty = lcls ? [[lcls alloc] init] : nil;
			ct_set(empty, @"devices", [NSMutableArray array]);
			Class rcls = NSClassFromString(@"CTXPCGetRemoteDevicesForTransferResponse");
			resp = rcls ? [(SweetCTDevsResp *)[rcls alloc] initWithDevices:empty] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetTransferPlansRequest"]) {
			Class lcls = NSClassFromString(@"CTRemoteDeviceList");
			id empty = lcls ? [[lcls alloc] init] : nil;
			ct_set(empty, @"devices", [NSMutableArray array]);
			Class rcls = NSClassFromString(@"CTXPCGetTransferPlansResponse");
			resp = rcls ? [(SweetCTTransferPlansResp *)[rcls alloc]
				       initWithRemtoeDevices:empty isFlexPolicyOn:NO] : nil;
		} else if ([cname isEqualToString:@"CTXPCGet2GSwitchEnabledRequest"] ||
			   [cname isEqualToString:@"CTXPCGet2GUserPreferenceRequest"] ||
			   [cname isEqualToString:@"CTXPCGetRegulatedRATsSwitchEnabledRequest"] ||
			   [cname isEqualToString:@"CTXPCGetRegulatedRATsUserPreferenceRequest"]) {
			Class rcls = NSClassFromString([cname stringByReplacingOccurrencesOfString:@"Request"
											withString:@"Response"]);
			resp = rcls ? [(SweetCTEnabledResp *)[rcls alloc] initWithIsEnabled:NO] : nil;
		} else if ([cname isEqualToString:@"CTXPCMobileEquipmentInfoRequest"]) {
			/* Privacy-safe equipment info: firmware baseVersion only, NO IMEI/ICCID/IMSI */
			Class mcls = NSClassFromString(@"CTMobileEquipmentInfo");
			Class lcls = NSClassFromString(@"CTMobileEquipmentInfoList");
			id me = mcls ? [[mcls alloc] init] : nil;
			ct_set(me, @"slotId", @(1));
			if (st.modem_rev[0])
				ct_set(me, @"baseVersion", [NSString stringWithUTF8String:st.modem_rev]);
			id list = lcls ? [[lcls alloc] init] : nil;
			ct_set(list, @"meInfoList", me ? @[me] : @[]);
			Class rcls = NSClassFromString(@"CTXPCMobileEquipmentInfoResponse");
			resp = rcls ? [(SweetCTMeInfoResp *)[rcls alloc] initWithEquipmentInfoList:list] : nil;
		} else if ([cname isEqualToString:@"CTXPCGetDeviceInfoRequest"]) {
			NSString *fw = st.modem_rev[0] ? [NSString stringWithUTF8String:st.modem_rev] : @"SM8150-MPSS";
			Class rcls = NSClassFromString(@"CTXPCGetDeviceInfoResponse");
			resp = rcls ? [(SweetCTInfoResp *)[rcls alloc] initWithInfo:@{ @"ModemFirmware": fw }] : nil;
		} else if ([cname isEqualToString:@"CTXPCFirmwareUpdateInfoRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCFirmwareUpdateInfoResponse");
			resp = rcls ? [(SweetCTFwInfoResp *)[rcls alloc] initWithFimwareUpdateInfo:@{}] : nil;
		} else if ([cname isEqualToString:@"CTXPCSetDefaultDataRequest"] ||
			   [cname isEqualToString:@"CTXPCSetDefaultVoiceRequest"] ||
			   [cname isEqualToString:@"CTXPCSetRatSelectionRequest"] ||
			   [cname isEqualToString:@"CTXPCSetRatSelectionMaskRequest"] ||
			   [cname isEqualToString:@"CTXPCSetMaxDataRateRequest"] ||
			   [cname isEqualToString:@"CTXPCSaveCellularSettingsRequest"]) {
			Class rcls = NSClassFromString(@"CTXPCResponseMessage");
			resp = rcls ? [[rcls alloc] init] : nil;
		}

		if (resp) {
			completionHandler(resp, nil);
			return;
		}

		os_unfair_lock_lock(&g_clients_lock);
		BOOL first = ![g_logged_unhandled containsObject:cname];
		if (first)
			[g_logged_unhandled addObject:cname];
		os_unfair_lock_unlock(&g_clients_lock);
		if (first)
			clog("unhandled CTXPCRequestMessage: %s", [cname UTF8String]);

		completionHandler(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOTSUP userInfo:nil]);
	} @catch (id exc) {
		clog("sendRequest exception: %s", [[exc description] UTF8String]);
		completionHandler(nil, [NSError errorWithDomain:NSPOSIXErrorDomain code:EFAULT userInfo:nil]);
	}
}

/* -- Type-safe fallback for any unimplemented CTXPCServiceInterface selector -- */

- (BOOL)respondsToSelector:(SEL)aSelector
{
	if ([super respondsToSelector:aSelector])
		return YES;
	Protocol *p = objc_getProtocol("CTXPCServiceInterface");
	if (p) {
		struct objc_method_description d = find_proto_method(p, aSelector, [NSMutableSet set]);
		if (d.types != NULL)
			return YES;
	}
	return NO;
}

- (NSMethodSignature *)methodSignatureForSelector:(SEL)aSelector
{
	NSMethodSignature *sig = [super methodSignatureForSelector:aSelector];
	if (sig)
		return sig;
	Protocol *p = objc_getProtocol("CTXPCServiceInterface");
	if (p) {
		struct objc_method_description d = find_proto_method(p, aSelector, [NSMutableSet set]);
		if (d.types)
			return [NSMethodSignature signatureWithObjCTypes:d.types];
	}
	return nil;
}

static void invoke_ct_reply_block(id replyBlock, BOOL setterOnly,
				  id objVal, BOOL boolVal, NSInteger intVal, NSError *errVal)
{
	if (!replyBlock || !fn_Block_signature)
		return;
	const char *bsig = fn_Block_signature((__bridge void *)replyBlock);
	if (!bsig)
		return;
	NSMethodSignature *bms = [NSMethodSignature signatureWithObjCTypes:bsig];
	if (!bms)
		return;
	NSInvocation *binv = [NSInvocation invocationWithMethodSignature:bms];
	[binv setTarget:replyBlock];
	uint8_t zero[64] = { 0 };
	NSUInteger n = bms.numberOfArguments;
	for (NSUInteger k = 1; k < n; k++) {
		const char *pt = [bms getArgumentTypeAtIndex:k];
		if (!pt) {
			[binv setArgument:zero atIndex:(NSInteger)k];
			continue;
		}
		bool isErrSlot = (strstr(pt, "NSError") != NULL) ||
				 (setterOnly && n == 2 && pt[0] == '@') ||
				 (n >= 3 && k == n - 1 && pt[0] == '@');
		if (isErrSlot) {
			__unsafe_unretained NSError *uerr = errVal;
			[binv setArgument:&uerr atIndex:(NSInteger)k];
		} else if (k == 1) {
			if (pt[0] == '@') {
				__unsafe_unretained id uobj = objVal;
				[binv setArgument:&uobj atIndex:(NSInteger)k];
			} else if (pt[0] == 'B') {
				bool bv = boolVal ? true : false;
				[binv setArgument:&bv atIndex:(NSInteger)k];
			} else if (pt[0] == 'c' || pt[0] == 'C') {
				signed char cv = boolVal ? 1 : 0;
				[binv setArgument:&cv atIndex:(NSInteger)k];
			} else if (pt[0] == 'i' || pt[0] == 'I') {
				int iv = (int)intVal;
				[binv setArgument:&iv atIndex:(NSInteger)k];
			} else if (pt[0] == 'q' || pt[0] == 'Q') {
				long long qv = (long long)intVal;
				[binv setArgument:&qv atIndex:(NSInteger)k];
			} else {
				[binv setArgument:zero atIndex:(NSInteger)k];
			}
		} else {
			[binv setArgument:zero atIndex:(NSInteger)k];
		}
	}
	[binv invoke];
}

- (BOOL)handleDynamicCTSelector:(NSString *)sname invocation:(NSInvocation *)inv
{
	NSMethodSignature *sig = inv.methodSignature;
	NSUInteger argc = sig.numberOfArguments;
	__unsafe_unretained id replyBlock = nil;
	if (argc >= 3) {
		const char *lastType = [sig getArgumentTypeAtIndex:argc - 1];
		if (lastType && strncmp(lastType, "@?", 2) == 0)
			[inv getArgument:&replyBlock atIndex:(NSInteger)(argc - 1)];
	}

	struct cellular_state st = snapshot_state();

	if ([sname isEqualToString:@"getCallCapabilities:completion:"] ||
	    [sname isEqualToString:@"getCallCapabilities:"]) {
		invoke_ct_reply_block(replyBlock, NO, build_call_capabilities_obj(), YES, 1, nil);
		return YES;
	}
	if ([sname isEqualToString:@"context:getSystemCapabilities:"] ||
	    [sname isEqualToString:@"context:getSystemCapabilitiesWithCompletion:"] ||
	    [sname isEqualToString:@"getSystemCapabilities:completion:"]) {
		invoke_ct_reply_block(replyBlock, NO, build_system_capabilities_dict(), YES, 1, nil);
		return YES;
	}
	if ([sname isEqualToString:@"copyCellInfo:completion:"]) {
		Class ccls = NSClassFromString(@"CTCellInfo");
		id ci = ccls ? [[ccls alloc] init] : nil;
		invoke_ct_reply_block(replyBlock, NO, ci, NO, 0, nil);
		return YES;
	}
	if ([sname isEqualToString:@"smartDataMode:completion:"]) {
		invoke_ct_reply_block(replyBlock, NO, @(NO), NO, 0, nil);
		return YES;
	}
	if ([sname isEqualToString:@"getEnglishCarrierNameFor:completion:"]) {
		NSString *op = st.operator_name[0] ? [NSString stringWithUTF8String:st.operator_name] : @"";
		invoke_ct_reply_block(replyBlock, NO, op, YES, 0, nil);
		return YES;
	}
	if ([sname isEqualToString:@"copyCarrierBundleValue:keyHierarchy:bundleType:completion:"] ||
	    [sname isEqualToString:@"copyCarrierBundleValueWithDefault:keyHierarchy:bundleType:completion:"]) {
		__unsafe_unretained id keyHier = nil;
		__unsafe_unretained id defVal = nil;
		if (argc > 3 && [sig getArgumentTypeAtIndex:3][0] == '@')
			[inv getArgument:&keyHier atIndex:3];
		if ([sname hasPrefix:@"copyCarrierBundleValueWithDefault:"] &&
		    argc > 2 && [sig getArgumentTypeAtIndex:2][0] == '@') {
			/* Note: arg2 is context on both; default value if present is nil-safe */
			(void)defVal;
		}
		id val = resolve_carrier_bundle_val(&st, keyHier, nil);
		invoke_ct_reply_block(replyBlock, NO, val, val != nil, 0, nil);
		return YES;
	}
	if ([sname isEqualToString:@"copyCarrierBookmarks:completion:"]) {
		invoke_ct_reply_block(replyBlock, NO, @[], NO, 0, nil);
		return YES;
	}
	if ([sname isEqualToString:@"getPNRContext:completion:"]) {
		Class pcls = NSClassFromString(@"CTPNRContextInfo");
		id pnr = pcls ? [[pcls alloc] init] : nil;
		ct_set(pnr, @"isReady", @NO);
		NSString *plmn = (st.mcc[0] && st.mnc[0])
				 ? [NSString stringWithFormat:@"%s%s", st.mcc, st.mnc] : @"";
		ct_set(pnr, @"plmn", plmn);
		invoke_ct_reply_block(replyBlock, NO, pnr, NO, 0, nil);
		return YES;
	}
	if ([sname isEqualToString:@"getSupports5G:completion:"]) {
		invoke_ct_reply_block(replyBlock, NO, @NO, NO, 0, nil);
		return YES;
	}
	if ([sname isEqualToString:@"getInternationalDataAccess:completion:"] ||
	    [sname isEqualToString:@"getInternationalDataAccessSync:completion:"]) {
		invoke_ct_reply_block(replyBlock, NO, @YES, YES, 1, nil);
		return YES;
	}
	if ([sname isEqualToString:@"getConnectionState:connectionType:completion:"]) {
		Class ccls = NSClassFromString(@"CTDataConnectionStatus");
		id cs = ccls ? [[ccls alloc] init] : nil;
		NSInteger stateInt = st.data_connected ? 2 : 0;
		ct_set(cs, @"state", @(stateInt));
		ct_set(cs, @"ipFamily", @(st.wds_ip_family));
		ct_set(cs, @"interfaceName", st.data_connected ? @"pdp_ip0" : @"");
		ct_set(cs, @"apnName", st.data_apn[0] ? [NSString stringWithUTF8String:st.data_apn] : @"");
		invoke_ct_reply_block(replyBlock, NO, cs, st.data_connected, stateInt, nil);
		return YES;
	}
	if ([sname isEqualToString:@"getTetheringStatus:connectionType:"] ||
	    [sname isEqualToString:@"getTetheringStatus:connectionType:completion:"] ||
	    [sname isEqualToString:@"getTetheringStatus:completion:"]) {
		Class tcls = NSClassFromString(@"CTTetheringStatus");
		id ts = tcls ? [[tcls alloc] init] : nil;
		ct_set(ts, @"carrierEnabled", @NO);
		ct_set(ts, @"userAuthenticated", @NO);
		ct_set(ts, @"misPdpMaxHosts", @0);
		invoke_ct_reply_block(replyBlock, NO, ts, NO, 0, nil);
		return YES;
	}
	if ([sname isEqualToString:@"refreshCellMonitor:completion:"] ||
	    [sname isEqualToString:@"setPacketContextActiveByServiceType:connectionType:active:completion:"] ||
	    [sname isEqualToString:@"setAssertionForConnectionType:enable:completion:"]) {
		invoke_ct_reply_block(replyBlock, YES, @YES, YES, 0, nil);
		return YES;
	}
	if ([sname isEqualToString:@"reliableNetworkFallback:completion:"]) {
		Class rcls = NSClassFromString(@"CTReliableNetworkFallbackSettings");
		id obj = rcls ? [[rcls alloc] init] : @NO;
		if (rcls)
			ct_set(obj, @"isEnabled", @NO);
		invoke_ct_reply_block(replyBlock, NO, obj, NO, 0, nil);
		return YES;
	}
	if ([sname isEqualToString:@"privacyProxy:completion:"]) {
		Class pcls = NSClassFromString(@"CTPrivacyProxyInfo");
		id obj = pcls ? [[pcls alloc] init] : @{};
		invoke_ct_reply_block(replyBlock, NO, obj, NO, 0, nil);
		return YES;
	}
	if ([sname isEqualToString:@"copyBundleIdentifier:bundleType:completion:"]) {
		invoke_ct_reply_block(replyBlock, NO, @"com.apple.DefaultCarrier_bundle", YES, 0, nil);
		return YES;
	}
	if ([sname isEqualToString:@"copyBundleVersion:bundleType:completion:"]) {
		invoke_ct_reply_block(replyBlock, NO, @"64.0", YES, 0, nil);
		return YES;
	}
	return NO;
}

- (void)forwardInvocation:(NSInvocation *)anInvocation
{
	NSString *sname = NSStringFromSelector(anInvocation.selector);
	@try {
		if ([self handleDynamicCTSelector:sname invocation:anInvocation])
			return;
	} @catch (id exc) {
		clog("handleDynamicCTSelector exception (%s): %s",
		     [sname UTF8String], [[exc description] UTF8String]);
	}

	os_unfair_lock_lock(&g_clients_lock);
	BOOL first = ![g_logged_unhandled containsObject:sname];
	if (first)
		[g_logged_unhandled addObject:sname];
	os_unfair_lock_unlock(&g_clients_lock);
	if (first)
		clog("unhandled XPC selector: %s", [sname UTF8String]);

	/* If last parameter is a reply block (@?), invoke it safely so caller never hangs */
	@try {
		NSMethodSignature *sig = anInvocation.methodSignature;
		NSUInteger argc = sig.numberOfArguments;
		if (argc >= 3) {
			const char *lastType = [sig getArgumentTypeAtIndex:argc - 1];
			if (lastType && strncmp(lastType, "@?", 2) == 0) {
				__unsafe_unretained id replyBlock = nil;
				[anInvocation getArgument:&replyBlock atIndex:(NSInteger)(argc - 1)];
				NSError *err = [NSError errorWithDomain:NSPOSIXErrorDomain
								   code:ENOTSUP
							       userInfo:nil];
				invoke_ct_reply_block(replyBlock, NO, nil, NO, 0, err);
			}
		}
	} @catch (id exc) {
		clog("forwardInvocation block reply exception (%s): %s",
		     [sname UTF8String], [[exc description] UTF8String]);
	}
}

@end

/* ---- NSXPCListener Delegate for com.apple.commcenter.coretelephony.xpc ---- */

@interface SweetCellularListenerDelegate : NSObject
@end

@implementation SweetCellularListenerDelegate

- (BOOL)listener:(id)listener shouldAcceptNewConnection:(SweetXPCConnectionSPI *)newConnection
{
	(void)listener;
	@try {
		Class ifcCls = NSClassFromString(@"NSXPCInterface");
		Protocol *expProto = objc_getProtocol("CTXPCServiceInterface");
		Protocol *remProto = objc_getProtocol("CoreTelephonyClientDelegateProxyInterface");
		if (!ifcCls || !expProto) {
			clog("CTXPCServiceInterface protocol missing in runtime!");
			return NO;
		}
		SweetXPCInterfaceSPI *expIfc = [(id)ifcCls interfaceWithProtocol:expProto];
		NSSet<Class> *allowed = build_allowed_ct_classes();
		[expIfc setClasses:allowed
		       forSelector:@selector(sendRequest:completionHandler:)
		     argumentIndex:0
			   ofReply:NO];
		[expIfc setClasses:allowed
		       forSelector:@selector(sendRequest:completionHandler:)
		     argumentIndex:0
			   ofReply:YES];
		static const char * const extraReplySels[] = {
			"copyCarrierBundleValue:keyHierarchy:bundleType:completion:",
			"copyCarrierBundleValueWithDefault:keyHierarchy:bundleType:completion:",
			"context:getSystemCapabilities:",
			"context:getSystemCapabilitiesWithCompletion:",
			"getCallCapabilities:completion:",
		};
		for (size_t si = 0; si < sizeof(extraReplySels) / sizeof(extraReplySels[0]); si++) {
			SEL s = sel_registerName(extraReplySels[si]);
			struct objc_method_description md = find_proto_method(expProto, s, [NSMutableSet set]);
			if (md.types) {
				@try {
					[expIfc setClasses:allowed forSelector:s argumentIndex:0 ofReply:YES];
				} @catch (id exc) {
					(void)exc;
				}
			}
		}

		SweetXPCInterfaceSPI *remIfc = remProto ? [(id)ifcCls interfaceWithProtocol:remProto] : nil;

		SweetCellularXPCService *svc = [[SweetCellularXPCService alloc] init];
		svc.connection = newConnection;
		newConnection.exportedInterface = expIfc;
		newConnection.exportedObject = svc;
		if (remIfc)
			newConnection.remoteObjectInterface = remIfc;

		os_unfair_lock_lock(&g_clients_lock);
		[g_xpc_clients addObject:svc];
		os_unfair_lock_unlock(&g_clients_lock);

		__weak SweetCellularXPCService *weakSvc = svc;
		int pid = newConnection.processIdentifier;
		newConnection.invalidationHandler = ^{
			SweetCellularXPCService *strongSvc = weakSvc;
			if (strongSvc) {
				os_unfair_lock_lock(&g_clients_lock);
				[g_xpc_clients removeObjectIdenticalTo:strongSvc];
				os_unfair_lock_unlock(&g_clients_lock);
			}
		};
		clog("accepted CoreTelephony XPC client pid %d", pid);
		[newConnection resume];
		return YES;
	} @catch (id exc) {
		clog("shouldAcceptNewConnection exception: %s", [[exc description] UTF8String]);
		return NO;
	}
}

@end

/* Push live delegate callbacks to all connected CoreTelephonyClient processes */
static void broadcast_ct_notifications(const struct cellular_state *st, bool sim_changed)
{
	NSArray<SweetCellularXPCService *> *clients = nil;
	os_unfair_lock_lock(&g_clients_lock);
	clients = [g_xpc_clients copy];
	os_unfair_lock_unlock(&g_clients_lock);

	id ctx1 = build_slot1_context(st);
	id sigInfo = build_signal_info(st);
	id dispStatus = build_display_status(st);
	id dataStatus = build_data_status(st);
	NSString *simSt = sim_status_string(st);
	NSString *opName = st->operator_name[0] ? [NSString stringWithUTF8String:st->operator_name] : @"";

	for (SweetCellularXPCService *svc in clients) {
		if (!svc.registeredNotifications || !svc.connection)
			continue;
		@try {
			SweetCTDelegateProxy *proxy = [svc.connection remoteObjectProxyWithErrorHandler:^(NSError *e) {
				(void)e;
			}];
			if (!proxy)
				continue;
			if (sim_changed) {
				[proxy subscriptionInfoDidChange];
				[proxy activeSubscriptionsDidChange];
				if (ctx1)
					[proxy simStatusDidChange:ctx1 status:simSt];
			}
			if (ctx1) {
				if (sigInfo)
					[proxy signalStrengthChanged:ctx1 info:sigInfo];
				[proxy operatorNameChanged:ctx1 name:opName];
				if (dispStatus)
					[proxy displayStatusChanged:ctx1 status:dispStatus];
				if (dataStatus)
					[proxy dataStatus:ctx1 dataStatusInfo:dataStatus];
				[proxy servingNetworkChanged:ctx1];
			}
		} @catch (id exc) {
			(void)exc;
		}
	}

	if (fn_notify_post) {
		if (sim_changed)
			fn_notify_post("com.apple.commcenter.SimStatus.changed");
		fn_notify_post("com.apple.commcenter.Plmn.changed");
	}
}

/* ---- Legacy com.apple.commcenter.xpc (CTServerConnection) Listener ---- */

static sweet_xpc_t g_legacy_listener;

static void handle_legacy_xpc_dict(sweet_xpc_t peer, sweet_xpc_t msg)
{
	if (fn_xpc_dict_get_string) {
		const char *prog = fn_xpc_dict_get_string(msg, "kProgName");
		if (prog && prog[0])
			clog("CTServerConnection whois from '%s'", prog);
	}
	if (!fn_xpc_dict_create_reply || !fn_xpc_conn_send)
		return;
	sweet_xpc_t reply = fn_xpc_dict_create_reply(msg);
	if (!reply)
		return; /* One-way notification/registration message without reply port */

	struct cellular_state st = snapshot_state();
	fn_xpc_dict_set_int64(reply, "kPosixError", 0);
	fn_xpc_dict_set_int64(reply, "kMachError", 0);
	fn_xpc_dict_set_string(reply, "kCTSIMSupportSIMStatus", [sim_status_string(&st) UTF8String]);
	fn_xpc_dict_set_string(reply, "kCTRegistrationStatus", [reg_status_string(&st) UTF8String]);
	if (st.operator_name[0])
		fn_xpc_dict_set_string(reply, "kCTRegistrationOperatorName", st.operator_name);
	fn_xpc_dict_set_bool(reply, "kAutoReadOutEnabled", false);
	fn_xpc_dict_set_bool(reply, "kSmsCellBroadcastEnabled", false);

	/* Populate kCTCopySystemCapabilities / kCTCapabilityPhoneServices */
	if (fn_xpc_dict_create && fn_xpc_dict_set_value) {
		sweet_xpc_t phone_caps = fn_xpc_dict_create(NULL, NULL, 0);
		sweet_xpc_t sys_caps = fn_xpc_dict_create(NULL, NULL, 0);
		if (phone_caps && sys_caps) {
			fn_xpc_dict_set_bool(phone_caps, "kCTCapabilityAgent", true);
			fn_xpc_dict_set_bool(phone_caps, "kCTCapabilityAgent2", true);
			fn_xpc_dict_set_bool(phone_caps, "kCTCallManagementIsPhoneServicesActive", true);
			fn_xpc_dict_set_bool(phone_caps, "kCTCallManagementPhoneServicesCSCallingAvailable", true);
			fn_xpc_dict_set_bool(phone_caps, "kCTCallManagementPhoneServicesVoLTECallingAvailable", true);
			fn_xpc_dict_set_bool(phone_caps, "kCTCallManagementEmergencyWifiAllowed", false);
			fn_xpc_dict_set_bool(phone_caps, "kCTCallManagementEmergencyWifiCallCapable", false);
			fn_xpc_dict_set_value(sys_caps, "kCTCapabilityPhoneServices", phone_caps);
			fn_xpc_dict_set_value(reply, "kCTCapabilityPhoneServices", phone_caps);
			fn_xpc_dict_set_value(reply, "kCTSystemCapabilities", sys_caps);
			fn_xpc_dict_set_value(reply, "kCTCopySystemCapabilities", sys_caps);
		}
		if (phone_caps && fn_xpc_release)
			fn_xpc_release(phone_caps);
		if (sys_caps && fn_xpc_release)
			fn_xpc_release(sys_caps);
	}

	/* Privacy-safe emergency number check in RAM ONLY (never log digits) */
	if (fn_xpc_dict_get_string) {
		static const char * const num_keys[] = {
			"kCTEmergencyNumber", "kCTCallNumber", "PhoneNumber", "number", "kNumber"
		};
		const char *num = NULL;
		for (size_t i = 0; i < sizeof(num_keys) / sizeof(num_keys[0]); i++) {
			num = fn_xpc_dict_get_string(msg, num_keys[i]);
			if (num && num[0])
				break;
		}
		const char *req_cmd = fn_xpc_dict_get_string(msg, "request");
		if (!req_cmd)
			req_cmd = fn_xpc_dict_get_string(msg, "kRequest");
		bool is_emerg_req = (num != NULL) ||
				    (req_cmd && strstr(req_cmd, "Emergency") != NULL) ||
				    (fn_xpc_dict_get_value &&
				     fn_xpc_dict_get_value(msg, "kCTCopyIsEmergencyNumber") != NULL);
		if (is_emerg_req) {
			bool em = is_emergency_cstr(num);
			fn_xpc_dict_set_bool(reply, "kCTIsEmergencyNumber", em);
			fn_xpc_dict_set_bool(reply, "isEmergencyNumber", em);
			fn_xpc_dict_set_bool(reply, "kIsEmergencyNumber", em);
		}
	}

	fn_xpc_conn_send(peer, reply);
	if (fn_xpc_release)
		fn_xpc_release(reply);
}

static void start_legacy_commcenter_xpc(dispatch_queue_t q)
{
	if (!fn_xpc_conn_create_mach || !fn_xpc_conn_set_handler || !fn_xpc_conn_resume) {
		clog("libxpc mach service symbols unavailable");
		return;
	}
	/* XPC_CONNECTION_MACH_SERVICE_LISTENER = (1ULL << 0) */
	sweet_xpc_t listener = fn_xpc_conn_create_mach("com.apple.commcenter.xpc", q, 1ULL);
	if (!listener) {
		clog("failed to create com.apple.commcenter.xpc listener");
		return;
	}
	g_legacy_listener = listener;
	fn_xpc_conn_set_handler(listener, ^(sweet_xpc_t event) {
		if (!event || !fn_xpc_get_type)
			return;
		const void *t = fn_xpc_get_type(event);
		if (t == sym_xpc_type_connection) {
			sweet_xpc_t peer = event;
			if (fn_xpc_retain)
				fn_xpc_retain(peer);
			if (fn_xpc_conn_set_target_queue)
				fn_xpc_conn_set_target_queue(peer, q);
			fn_xpc_conn_set_handler(peer, ^(sweet_xpc_t msg) {
				if (!msg)
					return;
				const void *mt = fn_xpc_get_type(msg);
				if (mt == sym_xpc_type_dictionary) {
					handle_legacy_xpc_dict(peer, msg);
				} else if (msg == sym_xpc_error_conn_invalid) {
					if (fn_xpc_release)
						fn_xpc_release(peer);
				}
			});
			fn_xpc_conn_resume(peer);
		}
	});
	fn_xpc_conn_resume(listener);
	clog("listening on com.apple.commcenter.xpc (CTServerConnection)");
}

/* ---- QMI / QRTR Background Engine ---- */

static dispatch_queue_t g_qmi_queue;
static dispatch_source_t g_qrtr_read_src;
static dispatch_source_t g_poll_timer;
static struct qmi_ctx g_qmi;
static unsigned g_poll_ticks;
static uint64_t g_last_online_try_ms;
static int g_online_attempts;
static uint8_t g_last_logged_dms_mode = 0xfe;
static uint64_t g_last_wds_try_ms;

static uint64_t cell_mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)(ts.tv_nsec / 1000000ull);
}

static uint32_t core_srv_mask(const struct qmi_ctx *ctx)
{
	uint32_t m = 0;
	if (qmi_has_service(ctx, QMI_SVC_DMS))   m |= QMI_IND_MASK_DMS;
	if (qmi_has_service(ctx, QMI_SVC_UIM))   m |= QMI_IND_MASK_UIM;
	if (qmi_has_service(ctx, QMI_SVC_NAS))   m |= QMI_IND_MASK_NAS;
	if (qmi_has_service(ctx, QMI_SVC_WDS))   m |= QMI_IND_MASK_WDS;
	if (qmi_has_service(ctx, QMI_SVC_VOICE)) m |= QMI_IND_MASK_VOICE;
	return m;
}

static void try_connect_and_sync(void);
static void commit_and_publish(const struct cellular_state *next, bool force_broadcast);

/* ---- CallKit CXXPCProvider + Voice Call Bridge + Local CLI Control Socket ---- */

@interface SweetTrackedCall : NSObject
@property (nonatomic, strong) NSUUID *uuid;
@property (nonatomic, assign) uint8_t callId;
@property (nonatomic, assign) uint8_t lastState;
@property (nonatomic, assign) BOOL outgoing;
@property (nonatomic, assign) BOOL reportedConnecting;
@property (nonatomic, assign) BOOL reportedSentInvitation;
@property (nonatomic, assign) BOOL reportedConnected;
@property (nonatomic, assign) BOOL reportedIncoming;
@property (nonatomic, assign) BOOL reportedRemoteHandle;
@end

@implementation SweetTrackedCall
@end

@interface SweetCXProviderDelegate : NSObject
@end

static SweetCXProviderSPI *g_cx_provider;
static SweetCXProviderDelegate *g_cx_delegate;
static NSMutableArray<SweetTrackedCall *> *g_tracked_calls;
static dispatch_source_t g_ctrl_sock_src;
static int g_ctrl_listen_fd = -1;

static NSString *iso_country_for_mcc(const char *mcc)
{
	if (!mcc || !mcc[0])
		return @"in";
	if (!strcmp(mcc, "404") || !strcmp(mcc, "405") || !strcmp(mcc, "406"))
		return @"in";
	if (!strncmp(mcc, "31", 2))
		return @"us";
	if (!strcmp(mcc, "234") || !strcmp(mcc, "235"))
		return @"gb";
	if (!strcmp(mcc, "262"))
		return @"de";
	if (!strcmp(mcc, "208"))
		return @"fr";
	if (!strcmp(mcc, "440") || !strcmp(mcc, "441"))
		return @"jp";
	if (!strcmp(mcc, "460"))
		return @"cn";
	if (!strcmp(mcc, "505"))
		return @"au";
	if (!strcmp(mcc, "302"))
		return @"ca";
	return @"in";
}

static id build_cx_call_update(const struct qmi_voice_call *c, const struct cellular_state *st);
static void refresh_callkit_configuration(const struct cellular_state *st);

static SweetTrackedCall *find_tracked_by_uuid(NSUUID *uuid)
{
	if (!uuid)
		return nil;
	for (SweetTrackedCall *tc in g_tracked_calls) {
		if ([tc.uuid isEqual:uuid])
			return tc;
	}
	return nil;
}

static SweetTrackedCall *find_tracked_by_id(uint8_t cid)
{
	if (!cid)
		return nil;
	for (SweetTrackedCall *tc in g_tracked_calls) {
		if (tc.callId == cid)
			return tc;
	}
	return nil;
}

@implementation SweetCXProviderDelegate

- (void)providerDidReset:(id)provider
{
	(void)provider;
	clog("CallKit providerDidReset (tracked=%lu)", (unsigned long)g_tracked_calls.count);
	if (g_tracked_calls.count > 0 && g_qmi.fd >= 0)
		(void)qmi_voice_manage(&g_qmi, 0x08u, 0);
	[g_tracked_calls removeAllObjects];
	sweet_voice_write_shm(0, 0, 0);
}

- (void)providerDidBegin:(id)provider
{
	(void)provider;
	clog("CallKit providerDidBegin");
	struct cellular_state st = snapshot_state();
	refresh_callkit_configuration(&st);
}

- (void)provider:(SweetCXProviderSPI *)provider performStartCallAction:(SweetCXActionSPI *)action
{
	@try {
		NSUUID *uuid = [action callUUID];
		id handle = ct_get(action, @"handle");
		NSString *numStr = [ct_get(handle, @"value") isKindOfClass:[NSString class]]
				   ? (NSString *)ct_get(handle, @"value") : @"";
		BOOL actEmerg = [ct_get(action, @"isEmergency") respondsToSelector:@selector(boolValue)]
				? [ct_get(action, @"isEmergency") boolValue] : NO;

		char numBuf[64];
		wipe_ram(numBuf, sizeof(numBuf));
		if (numStr.length > 0)
			snprintf(numBuf, sizeof(numBuf), "%s", [numStr UTF8String]);
		bool isEmerg = actEmerg || is_emergency_cstr(numBuf);

		clog("CallKit performStartCallAction uuid=%s num_len=%lu emergency=%d",
		     [[uuid UUIDString] UTF8String], (unsigned long)numStr.length, isEmerg ? 1 : 0);

		SweetTrackedCall *tc = [[SweetTrackedCall alloc] init];
		tc.uuid = uuid;
		tc.callId = 0;
		tc.lastState = QMI_VOICE_CALL_STATE_ORIGINATING;
		tc.outgoing = YES;
		[g_tracked_calls addObject:tc];

		uint8_t cid = 0;
		int rc = qmi_voice_dial(&g_qmi, numBuf, isEmerg, &cid);
		wipe_ram(numBuf, sizeof(numBuf));
		if (rc < 0) {
			clog("CallKit startCall qmi_voice_dial failed rc=%d", rc);
			[g_tracked_calls removeObjectIdenticalTo:tc];
			[action fail];
			return;
		}
		if (cid != 0 && tc.callId == 0)
			tc.callId = cid;

		struct cellular_state st = snapshot_state();
		struct qmi_voice_call mo_info;
		memset(&mo_info, 0, sizeof(mo_info));
		mo_info.present = true;
		mo_info.call_id = cid;
		mo_info.state = tc.lastState;
		mo_info.call_type = isEmerg ? 0x09u : 0x00u;
		mo_info.direction = 0x01u;
		id upd = build_cx_call_update(&mo_info, &st);
		if (upd && [provider respondsToSelector:@selector(reportCallWithUUID:updated:)])
			[provider reportCallWithUUID:uuid updated:upd];

		if (!tc.reportedConnecting &&
		    [provider respondsToSelector:@selector(reportOutgoingCallWithUUID:startedConnectingAtDate:)]) {
			tc.reportedConnecting = YES;
			[provider reportOutgoingCallWithUUID:uuid startedConnectingAtDate:[NSDate date]];
		}
		if (tc.lastState == QMI_VOICE_CALL_STATE_CONVERSATION && !tc.reportedConnected &&
		    [provider respondsToSelector:@selector(reportOutgoingCallWithUUID:connectedAtDate:)]) {
			tc.reportedConnected = YES;
			[provider reportOutgoingCallWithUUID:uuid connectedAtDate:[NSDate date]];
		}
		[action fulfill];
	} @catch (id exc) {
		clog("performStartCallAction exception: %s", [[exc description] UTF8String]);
		@try { [action fail]; } @catch (id e2) { (void)e2; }
	}
}

- (void)provider:(SweetCXProviderSPI *)provider performAnswerCallAction:(SweetCXActionSPI *)action
{
	(void)provider;
	@try {
		NSUUID *uuid = [action callUUID];
		SweetTrackedCall *tc = find_tracked_by_uuid(uuid);
		uint8_t cid = tc ? tc.callId : 0;
		int rc = qmi_voice_answer(&g_qmi, cid);
		clog("CallKit performAnswerCallAction uuid=%s call_id=%u -> rc=%d",
		     [[uuid UUIDString] UTF8String], cid, rc);
		if (rc == 0 || g_qmi.voice_call_count > 0)
			[action fulfill];
		else
			[action fail];
	} @catch (id exc) {
		clog("performAnswerCallAction exception: %s", [[exc description] UTF8String]);
		@try { [action fail]; } @catch (id e2) { (void)e2; }
	}
}

- (void)provider:(SweetCXProviderSPI *)provider performEndCallAction:(SweetCXActionSPI *)action
{
	(void)provider;
	@try {
		NSUUID *uuid = [action callUUID];
		SweetTrackedCall *tc = find_tracked_by_uuid(uuid);
		uint8_t cid = tc ? tc.callId : 0;
		int rc = (cid != 0) ? qmi_voice_end(&g_qmi, cid) : qmi_voice_manage(&g_qmi, 0x08u, 0);
		clog("CallKit performEndCallAction uuid=%s call_id=%u -> rc=%d",
		     [[uuid UUIDString] UTF8String], cid, rc);
		if (tc)
			[g_tracked_calls removeObjectIdenticalTo:tc];
		if (g_tracked_calls.count == 0)
			sweet_voice_write_shm(0, QMI_VOICE_CALL_STATE_END, 0);
		[action fulfill];
	} @catch (id exc) {
		clog("performEndCallAction exception: %s", [[exc description] UTF8String]);
		@try { [action fulfill]; } @catch (id e2) { (void)e2; }
	}
}

- (void)provider:(SweetCXProviderSPI *)provider performSetHeldCallAction:(SweetCXActionSPI *)action
{
	(void)provider;
	@try {
		NSUUID *uuid = [action callUUID];
		SweetTrackedCall *tc = find_tracked_by_uuid(uuid);
		uint8_t cid = tc ? tc.callId : 0;
		BOOL onHold = [ct_get(action, @"isOnHold") respondsToSelector:@selector(boolValue)]
			      ? [ct_get(action, @"isOnHold") boolValue] : NO;
		int rc = qmi_voice_manage(&g_qmi, 0x03u, cid);
		clog("CallKit performSetHeldCallAction call_id=%u hold=%d -> rc=%d", cid, onHold ? 1 : 0, rc);
		[action fulfill];
	} @catch (id exc) {
		clog("performSetHeldCallAction exception: %s", [[exc description] UTF8String]);
		@try { [action fulfill]; } @catch (id e2) { (void)e2; }
	}
}

- (void)provider:(SweetCXProviderSPI *)provider performSetMutedCallAction:(SweetCXActionSPI *)action
{
	(void)provider;
	@try {
		BOOL muted = [ct_get(action, @"isMuted") respondsToSelector:@selector(boolValue)]
			     ? [ct_get(action, @"isMuted") boolValue] : NO;
		clog("CallKit performSetMutedCallAction muted=%d", muted ? 1 : 0);
		[action fulfill];
	} @catch (id exc) {
		clog("performSetMutedCallAction exception: %s", [[exc description] UTF8String]);
		@try { [action fulfill]; } @catch (id e2) { (void)e2; }
	}
}

- (void)provider:(SweetCXProviderSPI *)provider performPlayDTMFCallAction:(SweetCXActionSPI *)action
{
	(void)provider;
	@try {
		NSUUID *uuid = [action callUUID];
		SweetTrackedCall *tc = find_tracked_by_uuid(uuid);
		uint8_t cid = tc ? tc.callId : 0;
		NSString *digits = [ct_get(action, @"digits") isKindOfClass:[NSString class]]
				   ? (NSString *)ct_get(action, @"digits") : @"";
		NSInteger dtype = [ct_get(action, @"type") respondsToSelector:@selector(integerValue)]
				  ? [ct_get(action, @"type") integerValue] : 1;
		clog("CallKit performPlayDTMFCallAction call_id=%u digits_len=%lu type=%ld",
		     cid, (unsigned long)digits.length, (long)dtype);
		for (NSUInteger i = 0; i < digits.length; i++) {
			unichar uc = [digits characterAtIndex:i];
			if (uc < 0x7f) {
				(void)qmi_voice_dtmf(&g_qmi, cid, (char)uc, true);
				usleep(100000);
				(void)qmi_voice_dtmf(&g_qmi, cid, (char)uc, false);
			}
		}
		[action fulfill];
	} @catch (id exc) {
		clog("performPlayDTMFCallAction exception: %s", [[exc description] UTF8String]);
		@try { [action fulfill]; } @catch (id e2) { (void)e2; }
	}
}

- (void)provider:(SweetCXProviderSPI *)provider performSendMMIOrUSSDCodeAction:(SweetCXActionSPI *)action
{
	(void)provider;
	@try {
		NSString *code = [ct_get(action, @"code") isKindOfClass:[NSString class]]
				 ? (NSString *)ct_get(action, @"code") : @"";
		clog("CallKit performSendMMIOrUSSDCodeAction code_len=%lu", (unsigned long)code.length);
		[action fail];
	} @catch (id exc) {
		(void)exc;
	}
}

- (void)provider:(SweetCXProviderSPI *)provider didActivateAudioSession:(id)audioSession
{
	(void)provider; (void)audioSession;
	clog("CallKit didActivateAudioSession");
}

- (void)provider:(SweetCXProviderSPI *)provider didDeactivateAudioSession:(id)audioSession
{
	(void)provider; (void)audioSession;
	clog("CallKit didDeactivateAudioSession");
}

@end

static id build_cx_call_update(const struct qmi_voice_call *c, const struct cellular_state *st)
{
	Class clsUpd = NSClassFromString(@"CXCallUpdate");
	Class clsHnd = NSClassFromString(@"CXHandle");
	if (!clsUpd)
		return nil;
	id upd = [[clsUpd alloc] init];
	if (c && c->number[0] && clsHnd) {
		NSString *numStr = [NSString stringWithUTF8String:c->number];
		if (numStr.length > 0) {
			id h = [(SweetCXHandleSPI *)[clsHnd alloc] initWithType:2 value:numStr];
			ct_set(upd, @"remoteHandle", h);
		}
	}
	/* Exact CommCenter TelephonyProvider baseband call properties (iOS 26.5.2) */
	ct_set(upd, @"localSenderIdentityUUID", slot1_uuid());
	ct_set(upd, @"localSenderIdentityAccountUUID", slot1_uuid());
	ct_set(upd, @"ISOCountryCode", iso_country_for_mcc(st ? st->mcc : NULL));
	ct_set(upd, @"audioCategory", @"AVAudioSessionCategoryPhoneCall");
	ct_set(upd, @"usingBaseband", @YES);
	ct_set(upd, @"callSubType", @1);
	ct_set(upd, @"hasVideo", @NO);
	ct_set(upd, @"emergency", @(c && c->call_type == 0x09));
	ct_set(upd, @"supportsHolding", @YES);
	ct_set(upd, @"supportsGrouping", @YES);
	ct_set(upd, @"supportsUngrouping", @YES);
	ct_set(upd, @"supportsDTMF", @YES);
	ct_set(upd, @"supportsDTMFUpdates", @YES);
	ct_set(upd, @"supportsAddCall", @YES);
	ct_set(upd, @"supportsSendingToVoicemail", @YES);
	ct_set(upd, @"supportsUnambiguousMultiPartyState", @YES);
	ct_set(upd, @"localizedCallerName", nil);
	return upd;
}

static void report_cx_call_ended_safe(NSUUID *uuid, NSInteger cxReason)
{
	if (!g_cx_provider || !uuid)
		return;
	@try {
		NSDate *now = [NSDate date];
		if ([g_cx_provider respondsToSelector:@selector(reportCallWithUUID:endedAtDate:reason:)]) {
			[g_cx_provider reportCallWithUUID:uuid endedAtDate:now reason:cxReason];
		} else if ([g_cx_provider respondsToSelector:@selector(reportCallWithUUID:endedAtDate:privateReason:)]) {
			[g_cx_provider reportCallWithUUID:uuid endedAtDate:now privateReason:cxReason];
		}
	} @catch (id exc) {
		clog("reportCallWithUUID ended exception: %s", [[exc description] UTF8String]);
	}
}

static void on_qmi_voice_calls_changed(struct qmi_ctx *ctx, const struct qmi_voice_call *calls,
				       size_t count, void *user)
{
	(void)ctx; (void)user;
	@try {
		if (!g_tracked_calls)
			g_tracked_calls = [NSMutableArray array];
		struct cellular_state st = snapshot_state();

		NSMutableSet<NSNumber *> *seenIds = [NSMutableSet set];
		for (size_t i = 0; i < count; i++) {
			const struct qmi_voice_call *c = &calls[i];
			if (!c->present || c->call_id == 0)
				continue;

			SweetTrackedCall *tc = find_tracked_by_id(c->call_id);
			if (!tc && c->direction == 0x01) {
				/* Bind pending CallKit/CLI MO call whose QMI call_id just arrived */
				for (SweetTrackedCall *cand in g_tracked_calls) {
					if (cand.callId == 0 && cand.outgoing) {
						cand.callId = c->call_id;
						tc = cand;
						break;
					}
				}
			}

			if (c->state == QMI_VOICE_CALL_STATE_END) {
				clog("voice call_id=%u -> END (end_reason=%u)", c->call_id, c->end_reason);
				if (tc) {
					/* CXCallEndedReasonRemoteEnded = 2, CXCallEndedReasonFailed = 1 */
					NSInteger cxReason = (tc.reportedConnected || c->end_reason == 0 ||
							      c->end_reason == 16 || c->end_reason == 27 ||
							      c->end_reason == 102 || c->end_reason == 143) ? 2 : 1;
					report_cx_call_ended_safe(tc.uuid, cxReason);
					[g_tracked_calls removeObjectIdenticalTo:tc];
				}
				continue;
			}

			[seenIds addObject:@(c->call_id)];

			if (!tc) {
				tc = [[SweetTrackedCall alloc] init];
				tc.uuid = [NSUUID UUID];
				tc.callId = c->call_id;
				tc.lastState = c->state;
				tc.outgoing = (c->direction == 0x01);
				[g_tracked_calls addObject:tc];

				clog("voice new call_id=%u dir=%s state=%s mode=%u num_len=%zu",
				     c->call_id, tc.outgoing ? "MO" : "MT",
				     qmi_voice_state_str(c->state), c->mode, strlen(c->number));

				if (!tc.outgoing &&
				    (c->state == QMI_VOICE_CALL_STATE_INCOMING ||
				     c->state == QMI_VOICE_CALL_STATE_WAITING ||
				     c->state == QMI_VOICE_CALL_STATE_SETUP)) {
					tc.reportedIncoming = YES;
					tc.reportedRemoteHandle = (c->number[0] != '\0');
					if (g_cx_provider) {
						id upd = build_cx_call_update(c, &st);
						NSUUID *u = tc.uuid;
						[g_cx_provider reportNewIncomingCallWithUUID:u
										      update:upd
										  completion:^(NSError *err) {
							if (err)
								clog("CallKit reportNewIncomingCall err: %s",
								     [[err description] UTF8String]);
							else
								clog("CallKit reportNewIncomingCall accepted uuid=%s",
								     [[u UUIDString] UTF8String]);
						}];
					}
				}
			} else if (tc.lastState != c->state) {
				clog("voice call_id=%u state %s -> %s (mode=%u, num_len=%zu)",
				     c->call_id, qmi_voice_state_str(tc.lastState),
				     qmi_voice_state_str(c->state), c->mode, strlen(c->number));
				tc.lastState = c->state;
			}

			if (!tc.outgoing && tc.reportedIncoming && !tc.reportedRemoteHandle && c->number[0]) {
				tc.reportedRemoteHandle = YES;
				if (g_cx_provider && [g_cx_provider respondsToSelector:@selector(reportCallWithUUID:updated:)]) {
					id upd = build_cx_call_update(c, &st);
					if (upd)
						[g_cx_provider reportCallWithUUID:tc.uuid updated:upd];
				}
			}

			if (tc.outgoing && g_cx_provider) {
				if (!tc.reportedConnecting &&
				    (c->state == QMI_VOICE_CALL_STATE_ORIGINATING ||
				     c->state == QMI_VOICE_CALL_STATE_ALERTING ||
				     c->state == QMI_VOICE_CALL_STATE_PRE_ALERTING) &&
				    [g_cx_provider respondsToSelector:@selector(reportOutgoingCallWithUUID:startedConnectingAtDate:)]) {
					tc.reportedConnecting = YES;
					[g_cx_provider reportOutgoingCallWithUUID:tc.uuid
							  startedConnectingAtDate:[NSDate date]];
				}
				if (!tc.reportedSentInvitation &&
				    (c->state == QMI_VOICE_CALL_STATE_ALERTING ||
				     c->state == QMI_VOICE_CALL_STATE_PRE_ALERTING) &&
				    [g_cx_provider respondsToSelector:@selector(reportOutgoingCallWithUUID:sentInvitationAtDate:)]) {
					tc.reportedSentInvitation = YES;
					[g_cx_provider reportOutgoingCallWithUUID:tc.uuid
							     sentInvitationAtDate:[NSDate date]];
				}
				if (!tc.reportedConnected && c->state == QMI_VOICE_CALL_STATE_CONVERSATION) {
					if (!tc.reportedConnecting &&
					    [g_cx_provider respondsToSelector:@selector(reportOutgoingCallWithUUID:startedConnectingAtDate:)]) {
						tc.reportedConnecting = YES;
						[g_cx_provider reportOutgoingCallWithUUID:tc.uuid
								  startedConnectingAtDate:[NSDate date]];
					}
					tc.reportedConnected = YES;
					if ([g_cx_provider respondsToSelector:@selector(reportOutgoingCallWithUUID:connectedAtDate:)]) {
						[g_cx_provider reportOutgoingCallWithUUID:tc.uuid
								  connectedAtDate:[NSDate date]];
					}
				}
			}
		}

		/* Reconcile any tracked calls that vanished from GET_ALL_CALL_INFO / ALL_CALL_STATUS_IND */
		for (NSInteger idx = (NSInteger)g_tracked_calls.count - 1; idx >= 0; idx--) {
			SweetTrackedCall *tc = g_tracked_calls[(NSUInteger)idx];
			if (tc.callId != 0 && ![seenIds containsObject:@(tc.callId)]) {
				clog("voice call_id=%u disappeared from active list -> reporting ended", tc.callId);
				report_cx_call_ended_safe(tc.uuid, tc.reportedConnected ? 2 : 1);
				[g_tracked_calls removeObjectAtIndex:(NSUInteger)idx];
			}
		}
	} @catch (id exc) {
		clog("on_qmi_voice_calls_changed exception: %s", [[exc description] UTF8String]);
	}
}

static SweetCXProviderConfigSPI *build_callkit_configuration(const struct cellular_state *st)
{
	Class clsCfg    = NSClassFromString(@"CXProviderConfiguration");
	Class clsAcct   = NSClassFromString(@"CXAccount");
	Class clsSender = NSClassFromString(@"CXSenderIdentity");
	Class clsHandle = NSClassFromString(@"CXHandle");
	if (!clsCfg)
		return nil;

	SweetCXProviderConfigSPI *cfg = [[clsCfg alloc] init];
	if ([cfg respondsToSelector:@selector(setLocalizedName:)])
		[cfg setLocalizedName:@"Phone"];
	if ([cfg respondsToSelector:@selector(setSupportedHandleTypes:)])
		[cfg setSupportedHandleTypes:[NSSet setWithObjects:@2, @1, nil]];
	if ([cfg respondsToSelector:@selector(setSupportsAudioOnly:)])
		[cfg setSupportsAudioOnly:YES];
	if ([cfg respondsToSelector:@selector(setSupportsVideo:)])
		[cfg setSupportsVideo:NO];
	if ([cfg respondsToSelector:@selector(setSupportsEmergency:)])
		[cfg setSupportsEmergency:YES];
	if ([cfg respondsToSelector:@selector(setSupportsVoicemail:)])
		[cfg setSupportsVoicemail:YES];
	if ([cfg respondsToSelector:@selector(setSupportsCurrentPlatform:)])
		[cfg setSupportsCurrentPlatform:YES];
	if ([cfg respondsToSelector:@selector(setSupportsRinging:)])
		[cfg setSupportsRinging:YES];
	if ([cfg respondsToSelector:@selector(setSupportsDynamicSystemUI:)])
		[cfg setSupportsDynamicSystemUI:YES];
	if ([cfg respondsToSelector:@selector(setSupportsAudioTranslation:)])
		[cfg setSupportsAudioTranslation:YES];
	if ([cfg respondsToSelector:@selector(setIncludesCallsInRecents:)])
		[cfg setIncludesCallsInRecents:YES];
	if ([cfg respondsToSelector:@selector(setMaximumCallGroups:)])
		[cfg setMaximumCallGroups:2];
	if ([cfg respondsToSelector:@selector(setMaximumCallsPerCallGroup:)])
		[cfg setMaximumCallsPerCallGroup:5];

	/* Match CommCenter TelephonyProvider: CXAccount + CXSenderIdentity -> prioritizedSenderIdentities */
	if (clsSender) {
		NSString *svcName = (st && st->operator_name[0])
				    ? [NSString stringWithUTF8String:st->operator_name] : @"Cellular";
		NSString *iso = iso_country_for_mcc(st ? st->mcc : NULL);
		id sender = nil;
		if (clsAcct &&
		    [clsAcct instancesRespondToSelector:
		     @selector(initWithUUID:description:serviceName:isoCountryCode:handle:shortLabel:)] &&
		    [clsSender instancesRespondToSelector:@selector(initWithUUID:account:)]) {
			id acct = [(SweetCXAccountSPI *)[clsAcct alloc]
				   initWithUUID:slot1_uuid()
				    description:@"Primary"
				    serviceName:svcName
				 isoCountryCode:iso
					 handle:nil
				     shortLabel:@"P"];
			if (acct)
				sender = [(SweetCXSenderIdentitySPI *)[clsSender alloc]
					  initWithUUID:slot1_uuid() account:acct];
		}
		if (!sender && clsHandle &&
		    [clsSender instancesRespondToSelector:@selector(initWithUUID:handle:localizedName:)]) {
			id senderH = [(SweetCXHandleSPI *)[clsHandle alloc] initWithType:1 value:@"Primary"];
			sender = [(SweetCXSenderIdentitySPI *)[clsSender alloc]
				  initWithUUID:slot1_uuid() handle:senderH localizedName:@"Primary"];
		}
		if (sender) {
			if ([cfg respondsToSelector:@selector(setPrioritizedSenderIdentities:)])
				[cfg setPrioritizedSenderIdentities:[NSOrderedSet orderedSetWithObject:sender]];
			else if ([cfg respondsToSelector:@selector(setSenderIdentities:)])
				[cfg setSenderIdentities:[NSSet setWithObject:sender]];
		}
	}

	if (clsHandle && [cfg respondsToSelector:@selector(setEmergencyHandles:)]) {
		static NSString * const emergNums[] = { @"112", @"911", @"100", @"101", @"102", @"108" };
		NSMutableArray *ehs = [NSMutableArray arrayWithCapacity:6];
		for (size_t i = 0; i < sizeof(emergNums) / sizeof(emergNums[0]); i++) {
			id eh = [(SweetCXHandleSPI *)[clsHandle alloc] initWithType:2 value:emergNums[i]];
			if (eh)
				[ehs addObject:eh];
		}
		[cfg setEmergencyHandles:ehs];
	}
	return cfg;
}

static void refresh_callkit_configuration(const struct cellular_state *st)
{
	if (!g_cx_provider || ![g_cx_provider respondsToSelector:@selector(setConfiguration:)])
		return;
	@try {
		SweetCXProviderConfigSPI *cfg = build_callkit_configuration(st);
		if (cfg)
			[g_cx_provider setConfiguration:cfg];
	} @catch (id exc) {
		clog("refresh_callkit_configuration exception: %s", [[exc description] UTF8String]);
	}
}

static void init_callkit_provider(void)
{
	g_tracked_calls = [NSMutableArray array];
	void *ck = dlopen("/System/Library/Frameworks/CallKit.framework/CallKit", RTLD_NOW);
	if (!ck) {
		clog("CallKit.framework dlopen failed: %s", dlerror());
		return;
	}
	Protocol *delProto = objc_getProtocol("CXProviderDelegate");
	if (delProto)
		(void)class_addProtocol([SweetCXProviderDelegate class], delProto);
	Protocol *privDelProto = objc_getProtocol("CXProviderDelegatePrivate");
	if (privDelProto)
		(void)class_addProtocol([SweetCXProviderDelegate class], privDelProto);

	Class clsXPCProv = NSClassFromString(@"CXXPCProvider");
	Class clsProv    = NSClassFromString(@"CXProvider");
	if (!clsXPCProv && !clsProv) {
		clog("CallKit provider classes unavailable");
		return;
	}

	@try {
		struct cellular_state st = snapshot_state();
		SweetCXProviderConfigSPI *cfg = build_callkit_configuration(&st);
		if (!cfg) {
			clog("failed to build CXProviderConfiguration");
			return;
		}
		Class chosenCls = clsXPCProv ? clsXPCProv : clsProv;
		SweetCXProviderSPI *prov = [(SweetCXProviderSPI *)[chosenCls alloc] initWithConfiguration:cfg];
		if (!prov && clsXPCProv && clsProv) {
			chosenCls = clsProv;
			prov = [(SweetCXProviderSPI *)[clsProv alloc] initWithConfiguration:cfg];
		}
		if (!prov) {
			clog("failed to instantiate CallKit provider");
			return;
		}
		g_cx_delegate = [[SweetCXProviderDelegate alloc] init];
		[prov setDelegate:g_cx_delegate queue:g_qmi_queue];
		g_cx_provider = prov;
		clog("initialized CallKit %s (prioritizedSender=%s, delegate on g_qmi_queue)",
		     class_getName(chosenCls), SLOT1_UUID_STR);
	} @catch (id exc) {
		clog("init_callkit_provider exception: %s", [[exc description] UTF8String]);
	}
}

static void handle_ctrl_client_on_qmi_queue(int cfd)
{
	char req[160];
	char rsp[4096];
	wipe_ram(req, sizeof(req));
	ssize_t nr = recv(cfd, req, sizeof(req) - 1, 0);
	if (nr <= 0) {
		wipe_ram(req, sizeof(req));
		close(cfd);
		return;
	}
	req[nr] = '\0';
	req[strcspn(req, "\r\n")] = '\0';

	int rlen = 0;
	if (!strncmp(req, "DIAL ", 5)) {
		int emerg = atoi(req + 5);
		const char *sp = strchr(req + 5, ' ');
		const char *num = sp ? sp + 1 : "";
		uint8_t cid = 0;
		int rc = qmi_voice_dial(&g_qmi, num, emerg != 0 || is_emergency_cstr(num), &cid);
		size_t nlen = strlen(num);
		wipe_ram(req, sizeof(req));
		clog("ctrl DIAL (len=%zu, emerg=%d) -> rc=%d call_id=%u", nlen, emerg, rc, cid);
		struct cellular_state cur = snapshot_state();
		if (rc == 0) {
			cur.active_calls = (uint32_t)g_qmi.voice_call_count;
			snprintf(cur.last_call_state, sizeof(cur.last_call_state), "originating");
			commit_and_publish(&cur, false);
		}
		rlen = snprintf(rsp, sizeof(rsp),
				"{\"ok\": %s, \"action\": \"dial\", \"call_id\": %u, \"rc\": %d}\n",
				rc == 0 ? "true" : "false", cid, rc);
	} else if (!strncmp(req, "ANSWER", 6)) {
		uint8_t cid = (req[6] == ' ') ? (uint8_t)atoi(req + 7) : 0;
		int rc = qmi_voice_answer(&g_qmi, cid);
		clog("ctrl ANSWER (call_id=%u) -> rc=%d", cid, rc);
		rlen = snprintf(rsp, sizeof(rsp),
				"{\"ok\": %s, \"action\": \"answer\", \"call_id\": %u, \"rc\": %d}\n",
				rc == 0 ? "true" : "false", cid, rc);
	} else if (!strncmp(req, "HANGUP", 6)) {
		uint8_t cid = (req[6] == ' ') ? (uint8_t)atoi(req + 7) : 0;
		int rc = qmi_voice_end(&g_qmi, cid);
		if (rc == 0 && cid == 0)
			sweet_voice_write_shm(0, QMI_VOICE_CALL_STATE_END, 0);
		clog("ctrl HANGUP (call_id=%u) -> rc=%d", cid, rc);
		rlen = snprintf(rsp, sizeof(rsp),
				"{\"ok\": %s, \"action\": \"hangup\", \"call_id\": %u, \"rc\": %d}\n",
				rc == 0 ? "true" : "false", cid, rc);
	} else if (!strcmp(req, "CALLS")) {
		struct cellular_state cur = snapshot_state();
		(void)qmi_voice_query_calls(&g_qmi, &cur);
		commit_and_publish(&cur, false);
		char items[2048];
		size_t io = 0;
		items[0] = '\0';
		for (size_t i = 0; i < g_qmi.voice_call_count; i++) {
			const struct qmi_voice_call *c = &g_qmi.voice_calls[i];
			if (!c->present)
				continue;
			int w = snprintf(items + io, sizeof(items) - io,
					 "%s{\"call_id\": %u, \"state\": \"%s\", \"direction\": \"%s\", "
					 "\"mode\": %u, \"call_type\": %u, \"is_mpty\": %s, "
					 "\"number_present\": %s, \"number_len\": %zu}",
					 io ? ", " : "",
					 c->call_id,
					 qmi_voice_state_str(c->state),
					 c->direction == 1 ? "mo" : (c->direction == 2 ? "mt" : "unknown"),
					 c->mode,
					 c->call_type,
					 c->is_mpty ? "true" : "false",
					 c->number[0] ? "true" : "false",
					 strlen(c->number));
			if (w < 0 || (size_t)w >= sizeof(items) - io)
				break;
			io += (size_t)w;
		}
		rlen = snprintf(rsp, sizeof(rsp),
			"{\n"
			"  \"ok\": true,\n"
			"  \"voice_svc_ready\": %s,\n"
			"  \"active_calls\": %u,\n"
			"  \"last_call_state\": \"%s\",\n"
			"  \"last_call_end_reason\": %u,\n"
			"  \"calls\": [%s]\n"
			"}\n",
			cur.voice_svc_ready ? "true" : "false",
			cur.active_calls,
			cur.last_call_state[0] ? cur.last_call_state : "idle",
			cur.last_call_end_reason,
			items);
	} else {
		rlen = snprintf(rsp, sizeof(rsp), "{\"ok\": false, \"error\": \"unknown_cmd\"}\n");
	}
	wipe_ram(req, sizeof(req));
	if (rlen > 0)
		(void)send(cfd, rsp, (size_t)rlen, 0);
	close(cfd);
}

static void start_ctrl_socket(void)
{
	(void)unlink(SWEET_CELLULARD_CTRL_SOCK);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return;
	struct sockaddr_un sun;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	snprintf(sun.sun_path, sizeof(sun.sun_path), "%s", SWEET_CELLULARD_CTRL_SOCK);
#ifdef __APPLE__
	sun.sun_len = (uint8_t)sizeof(sun);
#endif
	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0 || listen(fd, 4) < 0) {
		close(fd);
		return;
	}
	(void)chmod(SWEET_CELLULARD_CTRL_SOCK, 0600);
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0)
		(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
	g_ctrl_listen_fd = fd;
	g_ctrl_sock_src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, g_qmi_queue);
	dispatch_source_set_event_handler(g_ctrl_sock_src, ^{
		for (;;) {
			int cfd = accept(g_ctrl_listen_fd, NULL, NULL);
			if (cfd < 0)
				break;
#ifdef SO_NOSIGPIPE
			int one = 1;
			(void)setsockopt(cfd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
			struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
			(void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			handle_ctrl_client_on_qmi_queue(cfd);
		}
	});
	dispatch_resume(g_ctrl_sock_src);
	clog("listening on %s (0600)", SWEET_CELLULARD_CTRL_SOCK);
}

static void commit_and_publish(const struct cellular_state *next, bool force_broadcast)
{
	os_unfair_lock_lock(&g_state_lock);
	bool prev_sim = g_state.sim_present;
	char prev_sim_st[32], prev_op[64], prev_mcc[8], prev_reg[32], prev_rat[16], prev_call_st[24];
	int prev_bars = g_state.bars_0_to_4;
	bool prev_ps = g_state.ps_attached;
	bool prev_data = g_state.data_connected;
	uint8_t prev_dms = g_state.dms_mode_raw;
	uint16_t prev_rej = g_state.last_reject_cause;
	uint32_t prev_calls = g_state.active_calls;
	snprintf(prev_sim_st, sizeof(prev_sim_st), "%s", g_state.sim_state);
	snprintf(prev_op, sizeof(prev_op), "%s", g_state.operator_name);
	snprintf(prev_mcc, sizeof(prev_mcc), "%s", g_state.mcc);
	snprintf(prev_reg, sizeof(prev_reg), "%s", g_state.reg_state);
	snprintf(prev_rat, sizeof(prev_rat), "%s", g_state.rat);
	snprintf(prev_call_st, sizeof(prev_call_st), "%s", g_state.last_call_state);
	g_state = *next;
	os_unfair_lock_unlock(&g_state_lock);

	(void)cellular_state_write_json_file(next, CELLULARD_JSON);

	bool sim_changed = force_broadcast || (prev_sim != next->sim_present) ||
			   (strcmp(prev_sim_st, next->sim_state) != 0);
	bool op_or_mcc_changed = (strcmp(prev_op, next->operator_name) != 0) ||
				 (strcmp(prev_mcc, next->mcc) != 0);
	bool any_changed = sim_changed ||
			   (prev_dms != next->dms_mode_raw) ||
			   (prev_rej != next->last_reject_cause) ||
			   (prev_calls != next->active_calls) ||
			   (strcmp(prev_call_st, next->last_call_state) != 0) ||
			   (prev_bars != next->bars_0_to_4) ||
			   (prev_ps != next->ps_attached) ||
			   (prev_data != next->data_connected) ||
			   op_or_mcc_changed ||
			   (strcmp(prev_reg, next->reg_state) != 0) ||
			   (strcmp(prev_rat, next->rat) != 0);

	if (any_changed) {
		clog("state: imei=%d sim=%s(slot %d,%s) mode=%s(%u,off=0x%04x,hw=%d) reg=%s(rej=%u) rat=%s plmn=%s-%s op='%s' rsrp=%d bars=%d/4 voice=%d(cs=%d,ltev=%d,imsv=%d,calls=%u,%s) ps=%d data=%d",
		     next->imei_present ? 1 : 0,
		     next->sim_present ? "yes" : "no",
		     next->sim_slot, next->app_state, next->radio_mode, next->dms_mode_raw,
		     next->dms_offline_reason, next->dms_hw_restricted ? 1 : 0,
		     next->reg_state, next->last_reject_cause, next->rat, next->mcc, next->mnc,
		     next->operator_name, next->has_rsrp ? next->rsrp_dbm : 0,
		     next->bars_0_to_4, next->voice_svc_ready ? 1 : 0,
		     next->cs_attached ? 1 : 0, next->lte_voice_support ? 1 : 0, next->ims_voice_support ? 1 : 0,
		     next->active_calls, next->last_call_state,
		     next->ps_attached ? 1 : 0,
		     next->data_connected ? 1 : 0);
		if (sim_changed || op_or_mcc_changed)
			refresh_callkit_configuration(next);
		publish_system_status(next);
		broadcast_ct_notifications(next, sim_changed);
	}
}

static void on_qmi_indication(struct qmi_ctx *ctx, uint32_t service, uint16_t msg_id,
			      const uint8_t *tlvs, size_t tlv_len, void *user)
{
	(void)user;
	struct cellular_state cur = snapshot_state();
	if (qmi_apply_indication(ctx, &cur, service, msg_id, tlvs, tlv_len)) {
		commit_and_publish(&cur, service == QMI_SVC_UIM);
	}
}

static void teardown_qrtr_source(void)
{
	g_last_online_try_ms = 0;
	g_online_attempts = 0;
	g_last_logged_dms_mode = 0xfe;
	if (g_qrtr_read_src) {
		dispatch_source_cancel(g_qrtr_read_src);
		g_qrtr_read_src = nil;
	} else {
		qmi_close(&g_qmi);
	}
}

static void ensure_dms_online_if_needed(struct cellular_state *cur)
{
	if (!qmi_has_service(&g_qmi, QMI_SVC_DMS))
		return;
	if (cur->dms_mode_raw == 0) {
		g_online_attempts = 0;
		g_last_logged_dms_mode = 0;
		return;
	}
	if (cur->dms_mode_raw == 0xff)
		return;

	if (g_last_logged_dms_mode != cur->dms_mode_raw) {
		g_last_logged_dms_mode = cur->dms_mode_raw;
		clog("DMS non-online: mode='%s' (%u) offline_reason=0x%04x(valid=%d) hw_restricted=%d",
		     cur->radio_mode, cur->dms_mode_raw,
		     cur->dms_offline_reason, cur->has_dms_offline_reason ? 1 : 0,
		     cur->dms_hw_restricted ? 1 : 0);
	}

	if (g_airplane_mode || g_online_attempts >= 6)
		return;
	uint64_t now_ms = cell_mono_ms();
	if (g_last_online_try_ms != 0 && (now_ms - g_last_online_try_ms) < 10000ull)
		return;

	g_online_attempts++;
	g_last_online_try_ms = now_ms;
	uint16_t qerr = 0;
	int mrc = qmi_dms_set_operating_mode_ex(&g_qmi, 0x00, &qerr);
	clog("QMI_DMS_SET_OPERATING_MODE(ONLINE) attempt %d/6 from '%s'(%u, off_reason=0x%04x, hw=%d) -> rc=%d qmi_err=0x%04x",
	     g_online_attempts, cur->radio_mode, cur->dms_mode_raw,
	     cur->dms_offline_reason, cur->dms_hw_restricted ? 1 : 0, mrc, qerr);
	if (mrc != 0 && cur->dms_mode_raw == 6) {
		/* persistent_low_power sometimes requires transition via LOW_POWER (1) first */
		uint16_t lperr = 0;
		int lprc = qmi_dms_set_operating_mode_ex(&g_qmi, 0x01, &lperr);
		if (lprc == 0) {
			usleep(100000);
			mrc = qmi_dms_set_operating_mode_ex(&g_qmi, 0x00, &qerr);
			clog("QMI_DMS_SET_OPERATING_MODE(LP->ONLINE) retry -> rc=%d qmi_err=0x%04x", mrc, qerr);
		}
	}
	if (mrc == 0) {
		usleep(200000);
		(void)qmi_poll_all(&g_qmi, cur, !cur->modem_rev[0]);
		if (cur->dms_mode_raw == 0)
			g_online_attempts = 0;
	}
}

static void try_connect_and_sync(void)
{
	bool just_connected = false;
	if (g_qmi.fd < 0) {
		g_qmi.ind_cb = on_qmi_indication;
		g_qmi.on_voice_calls_changed = on_qmi_voice_calls_changed;
		if (qmi_open(&g_qmi, SWEET_QRTR_SOCK) < 0)
			return;
		just_connected = true;
		g_last_online_try_ms = 0;
		g_online_attempts = 0;
		g_last_logged_dms_mode = 0xfe;
		clog("connected to %s (fd %d)", SWEET_QRTR_SOCK, g_qmi.fd);

		int fd_copy = g_qmi.fd;
		dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
							       (uintptr_t)fd_copy, 0, g_qmi_queue);
		dispatch_source_set_event_handler(src, ^{
			uint32_t before_srv = core_srv_mask(&g_qmi);
			if (qmi_drain_rx(&g_qmi, 0) < 0) {
				clog("QRTR socket disconnected; will retry");
				teardown_qrtr_source();
				struct cellular_state cur = snapshot_state();
				cur.qrtr_connected = false;
				commit_and_publish(&cur, false);
				return;
			}
			uint32_t after_srv = core_srv_mask(&g_qmi);
			if ((after_srv & ~before_srv) != 0) {
				clog("late QMI services arrived (mask 0x%x -> 0x%x); syncing immediately",
				     before_srv, after_srv);
				try_connect_and_sync();
			}
		});
		dispatch_source_set_cancel_handler(src, ^{
			qmi_close(&g_qmi);
		});
		g_qrtr_read_src = src;
		dispatch_resume(src);

		(void)qmi_sync_servers(&g_qmi, 600);
	}

	/* Dynamically register indications as DMS/UIM/NAS/WDS become available */
	int new_ind = qmi_register_indications(&g_qmi);
	if (new_ind > 0)
		clog("registered QMI indications: +0x%x (mask=0x%x)", new_ind, g_qmi.ind_reg_mask);

	g_poll_ticks++;
	struct cellular_state cur = snapshot_state();
	bool need_full = just_connected || ((g_poll_ticks % 3) == 0) ||
			 !cur.sim_present || (cur.dms_mode_raw != 0) ||
			 !cur.modem_rev[0] || !qmi_has_service(&g_qmi, QMI_SVC_NAS);
	if (need_full) {
		(void)qmi_poll_all(&g_qmi, &cur, just_connected || !cur.modem_rev[0]);
	} else {
		(void)qmi_poll_fast(&g_qmi, &cur);
	}

	/* Ensure DMS operating mode reaches ONLINE when AirplaneMode is off */
	ensure_dms_online_if_needed(&cur);

	/* Re-run indication registration in case new services appeared during poll */
	new_ind = qmi_register_indications(&g_qmi);
	if (new_ind > 0)
		clog("registered QMI indications: +0x%x (mask=0x%x)", new_ind, g_qmi.ind_reg_mask);

	/* Stage 7B optional packet data session bring-up when gated on */
	if (g_enable_wds_data && g_cellular_data_enabled && !g_airplane_mode &&
	    cur.dms_mode_raw == 0 && cur.sim_present && cur.reg_state_raw == 1 &&
	    cur.ps_attached && !cur.data_connected) {
		uint64_t now_ms = cell_mono_ms();
		if (g_last_wds_try_ms == 0 || (now_ms - g_last_wds_try_ms) >= 15000ull) {
			g_last_wds_try_ms = now_ms;
			int wrc = qmi_wds_test_session(&g_qmi, &cur, g_configured_apn, true);
			clog("WDS auto-session bring-up (cfg_apn='%s') -> rc=%d apn='%s' ip='%s' gw='%s' dns='%s' mtu=%u end=%u/%u",
			     g_configured_apn, wrc, cur.data_apn, cur.data_ip,
			     cur.data_gateway, cur.data_dns1, cur.data_mtu,
			     cur.wds_end_reason_type, cur.wds_end_reason);
		}
	}

	commit_and_publish(&cur, just_connected);
	if (just_connected && fn_notify_post)
		fn_notify_post("kCTDaemonReadyLocalNotification");
}

static void trigger_qmi_radio_power(bool online)
{
	dispatch_async(g_qmi_queue, ^{
		g_online_attempts = 0;
		g_last_online_try_ms = 0;
		g_last_logged_dms_mode = 0xfe;
		if (g_qmi.fd >= 0 && qmi_has_service(&g_qmi, QMI_SVC_DMS)) {
			uint16_t qerr = 0;
			int mrc = qmi_dms_set_operating_mode_ex(&g_qmi, online ? 0x00 : 0x01, &qerr);
			clog("AirplaneMode radio power -> %s: rc=%d qmi_err=0x%04x",
			     online ? "ONLINE" : "LOW_POWER", mrc, qerr);
			struct cellular_state cur = snapshot_state();
			(void)qmi_poll_all(&g_qmi, &cur, !cur.modem_rev[0]);
			commit_and_publish(&cur, false);
		}
	});
}

/* ---- Entry Point ---- */

static SweetXPCListenerSPI *g_ct_listener;
static SweetCellularListenerDelegate *g_ct_delegate;

int main(int argc, char **argv)
{
	@autoreleasepool {
		bool foreground = false;
		for (int i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "--enable-data"))
				g_enable_wds_data = true;
			else if (!strcmp(argv[i], "--no-status-publisher"))
				g_publish_system_status = false;
			else if (!strcmp(argv[i], "--foreground"))
				foreground = true;
			else if (!strcmp(argv[i], "--apn") && i + 1 < argc)
				snprintf(g_configured_apn, sizeof(g_configured_apn), "%s", argv[++i]);
		}

		signal(SIGPIPE, SIG_IGN);
		(void)setpriority(PRIO_DARWIN_PROCESS, 0, 0);

		if (!foreground) {
			(void)mkdir("/var/local", 0755);
			struct timeval bt = { 0 };
			size_t blen = sizeof(bt);
			char want[32] = "", stamp[32] = "";
			if (sysctlbyname("kern.boottime", &bt, &blen, NULL, 0) == 0)
				snprintf(want, sizeof(want), "%ld\n", (long)bt.tv_sec);
			FILE *sf = fopen(CELLULARD_STAMP, "r");
			if (sf) {
				if (!fgets(stamp, sizeof(stamp), sf))
					stamp[0] = '\0';
				fclose(sf);
			}
			/* Rotate to .prev only once per phone boot so intra-boot restarts keep history */
			if (!want[0] || strcmp(stamp, want) != 0) {
				(void)rename(CELLULARD_LOG, CELLULARD_LOG ".prev");
				if (want[0] && (sf = fopen(CELLULARD_STAMP, "w")) != NULL) {
					fputs(want, sf);
					fclose(sf);
				}
			}
			g_logf = fopen(CELLULARD_LOG, "a");
		}
		clog("cellulard starting (pid %d)", (int)getpid());
		(void)dlopen("/System/Library/Frameworks/CoreTelephony.framework/CoreTelephony", RTLD_NOW);
		(void)[CTTelephonyNetworkInfo class];

		resolve_runtime_symbols();
		cellular_state_init(&g_state);
		g_qmi.fd = -1;
		g_xpc_clients = [NSMutableArray array];
		g_logged_unhandled = [NSMutableSet set];

		load_config();
		g_qmi.rat_mode_pref = g_rat_mode_pref;
		(void)cellular_state_write_json_file(&g_state, CELLULARD_JSON);

		/* 1. Initialize SystemStatus publisher */
		init_system_status_publisher();
		publish_system_status(&g_state);

		/* 2. Start com.apple.commcenter.coretelephony.xpc NSXPCListener */
		Class listenerCls = NSClassFromString(@"NSXPCListener");
		if (listenerCls) {
			g_ct_delegate = [[SweetCellularListenerDelegate alloc] init];
			g_ct_listener = [(SweetXPCListenerSPI *)[listenerCls alloc]
					 initWithMachServiceName:@"com.apple.commcenter.coretelephony.xpc"];
			g_ct_listener.delegate = g_ct_delegate;
			[g_ct_listener resume];
			clog("listening on com.apple.commcenter.coretelephony.xpc");
		} else {
			clog("ERROR: NSXPCListener class not found");
		}

		/* 3. Start com.apple.commcenter.xpc legacy CTServerConnection listener */
		dispatch_queue_t legacy_q = dispatch_queue_create("org.iosonpixel.cellulard.legacyxpc",
								  DISPATCH_QUEUE_SERIAL);
		start_legacy_commcenter_xpc(legacy_q);

		/* 4. Start QMI/QRTR queue, CallKit CXXPCProvider, CLI control socket & poll loop */
		g_qmi_queue = dispatch_queue_create("org.iosonpixel.cellulard.qmi", DISPATCH_QUEUE_SERIAL);
		g_qmi.on_voice_calls_changed = on_qmi_voice_calls_changed;
		sweet_voice_write_shm(0, 0, 0);
		init_callkit_provider();
		start_ctrl_socket();
		dispatch_async(g_qmi_queue, ^{
			try_connect_and_sync();
		});

		g_poll_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, g_qmi_queue);
		dispatch_source_set_timer(g_poll_timer,
					  dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
					  5 * NSEC_PER_SEC,
					  500 * NSEC_PER_MSEC);
		dispatch_source_set_event_handler(g_poll_timer, ^{
			try_connect_and_sync();
		});
		dispatch_resume(g_poll_timer);

		/* Watch AirplaneMode Darwin notification so radio tracks Control Center toggle! */
		int (*fn_notify_reg)(const char *, int *, dispatch_queue_t, void (^)(int)) =
			dlsym(RTLD_DEFAULT, "notify_register_dispatch");
		if (fn_notify_reg) {
			int tok = 0;
			fn_notify_reg("com.apple.radios.prefs", &tok, g_qmi_queue, ^(int t) {
				(void)t;
				bool prev = g_airplane_mode;
				load_config();
				if (prev != g_airplane_mode) {
					clog("AirplaneMode changed -> %d", g_airplane_mode ? 1 : 0);
					trigger_qmi_radio_power(!g_airplane_mode);
				}
			});
		}

		dispatch_main();
	}
	return 0;
}
