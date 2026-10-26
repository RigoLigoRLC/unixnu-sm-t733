// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "T733ToolsSettings.h"

#import <CoreFoundation/CoreFoundation.h>
#import <dlfcn.h>
#import <errno.h>
#import <mach/mach.h>
#import <notify.h>
#import <objc/runtime.h>
#import <syslog.h>
#import <unistd.h>

extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t port, const char *service_name,
    mach_port_t *service_port);
extern kern_return_t bootstrap_look_up_per_user(mach_port_t port,
    const char *service_name, uid_t uid, mach_port_t *service_port);
extern bool xpc_user_sessions_enabled(void);
extern uid_t xpc_user_sessions_get_foreground_uid(int *error);

static CFStringRef const T733ToolsDomain = CFSTR("org.unixnu.t733.tools");
static CFStringRef const T733BackBoardDomain = CFSTR("com.apple.backboardd");
static CFStringRef const T733TouchPreference = CFSTR("BKDigitizerVisualizeTouches");
static NSString *const T733ToolsApplicationPath = @"/private/var/ngpu/apps/T733Tools.app";
static const unsigned T733CompositorHUDOption = 36;

@interface NSObject (T733ToolsPrivateInterfaces)
+ (id)defaultWorkspace;
+ (NSXPCInterface *)XPCInterface;
- (BOOL)registerApplicationDictionary:(NSDictionary *)dictionary;
@end

@interface NSXPCConnection (T733ToolsPrivateConnection)
- (id)_xpcConnection;
@end

@protocol T733ToolsMachServiceInitialization
- (instancetype)initWithMachServiceName:(NSString *)name options:(NSUInteger)options;
@end

@protocol T733ToolsInstallationService
- (void)performPostInstallationRegistration:(NSArray<NSDictionary *> *)applications
    personaUniqueStrings:(NSArray<NSString *> *)personas
    operationUUID:(NSUUID *)operation
    reply:(void (^)(id recordPromise, id pendingSaveToken, NSError *error))reply;
@end

static CFStringRef T733ToolsCopyPreferenceUser(void)
{
    uid_t uid = getuid();
    if (xpc_user_sessions_enabled()) {
        int error = 0;
        uid = xpc_user_sessions_get_foreground_uid(&error);
        if (error) {
            return NULL;
        }
    }

    CFStringRef (*copyUser)(uid_t) = dlsym(RTLD_DEFAULT,
        "_CFPreferencesGetUserIdentifierForUID");
    if (!copyUser) {
        return NULL;
    }

    // CoreFoundation also records the UID used to select the user's cfprefsd.
    return copyUser(uid);
}

static CFStringRef T733ToolsPreferenceKey(T733ToolOption option)
{
    switch (option) {
        case T733ToolOptionCompositorHUD:
            return CFSTR("CompositorHUD");

        case T733ToolOptionVisualizeTouches:
            return CFSTR("VisualizeTouches");
    }

    return CFSTR("CompositorHUD");
}

BOOL T733ToolsDesiredValue(T733ToolOption option)
{
    CFStringRef user = T733ToolsCopyPreferenceUser();
    if (!user) {
        return YES;
    }

    CFPreferencesSynchronize(T733ToolsDomain, user, kCFPreferencesAnyHost);
    CFPropertyListRef stored = CFPreferencesCopyValue(T733ToolsPreferenceKey(option),
        T733ToolsDomain, user, kCFPreferencesAnyHost);
    CFRelease(user);
    BOOL enabled = YES;
    if (stored && CFGetTypeID(stored) == CFBooleanGetTypeID()) {
        enabled = CFBooleanGetValue((CFBooleanRef)stored);
    }

    if (stored) {
        CFRelease(stored);
    }

    return enabled;
}

BOOL T733ToolsSaveDesiredValue(T733ToolOption option, BOOL enabled)
{
    CFStringRef user = T733ToolsCopyPreferenceUser();
    if (!user) {
        return NO;
    }

    CFPreferencesSetValue(T733ToolsPreferenceKey(option),
        enabled ? kCFBooleanTrue : kCFBooleanFalse, T733ToolsDomain,
        user, kCFPreferencesAnyHost);

    BOOL saved = CFPreferencesSynchronize(T733ToolsDomain, user, kCFPreferencesAnyHost);
    CFRelease(user);

    return saved;
}

static mach_port_t T733ToolsCopyRenderServer(NSString **error)
{
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t result;
    if (xpc_user_sessions_enabled()) {
        int sessionError = 0;
        uid_t uid = xpc_user_sessions_get_foreground_uid(&sessionError);
        if (sessionError) {
            if (error) {
                *error = @"The foreground user session is not ready.";
            }

            return MACH_PORT_NULL;
        }

        result = bootstrap_look_up_per_user(bootstrap_port,
            "com.apple.CARenderServer", uid, &port);
    } else {
        result = bootstrap_look_up(bootstrap_port, "com.apple.CARenderServer", &port);
    }

    if (result != KERN_SUCCESS || port == MACH_PORT_NULL) {
        if (error) {
            *error = [NSString stringWithFormat:@"The compositor connection failed (%d).", result];
        }

        return MACH_PORT_NULL;
    }

    return port;
}

static void *T733ToolsQuartzCore(void)
{
    static void *framework;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        framework = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore",
            RTLD_NOW | RTLD_LOCAL);
    });

    return framework;
}

BOOL T733ToolsReadValue(T733ToolOption option, BOOL *enabled, NSString **error)
{
    @try {
        if (option == T733ToolOptionCompositorHUD) {
            void *framework = T733ToolsQuartzCore();
            BOOL (*getOption)(mach_port_t, unsigned) = framework
                ? dlsym(framework, "CARenderServerGetDebugOption") : NULL;
            if (!getOption) {
                if (error) {
                    *error = @"The system HUD interface is unavailable.";
                }

                return NO;
            }

            mach_port_t server = T733ToolsCopyRenderServer(error);
            if (server == MACH_PORT_NULL) {
                return NO;
            }

            *enabled = getOption(server, T733CompositorHUDOption);
            mach_port_deallocate(mach_task_self(), server);

            return YES;
        }

        CFStringRef user = T733ToolsCopyPreferenceUser();
        if (!user) {
            if (error) {
                *error = @"The foreground user's preferences are unavailable.";
            }

            return NO;
        }

        CFPreferencesSynchronize(T733BackBoardDomain, user, kCFPreferencesAnyHost);
        CFPropertyListRef value = CFPreferencesCopyValue(T733TouchPreference,
            T733BackBoardDomain, user, kCFPreferencesAnyHost);
        CFRelease(user);

        *enabled = value && CFGetTypeID(value) == CFBooleanGetTypeID()
            && CFBooleanGetValue((CFBooleanRef)value);
        if (value) {
            CFRelease(value);
        }

        return YES;
    } @catch (NSException *exception) {
        if (error) {
            *error = exception.reason ?: @"The system could not read this setting.";
        }

        return NO;
    }
}

BOOL T733ToolsApplyValue(T733ToolOption option, BOOL enabled, NSString **error)
{
    @try {
        if (option == T733ToolOptionCompositorHUD) {
            void *framework = T733ToolsQuartzCore();
            void (*setOption)(mach_port_t, unsigned, unsigned) = framework
                ? dlsym(framework, "CARenderServerSetDebugOption") : NULL;
            if (!setOption) {
                if (error) {
                    *error = @"The system HUD interface is unavailable.";
                }

                return NO;
            }

            mach_port_t server = T733ToolsCopyRenderServer(error);
            if (server == MACH_PORT_NULL) {
                return NO;
            }

            setOption(server, T733CompositorHUDOption, enabled ? 1 : 0);
            mach_port_deallocate(mach_task_self(), server);
        } else {
            CFStringRef user = T733ToolsCopyPreferenceUser();
            if (!user) {
                if (error) {
                    *error = @"The foreground user's preferences are unavailable.";
                }

                return NO;
            }

            CFPreferencesSetValue(T733TouchPreference,
                enabled ? kCFBooleanTrue : kCFBooleanFalse, T733BackBoardDomain,
                user, kCFPreferencesAnyHost);
            BOOL saved = CFPreferencesSynchronize(T733BackBoardDomain, user,
                kCFPreferencesAnyHost);
            CFRelease(user);
            if (!saved) {
                if (error) {
                    *error = @"The touch display setting could not be saved.";
                }

                return NO;
            }
        }

        BOOL actual = NO;
        if (!T733ToolsReadValue(option, &actual, error)) {
            return NO;
        }

        if (actual != enabled) {
            if (error) {
                *error = @"The choice was saved. The system has not applied it yet.";
            }

            return NO;
        }

        return YES;
    } @catch (NSException *exception) {
        if (error) {
            *error = exception.reason ?: @"The system could not apply this setting.";
        }

        return NO;
    }
}

int T733ToolsApplyAtBoot(void)
{
    if (getuid() != 0 && getuid() != 501) {
        syslog(LOG_ERR, "T733 Tools boot settings require root or mobile; uid=%u", getuid());

        return 2;
    }

    for (unsigned attempt = 0; attempt < 120; ++attempt) {
        @autoreleasepool {
            BOOL complete = YES;
            for (T733ToolOption option = T733ToolOptionCompositorHUD;
                option <= T733ToolOptionVisualizeTouches; ++option) {
                BOOL desired = T733ToolsDesiredValue(option);
                NSString *error;
                if (!T733ToolsApplyValue(option, desired, &error)) {
                    complete = NO;
                    if (attempt == 0 || attempt == 119) {
                        syslog(LOG_NOTICE, "T733 Tools boot option=%lu enabled=%d attempt=%u: %s",
                            (unsigned long)option, desired, attempt + 1, error.UTF8String);
                    }
                }
            }

            if (complete) {
                syslog(LOG_NOTICE, "T733 Tools boot settings applied HUD=%d touches=%d",
                    T733ToolsDesiredValue(T733ToolOptionCompositorHUD),
                    T733ToolsDesiredValue(T733ToolOptionVisualizeTouches));

                return 0;
            }
        }

        sleep(1);
    }

    return 1;
}

/* LaunchServices uses the inherited launchd namespace for its default
 * connection. The boot helper belongs to the system session, so this
 * connection selects the foreground user's session before contacting lsd.
 * The modify service owns application registration and database saving.
 */
static BOOL T733ToolsRegisterForForegroundUser(NSDictionary *registration,
    NSError **error)
{
    BOOL (*sessionsEnabled)(void) = dlsym(RTLD_DEFAULT, "xpc_user_sessions_enabled");
    if (!sessionsEnabled || !sessionsEnabled()) {
        id workspace = [(id)objc_getClass("LSApplicationWorkspace") defaultWorkspace];

        return [workspace registerApplicationDictionary:registration];
    }

    uid_t (*foregroundUser)(int *) = dlsym(RTLD_DEFAULT,
        "xpc_user_sessions_get_foreground_uid");
    void (*setTargetSession)(id, uid_t) = dlsym(RTLD_DEFAULT,
        "xpc_connection_set_target_user_session_uid");
    Class serviceClass = objc_getClass("_LSDModifyService");
    if (!foregroundUser || !setTargetSession ||
        ![serviceClass respondsToSelector:@selector(XPCInterface)]) {
        if (error) {
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:ENOSYS
                userInfo:@{NSLocalizedDescriptionKey: @"LaunchServices user session API is unavailable."}];
        }

        return NO;
    }

    int sessionError = 0;
    uid_t user = foregroundUser(&sessionError);
    if (sessionError != 0) {
        if (error) {
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:sessionError
                userInfo:@{NSLocalizedDescriptionKey: @"The foreground user session is not ready."}];
        }

        return NO;
    }

    NSXPCConnection *connection = (NSXPCConnection *)[
        (id<T733ToolsMachServiceInitialization>)[NSXPCConnection alloc]
        initWithMachServiceName:@"com.apple.lsd.modifydb" options:0];
    connection.remoteObjectInterface = [serviceClass XPCInterface];
    setTargetSession([connection _xpcConnection], user);
    [connection resume];

    __block NSError *registrationError;
    __block BOOL registered = NO;
    id<T733ToolsInstallationService> service = [connection
        synchronousRemoteObjectProxyWithErrorHandler:^(NSError *connectionError) {
            registrationError = connectionError;
        }];
    [service performPostInstallationRegistration:@[registration] personaUniqueStrings:nil
        operationUUID:[NSUUID UUID] reply:^(id recordPromise, id pendingSaveToken, NSError *replyError) {
            (void)pendingSaveToken;

            registered = recordPromise != nil && replyError == nil;
            registrationError = replyError;
        }];

    [connection invalidate];
    if (error) {
        *error = registrationError;
    }

    return registered;
}

int T733ToolsRegisterApplication(void)
{
    void *framework = dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices",
        RTLD_NOW | RTLD_LOCAL);
    Class workspaceClass = objc_getClass("LSApplicationWorkspace");
    if (!framework || ![workspaceClass respondsToSelector:@selector(defaultWorkspace)]) {
        syslog(LOG_ERR, "T733 Tools LaunchServices is unavailable");

        return 1;
    }

    NSString *infoPath = [T733ToolsApplicationPath stringByAppendingPathComponent:@"Info.plist"];
    NSString *entitlementsPath = [T733ToolsApplicationPath
        stringByAppendingPathComponent:@"Entitlements.plist"];
    NSDictionary *info = [NSDictionary dictionaryWithContentsOfFile:infoPath];
    NSDictionary *entitlements = [NSDictionary dictionaryWithContentsOfFile:entitlementsPath];
    if (![info[@"CFBundleIdentifier"] isEqualToString:@"org.unixnu.t733.tools"] || !entitlements) {
        syslog(LOG_ERR, "T733 Tools installation metadata is missing");

        return 1;
    }

    NSMutableDictionary *registration = [info mutableCopy];
    registration[@"ApplicationType"] = @"System";
    registration[@"Path"] = T733ToolsApplicationPath;
    registration[@"CodeInfoIdentifier"] = info[@"CFBundleIdentifier"];
    registration[@"IsContainerized"] = @NO;
    registration[@"IsDeletable"] = @NO;
    registration[@"IsAdHocSigned"] = @YES;
    registration[@"CompatibilityState"] = @0;
    registration[@"LSInstallType"] = @1;
    registration[@"Entitlements"] = entitlements;
    registration[@"EnvironmentVariables"] = @{
        @"CFFIXED_USER_HOME": @"/var/mobile",
        @"HOME": @"/var/mobile",
        @"TMPDIR": @"/var/tmp",
    };

    for (unsigned attempt = 0; attempt < 120; ++attempt) {
        @try {
            NSError *registrationError;
            if (T733ToolsRegisterForForegroundUser(registration, &registrationError)) {
                notify_post("com.apple.mobile.application_installed");
                syslog(LOG_NOTICE, "T733 Tools registered system app at %s",
                    T733ToolsApplicationPath.UTF8String);

                return 0;
            }

            if (attempt == 0 || attempt == 119) {
                syslog(LOG_ERR, "T733 Tools registration attempt=%u error=%s", attempt + 1,
                    registrationError.description.UTF8String ?: "no application record returned");
            }
        } @catch (NSException *exception) {
            syslog(LOG_ERR, "T733 Tools registration exception: %s", exception.reason.UTF8String);
        }

        sleep(1);
    }

    syslog(LOG_ERR, "T733 Tools LaunchServices registration did not succeed");

    return 1;
}
