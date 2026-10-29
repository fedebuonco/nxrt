// Linux sleep/wake notifications: logind/D-Bus is not wired yet; a no-op keeps the runtime independent of a
// desktop session (a Deck in Game Mode has no session bus for this yet).
#include "platform.h"

void plat_sleep_watch(PlatSleepFn fn) {
    (void)fn;
}
