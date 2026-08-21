#ifndef _LOFI_H
#define _LOFI_H

#define LOFI_BITDEPTH_MIN   4
#define LOFI_BITDEPTH_MAX   16
#define LOFI_DOWNSAMPLE_MIN 1
#define LOFI_DOWNSAMPLE_MAX 16

void dsp_set_lofi_bitdepth(int bitdepth);
void dsp_set_lofi_downsample(int factor);

#endif /* _LOFI_H */
