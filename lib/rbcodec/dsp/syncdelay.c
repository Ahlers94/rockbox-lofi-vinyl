#include "rbcodecconfig.h"
#include "dsp_misc.h"
#include "dsp_proc_entry.h"
#include "dsp_filter.h"
#include "syncdelay.h"
#include "kernel.h"    /* current_tick, HZ */
#include "tick.h"    /* current_tick, HZ */
#include <string.h>

/* Max delay buffer length. Sized for a whole note at the slowest
 * supported tempo (40 BPM) at up to 48kHz, rounded up generously.
 * At 40 BPM a quarter note is 1.5s, so a whole note is 6s -- more
 * than any of our divisions will ever need, giving comfortable
 * headroom. 48000 * 6 = 288000 samples/channel. */
#define MAX_DELAY_SAMPLES 288000

static int delay_bpm       = 120;
static int delay_division  = DELAY_DIV_1_8;
static int delay_feedback  = 0;   /* 0-100, knob */
static int delay_mix       = 0;   /* 0-100, knob */

static int current_frequency = 44100;

/* Internal fixed-point (Q8) versions of the knobs, recomputed on set. */
static int32_t feedback_q8 = 0;
static int32_t mix_q8      = 0;

/* Target delay length in samples for the current bpm/division.
 * For swing divisions this holds the "long" length; delay_short_samples
 * holds the paired "short" length. */
static int32_t delay_target_samples = 0;
static int32_t delay_short_samples  = 0;
static int     delay_is_swing       = 0;

/* Per-sample-tick state (shared write pointer, matches vinyl.c's
 * flutter_write_pos pattern -- both channels advance together since
 * the loop is samples-outer / channels-inner). */
static int32_t delay_buf[2][MAX_DELAY_SAMPLES];
static int32_t write_pos = 0;

/* Swing state machine: alternates the *active* delay length between
 * the long and short values every time the current interval elapses.
 * This is an approximation of swing feel on a single-tap delay line --
 * true per-repeat swing would need a multi-tap architecture -- but it
 * gives an audibly alternating long/short repeat cadence. */
static int32_t swing_counter = 0;
static int     swing_phase   = 0;   /* 0 = long interval active, 1 = short */
static int32_t active_delay_samples = 0;

static void recompute_delay_times(void)
{
    if (delay_bpm < DELAY_BPM_MIN) delay_bpm = DELAY_BPM_MIN;
    if (delay_bpm > DELAY_BPM_MAX) delay_bpm = DELAY_BPM_MAX;

    /* Quarter note length in samples, 64-bit intermediate to avoid
     * overflow at low bpm / high sample rate. */
    int32_t q = (int32_t)(((int64_t)60 * current_frequency) / delay_bpm);

    delay_is_swing = 0;

    switch (delay_division)
    {
        case DELAY_DIV_1_4:
            delay_target_samples = q;
            break;
        case DELAY_DIV_1_4_DOT:
            delay_target_samples = q + q / 2;              /* 1.5x */
            break;
        case DELAY_DIV_1_8:
            delay_target_samples = q / 2;
            break;
        case DELAY_DIV_1_8_DOT:
            delay_target_samples = (q * 3) / 4;
            break;
        case DELAY_DIV_1_8_SWING:
            /* Swung eighth pair sums to one quarter note, split ~2:1. */
            delay_is_swing        = 1;
            delay_target_samples  = (q * 2) / 3;   /* long */
            delay_short_samples   = q - delay_target_samples; /* short */
            break;
        case DELAY_DIV_1_16:
            delay_target_samples = q / 4;
            break;
        case DELAY_DIV_1_16_DOT:
            delay_target_samples = (q * 3) / 8;
            break;
        case DELAY_DIV_1_16_SWING:
            delay_is_swing        = 1;
            delay_target_samples  = (q / 2 * 2) / 3;         /* long, pair sums to 1/8 note */
            delay_short_samples   = (q / 2) - delay_target_samples;
            break;
        case DELAY_DIV_1_32:
            delay_target_samples = q / 8;
            break;
        default:
            delay_target_samples = q / 2;
            break;
    }

    if (delay_target_samples < 1) delay_target_samples = 1;
    if (delay_target_samples >= MAX_DELAY_SAMPLES) delay_target_samples = MAX_DELAY_SAMPLES - 1;
    if (delay_is_swing)
    {
        if (delay_short_samples < 1) delay_short_samples = 1;
        if (delay_short_samples >= MAX_DELAY_SAMPLES) delay_short_samples = MAX_DELAY_SAMPLES - 1;
    }

    /* Reset the swing state machine onto the new timing rather than
     * leaving it counting down against a now-stale interval. */
    swing_phase          = 0;
    active_delay_samples = delay_target_samples;
    swing_counter         = active_delay_samples;
}

void dsp_set_delay_bpm(int bpm)
{
    if (bpm < DELAY_BPM_MIN) bpm = DELAY_BPM_MIN;
    if (bpm > DELAY_BPM_MAX) bpm = DELAY_BPM_MAX;
    delay_bpm = bpm;
    recompute_delay_times();
    dsp_proc_enable(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_SYNCDELAY, true);
    dsp_proc_activate(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_SYNCDELAY, true);
}

void dsp_set_delay_division(int division)
{
    if (division < 0 || division >= DELAY_DIV_COUNT)
        division = DELAY_DIV_1_8;
    delay_division = division;
    recompute_delay_times();
    dsp_proc_enable(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_SYNCDELAY, true);
    dsp_proc_activate(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_SYNCDELAY, true);
}

void dsp_set_delay_feedback(int amount)
{
    if (amount < DELAY_KNOB_MIN) amount = DELAY_KNOB_MIN;
    if (amount > DELAY_KNOB_MAX) amount = DELAY_KNOB_MAX;
    delay_feedback = amount;

    /* Cap effective feedback below 100% to avoid runaway buildup /
     * self-oscillation. 92% ceiling gives a long, musical decay
     * without the tail ever growing louder than the input. */
    int capped = (amount > 92) ? 92 : amount;
    feedback_q8 = ((int32_t)capped << 8) / 100;

    dsp_proc_enable(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_SYNCDELAY, true);
    dsp_proc_activate(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_SYNCDELAY, true);
}

void dsp_set_delay_mix(int amount)
{
    if (amount < DELAY_KNOB_MIN) amount = DELAY_KNOB_MIN;
    if (amount > DELAY_KNOB_MAX) amount = DELAY_KNOB_MAX;
    delay_mix = amount;
    mix_q8 = ((int32_t)amount << 8) / 100;

    dsp_proc_enable(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_SYNCDELAY, true);
    dsp_proc_activate(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_SYNCDELAY, true);
}

/* ---- Tap tempo -----------------------------------------------------
 * Averages the last TAP_HISTORY intervals between taps for a stable
 * reading rather than reacting to any single sloppy tap. If more than
 * TAP_TIMEOUT_TICKS pass between taps, the sequence resets -- an old
 * tap from a previous attempt (or the very first tap ever) shouldn't
 * be averaged against a fresh one. */
#define TAP_HISTORY        4
#define TAP_TIMEOUT_TICKS  (2 * HZ)

static long    tap_last_tick = 0;
static int32_t tap_intervals[TAP_HISTORY];
static int     tap_count = 0;   /* valid intervals currently stored */
static int     tap_index = 0;

void dsp_delay_tap_tempo(void)
{
    long now = current_tick;

    if (tap_last_tick != 0)
    {
        long interval = now - tap_last_tick;

        if (interval <= 0 || interval > TAP_TIMEOUT_TICKS)
        {
            /* Gap too long (or tick counter wrapped) -- treat this
             * tap as the start of a brand new sequence instead of
             * letting a stale interval skew the average. */
            tap_count = 0;
            tap_index = 0;
        }
        else
        {
            tap_intervals[tap_index] = (int32_t)interval;
            tap_index = (tap_index + 1) % TAP_HISTORY;
            if (tap_count < TAP_HISTORY)
                tap_count++;

            int32_t sum = 0;
            for (int k = 0; k < tap_count; k++)
                sum += tap_intervals[k];
            int32_t avg_ticks = sum / tap_count;

            if (avg_ticks > 0)
            {
                int bpm = (int)(((int64_t)60 * HZ) / avg_ticks);
                dsp_set_delay_bpm(bpm);   /* clamps + recomputes internally */
            }
        }
    }

    tap_last_tick = now;
}

void dsp_delay_tap_reset(void)
{
    tap_last_tick = 0;
    tap_count = 0;
    tap_index = 0;
}

void dsp_delay_bpm_adjust(int delta)
{
    dsp_set_delay_bpm(delay_bpm + delta);
}

int dsp_get_delay_bpm(void)
{
    return delay_bpm;
}

static void syncdelay_flush(void)
{
    memset(delay_buf, 0, sizeof(delay_buf));
    write_pos     = 0;
    swing_phase   = 0;
    active_delay_samples = delay_target_samples;
    swing_counter = active_delay_samples;
}

static void syncdelay_process(struct dsp_proc_entry *this, struct dsp_buffer **buf_p)
{
    struct dsp_buffer *buf = *buf_p;
    int channels = buf->format.num_channels;
    int count    = buf->remcount;

    if (delay_feedback <= 0 && delay_mix <= 0)
        return;

    int32_t *data[2];
    data[0] = buf->p32[0];
    if (channels > 1)
        data[1] = buf->p32[1];

    for (int i = 0; i < count; i++)
    {
        /* Swing timing: when the current interval elapses, flip
         * between the long and short lengths for the next interval. */
        if (delay_is_swing)
        {
            if (--swing_counter <= 0)
            {
                swing_phase = !swing_phase;
                active_delay_samples = swing_phase ? delay_short_samples
                                                     : delay_target_samples;
                swing_counter = active_delay_samples;
            }
        }
        else
        {
            active_delay_samples = delay_target_samples;
        }

        int read_pos = write_pos - active_delay_samples;
        while (read_pos < 0) read_pos += MAX_DELAY_SAMPLES;

        for (int ch = 0; ch < channels; ch++)
        {
            int32_t dry     = data[ch][i];
            int32_t delayed = delay_buf[ch][read_pos];

            /* Feed dry input + attenuated delayed signal back into the
             * line -- standard feedback-delay topology. */
            int32_t fed_back = dry + (int32_t)(((int64_t)delayed * feedback_q8) >> 8);
            delay_buf[ch][write_pos] = fed_back;

            /* Output: dry always passes through; repeats layered on
             * top scaled by mix, independent of feedback amount. */
            data[ch][i] = dry + (int32_t)(((int64_t)delayed * mix_q8) >> 8);
        }

        write_pos = (write_pos + 1) % MAX_DELAY_SAMPLES;
    }
    (void)this;
}

static intptr_t syncdelay_configure(struct dsp_proc_entry *this,
                                     struct dsp_config *dsp,
                                     unsigned int setting,
                                     intptr_t value)
{
    intptr_t retval = 0;
    (void)dsp;

    switch (setting)
    {
        case DSP_PROC_INIT:
            this->process = syncdelay_process;
            recompute_delay_times();
            syncdelay_flush();
            break;

        case DSP_PROC_CLOSE:
            syncdelay_flush();
            break;

        case DSP_FLUSH:
            syncdelay_flush();
            break;

        case DSP_PROC_NEW_FORMAT:
        {
            struct sample_format *format = (struct sample_format *)value;
            if (format->frequency > 0)
                current_frequency = format->frequency;
            recompute_delay_times();
            retval = PROC_NEW_FORMAT_OK;
            break;
        }
    }
    return retval;
}

DSP_PROC_DB_ENTRY(SYNCDELAY, syncdelay_configure);
