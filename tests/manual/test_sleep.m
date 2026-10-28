// The macOS sleep/wake observer (platform/sleep_macos.m): post the NSWorkspace notifications and check the callback.
#import <AppKit/AppKit.h>

#include <stdio.h>

#include "platform.h"

static int last = -1;
static void cb(bool waking) { last = waking ? 1 : 0; }

int main(void) {
    @autoreleasepool {
        plat_sleep_watch(cb);
        NSNotificationCenter *nc = [[NSWorkspace sharedWorkspace] notificationCenter];
        [nc postNotificationName:NSWorkspaceWillSleepNotification object:nil];
        if (last != 0) { fprintf(stderr, "willSleep: last=%d\n", last); return 1; }
        [nc postNotificationName:NSWorkspaceDidWakeNotification object:nil];
        if (last != 1) { fprintf(stderr, "didWake: last=%d\n", last); return 2; }
        printf("test_sleep: ok\n");
        return 0;
    }
}
