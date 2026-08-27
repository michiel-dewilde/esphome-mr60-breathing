/*
 * replay - run the firmware's DSP over a recorded fixture, on a workstation.
 *
 * This is the regression that keeps the firmware honest. The published rates
 * were validated against human breath counts using a Python reference
 * implementation; this binary compiles the *same C source the ESP32-C6 runs*
 * and checks it still reproduces those numbers. Every bug the port can have -
 * endianness, unwrap, filter design, sign alignment, the sub-harmonic guard -
 * surfaces here rather than in a device that quietly reports the wrong rate.
 *
 * Usage: replay <fixture.csv> [--profile human] [--window S] [--json]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "dsp.h"

static dsp_state_t g_state;

static int parse_line(char *line, int64_t *t_us, float *re, float *im) {
  char *p = line;
  char *end;
  long long t = strtoll(p, &end, 10);
  if (end == p) return 0;
  *t_us = (int64_t) t;
  p = end;
  for (int r = 0; r < DSP_ROWS; r++) {
    for (int c = 0; c < 2; c++) {
      if (*p != ',') return 0;
      p++;
      double v = strtod(p, &end);
      if (end == p) return 0;
      p = end;
      if (c == 0) re[r] = (float) v; else im[r] = (float) v;
    }
  }
  return 1;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <fixture.csv> [--profile human] "
                    "[--window S] [--json]\n", argv[0]);
    return 2;
  }

  dsp_config_t cfg;
  dsp_config_defaults(&cfg);
  int as_json = 0;
  double until_s = 0.0;

  for (int i = 2; i < argc; i++) {
    if (!strcmp(argv[i], "--json")) {
      as_json = 1;
    } else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
      if (!strcmp(argv[++i], "human")) {
        /* A resting adult sits near 0.2 Hz; the default band starts there and
         * would let the second harmonic of a slow breather compete. */
        cfg.band_lo_hz = 0.10f;
        cfg.band_hi_hz = 0.50f;
        cfg.highpass_hz = 0.05f;
        cfg.max_motion_ratio = 0.0f;
      }
    } else if (!strcmp(argv[i], "--window") && i + 1 < argc) {
      cfg.window_s = (float) atof(argv[++i]);
    } else if (!strcmp(argv[i], "--mindepth") && i + 1 < argc) {
      cfg.min_depth_um = (float) atof(argv[++i]);
    } else if (!strcmp(argv[i], "--until") && i + 1 < argc) {
      /* Stop reading at this offset in seconds. Lets a long unattended
       * recording be walked window by window with the same code the device
       * runs, rather than with a lookalike written for the occasion. */
      until_s = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--maxdepth") && i + 1 < argc) {
      cfg.max_motion_ratio = (float) atof(argv[++i]);
    } else if (!strcmp(argv[i], "--bandhi") && i + 1 < argc) {
      cfg.band_hi_hz = (float) atof(argv[++i]);
    } else if (!strcmp(argv[i], "--band") && i + 1 < argc) {
      cfg.band_lo_hz = (float) atof(argv[++i]);
    } else if (!strcmp(argv[i], "--hp") && i + 1 < argc) {
      cfg.highpass_hz = (float) atof(argv[++i]);
    } else if (!strcmp(argv[i], "--df") && i + 1 < argc) {
      cfg.freq_step_hz = (float) atof(argv[++i]);
    } else if (!strcmp(argv[i], "--stabwin") && i + 1 < argc) {
      cfg.stability_window_s = (float) atof(argv[++i]);
    }
  }

  FILE *f = fopen(argv[1], "r");
  if (!f) { perror(argv[1]); return 1; }

  dsp_init(&g_state);

  char line[4096];
  long n = 0, bad = 0;
  int64_t first_us = 0;
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    int64_t t_us;
    float re[DSP_ROWS], im[DSP_ROWS];
    if (!parse_line(line, &t_us, re, im)) { bad++; continue; }
    if (first_us == 0) first_us = t_us;
    if (until_s > 0.0 && (double) (t_us - first_us) / 1e6 > until_s) break;
    dsp_push(&g_state, t_us, re, im);
    n++;
  }
  fclose(f);

  if (!n) { fprintf(stderr, "%s: no usable rows\n", argv[1]); return 1; }

  dsp_result_t r;
  dsp_analyze(&g_state, &cfg, &r);

  if (as_json) {
    printf("{\"file\":\"%s\",\"status\":\"%s\",\"stable\":%d,"
           "\"rate_bpm\":%.3f,\"rate_td_bpm\":%.3f,\"snr_db\":%.2f,"
           "\"stability_pct\":%.1f,\"depth_um\":%.1f,\"motion_um\":%.1f,"
           "\"motion_ratio\":%.2f,\"fs_hz\":%.4f,"
           "\"duration_s\":%.2f,\"n_samples\":%d,\"n_rows\":%d,"
           "\"n_flipped\":%d,\"n_windows\":%d,\"n_breaths\":%d}\n",
           argv[1], dsp_status_name(r.status), r.stable,
           r.rate_bpm, r.rate_td_bpm, r.snr_db, r.stability_pct,
           r.depth_um, r.motion_um, r.motion_ratio, r.fs_hz, r.duration_s, r.n_samples, r.n_rows,
           r.n_flipped, r.n_windows, r.n_breaths);
    return 0;
  }

  printf("%s\n", argv[1]);
  printf("  sets read          %ld (%ld unparsable)\n", n, bad);
  printf("  window             %.1f s at %.3f Hz, %d samples\n",
         r.duration_s, r.fs_hz, r.n_samples);
  printf("  coherent average   %d rows, %d sign-flipped\n",
         r.n_rows, r.n_flipped);
  printf("  spectral rate      %.1f /min, SNR %.1f dB\n", r.rate_bpm, r.snr_db);
  printf("  time-domain rate   %.1f /min from %d peaks\n",
         r.rate_td_bpm, r.n_breaths);
  printf("  stability          %.0f %% of %d windows\n",
         r.stability_pct, r.n_windows);
  printf("  depth              %.0f um peak-to-peak (lower bound)\n",
         r.depth_um);
  printf("  status             %s\n", dsp_status_name(r.status));
  printf("  -> %s\n", r.stable ? "STABLE BREATHING PATTERN"
                               : "NO STABLE BREATHING PATTERN");
  return 0;
}
