/*
 * dsp.h - breathing-rate estimation from MR60BHA2 range-Doppler tiles.
 *
 * Plain C11. No ESPHome, Arduino, ESP-IDF or libc extras beyond <math.h> and
 * <string.h>, so the identical source compiles for the ESP32-C6 firmware and
 * for the host-side regression harness. That is deliberate: the numbers this
 * code produces were validated against human breath counts on a workstation,
 * and the only way to keep the firmware honest is to keep running the same
 * source against the same recordings.
 *
 * The pipeline, and why each step is there:
 *
 *   1. zero-Doppler bin of each range row -> atan2 -> incremental unwrap
 *   2. resample onto a uniform grid (tile sets do not arrive evenly)
 *   3. zero-phase high-pass, then zero-phase band-pass
 *   4. normalise each row by its standard deviation
 *   5. SIGN-ALIGNED coherent average across rows. A plain mean partially
 *      cancels the signal: range-sidelobe leakage alternates sign from bin to
 *      bin, so neighbouring rows carry anticorrelated copies of the same
 *      chest motion. Measured: three captures of one resting cat gave
 *      32.5 / 32.5 / 29.3 per minute naively and 29.3 / 29.3 / 29.3 aligned.
 *   6. spectral peak with a sub-harmonic guard. Chest motion is not
 *      sinusoidal and the second harmonic reached 13 dB on that cat.
 *   7. window agreement: the fraction of 30 s windows whose peak matches the
 *      session median. This is the statistic that answers "is the pattern
 *      stable". Across six recordings it separates without overlap - real
 *      targets 67-100 %, an empty room 0 %. The coefficient of variation of
 *      breath-to-breath intervals does NOT separate and is not used for it.
 *
 * Accuracy of the reported rate is +/-4 per minute, measured against three
 * human counts. Both blind trials read low, and the two largest errors were on
 * the two fastest rates, so there may be a low bias at high rates; that is
 * unproven at n=3. A time-domain estimate is returned alongside, and when the
 * two disagree the higher one has been closer to the truth.
 */

#ifndef MR60_DSP_H
#define MR60_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 8 range rows per channel, two receive channels (frame types 0x0A32 and
 * 0x0A34). Eight distinct ranges, sixteen row signals. */
#define DSP_ROWS 16
#define DSP_ROWS_PER_TILE 8
#define DSP_DOPPLER 32

/*
 * Ring capacity in tile sets. At 4.88 sets/s, 512 covers 104 s.
 *
 * Cost is DSP_ROWS * DSP_MAX_SAMPLES * 4 bytes for the history plus the same
 * again for the filtering scratch, so 512 costs about 75 kB of static RAM and
 * 1024 costs about 150 kB. Measured on hardware, 1024 put the benchmark at
 * 58 % of RAM before WiFi was even initialised, which does not leave room for
 * ESPHome. 512 is the shipped value and caps the analysis window near 100 s;
 * the host regression overrides it so the 180 s reference recording still
 * runs.
 */
#ifndef DSP_MAX_SAMPLES
#define DSP_MAX_SAMPLES 512
#endif

/* 60 GHz: lambda = 5 mm. Phase to one-way displacement is lambda/(4*pi),
 * which is 397.9 um per radian. */
#define DSP_UM_PER_RADIAN 397.887358f

typedef struct {
  float band_lo_hz;             /* search band, lower edge   (0.20 = 12 /min) */
  float band_hi_hz;             /* search band, upper edge   (1.10 = 66 /min) */
  float highpass_hz;            /* drift removal before the band-pass  (0.10) */
  float window_s;               /* analysis window                     (60)   */
  float stability_window_s;     /* sub-window for the agreement test   (30)   */
  float stability_tol_bpm;      /* tolerance around the median         (3)    */
  float stability_threshold_pct;/* verdict threshold                   (60)   */
  float min_snr_db;             /* verdict threshold                   (6)    */
  float min_depth_um;           /* verdict threshold, 0 disables the gate     */
  float freq_step_hz;           /* spectral grid resolution         (0.002)   */
  int   row_min, row_max;       /* which of the 8 ranges to include (0..7)    */
} dsp_config_t;

/* Sensible starting point; min_depth_um is left at 0 because an honest value
 * has to come from measuring this specific installation. */
void dsp_config_defaults(dsp_config_t *cfg);

typedef enum {
  DSP_STATUS_WARMING_UP = 0,  /* not enough history yet                      */
  DSP_STATUS_NO_DATA,         /* no tiles arriving, or all rows flat         */
  DSP_STATUS_UNSTABLE,        /* peak moves between windows                  */
  DSP_STATUS_LOW_SNR,         /* stable-looking but weak                     */
  DSP_STATUS_TOO_SHALLOW,     /* below the displacement gate                 */
  DSP_STATUS_OK               /* reliable measurement                        */
} dsp_status_t;

typedef struct {
  dsp_status_t status;
  int   stable;            /* 1 when the rate may be published               */
  float rate_bpm;          /* spectral estimate, NAN when not measurable     */
  float rate_td_bpm;       /* time-domain estimate, NAN when unavailable     */
  float snr_db;
  float stability_pct;
  float depth_um;          /* peak-to-peak displacement, a LOWER BOUND       */
  float fs_hz;
  float duration_s;
  int   n_samples;
  int   n_rows;            /* rows that carried usable signal                */
  int   n_flipped;         /* rows the sign alignment inverted               */
  int   n_windows;         /* windows the agreement test could form          */
  int   n_breaths;         /* peaks the time-domain detector found           */
} dsp_result_t;

const char *dsp_status_name(dsp_status_t s);

/* Opaque only by convention: the firmware needs the size to place it in a
 * static allocation rather than on a task stack. */
typedef struct {
  float   phase[DSP_ROWS][DSP_MAX_SAMPLES];  /* unwrapped, radians */
  int64_t t_us[DSP_MAX_SAMPLES];
  float   last_angle[DSP_ROWS];              /* unwrap carry */
  float   last_phase[DSP_ROWS];
  int     head;                              /* next write index */
  int     count;                             /* samples held, <= DSP_MAX_SAMPLES */
  int     started;
} dsp_state_t;

/*
 * Optional cooperative yield.
 *
 * One analysis pass costs about 450 ms on an ESP32-C6, which is far too long
 * to hold the CPU: the radar UART runs at 88 % of capacity with roughly 400 ms
 * of buffer slack, and starving the idle task trips the watchdog. The firmware
 * installs a hook here that gives other tasks a turn; the host harness leaves
 * it unset and the calls compile away to a null check.
 *
 * The hook must not touch dsp state and must return promptly.
 */
void dsp_set_yield_hook(void (*fn)(void));

void dsp_init(dsp_state_t *st);

/*
 * Decode one 1024-byte collection-mode payload into the zero-Doppler bin of
 * each of its 8 range rows. Payload is 8 rows x 32 Doppler bins x (int16 real,
 * int16 imag), little-endian.
 */
void dsp_decode_tile(const uint8_t *payload, float *re8, float *im8);

/*
 * Push one complete tile set: 16 complex values, channel A rows 0..7 followed
 * by channel C rows 0..7. Unwrapping happens here so the history stays a plain
 * float array.
 */
void dsp_push(dsp_state_t *st, int64_t t_us, const float *re, const float *im);

/*
 * Run the full analysis over the most recent cfg->window_s of history.
 * Returns 1 when a rate was computed (even an unreliable one), 0 otherwise.
 * out is always filled in, with status explaining any refusal.
 */
int dsp_analyze(const dsp_state_t *st, const dsp_config_t *cfg,
                dsp_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MR60_DSP_H */
