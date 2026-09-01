#include "rbcodecconfig.h"
#include "dsp_misc.h"
#include "dsp_proc_entry.h"
#include "dsp_filter.h"
#include "lofi.h"
#include <string.h>

static int lofi_bitdepth   = LOFI_BITDEPTH_MAX;
static int lofi_downsample = LOFI_DOWNSAMPLE_MIN;

static int32_t held_sample[2];
static int     hold_counter[2] = {0, 0};   /* per-channel */

static int shift_amount      = 0;
static int current_frac_bits = 0;

/* ---------------------------------------------------------------------
 * Asymmetric soft clip (input-stage saturation emulation)
 *
 * Real analog front ends don't clip the positive and negative halves of
 * a waveform identically -- op-amp bias and transistor asymmetry mean
 * one direction saturates a bit harder than the other. Modeled here as
 * two independent soft-knee thresholds/ratios rather than a DC offset,
 * so the average stays at zero (no DC injected into the signal) while
 * the *character* of positive vs. negative squashing differs.
 *
 * Thresholds are derived from frac_bits so they scale with the sample
 * format rather than being hardcoded to one bit width. Starting points
 * only -- nudge PCT/SHIFT values by ear.
 * ------------------------------------------------------------------- */
static int32_t clip_pos_threshold = 0;
static int32_t clip_neg_threshold = 0;

#define CLIP_POS_KNEE_SHIFT 2   /* harder knee on positive excursions */
#define CLIP_NEG_KNEE_SHIFT 4   /* softer knee on negative excursions */

static void recompute_clip_thresholds(void)
{
    /* Same "shift relative to frac_bits" idiom vinyl.c uses for its
     * compression threshold, so full_scale tracks the current sample
     * format instead of assuming a fixed bit width. */
    int base_shift = (38 - current_frac_bits) + 3;
    if (base_shift < 2)  base_shift = 2;
    if (base_shift > 30) base_shift = 30;
    int32_t full_scale = (int32_t)1 << (32 - base_shift);

    clip_pos_threshold = (full_scale * 3)  >> 2;   /* ~75% */
    clip_neg_threshold = (full_scale * 13) >> 4;   /* ~81% -- clips a bit later */
}

static inline int32_t asym_soft_clip(int32_t x)
{
    if (x >= 0)
    {
        if (x > clip_pos_threshold)
        {
            int32_t excess = x - clip_pos_threshold;
            x = clip_pos_threshold + (excess >> CLIP_POS_KNEE_SHIFT);
        }
    }
    else
    {
        int32_t mag = -x;
        if (mag > clip_neg_threshold)
        {
            int32_t excess = mag - clip_neg_threshold;
            mag = clip_neg_threshold + (excess >> CLIP_NEG_KNEE_SHIFT);
        }
        x = -mag;
    }
    return x;
}

/* ---------------------------------------------------------------------
 * Quantization-step-scaled dither noise (converter noise-floor emulation)
 *
 * Own independent PRNG/seed -- deliberately NOT shared with vinyl.c's
 * noise_state, so the two noise sources don't correlate (shared state
 * would make ADC-noise and vinyl-hiss share sign flips/timing and sound
 * like one noise source with a strange envelope instead of two
 * independent ones).
 *
 * Applied only at capture time, before truncation, sized to exactly one
 * quantization step (1 << shift_amount) so it dithers the truncation
 * itself and scales automatically with bit depth -- lower bitdepth
 * settings get proportionally coarser (louder) noise, matching how
 * real converter noise floor tracks quantization step size.
 * ------------------------------------------------------------------- */
static uint32_t lofi_noise_state = 0x9E3779B9;

static inline uint32_t next_lofi_noise(void)
{
    uint32_t x = lofi_noise_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    lofi_noise_state = x;
    return x;
}

static void recompute_shift(void)
{
    int reduction = current_frac_bits - lofi_bitdepth;
    shift_amount = (reduction < 0) ? 0 : reduction;
}

void dsp_set_lofi_bitdepth(int bitdepth)
{
    if (bitdepth < LOFI_BITDEPTH_MIN) bitdepth = LOFI_BITDEPTH_MIN;
    if (bitdepth > LOFI_BITDEPTH_MAX) bitdepth = LOFI_BITDEPTH_MAX;
    lofi_bitdepth = bitdepth;
    recompute_shift();
    dsp_proc_enable(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_LOFI, true);
    dsp_proc_activate(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_LOFI, true);
}

void dsp_set_lofi_downsample(int factor)
{
    if (factor < LOFI_DOWNSAMPLE_MIN) factor = LOFI_DOWNSAMPLE_MIN;
    if (factor > LOFI_DOWNSAMPLE_MAX) factor = LOFI_DOWNSAMPLE_MAX;
    lofi_downsample = factor;
    /* Clamp both channels' counters so a shrinking factor can't leave
     * a channel holding for longer than the new period allows. */
    for (int ch = 0; ch < 2; ch++)
    {
        if (hold_counter[ch] > lofi_downsample)
            hold_counter[ch] = lofi_downsample;
    }
    dsp_proc_enable(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_LOFI, true);
    dsp_proc_activate(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_LOFI, true);
}

/* ---------------------------------------------------------------------
 * Reconstruction / anti-aliasing filter
 *
 * Sample-and-hold downsampling produces a "staircase" signal full of
 * high-frequency images above the new, lower Nyquist limit -- real
 * hardware relied on an analog output filter to smooth this away
 * instead of letting it ring out as harsh digital aliasing. Modeled
 * here as two cascaded one-pole lowpass stages per channel (a cheap
 * ~12dB/oct approximation of a real reconstruction filter), run once
 * per buffer after the sample-and-hold/clip/dither/truncate pass.
 *
 * Cutoff tracks lofi_downsample directly: a bigger hold period means a
 * lower new Nyquist limit, so the filter needs to close down further.
 * At downsample=1 (no downsampling happening) the filter is bypassed
 * entirely -- there's no new aliasing to clean up.
 *
 * Table is indexed by (lofi_downsample - 1); shift 0 means bypass for
 * that stage. Values are starting points -- larger shift = darker/more
 * filtered, tune by ear.
 * ------------------------------------------------------------------- */
static const int recon_shift_table[16] =
    { 0, 1, 2, 2, 3, 3, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6 };

static int32_t recon_lp1[2] = {0, 0};
static int32_t recon_lp2[2] = {0, 0};

static void reconstruction_filter_apply(int32_t **data, int channels, int count)
{
    int idx = lofi_downsample - 1;
    if (idx < 0) idx = 0;
    if (idx > 15) idx = 15;
    int shift = recon_shift_table[idx];

    if (shift <= 0)
        return;  /* bypass -- downsample=1, nothing new to filter out */

    for (int ch = 0; ch < channels; ch++)
    {
        for (int i = 0; i < count; i++)
        {
            recon_lp1[ch] += (data[ch][i]  - recon_lp1[ch]) >> shift;
            recon_lp2[ch] += (recon_lp1[ch] - recon_lp2[ch]) >> shift;
            data[ch][i] = recon_lp2[ch];
        }
    }
}

static void lofi_flush(void)
{
    hold_counter[0] = 0;
    hold_counter[1] = 0;
    held_sample[0]  = 0;
    held_sample[1]  = 0;
    recon_lp1[0] = 0;
    recon_lp1[1] = 0;
    recon_lp2[0] = 0;
    recon_lp2[1] = 0;
    lofi_noise_state = 0x9E3779B9;
}

/* Samples-outer / channels-inner, matching vinyl.c's structure, so that
 * both channels advance through the same sample tick together. Each
 * channel keeps its own hold_counter, so L and R sample-and-hold on
 * identical schedules -- no inter-channel drift/smear.
 *
 * Clip -> dither -> truncate all happen only at capture time (once per
 * newly-sampled value), not on repeated held samples -- re-processing a
 * held value on every tick would make it flicker during its hold
 * period, which defeats the point of sample-and-hold. The held branch
 * just replays the already-processed value untouched. */
static void lofi_process(struct dsp_proc_entry *this, struct dsp_buffer **buf_p)
{
    struct dsp_buffer *buf = *buf_p;
    int channels = buf->format.num_channels;
    int count    = buf->remcount;

    for (int i = 0; i < count; i++)
    {
        for (int ch = 0; ch < channels; ch++)
        {
            int32_t *data = buf->p32[ch];
            if (hold_counter[ch] <= 0)
            {
                int32_t s = data[i];

                s = asym_soft_clip(s);

                if (shift_amount > 0)
                {
                    /* Dither sized to exactly one quantization step,
                     * zero-mean over [-half step, +half step). */
                    int32_t step = (int32_t)1 << shift_amount;
                    int32_t dither = (int32_t)(next_lofi_noise() & (uint32_t)(step - 1)) - (step >> 1);
                    s += dither;
                    s = (s >> shift_amount) << shift_amount;
                }

                held_sample[ch] = s;
                data[i] = s;
                hold_counter[ch] = lofi_downsample - 1;
            }
            else
            {
                data[i] = held_sample[ch];
                hold_counter[ch]--;
            }
        }
    }

    if (channels >= 1)
    {
        int32_t *chan_ptrs[2];
        chan_ptrs[0] = buf->p32[0];
        if (channels > 1)
            chan_ptrs[1] = buf->p32[1];
        reconstruction_filter_apply(chan_ptrs, channels, count);
    }

    (void)this;
}

static intptr_t lofi_configure(struct dsp_proc_entry *this,
                                struct dsp_config *dsp,
                                unsigned int setting,
                                intptr_t value)
{
    intptr_t retval = 0;
    (void)dsp;
    switch (setting)
    {
        case DSP_PROC_INIT:
            this->process = lofi_process;
            lofi_flush();
            break;
        case DSP_PROC_CLOSE:
            lofi_flush();
            break;
        case DSP_FLUSH:
            lofi_flush();
            break;
        case DSP_PROC_NEW_FORMAT:
        {
            struct sample_format *format = (struct sample_format *)value;
            current_frac_bits = format->frac_bits;
            recompute_shift();
            recompute_clip_thresholds();
            retval = PROC_NEW_FORMAT_OK;
            break;
        }
    }
    return retval;
}

DSP_PROC_DB_ENTRY(LOFI, lofi_configure);
