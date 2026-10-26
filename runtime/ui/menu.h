// In-game overlay (Dear ImGui): the "close the game?" question (Esc, F1, or Minus+Plus on a controller), the name
// entry, the FPS counter and the controller's finger on the touch screen. runtime/ui/menu.mm.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union SDL_Event SDL_Event;

// Main (UI) thread: returns true if the event was consumed by the overlay (the game must not see it).
bool menu_handle_event(const SDL_Event *e);
bool menu_is_open(void);
// Main thread: apply window requests (fullscreen, text input) made from the overlay.
void menu_apply_window_requests(void *sdl_window);
// Render thread, inside the window image's rendering (Vulkan command buffer cmd).
void menu_render(void *cmd, uint32_t target_w, uint32_t target_h);

#ifdef __cplusplus
}
#endif
