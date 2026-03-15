#include "rpi.h"
#include "audio-dma.h"

// from interrupts-asm.S
void disable_interrupts(void);
void enable_interrupts(void);

volatile unsigned pdm_bit_index = 0;
volatile unsigned total_pdm_bits = 0;
unsigned char *pdm_out;

#define GPIO_PIN_OUT    18
#define DEBUG_PIN       11
#define OVERSAMPLE      64
#define SAMPLES_PER_BUF 16
#define BITS_PER_BUF    (SAMPLES_PER_BUF * OVERSAMPLE)  // 1024
#define WORDS_PER_BUF   (BITS_PER_BUF / 32)             // 32

// PWM registers — channel 1 serializer outputs PDM on GPIO 18
enum {
    PWM_BASE = 0x2020C000,
    PWM_CTL  = PWM_BASE + 0x00,
    PWM_STA  = PWM_BASE + 0x04,
    PWM_DMAC = PWM_BASE + 0x08,
    PWM_RNG1 = PWM_BASE + 0x10,
    PWM_FIF1 = PWM_BASE + 0x18,
};

enum {
    CTL_PWEN1 = 1 << 0,
    CTL_MODE1 = 1 << 1,     // serializer mode
    CTL_USEF1 = 1 << 5,     // use FIFO
    CTL_CLRF  = 1 << 6,
};

// Clock Manager registers
enum {
    CM_BASE    = 0x20101000,
    CM_PWMCTL  = CM_BASE + 0xA0,
    CM_PWMDIV  = CM_BASE + 0xA4,
    CM_PASSWD  = 0x5A000000,
};

enum {
    CM_SRC_PLLD = 6,
    CM_ENAB     = 1 << 4,
    CM_KILL     = 1 << 5,
    CM_BUSY     = 1 << 7,
    CM_MASH1    = 1 << 9,
};

// DMA registers
enum {
    DMA_BASE   = 0x20007000,
    DMA_ENABLE = DMA_BASE + 0xFF0,
};

#define DMA_CS(ch)      (DMA_BASE + (ch) * 0x100 + 0x00)
#define DMA_CONBLK(ch)  (DMA_BASE + (ch) * 0x100 + 0x04)
#define DMA_SOURCE(ch)  (DMA_BASE + (ch) * 0x100 + 0x0C)
#define DMA_DEST(ch)    (DMA_BASE + (ch) * 0x100 + 0x10)
#define DMA_TXLEN(ch)   (DMA_BASE + (ch) * 0x100 + 0x14)
#define DMA_DEBUG(ch)   (DMA_BASE + (ch) * 0x100 + 0x20)

#define DMA_CHANNEL     5

typedef struct __attribute__((aligned(32))) {
    uint32_t ti;
    uint32_t source_ad;
    uint32_t dest_ad;
    uint32_t txfr_len;
    uint32_t stride;
    uint32_t nextconbk;
    uint32_t reserved[2];
} dma_cb_t;

enum {
    TI_INTEN      = 1 << 0,
    TI_WAIT_RESP  = 1 << 3,
    TI_DEST_DREQ  = 1 << 6,
    TI_SRC_INC    = 1 << 8,
    TI_PERMAP_PWM = 5 << 16,
};

enum {
    CS_ACTIVE = 1 << 0,
    CS_END    = 1 << 1,
    CS_INT    = 1 << 2,
    CS_RESET  = 1 << 31,
};

#define BUS_L2CACHED(x) ((uint32_t)(x) | 0x40000000)
#define BUS_PERIPH(x)   ((uint32_t)(x) - 0x20000000 + 0x7E000000)

// IRQ controller registers
enum {
    IRQ_Base          = 0x2000b200,
    IRQ_pending_1     = IRQ_Base + 0x04,
    IRQ_Enable_1      = IRQ_Base + 0x10,
    IRQ_Disable_1     = IRQ_Base + 0x1c,
    IRQ_Disable_2     = IRQ_Base + 0x20,
    IRQ_Disable_Basic = IRQ_Base + 0x24,
};

// DMA channel 5 = GPU IRQ 21 => bit 21 of IRQ_pending_1
#define DMA_CH_IRQ_BIT  (1 << (16 + DMA_CHANNEL))

// Double-buffer state
//
// PWM channel 1 serializer outputs PDM on GPIO 18 at 2.822 MHz.
// Each buffer = 16 PCM samples × 64 oversample = 1024 PDM bits
//             = 32 words of 32 bits, DMA'd to PWM FIFO.
// One DMA CB per buffer. DMA ping-pongs between two CBs.

// PWM clock = 2.822 MHz, RNG = 32 → DREQ every 32 bits.
// Buffer duration = 1024 / 2.822 MHz = 362.9 µs.
// ISR rate = 44100 / 16 = 2756 Hz.

static uint32_t pdm_buf[2][WORDS_PER_BUF];
static dma_cb_t cbs[2] __attribute__((aligned(32)));

// Which buffer DMA is currently playing (0 or 1).
static volatile unsigned dma_playing = 0;

// First-order sigma-delta state
static volatile int sd_integrator = 0;
static volatile int sd_feedback   = 0;

// Waveform buffer — one period, read one sample per ISR.
#define MAX_PERIOD 256
static const short *pcm_ptr;
static unsigned pcm_len;
static volatile unsigned pcm_pos;

void fast_interrupt_vector(unsigned pc) {
    panic("unexpected fast interrupt: pc=%x\n", pc);
}
void syscall_vector(unsigned pc) {
    panic("unexpected syscall: pc=%x\n", pc);
}
void reset_vector(unsigned pc) {
    panic("unexpected reset: pc=%x\n", pc);
}
void undefined_instruction_vector(unsigned pc) {
    panic("unexpected undef-inst: pc=%x\n", pc);
}
void prefetch_abort_vector(unsigned pc) {
    panic("unexpected prefetch abort: pc=%x\n", pc);
}
void data_abort_vector(unsigned pc) {
    panic("unexpected data abort: pc=%x\n", pc);
}

// DMA completion ISR — fires at 2756 Hz (every 16 samples)

// Sigma-delta modulates 16 PCM samples into 32 packed words
// (1024 PDM bits) for the just-completed (free) buffer.
// Per-sample wraparound: works with any waveform length.
void interrupt_vector(unsigned pc) {
    gpio_set_on(DEBUG_PIN);
    dev_barrier();

    if (GET32(IRQ_pending_1) & DMA_CH_IRQ_BIT) {
        // Clear DMA interrupt, keep DMA active
        PUT32(DMA_CS(DMA_CHANNEL), CS_ACTIVE | CS_INT);
        dev_barrier();

        unsigned done = dma_playing;
        dma_playing ^= 1;

        uint32_t *buf = pdm_buf[done];
        int integ = sd_integrator;
        int fb    = sd_feedback;
        unsigned pos = pcm_pos;
        unsigned len = pcm_len;
        const short *ptr = pcm_ptr;
        unsigned word_idx = 0;
        uint32_t word = 0;
        unsigned bit_in_word = 0;

        for (unsigned s = 0; s < SAMPLES_PER_BUF; s++) {
            int sample = ptr[pos];
            pos++;
            if (pos >= len)
                pos = 0;

            for (unsigned i = 0; i < OVERSAMPLE; i++) {
                integ += sample - fb;
                if (integ >= 0) {
                    fb = 32767;
                    word |= (1u << (31 - bit_in_word));
                } else {
                    fb = -32768;
                }
                bit_in_word++;
                if (bit_in_word == 32) {
                    buf[word_idx++] = word;
                    word = 0;
                    bit_in_word = 0;
                }
            }
        }

        pcm_pos = pos;
        sd_integrator = integ;
        sd_feedback   = fb;
    }

    gpio_set_off(DEBUG_PIN);
    dev_barrier();
}

// Build DMA CB for one buffer (called once at init)

// Single CB per buffer: DMA transfers 32 words (128 bytes)
// from pdm_buf[buf] to PWM_FIF1 with SRC_INC + DREQ pacing.
// CB[0].next → CB[1], CB[1].next → CB[0] (ping-pong).
static void build_cb(unsigned buf_idx) {
    unsigned other = 1 - buf_idx;
    dma_cb_t *cb = &cbs[buf_idx];

    cb->ti        = TI_SRC_INC | TI_DEST_DREQ | TI_PERMAP_PWM
                  | TI_WAIT_RESP | TI_INTEN;
    cb->source_ad = BUS_L2CACHED(pdm_buf[buf_idx]);
    cb->dest_ad   = BUS_PERIPH(PWM_FIF1);
    cb->txfr_len  = WORDS_PER_BUF * 4;
    cb->stride    = 0;
    cb->nextconbk = BUS_L2CACHED(&cbs[other]);
    cb->reserved[0] = 0;
    cb->reserved[1] = 0;
}

// initialize the interrupts
static void interrupt_init(void) {
    disable_interrupts();

    PUT32(IRQ_Disable_1, 0xffffffff);
    PUT32(IRQ_Disable_2, 0xffffffff);
    PUT32(IRQ_Disable_Basic, 0xffffffff);
    dev_barrier();

    extern uint32_t _interrupt_table[];
    extern uint32_t _interrupt_table_end[];
    uint32_t *dst = (void *)0;
    uint32_t *src = _interrupt_table;
    unsigned n = _interrupt_table_end - src;

    gcc_mb();
    for (unsigned i = 0; i < n; i++)
        dst[i] = src[i];
    gcc_mb();

    PUT32(IRQ_Enable_1, DMA_CH_IRQ_BIT);
    dev_barrier();
}

// PWM clock + init

// Clock = PLLD (500 MHz) / 177.16 ≈ 2.822 MHz 
// Each FIFO word = 32 bits shifted out at 2.822 MHz.
// DREQ fires every 32 clocks = 11.34 µs per word.

static void pwm_clock_init(void) {
    PUT32(PWM_CTL, 0);
    delay_us(110);

    PUT32(CM_PWMCTL, CM_PASSWD | CM_KILL);
    delay_us(110);
    while (GET32(CM_PWMCTL) & CM_BUSY)
        ;

    PUT32(CM_PWMDIV, CM_PASSWD | (177 << 12) | 656);
    delay_us(110);

    PUT32(CM_PWMCTL, CM_PASSWD | CM_MASH1 | CM_SRC_PLLD | CM_ENAB);
    delay_us(110);
    while (!(GET32(CM_PWMCTL) & CM_BUSY))
        ;
}

static void pwm_init(void) {
    // Set GPIO 18 to ALT5 (PWM0 / channel 1 output)
    gpio_set_function(GPIO_PIN_OUT, GPIO_FUNC_ALT5);
    delay_us(10);

    pwm_clock_init();

    // RNG = 32: serialize all 32 bits per FIFO word at 11.289 MHz
    PUT32(PWM_RNG1, 32);
    delay_us(10);

    // Clear status and FIFO
    PUT32(PWM_STA, 0x1FFF);
    delay_us(10);

    PUT32(PWM_CTL, CTL_PWEN1 | CTL_MODE1 | CTL_USEF1);
    delay_us(100);
    PUT32(PWM_CTL, CTL_CLRF);
    delay_us(10);
    PUT32(PWM_CTL, CTL_PWEN1 | CTL_MODE1 | CTL_USEF1);
    delay_us(100);

    // Enable DMA: DREQ threshold PANIC=7, DREQ=1
    PUT32(PWM_DMAC, (1u << 31) | (7 << 8) | 1);
    delay_us(10);
}

// Fill one buffer (used for pre-fill before DMA starts)
static void prefill_buf(unsigned buf_idx) {
    uint32_t *buf = pdm_buf[buf_idx];
    unsigned word_idx = 0;
    uint32_t word = 0;
    unsigned bit_in_word = 0;

    for (unsigned s = 0; s < SAMPLES_PER_BUF; s++) {
        int sample = pcm_ptr[pcm_pos];
        pcm_pos++;
        if (pcm_pos >= pcm_len)
            pcm_pos = 0;

        for (unsigned i = 0; i < OVERSAMPLE; i++) {
            sd_integrator += sample - sd_feedback;
            if (sd_integrator >= 0) {
                sd_feedback = 32767;
                word |= (1u << (31 - bit_in_word));
            } else {
                sd_feedback = -32768;
            }
            bit_in_word++;
            if (bit_in_word == 32) {
                buf[word_idx++] = word;
                word = 0;
                bit_in_word = 0;
            }
        }
    }
}

void audio_init(void) {
    gpio_set_output(DEBUG_PIN);
    pwm_init();

    PUT32(DMA_ENABLE, GET32(DMA_ENABLE) | (1 << DMA_CHANNEL));
    delay_us(10);

    build_cb(0);
    build_cb(1);

    interrupt_init();
}

void play_tone(const short *samples, unsigned n_samples, unsigned duration_us) {
    sd_integrator = 0;
    sd_feedback   = 0;

    pcm_ptr = samples;
    pcm_len = n_samples;
    pcm_pos = 0;

    prefill_buf(0);
    prefill_buf(1);

    dma_playing = 0;

    // Clear FIFO before starting
    PUT32(PWM_CTL, CTL_CLRF);
    delay_us(10);
    PUT32(PWM_CTL, CTL_PWEN1 | CTL_MODE1 | CTL_USEF1);
    delay_us(10);

    PUT32(DMA_CS(DMA_CHANNEL), CS_RESET);
    delay_us(2);
    PUT32(DMA_CS(DMA_CHANNEL), CS_END | CS_INT);
    PUT32(DMA_CONBLK(DMA_CHANNEL), BUS_L2CACHED(&cbs[0]));
    dev_barrier();

    enable_interrupts();
    PUT32(DMA_CS(DMA_CHANNEL), CS_ACTIVE);

    delay_us(duration_us);

    disable_interrupts();
    PUT32(DMA_CS(DMA_CHANNEL), CS_RESET);
}

void rest(unsigned us) {
    PUT32(DMA_CS(DMA_CHANNEL), CS_RESET);
    delay_us(us);
}
