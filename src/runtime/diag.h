// The master diagnostic switch. Android: `adb shell setprop debug.larecomp.diag 1`, exported by LetsGoActivity as
// LARECOMP_DIAG=1 before the runtime loads (anything else, or the property left empty, means off). Every probe that
// writes a file, floods the log, polls the storage or changes what is drawn asks this first: with it off the game
// draws exactly as without the probe. Read once per file that includes it (the environment is set before loading).
#ifndef RT_DIAG_H
#define RT_DIAG_H
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static inline bool rt_diag(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_DIAG");
        on = e && !strcmp(e, "1");
    }
    return on;
}

#endif
