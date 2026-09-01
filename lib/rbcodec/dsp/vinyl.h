#ifndef _VINYL_H
#define _VINYL_H

/* All three knobs use a common 0-100 "amount" scale, matching the
 * SP-505's single-knob-per-parameter feel. 0 = off, 100 = maximum. */
#define VINYL_KNOB_MIN 0
#define VINYL_KNOB_MAX 100

void dsp_set_vinyl_crackle(int amount);      /* Knob 2 - implemented */
void dsp_set_vinyl_compression(int amount);  /* Knob 1 - stub, phase 2 */
void dsp_set_vinyl_flutter(int amount);      /* Knob 3 - stub, phase 3 */

/* Mode switch -- changes what Crackle and Compression actually compute. */
#define VINYL_MODE_VINYL 0
#define VINYL_MODE_TAPE  1
void dsp_set_vinyl_mode(int mode);

#endif /* _VINYL_H */
