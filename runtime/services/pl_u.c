// pl:u — SharedFontManager IPC service
// Implements the font service via IPC, routing to the HLE font data in hle/pl.c.
// The game calls pl:u via Service Manager; without this, it gets a logging stub
// that returns only "ok" without font data, causing empty dialog boxes.

#include <string.h>

#include "../hle.h"
#include "../ipc.h"

// From hle/pl.c
extern uint64_t pl_font_address(uint64_t type);
extern uint64_t pl_font_size(uint64_t type);

// pl:u commands (from Switch IPC documentation):
// 0: RequestLoad (type) -> (no output)
// 1: GetLoadState (type) -> u32 state (1 = loaded)
// 2: GetFontSize (type) -> u32 size
// 3: GetSharedMemoryAddressOffset (type) -> u64 offset (we have no shared mem: 0)
// 4: GetSharedMemoryNativeHandle () -> handle (we have no shared mem: 0)
//
// The game only calls cmd 0 (RequestLoad) in practice; the font data comes
// through the HLE nn::pl functions (hle/pl.c), not through IPC shared memory.

SERVICE("pl:u", pl_u) {
    (void)s;
    uint32_t type = req->data_size >= 4 ? IN(uint32_t, 0) : 0;
    switch (req->cmd) {
    case 0:  // RequestLoad (the game reads a load state of 1 here and moves on)
        LOG("[pl:u] cmd 0 RequestLoad (type=%u) -> load state 1\n", type);
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    case 1:  // GetLoadState
        LOG("[pl:u] cmd 1 GetLoadState (type=%u) -> 1\n", type);
        OUT(uint32_t, 0) = 1;
        resp_size(resp, 4);
        break;
    case 2:  // GetFontSize
        LOG("[pl:u] cmd 2 GetFontSize (type=%u) -> %llu\n", type,
            (unsigned long long)pl_font_size(type));
        OUT(uint32_t, 0) = (uint32_t)pl_font_size(type);
        resp_size(resp, 4);
        break;
    case 3:  // GetSharedMemoryAddressOffset (no shared mem: the HLE heap address is used instead)
        LOG("[pl:u] cmd 3 GetSharedMemoryAddressOffset (type=%u) -> 0 (HLE heap: 0x%llx)\n", type,
            (unsigned long long)pl_font_address(type));
        OUT(uint64_t, 0) = 0;
        resp_size(resp, 8);
        break;
    case 4:  // GetSharedMemoryNativeHandle (no shared mem)
        LOG("[pl:u] cmd 4 GetSharedMemoryNativeHandle -> 0 (unsupported)\n");
        resp_size(resp, 0);
        break;
    default:
        LOG("[pl:u] unhandled cmd %u (type=%u)\n", req->cmd, type);
        resp_size(resp, 0x40);
        break;
    }
}
