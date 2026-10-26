// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>

FOUNDATION_EXPORT NSNotificationName const T733ToolsPairingDidChangeNotification;
FOUNDATION_EXPORT NSDictionary *T733ToolsPairingReadStatus(void);
FOUNDATION_EXPORT int T733ToolsPairingRun(void);
FOUNDATION_EXPORT int T733ToolsPairingRequestStop(void);

/* T733ToolsPairingController owns the system's wireless pairing session.
 * The controller starts a session only after a UI or CLI request, retains
 * the returned RPWirelessPairingSession, and ends it on stop or timeout.
 * RemotePairingDevice owns host authentication and the system PIN interface.
 */
@interface T733ToolsPairingController : NSObject
+ (instancetype)sharedController;
- (BOOL)startPairing;
- (void)stopPairing;
@property(nonatomic, readonly) BOOL running;
@end
