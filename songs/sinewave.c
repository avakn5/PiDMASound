#include "rpi.h"
#include "audio-dma.h"

// 1 kHz sine wave at 44.1 kHz sample rate — 44 samples/period
const short sinewave[44] = {
    0, 3730, 7391, 10890, 14177, 17160, 19777, 21970,
    23697, 24931, 25653, 26214, 25653, 24931, 23697, 21970,
    19777, 17160, 14177, 10890, 7391, 3730,
    0, -3730, -7391, -10890, -14177, -17160, -19777, -21970,
    -23697, -24931, -25653, -26214, -25653, -24931, -23697, -21970,
    -19777, -17160, -14177, -10890, -7391, -3730
};

void notmain(void) {
    trace("=== 1 kHz sinewave on GPIO 19 (double-buffered DMA) ===\n");
    audio_init();

    // play forever: 10 seconds at a time, looping
    while (1)
        play_tone(sinewave, 44, 10000000);
}
