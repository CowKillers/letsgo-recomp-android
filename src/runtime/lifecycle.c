// The game in the background: see lifecycle.h.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "lifecycle.h"
#include "rt.h"  // LOG, plat_now_ns

static pthread_mutex_t bg_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bg_cond = PTHREAD_COND_INITIALIZER;
static atomic_bool bg_on;
static uint64_t bg_since_ns, bg_waits;

void rt_set_background(bool background) {
    pthread_mutex_lock(&bg_lock);
    if (atomic_load(&bg_on) != background) {
        uint64_t now = plat_now_ns();
        if (background) {
            bg_since_ns = now;
            LOG("[life] background: the game stops at its next present\n");
        } else {
            LOG("[life] foreground: the game goes on (%.1f s in the background, %llu waits: presents + guest sleeps)\n",
                bg_since_ns ? (now - bg_since_ns) / 1e9 : 0.0, (unsigned long long)bg_waits);
        }
        atomic_store(&bg_on, background);
        pthread_cond_broadcast(&bg_cond);
    }
    pthread_mutex_unlock(&bg_lock);
}

bool rt_in_background(void) { return atomic_load(&bg_on); }

void rt_background_wait(void) {
    if (!atomic_load(&bg_on)) return;  // (the foreground path: one atomic read per present)
    pthread_mutex_lock(&bg_lock);
    if (atomic_load(&bg_on)) bg_waits++;
    while (atomic_load(&bg_on)) pthread_cond_wait(&bg_cond, &bg_lock);
    pthread_mutex_unlock(&bg_lock);
}

uint64_t rt_background_waits(void) {
    pthread_mutex_lock(&bg_lock);
    uint64_t n = bg_waits;
    pthread_mutex_unlock(&bg_lock);
    return n;
}
