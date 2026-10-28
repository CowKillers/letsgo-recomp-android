// The game in the background. Android (apk/java/.../LetsGoActivity.java onPause/onResume -> platform/android.c)
// says when the activity leaves the foreground; the game's present (nvn/objects.c nvnQueuePresentTexture) then waits
// here, and the game's other threads at their next SleepThread / yield (kernel.c), so nothing computes until the
// activity is in front again. Measured before (Fold7, 28/10): ~10 400 frames computed in the 6 min after the player went home, until
// Android killed the app for "excessive cpu".
#ifndef RT_LIFECYCLE_H
#define RT_LIFECYCLE_H
#include <stdbool.h>
#include <stdint.h>

void rt_set_background(bool background);  // any thread (Android UI thread)
bool rt_in_background(void);
void rt_background_wait(void);             // present + guest SleepThread: returns at once in the foreground
uint64_t rt_background_waits(void);        // waits so far (tests, log)

#endif
