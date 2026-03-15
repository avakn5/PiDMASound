#ifndef AUDIO_DMA_H
#define AUDIO_DMA_H

// Initialize PWM clock, DMA, interrupts, and static CB chains for double-buffered audio playback.
void audio_init(void);

// Play a waveform using double-buffered DMA for duration_us microseconds.
// samples: one period of the waveform, n_samples: length of that period.

// Each buffer = 16 PCM samples × 32 oversample = 512 PDM bits.
// DREQ at 1.411 MHz, buffer duration = 362.9 µs, ISR at 2756 Hz.
// Sample rate = 44,100 Hz.
void play_tone(const short *samples, unsigned n_samples, unsigned duration_us);

// Silence: stop DMA, hold pin low for us microseconds.
void rest(unsigned us);

#endif
