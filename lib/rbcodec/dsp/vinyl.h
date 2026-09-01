#ifndef _VINYL_H
#define _VINYL_H
/* Each knob is 0 (off) plus a small number of hand-tuned intensity
 * steps, sized so every step on a one-way scrolling quick-menu is
 * audibly distinct -- no dead clicks. Ranges are per-knob now rather
 * than a single shared 0-100 "amount" scale, since each effect's
 * underlying math supports a different number of meaningful steps. */
#define VINYL_CRACKLE_MIN     0
#define VINYL_CRACKLE_MAX     16   /* 0=off, 1-16 = table index 0-15 */

#define VINYL_COMPRESSION_MIN 0
#define VINYL_COMPRESSION_MAX 10   /* 0=off, 1-10 = table index 0-9 */

#define VINYL_FLUTTER_MIN     0
#define VINYL_FLUTTER_MAX     10   /* 0=off, 1-10 = table index 0-9 */

void dsp_set_vinyl_crackle(int amount);      /* Knob 2 */
void dsp_set_vinyl_compression(int amount);  /* Knob 1 */
void dsp_set_vinyl_flutter(int amount);      /* Knob 3 */

/* Mode switch -- changes what Crackle, Compression, and Flutter compute. */
#define VINYL_MODE_VINYL 0
#define VINYL_MODE_TAPE  1
void dsp_set_vinyl_mode(int mode);

#endif /* _VINYL_H */
