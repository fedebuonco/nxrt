// macOS sleep/wake observations (platform/platform.h): NSWorkspace's notifications, forwarded to the runtime.
#import <AppKit/AppKit.h>

#include "platform.h"

static PlatSleepFn g_fn;

@interface NxSleepWatcher : NSObject
@end

@implementation NxSleepWatcher
- (void)willSleep:(NSNotification *)n {
    (void)n;
    if (g_fn) g_fn(false);
}
- (void)didWake:(NSNotification *)n {
    (void)n;
    if (g_fn) g_fn(true);
}
@end

void plat_sleep_watch(PlatSleepFn fn) {
    static NxSleepWatcher *watcher;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        watcher = [NxSleepWatcher new];
        NSNotificationCenter *nc = [[NSWorkspace sharedWorkspace] notificationCenter];
        [nc addObserver:watcher selector:@selector(willSleep:) name:NSWorkspaceWillSleepNotification object:nil];
        [nc addObserver:watcher selector:@selector(didWake:) name:NSWorkspaceDidWakeNotification object:nil];
    });
    g_fn = fn;
}
