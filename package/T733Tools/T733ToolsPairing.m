// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "T733ToolsPairing.h"

#import <CoreFoundation/CoreFoundation.h>
#import <dlfcn.h>
#import <errno.h>
#import <notify.h>
#import <signal.h>
#import <sys/stat.h>
#import <syslog.h>
#import <unistd.h>

NSNotificationName const T733ToolsPairingDidChangeNotification = @"T733ToolsPairingDidChange";
static NSString *const T733PairingStatusPath =
    @"/private/var/mobile/Library/Preferences/org.unixnu.t733.tools.pairing.json";
static const char T733PairingChangedName[] = "org.unixnu.t733.tools.pairing.changed";
static const char T733PairingStopName[] = "org.unixnu.t733.tools.pairing.stop";
static const NSTimeInterval T733PairingDuration = 600;

@protocol T733ToolsNativePairingSession
- (void)endSession;
- (void)registerInvalidationHandlerOnQueue:(dispatch_queue_t)queue
    handler:(dispatch_block_t)handler;
@end

@interface T733ToolsPairingController ()
@property(nonatomic, strong) id<T733ToolsNativePairingSession> session;
@property(nonatomic, strong) dispatch_source_t expirationTimer;
@property(nonatomic, copy) NSString *generation;
@property(nonatomic, strong) NSDate *expirationDate;
@property(nonatomic) BOOL starting;
@property(nonatomic) BOOL commandLineOwner;
@property(nonatomic) int commandLineResult;
@property(nonatomic) int stopToken;
@property(nonatomic) int changedToken;
@end

NSDictionary *T733ToolsPairingReadStatus(void)
{
    NSData *data = [NSData dataWithContentsOfFile:T733PairingStatusPath];
    id decoded = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    if ([decoded isKindOfClass:NSDictionary.class]) {
        return decoded;
    }

    return @{@"state": @"unknown", @"message": @"No pairing status is available."};
}

static BOOL T733ToolsPairingStatusIsActive(NSDictionary *status)
{
    NSString *state = status[@"state"];
    if (![state isEqualToString:@"starting"] && ![state isEqualToString:@"active"]) {
        return NO;
    }

    NSNumber *expires = status[@"expires_at"];
    NSNumber *owner = status[@"pid"];
    if (![expires isKindOfClass:NSNumber.class] ||
        ![owner isKindOfClass:NSNumber.class] ||
        expires.doubleValue <= NSDate.date.timeIntervalSince1970 || owner.intValue <= 0) {
        return NO;
    }

    return kill(owner.intValue, 0) == 0 || errno == EPERM;
}

@implementation T733ToolsPairingController

+ (instancetype)sharedController
{
    static T733ToolsPairingController *controller;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        controller = [[self alloc] init];
    });

    return controller;
}

- (instancetype)init
{
    self = [super init];
    if (self) {
        _stopToken = -1;
        _changedToken = -1;
        __weak T733ToolsPairingController *weakSelf = self;
        notify_register_dispatch(T733PairingStopName, &_stopToken,
            dispatch_get_main_queue(), ^(int token) {
                (void)token;

                [weakSelf endPairingWithState:@"stopped" message:@"Pairing was stopped." error:nil];
            });
        notify_register_dispatch(T733PairingChangedName, &_changedToken,
            dispatch_get_main_queue(), ^(int token) {
                (void)token;

                [[NSNotificationCenter defaultCenter]
                    postNotificationName:T733ToolsPairingDidChangeNotification object:weakSelf];
            });
    }

    return self;
}

- (BOOL)running
{
    return self.starting || self.session != nil ||
        T733ToolsPairingStatusIsActive(T733ToolsPairingReadStatus());
}

- (void)publishState:(NSString *)state message:(NSString *)message error:(NSError *)error
{
    NSMutableDictionary *status = [@{
        @"state": state,
        @"message": message,
        @"pid": @(getpid()),
        @"updated_at": @(NSDate.date.timeIntervalSince1970),
        @"expires_at": @(self.expirationDate.timeIntervalSince1970),
    } mutableCopy];
    if (error) {
        status[@"error_domain"] = error.domain;
        status[@"error_code"] = @(error.code);
        status[@"error_description"] = error.localizedDescription;
    }

    NSError *writeError;
    NSData *data = [NSJSONSerialization dataWithJSONObject:status options:NSJSONWritingPrettyPrinted
        error:&writeError];
    if (data && [data writeToFile:T733PairingStatusPath options:NSDataWritingAtomic error:&writeError]) {
        if (geteuid() == 0) {
            chown(T733PairingStatusPath.fileSystemRepresentation, 501, 501);
        }

        chmod(T733PairingStatusPath.fileSystemRepresentation, 0644);
    } else {
        syslog(LOG_ERR, "T733 pairing status write failed: %s", writeError.description.UTF8String);
    }

    syslog(error ? LOG_ERR : LOG_NOTICE, "T733 pairing state=%s message=%s error=%s",
        state.UTF8String, message.UTF8String, error.description.UTF8String ?: "none");
    notify_post(T733PairingChangedName);
}

- (void)endPairingWithState:(NSString *)state message:(NSString *)message error:(NSError *)error
{
    if (!self.starting && !self.session) {
        return;
    }

    self.generation = NSUUID.UUID.UUIDString;
    self.starting = NO;

    id<T733ToolsNativePairingSession> session = self.session;
    self.session = nil;
    if (self.expirationTimer) {
        dispatch_source_cancel(self.expirationTimer);
        self.expirationTimer = nil;
    }

    [session endSession];
    [self publishState:state message:message error:error];
    self.commandLineResult = error ? 1 : 0;
    if (self.commandLineOwner) {
        CFRunLoopStop(CFRunLoopGetMain());
    }
}

- (BOOL)startPairing
{
    if (self.running) {
        return NO;
    }

    static void *framework;
    if (!framework) {
        framework = dlopen(
            "/System/Library/PrivateFrameworks/RemotePairingDevice.framework/RemotePairingDevice",
            RTLD_NOW | RTLD_LOCAL);
    }

    void (*createSession)(dispatch_queue_t, void (^)(id, NSError *)) = framework
        ? dlsym(framework, "createWirelessPairingSession") : NULL;
    if (!createSession) {
        NSError *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOSYS
            userInfo:@{NSLocalizedDescriptionKey: @"The native pairing API is unavailable."}];
        [self publishState:@"error" message:error.localizedDescription error:error];
        self.commandLineResult = 1;

        return NO;
    }

    self.starting = YES;
    self.generation = NSUUID.UUID.UUIDString;
    self.expirationDate = [NSDate dateWithTimeIntervalSinceNow:T733PairingDuration];
    NSString *generation = self.generation;
    [self publishState:@"starting" message:@"Starting a native pairing session." error:nil];

    __weak T733ToolsPairingController *weakSelf = self;
    self.expirationTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0,
        dispatch_get_main_queue());
    dispatch_source_set_timer(self.expirationTimer,
        dispatch_time(DISPATCH_TIME_NOW, (int64_t)(T733PairingDuration * NSEC_PER_SEC)),
        DISPATCH_TIME_FOREVER, NSEC_PER_SEC);
    dispatch_source_set_event_handler(self.expirationTimer, ^{
        [weakSelf endPairingWithState:@"expired" message:@"The 10 minute pairing session ended."
            error:nil];
    });
    dispatch_resume(self.expirationTimer);

    createSession(dispatch_get_main_queue(), ^(id returnedSession, NSError *error) {
        id<T733ToolsNativePairingSession> session = returnedSession;
        T733ToolsPairingController *owner = weakSelf;
        if (!owner || ![owner.generation isEqualToString:generation]) {
            [session endSession];

            return;
        }

        if (error || !session) {
            NSError *failure = error ?: [NSError errorWithDomain:NSPOSIXErrorDomain code:EIO
                userInfo:@{NSLocalizedDescriptionKey: @"The native pairing service returned no session."}];
            [owner endPairingWithState:@"error" message:failure.localizedDescription error:failure];

            return;
        }

        owner.session = session;
        owner.starting = NO;
        [session registerInvalidationHandlerOnQueue:dispatch_get_main_queue() handler:^{
            T733ToolsPairingController *activeOwner = weakSelf;
            if ([activeOwner.generation isEqualToString:generation]) {
                [activeOwner endPairingWithState:@"invalidated"
                    message:@"The system closed the native pairing session." error:nil];
            }
        }];
        [owner publishState:@"active"
            message:@"Pairing is available for 10 minutes. Follow the system pairing prompts."
            error:nil];
    });

    return YES;
}

- (void)stopPairing
{
    notify_post(T733PairingStopName);
    [self endPairingWithState:@"stopped" message:@"Pairing was stopped." error:nil];
}

@end

int T733ToolsPairingRequestStop(void)
{
    return notify_post(T733PairingStopName) == NOTIFY_STATUS_OK ? 0 : 1;
}

int T733ToolsPairingRun(void)
{
    T733ToolsPairingController *controller = T733ToolsPairingController.sharedController;
    controller.commandLineOwner = YES;
    if (![controller startPairing]) {
        syslog(LOG_ERR, "T733 pairing could not start: %s",
            T733ToolsPairingReadStatus().description.UTF8String);

        return 1;
    }

    if (!controller.starting && !controller.session) {
        return controller.commandLineResult;
    }

    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    dispatch_source_t interrupt = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
        SIGINT, 0, dispatch_get_main_queue());
    dispatch_source_t terminate = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL,
        SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(interrupt, ^{
        [controller stopPairing];
    });
    dispatch_source_set_event_handler(terminate, ^{
        [controller stopPairing];
    });
    dispatch_resume(interrupt);
    dispatch_resume(terminate);

    CFRunLoopSourceContext context = {0};
    CFRunLoopSourceRef source = CFRunLoopSourceCreate(kCFAllocatorDefault, 0, &context);
    CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopDefaultMode);
    CFRunLoopRun();
    CFRunLoopRemoveSource(CFRunLoopGetMain(), source, kCFRunLoopDefaultMode);
    CFRelease(source);

    dispatch_source_cancel(interrupt);
    dispatch_source_cancel(terminate);

    return controller.commandLineResult;
}
