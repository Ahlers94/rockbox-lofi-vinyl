#include "rbcodecconfig.h"
#include "dsp_misc.h"
#include "dsp_proc_entry.h"
#include "dsp_filter.h"
#include "vinyl.h"
#include <string.h>

static int vinyl_crackle     = 0;
static int vinyl_compression = 0;
static int vinyl_flutter     = 0;
static int vinyl_mode        = 0;  /* 0=Vinyl, 1=Tape */

static int current_frac_bits = 0;
static int current_frequency = 44100;

static uint32_t noise_state = 0x12345678;

static inline uint32_t next_noise(void)
{
    uint32_t x = noise_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    noise_state = x;
    return x;
}

static int32_t lp_state[2] = {0, 0};
static int32_t pop_amp  = 0;
static int     pop_wait = 0;
static int32_t comp_envelope = 0;  /* Vinyl mode: slow loudness tracker, S.frac_bits */

#define FLUTTER_BUF_SIZE 512
static int32_t flutter_buf[2][FLUTTER_BUF_SIZE];
static int     flutter_write_pos = 0;
static uint32_t flutter_phase = 0;      /* LFO phase accumulator */
static uint32_t flutter_jitter_state = 0xA5A5A5A5;

/* ---------------------------------------------------------------------
 * Lookup tables, one entry per quick-menu step (index = knob - 1).
 * Hand-authored rather than derived by /100 division so every step is
 * guaranteed to change the sound -- no repeated/dead positions. Values
 * are starting points tuned by ear against S950/MPC60-style character;
 * nudge freely once you've listened.
 * ------------------------------------------------------------------- */

/* Crackle (1-16): three sub-effects driven off one knob. */

/* Subtracted from the frac_bits-derived base hiss shift -- larger value
 * here = louder hiss. Extended to 0-15 (was implicitly capped at 11). */
static const int crackle_hiss_term[16] =
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

/* Added to the base lowpass shift of 2 -- larger value = darker/duller
 * hiss color. Extended to 0-7 (was capped at 4). */
static const int crackle_lp_term[16] =
    { 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7 };

/* Average gap between pops, in ms. Directly authored instead of run
 * through a formula that plateaued at 75/100 -- top step is now
 * genuinely dense crackle instead of the old near-static ~2-state
 * range. */
static const int crackle_pop_gap_ms[16] =
    { 700, 600, 500, 420, 350, 290, 240, 200,
      160, 130, 100,  75,  55,  40,  25,  15 };

/* Subtracted (on top of the existing -2) from hiss_shift to get pop
 * amplitude -- larger value = louder pops at high settings. */
static const int crackle_pop_extra_term[16] =
    { 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8 };

/* Compression (1-10): ratio shift and how far the threshold drops as
 * the knob rises. Ceiling widened from the old fixed 0-4 to 0-8 so max
 * setting is a genuinely aggressive, obviously-pumping squash rather
 * than topping out at mild glue. */
static const int comp_ratio_table[10] =
    { 0, 1, 1, 2, 2, 3, 4, 5, 6, 8 };

/* Added to comp_thresh_shift -- raises the shift, which lowers the
 * effective threshold, so higher settings catch more of the signal
 * instead of the threshold sitting static regardless of knob position. */
static const int comp_thresh_extra_table[10] =
    { 0, 0, 0, 1, 1, 1, 2, 2, 3, 3 };

/* Flutter (1-10): peak depth per mode, separate tables so tape gets its
 * own full 10-step spread instead of being squeezed by a shared
 * multiplier into fewer real states than vinyl mode. */
static const int flutter_depth_vinyl[10] =
    {  4,  6,  8, 10, 12, 14, 17, 20, 24, 28 };
static const int flutter_depth_tape[10] =
    {  2,  3,  4,  6,  8, 10, 13, 16, 20, 24 };

void dsp_set_vinyl_crackle(int amount)
{
    if (amount < VINYL_CRACKLE_MIN) amount = VINYL_CRACKLE_MIN;
    if (amount > VINYL_CRACKLE_MAX) amount = VINYL_CRACKLE_MAX;
    vinyl_crackle = amount;
    dsp_proc_enable(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_VINYL, true);
    dsp_proc_activate(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_VINYL, true);
}

void dsp_set_vinyl_compression(int amount)
{
    if (amount < VINYL_COMPRESSION_MIN) amount = VINYL_COMPRESSION_MIN;
    if (amount > VINYL_COMPRESSION_MAX) amount = VINYL_COMPRESSION_MAX;
    vinyl_compression = amount;
    dsp_proc_enable(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_VINYL, true);
    dsp_proc_activate(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_VINYL, true);
}

void dsp_set_vinyl_flutter(int amount)
{
    if (amount < VINYL_FLUTTER_MIN) amount = VINYL_FLUTTER_MIN;
    if (amount > VINYL_FLUTTER_MAX) amount = VINYL_FLUTTER_MAX;
    vinyl_flutter = amount;
    dsp_proc_enable(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_VINYL, true);
    dsp_proc_activate(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_VINYL, true);
}

void dsp_set_vinyl_mode(int mode)
{
    if (mode != VINYL_MODE_VINYL && mode != VINYL_MODE_TAPE)
        mode = VINYL_MODE_VINYL;
    vinyl_mode = mode;
    dsp_proc_enable(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_VINYL, true);
    dsp_proc_activate(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_VINYL, true);
}

static void vinyl_flush(void)
{
    noise_state = 0x12345678;
    lp_state[0] = 0;
    lp_state[1] = 0;
    pop_amp     = 0;
    pop_wait    = 0;
    comp_envelope = 0;
    memset(flutter_buf, 0, sizeof(flutter_buf));
    flutter_write_pos = 0;
    flutter_phase = 0;
}

/* ---- Flutter/Warble (Knob 3) ---------------------------------------------
 * Real technique: write incoming audio into a small circular buffer, then
 * read back from a position that drifts slightly around "now" driven by a
 * slow LFO. The drift is what produces pitch wobble -- same principle as a
 * chorus/vibrato effect. Linear interpolation between the two nearest
 * buffered samples avoids stepped/zippery artifacts from the fractional
 * read position.
 *
 * Mode-specific tuning grounded in real causes:
 *   Vinyl: slow, regular wobble near 0.556 Hz (33 1/3 RPM once-per-
 *          rotation warp/off-center wow).
 *   Tape:  faster (~5 Hz) and less regular -- transport wear/slip rather
 *          than a rotational lock, so a touch of randomized jitter is
 *          added to the LFO itself. */
static int32_t flutter_apply(int ch, int32_t input_sample,
                              uint32_t lfo_inc, int32_t depth_q8)
{
    flutter_buf[ch][flutter_write_pos] = input_sample;

    flutter_phase += lfo_inc;

    /* Top 16 bits of the 32-bit phase = a 0..65535 sawtooth, one full
     * ramp per LFO cycle. Fold that into a triangle wave. */
    uint32_t saw = flutter_phase >> 16;
    uint32_t tri = (saw < 32768) ? (saw * 2) : ((65535 - saw) * 2);

    int32_t jitter_q8 = 0;
    if (vinyl_mode == VINYL_MODE_TAPE)
    {
        uint32_t x = flutter_jitter_state;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        flutter_jitter_state = x;
        jitter_q8 = ((int32_t)(x % 5) - 2) << 8;  /* -2..+2 samples, Q8 */
    }

    /* Offset stays in Q8 fixed point through the divide so the
     * fractional part survives for real interpolation, instead of
     * being truncated away before we ever get to use it. */
    int32_t offset_q8 = (int32_t)(((int64_t)tri * depth_q8) / 65536) + jitter_q8;

    int offset_int = offset_q8 >> 8;
    if (offset_int < 1) offset_int = 1;  /* never read stale wraparound audio */
    int32_t frac    = offset_q8 & 0xFF;

    int read_pos = flutter_write_pos - offset_int;
    while (read_pos < 0) read_pos += FLUTTER_BUF_SIZE;
    int read_pos_next = (read_pos + 1) % FLUTTER_BUF_SIZE;

    int32_t a = flutter_buf[ch][read_pos];
    int32_t b = flutter_buf[ch][read_pos_next];

    int32_t result = a + (((b - a) * frac) >> 8);

    return result;
}
static void vinyl_process(struct dsp_proc_entry *this, struct dsp_buffer **buf_p)
{
    struct dsp_buffer *buf = *buf_p;
    int channels = buf->format.num_channels;
    int count    = buf->remcount;

    if (vinyl_crackle <= 0 && vinyl_compression <= 0 && vinyl_flutter <= 0)
        return;

    int32_t *data[2];
    data[0] = buf->p32[0];
    if (channels > 1)
        data[1] = buf->p32[1];

    int hiss_shift, lp_shift, pop_shift;
    int avg_gap_samples = 0;
    uint32_t pop_threshold = 0;

    if (vinyl_crackle > 0)
    {
        int idx = vinyl_crackle - 1;  /* 0-15 */

        int base_hiss = (38 - current_frac_bits) + 9;
        hiss_shift = base_hiss - crackle_hiss_term[idx];
        if (hiss_shift < 3)  hiss_shift = 3;
        if (hiss_shift > 30) hiss_shift = 30;

        lp_shift = 2 + crackle_lp_term[idx];
        if (lp_shift < 2) lp_shift = 2;
        if (lp_shift > 9) lp_shift = 9;

        int avg_gap_ms = crackle_pop_gap_ms[idx];
        avg_gap_samples = (avg_gap_ms * current_frequency) / 1000;
        if (avg_gap_samples < 32) avg_gap_samples = 32;
        pop_threshold = 65536u / (uint32_t)avg_gap_samples;
        if (pop_threshold < 1) pop_threshold = 1;

        pop_shift = hiss_shift - 2 - crackle_pop_extra_term[idx];
        if (pop_shift < 1) pop_shift = 1;
    }
    else
    {
        /* Values are unused (loop bodies below are gated on
         * vinyl_crackle > 0) but keep them sane for safety. */
        hiss_shift = 30;
        lp_shift   = 9;
        pop_shift  = 1;
    }

    /* Flutter LFO: proper frequency-based increment, tied to the
     * actual current sample rate, not an arbitrary small integer.
     * Full 32-bit phase wraps exactly once per LFO cycle. */
    uint32_t flutter_freq_mhz;   /* target frequency, milli-Hz */
    int flutter_depth_max = 0;
    if (vinyl_mode == VINYL_MODE_VINYL)
    {
        flutter_freq_mhz = 556;  /* 0.556 Hz -- 33 1/3 RPM once-per-rotation wow */
        if (vinyl_flutter > 0)
            flutter_depth_max = flutter_depth_vinyl[vinyl_flutter - 1];
    }
    else
    {
        flutter_freq_mhz = 5000; /* ~5 Hz -- tape transport flutter */
        if (vinyl_flutter > 0)
            flutter_depth_max = flutter_depth_tape[vinyl_flutter - 1];
    }
    uint32_t flutter_lfo_inc = (uint32_t)(((uint64_t)flutter_freq_mhz << 32) /
                                           ((uint64_t)current_frequency * 1000));
    int32_t flutter_depth_q8 = flutter_depth_max << 8;

    int comp_ratio_shift = 0;
    int32_t comp_threshold = 0;
    if (vinyl_compression > 0)
    {
        int idx = vinyl_compression - 1;  /* 0-9 */
        comp_ratio_shift = comp_ratio_table[idx];

        int comp_thresh_shift = (38 - current_frac_bits) + 3 + comp_thresh_extra_table[idx];
        if (comp_thresh_shift < 2)  comp_thresh_shift = 2;
        if (comp_thresh_shift > 30) comp_thresh_shift = 30;
        comp_threshold = (int32_t)1 << (32 - comp_thresh_shift);
    }

    for (int i = 0; i < count; i++)
    {
        if (vinyl_crackle > 0 && vinyl_mode == VINYL_MODE_VINYL)
        {
            if (pop_wait > 0)
            {
                pop_wait--;
            }
            else if ((next_noise() & 0xFFFF) < pop_threshold)
            {
                int32_t peak = (int32_t)(next_noise() >> pop_shift);
                pop_amp  = (next_noise() & 1) ? peak : -peak;
                pop_wait = avg_gap_samples / 4;
            }
        }

        for (int ch = 0; ch < channels; ch++)
        {
            if (vinyl_crackle > 0)
            {
                int32_t raw = (int32_t)(next_noise() >> hiss_shift);
                if (next_noise() & 1) raw = -raw;

                lp_state[ch] += (raw - lp_state[ch]) >> lp_shift;
            }

            if (vinyl_flutter > 0)
                data[ch][i] = flutter_apply(ch, data[ch][i], flutter_lfo_inc, flutter_depth_q8);

            if (vinyl_crackle > 0)
                data[ch][i] += lp_state[ch] + pop_amp;
        }

        flutter_write_pos = (flutter_write_pos + 1) % FLUTTER_BUF_SIZE;
        if (vinyl_compression > 0)
        {
            if (vinyl_mode == VINYL_MODE_VINYL)
            {
                int32_t peak_mag = data[0][i] < 0 ? -data[0][i] : data[0][i];
                if (channels > 1)
                {
                    int32_t r_mag = data[1][i] < 0 ? -data[1][i] : data[1][i];
                    if (r_mag > peak_mag) peak_mag = r_mag;
                }
                comp_envelope += (peak_mag - comp_envelope) >> (peak_mag > comp_envelope ? 3 : 9);

                if (comp_envelope > comp_threshold)
                {
                    for (int ch = 0; ch < channels; ch++)
                    {
                        int32_t s = data[ch][i];
                        int32_t mag = s < 0 ? -s : s;
                        if (mag > comp_threshold)
                        {
                            int32_t excess = mag - comp_threshold;
                            int32_t squashed = comp_threshold + (excess >> comp_ratio_shift);
                            data[ch][i] = (s < 0) ? -squashed : squashed;
                        }
                    }
                }
            }
            else
            {
                for (int ch = 0; ch < channels; ch++)
                {
                    int32_t s = data[ch][i];
                    int32_t mag = s < 0 ? -s : s;
                    if (mag > comp_threshold)
                    {
                        int32_t excess = mag - comp_threshold;
                        int32_t squashed = comp_threshold + (excess >> comp_ratio_shift);
                        data[ch][i] = (s < 0) ? -squashed : squashed;
                    }
                }
            }
        }

        if (pop_amp != 0)
            pop_amp -= (pop_amp >> 3);
    }
    (void)this;
}

static intptr_t vinyl_configure(struct dsp_proc_entry *this,
                                 struct dsp_config *dsp,
                                 unsigned int setting,
                                 intptr_t value)
{
    intptr_t retval = 0;
    (void)dsp;

    switch (setting)
    {
        case DSP_PROC_INIT:
            this->process = vinyl_process;
            vinyl_flush();
            break;

        case DSP_PROC_CLOSE:
            vinyl_flush();
            break;

        case DSP_FLUSH:
            vinyl_flush();
            break;

        case DSP_PROC_NEW_FORMAT:
        {
            struct sample_format *format = (struct sample_format *)value;
            current_frac_bits = format->frac_bits;
            if (format->frequency > 0)
                current_frequency = format->frequency;
            retval = PROC_NEW_FORMAT_OK;
            break;
        }
    }
    return retval;
}

DSP_PROC_DB_ENTRY(VINYL, vinyl_configure);
