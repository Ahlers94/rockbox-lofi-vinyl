#include "rbcodecconfig.h"
#include "dsp_misc.h"
#include "dsp_proc_entry.h"
#include "dsp_filter.h"
#include "lofi.h"
#include <string.h>

static int lofi_bitdepth   = LOFI_BITDEPTH_MAX;
static int lofi_downsample = LOFI_DOWNSAMPLE_MIN;

static int32_t held_sample[2];
static int     hold_counter = 0;

static int shift_amount      = 0;
static int current_frac_bits = 0;

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
    if (hold_counter > lofi_downsample)
        hold_counter = lofi_downsample;
    dsp_proc_enable(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_LOFI, true);
    dsp_proc_activate(dsp_get_config(CODEC_IDX_AUDIO), DSP_PROC_LOFI, true);
}

static void lofi_flush(void)
{
    hold_counter   = 0;
    held_sample[0] = 0;
    held_sample[1] = 0;
}

static void lofi_process(struct dsp_proc_entry *this, struct dsp_buffer **buf_p)
{
    struct dsp_buffer *buf = *buf_p;
    int channels = buf->format.num_channels;
    int count    = buf->remcount;

    for (int ch = 0; ch < channels; ch++)
    {
        int32_t *data = buf->p32[ch];

        for (int i = 0; i < count; i++)
        {
            if (hold_counter <= 0)
            {
                held_sample[ch] = data[i];
                hold_counter = lofi_downsample - 1;
            }
            else
            {
                data[i] = held_sample[ch];
                hold_counter--;
            }

            if (shift_amount > 0)
                data[i] = (data[i] >> shift_amount) << shift_amount;
        }
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
            retval = PROC_NEW_FORMAT_OK;
            break;
        }
    }
    return retval;
}

DSP_PROC_DB_ENTRY(LOFI, lofi_configure);
