/*
 * HotStore 2.0 — App Store Discovery, Multi-Repo Native IPA Installer & WebApp Studio
 * for iOS 26.5.2 on RMX1931 / SWEET.
 *
 * Talks to local root daemon hotstored v2 (http://127.0.0.1:29170) to:
 *   1. Search & browse live Apple App Store Top Charts (itunes.apple.com public API).
 *   2. Cross-match bundleIds against AltStore / SideStore / Feather / GitHub unencrypted
 *      (cryptid=0) .ipa repositories for 1-tap native arm64/arm64e installation.
 *   3. Package commercial FairPlay-only services into standalone native Home Screen
 *      WKWebView .app bundles (HotWebAppStub) with official 512px App Store icons.
 *   4. Manage installed apps, per-app Adreno 640 HotMetal2 GPU / CPU toggles,
 *      Wi-Fi browser .ipa uploads (port 29172), and libhot_appfix.dylib logs.
 */

#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <sys/stat.h>

static NSString *const kDaemonBaseURL = @"http://127.0.0.1:29170";
static NSString *const kSourcesDefaultsKey = @"SSRepoSources_v2";
static NSString *const kLocalIpaStageDir = @"/private/var/local/ipa";

static NSNotificationName const SSStateDidChangeNotification = @"SSStateDidChangeNotification";
static NSNotificationName const SSReposDidChangeNotification = @"SSReposDidChangeNotification";

/* ---- Helpers ---- */

static NSString *SSString(id obj)
{
	if ([obj isKindOfClass:[NSString class]]) return (NSString *)obj;
	if ([obj respondsToSelector:@selector(stringValue)]) return [obj stringValue];
	return @"";
}

static NSString *SSFormatBytes(long long bytes)
{
	if (bytes <= 0) return @"";
	double mb = (double)bytes / (1024.0 * 1024.0);
	if (mb >= 1024.0) return [NSString stringWithFormat:@"%.2f GB", mb / 1024.0];
	if (mb >= 1.0) return [NSString stringWithFormat:@"%.1f MB", mb];
	return [NSString stringWithFormat:@"%.0f KB", (double)bytes / 1024.0];
}

static NSString *SSCurrentIOSVersion(void)
{
	NSString *v = [[UIDevice currentDevice] systemVersion];
	return v.length > 0 ? v : @"26.5.2";
}

static BOOL SSIsOSVersionCompatible(NSString *minOS)
{
	if (minOS.length == 0) return YES;
	NSArray<NSString *> *a = [minOS componentsSeparatedByString:@"."];
	NSArray<NSString *> *b = [SSCurrentIOSVersion() componentsSeparatedByString:@"."];
	NSUInteger count = MAX(a.count, b.count);
	for (NSUInteger i = 0; i < count; i++) {
		NSInteger va = (i < a.count) ? [a[i] integerValue] : 0;
		NSInteger vb = (i < b.count) ? [b[i] integerValue] : 0;
		if (va < vb) return YES;
		if (va > vb) return NO;
	}
	return YES;
}

/* Curated optimal WebApp endpoints & User-Agent profiles for popular FairPlay-locked commercial apps */
static NSDictionary<NSString *, NSDictionary *> *SSCuratedWebAppMap(void)
{
	static NSDictionary *map;
	static dispatch_once_t onceToken;
	dispatch_once(&onceToken, ^{
		map = @{
			@"net.whatsapp.whatsapp":          @{ @"url": @"https://web.whatsapp.com",          @"ua": @"desktop" },
			@"com.google.ios.youtube":         @{ @"url": @"https://m.youtube.com",             @"ua": @"mobile"  },
			@"com.burbn.instagram":            @{ @"url": @"https://www.instagram.com",         @"ua": @"mobile"  },
			@"com.atebits.tweetie2":           @{ @"url": @"https://x.com",                     @"ua": @"mobile"  },
			@"com.spotify.client":             @{ @"url": @"https://open.spotify.com",          @"ua": @"desktop" },
			@"com.hammerandchisel.discord":    @{ @"url": @"https://discord.com/app",           @"ua": @"desktop" },
			@"ph.telegra.telegraph":           @{ @"url": @"https://web.telegram.org/k/",       @"ua": @"mobile"  },
			@"com.openai.chat":                @{ @"url": @"https://chatgpt.com",               @"ua": @"mobile"  },
			@"com.anthropic.claude":           @{ @"url": @"https://claude.ai",                 @"ua": @"mobile"  },
			@"ai.perplexity.app":              @{ @"url": @"https://www.perplexity.ai",         @"ua": @"mobile"  },
			@"com.reddit.reddit":              @{ @"url": @"https://www.reddit.com",            @"ua": @"mobile"  },
			@"com.facebook.facebook":          @{ @"url": @"https://m.facebook.com",            @"ua": @"mobile"  },
			@"com.facebook.messenger":         @{ @"url": @"https://www.messenger.com",         @"ua": @"desktop" },
			@"com.burbn.barcelona":            @{ @"url": @"https://www.threads.net",           @"ua": @"mobile"  },
			@"com.toyopagroup.picaboo":        @{ @"url": @"https://web.snapchat.com",          @"ua": @"desktop" },
			@"com.zhiliaoapp.musically":       @{ @"url": @"https://www.tiktok.com",            @"ua": @"mobile"  },
			@"com.linkedin.linkedin":          @{ @"url": @"https://www.linkedin.com",          @"ua": @"mobile"  },
			@"pinterest":                      @{ @"url": @"https://www.pinterest.com",         @"ua": @"mobile"  },
			@"tv.twitch":                      @{ @"url": @"https://m.twitch.tv",               @"ua": @"mobile"  },
			@"com.netflix.netflix":            @{ @"url": @"https://www.netflix.com",           @"ua": @"desktop" },
			@"com.amazon.aiv.aivapp":          @{ @"url": @"https://www.primevideo.com",        @"ua": @"desktop" },
			@"in.startv.hotstar":              @{ @"url": @"https://www.hotstar.com/in",        @"ua": @"mobile"  },
			@"com.amazon.amazonin":            @{ @"url": @"https://www.amazon.in",             @"ua": @"mobile"  },
			@"com.amazon.amazon":              @{ @"url": @"https://www.amazon.com",            @"ua": @"mobile"  },
			@"com.flipkart.iphone":            @{ @"url": @"https://www.flipkart.com",          @"ua": @"mobile"  },
			@"com.ubercab.uberclient":         @{ @"url": @"https://m.uber.com",                @"ua": @"mobile"  },
			@"in.swiggy.swiggyapp":            @{ @"url": @"https://www.swiggy.com",            @"ua": @"mobile"  },
			@"com.zomato.zomato":              @{ @"url": @"https://www.zomato.com",            @"ua": @"mobile"  },
			@"com.google.gmail":               @{ @"url": @"https://mail.google.com",           @"ua": @"mobile"  },
			@"com.google.maps":                @{ @"url": @"https://maps.google.com",           @"ua": @"mobile"  },
			@"com.google.drive":               @{ @"url": @"https://drive.google.com",          @"ua": @"mobile"  },
			@"com.github.stormbreaker.prod":   @{ @"url": @"https://github.com",                @"ua": @"mobile"  },
			@"notion.id":                      @{ @"url": @"https://www.notion.so",             @"ua": @"mobile"  },
		};
	});
	return map;
}

static NSDictionary *SSResolveWebAppSpec(NSString *bundleId, NSString *name, NSString *sellerURL)
{
	NSDictionary *curated = SSCuratedWebAppMap()[bundleId.lowercaseString ?: @""];
	if (curated) return curated;

	NSString *lowName = name.lowercaseString ?: @"";
	if ([lowName hasPrefix:@"whatsapp"])  return @{ @"url": @"https://web.whatsapp.com",    @"ua": @"desktop" };
	if ([lowName hasPrefix:@"youtube"])   return @{ @"url": @"https://m.youtube.com",       @"ua": @"mobile"  };
	if ([lowName hasPrefix:@"instagram"]) return @{ @"url": @"https://www.instagram.com",   @"ua": @"mobile"  };
	if ([lowName hasPrefix:@"spotify"])   return @{ @"url": @"https://open.spotify.com",    @"ua": @"desktop" };
	if ([lowName hasPrefix:@"discord"])   return @{ @"url": @"https://discord.com/app",     @"ua": @"desktop" };
	if ([lowName hasPrefix:@"telegram"])  return @{ @"url": @"https://web.telegram.org/k/", @"ua": @"mobile"  };
	if ([lowName hasPrefix:@"chatgpt"])   return @{ @"url": @"https://chatgpt.com",         @"ua": @"mobile"  };

	if (sellerURL.length > 8 && ([sellerURL.lowercaseString hasPrefix:@"http://"] || [sellerURL.lowercaseString hasPrefix:@"https://"])) {
		return @{ @"url": sellerURL, @"ua": @"mobile" };
	}
	NSString *q = [name stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLQueryAllowedCharacterSet]] ?: @"app";
	return @{ @"url": [NSString stringWithFormat:@"https://duckduckgo.com/?q=%%5C%@", q], @"ua": @"mobile" };
}

/* ---- Models ---- */

@interface SSAppVersion : NSObject
@property (nonatomic, copy) NSString *version;
@property (nonatomic, copy) NSString *downloadURL;
@property (nonatomic, copy) NSString *minOSVersion;
@property (nonatomic, copy) NSString *date;
@property (nonatomic, copy) NSString *notes;
@property (nonatomic, assign) long long sizeBytes;
@end

@implementation SSAppVersion
@end

@interface SSRepoApp : NSObject
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *bundleId;
@property (nonatomic, copy) NSString *developer;
@property (nonatomic, copy) NSString *subtitle;
@property (nonatomic, copy) NSString *appDescription;
@property (nonatomic, copy) NSString *iconURL;
@property (nonatomic, copy) NSString *sourceName;
@property (nonatomic, copy) NSString *sourceURL;
@property (nonatomic, strong) SSAppVersion *bestVersion;
@property (nonatomic, strong) NSArray<SSAppVersion *> *versions;
@end

@implementation SSRepoApp
@end

@interface SSStoreCatalogItem : NSObject
@property (nonatomic, copy) NSString *trackId;
@property (nonatomic, copy) NSString *name;
@property (nonatomic, copy) NSString *bundleId;
@property (nonatomic, copy) NSString *developer;
@property (nonatomic, copy) NSString *genre;
@property (nonatomic, copy) NSString *summary;
@property (nonatomic, copy) NSString *version;
@property (nonatomic, copy) NSString *iconURL;
@property (nonatomic, copy) NSString *sellerURL;
@property (nonatomic, assign) double rating;
@property (nonatomic, assign) long long sizeBytes;
@property (nonatomic, strong) NSArray<NSString *> *screenshotURLs;
@end

@implementation SSStoreCatalogItem
@end

@interface SSRepoSourceInfo : NSObject
@property (nonatomic, copy) NSString *url;
@property (nonatomic, copy) NSString *name;
@property (nonatomic, assign) NSInteger appCount;
@property (nonatomic, copy) NSString *errorText;
@property (nonatomic, assign) BOOL loading;
@end

@implementation SSRepoSourceInfo
@end

/* ---- Icon & Image Loader ---- */

@interface SSIconLoader : NSObject
@property (nonatomic, strong) NSCache<NSString *, UIImage *> *cache;
@property (nonatomic, strong) NSMutableDictionary<NSString *, NSMutableArray<void (^)(UIImage *)> *> *pending;
@property (nonatomic, strong) NSURLSession *session;
+ (instancetype)shared;
- (UIImage *)placeholderIcon;
- (void)loadIconURL:(NSString *)urlStr completion:(void (^)(UIImage *img))completion;
- (void)loadRawImageURL:(NSString *)urlStr completion:(void (^)(UIImage *img))completion;
@end

@implementation SSIconLoader

+ (instancetype)shared
{
	static SSIconLoader *inst;
	static dispatch_once_t onceToken;
	dispatch_once(&onceToken, ^{
		inst = [[SSIconLoader alloc] init];
	});
	return inst;
}

- (instancetype)init
{
	if ((self = [super init])) {
		_cache = [[NSCache alloc] init];
		_cache.countLimit = 500;
		_pending = [NSMutableDictionary dictionary];
		NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration defaultSessionConfiguration];
		cfg.timeoutIntervalForRequest = 15.0;
		cfg.HTTPMaximumConnectionsPerHost = 6;
		_session = [NSURLSession sessionWithConfiguration:cfg];
	}
	return self;
}

- (UIImage *)placeholderIcon
{
	static UIImage *ph;
	static dispatch_once_t onceToken;
	dispatch_once(&onceToken, ^{
		UIGraphicsBeginImageContextWithOptions(CGSizeMake(56, 56), NO, 0.0);
		[[UIColor secondarySystemFillColor] setFill];
		[[UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, 56, 56) cornerRadius:13] fill];
		UIImage *sym = [UIImage systemImageNamed:@"app.fill"];
		if (sym) {
			UIImage *tinted = [sym imageWithTintColor:[UIColor tertiaryLabelColor] renderingMode:UIImageRenderingModeAlwaysOriginal];
			[tinted drawInRect:CGRectMake(14, 14, 28, 28)];
		}
		ph = UIGraphicsGetImageFromCurrentImageContext();
		UIGraphicsEndImageContext();
	});
	return ph;
}

- (void)loadIconURL:(NSString *)urlStr completion:(void (^)(UIImage *img))completion
{
	if (urlStr.length == 0) {
		completion([self placeholderIcon]);
		return;
	}
	UIImage *cached = [self.cache objectForKey:urlStr];
	if (cached) {
		completion(cached);
		return;
	}
	NSURL *url = [NSURL URLWithString:urlStr];
	if (!url) {
		completion([self placeholderIcon]);
		return;
	}
	@synchronized (self.pending) {
		NSMutableArray *waiters = self.pending[urlStr];
		if (waiters) {
			[waiters addObject:[completion copy]];
			return;
		}
		self.pending[urlStr] = [NSMutableArray arrayWithObject:[completion copy]];
	}
	NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
	[req setValue:@"HotStore/2.0 (iOS 26.5.2)" forHTTPHeaderField:@"User-Agent"];
	[[self.session dataTaskWithRequest:req completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
		(void)resp; (void)err;
		UIImage *decoded = nil;
		if (data.length > 0) {
			UIImage *raw = [UIImage imageWithData:data];
			if (raw && raw.size.width > 0 && raw.size.height > 0) {
				UIGraphicsBeginImageContextWithOptions(CGSizeMake(56, 56), NO, 2.0);
				[raw drawInRect:CGRectMake(0, 0, 56, 56)];
				decoded = UIGraphicsGetImageFromCurrentImageContext();
				UIGraphicsEndImageContext();
			}
		}
		if (decoded) {
			[self.cache setObject:decoded forKey:urlStr];
		}
		UIImage *finalImg = decoded ?: [self placeholderIcon];
		NSArray *callbacks = nil;
		@synchronized (self.pending) {
			callbacks = [self.pending[urlStr] copy];
			[self.pending removeObjectForKey:urlStr];
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			for (void (^cb)(UIImage *) in callbacks) {
				cb(finalImg);
			}
		});
	}] resume];
}

- (void)loadRawImageURL:(NSString *)urlStr completion:(void (^)(UIImage *img))completion
{
	if (urlStr.length == 0) { completion(nil); return; }
	NSString *key = [@"raw|" stringByAppendingString:urlStr];
	UIImage *cached = [self.cache objectForKey:key];
	if (cached) { completion(cached); return; }
	NSURL *url = [NSURL URLWithString:urlStr];
	if (!url) { completion(nil); return; }
	[[self.session dataTaskWithURL:url completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
		(void)resp; (void)err;
		UIImage *img = data.length > 0 ? [UIImage imageWithData:data] : nil;
		if (img) [self.cache setObject:img forKey:key];
		dispatch_async(dispatch_get_main_queue(), ^{
			completion(img);
		});
	}] resume];
}

@end

/* ---- Store Controller ---- */

@interface SSStoreController : NSObject <NSURLSessionDownloadDelegate>
@property (nonatomic, assign) BOOL daemonOnline;
@property (nonatomic, assign) BOOL daemonBusy;
@property (nonatomic, strong) NSDictionary *daemonJob;
@property (nonatomic, assign) BOOL lanEnabled;
@property (nonatomic, copy) NSString *lanURL;
@property (nonatomic, copy) NSString *wifiIP;
@property (nonatomic, strong) NSArray<NSDictionary *> *installedApps;
@property (nonatomic, strong) NSDictionary<NSString *, NSDictionary *> *installedByBundleId;
@property (nonatomic, strong) NSDictionary<NSString *, NSDictionary *> *installedByOrigBundleId;
@property (nonatomic, strong) NSArray<NSDictionary *> *localIpas;

@property (nonatomic, assign) BOOL downloading;
@property (nonatomic, copy) NSString *downloadTitle;
@property (nonatomic, copy) NSString *downloadError;
@property (nonatomic, assign) double downloadProgress;
@property (nonatomic, assign) long long downloadedBytes;
@property (nonatomic, assign) long long expectedBytes;
@property (nonatomic, strong) NSURLSessionDownloadTask *activeDownloadTask;
@property (nonatomic, strong) NSURLSession *downloadSession;
@property (nonatomic, strong) NSURLSession *apiSession;
@property (nonatomic, strong) NSTimer *pollTimer;

@property (nonatomic, strong) NSMutableArray<NSString *> *sourceURLs;
@property (nonatomic, strong) NSArray<SSRepoSourceInfo *> *sourceInfos;
@property (nonatomic, strong) NSArray<SSRepoApp *> *allApps;
@property (nonatomic, strong) NSDictionary<NSString *, SSRepoApp *> *repoAppByBundleId;
@property (nonatomic, assign) BOOL loadingRepos;

@property (nonatomic, weak) UITabBarController *rootTabs;

+ (instancetype)shared;
- (void)startPolling;
- (void)refreshStatusNow;
- (void)reloadAllRepos;
- (NSArray<NSString *> *)defaultSourceURLs;
- (void)addSourceURL:(NSString *)urlStr;
- (void)removeSourceAtIndex:(NSUInteger)idx;
- (void)resetDefaultSources;

- (BOOL)isBusy;
- (NSDictionary *)installedEntryForBundleId:(NSString *)bundleId;
- (SSRepoApp *)matchedRepoAppForBundleId:(NSString *)bundleId name:(NSString *)name;

- (void)downloadAndInstallURL:(NSString *)urlStr title:(NSString *)title fromVC:(UIViewController *)vc;
- (void)cancelActiveDownload;
- (void)installLocalFile:(NSString *)path deleteAfter:(BOOL)delAfter title:(NSString *)title;
- (void)importExternalFileURL:(NSURL *)url switchTab:(BOOL)switchTab;
- (void)createWebAppWithName:(NSString *)name url:(NSString *)urlStr userAgent:(NSString *)ua iconURL:(NSString *)iconURL origBundleId:(NSString *)origBid switchTab:(BOOL)switchTab;
- (void)setGPUMode:(BOOL)enabled forExecName:(NSString *)execName completion:(void (^)(BOOL ok))completion;
- (void)setLanUploadEnabled:(BOOL)enabled completion:(void (^)(BOOL ok))completion;
- (void)fetchAppFixLogWithCompletion:(void (^)(NSString *log))completion;
- (void)clearAppFixLogWithCompletion:(void (^)(BOOL ok))completion;
- (void)openBundleId:(NSString *)bundleId;
- (void)uninstallDirName:(NSString *)dirName deleteData:(BOOL)delData completion:(void (^)(BOOL ok, NSString *err))completion;
- (void)rebuildIconsWithCompletion:(void (^)(BOOL ok))completion;
@end

@implementation SSStoreController

+ (instancetype)shared
{
	static SSStoreController *inst;
	static dispatch_once_t onceToken;
	dispatch_once(&onceToken, ^{
		inst = [[SSStoreController alloc] init];
	});
	return inst;
}

- (NSArray<NSString *> *)defaultSourceURLs
{
	return @[
		@"https://community-apps.sidestore.io/sidecommunity.json",
		@"https://cdn.altstore.io/file/altstore/apps.json",
		@"https://raw.githubusercontent.com/swaggyP36000/TrollStore-IPAs/main/apps_esign.json",
		@"https://wuxu1.github.io/wuxu-complete-plus.json",
		@"https://ipa.cypwn.xyz/cypwn.json",
	];
}

- (instancetype)init
{
	if ((self = [super init])) {
		_installedApps = @[];
		_installedByBundleId = @{};
		_installedByOrigBundleId = @{};
		_localIpas = @[];
		_allApps = @[];
		_repoAppByBundleId = @{};
		_sourceInfos = @[];

		NSURLSessionConfiguration *apiCfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		apiCfg.timeoutIntervalForRequest = 6.0;
		_apiSession = [NSURLSession sessionWithConfiguration:apiCfg];

		NSURLSessionConfiguration *dlCfg = [NSURLSessionConfiguration defaultSessionConfiguration];
		dlCfg.timeoutIntervalForRequest = 60.0;
		dlCfg.timeoutIntervalForResource = 3600.0;
		_downloadSession = [NSURLSession sessionWithConfiguration:dlCfg delegate:self delegateQueue:[NSOperationQueue mainQueue]];

		NSArray *saved = [[NSUserDefaults standardUserDefaults] stringArrayForKey:kSourcesDefaultsKey];
		if (saved.count > 0) {
			_sourceURLs = [saved mutableCopy];
		} else {
			_sourceURLs = [[self defaultSourceURLs] mutableCopy];
			[[NSUserDefaults standardUserDefaults] setObject:_sourceURLs forKey:kSourcesDefaultsKey];
		}
	}
	return self;
}

- (BOOL)isBusy
{
	return self.downloading || self.daemonBusy;
}

- (NSDictionary *)installedEntryForBundleId:(NSString *)bundleId
{
	if (bundleId.length == 0) return nil;
	NSString *low = bundleId.lowercaseString;
	return self.installedByBundleId[low] ?: self.installedByOrigBundleId[low];
}

- (SSRepoApp *)matchedRepoAppForBundleId:(NSString *)bundleId name:(NSString *)name
{
	if (bundleId.length > 0) {
		SSRepoApp *m = self.repoAppByBundleId[bundleId.lowercaseString];
		if (m) return m;
	}
	if (name.length > 2) {
		NSString *ln = name.lowercaseString;
		for (SSRepoApp *a in self.allApps) {
			if ([a.name.lowercaseString isEqualToString:ln]) return a;
		}
	}
	return nil;
}

- (void)startPolling
{
	if (!self.pollTimer) {
		self.pollTimer = [NSTimer scheduledTimerWithTimeInterval:1.0
		                                                  target:self
		                                                selector:@selector(refreshStatusNow)
		                                                userInfo:nil
		                                                 repeats:YES];
	}
	[self refreshStatusNow];
}

- (void)refreshStatusNow
{
	NSURL *u = [NSURL URLWithString:[kDaemonBaseURL stringByAppendingString:@"/status"]];
	[[self.apiSession dataTaskWithURL:u completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
		(void)resp;
		if (err || !data) {
			dispatch_async(dispatch_get_main_queue(), ^{
				if (self.daemonOnline) {
					self.daemonOnline = NO;
					[[NSNotificationCenter defaultCenter] postNotificationName:SSStateDidChangeNotification object:self];
				}
			});
			return;
		}
		NSDictionary *json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
		if (![json isKindOfClass:[NSDictionary class]]) return;

		NSArray *inst = [json[@"installed"] isKindOfClass:[NSArray class]] ? json[@"installed"] : @[];
		NSMutableDictionary *byBid = [NSMutableDictionary dictionaryWithCapacity:inst.count];
		NSMutableDictionary *byOrig = [NSMutableDictionary dictionaryWithCapacity:inst.count];
		for (NSDictionary *item in inst) {
			NSString *bid = SSString(item[@"bundleId"]).lowercaseString;
			NSString *obid = SSString(item[@"origBundleId"]).lowercaseString;
			if (bid.length > 0) byBid[bid] = item;
			if (obid.length > 0) byOrig[obid] = item;
		}
		NSArray *locals = [json[@"localIpas"] isKindOfClass:[NSArray class]] ? json[@"localIpas"] : @[];
		NSDictionary *job = [json[@"job"] isKindOfClass:[NSDictionary class]] ? json[@"job"] : @{};
		BOOL busy = [json[@"busy"] respondsToSelector:@selector(boolValue)] ? [json[@"busy"] boolValue] : NO;
		BOOL lanOn = [json[@"lanEnabled"] respondsToSelector:@selector(boolValue)] ? [json[@"lanEnabled"] boolValue] : NO;
		NSString *lanU = SSString(json[@"lanURL"]);
		NSString *wIP = SSString(json[@"wifiIP"]);

		dispatch_async(dispatch_get_main_queue(), ^{
			self.daemonOnline = YES;
			self.daemonBusy = busy;
			self.daemonJob = job;
			self.lanEnabled = lanOn;
			self.lanURL = lanU;
			self.wifiIP = wIP;
			self.installedApps = inst;
			self.installedByBundleId = byBid;
			self.installedByOrigBundleId = byOrig;
			self.localIpas = locals;
			[[NSNotificationCenter defaultCenter] postNotificationName:SSStateDidChangeNotification object:self];
		});
	}] resume];
}

- (void)postDaemonPath:(NSString *)path payload:(NSDictionary *)payload completion:(void (^)(NSDictionary *resp, NSError *err))completion
{
	NSURL *u = [NSURL URLWithString:[kDaemonBaseURL stringByAppendingString:path]];
	NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:u];
	req.HTTPMethod = @"POST";
	req.timeoutInterval = 15.0;
	[req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
	req.HTTPBody = [NSJSONSerialization dataWithJSONObject:(payload ?: @{}) options:0 error:nil];
	[[self.apiSession dataTaskWithRequest:req completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
		(void)resp;
		NSDictionary *parsed = nil;
		if (data.length > 0) {
			id obj = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
			if ([obj isKindOfClass:[NSDictionary class]]) parsed = obj;
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			if (completion) completion(parsed, err);
			[self refreshStatusNow];
		});
	}] resume];
}

/* ---- Download, Install & WebApp Creation ---- */

- (void)downloadAndInstallURL:(NSString *)urlStr title:(NSString *)title fromVC:(UIViewController *)vc
{
	NSString *trimmed = [urlStr stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
	NSURL *url = [NSURL URLWithString:trimmed];
	if (!url || (![url.scheme.lowercaseString isEqualToString:@"http"] && ![url.scheme.lowercaseString isEqualToString:@"https"])) {
		UIAlertController *ac = [UIAlertController alertControllerWithTitle:@"Invalid URL"
		                                                            message:@"Please enter a valid http:// or https:// .ipa link."
		                                                     preferredStyle:UIAlertControllerStyleAlert];
		[ac addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
		[vc presentViewController:ac animated:YES completion:nil];
		return;
	}
	if ([self isBusy]) {
		UIAlertController *ac = [UIAlertController alertControllerWithTitle:@"Installer Busy"
		                                                            message:@"An app download or installation is already running. Check the Install tab."
		                                                     preferredStyle:UIAlertControllerStyleAlert];
		[ac addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
		[vc presentViewController:ac animated:YES completion:nil];
		return;
	}

	self.downloading = YES;
	self.downloadTitle = title.length > 0 ? title : (url.lastPathComponent.length > 0 ? url.lastPathComponent : @"App.ipa");
	self.downloadError = nil;
	self.downloadProgress = 0.0;
	self.downloadedBytes = 0;
	self.expectedBytes = 0;

	NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
	[req setValue:@"AltStore/2.0 HotStore/2.0 (iPhone; iOS 26.5.2)" forHTTPHeaderField:@"User-Agent"];
	self.activeDownloadTask = [self.downloadSession downloadTaskWithRequest:req];
	[self.activeDownloadTask resume];

	if (self.rootTabs && self.rootTabs.selectedIndex != 2) {
		self.rootTabs.selectedIndex = 2;
	}
	[[NSNotificationCenter defaultCenter] postNotificationName:SSStateDidChangeNotification object:self];
}

- (void)cancelActiveDownload
{
	if (self.activeDownloadTask) {
		[self.activeDownloadTask cancel];
		self.activeDownloadTask = nil;
	}
	self.downloading = NO;
	self.downloadError = @"Download cancelled";
	[[NSNotificationCenter defaultCenter] postNotificationName:SSStateDidChangeNotification object:self];
}

- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)downloadTask
      didWriteData:(int64_t)bytesWritten
 totalBytesWritten:(int64_t)totalBytesWritten
totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite
{
	(void)session; (void)downloadTask; (void)bytesWritten;
	self.downloadedBytes = totalBytesWritten;
	self.expectedBytes = totalBytesExpectedToWrite;
	self.downloadProgress = totalBytesExpectedToWrite > 0 ? ((double)totalBytesWritten / (double)totalBytesExpectedToWrite) : 0.0;
	[[NSNotificationCenter defaultCenter] postNotificationName:SSStateDidChangeNotification object:self];
}

- (void)URLSession:(NSURLSession *)session downloadTask:(NSURLSessionDownloadTask *)downloadTask
didFinishDownloadingToURL:(NSURL *)location
{
	(void)session;
	NSHTTPURLResponse *http = [downloadTask.response isKindOfClass:[NSHTTPURLResponse class]]
	                        ? (NSHTTPURLResponse *)downloadTask.response : nil;
	if (http && (http.statusCode < 200 || http.statusCode >= 300)) {
		self.downloading = NO;
		self.activeDownloadTask = nil;
		self.downloadError = [NSString stringWithFormat:@"Server returned HTTP %ld", (long)http.statusCode];
		[[NSNotificationCenter defaultCenter] postNotificationName:SSStateDidChangeNotification object:self];
		return;
	}

	NSFileManager *fm = [NSFileManager defaultManager];
	[fm createDirectoryAtPath:kLocalIpaStageDir withIntermediateDirectories:YES attributes:nil error:nil];
	NSString *dest = [NSString stringWithFormat:@"%@/.hotdl-%llu.ipa",
	                  kLocalIpaStageDir, (unsigned long long)([[NSDate date] timeIntervalSince1970] * 1000.0)];
	[fm removeItemAtPath:dest error:nil];

	NSError *mvErr = nil;
	if (![fm moveItemAtPath:location.path toPath:dest error:&mvErr]) {
		if (![fm copyItemAtPath:location.path toPath:dest error:&mvErr]) {
			self.downloading = NO;
			self.activeDownloadTask = nil;
			self.downloadError = [NSString stringWithFormat:@"Could not stage IPA: %@", mvErr.localizedDescription ?: @"I/O error"];
			[[NSNotificationCenter defaultCenter] postNotificationName:SSStateDidChangeNotification object:self];
			return;
		}
	}
	chmod(dest.UTF8String, 0644);

	NSString *title = self.downloadTitle ?: @"App";
	self.downloading = NO;
	self.activeDownloadTask = nil;
	[self installLocalFile:dest deleteAfter:YES title:title];
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error
{
	(void)session; (void)task;
	if (!error || error.code == NSURLErrorCancelled) return;
	self.downloading = NO;
	self.activeDownloadTask = nil;
	self.downloadError = error.localizedDescription ?: @"Download failed";
	[[NSNotificationCenter defaultCenter] postNotificationName:SSStateDidChangeNotification object:self];
}

- (void)installLocalFile:(NSString *)path deleteAfter:(BOOL)delAfter title:(NSString *)title
{
	self.downloadError = nil;
	[self postDaemonPath:@"/install-file" payload:@{
		@"path": path ?: @"",
		@"deleteAfter": @(delAfter),
		@"displayName": title ?: path.lastPathComponent ?: @"App",
	} completion:^(NSDictionary *resp, NSError *err) {
		if (err || ![resp[@"ok"] boolValue]) {
			self.downloadError = SSString(resp[@"error"]).length > 0
			                   ? SSString(resp[@"error"])
			                   : (err.localizedDescription ?: @"Cannot reach hotstored daemon");
			[[NSNotificationCenter defaultCenter] postNotificationName:SSStateDidChangeNotification object:self];
		}
	}];
}

- (void)importExternalFileURL:(NSURL *)url switchTab:(BOOL)switchTab
{
	if (!url) return;
	BOOL scoped = [url startAccessingSecurityScopedResource];
	NSFileManager *fm = [NSFileManager defaultManager];
	[fm createDirectoryAtPath:kLocalIpaStageDir withIntermediateDirectories:YES attributes:nil error:nil];
	NSString *baseName = url.lastPathComponent.length > 0 ? url.lastPathComponent : @"Imported.ipa";
	NSString *dest = [NSString stringWithFormat:@"%@/.hotdl-%llu-%@",
	                  kLocalIpaStageDir, (unsigned long long)([[NSDate date] timeIntervalSince1970] * 1000.0), baseName];
	[fm removeItemAtPath:dest error:nil];
	NSError *cpErr = nil;
	BOOL ok = [fm copyItemAtPath:url.path toPath:dest error:&cpErr];
	if (scoped) [url stopAccessingSecurityScopedResource];

	if (!ok) {
		self.downloadError = [NSString stringWithFormat:@"Import failed: %@", cpErr.localizedDescription ?: @"Cannot copy file"];
		[[NSNotificationCenter defaultCenter] postNotificationName:SSStateDidChangeNotification object:self];
		return;
	}
	chmod(dest.UTF8String, 0644);
	if (switchTab && self.rootTabs) {
		self.rootTabs.selectedIndex = 2;
	}
	[self installLocalFile:dest deleteAfter:YES title:[baseName stringByDeletingPathExtension]];
}

- (void)createWebAppWithName:(NSString *)name url:(NSString *)urlStr userAgent:(NSString *)ua iconURL:(NSString *)iconURL origBundleId:(NSString *)origBid switchTab:(BOOL)switchTab
{
	self.downloadError = nil;
	if (switchTab && self.rootTabs) {
		self.rootTabs.selectedIndex = 2;
	}
	[self postDaemonPath:@"/create-webapp" payload:@{
		@"name": name ?: @"Web App",
		@"url": urlStr ?: @"",
		@"userAgent": ua ?: @"mobile",
		@"iconURL": iconURL ?: @"",
		@"origBundleId": origBid ?: @"",
	} completion:^(NSDictionary *resp, NSError *err) {
		if (err || ![resp[@"ok"] boolValue]) {
			self.downloadError = SSString(resp[@"error"]).length > 0
			                   ? SSString(resp[@"error"])
			                   : (err.localizedDescription ?: @"Failed creating Web App");
			[[NSNotificationCenter defaultCenter] postNotificationName:SSStateDidChangeNotification object:self];
		}
	}];
}

- (void)setGPUMode:(BOOL)enabled forExecName:(NSString *)execName completion:(void (^)(BOOL ok))completion
{
	[self postDaemonPath:@"/gpu-mode" payload:@{
		@"execName": execName ?: @"",
		@"gpuEnabled": @(enabled),
	} completion:^(NSDictionary *resp, NSError *err) {
		if (completion) completion(!err && [resp[@"ok"] boolValue]);
	}];
}

- (void)setLanUploadEnabled:(BOOL)enabled completion:(void (^)(BOOL ok))completion
{
	[self postDaemonPath:@"/lan-upload" payload:@{ @"enabled": @(enabled) } completion:^(NSDictionary *resp, NSError *err) {
		if (completion) completion(!err && [resp[@"ok"] boolValue]);
	}];
}

- (void)fetchAppFixLogWithCompletion:(void (^)(NSString *log))completion
{
	NSURL *u = [NSURL URLWithString:[kDaemonBaseURL stringByAppendingString:@"/appfix-log"]];
	[[self.apiSession dataTaskWithURL:u completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
		(void)resp;
		NSString *log = @"";
		if (!err && data.length > 0) {
			NSDictionary *j = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
			if ([j isKindOfClass:[NSDictionary class]]) log = SSString(j[@"log"]);
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			if (completion) completion(log);
		});
	}] resume];
}

- (void)clearAppFixLogWithCompletion:(void (^)(BOOL ok))completion
{
	[self postDaemonPath:@"/clear-appfix-log" payload:@{} completion:^(NSDictionary *resp, NSError *err) {
		if (completion) completion(!err && [resp[@"ok"] boolValue]);
	}];
}

- (void)openBundleId:(NSString *)bundleId
{
	if (bundleId.length == 0) return;
	[self postDaemonPath:@"/open" payload:@{ @"bundleId": bundleId } completion:nil];
}

- (void)uninstallDirName:(NSString *)dirName deleteData:(BOOL)delData completion:(void (^)(BOOL ok, NSString *err))completion
{
	[self postDaemonPath:@"/uninstall" payload:@{
		@"dirName": dirName ?: @"",
		@"deleteData": @(delData),
	} completion:^(NSDictionary *resp, NSError *err) {
		BOOL ok = (!err && [resp[@"ok"] boolValue]);
		NSString *msg = ok ? nil : (SSString(resp[@"error"]).length > 0 ? SSString(resp[@"error"]) : err.localizedDescription);
		if (completion) completion(ok, msg);
	}];
}

- (void)rebuildIconsWithCompletion:(void (^)(BOOL ok))completion
{
	[self postDaemonPath:@"/rebuild-icons" payload:@{} completion:^(NSDictionary *resp, NSError *err) {
		if (completion) completion(!err && [resp[@"ok"] boolValue]);
	}];
}

/* ---- Repo Sources Management & Parsing ---- */

- (void)saveSources
{
	[[NSUserDefaults standardUserDefaults] setObject:self.sourceURLs forKey:kSourcesDefaultsKey];
}

- (void)addSourceURL:(NSString *)urlStr
{
	NSString *trimmed = [urlStr stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
	if (trimmed.length == 0) return;
	if (![self.sourceURLs containsObject:trimmed]) {
		[self.sourceURLs addObject:trimmed];
		[self saveSources];
		[self reloadAllRepos];
	}
}

- (void)removeSourceAtIndex:(NSUInteger)idx
{
	if (idx >= self.sourceURLs.count) return;
	[self.sourceURLs removeObjectAtIndex:idx];
	[self saveSources];
	[self reloadAllRepos];
}

- (void)resetDefaultSources
{
	self.sourceURLs = [[self defaultSourceURLs] mutableCopy];
	[self saveSources];
	[self reloadAllRepos];
}

- (NSArray<SSRepoApp *> *)parseRepoJSON:(id)root sourceURL:(NSString *)srcURL sourceNameOut:(NSString **)nameOut
{
	NSString *repoName = [NSURL URLWithString:srcURL].host ?: @"Repository";
	NSArray *rawApps = nil;
	if ([root isKindOfClass:[NSDictionary class]]) {
		NSDictionary *dict = (NSDictionary *)root;
		if (SSString(dict[@"name"]).length > 0) repoName = SSString(dict[@"name"]);
		if ([dict[@"apps"] isKindOfClass:[NSArray class]]) rawApps = dict[@"apps"];
	} else if ([root isKindOfClass:[NSArray class]]) {
		rawApps = (NSArray *)root;
	}
	if (nameOut) *nameOut = repoName;
	if (!rawApps) return @[];

	NSMutableArray<SSRepoApp *> *parsed = [NSMutableArray arrayWithCapacity:rawApps.count];
	for (id item in rawApps) {
		if (![item isKindOfClass:[NSDictionary class]]) continue;
		NSDictionary *d = (NSDictionary *)item;
		NSString *name = SSString(d[@"name"]);
		if (name.length == 0) continue;

		NSString *bid = SSString(d[@"bundleIdentifier"]);
		if (bid.length == 0) bid = SSString(d[@"bundleID"]);
		if (bid.length == 0) bid = SSString(d[@"id"]);

		NSMutableArray<SSAppVersion *> *vers = [NSMutableArray array];
		if ([d[@"versions"] isKindOfClass:[NSArray class]]) {
			for (id vObj in d[@"versions"]) {
				if (![vObj isKindOfClass:[NSDictionary class]]) continue;
				NSDictionary *vd = (NSDictionary *)vObj;
				NSString *dl = SSString(vd[@"downloadURL"]);
				if (![dl.lowercaseString hasPrefix:@"http"]) continue;
				SSAppVersion *v = [[SSAppVersion alloc] init];
				v.version = SSString(vd[@"version"]).length > 0 ? SSString(vd[@"version"]) : @"1.0";
				v.downloadURL = dl;
				v.minOSVersion = SSString(vd[@"minOSVersion"]);
				v.date = SSString(vd[@"date"]);
				v.notes = SSString(vd[@"localizedDescription"]);
				v.sizeBytes = [vd[@"size"] respondsToSelector:@selector(longLongValue)] ? [vd[@"size"] longLongValue] : 0;
				[vers addObject:v];
			}
		}

		NSString *topDL = SSString(d[@"downloadURL"]);
		if ([topDL.lowercaseString hasPrefix:@"http"]) {
			BOOL dup = NO;
			for (SSAppVersion *ev in vers) {
				if ([ev.downloadURL isEqualToString:topDL]) { dup = YES; break; }
			}
			if (!dup) {
				SSAppVersion *tv = [[SSAppVersion alloc] init];
				tv.version = SSString(d[@"version"]).length > 0 ? SSString(d[@"version"]) : @"1.0";
				tv.downloadURL = topDL;
				tv.minOSVersion = SSString(d[@"minOSVersion"]);
				tv.date = SSString(d[@"versionDate"]);
				tv.notes = SSString(d[@"versionDescription"]);
				tv.sizeBytes = [d[@"size"] respondsToSelector:@selector(longLongValue)] ? [d[@"size"] longLongValue] : 0;
				[vers insertObject:tv atIndex:0];
			}
		}
		if (vers.count == 0) continue;

		SSAppVersion *best = nil;
		for (SSAppVersion *cand in vers) {
			if (SSIsOSVersionCompatible(cand.minOSVersion)) {
				best = cand;
				break;
			}
		}
		if (!best) best = vers.firstObject;

		SSRepoApp *app = [[SSRepoApp alloc] init];
		app.name = name;
		app.bundleId = bid;
		app.developer = SSString(d[@"developerName"]);
		app.subtitle = SSString(d[@"subtitle"]);
		app.appDescription = SSString(d[@"localizedDescription"]).length > 0
		                   ? SSString(d[@"localizedDescription"])
		                   : SSString(d[@"description"]);
		app.iconURL = SSString(d[@"iconURL"]);
		app.sourceName = repoName;
		app.sourceURL = srcURL;
		app.bestVersion = best;
		app.versions = vers;
		[parsed addObject:app];
	}
	return parsed;
}

- (void)reloadAllRepos
{
	NSArray<NSString *> *urls = [self.sourceURLs copy];
	if (urls.count == 0) {
		self.sourceInfos = @[];
		self.allApps = @[];
		self.repoAppByBundleId = @{};
		self.loadingRepos = NO;
		[[NSNotificationCenter defaultCenter] postNotificationName:SSReposDidChangeNotification object:self];
		return;
	}

	self.loadingRepos = YES;
	NSMutableArray<SSRepoSourceInfo *> *infos = [NSMutableArray arrayWithCapacity:urls.count];
	for (NSString *u in urls) {
		SSRepoSourceInfo *info = [[SSRepoSourceInfo alloc] init];
		info.url = u;
		info.name = [NSURL URLWithString:u].host ?: u;
		info.loading = YES;
		[infos addObject:info];
	}
	self.sourceInfos = infos;
	[[NSNotificationCenter defaultCenter] postNotificationName:SSReposDidChangeNotification object:self];

	NSURLSessionConfiguration *cfg = [NSURLSessionConfiguration ephemeralSessionConfiguration];
	cfg.timeoutIntervalForRequest = 20.0;
	NSURLSession *repoSess = [NSURLSession sessionWithConfiguration:cfg];

	NSMutableDictionary<NSString *, NSArray<SSRepoApp *> *> *perSource = [NSMutableDictionary dictionary];
	dispatch_group_t grp = dispatch_group_create();

	for (SSRepoSourceInfo *info in infos) {
		NSURL *nsu = [NSURL URLWithString:info.url];
		if (!nsu) {
			info.loading = NO;
			info.errorText = @"Invalid URL";
			continue;
		}
		dispatch_group_enter(grp);
		NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:nsu];
		[req setValue:@"AltStore/2.0 HotStore/2.0" forHTTPHeaderField:@"User-Agent"];
		[[repoSess dataTaskWithRequest:req completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
			(void)resp;
			if (err || data.length == 0) {
				dispatch_async(dispatch_get_main_queue(), ^{
					info.loading = NO;
					info.errorText = err.localizedDescription ?: @"Empty response";
					dispatch_group_leave(grp);
				});
				return;
			}
			NSError *jErr = nil;
			id root = [NSJSONSerialization JSONObjectWithData:data options:0 error:&jErr];
			NSString *rName = nil;
			NSArray<SSRepoApp *> *apps = root ? [self parseRepoJSON:root sourceURL:info.url sourceNameOut:&rName] : @[];
			dispatch_async(dispatch_get_main_queue(), ^{
				info.loading = NO;
				if (!root) {
					info.errorText = jErr.localizedDescription ?: @"Invalid JSON";
				} else {
					if (rName.length > 0) info.name = rName;
					info.appCount = (NSInteger)apps.count;
					info.errorText = nil;
					perSource[info.url] = apps;
				}
				dispatch_group_leave(grp);
			});
		}] resume];
	}

	dispatch_group_notify(grp, dispatch_get_main_queue(), ^{
		[repoSess finishTasksAndInvalidate];
		NSMutableArray<SSRepoApp *> *merged = [NSMutableArray array];
		NSMutableDictionary<NSString *, SSRepoApp *> *byBid = [NSMutableDictionary dictionary];
		NSMutableSet<NSString *> *seenKeys = [NSMutableSet set];
		for (NSString *u in urls) {
			for (SSRepoApp *a in (perSource[u] ?: @[])) {
				NSString *dedupKey = a.bundleId.length > 0
				                   ? [NSString stringWithFormat:@"%@|%@", a.bundleId.lowercaseString, a.name.lowercaseString]
				                   : [NSString stringWithFormat:@"%@|%@", a.sourceURL, a.name.lowercaseString];
				if ([seenKeys containsObject:dedupKey]) continue;
				[seenKeys addObject:dedupKey];
				[merged addObject:a];
				if (a.bundleId.length > 0 && !byBid[a.bundleId.lowercaseString]) {
					byBid[a.bundleId.lowercaseString] = a;
				}
			}
		}
		[merged sortUsingComparator:^NSComparisonResult(SSRepoApp *x, SSRepoApp *y) {
			return [x.name localizedCaseInsensitiveCompare:y.name];
		}];
		self.allApps = merged;
		self.repoAppByBundleId = byBid;
		self.loadingRepos = NO;
		[[NSNotificationCenter defaultCenter] postNotificationName:SSReposDidChangeNotification object:self];
	});
}

@end

/* ---- Custom App Row Cell ---- */

@interface SSAppTableCell : UITableViewCell
@property (nonatomic, strong) UIImageView *iconView;
@property (nonatomic, strong) UILabel *titleLabel;
@property (nonatomic, strong) UILabel *subLabel;
@property (nonatomic, strong) UILabel *metaLabel;
@property (nonatomic, strong) UIButton *actionButton;
@property (nonatomic, copy) NSString *loadedIconURL;
@property (nonatomic, copy) void (^onActionTap)(void);
@end

@implementation SSAppTableCell

- (instancetype)initWithStyle:(UITableViewCellStyle)style reuseIdentifier:(NSString *)reuseIdentifier
{
	if ((self = [super initWithStyle:style reuseIdentifier:reuseIdentifier])) {
		_iconView = [[UIImageView alloc] init];
		_iconView.translatesAutoresizingMaskIntoConstraints = NO;
		_iconView.layer.cornerRadius = 12.0;
		_iconView.layer.cornerCurve = kCACornerCurveContinuous;
		_iconView.layer.borderWidth = 0.5;
		_iconView.layer.borderColor = [UIColor separatorColor].CGColor;
		_iconView.clipsToBounds = YES;
		_iconView.contentMode = UIViewContentModeScaleAspectFill;
		[self.contentView addSubview:_iconView];

		_titleLabel = [[UILabel alloc] init];
		_titleLabel.translatesAutoresizingMaskIntoConstraints = NO;
		_titleLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
		_titleLabel.textColor = [UIColor labelColor];
		[self.contentView addSubview:_titleLabel];

		_subLabel = [[UILabel alloc] init];
		_subLabel.translatesAutoresizingMaskIntoConstraints = NO;
		_subLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightRegular];
		_subLabel.textColor = [UIColor secondaryLabelColor];
		[self.contentView addSubview:_subLabel];

		_metaLabel = [[UILabel alloc] init];
		_metaLabel.translatesAutoresizingMaskIntoConstraints = NO;
		_metaLabel.font = [UIFont systemFontOfSize:11.5 weight:UIFontWeightMedium];
		_metaLabel.textColor = [UIColor tertiaryLabelColor];
		[self.contentView addSubview:_metaLabel];

		_actionButton = [UIButton buttonWithType:UIButtonTypeSystem];
		_actionButton.translatesAutoresizingMaskIntoConstraints = NO;
		_actionButton.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightBold];
		_actionButton.backgroundColor = [UIColor secondarySystemFillColor];
		_actionButton.layer.cornerRadius = 15.0;
		_actionButton.contentEdgeInsets = UIEdgeInsetsMake(6, 12, 6, 12);
		[_actionButton addTarget:self action:@selector(handleTap) forControlEvents:UIControlEventTouchUpInside];
		[self.contentView addSubview:_actionButton];

		[NSLayoutConstraint activateConstraints:@[
			[_iconView.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor constant:16],
			[_iconView.centerYAnchor constraintEqualToAnchor:self.contentView.centerYAnchor],
			[_iconView.widthAnchor constraintEqualToConstant:52],
			[_iconView.heightAnchor constraintEqualToConstant:52],

			[_actionButton.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor constant:-16],
			[_actionButton.centerYAnchor constraintEqualToAnchor:self.contentView.centerYAnchor],
			[_actionButton.heightAnchor constraintEqualToConstant:30],
			[_actionButton.widthAnchor constraintGreaterThanOrEqualToConstant:68],

			[_titleLabel.leadingAnchor constraintEqualToAnchor:_iconView.trailingAnchor constant:12],
			[_titleLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_actionButton.leadingAnchor constant:-10],
			[_titleLabel.topAnchor constraintEqualToAnchor:self.contentView.topAnchor constant:12],

			[_subLabel.leadingAnchor constraintEqualToAnchor:_titleLabel.leadingAnchor],
			[_subLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_actionButton.leadingAnchor constant:-10],
			[_subLabel.topAnchor constraintEqualToAnchor:_titleLabel.bottomAnchor constant:2],

			[_metaLabel.leadingAnchor constraintEqualToAnchor:_titleLabel.leadingAnchor],
			[_metaLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_actionButton.leadingAnchor constant:-10],
			[_metaLabel.topAnchor constraintEqualToAnchor:_subLabel.bottomAnchor constant:2],
		]];
	}
	return self;
}

- (void)handleTap
{
	if (self.onActionTap) self.onActionTap();
}

- (void)setIconWithURL:(NSString *)iconURL
{
	self.loadedIconURL = iconURL;
	self.iconView.image = [[SSIconLoader shared] placeholderIcon];
	if (iconURL.length > 0) {
		NSString *expected = [iconURL copy];
		[[SSIconLoader shared] loadIconURL:expected completion:^(UIImage *img) {
			if ([self.loadedIconURL isEqualToString:expected]) {
				self.iconView.image = img;
			}
		}];
	}
}

- (void)configureWithRepoApp:(SSRepoApp *)app installedEntry:(NSDictionary *)inst
{
	self.titleLabel.text = app.name;
	NSString *sub = app.subtitle.length > 0 ? app.subtitle : (app.developer.length > 0 ? app.developer : app.bundleId);
	self.subLabel.text = sub.length > 0 ? sub : @"Native iOS App";

	NSMutableArray *parts = [NSMutableArray array];
	if (app.bestVersion.version.length > 0) [parts addObject:[@"v" stringByAppendingString:app.bestVersion.version]];
	NSString *sz = SSFormatBytes(app.bestVersion.sizeBytes);
	if (sz.length > 0) [parts addObject:sz];
	if (app.sourceName.length > 0) [parts addObject:app.sourceName];
	self.metaLabel.text = [parts componentsJoinedByString:@" • "];

	if (inst) {
		[self.actionButton setTitle:@"OPEN" forState:UIControlStateNormal];
		[self.actionButton setTitleColor:[UIColor systemGreenColor] forState:UIControlStateNormal];
	} else {
		[self.actionButton setTitle:@"GET" forState:UIControlStateNormal];
		[self.actionButton setTitleColor:[UIColor systemBlueColor] forState:UIControlStateNormal];
	}
	[self setIconWithURL:app.iconURL];
}

- (void)configureWithCatalogItem:(SSStoreCatalogItem *)item installedEntry:(NSDictionary *)inst matchedRepo:(SSRepoApp *)repoApp
{
	self.titleLabel.text = item.name;
	NSMutableArray *subParts = [NSMutableArray array];
	if (item.developer.length > 0) [subParts addObject:item.developer];
	if (item.genre.length > 0) [subParts addObject:item.genre];
	self.subLabel.text = subParts.count > 0 ? [subParts componentsJoinedByString:@" • "] : item.bundleId;

	NSMutableArray *meta = [NSMutableArray array];
	if (item.rating > 0.0) [meta addObject:[NSString stringWithFormat:@"★ %.1f", item.rating]];
	if (repoApp) {
		[meta addObject:[NSString stringWithFormat:@"Native IPA (%@)", repoApp.sourceName ?: @"Repo"]];
	} else {
		[meta addObject:@"Instant WebApp"];
	}
	self.metaLabel.text = [meta componentsJoinedByString:@" • "];

	if (inst) {
		[self.actionButton setTitle:@"OPEN" forState:UIControlStateNormal];
		[self.actionButton setTitleColor:[UIColor systemGreenColor] forState:UIControlStateNormal];
	} else if (repoApp) {
		[self.actionButton setTitle:@"GET" forState:UIControlStateNormal];
		[self.actionButton setTitleColor:[UIColor systemBlueColor] forState:UIControlStateNormal];
	} else {
		[self.actionButton setTitle:@"WEB APP" forState:UIControlStateNormal];
		[self.actionButton setTitleColor:[UIColor systemIndigoColor] forState:UIControlStateNormal];
	}
	[self setIconWithURL:item.iconURL];
}

@end

/* ---- Repo App Detail View Controller ---- */

@interface SSAppDetailViewController : UITableViewController
@property (nonatomic, strong) SSRepoApp *app;
@end

@implementation SSAppDetailViewController

- (instancetype)initWithApp:(SSRepoApp *)app
{
	if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
		_app = app;
		self.title = app.name;
	}
	return self;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	[[NSNotificationCenter defaultCenter] addObserver:self.tableView selector:@selector(reloadData) name:SSStateDidChangeNotification object:nil];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { (void)tableView; return 3; }

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	(void)tableView;
	if (section == 0) return 2;
	if (section == 1) return 1;
	return (NSInteger)MIN(self.app.versions.count, 12);
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section
{
	(void)tableView;
	if (section == 0) return @"Application";
	if (section == 1) return @"Description";
	return @"Available Versions (Tap to Install)";
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	if (indexPath.section == 0 && indexPath.row == 0) {
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
		c.selectionStyle = UITableViewCellSelectionStyleNone;
		c.textLabel.text = self.app.name;
		c.textLabel.font = [UIFont systemFontOfSize:18 weight:UIFontWeightBold];
		c.detailTextLabel.text = [NSString stringWithFormat:@"%@\n%@ • %@",
		                          self.app.developer.length > 0 ? self.app.developer : @"Community Developer",
		                          self.app.bundleId.length > 0 ? self.app.bundleId : @"Unknown Bundle ID",
		                          self.app.sourceName ?: @""];
		c.detailTextLabel.numberOfLines = 0;
		c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
		c.imageView.image = [[SSIconLoader shared] placeholderIcon];
		c.imageView.layer.cornerRadius = 10;
		c.imageView.clipsToBounds = YES;
		[[SSIconLoader shared] loadIconURL:self.app.iconURL completion:^(UIImage *img) {
			c.imageView.image = img;
			[c setNeedsLayout];
		}];
		return c;
	}
	if (indexPath.section == 0 && indexPath.row == 1) {
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
		NSDictionary *inst = [[SSStoreController shared] installedEntryForBundleId:self.app.bundleId];
		c.textLabel.textAlignment = NSTextAlignmentCenter;
		c.textLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightBold];
		if (inst) {
			c.textLabel.text = [NSString stringWithFormat:@"Open %@ (or Reinstall v%@)", self.app.name, self.app.bestVersion.version];
			c.textLabel.textColor = [UIColor systemGreenColor];
		} else {
			NSString *sz = SSFormatBytes(self.app.bestVersion.sizeBytes);
			c.textLabel.text = [NSString stringWithFormat:@"Download & Install v%@%@",
			                    self.app.bestVersion.version,
			                    sz.length > 0 ? [NSString stringWithFormat:@" (%@)", sz] : @""];
			c.textLabel.textColor = [UIColor systemBlueColor];
		}
		return c;
	}
	if (indexPath.section == 1) {
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
		c.selectionStyle = UITableViewCellSelectionStyleNone;
		c.textLabel.numberOfLines = 0;
		c.textLabel.font = [UIFont systemFontOfSize:14];
		c.textLabel.textColor = [UIColor secondaryLabelColor];
		c.textLabel.text = self.app.appDescription.length > 0 ? self.app.appDescription : (self.app.subtitle.length > 0 ? self.app.subtitle : @"No description provided.");
		return c;
	}

	UITableViewCell *c = [tableView dequeueReusableCellWithIdentifier:@"VerCell"];
	if (!c) c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"VerCell"];
	SSAppVersion *v = self.app.versions[(NSUInteger)indexPath.row];
	NSString *sz = SSFormatBytes(v.sizeBytes);
	c.textLabel.text = [NSString stringWithFormat:@"Version %@%@", v.version, sz.length > 0 ? [NSString stringWithFormat:@" • %@", sz] : @""];
	c.textLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
	NSMutableArray *info = [NSMutableArray array];
	if (v.minOSVersion.length > 0) [info addObject:[NSString stringWithFormat:@"iOS %@+", v.minOSVersion]];
	if (v.date.length > 0) [info addObject:[v.date substringToIndex:MIN((NSUInteger)10, v.date.length)]];
	c.detailTextLabel.text = info.count > 0 ? [info componentsJoinedByString:@" • "] : v.downloadURL;
	c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
	c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
	return c;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	if (indexPath.section == 0 && indexPath.row == 1) {
		NSDictionary *inst = [[SSStoreController shared] installedEntryForBundleId:self.app.bundleId];
		if (inst) {
			UIAlertController *ac = [UIAlertController alertControllerWithTitle:self.app.name message:nil preferredStyle:UIAlertControllerStyleActionSheet];
			[ac addAction:[UIAlertAction actionWithTitle:@"Open App" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
				[[SSStoreController shared] openBundleId:SSString(inst[@"bundleId"])];
			}]];
			[ac addAction:[UIAlertAction actionWithTitle:[NSString stringWithFormat:@"Reinstall / Update (v%@)", self.app.bestVersion.version]
			                                       style:UIAlertActionStyleDefault
			                                     handler:^(__unused UIAlertAction *a) {
				[[SSStoreController shared] downloadAndInstallURL:self.app.bestVersion.downloadURL title:self.app.name fromVC:self];
			}]];
			[ac addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
			[self presentViewController:ac animated:YES completion:nil];
		} else {
			[[SSStoreController shared] downloadAndInstallURL:self.app.bestVersion.downloadURL title:self.app.name fromVC:self];
		}
	} else if (indexPath.section == 2) {
		SSAppVersion *v = self.app.versions[(NSUInteger)indexPath.row];
		[[SSStoreController shared] downloadAndInstallURL:v.downloadURL title:[NSString stringWithFormat:@"%@ v%@", self.app.name, v.version] fromVC:self];
	}
}

@end

/* ---- App Store Catalog Detail View Controller (Screenshots + Native/WebApp Actions) ---- */

@interface SSCatalogDetailViewController : UITableViewController <UIDocumentPickerDelegate>
@property (nonatomic, strong) SSStoreCatalogItem *item;
@end

@implementation SSCatalogDetailViewController

- (instancetype)initWithCatalogItem:(SSStoreCatalogItem *)item
{
	if ((self = [super initWithStyle:UITableViewStyleInsetGrouped])) {
		_item = item;
		self.title = item.name;
	}
	return self;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	[[NSNotificationCenter defaultCenter] addObserver:self.tableView selector:@selector(reloadData) name:SSStateDidChangeNotification object:nil];
	[[NSNotificationCenter defaultCenter] addObserver:self.tableView selector:@selector(reloadData) name:SSReposDidChangeNotification object:nil];

	/* Enrich RSS chart entries with full iTunes Lookup screenshots & description */
	if (self.item.screenshotURLs.count == 0 && self.item.bundleId.length > 0) {
		NSString *enc = [self.item.bundleId stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLQueryAllowedCharacterSet]] ?: @"";
		NSURL *lu = [NSURL URLWithString:[NSString stringWithFormat:@"https://itunes.apple.com/lookup?bundleId=%@&country=in", enc]];
		if (lu) {
			[[[NSURLSession sharedSession] dataTaskWithURL:lu completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
				(void)resp;
				if (err || data.length == 0) return;
				NSDictionary *j = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
				NSArray *res = [j[@"results"] isKindOfClass:[NSArray class]] ? j[@"results"] : nil;
				NSDictionary *r = [res.firstObject isKindOfClass:[NSDictionary class]] ? res.firstObject : nil;
				if (!r) return;
				dispatch_async(dispatch_get_main_queue(), ^{
					if ([r[@"screenshotUrls"] isKindOfClass:[NSArray class]]) {
						self.item.screenshotURLs = r[@"screenshotUrls"];
					}
					if (SSString(r[@"description"]).length > 0) self.item.summary = SSString(r[@"description"]);
					if (SSString(r[@"artworkUrl512"]).length > 0) self.item.iconURL = SSString(r[@"artworkUrl512"]);
					if (SSString(r[@"sellerUrl"]).length > 0) self.item.sellerURL = SSString(r[@"sellerUrl"]);
					if ([r[@"averageUserRating"] respondsToSelector:@selector(doubleValue)]) {
						self.item.rating = [r[@"averageUserRating"] doubleValue];
					}
					[self.tableView reloadData];
				});
			}] resume];
		}
	}
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView
{
	(void)tableView;
	return self.item.screenshotURLs.count > 0 ? 3 : 2;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	(void)tableView;
	if (section == 0) return 3;
	return 1;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section
{
	(void)tableView;
	if (section == 0) return @"App Store Listing";
	if (self.item.screenshotURLs.count > 0 && section == 1) return @"Screenshots";
	return @"About";
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	SSStoreController *sc = [SSStoreController shared];
	NSDictionary *inst = [sc installedEntryForBundleId:self.item.bundleId];
	SSRepoApp *repoMatch = [sc matchedRepoAppForBundleId:self.item.bundleId name:self.item.name];

	if (indexPath.section == 0 && indexPath.row == 0) {
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
		c.selectionStyle = UITableViewCellSelectionStyleNone;
		c.textLabel.text = self.item.name;
		c.textLabel.font = [UIFont systemFontOfSize:18 weight:UIFontWeightBold];
		NSMutableString *det = [NSMutableString stringWithFormat:@"%@\n%@",
		                        self.item.developer.length > 0 ? self.item.developer : @"App Store Developer",
		                        self.item.bundleId ?: @""];
		if (self.item.rating > 0) [det appendFormat:@" • ★ %.1f", self.item.rating];
		c.detailTextLabel.text = det;
		c.detailTextLabel.numberOfLines = 0;
		c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
		c.imageView.image = [[SSIconLoader shared] placeholderIcon];
		c.imageView.layer.cornerRadius = 11;
		c.imageView.clipsToBounds = YES;
		[[SSIconLoader shared] loadIconURL:self.item.iconURL completion:^(UIImage *img) {
			c.imageView.image = img;
			[c setNeedsLayout];
		}];
		return c;
	}

	if (indexPath.section == 0 && indexPath.row == 1) {
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
		c.textLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightBold];
		if (inst) {
			c.textLabel.text = [NSString stringWithFormat:@"Open %@", self.item.name];
			c.textLabel.textColor = [UIColor systemGreenColor];
			c.detailTextLabel.text = [NSString stringWithFormat:@"Installed (%@)", SSString(inst[@"kind"])];
		} else if (repoMatch) {
			c.textLabel.text = [NSString stringWithFormat:@"Install Native IPA (v%@)", repoMatch.bestVersion.version];
			c.textLabel.textColor = [UIColor systemBlueColor];
			c.detailTextLabel.text = [NSString stringWithFormat:@"Matched in %@ • Full ARM64 + Metal2", repoMatch.sourceName ?: @"Repo"];
		} else {
			NSDictionary *spec = SSResolveWebAppSpec(self.item.bundleId, self.item.name, self.item.sellerURL);
			c.textLabel.text = @"Install Standalone Web App";
			c.textLabel.textColor = [UIColor systemIndigoColor];
			c.detailTextLabel.text = [NSString stringWithFormat:@"Adds %@ icon to Home Screen (%@)", self.item.name, spec[@"url"]];
		}
		c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
		c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
		return c;
	}

	if (indexPath.section == 0 && indexPath.row == 2) {
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
		c.textLabel.text = @"Import Unencrypted .ipa File for This App...";
		c.textLabel.font = [UIFont systemFontOfSize:14.5 weight:UIFontWeightMedium];
		c.textLabel.textColor = [UIColor systemBlueColor];
		c.imageView.image = [UIImage systemImageNamed:@"arrow.down.doc"];
		return c;
	}

	if (self.item.screenshotURLs.count > 0 && indexPath.section == 1) {
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
		c.selectionStyle = UITableViewCellSelectionStyleNone;
		UIScrollView *sv = [[UIScrollView alloc] init];
		sv.translatesAutoresizingMaskIntoConstraints = NO;
		sv.showsHorizontalScrollIndicator = NO;
		[c.contentView addSubview:sv];
		[NSLayoutConstraint activateConstraints:@[
			[sv.topAnchor constraintEqualToAnchor:c.contentView.topAnchor constant:12],
			[sv.bottomAnchor constraintEqualToAnchor:c.contentView.bottomAnchor constant:-12],
			[sv.leadingAnchor constraintEqualToAnchor:c.contentView.leadingAnchor],
			[sv.trailingAnchor constraintEqualToAnchor:c.contentView.trailingAnchor],
			[sv.heightAnchor constraintEqualToConstant:260],
		]];
		NSUInteger cnt = MIN((NSUInteger)5, self.item.screenshotURLs.count);
		CGFloat x = 16.0;
		for (NSUInteger i = 0; i < cnt; i++) {
			UIImageView *iv = [[UIImageView alloc] initWithFrame:CGRectMake(x, 0, 146, 260)];
			iv.backgroundColor = [UIColor secondarySystemFillColor];
			iv.contentMode = UIViewContentModeScaleAspectFill;
			iv.layer.cornerRadius = 12.0;
			iv.clipsToBounds = YES;
			[sv addSubview:iv];
			[[SSIconLoader shared] loadRawImageURL:self.item.screenshotURLs[i] completion:^(UIImage *img) {
				if (img) iv.image = img;
			}];
			x += 156.0;
		}
		sv.contentSize = CGSizeMake(x + 4.0, 260);
		return c;
	}

	UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
	c.selectionStyle = UITableViewCellSelectionStyleNone;
	c.textLabel.numberOfLines = 0;
	c.textLabel.font = [UIFont systemFontOfSize:14];
	c.textLabel.textColor = [UIColor secondaryLabelColor];
	c.textLabel.text = self.item.summary.length > 0 ? self.item.summary : @"No description available.";
	return c;
}

- (void)promptWebAppInstall
{
	NSDictionary *spec = SSResolveWebAppSpec(self.item.bundleId, self.item.name, self.item.sellerURL);
	UIAlertController *ac = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"Install %@ Web App", self.item.name]
	                                                            message:@"Creates a standalone native Home Screen app with official icon & persistent storage:"
	                                                     preferredStyle:UIAlertControllerStyleAlert];
	[ac addTextFieldWithConfigurationHandler:^(UITextField *tf) {
		tf.text = spec[@"url"];
		tf.keyboardType = UIKeyboardTypeURL;
		tf.autocapitalizationType = UITextAutocapitalizationTypeNone;
		tf.autocorrectionType = UITextAutocorrectionTypeNo;
	}];
	[ac addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
	[ac addAction:[UIAlertAction actionWithTitle:@"Install (Mobile UA)" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		[[SSStoreController shared] createWebAppWithName:self.item.name
		                                             url:ac.textFields.firstObject.text
		                                       userAgent:@"mobile"
		                                         iconURL:self.item.iconURL
		                                    origBundleId:self.item.bundleId
		                                       switchTab:YES];
	}]];
	[ac addAction:[UIAlertAction actionWithTitle:@"Install (Desktop UA)" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		[[SSStoreController shared] createWebAppWithName:self.item.name
		                                             url:ac.textFields.firstObject.text
		                                       userAgent:@"desktop"
		                                         iconURL:self.item.iconURL
		                                    origBundleId:self.item.bundleId
		                                       switchTab:YES];
	}]];
	[self presentViewController:ac animated:YES completion:nil];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	if (indexPath.section != 0) return;

	SSStoreController *sc = [SSStoreController shared];
	NSDictionary *inst = [sc installedEntryForBundleId:self.item.bundleId];
	SSRepoApp *repoMatch = [sc matchedRepoAppForBundleId:self.item.bundleId name:self.item.name];

	if (indexPath.row == 1) {
		if (inst) {
			[sc openBundleId:SSString(inst[@"bundleId"])];
		} else if (repoMatch) {
			[sc downloadAndInstallURL:repoMatch.bestVersion.downloadURL title:repoMatch.name fromVC:self];
		} else {
			[self promptWebAppInstall];
		}
	} else if (indexPath.row == 2) {
		UTType *ipaType = [UTType typeWithFilenameExtension:@"ipa"] ?: UTTypeData;
		UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ipaType, UTTypeArchive, UTTypeData] asCopy:YES];
		picker.delegate = self;
		[self presentViewController:picker animated:YES completion:nil];
	}
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
	(void)controller;
	if (urls.firstObject) {
		[[SSStoreController shared] importExternalFileURL:urls.firstObject switchTab:YES];
	}
}

@end

/* ---- Sources Management Modal ---- */

@interface SSSourcesViewController : UITableViewController
@end

@implementation SSSourcesViewController

- (instancetype)init { return [super initWithStyle:UITableViewStyleInsetGrouped]; }

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Repo Sources";
	self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(closeSelf)];
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(promptAddSource)];
	[[NSNotificationCenter defaultCenter] addObserver:self.tableView selector:@selector(reloadData) name:SSReposDidChangeNotification object:nil];
}

- (void)closeSelf { [self dismissViewControllerAnimated:YES completion:nil]; }

- (void)promptAddSource
{
	UIAlertController *ac = [UIAlertController alertControllerWithTitle:@"Add AltStore / SideStore / Feather Repo"
	                                                            message:@"Paste any JSON repository URL:"
	                                                     preferredStyle:UIAlertControllerStyleAlert];
	[ac addTextFieldWithConfigurationHandler:^(UITextField *tf) {
		tf.placeholder = @"https://example.com/apps.json";
		tf.keyboardType = UIKeyboardTypeURL;
		tf.autocapitalizationType = UITextAutocapitalizationTypeNone;
		tf.autocorrectionType = UITextAutocorrectionTypeNo;
		if ([UIPasteboard generalPasteboard].string.length > 8 &&
		    [[UIPasteboard generalPasteboard].string.lowercaseString hasPrefix:@"http"]) {
			tf.text = [UIPasteboard generalPasteboard].string;
		}
	}];
	[ac addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
	[ac addAction:[UIAlertAction actionWithTitle:@"Add" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		[[SSStoreController shared] addSourceURL:ac.textFields.firstObject.text];
	}]];
	[self presentViewController:ac animated:YES completion:nil];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { (void)tableView; return 2; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	(void)tableView;
	return section == 0 ? (NSInteger)[SSStoreController shared].sourceInfos.count : 1;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section
{
	(void)tableView;
	return section == 0 ? @"Configured JSON Repositories" : @"Actions";
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	if (indexPath.section == 1) {
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
		c.textLabel.text = @"Reset to Default Sources";
		c.textLabel.textAlignment = NSTextAlignmentCenter;
		c.textLabel.textColor = [UIColor systemBlueColor];
		return c;
	}
	UITableViewCell *c = [tableView dequeueReusableCellWithIdentifier:@"SrcCell"];
	if (!c) c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"SrcCell"];
	c.selectionStyle = UITableViewCellSelectionStyleNone;
	SSRepoSourceInfo *info = [SSStoreController shared].sourceInfos[(NSUInteger)indexPath.row];
	c.textLabel.text = info.name;
	c.textLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
	if (info.loading) {
		c.detailTextLabel.text = [NSString stringWithFormat:@"Loading... • %@", info.url];
		c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
	} else if (info.errorText.length > 0) {
		c.detailTextLabel.text = [NSString stringWithFormat:@"Error: %@ • %@", info.errorText, info.url];
		c.detailTextLabel.textColor = [UIColor systemRedColor];
	} else {
		c.detailTextLabel.text = [NSString stringWithFormat:@"%ld apps • %@", (long)info.appCount, info.url];
		c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
	}
	return c;
}

- (BOOL)tableView:(UITableView *)tableView canEditRowAtIndexPath:(NSIndexPath *)indexPath { (void)tableView; return indexPath.section == 0; }

- (void)tableView:(UITableView *)tableView commitEditingStyle:(UITableViewCellEditingStyle)editingStyle forRowAtIndexPath:(NSIndexPath *)indexPath
{
	(void)tableView;
	if (editingStyle == UITableViewCellEditingStyleDelete && indexPath.section == 0) {
		[[SSStoreController shared] removeSourceAtIndex:(NSUInteger)indexPath.row];
	}
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	if (indexPath.section == 1) [[SSStoreController shared] resetDefaultSources];
}

@end

/* ---- Tab 1: App Store Discover & Live Search (itunes.apple.com) ---- */

@interface SSDiscoverViewController : UITableViewController <UISearchResultsUpdating, UISearchBarDelegate>
@property (nonatomic, strong) UISearchController *searchController;
@property (nonatomic, strong) NSArray<SSStoreCatalogItem *> *topCharts;
@property (nonatomic, strong) NSArray<SSStoreCatalogItem *> *searchResults;
@property (nonatomic, assign) BOOL loadingCharts;
@property (nonatomic, assign) BOOL searching;
@property (nonatomic, copy) NSString *statusText;
@property (nonatomic, strong) NSTimer *debounceTimer;
@end

@implementation SSDiscoverViewController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"App Store";
	self.navigationController.navigationBar.prefersLargeTitles = YES;
	self.tableView.rowHeight = 76.0;
	[self.tableView registerClass:[SSAppTableCell class] forCellReuseIdentifier:@"CatCell"];

	self.topCharts = @[];
	self.searchResults = @[];

	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
	                                                                                       target:self
	                                                                                       action:@selector(loadTopCharts)];

	self.searchController = [[UISearchController alloc] initWithSearchResultsController:nil];
	self.searchController.searchResultsUpdater = self;
	self.searchController.obscuresBackgroundDuringPresentation = NO;
	self.searchController.searchBar.placeholder = @"Search any App Store app or game...";
	self.searchController.searchBar.delegate = self;
	self.navigationItem.searchController = self.searchController;
	self.navigationItem.hidesSearchBarWhenScrolling = NO;
	self.definesPresentationContext = YES;

	UIRefreshControl *rc = [[UIRefreshControl alloc] init];
	[rc addTarget:self action:@selector(loadTopCharts) forControlEvents:UIControlEventValueChanged];
	self.refreshControl = rc;

	[[NSNotificationCenter defaultCenter] addObserver:self.tableView selector:@selector(reloadData) name:SSStateDidChangeNotification object:nil];
	[[NSNotificationCenter defaultCenter] addObserver:self.tableView selector:@selector(reloadData) name:SSReposDidChangeNotification object:nil];

	[self loadTopCharts];
}

- (void)updateBanner
{
	UILabel *hdr = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, self.view.bounds.size.width, 36)];
	hdr.font = [UIFont systemFontOfSize:12.5 weight:UIFontWeightMedium];
	hdr.textColor = [UIColor secondaryLabelColor];
	hdr.textAlignment = NSTextAlignmentCenter;
	hdr.text = self.statusText ?: @"Live Apple App Store Catalog • Auto Repo & WebApp Resolver";
	self.tableView.tableHeaderView = hdr;
}

- (void)loadTopCharts
{
	self.loadingCharts = YES;
	self.statusText = @"Loading Apple App Store Top Free Charts...";
	[self updateBanner];

	NSURL *u = [NSURL URLWithString:@"https://itunes.apple.com/in/rss/topfreeapplications/limit=40/json"];
	[[[NSURLSession sharedSession] dataTaskWithURL:u completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
		(void)resp;
		NSMutableArray<SSStoreCatalogItem *> *items = [NSMutableArray array];
		if (!err && data.length > 0) {
			NSDictionary *root = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
			NSDictionary *feed = [root[@"feed"] isKindOfClass:[NSDictionary class]] ? root[@"feed"] : nil;
			NSArray *entries = [feed[@"entry"] isKindOfClass:[NSArray class]] ? feed[@"entry"] : @[];
			for (NSDictionary *e in entries) {
				if (![e isKindOfClass:[NSDictionary class]]) continue;
				SSStoreCatalogItem *it = [[SSStoreCatalogItem alloc] init];
				it.name = SSString(e[@"im:name"][@"label"]);
				if (it.name.length == 0) continue;
				it.bundleId = SSString(e[@"id"][@"attributes"][@"im:bundleId"]);
				it.trackId = SSString(e[@"id"][@"attributes"][@"im:id"]);
				it.developer = SSString(e[@"im:artist"][@"label"]);
				it.genre = SSString(e[@"category"][@"attributes"][@"label"]);
				it.summary = SSString(e[@"summary"][@"label"]);
				NSArray *imgs = [e[@"im:image"] isKindOfClass:[NSArray class]] ? e[@"im:image"] : @[];
				NSDictionary *lastImg = [imgs.lastObject isKindOfClass:[NSDictionary class]] ? imgs.lastObject : nil;
				NSString *rawIcon = SSString(lastImg[@"label"]);
				/* Upgrade 100x100 RSS icon URL to 512x512 when served from mzstatic */
				it.iconURL = [rawIcon stringByReplacingOccurrencesOfString:@"100x100bb" withString:@"512x512bb"];
				[items addObject:it];
			}
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			self.loadingCharts = NO;
			if (self.refreshControl.refreshing) [self.refreshControl endRefreshing];
			if (items.count > 0) {
				self.topCharts = items;
				self.statusText = [NSString stringWithFormat:@"Top %lu Free iPhone Apps • iOS %@", (unsigned long)items.count, SSCurrentIOSVersion()];
			} else {
				self.statusText = err ? [NSString stringWithFormat:@"App Store offline (%@)", err.localizedDescription] : @"No chart items returned";
			}
			[self updateBanner];
			[self.tableView reloadData];
		});
	}] resume];
}

- (void)updateSearchResultsForSearchController:(UISearchController *)searchController
{
	[self.debounceTimer invalidate];
	NSString *q = [searchController.searchBar.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
	if (q.length == 0) {
		self.searching = NO;
		self.searchResults = @[];
		self.statusText = self.topCharts.count > 0
		                ? [NSString stringWithFormat:@"Top %lu Free iPhone Apps • iOS %@", (unsigned long)self.topCharts.count, SSCurrentIOSVersion()]
		                : @"Live Apple App Store Catalog";
		[self updateBanner];
		[self.tableView reloadData];
		return;
	}
	self.searching = YES;
	self.debounceTimer = [NSTimer scheduledTimerWithTimeInterval:0.35 repeats:NO block:^(__unused NSTimer *t) {
		[self performITunesSearch:q];
	}];
}

- (void)performITunesSearch:(NSString *)query
{
	self.statusText = [NSString stringWithFormat:@"Searching App Store for \"%@\"...", query];
	[self updateBanner];
	NSString *enc = [query stringByAddingPercentEncodingWithAllowedCharacters:[NSCharacterSet URLQueryAllowedCharacterSet]] ?: @"";
	NSURL *u = [NSURL URLWithString:[NSString stringWithFormat:@"https://itunes.apple.com/search?country=in&entity=software&limit=35&term=%@", enc]];
	if (!u) return;

	[[[NSURLSession sharedSession] dataTaskWithURL:u completionHandler:^(NSData *data, NSURLResponse *resp, NSError *err) {
		(void)resp;
		NSMutableArray<SSStoreCatalogItem *> *out = [NSMutableArray array];
		if (!err && data.length > 0) {
			NSDictionary *j = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
			NSArray *res = [j[@"results"] isKindOfClass:[NSArray class]] ? j[@"results"] : @[];
			for (NSDictionary *r in res) {
				if (![r isKindOfClass:[NSDictionary class]]) continue;
				SSStoreCatalogItem *it = [[SSStoreCatalogItem alloc] init];
				it.name = SSString(r[@"trackName"]);
				if (it.name.length == 0) continue;
				it.bundleId = SSString(r[@"bundleId"]);
				it.trackId = SSString(r[@"trackId"]);
				it.developer = SSString(r[@"artistName"]);
				it.genre = SSString(r[@"primaryGenreName"]);
				it.summary = SSString(r[@"description"]);
				it.version = SSString(r[@"version"]);
				it.sellerURL = SSString(r[@"sellerUrl"]);
				it.iconURL = SSString(r[@"artworkUrl512"]).length > 0 ? SSString(r[@"artworkUrl512"]) : SSString(r[@"artworkUrl100"]);
				it.rating = [r[@"averageUserRating"] respondsToSelector:@selector(doubleValue)] ? [r[@"averageUserRating"] doubleValue] : 0.0;
				it.sizeBytes = [r[@"fileSizeBytes"] respondsToSelector:@selector(longLongValue)] ? [r[@"fileSizeBytes"] longLongValue] : 0;
				it.screenshotURLs = [r[@"screenshotUrls"] isKindOfClass:[NSArray class]] ? r[@"screenshotUrls"] : @[];
				[out addObject:it];
			}
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			if (!self.searching) return;
			self.searchResults = out;
			self.statusText = err ? [NSString stringWithFormat:@"Search error: %@", err.localizedDescription]
			                      : [NSString stringWithFormat:@"%lu App Store results for \"%@\"", (unsigned long)out.count, query];
			[self updateBanner];
			[self.tableView reloadData];
		});
	}] resume];
}

- (NSArray<SSStoreCatalogItem *> *)activeList
{
	return self.searching ? self.searchResults : self.topCharts;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	(void)tableView; (void)section;
	return (NSInteger)[self activeList].count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	SSAppTableCell *cell = [tableView dequeueReusableCellWithIdentifier:@"CatCell" forIndexPath:indexPath];
	SSStoreCatalogItem *item = [self activeList][(NSUInteger)indexPath.row];
	SSStoreController *sc = [SSStoreController shared];
	NSDictionary *inst = [sc installedEntryForBundleId:item.bundleId];
	SSRepoApp *repoMatch = [sc matchedRepoAppForBundleId:item.bundleId name:item.name];
	[cell configureWithCatalogItem:item installedEntry:inst matchedRepo:repoMatch];

	__weak typeof(self) weakSelf = self;
	cell.onActionTap = ^{
		__strong typeof(weakSelf) strongSelf = weakSelf;
		if (!strongSelf) return;
		if (inst) {
			[sc openBundleId:SSString(inst[@"bundleId"])];
		} else if (repoMatch) {
			[sc downloadAndInstallURL:repoMatch.bestVersion.downloadURL title:repoMatch.name fromVC:strongSelf];
		} else {
			NSDictionary *spec = SSResolveWebAppSpec(item.bundleId, item.name, item.sellerURL);
			[sc createWebAppWithName:item.name
			                     url:spec[@"url"]
			               userAgent:spec[@"ua"]
			                 iconURL:item.iconURL
			            origBundleId:item.bundleId
			               switchTab:YES];
		}
	};
	return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	SSStoreCatalogItem *item = [self activeList][(NSUInteger)indexPath.row];
	SSCatalogDetailViewController *vc = [[SSCatalogDetailViewController alloc] initWithCatalogItem:item];
	[self.navigationController pushViewController:vc animated:YES];
}

@end

/* ---- Tab 2: Repos (Native cryptid=0 IPAs) ---- */

@interface SSBrowseViewController : UITableViewController <UISearchResultsUpdating>
@property (nonatomic, strong) UISearchController *searchController;
@property (nonatomic, strong) NSArray<SSRepoApp *> *filteredApps;
@end

@implementation SSBrowseViewController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Repos";
	self.navigationController.navigationBar.prefersLargeTitles = YES;
	self.tableView.rowHeight = 76.0;
	[self.tableView registerClass:[SSAppTableCell class] forCellReuseIdentifier:@"AppCell"];

	self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Sources" style:UIBarButtonItemStylePlain target:self action:@selector(openSources)];
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh target:self action:@selector(refreshRepos)];

	self.searchController = [[UISearchController alloc] initWithSearchResultsController:nil];
	self.searchController.searchResultsUpdater = self;
	self.searchController.obscuresBackgroundDuringPresentation = NO;
	self.searchController.searchBar.placeholder = @"Filter native .ipa repositories...";
	self.navigationItem.searchController = self.searchController;
	self.navigationItem.hidesSearchBarWhenScrolling = NO;
	self.definesPresentationContext = YES;

	UIRefreshControl *rc = [[UIRefreshControl alloc] init];
	[rc addTarget:self action:@selector(refreshRepos) forControlEvents:UIControlEventValueChanged];
	self.refreshControl = rc;

	[[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(reposUpdated) name:SSReposDidChangeNotification object:nil];
	[[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(stateUpdated) name:SSStateDidChangeNotification object:nil];

	[self reposUpdated];
}

- (void)openSources
{
	UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:[[SSSourcesViewController alloc] init]];
	[self presentViewController:nav animated:YES completion:nil];
}

- (void)refreshRepos { [[SSStoreController shared] reloadAllRepos]; }
- (void)stateUpdated { [self.tableView reloadData]; }

- (void)reposUpdated
{
	if (![SSStoreController shared].loadingRepos && self.refreshControl.refreshing) {
		[self.refreshControl endRefreshing];
	}
	[self applyFilter];
}

- (void)updateSearchResultsForSearchController:(UISearchController *)searchController
{
	(void)searchController;
	[self applyFilter];
}

- (void)applyFilter
{
	NSString *q = [self.searchController.searchBar.text stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
	NSArray<SSRepoApp *> *all = [SSStoreController shared].allApps;
	if (q.length == 0) {
		self.filteredApps = all;
	} else {
		NSMutableArray<SSRepoApp *> *out = [NSMutableArray array];
		for (SSRepoApp *a in all) {
			if ([a.name localizedCaseInsensitiveContainsString:q] ||
			    [a.subtitle localizedCaseInsensitiveContainsString:q] ||
			    [a.bundleId localizedCaseInsensitiveContainsString:q] ||
			    [a.developer localizedCaseInsensitiveContainsString:q] ||
			    [a.sourceName localizedCaseInsensitiveContainsString:q]) {
				[out addObject:a];
			}
		}
		self.filteredApps = out;
	}
	UILabel *hdr = [[UILabel alloc] initWithFrame:CGRectMake(0, 0, self.view.bounds.size.width, 34)];
	hdr.font = [UIFont systemFontOfSize:12.5 weight:UIFontWeightMedium];
	hdr.textColor = [UIColor secondaryLabelColor];
	hdr.textAlignment = NSTextAlignmentCenter;
	hdr.text = [SSStoreController shared].loadingRepos
	         ? @"Refreshing native IPA repositories..."
	         : [NSString stringWithFormat:@"%lu native IPAs across %lu sources • De-PAC + Ad-Hoc Signed",
	            (unsigned long)self.filteredApps.count, (unsigned long)[SSStoreController shared].sourceURLs.count];
	self.tableView.tableHeaderView = hdr;
	[self.tableView reloadData];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	(void)tableView; (void)section;
	return (NSInteger)self.filteredApps.count;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	SSAppTableCell *cell = [tableView dequeueReusableCellWithIdentifier:@"AppCell" forIndexPath:indexPath];
	SSRepoApp *app = self.filteredApps[(NSUInteger)indexPath.row];
	NSDictionary *inst = [[SSStoreController shared] installedEntryForBundleId:app.bundleId];
	[cell configureWithRepoApp:app installedEntry:inst];
	__weak typeof(self) weakSelf = self;
	cell.onActionTap = ^{
		__strong typeof(weakSelf) strongSelf = weakSelf;
		if (!strongSelf) return;
		if (inst) {
			[[SSStoreController shared] openBundleId:SSString(inst[@"bundleId"])];
		} else {
			[[SSStoreController shared] downloadAndInstallURL:app.bestVersion.downloadURL title:app.name fromVC:strongSelf];
		}
	};
	return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	SSRepoApp *app = self.filteredApps[(NSUInteger)indexPath.row];
	[self.navigationController pushViewController:[[SSAppDetailViewController alloc] initWithApp:app] animated:YES];
}

@end

/* ---- Tab 3: Install IPA / Wi-Fi Dropzone / Custom WebApp ---- */

@interface SSInstallViewController : UITableViewController <UIDocumentPickerDelegate, UITextFieldDelegate>
@property (nonatomic, strong) UITextField *urlField;
@end

@implementation SSInstallViewController

- (instancetype)init { return [super initWithStyle:UITableViewStyleInsetGrouped]; }

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Install";
	self.navigationController.navigationBar.prefersLargeTitles = YES;

	_urlField = [[UITextField alloc] init];
	_urlField.placeholder = @"https://example.com/App.ipa";
	_urlField.font = [UIFont systemFontOfSize:14.5];
	_urlField.keyboardType = UIKeyboardTypeURL;
	_urlField.autocapitalizationType = UITextAutocapitalizationTypeNone;
	_urlField.autocorrectionType = UITextAutocorrectionTypeNo;
	_urlField.clearButtonMode = UITextFieldViewModeWhileEditing;
	_urlField.returnKeyType = UIReturnKeyGo;
	_urlField.delegate = self;

	[[NSNotificationCenter defaultCenter] addObserver:self.tableView selector:@selector(reloadData) name:SSStateDidChangeNotification object:nil];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField
{
	[textField resignFirstResponder];
	if (textField.text.length > 0) {
		[[SSStoreController shared] downloadAndInstallURL:textField.text title:nil fromVC:self];
	}
	return YES;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { (void)tableView; return 4; }

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	(void)tableView;
	if (section == 0) return 1;
	if (section == 1) return 2;
	if (section == 2) return 3;
	return 1 + (NSInteger)[SSStoreController shared].localIpas.count;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section
{
	(void)tableView;
	if (section == 0) return @"Installer Status";
	if (section == 1) return @"Wi-Fi Browser Upload & WebApp Studio";
	if (section == 2) return @"Install from Direct .ipa Link";
	return @"Install from Device Storage";
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	SSStoreController *sc = [SSStoreController shared];

	if (indexPath.section == 0) {
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
		c.selectionStyle = UITableViewCellSelectionStyleNone;
		c.textLabel.numberOfLines = 0;
		c.detailTextLabel.numberOfLines = 0;
		c.textLabel.font = [UIFont systemFontOfSize:15.5 weight:UIFontWeightSemibold];
		c.detailTextLabel.font = [UIFont systemFontOfSize:13];

		NSString *jState = SSString(sc.daemonJob[@"state"]);
		NSString *jStep  = SSString(sc.daemonJob[@"step"]);
		NSString *jErr   = SSString(sc.daemonJob[@"error"]);
		NSString *jBid   = SSString(sc.daemonJob[@"bundleId"]);
		double jProg     = [sc.daemonJob[@"progress"] respondsToSelector:@selector(doubleValue)] ? [sc.daemonJob[@"progress"] doubleValue] : 0.0;

		if (sc.downloading) {
			NSString *dlText = sc.expectedBytes > 0
			                 ? [NSString stringWithFormat:@"%@ / %@ (%.0f%%)",
			                    SSFormatBytes(sc.downloadedBytes), SSFormatBytes(sc.expectedBytes), sc.downloadProgress * 100.0]
			                 : SSFormatBytes(sc.downloadedBytes);
			c.textLabel.text = [NSString stringWithFormat:@"Downloading %@...", sc.downloadTitle ?: @"IPA"];
			c.detailTextLabel.text = dlText;
			c.detailTextLabel.textColor = [UIColor systemBlueColor];
			UIButton *cancelBtn = [UIButton buttonWithType:UIButtonTypeSystem];
			[cancelBtn setTitle:@"Cancel" forState:UIControlStateNormal];
			[cancelBtn setTitleColor:[UIColor systemRedColor] forState:UIControlStateNormal];
			[cancelBtn sizeToFit];
			[cancelBtn addTarget:sc action:@selector(cancelActiveDownload) forControlEvents:UIControlEventTouchUpInside];
			c.accessoryView = cancelBtn;
		} else if (sc.daemonBusy) {
			c.textLabel.text = [NSString stringWithFormat:@"%@ (%.0f%%)",
			                    SSString(sc.daemonJob[@"appName"]).length > 0 ? SSString(sc.daemonJob[@"appName"]) : @"Installing",
			                    jProg * 100.0];
			c.detailTextLabel.text = jStep.length > 0 ? jStep : @"Working...";
			c.detailTextLabel.textColor = [UIColor systemBlueColor];
			UIActivityIndicatorView *sp = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
			[sp startAnimating];
			c.accessoryView = sp;
		} else if (sc.downloadError.length > 0) {
			c.textLabel.text = @"Installation Error";
			c.textLabel.textColor = [UIColor systemRedColor];
			c.detailTextLabel.text = sc.downloadError;
			c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
		} else if ([jState isEqualToString:@"error"] && jErr.length > 0) {
			c.textLabel.text = @"Installation Failed";
			c.textLabel.textColor = [UIColor systemRedColor];
			c.detailTextLabel.text = jErr;
			c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
		} else if ([jState isEqualToString:@"done"]) {
			c.textLabel.text = jStep.length > 0 ? jStep : @"Installation Complete!";
			c.textLabel.textColor = [UIColor systemGreenColor];
			c.detailTextLabel.text = jBid.length > 0 ? [NSString stringWithFormat:@"%@ • Tap to Open", jBid] : @"Added to Home Screen";
			c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
			if (jBid.length > 0) {
				c.selectionStyle = UITableViewCellSelectionStyleDefault;
				c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
			}
		} else if (!sc.daemonOnline) {
			c.textLabel.text = @"Connecting to hotstored v2...";
			c.textLabel.textColor = [UIColor systemOrangeColor];
			c.detailTextLabel.text = @"Waiting for local installer daemon on 127.0.0.1:29170";
			c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
		} else {
			c.textLabel.text = @"hotstored v2 Ready (iOS 26 De-PAC + AppFix)";
			c.textLabel.textColor = [UIColor systemGreenColor];
			c.detailTextLabel.text = [NSString stringWithFormat:@"%lu installed apps • %lu local .ipa files",
			                          (unsigned long)sc.installedApps.count, (unsigned long)sc.localIpas.count];
			c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
		}
		return c;
	}

	if (indexPath.section == 1) {
		if (indexPath.row == 0) {
			UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
			c.textLabel.text = @"Wi-Fi Browser .ipa Dropzone";
			c.textLabel.font = [UIFont systemFontOfSize:15.5 weight:UIFontWeightSemibold];
			c.detailTextLabel.text = sc.lanEnabled
			                       ? (sc.lanURL.length > 0 ? [NSString stringWithFormat:@"Open %@ on your PC/Mac", sc.lanURL] : @"Active on port 29172")
			                       : @"Upload .ipa files from any browser over Wi-Fi";
			c.detailTextLabel.textColor = sc.lanEnabled ? [UIColor systemGreenColor] : [UIColor secondaryLabelColor];
			UISwitch *sw = [[UISwitch alloc] init];
			sw.on = sc.lanEnabled;
			[sw addTarget:self action:@selector(toggleLanSwitch:) forControlEvents:UIControlEventValueChanged];
			c.accessoryView = sw;
			return c;
		}
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
		c.textLabel.text = @"Create Custom Standalone Web App...";
		c.textLabel.font = [UIFont systemFontOfSize:15.5 weight:UIFontWeightSemibold];
		c.textLabel.textColor = [UIColor systemIndigoColor];
		c.detailTextLabel.text = @"Turn any website into a native Home Screen app";
		c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
		c.imageView.image = [UIImage systemImageNamed:@"globe.badge.chevron.backward"];
		return c;
	}

	if (indexPath.section == 2) {
		if (indexPath.row == 0) {
			UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
			c.selectionStyle = UITableViewCellSelectionStyleNone;
			self.urlField.translatesAutoresizingMaskIntoConstraints = NO;
			[c.contentView addSubview:self.urlField];
			[NSLayoutConstraint activateConstraints:@[
				[self.urlField.leadingAnchor constraintEqualToAnchor:c.contentView.leadingAnchor constant:16],
				[self.urlField.trailingAnchor constraintEqualToAnchor:c.contentView.trailingAnchor constant:-16],
				[self.urlField.topAnchor constraintEqualToAnchor:c.contentView.topAnchor constant:11],
				[self.urlField.bottomAnchor constraintEqualToAnchor:c.contentView.bottomAnchor constant:-11],
				[self.urlField.heightAnchor constraintGreaterThanOrEqualToConstant:24],
			]];
			return c;
		}
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
		c.textLabel.font = [UIFont systemFontOfSize:15.5 weight:UIFontWeightSemibold];
		c.textLabel.textColor = [UIColor systemBlueColor];
		c.textLabel.text = (indexPath.row == 1) ? @"Download & Install URL" : @"Paste Clipboard URL & Install";
		return c;
	}

	if (indexPath.row == 0) {
		UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
		c.textLabel.text = @"Choose .ipa File from Files / Downloads...";
		c.textLabel.font = [UIFont systemFontOfSize:15.5 weight:UIFontWeightSemibold];
		c.textLabel.textColor = [UIColor systemBlueColor];
		c.imageView.image = [UIImage systemImageNamed:@"folder.badge.plus"];
		return c;
	}

	UITableViewCell *c = [tableView dequeueReusableCellWithIdentifier:@"LocalIpa"];
	if (!c) c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"LocalIpa"];
	NSDictionary *item = sc.localIpas[(NSUInteger)(indexPath.row - 1)];
	c.textLabel.text = SSString(item[@"name"]);
	c.textLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
	long long sz = [item[@"size"] respondsToSelector:@selector(longLongValue)] ? [item[@"size"] longLongValue] : 0;
	c.detailTextLabel.text = [NSString stringWithFormat:@"%@ • %@", SSFormatBytes(sz), SSString(item[@"path"])];
	c.detailTextLabel.textColor = [UIColor secondaryLabelColor];
	c.imageView.image = [UIImage systemImageNamed:@"shippingbox.fill"];
	c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
	return c;
}

- (void)toggleLanSwitch:(UISwitch *)sw
{
	[[SSStoreController shared] setLanUploadEnabled:sw.isOn completion:nil];
}

- (void)promptCustomWebApp
{
	UIAlertController *ac = [UIAlertController alertControllerWithTitle:@"Create Standalone Web App"
	                                                            message:@"Installs a real full-screen 20:9 WKWebView app onto your Home Screen:"
	                                                     preferredStyle:UIAlertControllerStyleAlert];
	[ac addTextFieldWithConfigurationHandler:^(UITextField *tf) {
		tf.placeholder = @"App Name (e.g. YouTube Studio)";
	}];
	[ac addTextFieldWithConfigurationHandler:^(UITextField *tf) {
		tf.placeholder = @"https://example.com";
		tf.keyboardType = UIKeyboardTypeURL;
		tf.autocapitalizationType = UITextAutocapitalizationTypeNone;
		tf.autocorrectionType = UITextAutocorrectionTypeNo;
	}];
	[ac addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
	[ac addAction:[UIAlertAction actionWithTitle:@"Install (Mobile)" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		[[SSStoreController shared] createWebAppWithName:ac.textFields[0].text
		                                             url:ac.textFields[1].text
		                                       userAgent:@"mobile"
		                                         iconURL:@""
		                                    origBundleId:@""
		                                       switchTab:NO];
	}]];
	[ac addAction:[UIAlertAction actionWithTitle:@"Install (Desktop)" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		[[SSStoreController shared] createWebAppWithName:ac.textFields[0].text
		                                             url:ac.textFields[1].text
		                                       userAgent:@"desktop"
		                                         iconURL:@""
		                                    origBundleId:@""
		                                       switchTab:NO];
	}]];
	[self presentViewController:ac animated:YES completion:nil];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	SSStoreController *sc = [SSStoreController shared];

	if (indexPath.section == 0) {
		NSString *jState = SSString(sc.daemonJob[@"state"]);
		NSString *jBid = SSString(sc.daemonJob[@"bundleId"]);
		if ([jState isEqualToString:@"done"] && jBid.length > 0) {
			[sc openBundleId:jBid];
		}
		return;
	}
	if (indexPath.section == 1) {
		if (indexPath.row == 0 && sc.lanURL.length > 0) {
			[UIPasteboard generalPasteboard].string = sc.lanURL;
			UIAlertController *ac = [UIAlertController alertControllerWithTitle:@"Copied Upload URL"
			                                                            message:sc.lanURL
			                                                     preferredStyle:UIAlertControllerStyleAlert];
			[ac addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
			[self presentViewController:ac animated:YES completion:nil];
		} else if (indexPath.row == 1) {
			[self promptCustomWebApp];
		}
		return;
	}
	if (indexPath.section == 2) {
		if (indexPath.row == 1) {
			[self.urlField resignFirstResponder];
			[sc downloadAndInstallURL:self.urlField.text title:nil fromVC:self];
		} else if (indexPath.row == 2) {
			NSString *clip = [UIPasteboard generalPasteboard].string;
			if (clip.length > 0) self.urlField.text = clip;
			[sc downloadAndInstallURL:clip title:nil fromVC:self];
		}
		return;
	}
	if (indexPath.row == 0) {
		UTType *ipaType = [UTType typeWithFilenameExtension:@"ipa"] ?: UTTypeData;
		UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ipaType, UTTypeArchive, UTTypeData] asCopy:YES];
		picker.delegate = self;
		[self presentViewController:picker animated:YES completion:nil];
		return;
	}

	NSDictionary *item = sc.localIpas[(NSUInteger)(indexPath.row - 1)];
	[sc installLocalFile:SSString(item[@"path"]) deleteAfter:NO title:[SSString(item[@"name"]) stringByDeletingPathExtension]];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
	(void)controller;
	if (urls.firstObject) {
		[[SSStoreController shared] importExternalFileURL:urls.firstObject switchTab:NO];
	}
}

@end

/* ---- AppFix Log Viewer Modal ---- */

@interface SSLogViewController : UIViewController
@property (nonatomic, strong) UITextView *textView;
@end

@implementation SSLogViewController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"AppFix Runtime Log";
	self.view.backgroundColor = [UIColor systemBackgroundColor];
	self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(closeSelf)];
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Clear" style:UIBarButtonItemStylePlain target:self action:@selector(clearLog)];

	self.textView = [[UITextView alloc] initWithFrame:self.view.bounds];
	self.textView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
	self.textView.editable = NO;
	self.textView.font = [UIFont monospacedSystemFontOfSize:11.5 weight:UIFontWeightRegular];
	[self.view addSubview:self.textView];
	[self reloadLog];
}

- (void)closeSelf { [self dismissViewControllerAnimated:YES completion:nil]; }

- (void)reloadLog
{
	[[SSStoreController shared] fetchAppFixLogWithCompletion:^(NSString *log) {
		self.textView.text = log.length > 0 ? log : @"(No appfix runtime events logged yet. Launch any installed third-party app to see container, AppGroup, and Keychain diagnostics here.)";
	}];
}

- (void)clearLog
{
	[[SSStoreController shared] clearAppFixLogWithCompletion:^(__unused BOOL ok) {
		[self reloadLog];
	}];
}

@end

/* ---- Tab 4: Installed Apps & Per-App GPU/Container Manager ---- */

@interface SSInstalledViewController : UITableViewController
@end

@implementation SSInstalledViewController

- (instancetype)init { return [super initWithStyle:UITableViewStyleInsetGrouped]; }

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Installed";
	self.navigationController.navigationBar.prefersLargeTitles = YES;
	self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"AppFix Log" style:UIBarButtonItemStylePlain target:self action:@selector(openLog)];
	self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Rebuild Icons" style:UIBarButtonItemStylePlain target:self action:@selector(rebuildIcons)];
	[[NSNotificationCenter defaultCenter] addObserver:self.tableView selector:@selector(reloadData) name:SSStateDidChangeNotification object:nil];
}

- (void)openLog
{
	UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:[[SSLogViewController alloc] init]];
	[self presentViewController:nav animated:YES completion:nil];
}

- (void)rebuildIcons
{
	self.navigationItem.rightBarButtonItem.enabled = NO;
	[[SSStoreController shared] rebuildIconsWithCompletion:^(BOOL ok) {
		self.navigationItem.rightBarButtonItem.enabled = YES;
		UIAlertController *ac = [UIAlertController alertControllerWithTitle:ok ? @"Home Screen Refreshed" : @"Refresh Failed"
		                                                            message:ok ? @"LaunchServices icon database rebuilt via lsreg -r." : @"Could not run lsreg -r."
		                                                     preferredStyle:UIAlertControllerStyleAlert];
		[ac addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
		[self presentViewController:ac animated:YES completion:nil];
	}];
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	(void)tableView; (void)section;
	return MAX((NSInteger)1, (NSInteger)[SSStoreController shared].installedApps.count);
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section
{
	(void)tableView; (void)section;
	return @"Tap an app to launch, toggle Adreno 640 Metal2 GPU / CPU rendering, or uninstall.";
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	NSArray *apps = [SSStoreController shared].installedApps;
	if (apps.count == 0) {
		UITableViewCell *empty = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
		empty.selectionStyle = UITableViewCellSelectionStyleNone;
		empty.textLabel.text = @"No apps installed yet — check App Store or Repos";
		empty.textLabel.textColor = [UIColor secondaryLabelColor];
		empty.textLabel.textAlignment = NSTextAlignmentCenter;
		return empty;
	}

	UITableViewCell *c = [tableView dequeueReusableCellWithIdentifier:@"InstCell"];
	if (!c) c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"InstCell"];
	NSDictionary *app = apps[(NSUInteger)indexPath.row];
	c.textLabel.text = SSString(app[@"name"]);
	c.textLabel.font = [UIFont systemFontOfSize:16 weight:UIFontWeightSemibold];
	BOOL gpuOn = [app[@"gpuEnabled"] respondsToSelector:@selector(boolValue)] ? [app[@"gpuEnabled"] boolValue] : YES;
	NSString *kind = [SSString(app[@"kind"]) isEqualToString:@"webapp"] ? @"WebApp" : [NSString stringWithFormat:@"v%@", SSString(app[@"version"])];
	c.detailTextLabel.text = [NSString stringWithFormat:@"%@ • %@ • %@",
	                          kind, gpuOn ? @"GPU: Metal2" : @"CPU Mode", SSString(app[@"bundleId"])];
	c.detailTextLabel.textColor = [UIColor secondaryLabelColor];

	NSString *appDir = SSString(app[@"path"]);
	UIImage *bundleIcon = [UIImage imageWithContentsOfFile:[appDir stringByAppendingPathComponent:@"AppIcon60x60@3x.png"]]
	                   ?: [UIImage imageWithContentsOfFile:[appDir stringByAppendingPathComponent:@"AppIcon60x60@2x.png"]]
	                   ?: [[SSIconLoader shared] placeholderIcon];
	c.imageView.image = bundleIcon;
	c.imageView.layer.cornerRadius = 10;
	c.imageView.clipsToBounds = YES;
	c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
	return c;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	NSArray *apps = [SSStoreController shared].installedApps;
	if ((NSUInteger)indexPath.row >= apps.count) return;
	NSDictionary *app = apps[(NSUInteger)indexPath.row];
	NSString *name = SSString(app[@"name"]);
	NSString *bid = SSString(app[@"bundleId"]);
	NSString *dirName = SSString(app[@"dirName"]);
	NSString *execName = SSString(app[@"execName"]);
	BOOL gpuOn = [app[@"gpuEnabled"] respondsToSelector:@selector(boolValue)] ? [app[@"gpuEnabled"] boolValue] : YES;

	UIAlertController *ac = [UIAlertController alertControllerWithTitle:name message:bid preferredStyle:UIAlertControllerStyleActionSheet];
	[ac addAction:[UIAlertAction actionWithTitle:@"Open App" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		[[SSStoreController shared] openBundleId:bid];
	}]];
	if (execName.length > 0) {
		NSString *gpuTitle = gpuOn ? @"Switch to CPU Rendering (Compatibility Mode)" : @"Enable Adreno 640 Metal2 GPU";
		[ac addAction:[UIAlertAction actionWithTitle:gpuTitle style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
			[[SSStoreController shared] setGPUMode:!gpuOn forExecName:execName completion:nil];
		}]];
	}
	[ac addAction:[UIAlertAction actionWithTitle:@"Uninstall (Keep App Data)" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *a) {
		[[SSStoreController shared] uninstallDirName:dirName deleteData:NO completion:nil];
	}]];
	[ac addAction:[UIAlertAction actionWithTitle:@"Uninstall & Delete App Data" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *a) {
		[[SSStoreController shared] uninstallDirName:dirName deleteData:YES completion:nil];
	}]];
	[ac addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
	[self presentViewController:ac animated:YES completion:nil];
}

@end

/* ---- Application Delegate ---- */

@interface SSAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation SSAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
	(void)application; (void)launchOptions;

	UINavigationController *discNav = [[UINavigationController alloc] initWithRootViewController:[[SSDiscoverViewController alloc] initWithStyle:UITableViewStylePlain]];
	discNav.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"App Store" image:[UIImage systemImageNamed:@"bag.fill"] tag:0];

	UINavigationController *reposNav = [[UINavigationController alloc] initWithRootViewController:[[SSBrowseViewController alloc] initWithStyle:UITableViewStylePlain]];
	reposNav.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Repos" image:[UIImage systemImageNamed:@"square.grid.2x2.fill"] tag:1];

	UINavigationController *installNav = [[UINavigationController alloc] initWithRootViewController:[[SSInstallViewController alloc] init]];
	installNav.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Install" image:[UIImage systemImageNamed:@"arrow.down.doc.fill"] tag:2];

	UINavigationController *installedNav = [[UINavigationController alloc] initWithRootViewController:[[SSInstalledViewController alloc] init]];
	installedNav.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Installed" image:[UIImage systemImageNamed:@"checkmark.seal.fill"] tag:3];

	UITabBarController *tabs = [[UITabBarController alloc] init];
	tabs.viewControllers = @[discNav, reposNav, installNav, installedNav];

	[SSStoreController shared].rootTabs = tabs;
	[[SSStoreController shared] startPolling];
	[[SSStoreController shared] reloadAllRepos];

	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	self.window.rootViewController = tabs;
	[self.window makeKeyAndVisible];
	return YES;
}

- (BOOL)application:(UIApplication *)app openURL:(NSURL *)url options:(NSDictionary<UIApplicationOpenURLOptionsKey,id> *)options
{
	(void)app; (void)options;
	if (url.isFileURL) {
		[[SSStoreController shared] importExternalFileURL:url switchTab:YES];
		return YES;
	}
	return NO;
}

@end

int main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([SSAppDelegate class]));
	}
}
