// Host regression for the game in the background (runtime/lifecycle.c, t_8cc15f32): a "game thread" presenting in a
// loop stops at its present while the activity is in the background, and goes on once it is in front again.
//   clang -I src/runtime src/tools/test_lifecycle.c src/runtime/lifecycle.c -lpthread -o build/test_lifecycle
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include "../runtime/lifecycle.h"

uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void sleep_ms(int ms) { struct timespec ts = {0, ms * 1000000L}; nanosleep(&ts, NULL); }

static atomic_ullong presents;
static atomic_bool quit;
static void *game(void *arg) {
    (void)arg;
    while (!atomic_load(&quit)) {
        rt_background_wait();   // (nvn/objects.c nvnQueuePresentTexture)
        atomic_fetch_add(&presents, 1);
        sleep_ms(2);            // a ~500 fps game: the test does not wait for 30 fps
    }
    return NULL;
}

int main(void) {
    pthread_t t;
    assert(!rt_in_background());
    pthread_create(&t, NULL, game, NULL);
    sleep_ms(100);
    unsigned long long a = atomic_load(&presents);
    assert(a > 10);  // in front: it runs
    rt_set_background(true);
    sleep_ms(50);    // (at most the present in flight)
    unsigned long long b = atomic_load(&presents);
    sleep_ms(300);
    unsigned long long c = atomic_load(&presents);
    printf("background: %llu presents in 300 ms (foreground: %llu in 100 ms)\n", c - b, a);
    assert(c == b);  // in the background: not a single present
    assert(rt_background_waits() == 1);
    rt_set_background(true);   // twice (onPause, then surfaceDestroyed): no harm
    rt_set_background(false);
    sleep_ms(100);
    unsigned long long d = atomic_load(&presents);
    assert(d > c + 10);  // in front again: it goes on
    atomic_store(&quit, true);
    pthread_join(t, NULL);
    puts("LIFECYCLE_OK: the game stops at its present in the background and goes on in front");
    return 0;
}
