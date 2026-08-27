/*
 * dsp.c - see dsp.h for what this computes and why.
 *
 * Everything is float rather than double. The ESP32-C6 is RV32IMAC and has no
 * hardware floating point, so every operation is a libgcc call and single
 * precision costs roughly half of what double costs. The host regression is
 * what proves that the reduced precision does not move the answers.
 *
 * The filters mirror scipy's sosfiltfilt closely - odd-symmetric edge padding
 * and initial conditions scaled to the first sample - because the reference
 * numbers this code is checked against came out of scipy.
 */

#include "dsp.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * The agreement test needs at least this many windows before its result means
 * anything. With two windows it can only return 0, 50 or 100 %, and two noise
 * peaks landing within tolerance of each other is not unlikely: a 45 s
 * empty-room recording scored 100 % that way, and was only caught by the SNR
 * gate with 0.9 dB to spare. Given three or more windows the statistic
 * separates properly - the same recording scores 33 %, against 80-100 % for a
 * breathing animal. At the default 60 s window and 30 s sub-windows, three is
 * exactly what forms.
 */
#define MIN_STABILITY_WINDOWS 3

#define MAX_SECTIONS 4
#define MAX_FREQS 2048
#define PAD_MAX 64
#define MAX_PEAKS 256
#define MAX_WINDOWS 64

/* ------------------------------------------------------------------ scratch */
/*
 * File-scope so the analysis does not need a large task stack. dsp_analyze is
 * not re-entrant, which is fine: one radar, one analysis task.
 */
static float g_work[DSP_ROWS][DSP_MAX_SAMPLES];
static float g_coh[DSP_MAX_SAMPLES];
static float g_std[DSP_ROWS];
static int   g_rowidx[DSP_ROWS];
static float g_psd[MAX_FREQS];
static float g_sorted[MAX_FREQS];
static float g_pad[DSP_MAX_SAMPLES + 2 * PAD_MAX];
static float g_pad2[DSP_MAX_SAMPLES + 2 * PAD_MAX];
static float g_win[DSP_MAX_SAMPLES];
static float g_corr[DSP_ROWS][DSP_ROWS];
static float g_seg[DSP_MAX_SAMPLES];

/*
 * Working arrays for the peak detector and the window-agreement test.
 *
 * These lived on the stack until an ESP32-C6 caught them: 3 kB of peak indices
 * plus the window lists overflowed a 4 kB task stack and the chip took a stack
 * protection fault on the first analysis. Keeping every sizeable buffer in
 * static storage is what lets the analysis task stay small.
 */
static int   g_peak_idx[MAX_PEAKS];
static int   g_peak_keep[MAX_PEAKS];
static float g_peak_gap[MAX_PEAKS];
static float g_win_bpm[MAX_WINDOWS];
static float g_win_tmp[MAX_WINDOWS];

/* ------------------------------------------------------------------- yield */

static void (*g_yield)(void) = NULL;

void dsp_set_yield_hook(void (*fn)(void)) { g_yield = fn; }

static inline void dsp_yield(void) {
  if (g_yield != NULL) g_yield();
}

/* --------------------------------------------------------------- utilities */

static float median_inplace(float *a, int n) {
  /* Insertion sort: n is at most a few thousand and this runs a handful of
   * times per analysis, so the simple thing is fast enough and has no
   * pathological cases. */
  for (int i = 1; i < n; i++) {
    float v = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
    a[j + 1] = v;
  }
  return (n & 1) ? a[n / 2] : 0.5f * (a[n / 2 - 1] + a[n / 2]);
}

static void detrend_linear(float *x, int n) {
  if (n < 2) return;
  /* Least-squares line through equally spaced samples. */
  float sx = 0.0f, sxx = 0.0f, sy = 0.0f, sxy = 0.0f;
  for (int i = 0; i < n; i++) {
    float t = (float) i;
    sx += t; sxx += t * t; sy += x[i]; sxy += t * x[i];
  }
  float den = (float) n * sxx - sx * sx;
  if (fabsf(den) < 1e-12f) return;
  float slope = ((float) n * sxy - sx * sy) / den;
  float icept = (sy - slope * sx) / (float) n;
  for (int i = 0; i < n; i++) x[i] -= slope * (float) i + icept;
}

static float stddev(const float *x, int n) {
  if (n < 2) return 0.0f;
  float m = 0.0f;
  for (int i = 0; i < n; i++) m += x[i];
  m /= (float) n;
  float s = 0.0f;
  for (int i = 0; i < n; i++) { float d = x[i] - m; s += d * d; }
  return sqrtf(s / (float) n);
}

/* ----------------------------------------------------------------- biquads */

typedef struct { float b0, b1, b2, a1, a2; } biquad_t;

typedef struct {
  biquad_t s[MAX_SECTIONS];
  float    zi[MAX_SECTIONS][2];   /* steady state for unit input */
  int      n;
} sos_t;

/*
 * Butterworth sections via the RBJ biquad forms with the Butterworth pole Qs.
 * For an even order N the k-th section has Q = 1 / (2 cos(pi (2k+1) / (2N))).
 */
static float butter_q(int order, int k) {
  return 1.0f / (2.0f * cosf((float) (M_PI * (2 * k + 1) / (2.0 * order))));
}

static biquad_t rbj_section(float fc, float fs, float q, int highpass) {
  biquad_t bq;
  float w0 = 2.0f * (float) M_PI * fc / fs;
  float c = cosf(w0);
  float alpha = sinf(w0) / (2.0f * q);
  float a0 = 1.0f + alpha;
  if (highpass) {
    bq.b0 = (1.0f + c) / 2.0f / a0;
    bq.b1 = -(1.0f + c) / a0;
    bq.b2 = bq.b0;
  } else {
    bq.b0 = (1.0f - c) / 2.0f / a0;
    bq.b1 = (1.0f - c) / a0;
    bq.b2 = bq.b0;
  }
  bq.a1 = (-2.0f * c) / a0;
  bq.a2 = (1.0f - alpha) / a0;
  return bq;
}

/*
 * Steady-state initial conditions, the closed form of scipy's lfilter_zi for
 * a second-order section, cascaded by the DC gain of the preceding sections.
 * Without this the first samples of a filtfilt swing wildly, which on a 60 s
 * window is a measurable fraction of the record.
 */
static void sos_compute_zi(sos_t *f) {
  float scale = 1.0f;
  for (int i = 0; i < f->n; i++) {
    const biquad_t *b = &f->s[i];
    float B0 = b->b1 - b->a1 * b->b0;
    float B1 = b->b2 - b->a2 * b->b0;
    float den = 1.0f + b->a1 + b->a2;
    float z0 = (fabsf(den) < 1e-12f) ? 0.0f : (B0 + B1) / den;
    float z1 = B1 - b->a2 * z0;
    f->zi[i][0] = z0 * scale;
    f->zi[i][1] = z1 * scale;
    float num = b->b0 + b->b1 + b->b2;
    scale *= (fabsf(den) < 1e-12f) ? 1.0f : num / den;
  }
}

static void sos_reset(sos_t *f, int order, float fc, float fs, int highpass) {
  f->n = order / 2;
  if (f->n > MAX_SECTIONS) f->n = MAX_SECTIONS;
  for (int i = 0; i < f->n; i++)
    f->s[i] = rbj_section(fc, fs, butter_q(order, i), highpass);
  sos_compute_zi(f);
}

static void sos_run(const sos_t *f, const float *x, float *y, int n) {
  float z[MAX_SECTIONS][2];
  for (int i = 0; i < f->n; i++) {
    z[i][0] = f->zi[i][0] * x[0];
    z[i][1] = f->zi[i][1] * x[0];
  }
  for (int k = 0; k < n; k++) {
    float v = x[k];
    for (int i = 0; i < f->n; i++) {
      const biquad_t *b = &f->s[i];
      float out = b->b0 * v + z[i][0];
      z[i][0] = b->b1 * v - b->a1 * out + z[i][1];
      z[i][1] = b->b2 * v - b->a2 * out;
      v = out;
    }
    y[k] = v;
  }
}

/*
 * Zero-phase filtering: forward, reverse, forward again on the reversed
 * signal. Odd-symmetric padding matches scipy's padtype="odd" default.
 */
static void sos_filtfilt(const sos_t *f, float *x, int n) {
  int pad = 3 * (2 * f->n + 1) - 1;
  if (pad > PAD_MAX) pad = PAD_MAX;
  if (pad > n - 1) pad = (n > 1) ? n - 1 : 0;
  int m = n + 2 * pad;

  for (int i = 0; i < pad; i++) g_pad[i] = 2.0f * x[0] - x[pad - i];
  memcpy(g_pad + pad, x, (size_t) n * sizeof(float));
  for (int i = 0; i < pad; i++)
    g_pad[pad + n + i] = 2.0f * x[n - 1] - x[n - 2 - i];

  sos_run(f, g_pad, g_pad2, m);

  for (int i = 0; i < m; i++) g_pad[i] = g_pad2[m - 1 - i];
  sos_run(f, g_pad, g_pad2, m);

  for (int i = 0; i < n; i++) x[i] = g_pad2[m - 1 - (pad + i)];
}

/* ------------------------------------------------------------------ spectra */

/*
 * Goertzel power at one frequency. Cheaper than a zero-padded FFT here: the
 * band of interest is 0.20-1.10 Hz out of a 2.44 Hz Nyquist, so only a few
 * hundred frequencies matter and an FFT would compute thousands.
 */
static float goertzel_power(const float *x, int n, float k) {
  float coeff = 2.0f * cosf(2.0f * (float) M_PI * k);
  float s1 = 0.0f, s2 = 0.0f;
  for (int i = 0; i < n; i++) {
    float s0 = x[i] + coeff * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

static void hann(float *w, int n) {
  for (int i = 0; i < n; i++)
    w[i] = 0.5f - 0.5f * cosf(2.0f * (float) M_PI * (float) i / (float) (n - 1));
}

/*
 * Welch PSD sampled on an explicit frequency grid. nseg segments of nperseg
 * samples with 50 % overlap, Hann-windowed, linearly detrended - the same
 * arrangement the reference implementation used.
 */
static int psd_grid(const float *x, int n, float fs, float f_lo, float f_hi,
                    float df, int nperseg, float *out, float *out_f) {
  int nfreq = (int) ((f_hi - f_lo) / df) + 1;
  if (nfreq > MAX_FREQS) nfreq = MAX_FREQS;
  if (nfreq < 3 || nperseg < 8 || nperseg > n) return 0;

  int step = nperseg / 2;
  if (step < 1) step = 1;

  hann(g_win, nperseg);
  float wsum2 = 0.0f;
  for (int i = 0; i < nperseg; i++) wsum2 += g_win[i] * g_win[i];

  for (int i = 0; i < nfreq; i++) out[i] = 0.0f;

  int nseg = 0;
  for (int s0 = 0; s0 + nperseg <= n; s0 += step) {
    float *seg = g_seg;
    memcpy(seg, x + s0, (size_t) nperseg * sizeof(float));
    detrend_linear(seg, nperseg);
    for (int i = 0; i < nperseg; i++) seg[i] *= g_win[i];
    for (int i = 0; i < nfreq; i++) {
      float f = f_lo + df * (float) i;
      out[i] += goertzel_power(seg, nperseg, f / fs);
      /* Every 32 frequencies is a few milliseconds of work at most, which is
       * short enough to keep the UART fed and long enough that the yield
       * itself costs nothing measurable. */
      if ((i & 31) == 31) dsp_yield();
    }
    nseg++;
  }
  if (!nseg) return 0;

  float scale = 2.0f / (fs * wsum2 * (float) nseg);
  for (int i = 0; i < nfreq; i++) {
    out[i] *= scale;
    if (out_f) out_f[i] = f_lo + df * (float) i;
  }
  return nfreq;
}

/*
 * Peak of the band, guarded against locking onto the second harmonic.
 *
 * The guard has a lower edge condition that is not optional. Residual drift
 * piles up just above the high-pass corner, and an earlier version happily
 * halved a genuine 0.44 Hz peak into that pile-up and reported 13.3 per minute
 * for a cat breathing at 30. A sub-harmonic is only believed when it sits
 * clear of the band edge AND stands 6 dB above the band floor in its own
 * right.
 */
static int band_peak(const float *psd, int nfreq, float f_lo, float df,
                     float *f_out, float *snr_out) {
  if (nfreq < 3) return 0;

  int k = 0;
  for (int i = 1; i < nfreq; i++) if (psd[i] > psd[k]) k = i;
  float p0 = psd[k];

  memcpy(g_sorted, psd, (size_t) nfreq * sizeof(float));
  float floorp = median_inplace(g_sorted, nfreq);

  float f0 = f_lo + df * (float) k;
  float half = f0 * 0.5f;
  if (half >= f_lo + 0.05f) {
    int j = (int) ((half - f_lo) / df + 0.5f);
    int lo = j - 2, hi = j + 2;
    if (lo < 0) lo = 0;
    if (hi > nfreq - 1) hi = nfreq - 1;
    if (lo <= hi) {
      int jj = lo;
      for (int i = lo; i <= hi; i++) if (psd[i] > psd[jj]) jj = i;
      float cand = psd[jj];
      int strong = (floorp > 0.0f) &&
                   (10.0f * log10f(cand / floorp) >= 6.0f);
      if (strong && cand > p0 / 3.98107f) {   /* within 6 dB of the argmax */
        k = jj;
        p0 = cand;
        f0 = f_lo + df * (float) k;
      }
    }
  }

  /* Parabolic refinement, to recover what the discrete grid throws away. */
  if (k > 0 && k < nfreq - 1) {
    float a = psd[k - 1], b = psd[k], c = psd[k + 1];
    float den = a - 2.0f * b + c;
    if (fabsf(den) > 1e-20f) {
      float d = 0.5f * (a - c) / den;
      if (d > -1.0f && d < 1.0f) f0 += d * df;
    }
  }

  *f_out = f0;
  *snr_out = (floorp > 0.0f) ? 10.0f * log10f(p0 / floorp) : 99.0f;
  return 1;
}

/* ------------------------------------------------------------------- public */

void dsp_config_defaults(dsp_config_t *cfg) {
  cfg->band_lo_hz = 0.20f;              /* 12 /min */
  cfg->band_hi_hz = 1.10f;              /* 66 /min */
  cfg->highpass_hz = 0.10f;
  cfg->window_s = 60.0f;
  cfg->stability_window_s = 30.0f;
  cfg->stability_tol_bpm = 3.0f;
  cfg->stability_threshold_pct = 60.0f;
  cfg->min_snr_db = 6.0f;
  cfg->min_depth_um = 0.0f;             /* gate disabled until measured */
  /* 0.6 /min raw resolution, recovered to better than 0.05 /min by the
   * parabolic refinement in band_peak. Measured against a 0.002 Hz grid on all
   * six reference recordings the rates move by at most 0.02 /min, and the grid
   * is the dominant cost of the whole analysis: 0.002 put one pass at 892 ms
   * on the ESP32-C6. */
  cfg->freq_step_hz = 0.010f;
  cfg->row_min = 0;
  cfg->row_max = 7;
}

const char *dsp_status_name(dsp_status_t s) {
  switch (s) {
    case DSP_STATUS_OK:          return "ok";
    case DSP_STATUS_UNSTABLE:    return "unstable";
    case DSP_STATUS_LOW_SNR:     return "low_snr";
    case DSP_STATUS_TOO_SHALLOW: return "too_shallow";
    case DSP_STATUS_NO_DATA:     return "no_data";
    default:                     return "warming_up";
  }
}

void dsp_init(dsp_state_t *st) {
  memset(st, 0, sizeof(*st));
}

void dsp_decode_tile(const uint8_t *p, float *re8, float *im8) {
  /* Zero-Doppler bin of each of the 8 rows: the static-reflection bin, which
   * is where a chest that is not translating shows up as phase modulation. */
  for (int r = 0; r < DSP_ROWS_PER_TILE; r++) {
    const uint8_t *q = p + (size_t) r * DSP_DOPPLER * 4;
    int16_t re = (int16_t) ((uint16_t) q[0] | ((uint16_t) q[1] << 8));
    int16_t im = (int16_t) ((uint16_t) q[2] | ((uint16_t) q[3] << 8));
    re8[r] = (float) re;
    im8[r] = (float) im;
  }
}

void dsp_push(dsp_state_t *st, int64_t t_us, const float *re, const float *im) {
  int i = st->head;
  for (int r = 0; r < DSP_ROWS; r++) {
    float a = atan2f(im[r], re[r]);
    float ph;
    if (!st->started) {
      ph = a;
    } else {
      float d = a - st->last_angle[r];
      while (d > (float) M_PI) d -= 2.0f * (float) M_PI;
      while (d < -(float) M_PI) d += 2.0f * (float) M_PI;
      ph = st->last_phase[r] + d;
    }
    st->last_angle[r] = a;
    st->last_phase[r] = ph;
    st->phase[r][i] = ph;
  }
  st->t_us[i] = t_us;
  st->started = 1;
  st->head = (i + 1) % DSP_MAX_SAMPLES;
  if (st->count < DSP_MAX_SAMPLES) st->count++;
}

/* Oldest-to-newest index of the j-th sample within the last `take` samples. */
static int ring_idx(const dsp_state_t *st, int take, int j) {
  int first = st->head - take;
  while (first < 0) first += DSP_MAX_SAMPLES;
  return (first + j) % DSP_MAX_SAMPLES;
}

static void fail(dsp_result_t *out, dsp_status_t s) {
  memset(out, 0, sizeof(*out));
  out->status = s;
  out->rate_bpm = NAN;
  out->rate_td_bpm = NAN;
}

int dsp_analyze(const dsp_state_t *st, const dsp_config_t *cfg,
                dsp_result_t *out) {
  fail(out, DSP_STATUS_WARMING_UP);
  if (st->count < 32) return 0;

  /* --- pick the most recent window_s of history ------------------------- */
  int take = st->count;
  {
    int last = ring_idx(st, take, take - 1);
    int64_t t_end = st->t_us[last];
    int64_t span = (int64_t) (cfg->window_s * 1e6f);
    while (take > 32) {
      int first = ring_idx(st, take, 0);
      if (t_end - st->t_us[first] <= span) break;
      take--;
    }
  }
  if (take < 32) return 0;

  int i0 = ring_idx(st, take, 0);
  int i1 = ring_idx(st, take, take - 1);
  double span_s = (double) (st->t_us[i1] - st->t_us[i0]) / 1e6;
  if (span_s <= 1.0) { fail(out, DSP_STATUS_NO_DATA); return 0; }

  int n = take;
  float fs = (float) ((n - 1) / span_s);
  if (fs <= 0.0f || !isfinite(fs)) { fail(out, DSP_STATUS_NO_DATA); return 0; }

  float nyq = fs * 0.5f;
  float hi = cfg->band_hi_hz;
  if (hi > 0.95f * nyq) hi = 0.95f * nyq;
  if (hi <= cfg->band_lo_hz + 0.05f) { fail(out, DSP_STATUS_NO_DATA); return 0; }

  /* --- resample each row onto a uniform grid, then filter ---------------- */
  sos_t hp, hp2, lp;
  sos_reset(&hp,  4, cfg->highpass_hz, fs, 1);
  sos_reset(&hp2, 4, cfg->band_lo_hz,  fs, 1);
  sos_reset(&lp,  4, hi,               fs, 0);

  double t0 = (double) st->t_us[i0] / 1e6;
  double dt = span_s / (double) (n - 1);

  int nrows = 0;
  for (int r = 0; r < DSP_ROWS; r++) {
    int range = r & 7;
    if (range < cfg->row_min || range > cfg->row_max) continue;

    float *y = g_work[nrows];
    int src = 0;
    for (int k = 0; k < n; k++) {
      double tt = t0 + dt * (double) k;
      while (src < n - 2 &&
             (double) st->t_us[ring_idx(st, take, src + 1)] / 1e6 < tt) src++;
      int ia = ring_idx(st, take, src);
      int ib = ring_idx(st, take, src + 1 < n ? src + 1 : src);
      double ta = (double) st->t_us[ia] / 1e6;
      double tb = (double) st->t_us[ib] / 1e6;
      float va = st->phase[r][ia], vb = st->phase[r][ib];
      float w = (tb > ta) ? (float) ((tt - ta) / (tb - ta)) : 0.0f;
      if (w < 0.0f) w = 0.0f;
      if (w > 1.0f) w = 1.0f;
      y[k] = va + (vb - va) * w;
    }

    sos_filtfilt(&hp, y, n);
    dsp_yield();
    sos_filtfilt(&hp2, y, n);
    dsp_yield();
    sos_filtfilt(&lp, y, n);
    dsp_yield();

    float s = stddev(y, n);
    if (!(s > 1e-9f) || !isfinite(s)) continue;   /* flat or broken row */
    g_std[nrows] = s;
    g_rowidx[nrows] = r;
    for (int k = 0; k < n; k++) y[k] /= s;
    nrows++;
  }

  if (nrows < 2) { fail(out, DSP_STATUS_NO_DATA); return 0; }

  /* --- sign-aligned coherent average ------------------------------------ */
  /* Rows are normalised, so the correlation is just the mean product. */
  for (int a = 0; a < nrows; a++) {
    dsp_yield();
    for (int b = a; b < nrows; b++) {
      float acc = 0.0f;
      for (int k = 0; k < n; k++) acc += g_work[a][k] * g_work[b][k];
      acc /= (float) n;
      g_corr[a][b] = acc;
      g_corr[b][a] = acc;
    }
  }
  int ref = 0;
  float best = -1.0f;
  for (int a = 0; a < nrows; a++) {
    float s = 0.0f;
    for (int b = 0; b < nrows; b++) s += fabsf(g_corr[a][b]);
    if (s > best) { best = s; ref = a; }
  }
  int nflip = 0;
  for (int k = 0; k < n; k++) g_coh[k] = 0.0f;
  for (int a = 0; a < nrows; a++) {
    float sgn = (g_corr[ref][a] < 0.0f) ? -1.0f : 1.0f;
    if (sgn < 0.0f) nflip++;
    for (int k = 0; k < n; k++) g_coh[k] += sgn * g_work[a][k];
  }
  for (int k = 0; k < n; k++) g_coh[k] /= (float) nrows;

  /* --- spectral rate ----------------------------------------------------- */
  int nperseg = n / 2;
  if (nperseg < 16) nperseg = (n < 16) ? n : 16;
  int nfreq = psd_grid(g_coh, n, fs, cfg->band_lo_hz, hi, cfg->freq_step_hz,
                       nperseg, g_psd, NULL);
  if (!nfreq) { fail(out, DSP_STATUS_NO_DATA); return 0; }

  float f0 = 0.0f, snr = 0.0f;
  if (!band_peak(g_psd, nfreq, cfg->band_lo_hz, cfg->freq_step_hz, &f0, &snr)) {
    fail(out, DSP_STATUS_NO_DATA);
    return 0;
  }

  /* --- window agreement, the stability statistic ------------------------- */
  int w = (int) (cfg->stability_window_s * fs);
  int nwin = 0, agree = 0;
  float *wf = g_win_bpm;
  if (w >= 16 && w <= n) {
    int wstep = w / 2;
    if (wstep < 1) wstep = 1;
    for (int s0 = 0; s0 + w <= n && nwin < MAX_WINDOWS; s0 += wstep) {
      int nf = psd_grid(g_coh + s0, w, fs, cfg->band_lo_hz, hi,
                        cfg->freq_step_hz, w, g_psd, NULL);
      if (!nf) continue;
      float wfq, wsnr;
      if (!band_peak(g_psd, nf, cfg->band_lo_hz, cfg->freq_step_hz,
                     &wfq, &wsnr)) continue;
      wf[nwin++] = wfq * 60.0f;
    }
  }
  float stability = 0.0f, win_median = NAN;
  if (nwin > 0) {
    float *tmp = g_win_tmp;
    memcpy(tmp, wf, (size_t) nwin * sizeof(float));
    win_median = median_inplace(tmp, nwin);
    for (int i = 0; i < nwin; i++)
      if (fabsf(wf[i] - win_median) <= cfg->stability_tol_bpm) agree++;
    stability = 100.0f * (float) agree / (float) nwin;
  }

  /* --- time-domain breath count, independent of the spectrum ------------- */
  float td = NAN;
  int nbreaths = 0;
  {
    int dist = (int) (fs / (f0 * 1.7f));
    if (dist < 1) dist = 1;
    float height = 0.3f * stddev(g_coh, n);
    /* Local maxima above the height gate, kept greedily by amplitude so that
     * the minimum-distance rule matches scipy's find_peaks. */
    int *idx = g_peak_idx;
    int cnt = 0;
    for (int k = 1; k < n - 1 && cnt < MAX_PEAKS; k++)
      if (g_coh[k] > height && g_coh[k] >= g_coh[k - 1] && g_coh[k] > g_coh[k + 1])
        idx[cnt++] = k;
    int *keep = g_peak_keep;
    int nkeep = 0;
    for (;;) {
      int bestk = -1;
      for (int i = 0; i < cnt; i++)
        if (idx[i] >= 0 && (bestk < 0 || g_coh[idx[i]] > g_coh[idx[bestk]]))
          bestk = i;
      if (bestk < 0) break;
      int pos = idx[bestk];
      keep[nkeep++] = pos;
      for (int i = 0; i < cnt; i++)
        if (idx[i] >= 0 && abs(idx[i] - pos) < dist) idx[i] = -1;
      if (nkeep >= MAX_PEAKS) break;
    }
    nbreaths = nkeep;
    if (nkeep > 1) {
      for (int i = 1; i < nkeep; i++) {         /* sort kept peaks by time */
        int v = keep[i], j = i - 1;
        while (j >= 0 && keep[j] > v) { keep[j + 1] = keep[j]; j--; }
        keep[j + 1] = v;
      }
      float *d = g_peak_gap;
      for (int i = 1; i < nkeep; i++) d[i - 1] = (float) (keep[i] - keep[i - 1]) / fs;
      float md = median_inplace(d, nkeep - 1);
      if (md > 0.0f) td = 60.0f / md;
    }
  }

  /* --- displacement, a lower bound --------------------------------------- */
  /* Taken from the reference row - the one most representative of the common
   * signal - using its amplitude before normalisation. Peak-to-peak of a
   * sinusoid is 2*sqrt(2) times the RMS. */
  float depth = g_std[ref] * 2.828427f * DSP_UM_PER_RADIAN;

  /* --- verdict ----------------------------------------------------------- */
  out->rate_bpm = f0 * 60.0f;
  out->rate_td_bpm = td;
  out->snr_db = snr;
  out->stability_pct = stability;
  out->depth_um = depth;
  out->fs_hz = fs;
  out->duration_s = (float) span_s;
  out->n_samples = n;
  out->n_rows = nrows;
  out->n_flipped = nflip;
  out->n_windows = nwin;
  out->n_breaths = nbreaths;

  if (nwin < MIN_STABILITY_WINDOWS) {
    out->status = DSP_STATUS_WARMING_UP;
    out->stable = 0;
  } else if (stability < cfg->stability_threshold_pct) {
    out->status = DSP_STATUS_UNSTABLE;
    out->stable = 0;
  } else if (snr < cfg->min_snr_db) {
    out->status = DSP_STATUS_LOW_SNR;
    out->stable = 0;
  } else if (cfg->min_depth_um > 0.0f && depth < cfg->min_depth_um) {
    out->status = DSP_STATUS_TOO_SHALLOW;
    out->stable = 0;
  } else {
    out->status = DSP_STATUS_OK;
    out->stable = 1;
  }
  (void) win_median;
  return 1;
}
