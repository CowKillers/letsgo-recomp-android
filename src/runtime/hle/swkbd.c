// Software keyboard (naming Link on a new file, ...). On the Switch nn::swkbd::ShowKeyboard launches the keyboard
// library applet; our am service has no applets, so the game crashed. ShowKeyboard is replaced here: the overlay
// (runtime/ui/menu.mm menu_keyboard_run) asks for the text - typed directly with a keyboard, an on-screen keyboard
// with a controller - while this (game) thread keeps re-presenting the last frame.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

int menu_keyboard_run(const char *initial, int max_len, char *out, int cap);  // runtime/ui/menu.mm

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t bufSize}
// ShowKeyboardArg (probed 2026-10-08, naming Link): +0x3ac text max length, +0x3b0 min length, +0x3c0 / +0x3c4
// initial text offset / length (UTF-16) in the work buffer at +0x4d0. The game reads the answer as UTF-8.
HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out), size = LD64(out + 8);
    int max_len = (int)*(const int32_t *)(uintptr_t)(arg + 0x3ac);
    uint32_t off = *(const uint32_t *)(uintptr_t)(arg + 0x3c0), n = *(const uint32_t *)(uintptr_t)(arg + 0x3c4);
    uint64_t wb = LD64(arg + 0x4d0);
    char initial[96] = "";
    for (uint32_t i = 0; wb && i < n && i < sizeof initial - 1; i++) {
        uint16_t ch = *(const uint16_t *)(uintptr_t)(wb + off + 2 * i);
        initial[i] = ch < 0x80 ? (char)ch : '?';
    }
    char text[96];
    int ok = menu_keyboard_run(initial, max_len, text, sizeof text);
    if (!ok) snprintf(text, sizeof text, "%s", initial);  // cancelled: keep what was there
    LOG("[swkbd] name entry (max %d, initial \"%s\"): %s \"%s\"\n", max_len, initial, ok ? "entered" : "cancelled", text);
    if (ptr && size) {
        memset((void *)(uintptr_t)ptr, 0, size);
        if (getenv("LARECOMP_SWKBD_UTF8")) {  // (Link's Awakening read its answer as UTF-8)
            snprintf((char *)(uintptr_t)ptr, size, "%s", text);
        } else {
            // Let's Go reads UTF-16: "RRS" written as UTF-8 came back as U+5252 'S' ("?S" on screen). The text is
            // UTF-8 from the overlay: decode it and store UTF-16LE, NUL-terminated.
            uint16_t *o = (uint16_t *)(uintptr_t)ptr;
            uint64_t cap = size / 2, k = 0;
            for (const unsigned char *s = (const unsigned char *)text; *s && k + 1 < cap;) {
                uint32_t cp = *s++;
                int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
                if (more) cp &= 0x3F >> more;
                while (more-- && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
                if (cp >= 0x10000) {
                    if (k + 2 >= cap) break;
                    cp -= 0x10000;
                    o[k++] = (uint16_t)(0xD800 | (cp >> 10));
                    o[k++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
                } else {
                    o[k++] = (uint16_t)cp;
                }
            }
            o[k] = 0;
        }
    }
    c->x[0] = 0;
}
