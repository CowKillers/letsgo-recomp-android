// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#if defined(__x86_64__)
#include <immintrin.h>  // (unused on other architectures: kept for the x86-64 build)
#endif
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#include "../../kernel.h"
#include "shaders.h"
#include "vk.h"
#include "../renderer.h"
// Forward declaration for hmix (defined as static later)
static uint64_t hmix(uint64_t h, const void *p, size_t n);
#include "../../settings.h"

static uint64_t g_draws_total;  // (never reset: LARECOMP_COUNTER_LOG)
static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
// Skipped draws by reason (logged with the stats): 0 no program bound, 1 program still compiling, 2 program failed,
// 3 no program object, 4 primitive mode, 5 pipeline still building, 6 pipeline failed.
static uint64_t stat_skip_why[8];
uint64_t g_skin_watch;  // (debug, LARECOMP_DUMP_SKIN: a vertex cbuf watched across frames)
// docs/PLANO-3D.md experiments, logged with the stats: [0] palettes replaced by identity, [1] skinned-draw cbufs
// read in place (UBO_DIRECT), [2] skinned-draw cbufs UBO_DIRECT could not place (copied), [3] one-bone shaders.
static uint64_t g_exp_count[4];
#define SKIP(why) (stat_skipped++, stat_skip_why[(why) & 7]++)
static uint64_t stat_upload_bytes;
// LARECOMP_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
static VkQueryPool prof_qp;
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;  // (profiling) never reset: hitch log
static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
static _Atomic int stat_compiles;
_Atomic int g_font_atlas_bound = 0;  // MISSÃO C: set when ATLAS2048 is bound
_Atomic int g_font_capture_frame = 0;  // P1 v0066: set to capture frame screenshot after font draw
// FALHA 1: contadores independentes para diagnóstico
_Atomic uint64_t cnt_text_candidates = 0;   // draws avaliados para filtro de atlas
_Atomic uint64_t cnt_text_matched = 0;      // draws com atlas confirmado no descriptor
_Atomic uint64_t cnt_text_captured = 0;     // draws com log detalhado emitido
_Atomic uint64_t cnt_text_rejected = 0;     // draws rejeitados pelo filtro
_Atomic uint64_t cnt_text_dropped = 0;      // logs descartados por limite
static _Atomic uint64_t stat_translate_ns, stat_spirv_ns, stat_pipe_ns;  // CPU time translating / compiling programs
static uint64_t stat_shader_wait_ns;  // draws waiting for their program / pipeline (wait_built)
static _Atomic int new_pipes;  // pipelines created since the pipeline cache was last saved
static uint64_t op_counts[CMD_COUNT];
static uint64_t stat_vtx_copied, stat_vtx_direct;  // vertex / index ranges of the draws: copied to VRAM / read in place
bool g_debug_frame;
static char stats_text[1024];
void vk_collect_dead(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define SUBS 8
typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (profiling)
    uint64_t seq;
    VkDescriptorPool pools[16];
    int npools, cur_pool;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static uint64_t submit_seq;  // last submitted
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;

uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

void vk_wait_seq(uint64_t seq) {
    if (!seq) return;
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    stat_wait_ns += SDL_GetTicksNS() - t0;
    cum_wait += SDL_GetTicksNS() - t0;
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r != VK_SUCCESS) {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        vk_wait_seq(s->seq);
        if (s->ts) {  // (profiling) GPU time of that submission
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
                prof_gpu += (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
            s->ts = false;
        }
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        s->cur_pool = 0;
        pthread_mutex_lock(&seq_lock);
        s->main_begun = s->upload_begun = false;
        s->seq = submit_seq + 1;
        pthread_mutex_unlock(&seq_lock);
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    pthread_mutex_lock(&seq_lock);
    *begun = true;
    pthread_mutex_unlock(&seq_lock);
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        gp_mark(s->main, "(submission start; includes GPU idle before it)");
        if (prof && prof_qp) {
            vkCmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 2, 2);
            vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 2);
            s->ts = true;
        }
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    begin_cb(&s->upload, &s->upload_begun);
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void ring_retire(void);

// ---- LARECOMP_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
bool g_gpuprof_frame;
static VkQueryPool gp_qp;
static int gp_n;
#define GP_MAX 4096
static char gp_label[GP_MAX][112];
static uint32_t gp_draws[GP_MAX];
// LARECOMP_GPUPROF_STATS=1 (with LARECOMP_GPUPROF_DRAWS): the pipeline statistics of every draw of the profiled frames
// (input vertices, vertex shader invocations, clipped primitives, fragment shader invocations), printed on its row.
static VkQueryPool gps_qp;
static int gps_n;
static int gp_stat[GP_MAX];  // mark -> its index in gps_qp + 1 (0: none)
// LARECOMP_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with LARECOMP_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted).
static char dl_ring[64][128];
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = getenv("LARECOMP_VK_DEVLOST_TRACE") ? atoi(getenv("LARECOMP_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl_ring[dl_n++ & 63], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }
void vk_devlost_dump(void) {
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) LOG("[vk]   %s\n", dl_ring[i & 63]);
}
uint64_t vk_frame_counter(void);
static uint64_t gp_frame;  // the frame counter when the profiled frame began (parity: LARECOMP_EXP_ALT)
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= GP_MAX - 1) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_MAX};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) {
        gp_frame = vk_frame_counter();
        vkCmdResetQueryPool(cb, gp_qp, 0, GP_MAX);
        if (getenv("LARECOMP_GPUPROF_STATS")) {
            if (!gps_qp) {
                VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
                                            .queryCount = GP_MAX,
                                            .pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                                                                  VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                                                                  VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                                                                  VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT};
                if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gps_qp) != VK_SUCCESS) gps_qp = VK_NULL_HANDLE;
            }
            if (gps_qp) vkCmdResetQueryPool(cb, gps_qp, 0, GP_MAX);
            gps_n = 0;
        }
    }
    gp_stat[gp_n] = 0;
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
void renderer_flush(void);
void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    static uint64_t t[GP_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (frame %llu; rows: the work after each mark, until the next)\n", gp_n, total,
            (unsigned long long)gp_frame);
        static uint64_t stats[GP_MAX * 4];
        bool have_stats = gps_qp && gps_n &&
            vkGetQueryPoolResults(g_vk.dev, gps_qp, 0, (uint32_t)gps_n, sizeof stats, stats, 32, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS;
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            bool st = have_stats && gp_stat[i];
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] || st ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
            if (st) {
                const uint64_t *q = &stats[(gp_stat[i] - 1) * 4];  // (the enabled bits, in order: IA vertices, VS invocations, clip prims, FS invocations)
                LOG(" | ia %llu vsinv %llu clipprim %llu fsinv %llu\n", (unsigned long long)q[0], (unsigned long long)q[1],
                    (unsigned long long)q[2], (unsigned long long)q[3]);
            }
        }
    }
    gp_n = 0;
}

static void ring_flush_gpu(void);
// Submit the pending work (if any). wait/signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    vk_end_pass();
    ring_flush_gpu();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    if (s->ts) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 2 + 1);
    VK_CHECK(vkEndCommandBuffer(s->main));
    cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
                                     .value = submit_seq + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSemaphoreSubmitInfo wt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)n, .pCommandBufferInfos = cbs,
                        .signalSemaphoreInfoCount = signal ? 2 : 1, .pSignalSemaphoreInfos = sig,
                        .waitSemaphoreInfoCount = wait ? 1 : 0, .pWaitSemaphoreInfos = &wt};
    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
        VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
        if (sr == VK_ERROR_DEVICE_LOST) vk_devlost_dump();
        VK_CHECK(sr);
        PROF_ADD(prof_submit);
    }
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    cur_sub = (cur_sub + 1) % SUBS;
    pthread_mutex_unlock(&seq_lock);
    stat_flushes++;
    cum_submits++;
    ring_retire();
    vk_collect_dead();
}

void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE); }

void renderer_flush(void) {
    renderer_submit();
    vk_wait_seq(submit_seq);
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(s);
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    Sub *s = &subs[cur_sub];
    // A reused slot keeps its old begun flags until sub() prepares it: an empty queue's fence must name the last
    // submission, not a future one that submit's empty-work fast path never signals (LAS Recomp 37b7ff6).
    bool pending = s->seq == submit_seq + 1 && (s->main_begun || s->upload_begun);
    uint64_t q = submit_seq + (pending ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    vk_wait_seq(seq > s ? s : seq);  // not submitted yet (other thread): everything submitted so far
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 16) FATAL("descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024}};
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = 5, .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->cur_pool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        s->cur_pool++;
    }
}

// ================================================================== transient memory ring
// The largest index of a draw (bounds the vertex data copied for it), ignoring the restart value: AVX2 where the CPU has it (the
// game code needs it too), a plain loop elsewhere. A draw scans a few thousand indices: ~1.4 us scalar, ~0.15 us here.
#if defined(__x86_64__)
// (x86-64 only: elsewhere the scalar loop below is used)
__attribute__((target("avx2"))) static uint32_t scan_max16_avx2(const uint16_t *q, uint32_t n, uint16_t restart) {
    __m256i acc = _mm256_setzero_si256(), rs = _mm256_set1_epi16((short)restart);
    uint32_t i = 0;
    for (; i + 16 <= n; i += 16) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(q + i));
        acc = _mm256_max_epu16(acc, _mm256_andnot_si256(_mm256_cmpeq_epi16(v, rs), v));
    }
    uint16_t t[16];
    _mm256_storeu_si256((__m256i *)t, acc);
    uint32_t m = 0;
    for (int k = 0; k < 16; k++) if (t[k] > m) m = t[k];
    for (; i < n; i++) if (q[i] != restart && q[i] > m) m = q[i];
    return m;
}
__attribute__((target("avx2"))) static uint32_t scan_max32_avx2(const uint32_t *q, uint32_t n, uint32_t restart) {
    __m256i acc = _mm256_setzero_si256(), rs = _mm256_set1_epi32((int)restart);
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(q + i));
        acc = _mm256_max_epu32(acc, _mm256_andnot_si256(_mm256_cmpeq_epi32(v, rs), v));
    }
    uint32_t t[8];
    _mm256_storeu_si256((__m256i *)t, acc);
    uint32_t m = 0;
    for (int k = 0; k < 8; k++) if (t[k] > m) m = t[k];
    for (; i < n; i++) if (q[i] != restart && q[i] > m) m = q[i];
    return m;
}
#endif  // __x86_64__: the AVX2 helpers above; the scalar fallback below runs everywhere
static uint32_t scan_max_index(const void *src, uint32_t isz, uint32_t n, uint32_t restart) {
#if defined(__x86_64__)
    static int avx2 = -1;
    if (avx2 < 0) avx2 = __builtin_cpu_supports("avx2");
#endif
    uint32_t m = 0;
    if (isz == 2) {
#if defined(__x86_64__)
        if (avx2) return scan_max16_avx2(src, n, (uint16_t)restart);
#endif
        const uint16_t *q = src;
        for (uint32_t i = 0; i < n; i++) if (q[i] > m && q[i] != (uint16_t)restart) m = q[i];
    } else {
#if defined(__x86_64__)
        if (avx2) return scan_max32_avx2(src, n, restart);
#endif
        const uint32_t *q = src;
        for (uint32_t i = 0; i < n; i++) if (q[i] > m && q[i] != restart) m = q[i];
    }
    return m;
}

// Two pools of 64 MB chunks, a chunk reused once every submission that allocated from it has completed.
// Pool 0: host-visible chunks (VRAM through the BAR when the GPU has one) the GPU reads in place: constants and the sources of
// texture uploads. Pool 1 (ring_gpu_enabled, discrete GPUs): the vertex and index data of the draws, in VRAM. The CPU writes a
// staging chunk in system memory and each submission starts with a copy of what was written since the last one (ring_flush_gpu),
// so the GPU fetches vertices from VRAM instead of reading the game's memory over PCIe once per vertex invocation (an
// instanced draw re-read its per-instance streams for every vertex: ~0.8 ms for 71 thousand invocations, docs/PESQUISA-GPU.md).
#define CHUNK_SIZE (64ull << 20)
typedef struct { VkBuffer buf, dev; VmaAllocation mem, dmem; uint8_t *ptr; uint64_t size, used, seq, flushed; } Chunk;
typedef struct { Chunk *c; int n, active; bool gpu; } Pool;
static Pool pools[2] = {{.active = -1}, {.active = -1, .gpu = true}};

// LARECOMP_VERTEX_READ=direct|copy|auto (default auto: copies on a discrete GPU, in place on an integrated one, whose memory
// is the system's). The draws' own ranges the GPU writes stay in place either way (gpuw below).
static bool ring_gpu_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LARECOMP_VERTEX_READ");
        if (!e) e = g_settings.vertex_read == VREAD_DIRECT ? "direct" : g_settings.vertex_read == VREAD_COPY ? "copy" : NULL;
        if (e && !strcmp(e, "direct")) on = 0;
        else if (e && (!strcmp(e, "copy") || !strcmp(e, "hybrid"))) on = 1;
        else on = g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        LOG("[vk] vertex and index data: %s\n", on ? "copied to VRAM (ranges the GPU writes are read in place)" : "read in place from the game's memory");
    }
    return on;
}

static int new_chunk(Pool *pl, uint64_t size) {
    pl->c = realloc(pl->c, (pl->n + 1) * sizeof *pl->c);
    Chunk *c = &pl->c[pl->n];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = pl->gpu ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                              : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = pl->gpu ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO,
                                  .flags = (pl->gpu ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info));
    c->ptr = info.pMappedData;
    c->size = size;
    if (pl->gpu) {
        VkBufferCreateInfo di = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo da = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &di, &da, &c->dev, &c->dmem, NULL));
    }
    {   // (where the GPU reads the copies from: VRAM through the BAR, or system memory over PCIe)
        VkMemoryPropertyFlags mp = 0;
        vmaGetAllocationMemoryProperties(g_vk.vma, pl->gpu ? c->dmem : c->mem, &mp);
        LOG("[vk] ring %d chunk %d: %llu MB, %s%s\n", pl->gpu ? 1 : 0, pl->n, (unsigned long long)(size >> 20),
            mp & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device-local" : "system memory",
            mp & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " host-visible" : "");
    }
    return pl->n++;
}

static void ring_retire(void) {}  // chunks are recycled lazily in transient_from

static VkSpan transient_from(Pool *pl, uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (pl->active >= 0) {
        Chunk *c = &pl->c[pl->active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            return (VkSpan){pl->gpu ? c->dev : c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < pl->n; i++)
        if (i != pl->active && pl->c[i].seq <= done && pl->c[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(pl, size > CHUNK_SIZE ? size : CHUNK_SIZE);
    pl->active = pick;
    Chunk *c = &pl->c[pick];
    c->used = size;
    c->flushed = 0;
    c->seq = pending;
    stat_upload_bytes += size;
    return (VkSpan){pl->gpu ? c->dev : c->buf, 0, c->ptr};
}

VkSpan vk_transient(uint64_t size, uint64_t align) { return transient_from(&pools[0], size, align); }
// Vertex / index data of a draw: in VRAM (valid once the submission's first copy has run), or as vk_transient where that is off.
static VkSpan transient_gpu(uint64_t size, uint64_t align) { return transient_from(&pools[ring_gpu_enabled() ? 1 : 0], size, align); }

// The start of a submission: the staging bytes written since the last one go to VRAM (the upload buffer runs first, then a
// barrier, vk_submit_extra).
static void ring_flush_gpu(void) {
    Pool *pl = &pools[1];
    for (int i = 0; i < pl->n; i++) {
        Chunk *c = &pl->c[i];
        if (c->used <= c->flushed) continue;
        VkBufferCopy rg = {c->flushed, c->flushed, c->used - c->flushed};
        vkCmdCopyBuffer(vk_upload_cmd(), c->buf, c->dev, 1, &rg);
        c->flushed = c->used;
    }
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C, DEDUPE_SLOTS };
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 16384
static struct { uint64_t addr, size, seq; VkSpan span; bool gpu; } gcopy[GCOPY_TAB];
static uint64_t gcopy_gen;  // bumped to drop every shared copy (guest memory written mid-list)
static void guest_copy_flush(void) { gcopy_gen++; }
static VkSpan transient_gpu_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = transient_gpu(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}
static VkSpan guest_copy_pool(uint64_t addr, uint64_t size, bool gpu) {  // (gpu: vertex / index data, in VRAM when that ring is on)
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40), h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    if (gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size && gcopy[slot].gpu == gpu) return gcopy[slot].span;
    VkSpan s = gpu ? transient_gpu_copy((const void *)(uintptr_t)addr, size, 256) : vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s; gcopy[slot].gpu = gpu;
    return s;
}
static VkSpan guest_copy(uint64_t addr, uint64_t size) { return guest_copy_pool(addr, size, false); }

// Vertex data copied to VRAM, once per start address and submission: every draw of a mesh starts at the same address and wants a
// prefix of it (up to its largest index), so one copy serves them all. A per-vertex stream is copied whole (its bound range, up to
// 8 MB) the first time, so that later draws of other parts of the buffer don't copy it again (37 MB a frame became 16).
#define VCOPY_TAB 1024
static struct { uint64_t addr, len, seq; VkSpan span; } vcopy[VCOPY_TAB];
static VkSpan vertex_copy(uint64_t addr, uint64_t need, uint64_t bound, bool per_vertex) {
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40);
    uint32_t h = (uint32_t)((addr >> 4) * 0x9E3779B1u) & (VCOPY_TAB - 1), at = h;
    for (int i = 0; i < 4; i++) {
        uint32_t k = (h + i) & (VCOPY_TAB - 1);
        if (vcopy[k].addr == addr && vcopy[k].seq == seq) {
            if (vcopy[k].len >= need) return vcopy[k].span;
            at = k;  // (a longer prefix is needed now: a new copy replaces it)
            goto copy;
        }
        if (vcopy[k].seq != seq) at = k;  // (a free or stale slot)
    }
copy:;
    uint64_t len = per_vertex && bound <= (8ull << 20) && bound > need ? bound : need;
    VkSpan sp = transient_gpu_copy((const void *)(uintptr_t)addr, len, 256);
    vcopy[at].addr = addr; vcopy[at].len = len; vcopy[at].seq = seq; vcopy[at].span = sp;
    return sp;
}

// ================================================================== worker threads (shader / pipeline builds)
// First-use compiles run off the render thread; draws needing them are skipped until ready (as on Metal).
// LARECOMP_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
static Job *jobs_head, *jobs_tail;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;

static void *worker(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&jobs_lock);
        while (!jobs_head) pthread_cond_wait(&jobs_cond, &jobs_lock);
        Job *j = jobs_head;
        jobs_head = j->next;
        if (!jobs_head) jobs_tail = NULL;
        pthread_mutex_unlock(&jobs_lock);
        j->fn(j->arg);
        free(j);
    }
    return NULL;
}

static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) v = getenv("LARECOMP_SYNC_SHADERS") != NULL;
    return v;
}

// Shader translation runs on the workers too, then compiles on the same worker (translate_program). It had one
// dedicated thread (the translator used a static string ring, now per thread): on a first visit, ~380 programs took
// ~240 ms one after another while the workers idled. Off the game thread either way (up to ~90 ms per frame there).
static void run_job(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) {
        started = true;
        // first entry into an area builds dozens of pipelines at once: use most of the CPU (draws wait for them)
        int n = SDL_GetNumLogicalCPUCores() - 4;
#ifdef __ANDROID__
        // Android: 3 workers (reference LASRecomp-Android-Plus tuning:
        // game, render and translator threads keep cores of their own - less heat)
        n = 3;
#else
        if (n < 4) n = 4;
        if (n > 16) n = 16;
#endif
        for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); }
    }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&jobs_lock);
    if (jobs_tail) jobs_tail->next = j; else jobs_head = j;
    jobs_tail = j;
    pthread_cond_signal(&jobs_cond);
    pthread_mutex_unlock(&jobs_lock);
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
    uint64_t obj;       // frame interpolation: the game object the current draws belong to (CMD_OBJECT), 0 = none
    uint32_t obj_draw;  // draws of that object so far in this run of it
} st;

// frame interpolation (prototype, LARECOMP_INTERP=1): the hooks below; the rest at the end of this file
static int ip_on = -1;
static bool ip_replaying;     // running a recorded frame again for the in-between image
static uint64_t ip_stats_tagged, ip_stats_untagged;  // draws with / without a game object (CMD_OBJECT)
static uint64_t ip_draw_sig;  // the current draw's identity (draw_)
static bool ip_draw_obj;      // ... and whether it belongs to a game object (CMD_OBJECT): only those are blended
static bool ip_draw_2d;       // ... a 2D draw (no depth test): interface, menus
static bool ip_enabled(void);
bool vk_interp_enabled(void) { return ip_enabled(); }  // (cmdbuf.c: hooks the game's object draw functions)
static bool ip_enabled(void) {
    // The 60 fps mode (runtime/hle/fps60.c): the world at 60 natively; menus and the Pokédex at the game's 30 with the
    // in-between image; battles and captures at 30 without it (tried, left out: docs/HISTORICO.md 46).
    // LARECOMP_INTERP=1 (research): always on.
    if (ip_on == -1) ip_on = getenv("LARECOMP_INTERP") ? atoi(getenv("LARECOMP_INTERP")) : 0;
    extern bool fps60_menu(void);
    return ip_on > 0 || fps60_menu();
}
static const void *ip_cbuf(int stage, int k, uint64_t addr, uint32_t size);
static void ip_note_list(uint64_t handle);
static bool ip_skip_op(uint32_t op) {  // what the replay must not do again: it writes guest memory or signals the game
    switch (op) {
    case CMD_DISPATCH: case CMD_DISPATCH_INDIRECT: case CMD_COPY_BUFFER_TO_BUFFER: case CMD_COPY_TEXTURE_TO_BUFFER:
    case CMD_UPDATE_UNIFORM_BUFFER: case CMD_FENCE_SYNC: case CMD_WAIT_SYNC: case CMD_SIGNAL_EVENT: case CMD_WAIT_EVENT:
    case CMD_REPORT_COUNTER: case CMD_CLEAR_BUFFER:
    case CMD_COPY_BUFFER_TO_TEXTURE:  // (uploads: done when the frame ran; again, they overran scaled images)
        return true;
    default:
        return false;
    }
}

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// (LARECOMP_VK_PROF) called once per presented frame: frames over 50 ms are logged with where their time went.
// present_ns: time in backend_present's acquire/blit/present.
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    static uint64_t last_t, l_exec, l_wait, l_draws, l_subs;
    uint64_t now = SDL_GetTicksNS();
    if (last_t && now - last_t > 50000000) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        LOG("[hitch] frame %llu: %.0f ms = backend exec %.1f (of which GPU waits %.1f) + present %.1f + other %.1f; %llu draws, "
            "%llu submits, compiles %d pending\n", (unsigned long long)frame, dt / 1e6, ex / 1e6, wt / 1e6, present_ns / 1e6,
            ((double)dt - ex - present_ns) / 1e6, (unsigned long long)(cum_draws - l_draws), (unsigned long long)(cum_submits - l_subs),
            atomic_load(&stat_compiles));
    }
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_draws = cum_draws; l_subs = cum_submits;
}

static void save_pipeline_cache(void *a) {
    (void)a;
    uint64_t t0 = SDL_GetTicksNS();
    vk_pipeline_cache_save();
    LOG("[vk] pipeline cache saved (%.0f ms)\n", (SDL_GetTicksNS() - t0) / 1e6);
}

const char *renderer_stats(void) {
    // The pipeline cache was written only at a clean exit: a crash or a killed process lost every pipeline built in
    // the session, and the next one popped in again. Save it on a worker once a burst of new pipelines settles
    // (no new ones for ~6 s: entering an area builds them for a few seconds).
    static int quiet, last_new;
    int np = atomic_load(&new_pipes);
    if (np && np == last_new) {
        if (++quiet >= 3) { atomic_fetch_sub(&new_pipes, np); np = 0; quiet = 0; run_job(save_pipeline_cache, NULL); }
    } else {
        quiet = 0;
    }
    last_new = np;
    static int dumps;
    if (dumps++ < 3) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d (translate %.0fms, spirv %.0fms, pipelines %.0fms, draws waited %.0fms), fbcopy %llu, upload %.1fMB",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), atomic_exchange(&stat_translate_ns, 0) / 1e6, atomic_exchange(&stat_spirv_ns, 0) / 1e6, atomic_exchange(&stat_pipe_ns, 0) / 1e6,
             stat_shader_wait_ns / 1e6,
             (unsigned long long)stat_feedback_copies, (double)stat_upload_bytes / 1048576.0);
    {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | vertex ranges %llu copied, %llu in place", (unsigned long long)stat_vtx_copied, (unsigned long long)stat_vtx_direct);
        stat_vtx_copied = stat_vtx_direct = 0;
    }
    stat_feedback_copies = stat_upload_bytes = stat_shader_wait_ns = 0;
    if (g_exp_count[0] | g_exp_count[1] | g_exp_count[2] | g_exp_count[3]) {
        LOG("[exp] skin identity %llu, ubo direct %llu, ubo copied %llu, one-bone shaders %llu\n",
            (unsigned long long)g_exp_count[0], (unsigned long long)g_exp_count[1], (unsigned long long)g_exp_count[2],
            (unsigned long long)g_exp_count[3]);
        g_exp_count[0] = g_exp_count[1] = g_exp_count[2] = 0;  // ([3]: a running total)
    }
    if (g_skin_watch) {
        const uint32_t *w = (const uint32_t *)(uintptr_t)g_skin_watch;
        LOG("[skin] cb3 %llx now: row8 %08x %08x row9 %08x %08x\n", (unsigned long long)g_skin_watch, w[32], w[33], w[36], w[37]);
    }
    if (stat_skipped) {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " [skip: noprog %llu compiling %llu failed %llu noobj %llu mode %llu pipe-building %llu pipe-failed %llu]",
                 (unsigned long long)stat_skip_why[0], (unsigned long long)stat_skip_why[1],
                 (unsigned long long)stat_skip_why[2], (unsigned long long)stat_skip_why[3],
                 (unsigned long long)stat_skip_why[4], (unsigned long long)stat_skip_why[5],
                 (unsigned long long)stat_skip_why[6]);
        memset(stat_skip_why, 0, sizeof stat_skip_why);
    }
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu", (unsigned long long)prof_views_created);
        prof_views_created = 0;
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

void vk_exec_init(void) {
    prof = getenv("LARECOMP_VK_PROF") != NULL;
    if (prof) {
        prof_up_ns = 0;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 2};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    st_defaults();
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];
static uint32_t tex_wh[2][64][2];

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static int debug_pass_no;

static void vk_end_pass_at(int line);
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static void vk_end_pass_at(int line) {
    if (!in_pass) return;
    if (prof && line < 4096) prof_end_line[line]++;
    vkCmdEndRendering(vk_cmd());
    in_pass = false;
    if (g_debug_frame && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = getenv("LARECOMP_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no++);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no++);
        renderer_save_texture_tex(st.rt.color[0]->backend, path);
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }

static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each pass its own submission)
    st.rt_dirty = false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
    }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        gp_mark(cb, "pass: %d color %ux%u fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                t0 ? t0->format : 0, st.rt.depth ? "yes" : "no");
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        TexBackend *b = t->backend;
        if (!v) continue;
        ca[i].imageView = v;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
        }
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    // Bound color slots without a texture / view went in as VK_NULL_HANDLE attachments (legal), but Intel's Windows
    // driver for UHD 630 crashes in vkCmdBeginRendering on them (LAS Recomp f4cb039, its issues #8 / #46). Empty slots
    // at the end are left out (pipelines and clears follow pass_ncolor); one in the middle is logged once.
    while (pass_ncolor && !ca[pass_ncolor - 1].imageView) pass_ncolor--;
    static bool logged_hole;
    for (uint32_t i = 0; i < pass_ncolor && !logged_hole; i++)
        if (!ca[i].imageView) {
            logged_hole = true;
            LOG("[vk] pass with an empty color slot %u of %u (texture %s)\n", i, pass_ncolor, st.rt.color[i] ? "without a view" : "none");
        }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) { pass_sx = pass_sy = 1; minw = minh = 16; }
    pass_w = minw; pass_h = minh;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = 1,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    in_pass = true;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    vkCmdSetScissor(vk_cmd(), 0, 1, &r);
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    vkCmdSetViewport(vk_cmd(), 0, 1, &vp);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
#define NDYN (sizeof DYN / sizeof DYN[0])

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    dyn_defaults(cb);
    if (depth) { vkCmdSetDepthTestEnable(cb, VK_TRUE); vkCmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        vkCmdSetStencilTestEnable(cb, VK_TRUE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    bool vs_uses_vid;
    _Atomic int state;  // 0 building, 1 ready, 2 failed
} Prog;

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout};
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!getenv("LARECOMP_SAVE_MSL")) return;
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

static void compile_program(void *arg) {
    Prog *p = arg;
    size_t nv = 0, nf = 0;
    uint64_t t0 = SDL_GetTicksNS();
    // MISSÃO 1 v0073: calcula fshash (mesma fórmula do log [font-draw])
    // p->code[] são endereços; fshash = hmix sobre os bytes do shader
    uint64_t vs_fshash = p->code[0] ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(p->code[0] + 0x30), 0x200) : 0;
    uint64_t fs_fshash = p->code[1] ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(p->code[1] + 0x30), 0x200) : 0;
    // v0087 VS_DIAG: decisive test for VS positions as the killer of dialog fragments.
    // (v0088 CORRECTION — the mechanism claimed below is REFUTED, do not reuse it:
    // symbolic execution of case 8u tracking only address registers PROVED the
    // real loads are r4-r7 <- +0x00..+0x0c, r12-r15 <- +0x40..+0x4c,
    // r16-r19 <- +0x50..+0x5c (NOT +0x20). Color at +0x20/+0x24 goes only to
    // r20/r21 (varying for the FS), never to a position multiplier. Numeric
    // simulation with the real [gcopy-diag] inst0 data gives VALID local
    // positions — (1.5,-29.5), (17.5,-29.5), (1.5,16.5), (17.5,16.5), w=1.0,
    // a 16x46 quad — NOT degenerate. What VS_DIAG actually proved is narrower:
    // forcing gl_Position restores fragments, i.e. the defect is in gl_Position's
    // INPUTS, not the FS/pipeline/draw. The one gl_Position input NEVER verified
    // is a0 (vertex attributes): VS_DIAG bypasses a0 entirely (gl_VertexIndex).
    // If a0 is unbound for the dialog draw, draw_() feeds it the ZERO_BINDING
    // constant (0,0,0,1) -> all 4 corners collapse to (1.5,-29.5) -> degenerate.
    // See the v0088 A0_FIX block below, which is the operative change now.)
    // Gated by LARECOMP_VS_DIAG=1 (set in LetsGoActivity alongside TEXT_DIAG).
    // Applies to both dialog VS variants (8e41cc9edf14ab8c current, afd67833c2e8f5ee).
    const char *vs_src = p->vi->msl;
    char *vs_diag = NULL;
    if ((vs_fshash == 0x8e41cc9edf14ab8cULL || vs_fshash == 0xafd67833c2e8f5eeULL) && getenv("LARECOMP_VS_DIAG") && p->vi && p->vi->msl) {
        const char *epi = strstr(p->vi->msl, "gl_Position.y = -gl_Position.y;");
        if (epi) {
            const char *ins = "  // VS_DIAG v0087: force visible centered quad (proves VS positions are the killer)\n"
                              "  { vec2 qp = vec2(0.5, 0.5);\n"
                              "    if (gl_VertexIndex == 0u) qp = vec2(-0.5, -0.5);\n"
                              "    else if (gl_VertexIndex == 1u) qp = vec2(0.5, -0.5);\n"
                              "    else if (gl_VertexIndex == 2u) qp = vec2(-0.5, 0.5);\n"
                              "    gl_Position = vec4(qp, 0.0, 1.0); }\n";
            size_t len = strlen(p->vi->msl), ilen = strlen(ins);
            vs_diag = malloc(len + ilen + 1);
            if (vs_diag) {
                size_t off = (size_t)(epi - p->vi->msl);
                memcpy(vs_diag, p->vi->msl, off);
                memcpy(vs_diag + off, ins, ilen);
                memcpy(vs_diag + off + ilen, p->vi->msl + off, len - off + 1);
                vs_src = vs_diag;
                LOG("[shader] VS_DIAG: forced quad for dialog VS %016llx\n", (unsigned long long)vs_fshash);
            }
        }
    }
    // v0088 A0_FIX: the operative change (LetsGoActivity no longer sets
    // LARECOMP_VS_DIAG; the VS_DIAG block above stays, gated, inert). Derives
    // the quad corners from gl_VertexIndex (standard indexed-quad layout:
    // 0=(0,0) 1=(1,0) 2=(0,1) 3=(1,1)) into a local a0f and rewrites the a0[]
    // uses to a0f[]. For a bound a0 with the standard layout this is identical
    // (harmless); for an unbound a0 (ZERO_BINDING constant (0,0,0,1)) it
    // restores real glyph quads from the real gmem data + real cb3 — NOT the
    // centered VS_DIAG blob. Applies to both dialog VS variants.
    // Unconditional: the VS source cannot know the per-draw binding, and the
    // override is provably harmless for standard-layout bound a0. a0 appears
    // in these VSs only as the declaration + a0[0]/a0[1] uses, so rewriting
    // "a0[" in the main body is complete; a0.z/a0.w are preserved.
    char *vs_a0fix = NULL;
    if ((vs_fshash == 0x8e41cc9edf14ab8cULL || vs_fshash == 0xafd67833c2e8f5eeULL) && p->vi && p->vi->msl) {
        const char *mainp = strstr(vs_src, "void main() {");
        if (mainp) {
            const char *ins = "  vec4 a0f = vec4((gl_VertexIndex == 1u || gl_VertexIndex == 3u) ? 1.0 : 0.0, (gl_VertexIndex >= 2u) ? 1.0 : 0.0, a0.z, a0.w);\n";
            const char *body = mainp + strlen("void main() {");
            size_t reps = 0;
            for (const char *q = body; (q = strstr(q, "a0[")) != NULL; q += 3) reps++;
            size_t srclen = strlen(vs_src), ilen = strlen(ins);
            vs_a0fix = malloc(srclen + ilen + reps + 1);
            if (vs_a0fix) {
                size_t head = (size_t)(body - vs_src);
                memcpy(vs_a0fix, vs_src, head);
                memcpy(vs_a0fix + head, ins, ilen);
                char *dst = vs_a0fix + head + ilen;
                for (const char *q = body; *q; ) {
                    if (q[0] == 'a' && q[1] == '0' && q[2] == '[') { memcpy(dst, "a0f[", 4); dst += 4; q += 3; }
                    else *dst++ = *q++;
                }
                *dst = '\0';
                vs_src = vs_a0fix;
                LOG("[shader] A0_FIX: dialog VS %016llx: a0[0]/a0[1] from gl_VertexIndex (%zu uses)\n", (unsigned long long)vs_fshash, reps);
            }
        }
    }
    // v0090 VS_RANGE_DIAG: conditional override — the decisive VS-output test.
    // All state is verified (gcopy data, gfind, a0 bound, write_mask 0xF,
    // scissor/viewport full, cull NONE, depth OFF, TEXT_DIAG in pipeline,
    // cb3 valid ortho with NDC inside [-1,1] by static simulation v0089).
    // The one thing never observed is what the translated VS REALLY emits
    // on-device. This injects, after the epilogue (gl_Position fully
    // computed), a clip-range check: if w<=0, NaN, or |x|/|y|>w, z<0, z>w,
    // force the centered quad (same as VS_DIAG). Gated by
    // LARECOMP_VS_RANGE_DIAG=1 (set in LetsGoActivity alongside TEXT_DIAG).
    // Applies to both dialog VS variants. Interpretation (decisive):
    // - Centered magenta quads appear -> the real VS emits out-of-range
    //   positions (diverges from the static simulation) -> VS is the killer.
    // - Nothing appears -> the real VS emits in-range positions (the
    //   override did not fire) -> the VS is exonerated and the problem is
    //   downstream of the VS. A null result becomes positive information
    //   because the override would have forced quads if VS output were bad.
    // Clip-space comparisons (no division, NaN-safe via x!=x checks).
    // Names vsrd_* are block-scoped and unique to avoid collisions.
    char *vs_rangediag = NULL;
    if ((vs_fshash == 0x8e41cc9edf14ab8cULL || vs_fshash == 0xafd67833c2e8f5eeULL) && getenv("LARECOMP_VS_RANGE_DIAG") && p->vi && p->vi->msl) {
        const char *anchor = strstr(vs_src, "gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5f;");
        const char *eol = anchor ? strchr(anchor, '\n') : NULL;
        if (eol) {
            const char *ins = "  // VS_RANGE_DIAG v0090: force centered quad ONLY if computed gl_Position is out of clip range\n"
                              "  { vec4 vsrd_p = gl_Position;\n"
                              "    bool vsrd_bad = (vsrd_p.w <= 0.0) || (vsrd_p.x != vsrd_p.x) || (vsrd_p.y != vsrd_p.y) || (vsrd_p.z != vsrd_p.z) || (vsrd_p.w != vsrd_p.w)\n"
                              "        || (vsrd_p.x < -vsrd_p.w) || (vsrd_p.x > vsrd_p.w) || (vsrd_p.y < -vsrd_p.w) || (vsrd_p.y > vsrd_p.w) || (vsrd_p.z < 0.0) || (vsrd_p.z > vsrd_p.w);\n"
                              "    if (vsrd_bad) {\n"
                              "      vec2 vsrd_qp = vec2(0.5, 0.5);\n"
                              "      if (gl_VertexIndex == 0u) vsrd_qp = vec2(-0.5, -0.5);\n"
                              "      else if (gl_VertexIndex == 1u) vsrd_qp = vec2(0.5, -0.5);\n"
                              "      else if (gl_VertexIndex == 2u) vsrd_qp = vec2(-0.5, 0.5);\n"
                              "      gl_Position = vec4(vsrd_qp, 0.5, 1.0); } }\n";
            size_t srclen = strlen(vs_src), ilen = strlen(ins);
            size_t inspos = (size_t)(eol - vs_src) + 1;
            vs_rangediag = malloc(srclen + ilen + 1);
            if (vs_rangediag) {
                memcpy(vs_rangediag, vs_src, inspos);
                memcpy(vs_rangediag + inspos, ins, ilen);
                memcpy(vs_rangediag + inspos + ilen, vs_src + inspos, srclen - inspos + 1);
                vs_src = vs_rangediag;
                LOG("[shader] VS_RANGE_DIAG: conditional quad for dialog VS %016llx\n", (unsigned long long)vs_fshash);
            }
        }
    }
    uint32_t *sv = vk_glsl_to_spirv(vs_src, STAGE_VERTEX, "vs", &nv, vs_fshash);
    uint32_t *sf = sv ? vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, "fs", &nf, fs_fshash) : NULL;
    free(vs_diag);
    free(vs_a0fix);
    free(vs_rangediag);
    if (sv && getenv("LARECOMP_RENDERDOC_AT")) {  // which guest shader each SPIR-V is: tools/rdc_skin.py (FNV-1a 64)
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < nv * 4; i++) { h ^= ((const uint8_t *)sv)[i]; h *= 0x100000001B3ull; }
        pthread_mutex_lock(&lock);
        FILE *f = fopen("build/rdc-shaders.txt", "a");
        if (f) { fprintf(f, "%016llx vs %llx\n", (unsigned long long)h, (unsigned long long)p->code[0]); fclose(f); }
        pthread_mutex_unlock(&lock);
    }
    if (!sv || !sf) { free(sv); free(sf); atomic_store(&p->state, 2); return; }
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    p->vs = vk_module(sv, nv);
    p->fs = vk_module(sf, nf);
    free(sv);
    free(sf);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    atomic_fetch_add(&stat_spirv_ns, SDL_GetTicksNS() - t0);
    atomic_fetch_add(&stat_compiles, 1);
    atomic_store(&p->state, p->vs && p->fs ? 1 : 2);
}

static void translate_program(void *arg) {
    Prog *p = arg;
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    uint64_t t0 = SDL_GetTicksNS();
    p->vi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], STAGE_VERTEX, &p->vkey);
    // Experiment (research): LARECOMP_EXP_VS_EARLY=<vertex program address, hex>: that program's vertex shader returns at
    // the start of main (every output at its default), to tell the cost of running the shader from the rest of its draw.
    // (LARECOMP_EXP_VS_EARLY=<address>:<pc, hex>: it returns just before the instruction at that pc instead)
    if (p->vi->msl && getenv("LARECOMP_EXP_VS_EARLY") && p->code[0] == strtoull(getenv("LARECOMP_EXP_VS_EARLY"), NULL, 16)) {
        const char *m = strstr(p->vi->msl, "void main() {"), *r = m ? strstr(m, "  uint r") : NULL;
        const char *colon = strchr(getenv("LARECOMP_EXP_VS_EARLY"), ':');
        if (m && colon) {
            char pat[32];
            snprintf(pat, sizeof pat, "// pc %s:", colon + 1);
            const char *at = strstr(m, pat);
            while (at && at > m && at[-1] != '\n') at--;  // (the start of that line)
            r = at ? at : NULL;
        }
        if (r) {
            size_t len = strlen(p->vi->msl), pr = (size_t)(r - p->vi->msl);
            char *s = malloc(len + 16);
            memcpy(s, p->vi->msl, pr);
            memcpy(s + pr, "  return;\n", 10);
            memcpy(s + pr + 10, p->vi->msl + pr, len - pr + 1);
            p->vi->msl = s;
            LOG("[exp] vertex program %llx returns at the start of main\n", (unsigned long long)p->code[0]);
        }
    }
    // Experiment (research): LARECOMP_GLSL_OVERRIDE_DIR=<dir>: <dir>/<code address, hex>_vs.glsl replaces the generated GLSL
    // of that vertex program (a hand-edited version of what LARECOMP_SAVE_MSL=1 wrote in build/glsl/).
    if (p->vi->msl && getenv("LARECOMP_GLSL_OVERRIDE_DIR")) {
        char path[512];
        snprintf(path, sizeof path, "%s/%llx_vs.glsl", getenv("LARECOMP_GLSL_OVERRIDE_DIR"), (unsigned long long)p->code[0]);
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *s = malloc((size_t)n + 1);
            if (s && fread(s, 1, (size_t)n, f) == (size_t)n) { s[n] = 0; p->vi->msl = s; LOG("[exp] GLSL override: %s\n", path); }
            fclose(f);
        }
    }
    // Experiment X4 (docs/PLANO-3D.md), LARECOMP_VK_SKIN_ONEBONE=1: skinned vertex shaders see 100% weight on the
    // first bone (a12 = bone weights) - rigid, but whole, if the first bone's matrix reaches the GPU correctly.
    // Experiment X6, LARECOMP_VK_SKIN_IDX_X=1: every bone slot uses the first bone index (a11.x), weights unchanged.
    if (p->vi->msl && getenv("LARECOMP_VK_SKIN_IDX_X") && strstr(p->vi->msl, "in uvec4 a11;")) {
        const char *d = strstr(p->vi->msl, "in uvec4 a11;"), *m = strstr(p->vi->msl, "void main() {");
        if (d && m && d < m) {
            static const char def[] = "#define a11 uvec4(a11_in.x, a11_in.x, a11_in.x, a11_in.x)\n";
            size_t len = strlen(p->vi->msl), pd = (size_t)(d - p->vi->msl) + strlen("in uvec4 a11"), pm = (size_t)(m - p->vi->msl);
            char *s = malloc(len + 16 + sizeof def), *o = s;
            memcpy(o, p->vi->msl, pd); o += pd;
            memcpy(o, "_in", 3); o += 3;                       // in uvec4 a11_in;
            memcpy(o, p->vi->msl + pd, pm - pd); o += pm - pd;
            memcpy(o, def, sizeof def - 1); o += sizeof def - 1;
            memcpy(o, p->vi->msl + pm, len - pm + 1);
            p->vi->msl = s;
            g_exp_count[3]++;
        }
    }
    // Experiments X8+, LARECOMP_VK_SKIN_W="<GLSL vec4 expression of a12_in>": the bone weights a skinned vertex shader
    // sees, rewritten (a12_in = the weights as the GPU fetched them).
    if (p->vi->msl && getenv("LARECOMP_VK_SKIN_W") && strstr(p->vi->msl, "in vec4 a12;")) {
        const char *d = strstr(p->vi->msl, "in vec4 a12;"), *m = strstr(p->vi->msl, "void main() {");
        if (d && m && d < m) {
            char def[512];
            snprintf(def, sizeof def, "#define a12 (%s)\n", getenv("LARECOMP_VK_SKIN_W"));
            size_t dl = strlen(def), len = strlen(p->vi->msl), pd = (size_t)(d - p->vi->msl) + strlen("in vec4 a12"),
                   pm = (size_t)(m - p->vi->msl);
            char *s = malloc(len + 16 + dl), *o = s;
            memcpy(o, p->vi->msl, pd); o += pd;
            memcpy(o, "_in", 3); o += 3;                       // in vec4 a12_in;
            memcpy(o, p->vi->msl + pd, pm - pd); o += pm - pd;
            memcpy(o, def, dl); o += dl;
            memcpy(o, p->vi->msl + pm, len - pm + 1);
            p->vi->msl = s;
            g_exp_count[3]++;
        }
    }
    // (only skinned shaders - the ones with bone indices too: a12 is something else elsewhere.)
    if (p->vi->msl && getenv("LARECOMP_VK_SKIN_ONEBONE") && strstr(p->vi->msl, "in vec4 a12;") &&
        strstr(p->vi->msl, "in uvec4 a11;")) {
        const char *m = strstr(p->vi->msl, "void main() {");
        if (m) {
            static const char def[] = "#define a12 vec4(1.0, 0.0, 0.0, 0.0)\n";
            size_t pre = (size_t)(m - p->vi->msl), len = strlen(p->vi->msl);
            char *s = malloc(len + sizeof def);
            memcpy(s, p->vi->msl, pre);
            memcpy(s + pre, def, sizeof def - 1);
            memcpy(s + pre + sizeof def - 1, m, len - pre + 1);
            p->vi->msl = s;  // (the translator's string is kept: it lives as long as the program)
            g_exp_count[3]++;
        }
    }
    if (p->vi->msl && !p->code[1]) {  // vertex-only program (see get_program): an empty fragment shader
        p->fi = calloc(1, sizeof *p->fi);
        p->fi->stage = STAGE_FRAGMENT;
        p->fi->msl = strdup("#version 460\nvoid main() {}\n");
    } else if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        p->fi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1], STAGE_FRAGMENT, &p->fkey);
    }
    atomic_fetch_add(&stat_translate_ns, SDL_GetTicksNS() - t0);
    if (!p->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        atomic_store(&p->state, 2);
        return;
    }
    if (!p->fi->msl) {
        LOG("[shader] FS translate failed: %s\n", p->fi->error);
        atomic_store(&p->state, 2);
        return;
    }
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;
    save_glsl(p->vi->msl, p->code[0], "vs");
    save_glsl(p->fi->msl, p->code[1], "fs");
    compile_program(p);  // (this worker: queued again it would wait behind the other translations)
}

#define PROG_TAB 4096
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

// BindProgram(program, stages): the stages in the mask come from `program` (NULL: unbound), the others stay. A bind
// that leaves one program's stages and nothing else uses that program itself (as before); a mix of stages from
// several programs (Let's Go: vertex and fragment bound separately) uses a combined program. The shader caches are
// keyed by code address, so the combination needs no identity of its own.
static NvnProgram g_comb_prog;
static void bind_program_stages(NvnProgram *p, uint32_t mask) {
    if (!mask) mask = p ? p->stage_mask : 0x3F;  // (no mask recorded: the whole program)
    NvnProgram *cur = st.program;
    NvnProgram next = cur ? *cur : (NvnProgram){0};
    next.backend = NULL;
    for (int i = 0; i < 6; i++) {
        uint32_t bit = 1u << i;
        if (!(mask & bit)) continue;
        if (p && (p->stage_mask & bit)) next.stages[i] = p->stages[i], next.stage_mask |= bit;
        else memset(&next.stages[i], 0, sizeof next.stages[i]), next.stage_mask &= ~bit;
    }
    if (!next.stage_mask) { st.program = NULL; return; }
    if (p && next.stage_mask == p->stage_mask &&
        !memcmp(next.stages, p->stages, sizeof next.stages)) { st.program = p; return; }
    g_comb_prog = next;
    st.program = &g_comb_prog;
}

static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    // A vertex shader without a fragment shader (Let's Go's depth / shadow passes: stages 0x1) gets an empty fragment
    // shader in translate_program - no color output, only depth, as the hardware does. They were all skipped.
    if (!(np->stage_mask & 1)) {
        static uint32_t seen;
        if (np->stage_mask < 32 && !(seen & (1u << np->stage_mask))) {
            seen |= 1u << np->stage_mask;
            LOG("[shader] draw with stages %#x (needs vertex + fragment): skipped\n", np->stage_mask);
        }
        return NULL;
    }
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = np->stages[i].code_addr; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) return prog_tab[slot].prog;
    Prog *p = calloc(1, sizeof *p);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    for (int i = 0; i < 2; i++) {
        p->code[i] = np->stages[i].code_addr;
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    run_job(translate_program, p);
    return p;
}

// ================================================================== vertex formats / fixed function tables
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x51: return VK_FORMAT_R8_UINT;
    default: {
        static uint8_t told[256];
        if (f < 256 && !told[f]++) LOG("[vk] vertex format %#x unknown: read as RGBA32F\n", f);
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in, captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill;
    uint32_t write_mask[8];
} PipeKey;

typedef struct { PipeKey k; uint64_t hash; VkPipeline pipe; _Atomic int state; } Pipe;  // state: 0 building, 1 ready, 2 failed
#define PIPE_TAB 8192
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

// LARECOMP_PIPE_STATS=<vertex program address, hex>|all: after each pipeline is created, what the driver reports about its
// compiled shaders (VK_KHR_pipeline_executable_properties: one executable per stage; registers, spills, instructions...).
static void pipe_exec_report(VkPipeline pipe, uint64_t vs, uint64_t fs) {
    const char *want = getenv("LARECOMP_PIPE_STATS");
    if (!want || (strcmp(want, "all") && strtoull(want, NULL, 16) != vs)) return;
    static PFN_vkGetPipelineExecutablePropertiesKHR get_props;
    static PFN_vkGetPipelineExecutableStatisticsKHR get_stats;
    if (!get_props) {
        get_props = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetPipelineExecutablePropertiesKHR");
        get_stats = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetPipelineExecutableStatisticsKHR");
        if (!get_props || !get_stats) { LOG("[pipestats] the driver does not export the functions\n"); return; }
    }
    uint32_t n = 0;
    VkPipelineInfoKHR pinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pipe};
    if (get_props(g_vk.dev, &pinfo, &n, NULL) != VK_SUCCESS || !n) return;
    VkPipelineExecutablePropertiesKHR props[8] = {0};
    if (n > 8) n = 8;
    for (uint32_t i = 0; i < n; i++) props[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR;
    if (get_props(g_vk.dev, &pinfo, &n, props) != VK_SUCCESS) return;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ns = 0;
        VkPipelineExecutableInfoKHR einfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = pipe, .executableIndex = i};
        if (get_stats(g_vk.dev, &einfo, &ns, NULL) != VK_SUCCESS) continue;
        VkPipelineExecutableStatisticKHR stats[64] = {0};
        if (ns > 64) ns = 64;
        for (uint32_t s = 0; s < ns; s++) stats[s].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR;
        if (get_stats(g_vk.dev, &einfo, &ns, stats) != VK_SUCCESS) continue;
        char line[1024];
        int l = snprintf(line, sizeof line, "[pipestats] vs %llx fs %llx  %s (stages %#x):", (unsigned long long)vs, (unsigned long long)fs,
                         props[i].name, props[i].stages);
        for (uint32_t s = 0; s < ns && l < (int)sizeof line - 80; s++) {
            const VkPipelineExecutableStatisticValueKHR *v = &stats[s].value;
            switch (stats[s].format) {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: l += snprintf(line + l, sizeof line - l, " %s=%u;", stats[s].name, v->b32); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: l += snprintf(line + l, sizeof line - l, " %s=%lld;", stats[s].name, (long long)v->i64); break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: l += snprintf(line + l, sizeof line - l, " %s=%llu;", stats[s].name, (unsigned long long)v->u64); break;
            default: l += snprintf(line + l, sizeof line - l, " %s=%g;", stats[s].name, v->f64); break;
            }
        }
        LOG("%s\n", line);
    }
}

static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    PipeKey *k = &pp->k;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->prog->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->prog->fs, .pName = "main"}};
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    {   // (LARECOMP_PIPE_STATS: the vertex fetch layout of the program asked for)
        const char *w = getenv("LARECOMP_PIPE_STATS");
        if (w && (!strcmp(w, "all") || strtoull(w, NULL, 16) == k->prog->code[0])) {
            LOG("[pipestats] vs %llx inputs:", (unsigned long long)k->prog->code[0]);
            for (uint32_t i = 0; i < na; i++) LOG(" a%u(bind %u fmt %d off %u)", va[i].location, va[i].binding, (int)va[i].format, va[i].offset);
            LOG(" | bindings:");
            for (uint32_t i = 0; i < nb; i++)
                LOG(" %u(stride %u %s)", vb[i].binding, vb[i].stride, vb[i].inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? "instance" : "vertex");
            for (uint32_t i = 0; i < nd; i++) LOG(" div%u=%u", vd[i].binding, vd[i].divisor);
            LOG("\n");
        }
    }
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->prog->layout};
    uint64_t tp = SDL_GetTicksNS();
    extern bool g_pipe_exec_stats;
    if (g_pipe_exec_stats) pi.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    // MISSÃO 1 v0068: log detalhado antes de vkCreateGraphicsPipelines
    // Para identificar o pipeline que causa SIGSEGV no driver Qualcomm
    static _Atomic uint64_t pipe_attempt_id = 0;
    uint64_t attempt = atomic_fetch_add(&pipe_attempt_id, 1);
    {
        // Escreve em arquivo persistente com flush imediato
        const char *root = getenv("LARECOMP_DATA_ROOT");
        char path[1024];
        if (root && root[0]) snprintf(path, sizeof(path), "%s/pipeline_create.log", root);
        else snprintf(path, sizeof(path), "pipeline_create.log");
        FILE *pf = fopen(path, "a");
        if (pf) {
            // Thread ID
            unsigned long tid = (unsigned long)pthread_self();
            // PIPELINE_CREATE_BEGIN marker
            fprintf(pf, "PIPELINE_CREATE_BEGIN id=%llu thread=%lu\n", (unsigned long long)attempt, tid);
            // Shader identifiers (Maxwell code addresses)
            fprintf(pf, "  vs_code=%016llx fs_code=%016llx\n",
                    (unsigned long long)k->prog->code[0], (unsigned long long)k->prog->code[1]);
            // MISSÃO 1 v0073: fshash (mesma fórmula do [font-draw]) para vínculo com o draw
            uint64_t vs_fsh = k->prog->code[0] ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(k->prog->code[0] + 0x30), 0x200) : 0;
            uint64_t fs_fsh = k->prog->code[1] ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(k->prog->code[1] + 0x30), 0x200) : 0;
            fprintf(pf, "  vs_fshash=%016llx fs_fshash=%016llx\n",
                    (unsigned long long)vs_fsh, (unsigned long long)fs_fsh);
            // Color attachments and depth/stencil formats
            fprintf(pf, "  ncolor=%u", k->ncolor);
            for (uint32_t i = 0; i < k->ncolor && i < 8; i++) fprintf(pf, " color%u=%d", i, (int)k->color[i]);
            fprintf(pf, " depth=%d stencil=%d\n", (int)k->depth, (int)k->stencil);
            // Sample count and topology
            fprintf(pf, "  samples=%u topology=%d\n", (unsigned)k->samples, (int)k->topo_class);
            // Blend state
            fprintf(pf, "  blend_on=0x%x\n", (unsigned)k->blend_on);
            // Dynamic states count
            fprintf(pf, "  ndyn=%u\n", (unsigned)NDYN);
            // FRENTE 4 v0076: cull mode (0=NONE, 1=FRONT, 2=BACK, 3=FRONT_AND_BACK)
            fprintf(pf, "  cullMode=%d\n", (int)rs.cullMode);
            // Pipeline cache state
            fprintf(pf, "  pcache=%p\n", (void*)g_vk.pcache);
            // Build identifier
            fprintf(pf, "  build=v0076\n");
            fflush(pf);
            fclose(pf);
        }
    }
    // v0074: bypass do pipeline cache para o shader diagnóstico
    // Garante que o pipeline usa o SPIR-V com magenta, não uma variante em cache
    VkPipelineCache use_pcache = g_vk.pcache;
    {
        static int diag_cache_checked = -1;
        if (diag_cache_checked < 0) {
            const char *d = getenv("LARECOMP_TEXT_DIAG");
            diag_cache_checked = (d && atoi(d) != 0) ? 1 : 0;
        }
        if (diag_cache_checked) {
            uint64_t fs_fsh = k->prog->code[1] ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(k->prog->code[1] + 0x30), 0x200) : 0;
            if (fs_fsh == 0x6443b63b341c73caULL) {
                use_pcache = VK_NULL_HANDLE;
                LOG("[pipeline] DIAG: bypassing pipeline cache for fshash %016llx (fresh pipeline with diagnostic shader)\n", (unsigned long long)fs_fsh);
            }
        }
    }
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, use_pcache, 1, &pi, NULL, &pp->pipe);
    {
        // PIPELINE_CREATE_END marker
        const char *root = getenv("LARECOMP_DATA_ROOT");
        char path[1024];
        if (root && root[0]) snprintf(path, sizeof(path), "%s/pipeline_create.log", root);
        else snprintf(path, sizeof(path), "pipeline_create.log");
        FILE *pf = fopen(path, "a");
        if (pf) {
            uint64_t dur = SDL_GetTicksNS() - tp;
            fprintf(pf, "PIPELINE_CREATE_END id=%llu result=%d duration_ns=%llu\n",
                    (unsigned long long)attempt, (int)r, (unsigned long long)dur);
            fflush(pf);
            fclose(pf);
        }
    }
    atomic_fetch_add(&stat_pipe_ns, SDL_GetTicksNS() - tp);
    if (r == VK_SUCCESS && g_pipe_exec_stats) pipe_exec_report(pp->pipe, k->prog->code[0], k->prog->code[1]);
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
    atomic_fetch_add(&stat_compiles, 1);
    if (r == VK_SUCCESS) atomic_fetch_add(&new_pipes, 1);
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static int nfeedback;
static uint64_t g_cur_fs_hash;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

typedef struct { NvnTexture *key; TexBackend copy; } Snap;
static Snap snaps[16];
static int nsnaps;

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t;
    }
    TexBackend *d = &s->copy;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);  // (resolution change)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || b->format != VK_FORMAT_D32_SFLOAT_S8_UINT) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("LARECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing) {
                feedback_tex[nfeedback] = t;
                feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
                continue;
            }
            stat_feedback_copies++;
            vk_end_pass();
            TexBackend *dst = snapshot_image(t, src);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1},
                              .extent = {src->width, src->height, 1}};
            vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            feedback_tex[nfeedback] = t;
            feedback_view[nfeedback++] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;
        }
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    bool skinned_vs = stage == 0 && ((si->attr_mask >> 11) & 1);  // (Let's Go: a11 = bone indices)
    // Experiment X2 (docs/PLANO-3D.md), LARECOMP_VK_SKIN_IDENTITY=1: a skinned vertex shader's dynamically indexed
    // cbuf (its bone palette) reads identity matrices - models in their bind pose if the rest of the chain is right.
    static int skin_identity = -1;
    if (skin_identity < 0) skin_identity = getenv("LARECOMP_VK_SKIN_IDENTITY") != NULL;
    if (skin_identity && skinned_vs && (si->cbuf_dynamic & (1u << k))) {
        static float ident[0x10000 / 4];
        if (ident[0] != 1.0f)
            for (int m = 0; m < 0x10000 / 64; m++) ident[m * 16] = ident[m * 16 + 5] = ident[m * 16 + 10] = ident[m * 16 + 15] = 1.0f;
        g_exp_count[0]++;
        return ubo_span(vk_transient_copy(ident, size, 256), size);
    }
    // LARECOMP_VK_UBO_DIRECT=1 (experiment X3): bind the range in place in the imported guest heap, read when the GPU
    // runs - as on the console - in case compute shaders write it after this point. Counted: direct vs copied.
    static int direct = -1;
    if (direct < 0) direct = getenv("LARECOMP_VK_UBO_DIRECT") ? atoi(getenv("LARECOMP_VK_UBO_DIRECT")) : 0;
    if (direct) {
        extern bool vk_guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off);
        VkBuffer gb;
        VkDeviceSize goff;
        uint64_t align = g_vk.props.limits.minUniformBufferOffsetAlignment;
        if (!(r.addr % (align ? align : 256)) && vk_guest_direct(r.addr, size, &gb, &goff)) {
            if (skinned_vs) g_exp_count[1]++;
            return ubo_span((VkSpan){.buf = gb, .off = goff}, size);
        }
        if (skinned_vs) g_exp_count[2]++;
    }
    if (ip_enabled()) {  // frame interpolation: recorded, or the blended copy in the replay
        const void *d = ip_cbuf(stage, k, r.addr, (uint32_t)size);
        if (d) return ubo_span(vk_transient_copy(d, size, 256), size);
    }
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    return ubo_span(guest_copy(r.addr, size), size);
}

// Global memory (shader LDG/STG, maxwell_gmem_base = HEAP_BASE): the guest heap imported as a GPU buffer
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so compute results land in guest
// memory. The import pins its pages, so it covers only the heap the game set up (SetHeapSize), made on first use.
// LARECOMP_VK_NO_GMEM=1: no import, reads return zero.
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;
// GWIN (ported from BOTWRecompAndroid): when the heap is larger than one storage buffer reaches
// (maxStorageBufferRange, 2 GB on Adreno), draws use a window of the heap plus the GMap table.
static bool gwin_mode;
static uint32_t gmap_tab[(GMAP_MAX + 1) * 4];  // this draw's table: [i] = heap offset, size, word offset; [GMAP_MAX].x = count
static VkDescriptorBufferInfo gfast_win;  // the heap window bound as GFast (buffer set: window of the heap)
// GCOPY (ported from BOTWRecompAndroid): the system Adreno driver has no VK_EXT_external_memory_host,
// so the guest heap is never imported and shader global reads return zero (the text VS reads glyph data
// from gmem: all vertices degenerate, no fragments, no text, no magenta). When host import is unavailable,
// copy each draw's storage-buffer ranges from CPU memory to a transient GPU buffer and let the shader
// reach them through the GMap table (same mechanism as gwin, but with a copy instead of a window).
static bool gcopy_mode;  // decided lazily from g_vk.host_import (device init runs before draws)
static VkSpan gc_span;
static uint64_t gc_size;
uint64_t g_text_fshash;  // v0080: fshash of current draw's fragment shader (for gcopy logging)

static bool gmem_import(uint64_t size) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (!props || props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)HEAP_BASE, &mp) != VK_SUCCESS ||
        !mp.memoryTypeBits)
        return false;
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)HEAP_BASE};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, &mem) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};  // (uniforms read in place: cbuf_info)
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (gmem_buf) {  // the heap grew: the old import may still be in use by submitted work
        vk_wait_seq(submit_seq);
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
    }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    // GWIN: a storage buffer's range is limited (maxStorageBufferRange, 2 GB on Adreno): if the
    // heap is larger, a vertex shader's global loads past that read zero (BOTW's text, whose
    // glyphs a vertex shader reads from a buffer ~2.7 GB into the heap, didn't show). Draws then
    // use a window of the heap plus the GMap table (gwin_prepare). LARECOMP_VK_NO_GWIN=1 disables.
    gwin_mode = size > g_vk.props.limits.maxStorageBufferRange && !getenv("LARECOMP_VK_NO_GWIN");
    maxwell_gmem_map_all = gwin_mode;
    LOG("[vk] guest heap imported for shader global memory: %llu MB (storage range %llu MB)%s\n",
        (unsigned long long)(size >> 20), (unsigned long long)(g_vk.props.limits.maxStorageBufferRange >> 20),
        gwin_mode ? "; draws reach global memory through windows (gwin)" : "");
    return true;
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)

static VkDescriptorBufferInfo gmem_info(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_VK_NO_GMEM") != NULL || !g_vk.host_import;
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    if (want > HEAP_MAX) want = HEAP_MAX;
    if (!off && want > gmem_size && !gmem_import(want)) {
        LOG("[vk] guest heap import failed (%llu MB): shader global memory reads return zero\n", (unsigned long long)(want >> 20));
        off = 1;
    }
    if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// GWIN (ported from BOTWRecompAndroid): the heap is larger than one storage buffer reaches
// (maxStorageBufferRange, 2 GB on Adreno; BOTW's heap is ~3 GB): a vertex or fragment shader's
// global loads past that read zero - BOTW's text, whose glyphs a vertex shader reads from a
// buffer ~2.7 GB into the heap, didn't show. Draws then work like a window: the table (GMap)
// lists the storage buffers bound to the draw's stages (with a 4 KB margin before each), all
// inside one window of the heap that starts at the first of them (the GFast descriptor:
// gmem_buf from that offset). Accesses outside the table go to the heap from its start, as before.
typedef struct { uint64_t lo, hi; } GwinRange;  // heap offsets, 256-aligned
static void gwin_prepare(unsigned stage_mask) {
    memset(gmap_tab, 0, sizeof gmap_tab);
    gmem_info();
    uint64_t reach = g_vk.props.limits.maxStorageBufferRange & ~3ull;
    gfast_win = (VkDescriptorBufferInfo){gmem_buf, 0, reach < gmem_size ? reach : gmem_size};
    if (!gmem_buf) return;
    GwinRange r[6 * 16];
    int n = 0;
    for (int s = 0; s < 6; s++) {
        if (!(stage_mask & (1u << s))) continue;
        for (int i = 0; i < 16; i++) {  // heap offsets, 256-aligned, 4 KB of margin before
            uint64_t a = st.sbuf[s][i].addr, sz = st.sbuf[s][i].size;
            if (!a || !sz || a < HEAP_BASE || a - HEAP_BASE + sz > gmem_size) continue;
            uint64_t lo = (a - HEAP_BASE) & ~255ull, hi = (a - HEAP_BASE + sz + 255) & ~255ull;
            lo = lo > 4096 ? (lo - 4096) & ~255ull : 0;
            if (hi > gmem_size) hi = gmem_size;
            int k = n++;
            while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }
            r[k].lo = lo; r[k].hi = hi;
        }
    }
    int m = 0;
    for (int i = 0; i < n; i++) {  // merged
        if (m && r[i].lo <= r[m - 1].hi + 4096) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; }
        else r[m++] = r[i];
    }
    if (!m) return;
    uint64_t al = g_vk.props.limits.minStorageBufferOffsetAlignment ? g_vk.props.limits.minStorageBufferOffsetAlignment : 256;
    uint64_t base = r[0].lo / al * al;
    if (reach > gmem_size - base) reach = gmem_size - base;
    int k = 0;
    for (int i = 0; i < m && k < GMAP_MAX; i++) {
        if (r[i].hi - base > reach) break;  // (out of the window's reach: the heap from its start, as before)
        gmap_tab[k * 4 + 0] = (uint32_t)r[i].lo;
        gmap_tab[k * 4 + 1] = (uint32_t)(r[i].hi - r[i].lo);
        gmap_tab[k * 4 + 2] = (uint32_t)((r[i].lo - base) >> 2);
        k++;
    }
    gmap_tab[GMAP_MAX * 4] = (uint32_t)k;
    gfast_win = (VkDescriptorBufferInfo){gmem_buf, base, reach};
    static int logged = 0;
    if (!logged) {
        logged = 1;
        LOG("[gwin] window base=%llu MB reach=%llu MB entries=%d (heap %llu MB, storage range %llu MB)\n",
            (unsigned long long)(base >> 20), (unsigned long long)(reach >> 20), k,
            (unsigned long long)(gmem_size >> 20), (unsigned long long)(g_vk.props.limits.maxStorageBufferRange >> 20));
    }
}

// GCOPY: no heap import (system Adreno driver): copy this draw's storage-buffer ranges from CPU
// memory into a transient GPU buffer; the shader reaches them through gmap_tab (gfind) bound as
// GFast. Read-only: text/glyph loads are reads; stores keep the old path (gmem dummy = dropped).
// Ranges beyond CHUNK_SIZE (64 MB) are skipped: those loads read zero, as without this.
#define GCOPY_CHUNK_SIZE (64ull << 20)
static void gcopy_prepare(unsigned stage_mask) {
    memset(gmap_tab, 0, sizeof gmap_tab);
    gfast_win = (VkDescriptorBufferInfo){0};
    gc_size = 0;
    GwinRange r[6 * 16];
    int n = 0;
    static long margin = -1;
    if (margin < 0) { const char *e = getenv("LARECOMP_GMARGIN"); margin = e ? atol(e) : 4096; }
    for (int s = 0; s < 6; s++) {
        if (!(stage_mask & (1u << s))) continue;
        for (int i = 0; i < 16; i++) {  // heap offsets, 256-aligned, margin before each range
            uint64_t a = st.sbuf[s][i].addr, sz = st.sbuf[s][i].size;
            if (!a || !sz || a < HEAP_BASE || a - HEAP_BASE + sz > HEAP_MAX) continue;
            uint64_t lo = (a - HEAP_BASE) & ~255ull, hi = (a - HEAP_BASE + sz + 255) & ~255ull;
            lo = lo > (uint64_t)margin ? (lo - (uint64_t)margin) & ~255ull : 0;
            // v0084: trailing margin (same 4 KB as the leading one). Shaders stride
            // past the BOUND size: the dialog VS reads instance*0x60 bytes from the
            // sbuf base, but the bound size is only ~64 bytes/instance (576 for 9
            // instances, 1280 for 20). Without a trailing margin, the last instances'
            // reads fall outside the window -> gfind fails -> fallback to the dummy
            // zero buffer -> degenerate quads. A 4 KB trailing margin covers up to
            // ~125 instances (need inst*96+80 <= inst*64+4096).
            hi = (hi + (uint64_t)margin) & ~255ull;
            if (hi > HEAP_MAX) hi = HEAP_MAX;
            int k = n++;
            while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }
            r[k].lo = lo; r[k].hi = hi;
        }
    }
    // v0079 Path 2: scan constant buffers for heap addresses (the text VS reads glyph
    // data from an address stored in CB0[17], not from a bound storage buffer — so the
    // sbuf loop above never sees it). For each stage, read CB0 and look for 64-bit
    // values pointing into the heap; add a 256 KB window around each.
    // FIX v0080: read the FULL CB size from guest memory, not just the bound size.
    // The game binds CB0 with only 128 bytes (8 uvec4), but the shader reads CB0[17]
    // at offset 272 bytes. The full CB (512 bytes for 32 uvec4) is in guest memory
    // even if only part is bound — reading beyond cb_size finds the glyph address.
    for (int s = 0; s < 6 && n < 6 * 16; s++) {
        if (!(stage_mask & (1u << s))) continue;
        for (int c = 0; c < 18 && n < 6 * 16; c++) {
            uint64_t cb_addr = st.ubuf[s][c].addr, cb_size = st.ubuf[s][c].size;
            if (!cb_addr || cb_size < 16 || cb_addr < HEAP_BASE) continue;
            // Read up to 512 bytes (full CB0 with 32 uvec4) from guest memory,
            // even if the bound size is smaller. The CB data is in the heap mapping.
            uint64_t read_size = cb_size < 512 ? 512 : cb_size;
            if (cb_addr - HEAP_BASE + read_size > HEAP_MAX) read_size = HEAP_MAX - (cb_addr - HEAP_BASE);
            if (read_size < 16) continue;
            // CB data is in game memory, readable directly (guest address mapped in-process)
            const uint64_t *words = (const uint64_t *)(uintptr_t)cb_addr;
            uint64_t nwords = read_size / 8;
            if (nwords > 64) nwords = 64;  // cap: 512 bytes = 64 words (full CB0)
            for (uint64_t w = 0; w < nwords && n < 6 * 16; w++) {
                uint64_t a = words[w];
                // Must be a heap address (aligned, within range, not the CB itself)
                if (a < HEAP_BASE || a - HEAP_BASE >= HEAP_MAX || a == cb_addr) continue;
                if (a & 15) continue;  // must be 16-aligned (uvec4 slots)
                uint64_t lo = (a - HEAP_BASE) & ~255ull;
                uint64_t hi = lo + (256 << 10);  // 256 KB window
                if (hi > HEAP_MAX) hi = HEAP_MAX;
                // Skip if already covered by an existing range
                int covered = 0;
                for (int j = 0; j < n; j++) {
                    if (lo >= r[j].lo && hi <= r[j].hi) { covered = 1; break; }
                }
                if (covered) continue;
                int k = n++;
                while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }
                r[k].lo = lo; r[k].hi = hi;
            }
        }
    }
    int m = 0;
    for (int i = 0; i < n; i++) {  // merged (nearby ranges too: a table entry costs more than a small gap)
        if (m && r[i].lo <= r[m - 1].hi + 4096) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; }
        else r[m++] = r[i];
    }
    if (m > GMAP_MAX) {  // more ranges than the table holds: one covering them all
        r[0].hi = r[m - 1].hi;
        m = 1;
    }
    uint64_t total = 0;
    for (int i = 0; i < m; i++) total += (r[i].hi - r[i].lo + 255) & ~255ull;
    if (!m || total > GCOPY_CHUNK_SIZE) return;  // too much: those loads read zero, as without this
    gc_span = vk_transient(total, 256);
    gc_size = total;
    uint64_t off = 0;
    for (int i = 0; i < m; i++) {
        uint64_t len = r[i].hi - r[i].lo;
        memcpy((uint8_t *)gc_span.ptr + off, (const void *)(uintptr_t)(HEAP_BASE + r[i].lo), len);
        gmap_tab[i * 4 + 0] = (uint32_t)r[i].lo;
        gmap_tab[i * 4 + 1] = (uint32_t)len;
        gmap_tab[i * 4 + 2] = (uint32_t)(off >> 2);
        off += (len + 255) & ~255ull;
    }
    gmap_tab[GMAP_MAX * 4] = (uint32_t)m;
    gfast_win = (VkDescriptorBufferInfo){gc_span.buf, gc_span.off, gc_size};
    // v0080: log every gcopy for the text shader (fshash 6443b63b341c73ca), not just
    // once — the previous once-only log hid whether the text draw's gcopy worked.
    // The caller (draw_) sets g_text_fshash before calling gcopy_prepare.
    // v0081: also flag the MENU shader (7270eb69fbecccee) — its VS does not use gmem,
    // so gcopy should NOT run for it; the flag confirms that (or reveals it does).
    extern uint64_t g_text_fshash;
    static int logged = 0;
    int is_text = (g_text_fshash == 0x6443b63b341c73caULL || g_text_fshash == 0x7270eb69fbeccceeULL);
    if (!logged || is_text) {
        if (!logged) logged = 1;
        LOG("[gcopy] %d ranges %.1f KB copied to GPU buffer (no heap import)%s\n", m, total / 1024.0,
            is_text ? " [TEXT]" : "");
    }
    // v0084: decisive diagnostic for the DIALOG shader only (first 5 draws).
    // The static chain is fully verified (cb0[17] correct, gfind math correct,
    // bindings correct, order correct, TEXT_DIAG correct) yet dialog draws
    // produce zero fragments. The only unverified link is runtime DATA: does
    // the gfast window hold real glyph data or zeros, and does the gmap table
    // cover the VS read addresses? Log both, straight from the copied window.
    if (g_text_fshash == 0x6443b63b341c73caULL) {
        static int diag_n = 0;
        if (diag_n < 5) {
            diag_n++;
            LOG("[gcopy-diag] dialog draw #%d: %d ranges, sbuf0_0=%llx+%llu, gfast_buf=%p\n",
                diag_n, m, (unsigned long long)st.sbuf[0][0].addr,
                (unsigned long long)st.sbuf[0][0].size, (void *)gfast_win.buffer);
            for (int i = 0; i < m; i++)
                LOG("[gcopy-diag]   range %d: heap_off=0x%x len=%u word_off=%u\n",
                    i, gmap_tab[i * 4 + 0], gmap_tab[i * 4 + 1], gmap_tab[i * 4 + 2]);
            if (gc_span.ptr && gc_size >= 32) {
                const uint32_t *w = (const uint32_t *)gc_span.ptr;
                LOG("[gcopy-diag]   window head words: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                    w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                // Words at the sbuf base offset inside the window (where the VS
                // reads instance 0): window offset = (sbuf heap off - range lo).
                // Find the range containing sbuf[0][0] and dump its first words.
                uint64_t sb = st.sbuf[0][0].addr - HEAP_BASE;
                for (int i = 0; i < m; i++) {
                    uint64_t lo = gmap_tab[i * 4 + 0], len = gmap_tab[i * 4 + 1];
                    if (sb >= lo && sb < lo + len) {
                        const uint32_t *sw = w + gmap_tab[i * 4 + 2] + (uint32_t)((sb - lo) >> 2);
                        LOG("[gcopy-diag]   sbuf-base words (inst0 glyph data): %08x %08x %08x %08x %08x %08x %08x %08x\n",
                            sw[0], sw[1], sw[2], sw[3], sw[4], sw[5], sw[6], sw[7]);
                        // v0085: words the dialog VS actually reads for instance 0.
                        // The sbuf-base words above are offsets 0x00-0x1c; positions
                        // come from 0x10/0x20/0x30/0x40/0x50 within the 0x60 block.
                        // If these are zero while the base is real, the glyph layout
                        // data itself is missing (game writes it elsewhere/later).
                        size_t nwords = gc_size / 4;
                        size_t base_w = (size_t)(sw - w);
                        static const int offs[] = {0x10, 0x20, 0x30, 0x40, 0x50};
                        for (int j = 0; j < 5; j++) {
                            size_t rw = base_w + (size_t)(offs[j] >> 2);
                            if (rw + 4 <= nwords)
                                LOG("[gcopy-diag]   inst0+%#x: %08x %08x %08x %08x\n",
                                    offs[j], w[rw], w[rw + 1], w[rw + 2], w[rw + 3]);
                        }
                        size_t i1 = base_w + (0x60 >> 2);
                        if (i1 + 4 <= nwords)
                            LOG("[gcopy-diag]   inst1 base: %08x %08x %08x %08x\n",
                                w[i1], w[i1 + 1], w[i1 + 2], w[i1 + 3]);
                        break;
                    }
                }
            } else {
                LOG("[gcopy-diag]   window EMPTY (no copy happened!)\n");
            }
        }
        // v0087: data-stability probe beyond the first 5 draws. The first-5 dump
        // showed sbuf+0x20/+0x24 = 0xff232323 (color-like, -1.9e38 as float) and
        // +0x44 = 0 (kills the a0[1] variation) — if this data is STALE (game
        // writes the real layout later, or via STG which gcopy drops), later
        // draws/frames may show different values. Log one line every 200th
        // dialog draw so stability across the whole session is visible without
        // spamming the log. If +0x20 stays 0xff232323 forever, the block the
        // game bound is not a valid glyph layout for this VS (wrong sbuf, or
        // the layout is written elsewhere); if it becomes small floats, the
        // copy is racing the game's write.
        {
            static int diag_all = 0;
            if ((++diag_all % 200) == 0 && gc_span.ptr && gc_size >= 32) {
                const uint32_t *w = (const uint32_t *)gc_span.ptr;
                uint64_t sb = st.sbuf[0][0].addr - HEAP_BASE;
                for (int i = 0; i < m; i++) {
                    uint64_t lo = gmap_tab[i * 4 + 0], len = gmap_tab[i * 4 + 1];
                    if (sb >= lo && sb < lo + len) {
                        const uint32_t *sw = w + gmap_tab[i * 4 + 2] + (uint32_t)((sb - lo) >> 2);
                        LOG("[gcopy-diag]   stability #%d: +0x20=%08x %08x +0x44=%08x +0x40=%08x sbuf=%llx\n",
                            diag_all, sw[8], sw[9], sw[17], sw[16],
                            (unsigned long long)st.sbuf[0][0].addr);
                        break;
                    }
                }
            }
        }
    }
}

// This draw's gmap table in transient memory, shared with the draws of the submission that
// have the same table (dedupe: the table rarely changes between draws of one submission).
static VkSpan gmap_copy(void) {
    static struct { uint64_t seq; uint32_t tab[(GMAP_MAX + 1) * 4]; VkSpan span; } c[8];
    static int next;
    uint64_t seq = submit_seq;
    for (int i = 0; i < 8; i++)
        if (c[i].seq == seq && !memcmp(c[i].tab, gmap_tab, sizeof c[i].tab)) return c[i].span;
    VkSpan sp = vk_transient_copy(gmap_tab, sizeof c[0].tab, 256);
    c[next].seq = seq;
    memcpy(c[next].tab, gmap_tab, sizeof c[next].tab);
    c[next].span = sp;
    next = (next + 1) & 7;
    return sp;
}

// Guest memory ranges the GPU may write: the storage buffers a compute dispatch binds (the game's compute shaders produce vertex
// data for some draws: a 16-byte stream here, a few more there). Vertex or index data inside one of them is read in place when
// the GPU runs, as the game expects, and is never copied when the draw is recorded (the copy would hold what the GPU has not
// written yet). Sorted, merged intervals; the render thread is the only user.
static struct { uint64_t a, e; } gpuw[512];
static int ngpuw;
static void gpuw_note(uint64_t a, uint64_t n) {
    uint64_t e = a + (n ? n : 16);
    int i = 0, j;
    while (i < ngpuw && gpuw[i].e < a) i++;
    for (j = i; j < ngpuw && gpuw[j].a <= e; j++) {
        if (gpuw[j].a < a) a = gpuw[j].a;
        if (gpuw[j].e > e) e = gpuw[j].e;
    }
    if (j > i) {  // merged with [i, j)
        if (i + 1 == j && gpuw[i].a == a && gpuw[i].e == e) return;  // (known)
        gpuw[i].a = a; gpuw[i].e = e;
        memmove(&gpuw[i + 1], &gpuw[j], (size_t)(ngpuw - j) * sizeof gpuw[0]);
        ngpuw -= j - i - 1;
    } else {
        if (ngpuw == 512) { if (gpuw[ngpuw - 1].e < e) gpuw[ngpuw - 1].e = e; return; }  // (full: widen the last, stays safe)
        memmove(&gpuw[i + 1], &gpuw[i], (size_t)(ngpuw - i) * sizeof gpuw[0]);
        gpuw[i].a = a; gpuw[i].e = e;
        ngpuw++;
    }
    if (ENV("LARECOMP_LOG_GPUWRITES")) LOG("[gpuw] the GPU may write %llx-%llx (%d ranges)\n", (unsigned long long)a, (unsigned long long)e, ngpuw);
}
// A shader that stores to global memory with no storage buffer bound writes through pointers we cannot see: from then on every
// vertex / index range is read in place, as before the VRAM ring (slower, never stale).
static bool gpuw_unknown;
static void gpuw_stores(const ShaderInfo *si, const Range *sbufs, int n, const char *what, uint64_t code) {
    if (!si || !si->gmem_stores) return;
    bool any = false;
    for (int i = 0; i < n; i++) if (sbufs[i].addr) { gpuw_note(sbufs[i].addr, sbufs[i].size); any = true; }
    if (!any && !gpuw_unknown) {
        gpuw_unknown = true;
        LOG("[vk] %s %llx stores to global memory without a storage buffer: vertex and index data is read in place from now on\n", what, (unsigned long long)code);
    }
}
static bool gpuw_overlaps(uint64_t a, uint64_t n) {
    uint64_t e = a + n;
    if (gpuw_unknown) return true;
    int lo = 0, hi = ngpuw;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (gpuw[mid].e <= a) lo = mid + 1; else hi = mid;
    }
    return lo < ngpuw && gpuw[lo].a < e;
}

// Vertex / index data read in place from the imported guest heap (renderer.m bind_guest does the same with
// g_guest_buffer): no per-draw copy, no index scan for the copy size. The game fences its CPU writes as on the
// Switch. Falls back to copies outside the heap or without the import; LARECOMP_VK_COPY_VERTICES=1 forces copies.
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
    if (copy || addr < HEAP_BASE) return false;
    if (addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) return false;
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    return true;
}
// Vertex / index range of a draw: in place from the game's memory (true) or to be copied (false). With the VRAM ring on, copied
// unless a compute dispatch may write it.
volatile int g_heal_tex, g_heal_vtx;  // (LARECOMP_HEAL_KEYS research keys, runtime/hle/input.c)
static bool vtx_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (g_heal_vtx) gpuw_unknown = true;
    if (ring_gpu_enabled() && !gpuw_overlaps(addr, size)) { stat_vtx_copied++; return false; }
    bool d = guest_direct(addr, size, buf, off);
    if (d) stat_vtx_direct++;
    return d;
}
bool vk_guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {  // (cbuf_info, above)
    return guest_direct(addr, size, buf, off);
}

static int g_cs_n;  // (compute program table below)

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    g_cs_n = 0;
    ngpuw = 0;  // (guest memory is replaced)
    gcopy_gen++;
    if (gmem_buf) {
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
    }
    gmem_size = gmem_tried = 0;
    st_defaults();
}

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    ShaderTexture *st_ = &si->tex[i];
    int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (e->tex) {
            // P1 DIAGNÓSTICO: log quando textura R8 de fonte é vinculada
            // Atlas 2048x1024 em 0x729c1bc000, texturas 64x64 em 0x7285e32000
            uint64_t taddr = (uint64_t)(uintptr_t)e->tex->addr;
            int is_font_atlas = (e->tex->format == 0x1 && e->tex->width == 2048 && e->tex->height == 1024);
            int is_small_font = (e->tex->format == 0x1 && e->tex->width == 64 && e->tex->height == 64);
            if (is_font_atlas || is_small_font) {
                static int n_font_bind = 0;
                // MISSÃO C: flag global para instrumentação no draw
                extern _Atomic int g_font_atlas_bound;
                if (is_font_atlas) atomic_store(&g_font_atlas_bound, 1);
                if (n_font_bind < 100) {
                    n_font_bind++;
                    LOG("[font-bind] %s %ux%u addr=0x%llx bound to descriptor (stage %d, slot %d)\n",
                        is_font_atlas ? "ATLAS2048" : "SMALL64",
                        e->tex->width, e->tex->height,
                        (unsigned long long)taddr, stage, i);
                }
            }
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. LARECOMP_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = getenv("LARECOMP_VK_NO_TEX_MEMO") != NULL;
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo[1024];
            uint64_t vh = e->has_view ? hash_words(0, &e->view, sizeof e->view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)e->tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(e->tex, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = e->tex; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].view = view;
            }
            renderer_texture_scale(e->tex, &sc[0], &sc[1]);
            wh[0] = e->tex->width; wh[1] = e->tex->height;
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = feedback_view[k];
                    if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY);  // (2D snapshot as array: rare)
                }
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) smp = vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc]);
    if (!view) view = vk_dummy_view(st_->type);
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii) {
    VkDescriptorSet ds = vk_alloc_set(layout);
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    float rs[65][4] = {{1, 1, 0, 0}};
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            // GWIN: the window (gfast_win) for GFast, the heap from its start for GMem.
            // gfast_win is only set by gwin_prepare (called before stage_set when the draw's
            // shaders use the map); without it, fall back to the heap as before.
            bi[n] = b->binding == GFAST_BUFFER_INDEX && gfast_win.buffer ? gfast_win : gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint / gmap: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || (b->binding != 18 && b->binding != 19 && b->binding != GMAP_BUFFER_INDEX)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == GMAP_BUFFER_INDEX) {  // (gwin) this draw's GMap table: gwin_prepare
            bi[n] = ubo_span(gmap_copy(), sizeof gmap_tab);
        } else if (b->binding == 18) {
            for (int t = 0; t < si->ntex && t < 64; t++) { rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; }
            bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, sizeof rs, 256), sizeof rs);
        } else {
            float zero[8] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    return ds;
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; } CsProg;

static void build_compute(void *arg) {
    CsProg *cp = arg;
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n, 0);
    if (!spv) { atomic_store(&cp->p.state, 2); return; }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = cp->p.layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    atomic_fetch_add(&stat_compiles, 1);
    if (r == VK_SUCCESS) atomic_fetch_add(&new_pipes, 1);
    atomic_store(&cp->p.state, r == VK_SUCCESS ? 1 : 2);
    if (r != VK_SUCCESS) LOG("[compute] pipeline failed: %d\n", r);
}

static struct CsEnt { uint64_t addr; CsProg *cp; } g_cs_tab[256];  // compute programs by code address

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++) if (tab[i].addr == ss->code_addr) return tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);
    if (n < 256) { tab[n].addr = ss->code_addr; tab[n].cp = cp; g_cs_n = n + 1; }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderKey k = {0};
    k.glsl = 1;
    cp->si = maxwell_translate((const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, STAGE_COMPUTE, &k);
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)ss->code_addr);
        atomic_store(&cp->p.state, 2);
        return cp;
    }
    for (int i = 0; i < 3; i++) {
        cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    run_job(build_compute, cp);
    return cp;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (!cp || atomic_load(&cp->p.state) != 1) { stat_skipped++; return; }
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u)", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2]);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    gpuw_stores(cp->si, st.sbuf[5], 16, "compute shader", st.program->stages[5].code_addr);
    for (int i = 0; i < 16; i++) if (st.sbuf[5][i].addr) gpuw_note(st.sbuf[5][i].addr, st.sbuf[5][i].size);  // (any storage buffer it has bound)
    nfeedback = 0;
    // GWIN: compute shaders translated with the map path need their table too (stage 5).
    if (gwin_mode && cp->si->uses_gmem_map) gwin_prepare(1u << 5);
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    stat_dispatches++;
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

static void draw_(CmdRec *r, bool indexed);
static void draw(CmdRec *r, bool indexed) {
    PROF_T0();
    draw_(r, indexed);
    PROF_ADD(prof_draw);
}
// A draw whose program or pipeline is still being built waits for it, up to a budget per frame, instead of being
// skipped (objects popping in on entering an area). At 30 fps a frame has ~27 ms to spare (uncapped, the overworld
// runs at ~160 fps), so 20 ms per frame hides the pop-in with a warm cache (~1 ms per program) without costing frames;
// a cold cache (first visit) spends the budget and skips the rest as before. LARECOMP_SHADER_WAIT_MS=<ms> (0: never).
static bool wait_built(_Atomic int *state) {
    if (atomic_load(state) != 0) return true;
    static int64_t budget = -1;
    if (budget < 0) budget = (getenv("LARECOMP_SHADER_WAIT_MS") ? atoi(getenv("LARECOMP_SHADER_WAIT_MS")) : 20) * 1000000ll;
    if (sync_shaders()) return true;  // (built already)
    extern uint64_t vk_frame_counter(void);
    static uint64_t frame;
    static int64_t spent;
    if (vk_frame_counter() != frame) { frame = vk_frame_counter(); spent = 0; }
    if (spent >= budget) return false;
    uint64_t t0 = SDL_GetTicksNS();
    while (atomic_load(state) == 0 && spent + (int64_t)(SDL_GetTicksNS() - t0) < budget) SDL_DelayNS(100000);
    uint64_t dt = SDL_GetTicksNS() - t0;
    spent += (int64_t)dt;
    stat_shader_wait_ns += dt;
    return atomic_load(state) != 0;
}

static uint64_t prof_tp_;
static void draw_(CmdRec *r, bool indexed) {
    if (prof) prof_tp_ = SDL_GetTicksNS();
    if (!st.program) { SKIP(0); return; }
    if (ip_enabled()) {  // (frame interpolation: what identifies this draw from one frame to the next)
        // what the draw is (program, vertex buffer, parameters), and which game object it belongs to: the same mesh
        // drawn for two objects (bushes, Pidgeys) is told apart by the object, not by the order the game drew them in
        uint64_t h = 0xcbf29ce484222325ull ^ (uint64_t)(uintptr_t)st.program ^ ((uint64_t)indexed << 63);
        h = (h ^ st.vbuf[0].addr) * 0x100000001B3ull;
        for (int i = 0; i < 8; i++) h = (h ^ r->x[i]) * 0x100000001B3ull;
        if (st.obj) h = (h ^ st.obj) * 0x100000001B3ull, ip_stats_tagged++;
        else ip_stats_untagged++;
        ip_draw_sig = h;
        // 3D draws (a depth target with the depth test on) are blended, with or without a game object: the ground and the
        // scenery have none, and they must take the same (blended) light matrices as the shadow map the objects are drawn
        // into, or shadows slide away from their casters. Interface and full-screen passes don't test depth: frame N.
        ip_draw_obj = st.rt.depth != NULL && st.ds[0];
        // In menus (the Pokédex, the bag...) the 2D draws are blended too: lists and panels slide there. A 2D draw takes the
        // strict jump test in ip_cbuf (a recycled row or glyph moves far: it keeps frame N).
        ip_draw_2d = !ip_draw_obj;
        extern bool fps60_menu(void);
        if (fps60_menu()) ip_draw_obj = true;
    }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    // GWIN/GCOPY: ensure the heap import (and gwin_mode) is decided BEFORE translation, so shaders
    // translated from here on use the map path when the heap exceeds the storage range, or when
    // there is no heap import at all (system Adreno driver: gcopy copies the ranges instead).
    gmem_info();
    static int gcopy_init = -1;
    if (gcopy_init < 0) {
        gcopy_init = 1;
        gcopy_mode = !g_vk.host_import && !getenv("LARECOMP_VK_NO_GCOPY");
        if (gcopy_mode) LOG("[vk] no heap import (no VK_EXT_external_memory_host): shader global memory through CPU copies (gcopy)\n");
    }
    maxwell_gmem_map_all = gcopy_mode || gwin_mode;
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (pr) wait_built(&pr->state);
    if (!pr || atomic_load(&pr->state) != 1) { SKIP(pr ? 1 + (atomic_load(&pr->state) > 1) : 3); return; }
    // MISSÃO C: log draws que usam o atlas de fontes
    // FALHA 1: instrumentação robusta com contadores
    // MISSÃO 1 v0064: inclui hash do fragment shader para identificação
    // MISSÃO C v0065: gatilho de captura sob demanda via arquivo
    atomic_fetch_add(&cnt_text_candidates, 1);
    if (atomic_load(&g_font_atlas_bound)) {
        atomic_fetch_add(&cnt_text_matched, 1);
        static int n_font_draw = 0;
        // MISSÃO C: verifica gatilho de captura sob demanda
        // Arquivo: $LARECOMP_DATA_ROOT/capture_font (criado via adb ou app)
        // Quando presente, captura detalhada; remove o arquivo após capturar
        static int capture_triggered = 0;
        static int capture_done = 0;
        static int trigger_checked = 0;
        if (!capture_triggered) {
            const char *root = getenv("LARECOMP_DATA_ROOT");
            char trig[1024];
            if (root && root[0]) snprintf(trig, sizeof(trig), "%s/capture_font", root);
            else snprintf(trig, sizeof(trig), "capture_font");
            // DEFEITO A v0070: na primeira verificação, REMOVE gatilho remanescente
            // sem disparar — evita CAP automático no frame 12
            if (!trigger_checked) {
                trigger_checked = 1;
                LOG("[font-capture] Monitoring trigger file: %s\n", trig);
                // Limpa gatilho de sessão anterior
                if (remove(trig) == 0) {
                    LOG("[font-capture] Stale trigger file removed (not triggering)\n");
                }
            }
            FILE *tf = fopen(trig, "r");
            if (tf) {
                fclose(tf);
                capture_triggered = 1;
                // Remove o arquivo para não capturar novamente (com verificação)
                if (remove(trig) == 0) {
                    LOG("[font-capture] TRIGGERED: on-demand capture activated (trigger consumed)\n");
                } else {
                    LOG("[font-capture] TRIGGERED: on-demand capture activated (trigger remove FAILED: %s)\n", strerror(errno));
                }
                LOG("[font-capture] CAP_FRAME_SELECTED: frame %llu\n", (unsigned long long)vk_frame_counter());
                // P1 v0066: marca para capturar screenshot do frame
                extern _Atomic int g_font_capture_frame;
                atomic_store(&g_font_capture_frame, 1);
            }
        }
        // Captura se: (a) dentro dos 100 primeiros (v0083: era 10 — escondia os draws
        // de diálogo, que vêm depois dos draws do menu em cada frame), OU (b) gatilho
        // ativado e ainda não capturou
        int should_log = (n_font_draw < 100) || (capture_triggered && !capture_done);
        if (should_log) {
            n_font_draw++;
            atomic_fetch_add(&cnt_text_captured, 1);
            if (capture_triggered && !capture_done && n_font_draw > 100) {
                capture_done = 1;
                LOG("[font-capture] CAPTURED: frame with on-demand trigger\n");
            }
            extern uint64_t vk_frame_counter(void);
            // Calcula hash do fragment shader diretamente (mesma fórmula do bloco fshash)
            uint64_t fs_addr = st.program ? st.program->stages[1].code_addr : 0;
            uint64_t fs_hash = fs_addr ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs_addr + 0x30), 0x200) : 0;
            // P1 v0066: log detalhado de estado
            // viewport[0]: x, y, w, h (float) | scissor[0]: x, y, w, h (int32)
            // ds: [0]=depth_test, [1]=depth_write, [2]=depth_func, [3]=stencil_test (uint8)
            // blend[0]: [0]=enable?, [1]=srcRGB, [2]=dstRGB, [3]=srcA, [4]=dstA (uint8)
            LOG("[font-draw] #%d frame=%llu indexed=%d prog=%p fshash=%016llx (atlas bound, matched=%llu)%s\n",
                n_font_draw, (unsigned long long)vk_frame_counter(),
                indexed, (void*)st.program,
                (unsigned long long)fs_hash,
                (unsigned long long)atomic_load(&cnt_text_matched),
                capture_triggered ? " [TRIGGERED]" : "");
            // FRENTE 3 v0076: loga contagem de índices/vértices para o draw de texto
            // CORREÇÃO v0083: o count real é r->x[2] (ver draw_: `count = r->x[2]`).
            // As versões anteriores logavam r->x[6] (indexed) / r->x[4] (não-indexed)
            // como "indexCount"/"vertexCount", mas esses campos são o INSTANCE count
            // (indexed: inst = r->x[6]; não-indexed: inst = r->x[4]). O "indexCount=1"
            // do log v0082 era instance count = 1 (normal), NÃO prova de pass fullscreen.
            // v0081: estendido ao shader do MENU (7270eb69fbecccee) — o menu é o caso de
            // teste do 1º frame (não precisa entrar no jogo) e seu VS não usa gmem.
            if (fs_hash == 0x6443b63b341c73caULL || fs_hash == 0x7270eb69fbeccceeULL) {
                if (indexed) {
                    LOG("[font-geo] fshash=%016llx indexCount=%llu indexAddr=%llx baseVertex=%d inst=%llu sbuf0_0=%llx+%llu ubuf0_0=%llx+%llu\n",
                        (unsigned long long)fs_hash,
                        (unsigned long long)r->x[2], (unsigned long long)r->x[3], (int)(int32_t)r->x[4], (unsigned long long)r->x[6],
                        (unsigned long long)st.sbuf[0][0].addr, (unsigned long long)st.sbuf[0][0].size,
                        (unsigned long long)st.ubuf[0][0].addr, (unsigned long long)st.ubuf[0][0].size);
                } else {
                    LOG("[font-geo] fshash=%016llx vertexCount=%llu first=%llu inst=%llu sbuf0_0=%llx+%llu ubuf0_0=%llx+%llu\n",
                        (unsigned long long)fs_hash,
                        (unsigned long long)r->x[2], (unsigned long long)r->x[1], (unsigned long long)r->x[4],
                        (unsigned long long)st.sbuf[0][0].addr, (unsigned long long)st.sbuf[0][0].size,
                        (unsigned long long)st.ubuf[0][0].addr, (unsigned long long)st.ubuf[0][0].size);
                }
            }
            // MISSÃO 1 v0067: corrigidos especificadores (%g para float viewport)
            LOG("[font-state] viewport=(%g,%g %gx%g) scissor=(%d,%d %dx%d) depth_test=%u depth_write=%u stencil=%u\n",
                st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3],
                st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3],
                (unsigned)st.ds[0], (unsigned)st.ds[1], (unsigned)st.ds[3]);
            LOG("[font-state] blend0: srcRGB=%u dstRGB=%u srcA=%u dstA=%u\n",
                (unsigned)st.blend[0][1], (unsigned)st.blend[0][2], (unsigned)st.blend[0][3], (unsigned)st.blend[0][4]);
        } else {
            atomic_fetch_add(&cnt_text_dropped, 1);
        }
        atomic_store(&g_font_atlas_bound, 0);
    } else {
        atomic_fetch_add(&cnt_text_rejected, 1);
    }
    {   // fragment-shader hash: interior backdrop detection (widescreen), skip lists, item twinkle (renderer.m)
        static uint64_t last_fs, last_hash;
        uint64_t fs = st.program->stages[1].code_addr;
        if (fs != last_fs) {
            last_fs = fs;
            last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
        }
        static uint64_t skip[16];
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            for (const char *p = getenv("LARECOMP_SKIP_FSHASH"); p && *p && nskip < 16;) {
                skip[nskip++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        for (int i = 0; i < nskip; i++) if (last_hash == skip[i]) { stat_skipped++; return; }
        if (g_debug_frame && getenv("LARECOMP_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
    }
    {   // (research, LARECOMP_COUNT_TEX=<w>x<h>: per frame, the draws and instances sampling a fragment texture of that size)
        static int tw = -1, th;
        if (tw < 0) { const char *e = getenv("LARECOMP_COUNT_TEX"); tw = 0; if (e) sscanf(e, "%dx%d", &tw, &th); }
        if (tw > 0) {
            static uint64_t frame = ~0ull;
            static unsigned nd, ni;
                        if (renderer_present_count() != frame) {
                if (frame != ~0ull) LOG("[counttex] frame %llu: %u draws %u instances\n", (unsigned long long)frame, nd, ni);
                frame = renderer_present_count(), nd = ni = 0;
            }
            for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
                NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
                if (t && (int)t->width == tw && (int)t->height == th) { nd++; ni += (unsigned)(indexed ? r->x[6] : r->x[4]); break; }
            }
        }
    }
    if (g_debug_frame) debug_draw(pr, r, indexed);
    else if ((pr->vi->attr_mask >> 11) & 1) {  // skinned draws (a11: bone indices)
        static int n;
        static int64_t dump_frame = -2;  // LARECOMP_DUMP_SKIN_FRAME: dump the first one from that frame on
        if (dump_frame == -2) dump_frame = getenv("LARECOMP_DUMP_SKIN_FRAME") ? atoll(getenv("LARECOMP_DUMP_SKIN_FRAME")) : -1;
        if (dump_frame >= 0 && renderer_present_count() >= (uint64_t)dump_frame) {
            debug_draw(pr, r, indexed);  // (debug_draw writes build/skin/ once)
            dump_frame = -1;
        } else if (getenv("LARECOMP_LOG_SKIN") && n++ % 50 == 0 && n < 1000) {
            debug_draw(pr, r, indexed);
        }
    }
    resolve_feedback(pr);
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    begin_pass();
    VkCommandBuffer cb = vk_cmd();

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { SKIP(4); return; }

    uint64_t prof_tk_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_pre += prof_tk_ - prof_tp_;
    // ---- pipeline
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = pr;
    k.ncolor = pass_ncolor;
    // (only the pass's own slots: the ones past them keep what the last pass with more targets set, so a depth-only
    // pass's pipelines got several keys and were built again for each - LAS Recomp 3921201)
    for (uint32_t i = 0; i < pass_ncolor && i < 8; i++) k.color[i] = pass_color[i];
    k.depth = pass_depth;
    k.stencil = pass_stencil;
    k.samples = pass_samples;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr_bind[i] = ZERO_BINDING;
            k.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        k.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        k.attr_off[i] = off;
        k.attr_bind[i] = stream;
        k.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        k.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.divisor[s] = s < st.nstream && k.stride[s] ? st.stream[s][1] : 0;
        if (k.divisor[s] > 1 && !g_vk.divisor) k.divisor[s] = 1;
    }
    for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || k.color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(st.rt.color[i]->format)) {
            k.blend_on |= (uint8_t)(1u << i);
            memcpy(k.blend[i], st.blend[i], 8);
        }
    }
    // v0086: dialog text produces ZERO fragments despite verified gmem data, gfind math,
    // bindings, and cull NONE + depth OFF (v0085). Remaining STATIC killer: colorWriteMask.
    // k.write_mask[0] = 0 happens when EITHER the FS omap_target excludes target 0 OR the
    // guest channel_mask for target 0 is 0 — and a 0 write mask stores NOTHING, so even a
    // TEXT_DIAG FS that runs and outputs magenta leaves no visible pixel. Force full RGBA
    // for dialog draws only (this changes the pipeline key, so the dialog gets its own
    // pipeline built from the Prog's TEXT_DIAG SPIR-V). The log line is the proof: if the
    // original was 0 and text/magenta appears, write_mask was the killer; if the original
    // was already 0xF, write_mask is eliminated by the same log.
    {
        uint64_t fs_fsh_wm = pr->code[1] ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(pr->code[1] + 0x30), 0x200) : 0;
        if (fs_fsh_wm == 0x6443b63b341c73caULL) {
            static int wm_logged = 0;
            if (!wm_logged) { wm_logged = 1; LOG("[diag] v0086: dialog write_mask orig=%u (out_mask=%08x chmask=%08x ncolor=%u) -> forced 0xF\n", (unsigned)k.write_mask[0], (unsigned)pr->fi->out_mask, (unsigned)st.channel_mask, (unsigned)k.ncolor); }
            if (k.ncolor > 0) k.write_mask[0] = 0xF;
        }
    }
    // v0088: a0 binding diagnostic — the one gl_Position input never verified.
    // If a0 is unbound (ZERO_BINDING), the VS reads (0,0,0,1) for every vertex
    // and all quad corners collapse to one point -> zero fragments. Log once
    // for the dialog FS: VS attr_mask bit 0, guest attrib[0] (fmt/stream/off),
    // pipeline attr_bind[0]/attr_fmt[0], and the first 4 vertices' a0[0]/a0[1]
    // when bound (float interpretation + raw bytes, so a non-float format still
    // shows whether the values vary across vertices). This is the proof for
    // whether the A0_FIX above was load-bearing or a no-op.
    {
        uint64_t fs_fsh_a0 = pr->code[1] ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(pr->code[1] + 0x30), 0x200) : 0;
        if (fs_fsh_a0 == 0x6443b63b341c73caULL) {
            static int a0_logged = 0;
            if (!a0_logged) {
                a0_logged = 1;
                uint32_t a0raw = st.nattrib > 0 ? st.attrib[0] : 0x2E;
                LOG("[diag] v0088: dialog a0: vs_attr_mask bit0=%u nattrib=%d attrib0 raw=%08x fmt=%#x stream=%u off=%u pipe_bind=%u pipe_fmt=%d\n",
                    (unsigned)((pr->vi->attr_mask >> 0) & 1), st.nattrib, a0raw, a0raw & 0xFF, (a0raw >> 8) & 0x1F, a0raw >> 13,
                    (unsigned)k.attr_bind[0], (int)k.attr_fmt[0]);
                if (k.attr_bind[0] == ZERO_BINDING) {
                    LOG("[diag] v0088: dialog a0 is ZERO_BINDING (unbound) -> VS reads (0,0,0,1) for every vertex\n");
                } else {
                    uint32_t s0 = k.attr_bind[0];
                    uint64_t stride = k.stride[s0];
                    const uint8_t *base = (const uint8_t *)(uintptr_t)st.vbuf[s0].addr;
                    uint64_t vsize = st.vbuf[s0].size;
                    if (base && stride) {
                        for (int v = 0; v < 4; v++) {
                            const uint8_t *pp = base + (uint64_t)v * stride + k.attr_off[0];
                            if ((uint64_t)(pp - base) + 8 > vsize) { LOG("[diag] v0088: dialog a0 vert %d: out of bound (need %llu of %llu)\n", v, (unsigned long long)((uint64_t)(pp - base) + 8), (unsigned long long)vsize); break; }
                            float f0 = 0, f1 = 0;
                            memcpy(&f0, pp, 4); memcpy(&f1, pp + 4, 4);
                            LOG("[diag] v0088: dialog a0 vert %d: f0=%g f1=%g bytes=%02x%02x%02x%02x %02x%02x%02x%02x\n",
                                v, (double)f0, (double)f1, pp[0], pp[1], pp[2], pp[3], pp[4], pp[5], pp[6], pp[7]);
                        }
                    } else {
                        LOG("[diag] v0088: dialog a0 vert dump skipped: base=%p stride=%llu\n", (void *)base, (unsigned long long)stride);
                    }
                }
            }
        }
    }
    // v0089: cb3 diagnostic — the one gl_Position input never logged.
    // The dialog VS computes gl_Position = cb3[0..3] x (r8, r9, r5, r6), where the
    // v0088 simulation gave pre-matrix inputs (1.5..17.5, -29.5..16.5, 0, 1.0).
    // If cb3 is identity, NDC = (1.5..17.5, 29.5..-16.5, 0.5) — outside the clip
    // volume -> trivially rejected -> zero fragments. If cb3 is zeros, w = 0 ->
    // rejected. Log the 16 floats of cb3 rows 0..3 for the dialog draw AND for
    // the menu draw (whose VS uses the same cb3[0..3] matrix and works), so the
    // next test can compare. cb3 (VS binding 3) comes from st.ubuf[0][0] via
    // cbuf_info; addr == 0 means cbuf_info binds a zeros buffer.
    {
        uint64_t fs_fsh_cb3 = pr->code[1] ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(pr->code[1] + 0x30), 0x200) : 0;
        if (fs_fsh_cb3 == 0x6443b63b341c73caULL || fs_fsh_cb3 == 0x7270eb69fbeccceeULL) {
            static int cb3_logged_dlg = 0, cb3_logged_menu = 0;
            int is_dlg = fs_fsh_cb3 == 0x6443b63b341c73caULL;
            int *logged = is_dlg ? &cb3_logged_dlg : &cb3_logged_menu;
            if (!*logged) {
                *logged = 1;
                uint64_t cb_addr = st.ubuf[0][0].addr;
                uint64_t cb_size = st.ubuf[0][0].size;
                if (!cb_addr) {
                    LOG("[diag] v0089: %s cb3: UNBOUND (addr=0, cbuf_info binds zeros) size=%llu\n",
                        is_dlg ? "dialog" : "menu", (unsigned long long)cb_size);
                } else {
                    float row[16] = {0};
                    uint64_t avail = cb_size < 64 ? cb_size : 64;
                    memcpy(row, (const void *)(uintptr_t)cb_addr, (size_t)avail);
                    LOG("[diag] v0089: %s cb3: addr=%llx size=%llu\n",
                        is_dlg ? "dialog" : "menu", (unsigned long long)cb_addr, (unsigned long long)cb_size);
                    LOG("[diag] v0089:   row0: %g %g %g %g\n", (double)row[0], (double)row[1], (double)row[2], (double)row[3]);
                    LOG("[diag] v0089:   row1: %g %g %g %g\n", (double)row[4], (double)row[5], (double)row[6], (double)row[7]);
                    LOG("[diag] v0089:   row2: %g %g %g %g\n", (double)row[8], (double)row[9], (double)row[10], (double)row[11]);
                    LOG("[diag] v0089:   row3: %g %g %g %g\n", (double)row[12], (double)row[13], (double)row[14], (double)row[15]);
                }
            }
        }
    }
    k.a2c = st.ms[0xC] != 0;
    k.topo_class = cls;
    k.poly_fill = st.poly[2] != 1;
    uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
    if (!ph) ph = 1;
    uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
    while (pipe_tab[pslot].key && pipe_tab[pslot].key != ph) pslot = (pslot + 1) & (PIPE_TAB - 1);
    Pipe *pp = pipe_tab[pslot].pipe;
    // v0075: força reconstrução do pipeline para o shader diagnóstico
    // Se TEXT_DIAG ativo e o pipe existente é do shader de texto, destrói e reconstrói
    // com o shader magenta (o bypass no build_pipeline só funciona para pipes novos)
    if (pp && atomic_load(&pp->state) == 1) {
        static int diag_rebuild_checked = -1;
        if (diag_rebuild_checked < 0) {
            const char *d = getenv("LARECOMP_TEXT_DIAG");
            diag_rebuild_checked = (d && atoi(d) != 0) ? 1 : 0;
        }
        if (diag_rebuild_checked && pp->k.prog && pp->k.prog->code[1]) {
            uint64_t fs_fsh = hmix(1469598103934665603ull, (const void *)(uintptr_t)(pp->k.prog->code[1] + 0x30), 0x200);
            if (fs_fsh == 0x6443b63b341c73caULL) {
                static int rebuild_done = 0;
                int *done_flag = &rebuild_done;
                if (!*done_flag) {
                    *done_flag = 1;
                    LOG("[pipeline] DIAG: forcing rebuild of text shader pipeline (destroying cached) fshash=%016llx\n", (unsigned long long)fs_fsh);
                    vkDestroyPipeline(g_vk.dev, pp->pipe, NULL);
                    pp->pipe = VK_NULL_HANDLE;
                    atomic_store(&pp->state, 0);
                    run_job(build_pipeline, pp);
                }
            }
        }
    }
    if (!pp) {
        pp = calloc(1, sizeof *pp);
        pp->k = k;
        pipe_tab[pslot].key = ph;
        pipe_tab[pslot].pipe = pp;
        run_job(build_pipeline, pp);
    }
    wait_built(&pp->state);
    if (atomic_load(&pp->state) != 1) { SKIP(5 + (atomic_load(&pp->state) > 1)); return; }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pipe);
    uint64_t prof_td_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("LARECOMP_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth;
    vkCmdSetDepthTestEnable(cb, dt);
    vkCmdSetDepthWriteEnable(cb, dt && st.ds[1]);
    VkCompareOp dop = dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). LARECOMP_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = getenv("LARECOMP_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCmdSetDepthCompareOp(cb, dop);
    bool sten = st.ds[3] && pass_stencil;
    vkCmdSetStencilTestEnable(cb, sten);
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            vkCmdSetStencilOp(cb, face, stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                              compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF));
            vkCmdSetStencilCompareMask(cb, face, st.stencil_vmask[f] & 0xFF);
            vkCmdSetStencilWriteMask(cb, face, st.stencil_mask[f] & 0xFF);
            vkCmdSetStencilReference(cb, face, st.stencil_ref[f]);
        }
    }
    vkCmdSetBlendConstants(cb, st.blend_color);

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    vkCmdSetCullMode(cb, cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // LARECOMP_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("LARECOMP_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    vkCmdSetFrontFace(cb, front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetPrimitiveRestartEnable(cb, st.prim_restart && native && (mode == 3 || mode == 5));
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("LARECOMP_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        vkCmdSetDepthBiasEnable(cb, on);
        vkCmdSetDepthBias(cb, f[1], f[2], f[0]);
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    float tint[8] = {0};  // (the per-draw tint the translator can apply to colour target 0: none)
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (prof) prof_dyn += SDL_GetTicksNS() - prof_td_;
    PROF_T0();
    // GWIN/GCOPY: the table both stages' sets use (stage 0 = vertex, stage 1 = fragment).
    // v0080: set g_text_fshash before gcopy_prepare so the log can flag text draws.
    if (pr->code[1])
        g_text_fshash = hmix(1469598103934665603ull, (const void *)(uintptr_t)(pr->code[1] + 0x30), 0x200);
    else
        g_text_fshash = 0;
    if (gwin_mode && (pr->vi->uses_gmem_map || pr->fi->uses_gmem_map))
        gwin_prepare((pr->vi->uses_gmem_map ? 1u : 0u) | (pr->fi->uses_gmem_map ? 2u : 0u));
    if (gcopy_mode && (pr->vi->uses_gmem_map || pr->fi->uses_gmem_map))
        gcopy_prepare((pr->vi->uses_gmem_map ? 1u : 0u) | (pr->fi->uses_gmem_map ? 2u : 0u));
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL);
    PROF_ADD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = prof ? SDL_GetTicksNS() : 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh);
        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
    }
    if (prof) prof_tex += SDL_GetTicksNS() - prof_tx_;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        static uint64_t wide_frame = ~0ull;
        if (reads_scene) wide_frame = renderer_present_count();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {   // (research, LARECOMP_COUNT_FS=<fs hash>: draws with that fragment shader per frame, logged when the count changes)
        static uint64_t want = 1, frame = ~0ull;
        static unsigned n, last = ~0u;
        if (want == 1) { const char *e = getenv("LARECOMP_COUNT_FS"); want = e ? strtoull(e, NULL, 16) : 0; }
        if (want) {
                        if (renderer_present_count() != frame) {
                if (frame != ~0ull && n != last) LOG("[countfs] frame %llu: %u\n", (unsigned long long)frame, n);
                if (frame != ~0ull) last = n;
                frame = renderer_present_count(), n = 0;
            }
            if (g_cur_fs_hash == want) n++;
        }
    }
    {
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        set_viewport(vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf);
        // v0086: prove the FINAL Vulkan scissor/viewport for dialog draws (suspect 1).
        // [font-state] only logs GUEST st.viewport/st.scissor (full 1920x1080); the final
        // rect after widescreen scaling + pass clamping was never logged. If this shows
        // a 0x0 scissor, scissor is the killer; if full, scissor is eliminated.
        if (g_text_fshash == 0x6443b63b341c73caULL) {
            static int sc_logged = 0;
            if (sc_logged < 3) {
                sc_logged++;
                VkRect2D sr2 = scissor_rect();
                LOG("[diag] v0086: dialog final vp=(%.1f,%.1f %.1fx%.1f) scissor=(%d,%d %ux%u) pass=%ux%u scale=%.4f,%.4f off=%.1f,%.1f\n",
                    vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy,
                    sr2.offset.x, sr2.offset.y, sr2.extent.width, sr2.extent.height,
                    pass_w, pass_h, vp_sx, vp_sy, vp_ox, vp_oy);
            }
        }
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy);
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = prof ? SDL_GetTicksNS() : 0;
    gpuw_stores(pr->vi, st.sbuf[0], 16, "vertex shader", st.program->stages[0].code_addr);
    gpuw_stores(pr->fi, st.sbuf[1], 16, "fragment shader", st.program->stages[1].code_addr);
    VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii);
    if (prof) prof_sets += SDL_GetTicksNS() - prof_t1_;
    VkDescriptorSet sets[2] = {vset, fset};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, 0, NULL);

    uint64_t prof_t2_ = prof ? SDL_GetTicksNS() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            if (iaddr % isz == 0 && vtx_direct(iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = transient_gpu_copy(src, (uint64_t)count * isz + 4, 4);
                { uint32_t m = scan_max_index(src, isz, count, restart); if (m > max_index) max_index = m; }
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    if (use_index) vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            if (v.addr && v.size && vtx_direct(v.addr, v.size, &dbuf, &doff)) {
                vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &dbuf, &doff);
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                { uint32_t m = scan_max_index(scan_src, scan_isz, icount, scan_restart); if (m > max_index) max_index = m; }
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = k.stride[s], need;
            if (!stride) need = 256;
            else if (k.divisor[s]) need = ((uint64_t)base_inst + inst + k.divisor[s] - 1) / k.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else {
                sp = vertex_copy(v.addr, need ? need : 16, v.size, k.divisor[s] == 0);
                if (ENV("LARECOMP_LOG_VCOPY") && g_gpuprof_frame)  // (research: what a profiled frame copies, per stream)
                    LOG("[vcopy] vs %llx stream %d addr %llx need %llu of bound %llu stride %llu %s max_index %u base_vertex %d base_inst %u inst %u\n",
                        (unsigned long long)st.program->stages[0].code_addr, s, (unsigned long long)v.addr, (unsigned long long)need,
                        (unsigned long long)v.size, (unsigned long long)stride, k.divisor[s] ? "instance" : "vertex", max_index, base_vertex, base_inst, inst);
            }
        }
        VkDeviceSize off = sp.off;
        vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &off);
    }

    if (prof) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    // ---- draw
    // LARECOMP_GPUPROF_DRAWS=1 (with LARECOMP_GPUPROF_AT): a mark before every draw, so each row is one draw's GPU time
    static int gp_draw_marks = -1;
    if (gp_draw_marks < 0) gp_draw_marks = getenv("LARECOMP_GPUPROF_DRAWS") != NULL;
    if (g_gpuprof_frame && gp_draw_marks)
        gp_mark(cb, "draw vs %llx fs %016llx fsaddr %llx %s %u inst %u", (unsigned long long)st.program->stages[0].code_addr,
                (unsigned long long)g_cur_fs_hash, (unsigned long long)st.program->stages[1].code_addr, use_index ? "elements" : "arrays", use_index ? icount : count, inst);
    // Experiment (research): LARECOMP_EXP_VS=<vertex program address, hex> with LARECOMP_EXP_MODE=skip | scissor | half
    // changes only the draws of that program: no draw, a 1x1 scissor (no pixel work, vertex work kept) or half the instances.
    static uint64_t exp_vs;
    static int exp_mode = -1;
    if (exp_mode < 0) {
        const char *v = getenv("LARECOMP_EXP_VS"), *m = getenv("LARECOMP_EXP_MODE");
        exp_vs = v ? strtoull(v, NULL, 16) : 0;
        exp_mode = !exp_vs || !m ? 0 : !strcmp(m, "skip") ? 1 : !strcmp(m, "scissor") ? 2 : !strcmp(m, "half") ? 3 : 0;
    }
    // (LARECOMP_EXP_ALT=1: only on odd frames, so one run holds both versions of the same scene)
    static int exp_alt = -1;
    if (exp_alt < 0) exp_alt = getenv("LARECOMP_EXP_ALT") != NULL;
    bool exp_hit = exp_mode && st.program->stages[0].code_addr == exp_vs && (!exp_alt || (vk_frame_counter() & 1));
    uint32_t inst_eff = exp_hit && exp_mode == 3 && inst > 1 ? inst / 2 : inst;
    if (exp_hit && exp_mode == 2) { VkRect2D one = {{0, 0}, {1, 1}}; vkCmdSetScissor(cb, 0, 1, &one); }
    bool gp_q = g_gpuprof_frame && gp_draw_marks && gps_qp && gp_n && gps_n < GP_MAX;
    if (gp_q) vkCmdBeginQuery(cb, gps_qp, (uint32_t)gps_n, 0);
    // v0085: dialog text draws produce ZERO fragments despite verified-correct
    // gmem data, gfind math, bindings, draw order, and a TEXT_DIAG FS that would
    // show any fragment as magenta. The pipeline has no static cull/depth, but
    // both are DYNAMIC per draw: cull from guest st.poly[0] (vkCmdSetCullMode)
    // and depth test from guest st.ds[0] (vkCmdSetDepthTestEnable). 2D dialog
    // text over its own box must render regardless of both: force them off for
    // the dialog shader only. If text/magenta appears, the killer was one of
    // these; if not, both are ruled out in one shot. Each draw sets its own
    // dynamic state at its start, so this override cannot leak to other draws.
    if (g_text_fshash == 0x6443b63b341c73caULL) {
        vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
        vkCmdSetDepthTestEnable(cb, VK_FALSE);
        static int ov_logged = 0;
        if (!ov_logged) { ov_logged = 1; LOG("[diag] v0085: dialog draw: forced cull NONE + depth OFF\n"); }
    }
    if (exp_hit && exp_mode == 1) { /* skipped */ }
    else if (!use_index) vkCmdDraw(cb, count, inst_eff, first, base_inst);
    else vkCmdDrawIndexed(cb, icount, inst_eff, 0, base_vertex, base_inst);
    if (gp_q) { vkCmdEndQuery(cb, gps_qp, (uint32_t)gps_n); gp_stat[gp_n - 1] = gps_n + 1; gps_n++; }
    if (exp_hit && exp_mode == 2) apply_scissor();
    stat_draws++;
    g_draws_total++;
    cum_draws++;
    if (g_gpuprof_frame && gp_n && !gp_draw_marks) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x\n", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask);
    LOG("[draw]   attribs:");  // format / stream / offset per attribute, and the streams' strides
    for (int i = 0; i < st.nattrib; i++)
        if ((pr->vi->attr_mask >> i) & 1)
            LOG(" a%d=f%#x/s%u/+%u", i, st.attrib[i] & 0xFF, (st.attrib[i] >> 8) & 0x1F, st.attrib[i] >> 13);
    for (int s = 0; s < st.nstream && s < 4; s++) LOG(" | stream%d stride %u", s, st.stream[s][0]);
    LOG("\n");
    if (getenv("LARECOMP_DUMP_SKIN") && ((pr->vi->attr_mask >> 11) & 1)) {  // one skinned draw's inputs -> build/skin/
        static int dumped;
        const char *at_frame = getenv("LARECOMP_DUMP_SKIN_FRAME");  // (the first one from that frame on)
        if (at_frame ? renderer_present_count() >= (uint64_t)atoll(at_frame) && !dumped++
                     : ++dumped == (atoi(getenv("LARECOMP_DUMP_SKIN")) > 1 ? atoi(getenv("LARECOMP_DUMP_SKIN")) : 1)) {  // (or the Nth skinned draw)
            mkdir("build/skin", 0755);
            char path[256];
            FILE *f;
            snprintf(path, sizeof path, "build/skin/info.txt");
            if ((f = fopen(path, "w"))) {
                fprintf(f, "vs %llx stride %u\n", (unsigned long long)st.program->stages[0].code_addr, st.stream[0][0]);
                for (int i = 0; i < st.nattrib; i++)
                    if ((pr->vi->attr_mask >> i) & 1)
                        fprintf(f, "attr %d fmt %#x off %u\n", i, st.attrib[i] & 0xFF, st.attrib[i] >> 13);
                for (int c = 0; c < 18; c++)
                    if ((pr->vi->cbuf_mask >> (c + 3)) & 1)
                        fprintf(f, "cb %d size %u\n", c + 3, (unsigned)st.ubuf[0][c].size);
                fclose(f);
            }
            uint64_t vsz = (uint64_t)st.stream[0][0] * 64;
            if (st.vbuf[0].addr && (f = fopen("build/skin/verts.bin", "wb"))) { fwrite((void *)(uintptr_t)st.vbuf[0].addr, 1, vsz < st.vbuf[0].size ? vsz : st.vbuf[0].size, f); fclose(f); }
            for (int c = 0; c < 18; c++) {
                if (!((pr->vi->cbuf_mask >> (c + 3)) & 1) || !st.ubuf[0][c].addr) continue;
                snprintf(path, sizeof path, "build/skin/cb%d.bin", c + 3);
                if ((f = fopen(path, "wb"))) { fwrite((void *)(uintptr_t)st.ubuf[0][c].addr, 1, st.ubuf[0][c].size, f); fclose(f); }
            }
            {   // compiler constants (cb1) live after the shader code
                NvnShaderStage *ss = &st.program->stages[0];
                if (ss->const_offset && (f = fopen("build/skin/cb1.bin", "wb"))) {
                    fwrite((void *)(uintptr_t)(ss->code_addr + ss->const_offset), 1, ss->const_size ? ss->const_size : 256, f);
                    fclose(f);
                }
            }
            LOG("[skin] dumped draw inputs of vs %llx to build/skin/\n", (unsigned long long)st.program->stages[0].code_addr);
            extern uint64_t g_skin_watch;  // (renderer_stats: is this cbuf filled later, by the GPU?)
            g_skin_watch = st.ubuf[0][0].addr;
            const uint32_t *w = (const uint32_t *)(uintptr_t)g_skin_watch;
            LOG("[skin] cb3 %llx at draw: row8 %08x %08x row9 %08x %08x\n", (unsigned long long)g_skin_watch, w[32], w[33], w[36], w[37]);
        }
    }
    if (getenv("LARECOMP_DEBUG_VSCB"))
        LOG("[draw]   vbuf0 %llx+%llu vbuf1 %llx+%llu\n", (unsigned long long)st.vbuf[0].addr,
            (unsigned long long)st.vbuf[0].size, (unsigned long long)st.vbuf[1].addr, (unsigned long long)st.vbuf[1].size);
    if (getenv("LARECOMP_DEBUG_VSCB") && st.vbuf[0].addr) {  // first vertices: bytes of attrs 11 / 12
        const uint8_t *v = (const uint8_t *)(uintptr_t)st.vbuf[0].addr;
        uint32_t stride = st.stream[0][0];
        for (int i = 0; i < 4 && stride; i++) {
            const uint8_t *p = v + (uint64_t)i * stride;
            LOG("[draw]   v%d pos %.3f %.3f %.3f", i, ((const float *)p)[0], ((const float *)p)[1], ((const float *)p)[2]);
            for (int a = 11; a <= 12; a++) {
                if (!((pr->vi->attr_mask >> a) & 1)) continue;
                const uint8_t *q = p + (st.attrib[a] >> 13);
                LOG(" a%d %u %u %u %u", a, q[0], q[1], q[2], q[3]);
            }
            LOG("\n");
        }
    }
    for (int c = 0; c < 18 && getenv("LARECOMP_DEBUG_VSCB"); c++) {  // dynamically indexed vertex cbufs (bone palettes)
        if (!((pr->vi->cbuf_dynamic >> (c + 3)) & 1) || !st.ubuf[0][c].addr) continue;
        const float *u = (const float *)(uintptr_t)st.ubuf[0][c].addr;
        LOG("[draw]   vcb%d %llx size %u:", c, (unsigned long long)st.ubuf[0][c].addr, (unsigned)st.ubuf[0][c].size);
        int n = (int)(st.ubuf[0][c].size / 4), bad = 0, ident = 0;
        float mx = 0;
        for (int k = 0; k < n; k++) {
            float f = u[k];
            if (f != f || f > 1e6f || f < -1e6f) bad++;
            else if (fabsf(f) > mx) mx = fabsf(f);
        }
        for (int m = 0; m + 16 <= n; m += 16) {
            bool id = true;
            for (int k = 0; k < 16; k++) if (u[m + k] != ((k % 5) == 0 ? 1.0f : 0.0f)) id = false;
            ident += id;
        }
        LOG(" floats %d, non-finite/huge %d, max |x| %.4g, identity 4x4 %d of %d | m1:", n, bad, mx, ident, n / 16);
        for (int k = 16; k < 32 && k < n; k++) LOG(" %.4g", u[k]);
        LOG(" | m5:");
        for (int k = 80; k < 96 && k < n; k++) LOG(" %.4g", u[k]);
        LOG("\n");
    }
    if (getenv("LARECOMP_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18; c++) {
            if (!((pr->fi->cbuf_mask >> c) & 1) || !st.ubuf[1][c].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[1][c].addr;
            LOG("[draw]   fcb%d %llx size %u:", c, (unsigned long long)st.ubuf[1][c].addr, (unsigned)st.ubuf[1][c].size);
            for (int k = 0; k < 96 && k * 4 < (int)st.ubuf[1][c].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s | target %u flags %#x layers %u bh %u addr %llx "
            "stride %llu%s%s '%s'\n", i, (void *)t, t->width, t->height, t->format,
            b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0, b && b->scalable ? " scalable" : "",
            t->target, t->flags, t->layout.layers, t->layout.block_height_log2, (unsigned long long)t->addr,
            (unsigned long long)t->stride, b && b->gpu_owned ? " gpu-owned" : "", b && b->dynamic ? " dynamic" : "", t->label);
    }
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    if (getenv("LARECOMP_LOG_BUF2TEX") && (t->format == 0x1 || t->format == 0xd)) {  // (movie planes)
        static int n;
        if (n++ < 12) {
            const uint8_t *p = (const uint8_t *)(uintptr_t)src;
            unsigned sum = 0;
            for (int i = 0; i < 4096; i++) sum += p[(uint64_t)i * 97 % (row * (uint64_t)rg[4] ? row * (uint64_t)rg[4] : 1)];
            LOG("[vk] buf2tex src %llx -> %ux%u fmt %#x region %d,%d %dx%d row %llu sample-sum %u\n",
                (unsigned long long)src, t->width, t->height, t->format, rg[0], rg[1], rg[3], rg[4],
                (unsigned long long)row, sum);
        }
    }
    extern void vk_tex_unscale(NvnTexture *t);
    vk_tex_unscale(t);  // the copied data is full size: no output-resolution scaling for this texture
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();  // the CPU may read the result right after the fence
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)s->width, sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)s->height;
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)d->width, dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the level's extent, and every region stays inside its level: scaled allocations
        // (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies / blits are
        // undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's users)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    if (g_debug_frame)
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[4];
    for (int i = 0; i < 4; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 4; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

static VkPipeline post_pipeline(const char *fs) {
    static struct { const char *fs; VkPipeline p; } cache[8];
    for (int i = 0; i < 8; i++) if (cache[i].p && cache[i].fs == fs) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 8; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].p = p; break; }
    return p;
}

// Post image `i` at w x h (RGBA8, kept until the size changes). 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS.
static TexBackend *post_image(int i, uint32_t w, uint32_t h) {
    static TexBackend img[5];
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = VK_FORMAT_R8G8B8A8_UNORM; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    VkPipeline pipe = post_pipeline(fs);  // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {dst->width, dst->height}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = {{0, 0}, {dst->width, dst->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
    VkImageView v = raw_view(b);
    bool smaa = g_settings.antialias == AA_SMAA;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    bool fsr = g_settings.upscaler == UPSCALE_FSR1 && (ow > b->width + 1 || oh > b->height + 1);  // (an upscale only)
    if (!smaa && !fsr) return v;
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, b->width, b->height), *weights = post_image(1, b->width, b->height);
        TexBackend *out = post_image(2, b->width, b->height);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
    }
    if (fsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    vk_full_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || fsr != pfs)
            LOG("[post] %ux%u%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "", fsr ? " fsr1" : "", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = smaa; pfs = fsr;
    }
    return v;
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
// (bx,by,bw,bh): the viewport in the swapchain image; (pw,ph): the upright size the post passes produce
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float bx, float by, float bw, float bh, float pw, float ph, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkImageView src = post_process(cb, b, pv[6] > 0 ? pw / pv[6] : pw, pv[7] > 0 ? ph / pv[7] : ph);
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {bx, by, bw, bh, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
void renderer_save_texture_tex(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pv[8] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_save_png(path, out.ptr, w, h);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

// ================================================================== executor (renderer.m exec_list)
static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    if (depth == 0) st.obj = 0, st.obj_draw = 0;  // (a submitted list starts with no object; its called lists inherit)
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        if (ip_replaying && ip_skip_op(r->op)) { p += r->size; continue; }
        switch (r->op) {
        case CMD_END: return;
        case CMD_OBJECT: st.obj = r->x[0]; st.obj_draw = 0; { static int d; if (getenv("LARECOMP_OBJDBG") && vk_frame_counter() == 1450 && d++ < 80) LOG("[objdbg] depth %d object %llx\n", depth,(unsigned long long)r->x[0]); } break;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            uint64_t obj = st.obj;  // (a called list names objects of its own: they end with it)
            uint32_t obj_draw = st.obj_draw;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) {
                st.obj = obj, st.obj_draw = obj_draw;
                exec_list(h[i], depth + 1);
            }
            st.obj = obj, st.obj_draw = obj_draw;
            break;
        }
        case CMD_BIND_PROGRAM: bind_program_stages((NvnProgram *)(uintptr_t)r->x[0], (uint32_t)r->x[1]); break;
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            static int always = -1;
            if (always < 0) always = getenv("LARECOMP_VK_PASS_RESTART") != NULL;
            if (always || memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (getenv("LARECOMP_LOG_CLEARS") && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x color %.3f %.3f %.3f %.3f\n", (unsigned long long)renderer_present_count(),
                    index, st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2],
                    ((const float *)x)[0], ((const float *)x)[1], ((const float *)x)[2], ((const float *)x)[3]);
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            guest_copy_flush();  // (cbuf copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC: {
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow;
            // the value is our translation time, not GPU time. LARECOMP_VK_COUNTER=frozen: the same timestamp for
            // every counter within a presented frame (experiment).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            static int frozen = -1;  // 1: frozen; 2: ideal (LARECOMP_VK_COUNTER=ideal: a 33.3 ms frame, 1 ms per counter in it)
            if (frozen < 0) { const char *e = getenv("LARECOMP_VK_COUNTER"); frozen = !e ? 0 : !strcmp(e, "frozen") ? 1 : !strcmp(e, "ideal") ? 2 : 0; }
            static uint64_t frame_seen = ~0ull, frame_ts, in_frame;
            extern uint64_t vk_frame_counter(void);
            if (vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = SDL_GetTicksNS(); in_frame = 0; }
            uint64_t v = frozen == 1 ? frame_ts : frozen == 2 ? frame_seen * 33333333ull + in_frame * 1000000ull : SDL_GetTicksNS();
            in_frame++;
            if (out) { out[0] = 0; out[1] = v; }
            static int clog = -1;  // LARECOMP_COUNTER_LOG=1: each counter, with the draws recorded since the previous one
            if (clog < 0) clog = getenv("LARECOMP_COUNTER_LOG") != NULL;
            static uint64_t last_draws;
            if (clog) LOG("[counter] frame %llu #%llu value %llu draws %llu\n", (unsigned long long)frame_seen, (unsigned long long)in_frame,
                          (unsigned long long)v, (unsigned long long)(g_draws_total - last_draws));
            last_draws = g_draws_total;
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        default: break;
        }
        p += r->size;
    }
}

void backend_execute(uint64_t handle) {
    if (ENV("LARECOMP_EXP_NOEXEC")) return;  // (research: the game's chain without the backend's work; the picture stays blank)
    PROF_T0();
    if (ip_enabled()) ip_note_list(handle);
    exec_list(handle, 0);
    PROF_ADD(prof_exec);
    if (prof) cum_exec += SDL_GetTicksNS() - prof_t0_;
    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = getenv("LARECOMP_SYNC_SUBMIT") ? 1 : 0;
    if (sync_submit) renderer_flush();
    else renderer_submit();
}

// ================================================================== frame interpolation (prototype)
// LARECOMP_INTERP=1: 60 fps from the game's 30 by drawing an in-between frame (docs/PESQUISA-60FPS.md). The game's
// logic advances one step per frame, so the frame rate can't simply be raised; instead, like RT64 (Zelda64Recomp)
// and Ship of Harkinian, frame N's draws are run a second time with their constant buffers (camera, world, bone
// matrices) blended halfway between N-1 and N, and that image is shown half a frame before N.
//  - recording: the command lists submitted in a frame (and a hash of their records), the exec state at the frame's
//    start, and a copy of every vertex / fragment constant buffer a draw binds (cbuf 3 and up), keyed by stage,
//    binding, the program, vertex buffer 0, the draw's parameters and the occurrence of that key in the frame;
//  - replay (vk_interp_render, at the present): the same lists from the same start state, skipping what writes guest
//    memory (compute, copies to buffers, inline uniform updates, syncs, counters); each constant buffer is the blend of
//    its two copies, word by word, for normal floats close enough (other words, and draws without a match, keep N);
//  - a list whose records changed since it ran (the game reused its memory) cancels that frame's in-between image.
typedef struct { uint64_t key; uint32_t off, size; uint64_t obj; } IpEntry;  // (obj: the game object of the draw)
typedef struct {
    IpEntry *e;
    uint32_t n, cap;
    uint8_t *data;
    uint64_t used, dcap;
    uint32_t *tab;  // open addressing: key -> e index + 1
    uint64_t lists[256], hashes[256];
    int nlists;
    bool overflow, valid;
    uint64_t destroyed;  // g_nvn_destroyed when the frame started
    __typeof__(st) start;
} IpFrame;
extern uint64_t g_nvn_destroyed;  // (objects.c: textures, pools, programs the game finalized)
#define IP_TAB (1u << 16)
static IpFrame ipf[2];
static int ipc;  // ipf[ipc]: the frame being recorded; ipf[ipc ^ 1]: the one before
static float ip_t = 0.5f;
static uint32_t ip_occ[IP_TAB];  // occurrences of each base key in this frame / replay (by slot)
static uint64_t ip_occ_key[IP_TAB];
static uint64_t ip_stats[4];  // blended, kept (no match), words kept, frames replayed
// LARECOMP_INTERP_DIAG=1: what the in-between image is made of (docs/PESQUISA-60FPS.md, "Diagnóstico da interpolação")
typedef struct { int stage, k; uint32_t size; uint64_t draws, nomatch, tagged, words, changed, blended, jumps, nonfloat; } IpDiag;
static IpDiag ip_diag[96];
static int ip_ndiag;
static uint64_t ip_rej[8];  // frames without an in-between image: no previous frame, overflow, destroyed, format, list changed
static IpDiag *ip_diag_for(int stage, int k, uint32_t size) {
    for (int i = 0; i < ip_ndiag; i++)
        if (ip_diag[i].stage == stage && ip_diag[i].k == k && ip_diag[i].size == size) return &ip_diag[i];
    if (ip_ndiag >= 96) return &ip_diag[95];
    ip_diag[ip_ndiag] = (IpDiag){.stage = stage, .k = k, .size = size};
    return &ip_diag[ip_ndiag++];
}
static void ip_diag_dump(void) {
    LOG("[interp-diag] rejected frames: no-prev %llu overflow %llu destroyed %llu format %llu list-changed %llu; replayed %llu\n",
        (unsigned long long)ip_rej[0], (unsigned long long)ip_rej[1], (unsigned long long)ip_rej[2], (unsigned long long)ip_rej[3],
        (unsigned long long)ip_rej[4], (unsigned long long)ip_stats[3]);
    for (int i = 0; i < ip_ndiag; i++) {
        const IpDiag *d = &ip_diag[i];
        LOG("[interp-diag] stage %d cbuf %d size %u: binds %llu (tagged %llu, no match %llu) words %llu changed %llu blended %llu jumps %llu nonfloat %llu\n",
            d->stage, d->k, d->size, (unsigned long long)d->draws, (unsigned long long)d->tagged, (unsigned long long)d->nomatch,
            (unsigned long long)d->words, (unsigned long long)d->changed, (unsigned long long)d->blended, (unsigned long long)d->jumps,
            (unsigned long long)d->nonfloat);
    }
    memset(ip_diag, 0, sizeof ip_diag);
    ip_ndiag = 0;
    memset(ip_rej, 0, sizeof ip_rej);
}

static uint64_t ip_mix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h * 0x100000001B3ull; }

static void ip_occ_reset(void) { memset(ip_occ_key, 0, sizeof ip_occ_key); }

static uint64_t ip_key(int stage, int k) {
    uint64_t base = ip_mix(ip_mix(ip_draw_sig, (uint64_t)stage), (uint64_t)k);
    if (!base) base = 1;
    uint32_t s = (uint32_t)(base ^ (base >> 29)) & (IP_TAB - 1);
    while (ip_occ_key[s] && ip_occ_key[s] != base) s = (s + 1) & (IP_TAB - 1);
    if (!ip_occ_key[s]) { ip_occ_key[s] = base; ip_occ[s] = 0; }
    return ip_mix(base, ip_occ[s]++);
}

static const IpEntry *ip_find(const IpFrame *f, uint64_t key) {
    if (!f->tab) return NULL;
    uint32_t s = (uint32_t)(key ^ (key >> 31)) & (IP_TAB - 1);
    while (f->tab[s]) {
        const IpEntry *e = &f->e[f->tab[s] - 1];
        if (e->key == key) return e;
        s = (s + 1) & (IP_TAB - 1);
    }
    return NULL;
}

static void ip_frame_clear(IpFrame *f) {
    f->n = 0; f->used = 0; f->nlists = 0; f->overflow = false; f->valid = false;
    if (f->tab) memset(f->tab, 0, IP_TAB * sizeof *f->tab);
}

static void ip_record(uint64_t key, const void *data, uint32_t size) {
    IpFrame *f = &ipf[ipc];
    if (!f->tab) f->tab = calloc(IP_TAB, sizeof *f->tab);
    if (f->n >= IP_TAB / 2) { f->overflow = true; return; }
    if (f->n == f->cap) { f->cap = f->cap ? f->cap * 2 : 4096; f->e = realloc(f->e, f->cap * sizeof *f->e); }
    if (f->used + size > f->dcap) { f->dcap = (f->used + size) * 2 + (1 << 20); f->data = realloc(f->data, f->dcap); }
    memcpy(f->data + f->used, data, size);
    f->e[f->n] = (IpEntry){key, (uint32_t)f->used, size, st.obj};
    f->used += size;
    uint32_t s = (uint32_t)(key ^ (key >> 31)) & (IP_TAB - 1);
    while (f->tab[s]) s = (s + 1) & (IP_TAB - 1);
    f->tab[s] = ++f->n;
}

// cbuf_info, constant buffers 3+ of the vertex / fragment stage: recording copies them (returns NULL: bind as usual);
// the replay returns the blended data to bind.
// Objects whose buffers changed too much between N-1 and N to be blended (vk_interp_render's check, before the replay):
// a camera cut, a model swapped (another level of detail, another skeleton) or two draws paired wrongly. Blending them
// mixes unrelated poses - characters stretched across the screen, Pokémon torn apart. They keep frame N's pose.
#define IP_REJ 1024
static uint64_t ip_rej_obj[IP_REJ];
static bool ip_obj_rejected(uint64_t obj) {
    uint32_t s = (uint32_t)((obj >> 4) * 0x9E3779B1u) & (IP_REJ - 1);
    while (ip_rej_obj[s]) { if (ip_rej_obj[s] == obj) return true; s = (s + 1) & (IP_REJ - 1); }
    return false;
}
static const void *ip_cbuf(int stage, int k, uint64_t addr, uint32_t size) {
    if (stage > 1) return NULL;
    uint64_t key = ip_key(stage, k);
    if (!ip_replaying) { ip_record(key, (const void *)(uintptr_t)addr, size); return NULL; }
    const IpEntry *n = ip_find(&ipf[ipc], key), *o = ip_find(&ipf[ipc ^ 1], key);
    static int diag = -1;
    if (diag < 0) diag = getenv("LARECOMP_INTERP_DIAG") ? 1 : 0;
    IpDiag *dg = diag ? ip_diag_for(stage, k, size) : NULL;
    if (dg) { dg->draws++; if (ip_draw_obj) dg->tagged++; if (!o) dg->nomatch++; }
    if (!n) return NULL;
    const uint8_t *nd = ipf[ipc].data + n->off;
    if (!ip_draw_obj || ip_obj_rejected(st.obj ? st.obj : 1)) { ip_stats[1]++; return nd; }  // interface and other draws no object owns: their order and text change between frames, a blend smears them
    if (!o || o->size != n->size) { ip_stats[1]++; return nd; }
    static uint32_t buf[0x10000 / 4];
    const uint32_t *a = (const uint32_t *)(ipf[ipc ^ 1].data + o->off), *b = (const uint32_t *)nd;
    uint32_t nw = n->size / 4;
    // A jump anywhere in the buffer (a bone, a matrix row) keeps all of it at N: blending the rest would pull part of a
    // skeleton or of a matrix halfway and leave the other part, which stretches the model.
    for (uint32_t i = 0; i < nw; i++) {
        if (a[i] == b[i]) continue;
        uint32_t ea = (a[i] >> 23) & 0xFF, eb = (b[i] >> 23) & 0xFF;
        if (!ea || !eb || ea == 0xFF || eb == 0xFF) continue;
        float fa, fb;
        memcpy(&fa, &a[i], 4);
        memcpy(&fb, &b[i], 4);
        bool jump = ip_draw_2d ? fabsf(fb - fa) > 0.3f * fmaxf(fabsf(fa), fabsf(fb)) && fabsf(fb - fa) > 0.02f  // (2D: a big move is a recycled row or glyph, not motion)
                               : fabsf(fb - fa) > 0.5f * (fabsf(fa) + fabsf(fb)) + 4.0f;
        if (jump) {
            if (dg) dg->jumps += nw;
            ip_stats[1]++;
            return nd;
        }
    }
    // Blended a vec4 at a time, all or nothing: a matrix row with some components blended and others kept (one of them
    // crossing zero, or an integer in a float's place) is not a transformation any more - it shears and shrinks the model.
    // Words that are equal need no test; the others must be finite floats (0 counts) without a jump.
    for (uint32_t g = 0; g < nw; g += 4) {
        uint32_t m = nw - g < 4 ? nw - g : 4;
        int bad = 0, changed = 0, jump = 0;
        for (uint32_t i = g; i < g + m; i++) {
            if (a[i] == b[i]) continue;
            changed++;
            uint32_t ea = (a[i] >> 23) & 0xFF, eb = (b[i] >> 23) & 0xFF;
            bool fa_ok = ea != 0xFF && (ea || !(a[i] & 0x7FFFFF)), fb_ok = eb != 0xFF && (eb || !(b[i] & 0x7FFFFF));
            float fa, fb;
            memcpy(&fa, &a[i], 4);
            memcpy(&fb, &b[i], 4);
            if (!fa_ok || !fb_ok) bad++;
            else if (fabsf(fb - fa) > 0.5f * (fabsf(fa) + fabsf(fb)) + 4.0f) jump++;
        }
        if (dg) { dg->words += m; dg->changed += changed; }
        if (!bad && !jump) {
            for (uint32_t i = g; i < g + m; i++) {
                if (a[i] == b[i]) { buf[i] = b[i]; continue; }
                float fa, fb, v;
                memcpy(&fa, &a[i], 4);
                memcpy(&fb, &b[i], 4);
                v = fa + (fb - fa) * ip_t;
                memcpy(&buf[i], &v, 4);
            }
            if (dg) dg->blended += changed;
        } else {
            for (uint32_t i = g; i < g + m; i++) buf[i] = b[i];
            ip_stats[2] += changed;
            if (dg) { dg->jumps += jump ? changed : 0; dg->nonfloat += bad ? changed : 0; }
        }
    }
    if (n->size > nw * 4) memcpy((uint8_t *)buf + nw * 4, nd + nw * 4, n->size - nw * 4);
    ip_stats[0]++;
    return buf;
}

// hash of a command list's records (JUMPs and CALLs followed), to tell whether its memory still holds what ran
static uint64_t ip_list_hash(uint64_t handle, int depth) {
    uint64_t h = 0xcbf29ce484222325ull, p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        const CmdRec *r = (const CmdRec *)(uintptr_t)p;
        if (r->op >= CMD_COUNT || r->size < sizeof(CmdRec) || r->size > (1u << 20)) return 0;
        const uint64_t *w = (const uint64_t *)r;
        for (uint32_t i = 0; i < r->size / 8; i++) h = ip_mix(h, w[i]);
        if (r->op == CMD_END) return h;
        if (r->op == CMD_JUMP) { p = r->x[0]; continue; }
        if (r->op == CMD_CALL) {
            const uint64_t *hs = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) h = ip_mix(h, ip_list_hash(hs[i], depth + 1));
        }
        p += r->size;
    }
    return 0;
}

static void ip_note_list(uint64_t handle) {
    IpFrame *f = &ipf[ipc];
    if (f->nlists >= 256) { f->overflow = true; return; }
    f->hashes[f->nlists] = ip_list_hash(handle, 0);
    f->lists[f->nlists++] = handle;
}

// The hold image: frame N's presented texture, kept while the replay overwrites it with the in-between image.
static TexBackend ip_hold;
static TexBackend *ip_hold_copy(TexBackend *src) {
    TexBackend *d = &ip_hold;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                                .imageType = VK_IMAGE_TYPE_2D, .format = src->format, .extent = {src->width, src->height, 1},
                                .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    d->sx = src->sx; d->sy = src->sy; d->scalable = src->scalable;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                      .extent = {src->width, src->height, 1}};
    vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
    vk_full_barrier(cb);
    return d;
}

// At the present of frame N (presented texture b): when frame N-1 was recorded too and N's lists are intact, renders
// the in-between image into the game's textures (b then holds it) and returns frame N's own image in *hold; false:
// nothing to show before N. Either way, recording moves on to the next frame.
bool vk_interp_render(TexBackend *b, TexBackend **hold) {
    if (!ip_enabled()) {
        // Off (the world, a battle): what was recorded the last time it was on is stale - a menu opened later blended
        // with that old scene (a flash when the menu opened while walking)
        ipf[0].valid = ipf[1].valid = false;
        return false;
    }
    IpFrame *cur = &ipf[ipc], *prev = &ipf[ipc ^ 1];
    bool ok = prev->valid && cur->valid && cur->nlists > 0 && !cur->overflow && !prev->overflow && b && b->image &&
              b->aspect == VK_IMAGE_ASPECT_COLOR_BIT && b->samples <= 1 && cur->destroyed == g_nvn_destroyed;
    bool structural = ok;
    for (int i = 0; ok && i < cur->nlists; i++)
        if (!cur->hashes[i] || ip_list_hash(cur->lists[i], 0) != cur->hashes[i]) ok = false;
    if (!ok) {
        if (!prev->valid || !cur->valid || cur->nlists == 0) ip_rej[0]++;
        else if (cur->overflow || prev->overflow) ip_rej[1]++;
        else if (cur->destroyed != g_nvn_destroyed) ip_rej[2]++;
        else if (!structural) ip_rej[3]++;
        else ip_rej[4]++;
    }
    if (ok) {
        // Per object, before the replay: the share of its changed float words that changed by a large step. Above
        // LARECOMP_INTERP_OBJ_LIMIT (0.2) the object is not blended (ip_obj_rejected); when the rejected objects hold
        // most of the frame's changes (a camera cut), there is no in-between image at all.
        typedef struct { uint64_t obj, changed, big; } ObjAcc;
        static ObjAcc acc[IP_REJ];
        memset(acc, 0, sizeof acc);
        memset(ip_rej_obj, 0, sizeof ip_rej_obj);
        uint64_t changed = 0, big = 0, rej_changed = 0;
        int nrej = 0, nobj = 0;
        for (uint32_t i = 0; i < cur->n; i++) {
            const IpEntry *n = &cur->e[i], *o = ip_find(prev, n->key);
            if (!o || o->size != n->size) continue;
            uint64_t gobj = n->obj ? n->obj : 1;  // (draws without an object: one group)
            const float *fa = (const float *)(prev->data + o->off), *fb = (const float *)(cur->data + n->off);
            uint64_t c = 0, bg = 0;
            for (uint32_t w = 0; w < n->size / 4; w++) {
                float a = fa[w], v = fb[w];
                if (a == v || !isfinite(a) || !isfinite(v)) continue;
                float m = fmaxf(fabsf(a), fabsf(v));
                if (m < 1e-3f || m > 1e7f) continue;
                c++;
                if (fabsf(v - a) > 0.3f * m && fabsf(v - a) > 0.1f) bg++;
            }
            if (!c) continue;
            uint32_t sl = (uint32_t)((gobj >> 4) * 0x9E3779B1u) & (IP_REJ - 1);
            while (acc[sl].obj && acc[sl].obj != gobj) sl = (sl + 1) & (IP_REJ - 1);
            if (!acc[sl].obj) { acc[sl].obj = gobj; nobj++; }
            acc[sl].changed += c, acc[sl].big += bg;
            changed += c, big += bg;
        }
        extern bool fps60_menu(void);
        bool menu = fps60_menu();  // (menus: the cut checks are for battles; a menu screen changes by large steps all the time)
        static double obj_limit = -1;
        if (obj_limit < 0) obj_limit = getenv("LARECOMP_INTERP_OBJ_LIMIT") ? atof(getenv("LARECOMP_INTERP_OBJ_LIMIT")) : 0.2;
        for (int i = 0; i < IP_REJ; i++)
            if (!menu && acc[i].obj && acc[i].changed >= 8 && (double)acc[i].big > obj_limit * (double)acc[i].changed) {
                uint32_t sl = (uint32_t)((acc[i].obj >> 4) * 0x9E3779B1u) & (IP_REJ - 1);
                while (ip_rej_obj[sl]) sl = (sl + 1) & (IP_REJ - 1);
                ip_rej_obj[sl] = acc[i].obj;
                nrej++;
                rej_changed += acc[i].changed;
            }
        if (getenv("LARECOMP_INTERP_DIAG"))
            LOG("[interp-cut] present %llu: objects %d rejected %d (%llu of %llu changed words; big %llu)\n",
                (unsigned long long)renderer_present_count(), nobj, nrej, (unsigned long long)rej_changed, (unsigned long long)changed,
                (unsigned long long)big);
        if (!menu && changed && rej_changed * 10 > changed * 6) { ok = false; ip_rej[5]++; }
    }
    if (ok) {
        vk_end_pass();
        *hold = ip_hold_copy(b);
        __typeof__(st) end = st;
        st = cur->start;
        st.rt_dirty = true;
        ip_replaying = true;
        ip_occ_reset();
        for (int i = 0; i < cur->nlists; i++) exec_list(cur->lists[i], 0);
        vk_end_pass();
        ip_replaying = false;
        st = end;
        st.rt_dirty = true;
        ip_stats[3]++;
        if (ip_stats[3] % 300 == 1)
            LOG("[interp] frame %llu: %llu blended, %llu without a match, %llu words kept (replays %llu)\n",
                (unsigned long long)vk_frame_counter(), (unsigned long long)ip_stats[0], (unsigned long long)ip_stats[1],
                (unsigned long long)ip_stats[2], (unsigned long long)ip_stats[3]);
        if (ip_stats[3] % 300 == 0 && getenv("LARECOMP_INTERP_DIAG")) ip_diag_dump();
        if (ip_stats[3] % 300 == 1)
            LOG("[interp] draws with an object %llu, without %llu\n", (unsigned long long)ip_stats_tagged, (unsigned long long)ip_stats_untagged);
    }
    // the next frame records into the other slot, from the state this one ended with
    ipc ^= 1;
    ip_frame_clear(&ipf[ipc]);
    ipf[ipc].start = st;
    ipf[ipc].destroyed = g_nvn_destroyed;
    ipf[ipc].valid = true;
    ip_occ_reset();
    return ok;
}
