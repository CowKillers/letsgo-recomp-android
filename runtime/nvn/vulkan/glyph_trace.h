// LARECOMP_TRACE_GLYPHS: selective tracing for Beluga font engine diagnostics.
// Disabled by default. Enable with: LARECOMP_TRACE_GLYPHS=1
// Traces the path: BFOTF read -> font parse -> glyph select -> rasterize -> buffer write -> R8 texture upload

#ifndef LARECOMP_GLYPH_TRACE_H
#define LARECOMP_GLYPH_TRACE_H

#include <stdlib.h>
#include <stdint.h>

// Check if tracing is enabled (cached)
static inline int glyph_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = getenv("LARECOMP_TRACE_GLYPHS") != NULL ? 1 : 0;
    }
    return enabled;
}

// Simple hash for buffer comparison (FNV-1a)
static inline uint64_t glyph_hash(const uint8_t *data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    size_t n = len > 4096 ? 4096 : len; // hash first 4KB max
    for (size_t i = 0; i < n; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// Count non-zero bytes
static inline uint64_t glyph_nonzero(const uint8_t *data, size_t len) {
    uint64_t nz = 0;
    for (size_t i = 0; i < len; i++) {
        if (data[i]) nz++;
    }
    return nz;
}

#endif
