// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>

typedef NS_ENUM(NSUInteger, T733ToolOption) {
    T733ToolOptionCompositorHUD,
    T733ToolOptionVisualizeTouches,
};

FOUNDATION_EXPORT BOOL T733ToolsDesiredValue(T733ToolOption option);
FOUNDATION_EXPORT BOOL T733ToolsSaveDesiredValue(T733ToolOption option, BOOL enabled);
FOUNDATION_EXPORT BOOL T733ToolsReadValue(T733ToolOption option, BOOL *enabled,
    NSString **error);
FOUNDATION_EXPORT BOOL T733ToolsApplyValue(T733ToolOption option, BOOL enabled,
    NSString **error);
FOUNDATION_EXPORT int T733ToolsApplyAtBoot(void);
FOUNDATION_EXPORT int T733ToolsRegisterApplication(void);
