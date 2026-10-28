/*
 * SweetAuth.m — "microG Settings" Apple Account & iCloud Compatibility App
 * for iOS 26.5.2 on RMX1931 / SWEET (/Applications/SweetAuth.app).
 *
 * 4 Tabs talking to local sweetauthd (http://127.0.0.1:29171):
 *   1. Self-Check      — Live subsystem health checklist & sweetauth.log viewer
 *   2. iCloud Sync     — Native Contacts, Calendar, Mail & Notes via App-Specific Password
 *   3. iCloud Web      — Persistent full-screen iCloud.com WebKit container (Drive, Photos, Notes)
 *   4. Settings Bridge — Optional Anisette v1/v3 configuration & akd Settings Sign-In tester
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#import <WebKit/WebKit.h>
#include <objc/message.h>
#include <objc/runtime.h>

static NSString *const kAuthDaemonBase = @"http://127.0.0.1:29171";
static NSString *const kDesktopUA =
	@"Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
static NSString *const kMobileUA =
	@"Mozilla/5.0 (iPhone; CPU iPhone OS 26_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Mobile/15E148 Safari/604.1";

static void SAHttpJSON(NSString *method, NSString *path, NSDictionary *body, void (^done)(NSDictionary *json, NSError *err))
{
	NSURL *u = [NSURL URLWithString:[kAuthDaemonBase stringByAppendingString:path]];
	NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:u];
	req.HTTPMethod = method;
	req.timeoutInterval = 18.0;
	if (body) {
		[req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
		req.HTTPBody = [NSJSONSerialization dataWithJSONObject:body options:0 error:nil];
	}
	[[[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData *data, __unused NSURLResponse *resp, NSError *err) {
		NSDictionary *j = nil;
		if (data.length > 0) {
			id obj = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
			if ([obj isKindOfClass:[NSDictionary class]]) j = (NSDictionary *)obj;
		}
		dispatch_async(dispatch_get_main_queue(), ^{
			if (done) done(j, err);
		});
	}] resume];
}

static void SAOpenBundleID(NSString *bundleID)
{
	Class wsCls = objc_getClass("LSApplicationWorkspace");
	SEL defSel = sel_registerName("defaultWorkspace");
	SEL openSel = sel_registerName("openApplicationWithBundleID:");
	if (wsCls && [wsCls respondsToSelector:defSel]) {
		id ws = ((id (*)(id, SEL))objc_msgSend)((id)wsCls, defSel);
		if (ws && [ws respondsToSelector:openSel]) {
			((BOOL (*)(id, SEL, id))objc_msgSend)(ws, openSel, bundleID);
		}
	}
}

/* ---- Reusable Popup Web Sheet (for account.apple.com & 2FA) ---- */

@interface SAWebSheetController : UIViewController <WKNavigationDelegate, WKUIDelegate>
@property (nonatomic, strong) WKWebView *webView;
@property (nonatomic, copy) NSString *initialURL;
@property (nonatomic, assign) BOOL desktopMode;
@end

@implementation SAWebSheetController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.view.backgroundColor = [UIColor systemBackgroundColor];
	self.navigationItem.leftBarButtonItem =
		[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
		                                              target:self
		                                              action:@selector(closeSheet)];
	self.navigationItem.rightBarButtonItem =
		[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
		                                              target:self
		                                              action:@selector(reloadSheet)];

	WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
	cfg.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
	cfg.preferences.javaScriptCanOpenWindowsAutomatically = YES;

	self.webView = [[WKWebView alloc] initWithFrame:CGRectZero configuration:cfg];
	self.webView.translatesAutoresizingMaskIntoConstraints = NO;
	self.webView.navigationDelegate = self;
	self.webView.UIDelegate = self;
	self.webView.allowsBackForwardNavigationGestures = YES;
	self.webView.customUserAgent = self.desktopMode ? kDesktopUA : kMobileUA;

	[self.view addSubview:self.webView];
	[NSLayoutConstraint activateConstraints:@[
		[self.webView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
		[self.webView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
		[self.webView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
		[self.webView.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
	]];

	NSURL *u = [NSURL URLWithString:self.initialURL ?: @"https://account.apple.com"];
	if (u) [self.webView loadRequest:[NSURLRequest requestWithURL:u]];
}

- (void)closeSheet { [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)reloadSheet { [self.webView reload]; }

@end

/* ---- Tab 1: Self-Check ---- */

@interface SASelfCheckViewController : UITableViewController
@property (nonatomic, strong) NSDictionary *status;
@property (nonatomic, assign) BOOL loading;
@end

@implementation SASelfCheckViewController

- (instancetype)init
{
	return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Self-Check";
	self.navigationController.navigationBar.prefersLargeTitles = YES;
	self.navigationItem.rightBarButtonItem =
		[[UIBarButtonItem alloc] initWithTitle:@"Logs"
		                                 style:UIBarButtonItemStylePlain
		                                target:self
		                                action:@selector(showLogs)];
	UIRefreshControl *rc = [[UIRefreshControl alloc] init];
	[rc addTarget:self action:@selector(runCheck) forControlEvents:UIControlEventValueChanged];
	self.refreshControl = rc;
	[self runCheck];
}

- (void)runCheck
{
	self.loading = YES;
	[self.tableView reloadData];
	SAHttpJSON(@"GET", @"/v1/selfcheck", nil, ^(NSDictionary *json, __unused NSError *err) {
		self.loading = NO;
		[self.refreshControl endRefreshing];
		self.status = json ?: @{ @"ok": @NO };
		[self.tableView reloadData];
	});
}

- (void)showLogs
{
	SAHttpJSON(@"GET", @"/v1/logs", nil, ^(NSDictionary *json, __unused NSError *err) {
		NSString *logText = [json[@"log"] isKindOfClass:[NSString class]] ? json[@"log"] : @"";
		if (logText.length == 0) logText = @"(No entries in /var/local/sweetauth.log yet)";
		UIViewController *vc = [[UIViewController alloc] init];
		vc.title = @"sweetauth.log";
		vc.view.backgroundColor = [UIColor systemBackgroundColor];
		UITextView *tv = [[UITextView alloc] initWithFrame:CGRectZero];
		tv.translatesAutoresizingMaskIntoConstraints = NO;
		tv.editable = NO;
		tv.font = [UIFont monospacedSystemFontOfSize:11.5 weight:UIFontWeightRegular];
		tv.text = logText;
		[vc.view addSubview:tv];
		[NSLayoutConstraint activateConstraints:@[
			[tv.topAnchor constraintEqualToAnchor:vc.view.safeAreaLayoutGuide.topAnchor],
			[tv.leadingAnchor constraintEqualToAnchor:vc.view.leadingAnchor constant:8],
			[tv.trailingAnchor constraintEqualToAnchor:vc.view.trailingAnchor constant:-8],
			[tv.bottomAnchor constraintEqualToAnchor:vc.view.bottomAnchor],
		]];
		UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:vc];
		vc.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(closeLogs)];
		vc.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Clear" style:UIBarButtonItemStylePlain target:self action:@selector(clearLogs)];
		[self presentViewController:nav animated:YES completion:nil];
	});
}

- (void)closeLogs
{
	[self dismissViewControllerAnimated:YES completion:nil];
}

- (void)clearLogs
{
	SAHttpJSON(@"POST", @"/v1/clear-logs", @{}, ^(__unused NSDictionary *j, __unused NSError *e) {
		[self dismissViewControllerAnimated:YES completion:nil];
	});
}

- (NSInteger)numberOfSectionsInTableView:(__unused UITableView *)tableView { return 3; }

- (NSString *)tableView:(__unused UITableView *)tableView titleForHeaderInSection:(NSInteger)section
{
	if (section == 0) return @"Core iOS 26 Subsystems";
	if (section == 1) return @"Apple Cloud & Native Accounts";
	return @"Actions";
}

- (NSInteger)tableView:(__unused UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	if (section == 0) return 5;
	if (section == 1) return 3;
	return 1;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"c"];
	if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"c"];
	cell.selectionStyle = UITableViewCellSelectionStyleNone;
	cell.accessoryType = UITableViewCellAccessoryNone;
	cell.textLabel.textColor = [UIColor labelColor];
	cell.detailTextLabel.numberOfLines = 2;

	NSDictionary *s = self.status;
	BOOL hasDaemon = [s[@"ok"] boolValue];

	if (indexPath.section == 2) {
		cell.selectionStyle = UITableViewCellSelectionStyleDefault;
		cell.textLabel.text = self.loading ? @"Running Self-Check..." : @"Re-Run Self-Check Now";
		cell.textLabel.textColor = [UIColor systemBlueColor];
		cell.detailTextLabel.text = @"Queries sweetauthd on 127.0.0.1:29171";
		return cell;
	}

	NSString *title = @"";
	NSString *sub = @"Checking...";
	BOOL ok = NO;
	BOOL amber = NO;

	if (!hasDaemon) {
		title = @"sweetauthd Daemon";
		sub = self.loading ? @"Querying 127.0.0.1:29171..." : @"Daemon unreachable on 127.0.0.1:29171";
	} else if (indexPath.section == 0) {
		if (indexPath.row == 0) {
			title = @"MobileActivation";
			ok = [s[@"activation_ok"] boolValue];
			sub = [NSString stringWithFormat:@"State: %@", s[@"activation_state"] ?: @"?"];
		} else if (indexPath.row == 1) {
			title = @"SweetKeyStore 512 B Keychain";
			ok = [s[@"keychain_512b_ok"] boolValue];
			sub = [NSString stringWithFormat:@"SecItemAdd/CopyMatching/Delete status=%@", s[@"keychain_status"] ?: @"?"];
		} else if (indexPath.row == 2) {
			title = @"System Clock & TLS Epoch";
			ok = [s[@"clock_ok"] boolValue];
			sub = [NSString stringWithFormat:@"Epoch: %@", s[@"clock_epoch"] ?: @"0"];
		} else if (indexPath.row == 3) {
			title = @"SysFix (akd + cdpd Hooks)";
			ok = [s[@"sysfix_akd_cdpd_ok"] boolValue];
			sub = ok ? @"libsweet_sysfix.dylib active in inject.conf" : @"Missing akd/cdpd in /var/local/inject.conf";
		} else {
			title = @"AppFix (@userapps Container & Keychain)";
			ok = [s[@"appfix_userapps_ok"] boolValue];
			sub = ok ? @"libsweet_appfix.dylib active for 3rd-party apps" : @"Missing @userapps in /var/local/inject.conf";
		}
	} else {
		if (indexPath.row == 0) {
			title = @"Apple GrandSlam & iCloud TLS";
			ok = [s[@"tls_ok"] boolValue];
			sub = [NSString stringWithFormat:@"gsa.apple.com HTTP %@ • icloud.com HTTP %@",
			       s[@"gsa_http"] ?: @"0", s[@"icloud_http"] ?: @"0"];
		} else if (indexPath.row == 1) {
			title = @"Native Sync Accounts (accountsd)";
			NSInteger cnt = [s[@"accounts_count"] integerValue];
			ok = (cnt > 0);
			amber = (cnt == 0);
			NSArray *list = [s[@"accounts"] isKindOfClass:[NSArray class]] ? s[@"accounts"] : @[];
			sub = cnt > 0 ? [list componentsJoinedByString:@", "] : @"0 accounts — add via iCloud Sync tab";
		} else {
			title = @"Settings Sign-In Bridge (Anisette)";
			BOOL en = [s[@"anisette_enabled"] boolValue];
			BOOL pr = [s[@"anisette_provisioned"] boolValue];
			ok = en;
			amber = !en;
			sub = en ? [NSString stringWithFormat:@"Enabled (%@) • %@", s[@"anisette_mode"] ?: @"v3", pr ? @"ADI provisioned" : @"Ready"]
			         : @"Optional — configure in Settings Bridge tab";
		}
	}

	NSString *badge = ok ? @"✓ " : (amber ? @"• " : @"✕ ");
	cell.textLabel.text = [badge stringByAppendingString:title];
	cell.detailTextLabel.text = sub;
	return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	if (indexPath.section == 2) [self runCheck];
}

@end

/* ---- Tab 2: iCloud Sync (App-Specific Password) ---- */

@interface SAICloudSyncViewController : UITableViewController
@property (nonatomic, strong) UITextField *emailField;
@property (nonatomic, strong) UITextField *pwField;
@property (nonatomic, strong) UISwitch *contactsSwitch;
@property (nonatomic, strong) UISwitch *calendarSwitch;
@property (nonatomic, strong) UISwitch *mailSwitch;
@property (nonatomic, assign) BOOL busy;
@end

@implementation SAICloudSyncViewController

- (instancetype)init { return [super initWithStyle:UITableViewStyleInsetGrouped]; }

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"iCloud Sync";
	self.navigationController.navigationBar.prefersLargeTitles = YES;

	self.emailField = [[UITextField alloc] initWithFrame:CGRectMake(0, 0, 260, 36)];
	self.emailField.placeholder = @"name@icloud.com";
	self.emailField.keyboardType = UIKeyboardTypeEmailAddress;
	self.emailField.autocapitalizationType = UITextAutocapitalizationTypeNone;
	self.emailField.autocorrectionType = UITextAutocorrectionTypeNo;

	self.pwField = [[UITextField alloc] initWithFrame:CGRectMake(0, 0, 260, 36)];
	self.pwField.placeholder = @"xxxx-xxxx-xxxx-xxxx";
	self.pwField.secureTextEntry = YES;
	self.pwField.autocapitalizationType = UITextAutocapitalizationTypeNone;
	self.pwField.autocorrectionType = UITextAutocorrectionTypeNo;

	self.contactsSwitch = [[UISwitch alloc] init]; self.contactsSwitch.on = YES;
	self.calendarSwitch = [[UISwitch alloc] init]; self.calendarSwitch.on = YES;
	self.mailSwitch     = [[UISwitch alloc] init]; self.mailSwitch.on = YES;
}

- (NSInteger)numberOfSectionsInTableView:(__unused UITableView *)tableView { return 3; }

- (NSString *)tableView:(__unused UITableView *)tableView titleForHeaderInSection:(NSInteger)section
{
	if (section == 0) return @"Apple ID & App-Specific Password (100% On-Device)";
	if (section == 1) return @"Stock iOS Apps to Sync";
	return @"Setup";
}

- (NSString *)tableView:(__unused UITableView *)tableView titleForFooterInSection:(NSInteger)section
{
	if (section == 0) {
		return @"Safe for your primary Apple ID: uses Apple's official CardDAV, CalDAV, and IMAP endpoints with an App-Specific Password (no Anisette server needed).";
	}
	return nil;
}

- (NSInteger)tableView:(__unused UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	if (section == 0) return 3;
	if (section == 1) return 3;
	return 1;
}

- (UITableViewCell *)tableView:(__unused UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
	cell.selectionStyle = UITableViewCellSelectionStyleNone;

	if (indexPath.section == 0) {
		if (indexPath.row == 0) {
			cell.textLabel.text = @"Apple ID";
			cell.accessoryView = self.emailField;
		} else if (indexPath.row == 1) {
			cell.textLabel.text = @"App Password";
			cell.accessoryView = self.pwField;
		} else {
			cell.selectionStyle = UITableViewCellSelectionStyleDefault;
			cell.textLabel.text = @"Generate Password on account.apple.com";
			cell.textLabel.textColor = [UIColor systemBlueColor];
			cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
		}
	} else if (indexPath.section == 1) {
		if (indexPath.row == 0) {
			cell.textLabel.text = @"Contacts (CardDAV)";
			cell.accessoryView = self.contactsSwitch;
		} else if (indexPath.row == 1) {
			cell.textLabel.text = @"Calendar & Reminders (CalDAV)";
			cell.accessoryView = self.calendarSwitch;
		} else {
			cell.textLabel.text = @"Mail & Notes (IMAP/SMTP)";
			cell.accessoryView = self.mailSwitch;
		}
	} else {
		cell.selectionStyle = UITableViewCellSelectionStyleDefault;
		cell.textLabel.text = self.busy ? @"Verifying & Provisioning..." : @"Verify & Add iCloud Accounts";
		cell.textLabel.textColor = [UIColor systemGreenColor];
		cell.textLabel.font = [UIFont boldSystemFontOfSize:16.5];
	}
	return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	if (indexPath.section == 0 && indexPath.row == 2) {
		SAWebSheetController *ws = [[SAWebSheetController alloc] init];
		ws.title = @"Apple Account";
		ws.initialURL = @"https://account.apple.com/sign-in";
		ws.desktopMode = NO;
		UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:ws];
		[self presentViewController:nav animated:YES completion:nil];
		return;
	}
	if (indexPath.section == 2 && !self.busy) {
		[self.view endEditing:YES];
		self.busy = YES;
		[self.tableView reloadData];
		NSDictionary *payload = @{
			@"email": self.emailField.text ?: @"",
			@"appPassword": self.pwField.text ?: @"",
			@"enableContacts": @(self.contactsSwitch.isOn),
			@"enableCalendar": @(self.calendarSwitch.isOn),
			@"enableMail": @(self.mailSwitch.isOn),
		};
		SAHttpJSON(@"POST", @"/v1/setup-icloud-dav", payload, ^(NSDictionary *json, NSError *err) {
			self.busy = NO;
			[self.tableView reloadData];
			BOOL ok = [json[@"ok"] boolValue];
			NSString *msg = ok
				? [NSString stringWithFormat:@"Provisioned: %@", [(NSArray *)(json[@"provisioned"] ?: @[]) componentsJoinedByString:@", "]]
				: (json[@"error"] ?: err.localizedDescription ?: @"Request failed");
			UIAlertController *ac = [UIAlertController alertControllerWithTitle:(ok ? @"iCloud Sync Ready" : @"Setup Failed")
			                                                            message:msg
			                                                     preferredStyle:UIAlertControllerStyleAlert];
			[ac addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
			[self presentViewController:ac animated:YES completion:nil];
		});
	}
}

@end

/* ---- Tab 3: iCloud Web (icloud.com) ---- */

@interface SAICloudWebViewController : UIViewController <WKNavigationDelegate, WKUIDelegate>
@property (nonatomic, strong) WKWebView *webView;
@property (nonatomic, assign) BOOL desktopMode;
@end

@implementation SAICloudWebViewController

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"iCloud Web";
	self.view.backgroundColor = [UIColor systemBackgroundColor];
	self.desktopMode = YES;

	self.navigationItem.leftBarButtonItem =
		[[UIBarButtonItem alloc] initWithTitle:@"Desktop UA"
		                                 style:UIBarButtonItemStylePlain
		                                target:self
		                                action:@selector(toggleUA)];
	self.navigationItem.rightBarButtonItem =
		[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
		                                              target:self
		                                              action:@selector(reloadWeb)];

	WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
	cfg.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
	cfg.allowsInlineMediaPlayback = YES;
	cfg.preferences.javaScriptCanOpenWindowsAutomatically = YES;

	self.webView = [[WKWebView alloc] initWithFrame:CGRectZero configuration:cfg];
	self.webView.translatesAutoresizingMaskIntoConstraints = NO;
	self.webView.navigationDelegate = self;
	self.webView.UIDelegate = self;
	self.webView.allowsBackForwardNavigationGestures = YES;
	self.webView.customUserAgent = kDesktopUA;

	[self.view addSubview:self.webView];
	[NSLayoutConstraint activateConstraints:@[
		[self.webView.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
		[self.webView.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
		[self.webView.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
		[self.webView.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor],
	]];

	[self.webView loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:@"https://www.icloud.com"]]];
}

- (void)toggleUA
{
	self.desktopMode = !self.desktopMode;
	self.navigationItem.leftBarButtonItem.title = self.desktopMode ? @"Desktop UA" : @"Mobile UA";
	self.webView.customUserAgent = self.desktopMode ? kDesktopUA : kMobileUA;
	[self.webView reloadFromOrigin];
}

- (void)reloadWeb { [self.webView reload]; }

- (WKWebView *)webView:(__unused WKWebView *)webView createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration forNavigationAction:(WKNavigationAction *)navigationAction windowFeatures:(__unused WKWindowFeatures *)windowFeatures
{
	if (navigationAction.targetFrame == nil && navigationAction.request.URL) {
		[self.webView loadRequest:navigationAction.request];
	}
	(void)configuration;
	return nil;
}

@end

/* ---- Tab 4: Settings Sign-In Bridge (Anisette v1/v3) ---- */

@interface SASettingsBridgeViewController : UITableViewController
@property (nonatomic, strong) UISwitch *enableSwitch;
@property (nonatomic, strong) UISegmentedControl *modeSeg;
@property (nonatomic, strong) UITextField *urlField;
@property (nonatomic, copy) NSString *lastTestResult;
@end

@implementation SASettingsBridgeViewController

- (instancetype)init { return [super initWithStyle:UITableViewStyleInsetGrouped]; }

- (void)viewDidLoad
{
	[super viewDidLoad];
	self.title = @"Settings Bridge";
	self.navigationController.navigationBar.prefersLargeTitles = YES;

	self.enableSwitch = [[UISwitch alloc] init];
	self.modeSeg = [[UISegmentedControl alloc] initWithItems:@[@"v3 (Provisioned)", @"v1 (Headers)"]];
	self.modeSeg.selectedSegmentIndex = 0;

	self.urlField = [[UITextField alloc] initWithFrame:CGRectMake(0, 0, 235, 36)];
	self.urlField.placeholder = @"https://your-anisette-host";
	self.urlField.keyboardType = UIKeyboardTypeURL;
	self.urlField.autocapitalizationType = UITextAutocapitalizationTypeNone;
	self.urlField.autocorrectionType = UITextAutocorrectionTypeNo;

	[self loadConfig];
}

- (void)loadConfig
{
	SAHttpJSON(@"GET", @"/v1/config", nil, ^(NSDictionary *j, __unused NSError *e) {
		if (!j) return;
		self.enableSwitch.on = [j[@"enabled"] boolValue];
		NSString *m = [j[@"mode"] isKindOfClass:[NSString class]] ? j[@"mode"] : @"v3";
		self.modeSeg.selectedSegmentIndex = [[m lowercaseString] isEqualToString:@"v1"] ? 1 : 0;
		self.urlField.text = [j[@"url"] isKindOfClass:[NSString class]] ? j[@"url"] : @"";
		[self.tableView reloadData];
	});
}

- (NSInteger)numberOfSectionsInTableView:(__unused UITableView *)tableView { return 2; }

- (NSString *)tableView:(__unused UITableView *)tableView titleForHeaderInSection:(NSInteger)section
{
	return section == 0 ? @"akd Anisette Provider" : @"Test & Launch Settings";
}

- (NSString *)tableView:(__unused UITableView *)tableView titleForFooterInSection:(NSInteger)section
{
	if (section == 0) {
		return @"Account hygiene: use a secondary/test Apple ID and your own self-hosted or trusted v3 Anisette endpoint when signing into Settings -> Sign in to your iPhone.";
	}
	return self.lastTestResult;
}

- (NSInteger)tableView:(__unused UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
	return section == 0 ? 3 : 3;
}

- (UITableViewCell *)tableView:(__unused UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
	cell.selectionStyle = UITableViewCellSelectionStyleNone;

	if (indexPath.section == 0) {
		if (indexPath.row == 0) {
			cell.textLabel.text = @"Enable akd Bridge";
			cell.accessoryView = self.enableSwitch;
		} else if (indexPath.row == 1) {
			cell.textLabel.text = @"Protocol";
			cell.accessoryView = self.modeSeg;
		} else {
			cell.textLabel.text = @"Server URL";
			cell.accessoryView = self.urlField;
		}
	} else {
		cell.selectionStyle = UITableViewCellSelectionStyleDefault;
		if (indexPath.row == 0) {
			cell.textLabel.text = @"Save & Test Provisioning";
			cell.textLabel.textColor = [UIColor systemBlueColor];
		} else if (indexPath.row == 1) {
			cell.textLabel.text = @"Open iOS Settings";
			cell.textLabel.textColor = [UIColor systemGreenColor];
		} else {
			cell.textLabel.text = @"Erase Cached ADI Machine State";
			cell.textLabel.textColor = [UIColor systemRedColor];
		}
	}
	return cell;
}

- (void)saveConfigErase:(BOOL)erase done:(void (^)(void))done
{
	[self.view endEditing:YES];
	NSDictionary *body = @{
		@"enabled": @(self.enableSwitch.isOn),
		@"mode": (self.modeSeg.selectedSegmentIndex == 1 ? @"v1" : @"v3"),
		@"url": self.urlField.text ?: @"",
		@"eraseAdi": @(erase),
	};
	SAHttpJSON(@"POST", @"/v1/config", body, ^(__unused NSDictionary *j, __unused NSError *e) {
		if (done) done();
	});
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	if (indexPath.section != 1) return;

	if (indexPath.row == 0) {
		self.lastTestResult = @"Saving config & testing Anisette...";
		[self.tableView reloadData];
		[self saveConfigErase:NO done:^{
			SAHttpJSON(@"GET", @"/v1/anisette?op=fetch_prov", nil, ^(NSDictionary *j, NSError *e) {
				if ([j[@"ok"] boolValue]) {
					NSString *mid = j[@"machineID"] ?: @"";
					self.lastTestResult = [NSString stringWithFormat:@"✓ Provisioning succeeded! machineID=%@... rinfo=%@",
					    mid.length > 12 ? [mid substringToIndex:12] : mid, j[@"routingInfo"] ?: @"17106176"];
				} else {
					self.lastTestResult = [NSString stringWithFormat:@"✕ %@", j[@"error"] ?: e.localizedDescription ?: @"Failed"];
				}
				[self.tableView reloadData];
			});
		}];
	} else if (indexPath.row == 1) {
		[self saveConfigErase:NO done:^{
			SAOpenBundleID(@"com.apple.Preferences");
		}];
	} else {
		[self saveConfigErase:YES done:^{
			self.lastTestResult = @"Cleared cached ADI state (/var/local/sweetauth/adi_state.plist).";
			[self.tableView reloadData];
		}];
	}
}

@end

/* ---- App Delegate & Root Tab Bar ---- */

@interface SAAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation SAAppDelegate

- (BOOL)application:(__unused UIApplication *)application didFinishLaunchingWithOptions:(__unused NSDictionary *)launchOptions
{
	UINavigationController *t1 = [[UINavigationController alloc] initWithRootViewController:[[SASelfCheckViewController alloc] init]];
	t1.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Self-Check" image:[UIImage systemImageNamed:@"checkmark.shield.fill"] tag:0];

	UINavigationController *t2 = [[UINavigationController alloc] initWithRootViewController:[[SAICloudSyncViewController alloc] init]];
	t2.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"iCloud Sync" image:[UIImage systemImageNamed:@"arrow.triangle.2.circlepath.icloud.fill"] tag:1];

	UINavigationController *t3 = [[UINavigationController alloc] initWithRootViewController:[[SAICloudWebViewController alloc] init]];
	t3.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"iCloud Web" image:[UIImage systemImageNamed:@"icloud.fill"] tag:2];

	UINavigationController *t4 = [[UINavigationController alloc] initWithRootViewController:[[SASettingsBridgeViewController alloc] init]];
	t4.tabBarItem = [[UITabBarItem alloc] initWithTitle:@"Settings Bridge" image:[UIImage systemImageNamed:@"key.fill"] tag:3];

	UITabBarController *tabs = [[UITabBarController alloc] init];
	tabs.viewControllers = @[t1, t2, t3, t4];

	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	self.window.rootViewController = tabs;
	[self.window makeKeyAndVisible];
	return YES;
}

@end

int main(int argc, char *argv[])
{
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([SAAppDelegate class]));
	}
}
