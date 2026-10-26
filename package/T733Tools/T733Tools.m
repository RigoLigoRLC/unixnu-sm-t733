// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <UIKit/UIKit.h>
#import <errno.h>
#import <grp.h>
#import <stdlib.h>
#import <string.h>
#import <syslog.h>
#import <unistd.h>

#import "T733ToolsSettings.h"
#import "T733ToolsPairing.h"

/* T733ToolsViewController displays the saved startup choices and reads the
 * current compositor and BackBoard settings when the app becomes active.
 * Each switch saves its choice before asking the corresponding system API
 * to apply it. The launchd helper invokes the same settings code at boot.
 */
@interface T733ToolsViewController : UITableViewController
@property(nonatomic, strong) NSArray<UISwitch *> *switches;
@property(nonatomic, strong) NSMutableArray<NSString *> *statusMessages;
@property(nonatomic, strong) NSArray<NSString *> *debugServiceValues;
@end

@implementation T733ToolsViewController

- (instancetype)init
{
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    if (self) {
        self.title = @"T733 Tools";
        self.statusMessages = [NSMutableArray arrayWithObjects:@"", @"", nil];
        self.debugServiceValues = @[@"Unknown", @"Unknown", @"Unknown", @"Unknown"];

        NSMutableArray<UISwitch *> *switches = [NSMutableArray array];
        for (NSUInteger index = 0; index < 2; ++index) {
            UISwitch *toggle = [[UISwitch alloc] init];
            toggle.tag = (NSInteger)index;
            toggle.accessibilityIdentifier = index == 0 ? @"compositorHUD" : @"visualizeTouches";
            [toggle addTarget:self action:@selector(optionChanged:)
                forControlEvents:UIControlEventValueChanged];
            [switches addObject:toggle];
        }

        self.switches = switches;
        [[NSNotificationCenter defaultCenter] addObserver:self
            selector:@selector(refreshSettings) name:UIApplicationDidBecomeActiveNotification
            object:nil];
        [[NSNotificationCenter defaultCenter] addObserver:self
            selector:@selector(refreshPairingStatus) name:T733ToolsPairingDidChangeNotification
            object:nil];
        [T733ToolsPairingController sharedController];
    }

    return self;
}

- (void)viewDidLoad
{
    [super viewDidLoad];

    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 76;
    [self refreshSettings];
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)refreshSettings
{
    for (NSUInteger index = 0; index < self.switches.count; ++index) {
        T733ToolOption option = (T733ToolOption)index;
        BOOL desired = T733ToolsDesiredValue(option);
        BOOL actual = NO;
        NSString *error;
        self.switches[index].on = desired;
        if (T733ToolsReadValue(option, &actual, &error)) {
            self.statusMessages[index] = actual == desired
                ? (actual ? @"On now. Restored at every startup." : @"Off now. Restored at every startup.")
                : @"Saved choice differs from the current system state. Toggle to apply it.";
        } else {
            self.statusMessages[index] = error ?: @"The system setting is unavailable.";
        }
    }

    [self refreshDebugServiceStatus];
    [self.tableView reloadData];
}

- (void)refreshDebugServiceStatus
{
    NSURL *url = [NSURL fileURLWithPath:@"/private/var/run/t733-debug-network.json"];
    NSData *data = [NSData dataWithContentsOfURL:url];
    id decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    NSDictionary *status = [decoded isKindOfClass:NSDictionary.class] ? decoded : nil;

    NSDictionary<NSString *, NSString *> *stateNames = @{
        @"starting": @"Starting",
        @"listening": @"Listening",
        @"session": @"Debugging session",
        @"stopped": @"Stopped",
    };
    NSString *state = [status[@"state"] isKindOfClass:NSString.class] ? status[@"state"] : nil;
    NSString *stateText = state ? stateNames[state] : nil;
    NSString *address = [status[@"address"] isKindOfClass:NSString.class]
        ? status[@"address"] : nil;
    NSNumber *port = [status[@"port"] isKindOfClass:NSNumber.class] ? status[@"port"] : nil;
    NSNumber *supervisor = [status[@"supervisor_pid"] isKindOfClass:NSNumber.class]
        ? status[@"supervisor_pid"] : nil;
    NSNumber *debugger = [status[@"debugger_pid"] isKindOfClass:NSNumber.class]
        ? status[@"debugger_pid"] : nil;
    NSString *endpoint = address && port
        ? [NSString stringWithFormat:@"%@:%@", address, port] : @"Unknown";

    self.debugServiceValues = @[
        stateText ?: @"Unknown",
        endpoint,
        supervisor ? supervisor.stringValue : @"Unknown",
        debugger ? debugger.stringValue : @"Unknown",
    ];
}

- (void)optionChanged:(UISwitch *)toggle
{
    T733ToolOption option = (T733ToolOption)toggle.tag;
    if (!T733ToolsSaveDesiredValue(option, toggle.on)) {
        toggle.on = T733ToolsDesiredValue(option);
        self.statusMessages[(NSUInteger)toggle.tag] = @"The choice could not be saved. Try again.";
    } else {
        NSString *error;
        BOOL applied = T733ToolsApplyValue(option, toggle.on, &error);
        self.statusMessages[(NSUInteger)toggle.tag] = applied
            ? (toggle.on ? @"On now. Restored at every startup." : @"Off now. Restored at every startup.")
            : (error ?: @"Saved. The system has not applied this choice yet.");
    }

    NSIndexPath *path = [NSIndexPath indexPathForRow:0 inSection:toggle.tag];
    [self.tableView reloadRowsAtIndexPaths:@[path] withRowAnimation:UITableViewRowAnimationNone];
}

- (void)refreshPairingStatus
{
    [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:3]
        withRowAnimation:UITableViewRowAnimationNone];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView
{
    (void)tableView;

    return 4;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section
{
    (void)tableView;

    if (section == 3) {
        return 4;
    }

    return section == 2 ? (NSInteger)self.debugServiceValues.count : 1;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section
{
    (void)tableView;

    if (section == 3) {
        return @"Native pairing";
    }

    if (section == 2) {
        return @"Debug service";
    }

    return section == 0 ? @"Compositor" : @"Touch input";
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section
{
    (void)tableView;

    if (section == 3) {
        return @"Start a native pairing session before pairing from your Mac. The system presents any PIN prompts. A session lasts up to 10 minutes.";
    }

    if (section == 2) {
        return @"The startup service publishes this status on the device.";
    }

    return section == 0
        ? @"Shows the Core Animation compositor performance HUD. The first startup uses On."
        : @"Shows the system touch contact display. The first startup uses On. Your choices survive reboots.";
}

- (UITableViewCell *)tableView:(UITableView *)tableView
    cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"Setting"];
    if (!cell) {
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
            reuseIdentifier:@"Setting"];
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
    }

    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    UIListContentConfiguration *content = [UIListContentConfiguration subtitleCellConfiguration];
    if (indexPath.section == 3) {
        NSDictionary *status = T733ToolsPairingReadStatus();
        BOOL running = T733ToolsPairingController.sharedController.running;
        if (indexPath.row == 0) {
            content.text = @"Pairing state";
            NSString *message = [status[@"message"] isKindOfClass:NSString.class]
                ? status[@"message"] : @"Pairing status is unavailable.";
            NSString *error = [status[@"error_description"] isKindOfClass:NSString.class]
                ? status[@"error_description"] : nil;
            content.secondaryText = error && ![error isEqualToString:message]
                ? [NSString stringWithFormat:@"%@\n%@", message, error] : message;
        } else if (indexPath.row == 1) {
            content.text = @"Session expires";
            NSNumber *expires = [status[@"expires_at"] isKindOfClass:NSNumber.class]
                ? status[@"expires_at"] : nil;
            content.secondaryText = running && expires
                ? [NSDateFormatter localizedStringFromDate:
                    [NSDate dateWithTimeIntervalSince1970:expires.doubleValue]
                    dateStyle:NSDateFormatterNoStyle timeStyle:NSDateFormatterMediumStyle]
                : @"No active session";
        } else {
            BOOL enabled = indexPath.row == 2 ? !running : running;
            content.text = indexPath.row == 2 ? @"Start pairing" : @"Stop pairing";
            content.textProperties.color = enabled ? self.view.tintColor : UIColor.secondaryLabelColor;
            if (enabled) {
                cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            }
        }

        content.secondaryTextProperties.numberOfLines = 0;
        cell.contentConfiguration = content;
        cell.accessoryView = nil;

        return cell;
    }

    if (indexPath.section == 2) {
        NSArray<NSString *> *labels = @[@"Service", @"Address", @"Supervisor PID", @"debugserver PID"];
        NSUInteger row = (NSUInteger)indexPath.row;
        content.text = labels[row];
        content.secondaryText = self.debugServiceValues[row];
        cell.contentConfiguration = content;
        cell.accessoryView = nil;

        return cell;
    }

    NSUInteger index = (NSUInteger)indexPath.section;
    content.text = index == 0 ? @"Compositor HUD" : @"Show touches";
    content.secondaryText = self.statusMessages[index];
    content.secondaryTextProperties.numberOfLines = 0;
    cell.contentConfiguration = content;
    cell.accessoryView = self.switches[index];

    return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (indexPath.section != 3) {
        return;
    }

    T733ToolsPairingController *pairing = T733ToolsPairingController.sharedController;
    if (indexPath.row == 2 && !pairing.running) {
        [pairing startPairing];
    } else if (indexPath.row == 3 && pairing.running) {
        [pairing stopPairing];
    }

    [self refreshPairingStatus];
}

@end

@interface T733ToolsAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

@implementation T733ToolsAppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
    (void)application;
    (void)launchOptions;

    T733ToolsViewController *controller = [[T733ToolsViewController alloc] init];
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [[UINavigationController alloc]
        initWithRootViewController:controller];
    [self.window makeKeyAndVisible];

    return YES;
}

@end

int main(int argc, char *argv[])
{
    openlog("T733Tools", LOG_PID | LOG_PERROR, LOG_DAEMON);

    BOOL applyAtBoot = argc == 2 && strcmp(argv[1], "--boot-apply") == 0;

    @autoreleasepool {
        if (argc == 2 && strcmp(argv[1], "--pair") == 0) {
            return T733ToolsPairingRun();
        }

        if (argc == 2 && strcmp(argv[1], "--pair-stop") == 0) {
            return T733ToolsPairingRequestStop();
        }

        if (applyAtBoot) {
            return T733ToolsApplyAtBoot();
        }

        if (argc != 1) {
            syslog(LOG_ERR, "T733 Tools received an unsupported command");

            return 2;
        }

        return UIApplicationMain(argc, argv, nil, NSStringFromClass(T733ToolsAppDelegate.class));
    }
}
