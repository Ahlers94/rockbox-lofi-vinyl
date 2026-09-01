#ifndef SYNCDELAY_H
#define SYNCDELAY_H

/* Note division selectable on the delay line. "Swing" divisions produce
 * an alternating long/short repeat pattern (~2:1 ratio) rather than a
 * single fixed interval. */
enum syncdelay_division {
    DELAY_DIV_1_4 = 0,
    DELAY_DIV_1_4_DOT,
    DELAY_DIV_1_8,
    DELAY_DIV_1_8_DOT,
    DELAY_DIV_1_8_SWING,
    DELAY_DIV_1_16,
    DELAY_DIV_1_16_DOT,
    DELAY_DIV_1_16_SWING,
    DELAY_DIV_1_32,
    DELAY_DIV_COUNT
};

#define DELAY_BPM_MIN      40
#define DELAY_BPM_MAX     300

#define DELAY_KNOB_MIN      0
#define DELAY_KNOB_MAX    100

/* Set the tempo used to compute delay times directly (manual entry /
 * +-/- selector). Clamped to DELAY_BPM_MIN..DELAY_BPM_MAX. */
void dsp_set_delay_bpm(int bpm);

/* Nudge the current BPM by delta (e.g. +1/-1 or +5/-5 from a selector
 * UI). Clamped the same as dsp_set_delay_bpm. */
void dsp_delay_bpm_adjust(int delta);

/* Read back the current BPM, e.g. for a UI readout. */
int dsp_get_delay_bpm(void);

/* Call this once per tap on a "tap tempo" button. Averages the last
 * few tap intervals for a stable reading and updates the delay BPM
 * automatically. If more than ~2 seconds pass between taps, the tap
 * sequence resets so a stale interval doesn't skew the next reading. */
void dsp_delay_tap_tempo(void);

/* Manually reset the tap-tempo sequence (e.g. on a long-press or when
 * switching files/loops) without waiting for the timeout. */
void dsp_delay_tap_reset(void);

/* Select note division (see enum above). */
void dsp_set_delay_division(int division);

/* 0-100: how much of the delayed signal feeds back into the delay line
 * (repeat count / decay length). Internally capped below runaway/
 * self-oscillation territory. */
void dsp_set_delay_feedback(int amount);

/* 0-100: how loud the repeats are in the output relative to dry signal.
 * Dry signal always passes through unattenuated. */
void dsp_set_delay_mix(int amount);

#endif /* SYNCDELAY_H */
