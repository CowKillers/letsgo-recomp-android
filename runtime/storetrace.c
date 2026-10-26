// Store tracing (research): LARECOMP_TRACE_STORES="<first>-<last>" with a game library built by `gen.py --trace-stores`
// (make TRACE=1, docs/DEPURACAO.md). Every store the game makes reports (instruction address, address, old, new value)
// here; during those presents each instruction is tallied: in how many presents it stored, how often by +1 / -1, how
// often by the same step as its previous store. At the last present the instructions that store in nearly every
// present with a steady step are listed: those are the game's frame counters and per-frame steps.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"

uint64_t renderer_present_count(void);

typedef struct {
    uint64_t pc;
    uint64_t frames, last_frame;  // presents it stored in
    uint64_t hits, inc1, dec1, same;
    int64_t last_delta;
    uint64_t addr, old, nw;
    int size;
} Site;

#define SITES (1u << 16)
static Site g_sites[SITES];
static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static long g_from = -2, g_to;
static bool g_printed;

static char g_env[64];
static char g_undo_env[512];
__attribute__((constructor)) static void trace_env(void) {
    const char *e = getenv("LARECOMP_TRACE_STORES");
    if (e) snprintf(g_env, sizeof g_env, "%s", e);
    e = getenv("LARECOMP_TRACE_UNDO");
    if (e) snprintf(g_undo_env, sizeof g_undo_env, "%s", e);
}

static void init(void) {
    const char *e = g_env[0] ? g_env : NULL;
    g_from = -1;
    if (e && atol(e) > 0) {
        g_from = atol(e);
        g_to = strchr(e, '-') ? atol(strchr(e, '-') + 1) : g_from;
    }
}

static int cmp_site(const void *a, const void *b) {
    const Site *x = a, *y = b;
    return x->frames < y->frames ? 1 : x->frames > y->frames ? -1 : 0;
}

static void report(void) {
    static Site out[SITES];
    int n = 0;
    long window = g_to - g_from + 1;
    for (unsigned i = 0; i < SITES; i++)
        if (g_sites[i].pc && g_sites[i].frames * 10 >= (uint64_t)window * 8) out[n++] = g_sites[i];  // (>= 80% of the presents)
    qsort(out, (size_t)n, sizeof out[0], cmp_site);
    LOG("[trace] %d store sites in >= 80%% of the %ld presents\n", n, window);
    for (int i = 0; i < n && i < 600; i++) {
        const Site *s = &out[i];
        LOG("[trace] pc %llx presents %llu hits %llu +1 %llu -1 %llu same-step %llu size %d addr %llx %llx -> %llx\n", (unsigned long long)s->pc,
            (unsigned long long)s->frames, (unsigned long long)s->hits, (unsigned long long)s->inc1, (unsigned long long)s->dec1,
            (unsigned long long)s->same, s->size, (unsigned long long)s->addr, (unsigned long long)s->old, (unsigned long long)s->nw);
    }
}

// Experiment (LARECOMP_TRACE_UNDO="<pc>[,<pc>...]", hex): the stores of those instructions are undone on odd presents,
// so a counter stepping once per update steps once per two presents: tests whether a counter is what a scene's speed
// comes from, without regenerating the game code.
static uint64_t g_undo[32];
static int g_nundo = -1;
static void undo_init(void) {
    g_nundo = 0;
    char buf[512];
    if (!g_undo_env[0]) return;
    snprintf(buf, sizeof buf, "%s", g_undo_env);
    for (char *t = strtok(buf, ","); t && g_nundo < 32; t = strtok(NULL, ",")) g_undo[g_nundo++] = strtoull(t, NULL, 16);
}

void rt_trace_store(uint64_t pc, uint64_t addr, uint64_t old, uint64_t nw, int size) {
    if (g_nundo < 0) undo_init();
    if (g_nundo && (renderer_present_count() & 1))
        for (int i = 0; i < g_nundo; i++)
            if (g_undo[i] == pc) {
                if (size == 4) *(uint32_t *)(uintptr_t)addr = (uint32_t)old;
                else if (size == 1) *(uint8_t *)(uintptr_t)addr = (uint8_t)old;
                else if (size == 8) *(uint64_t *)(uintptr_t)addr = old;
                break;
            }
    if (g_from == -2) {
        init();
        const char *e = getenv("LARECOMP_TRACE_STORES");
        LOG("[trace] first store: env \"%s\" window %ld-%ld present %llu\n", e ? e : "(null)", g_from, g_to, (unsigned long long)renderer_present_count());
    }
    if (g_from < 0) return;
    long f = (long)renderer_present_count();
    if (f < g_from) return;
    if (f > g_to) {
        if (!g_printed) {
            g_printed = true;
            while (atomic_flag_test_and_set(&g_lock)) {}
            report();
            atomic_flag_clear(&g_lock);
        }
        return;
    }
    while (atomic_flag_test_and_set(&g_lock)) {}
    uint32_t i = (uint32_t)((pc >> 2) * 0x9E3779B1u) & (SITES - 1);
    while (g_sites[i].pc && g_sites[i].pc != pc) i = (i + 1) & (SITES - 1);
    Site *s = &g_sites[i];
    s->pc = pc;
    if (s->last_frame != (uint64_t)f + 1) s->frames++, s->last_frame = (uint64_t)f + 1;
    s->hits++;
    int64_t d = (int64_t)(nw - old);
    if (size == 4) d = (int32_t)(uint32_t)(nw - old);
    if (d == 1) s->inc1++;
    if (d == -1) s->dec1++;
    if (d == s->last_delta) s->same++;
    s->last_delta = d;
    s->addr = addr, s->old = old, s->nw = nw, s->size = size;
    atomic_flag_clear(&g_lock);
}
