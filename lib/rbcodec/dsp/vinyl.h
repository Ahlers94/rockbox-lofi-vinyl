#ifndef _VINYL_H
#define _VINYL_H
/* Compact ranges for a one-way scrolling quick-menu -- every step is
 * meant to be reachable and distinct. The underlying DSP math in
 * vinyl.c is UNCHANGED from the original 0-100 version; each knob
 * value here is rescaled back up to that same 0-100 domain before the
 * original formulas run, so the sound at any given knob position
 * matches the same proportional position on the old 0-100 scale. */
#define VINYL_CRACKLE_MIN     0
#define VINYL_CRACKLE_MAX     16

#define VINYL_COMPRESSION_MIN 0
#define VINYL_COMPRESSION_MAX 10

#define VINYL_FLUTTER_MIN     0
#define VINYL_FLUTTER_MAX     10

void dsp_set_vinyl_crackle(int amount);      /* Knob 2 */
void dsp_set_vinyl_compression(int amount);  /* Knob 1 */
void dsp_set_vinyl_flutter(int amount);      /* Knob 3 */

/* Mode switch -- changes what Crackle, Compression, and Flutter compute. */
#define VINYL_MODE_VINYL 0
#define VINYL_MODE_TAPE  1
void dsp_set_vinyl_mode(int mode);

#endif /* _VINYL_H */
