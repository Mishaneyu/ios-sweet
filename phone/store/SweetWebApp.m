/*
 * SweetWebApp.m — Standalone native UIKit + WKWebView app runtime stub ("SweetWebAppStub").
 *
 * Packaged by sweetstored (POST /create-webapp) into real Home Screen .app bundles for
 * commercial apps whose native binaries are FairPlay-encrypted (cryptid = 1).
 *
 * Driven purely by Info.plist keys:
 *   - CFBundleDisplayName / CFBundleName : App title
 *   - SWStartURL                         : Initial HTTPS URL (e.g. https://web.whatsapp.com)
 *   - SWUserAgentMode                    : "desktop" or "mobile"
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <WebKit/WebKit.h>

static NSString *const kDesktopSafariUA =
	@"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
static NSString *const kMobileSafariUA =
	@"Mozilla/5.0 (iPhone; CPU iPhone OS 26_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1";

@interface SWPopupViewController : UIViewController <WKNavigationDelegate, WKUIDelegate>
@property (nonatomic, strong) WKWebView *webView;
- (instancetype)initWithConfiguration:(WKWebViewConfiguration *)config request:(NSURLRequest *)req;
@end

@implementation SWPopupViewController

- (instancetype)initWithConfiguration:(WKWebViewConfiguration *)config request:(NSURLRequest *)req
{
	self = [super init];
	if (self) {
		_webView = [[WKWebView alloc] initWithFrame:CGRectZero configuration:config];
		_webView.navigationDelegate = self;
		_webView.UIDelegate = self;
		_webView.allowsBackForwardNavigationGestures = YES;
		if (req.URL) {
			[_webView loadRequest:req];
		}
	}
	return self;
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = [UIColor systemBackgroundColor];
	self.navigationItem.leftBarButtonItem =
		[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
		                                              target:self
		                                              action:@selector(closePopup)];
	self.webView.translatesAutoresizingMaskIntoConstraints = NO;
	[self.view addSubview:self.webView];
	[NSLayoutConstraint activateConstraints:@[
		[self.webView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
		[self.webView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
		[self.webView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
		[self.webView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
	]];
}

- (void)closePopup
{
	[self dismissViewControllerAnimated:YES completion:nil];
}

- (void)webViewDidClose:(WKWebView *)webView
{
	(void)webView;
	[self dismissViewControllerAnimated:YES completion:nil];
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation
{
	(void)navigation;
	self.title = webView.title.length > 0 ? webView.title : webView.URL.host;
}

@end

@interface SWMainViewController : UIViewController <WKNavigationDelegate, WKUIDelegate>
@property (nonatomic, strong) WKWebView *webView;
@property (nonatomic, strong) UIProgressView *progressView;
@property (nonatomic, strong) UIButton *menuButton;
@property (nonatomic, copy) NSString *homeURLString;
@property (nonatomic, assign) BOOL desktopMode;
@end

@implementation SWMainViewController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = [UIColor systemBackgroundColor];

	NSDictionary *info = [[NSBundle mainBundle] infoDictionary];
	NSString *cfgURL = [info[@"SWStartURL"] isKindOfClass:[NSString class]] ? info[@"SWStartURL"] : @"https://www.apple.com";
	NSString *uaMode = [info[@"SWUserAgentMode"] isKindOfClass:[NSString class]] ? info[@"SWUserAgentMode"] : @"mobile";

	NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
	NSString *savedUA = [ud stringForKey:@"SWUserAgentOverride"];
	if (savedUA.length > 0) {
		uaMode = savedUA;
	}
	self.homeURLString = cfgURL;
	self.desktopMode = [[uaMode lowercaseString] isEqualToString:@"desktop"];

	WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
	cfg.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
	cfg.allowsInlineMediaPlayback = YES;
	cfg.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
	cfg.preferences.javaScriptCanOpenWindowsAutomatically = YES;

	self.webView = [[WKWebView alloc] initWithFrame:CGRectZero configuration:cfg];
	self.webView.translatesAutoresizingMaskIntoConstraints = NO;
	self.webView.navigationDelegate = self;
	self.webView.UIDelegate = self;
	self.webView.allowsBackForwardNavigationGestures = YES;
	self.webView.customUserAgent = self.desktopMode ? kDesktopSafariUA : kMobileSafariUA;

	UIRefreshControl *rc = [[UIRefreshControl alloc] init];
	[rc addTarget:self action:@selector(handlePullRefresh:) forControlEvents:UIControlEventValueChanged];
	self.webView.scrollView.refreshControl = rc;

	self.progressView = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleBar];
	self.progressView.translatesAutoresizingMaskIntoConstraints = NO;
	self.progressView.progressTintColor = [UIColor systemBlueColor];
	self.progressView.hidden = YES;

	self.menuButton = [UIButton buttonWithType:UIButtonTypeSystem];
	self.menuButton.translatesAutoresizingMaskIntoConstraints = NO;
	[self.menuButton setImage:[UIImage systemImageNamed:@"ellipsis.circle.fill"] forState:UIControlStateNormal];
	self.menuButton.tintColor = [[UIColor secondaryLabelColor] colorWithAlphaComponent:0.78];
	self.menuButton.backgroundColor = [[UIColor secondarySystemBackgroundColor] colorWithAlphaComponent:0.72];
	self.menuButton.layer.cornerRadius = 15.0;
	self.menuButton.clipsToBounds = YES;
	[self.menuButton addTarget:self action:@selector(openQuickMenu) forControlEvents:UIControlEventTouchUpInside];

	[self.view addSubview:self.webView];
	[self.view addSubview:self.progressView];
	[self.view addSubview:self.menuButton];

	[NSLayoutConstraint activateConstraints:@[
		[self.webView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
		[self.webView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
		[self.webView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
		[self.webView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],

		[self.progressView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
		[self.progressView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
		[self.progressView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
		[self.progressView.heightAnchor constraintEqualToConstant:2.5],

		[self.menuButton.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor constant:8.0],
		[self.menuButton.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor constant:-10.0],
		[self.menuButton.widthAnchor constraintEqualToConstant:30.0],
		[self.menuButton.heightAnchor constraintEqualToConstant:30.0],
	]];

	[self.webView addObserver:self forKeyPath:@"estimatedProgress" options:NSKeyValueObservingOptionNew context:NULL];

	NSURL *startURL = [NSURL URLWithString:self.homeURLString];
	if (startURL) {
		[self.webView loadRequest:[NSURLRequest requestWithURL:startURL]];
	}
}

- (void)dealloc
{
	@try {
		[self.webView removeObserver:self forKeyPath:@"estimatedProgress"];
	} @catch (__unused NSException *e) {}
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context
{
	if ([keyPath isEqualToString:@"estimatedProgress"]) {
		double p = self.webView.estimatedProgress;
		self.progressView.hidden = (p >= 0.99);
		[self.progressView setProgress:(float)p animated:YES];
		return;
	}
	[super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
}

- (void)handlePullRefresh:(UIRefreshControl *)rc
{
	[self.webView reload];
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.8 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
		[rc endRefreshing];
	});
}

- (void)openQuickMenu
{
	NSString *appTitle = [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleDisplayName"]
	                  ?: [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleName"]
	                  ?: @"Web App";
	UIAlertController *ac = [UIAlertController alertControllerWithTitle:appTitle
	                                                            message:self.webView.URL.absoluteString
	                                                     preferredStyle:UIAlertControllerStyleActionSheet];
	if (self.webView.canGoBack) {
		[ac addAction:[UIAlertAction actionWithTitle:@"Back" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
			[self.webView goBack];
		}]];
	}
	[ac addAction:[UIAlertAction actionWithTitle:@"Reload Page" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		[self.webView reload];
	}]];
	[ac addAction:[UIAlertAction actionWithTitle:@"Go to App Home" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		NSURL *u = [NSURL URLWithString:self.homeURLString];
		if (u) [self.webView loadRequest:[NSURLRequest requestWithURL:u]];
	}]];
	NSString *uaToggleTitle = self.desktopMode ? @"Switch to Mobile Site" : @"Switch to Desktop Site";
	[ac addAction:[UIAlertAction actionWithTitle:uaToggleTitle style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		self.desktopMode = !self.desktopMode;
		[[NSUserDefaults standardUserDefaults] setObject:(self.desktopMode ? @"desktop" : @"mobile") forKey:@"SWUserAgentOverride"];
		self.webView.customUserAgent = self.desktopMode ? kDesktopSafariUA : kMobileSafariUA;
		[self.webView reloadFromOrigin];
	}]];
	[ac addAction:[UIAlertAction actionWithTitle:@"Share / Open in Safari" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		if (!self.webView.URL) return;
		UIActivityViewController *sh = [[UIActivityViewController alloc] initWithActivityItems:@[self.webView.URL] applicationActivities:nil];
		sh.popoverPresentationController.sourceView = self.menuButton;
		[self presentViewController:sh animated:YES completion:nil];
	}]];
	[ac addAction:[UIAlertAction actionWithTitle:@"Clear Website Data" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *a) {
		NSSet *types = [WKWebsiteDataStore allWebsiteDataTypes];
		[[WKWebsiteDataStore defaultDataStore] removeDataOfTypes:types modifiedSince:[NSDate distantPast] completionHandler:^{
			NSURL *u = [NSURL URLWithString:self.homeURLString];
			if (u) [self.webView loadRequest:[NSURLRequest requestWithURL:u]];
		}];
	}]];
	[ac addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
	ac.popoverPresentationController.sourceView = self.menuButton;
	[self presentViewController:ac animated:YES completion:nil];
}

/* ---- WKNavigationDelegate & WKUIDelegate ---- */

- (void)webView:(WKWebView *)webView decidePolicyForNavigationAction:(WKNavigationAction *)navigationAction decisionHandler:(void (^)(WKNavigationActionPolicy))decisionHandler
{
	(void)webView;
	NSURL *u = navigationAction.request.URL;
	NSString *scheme = u.scheme.lowercaseString;
	if (u && scheme.length > 0 &&
	    ![scheme isEqualToString:@"http"] && ![scheme isEqualToString:@"https"] &&
	    ![scheme isEqualToString:@"about"] && ![scheme isEqualToString:@"blob"] &&
	    ![scheme isEqualToString:@"data"] && ![scheme isEqualToString:@"javascript"]) {
		if ([[UIApplication sharedApplication] canOpenURL:u]) {
			[[UIApplication sharedApplication] openURL:u options:@{} completionHandler:nil];
		}
		decisionHandler(WKNavigationActionPolicyCancel);
		return;
	}
	decisionHandler(WKNavigationActionPolicyAllow);
}

- (WKWebView *)webView:(WKWebView *)webView createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration forNavigationAction:(WKNavigationAction *)navigationAction windowFeatures:(WKWindowFeatures *)windowFeatures
{
	(void)webView; (void)windowFeatures;
	if (navigationAction.targetFrame == nil) {
		SWPopupViewController *popup = [[SWPopupViewController alloc] initWithConfiguration:configuration request:navigationAction.request];
		UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:popup];
		[self presentViewController:nav animated:YES completion:nil];
		return popup.webView;
	}
	return nil;
}

- (void)webView:(WKWebView *)webView runJavaScriptAlertPanelWithMessage:(NSString *)message initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(void))completionHandler
{
	(void)webView; (void)frame;
	UIAlertController *ac = [UIAlertController alertControllerWithTitle:nil message:message preferredStyle:UIAlertControllerStyleAlert];
	[ac addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		completionHandler();
	}]];
	[self presentViewController:ac animated:YES completion:nil];
}

- (void)webView:(WKWebView *)webView runJavaScriptConfirmPanelWithMessage:(NSString *)message initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(BOOL))completionHandler
{
	(void)webView; (void)frame;
	UIAlertController *ac = [UIAlertController alertControllerWithTitle:nil message:message preferredStyle:UIAlertControllerStyleAlert];
	[ac addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(__unused UIAlertAction *a) {
		completionHandler(NO);
	}]];
	[ac addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		completionHandler(YES);
	}]];
	[self presentViewController:ac animated:YES completion:nil];
}

- (void)webView:(WKWebView *)webView runJavaScriptTextInputPanelWithPrompt:(NSString *)prompt defaultText:(NSString *)defaultText initiatedByFrame:(WKFrameInfo *)frame completionHandler:(void (^)(NSString *))completionHandler
{
	(void)webView; (void)frame;
	UIAlertController *ac = [UIAlertController alertControllerWithTitle:nil message:prompt preferredStyle:UIAlertControllerStyleAlert];
	[ac addTextFieldWithConfigurationHandler:^(UITextField *tf) {
		tf.text = defaultText;
	}];
	[ac addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(__unused UIAlertAction *a) {
		completionHandler(nil);
	}]];
	[ac addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
		completionHandler(ac.textFields.firstObject.text);
	}]];
	[self presentViewController:ac animated:YES completion:nil];
}

@end

@interface SWAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation SWAppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
	(void)application; (void)launchOptions;
	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	self.window.rootViewController = [[SWMainViewController alloc] init];
	[self.window makeKeyAndVisible];
	return YES;
}

@end

int main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([SWAppDelegate class]));
	}
}
