// Deterministic check of dynamic resolution pacing, without launching the game.
// Build: clang -O2 -Iruntime tools/test_output_scale.c runtime/nvn/output_scale.c -o build/test_output_scale.exe
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "nvn/output_scale.h"

Settings g_settings = {.res_mode = RES_DYNAMIC};
static bool mode_60;
bool fps60_enabled(void) { return mode_60; }
bool fps60_world(void) { return mode_60; }
bool fps60_menu(void) { return false; }

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "transition")) {
        OutScale out;
        double now = 1.0;
        for (int phase = 0; phase < 3; phase++) {
            mode_60 = phase == 1;
            double frame_ms = mode_60 ? 16.666667 : 33.333333;
            for (int i = 0; i < 90; i++) {
                now += frame_ms / 1000.0;
                output_scale(1920, 1080, false, now, -1, &out);
            }
        }
        double factor = output_scale_dynamic_factor();
        printf("transition 30->60->30 scale=%.2f\n", factor);
        return factor >= 0.99 ? 0 : 1;
    }
    if (argc != 3) return 2;
    mode_60 = atoi(argv[1]) == 60;
    double frame_ms = atof(argv[2]);
    OutScale out;
    for (int i = 1; i <= 180; i++)
        output_scale(1920, 1080, false, 1.0 + i * frame_ms / 1000.0, -1, &out);
    double factor = output_scale_dynamic_factor();
    printf("target=%s measured=%.2fms scale=%.2f\n", argv[1], frame_ms, factor);
    if (frame_ms <= (mode_60 ? 16.67 : 33.34)) return factor >= 0.99 ? 0 : 1;
    return factor < 0.99 ? 0 : 1;
}
