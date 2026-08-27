/*
 * Benchmark: how expensive is one breathing analysis on this chip?
 *
 * Prints the cost of a full dsp_analyze over an embedded 60 s recording, plus
 * the result itself so the device can be checked against the host regression.
 * If the numbers differ from the host, something about the port is wrong -
 * compiler, float behaviour, or memory - and it is better to find that here
 * than in a deployed sensor.
 */

#include <Arduino.h>
#include <esp_timer.h>

extern "C" {
#include "dsp.h"
}
#include "bench_data.h"

static dsp_state_t state;
static dsp_config_t cfg;

static void fill() {
  dsp_init(&state);
  for (int i = 0; i < BENCH_SETS; i++) {
    float re[DSP_ROWS], im[DSP_ROWS];
    for (int r = 0; r < DSP_ROWS; r++) {
      re[r] = (float) bench_iq[i][r * 2];
      im[r] = (float) bench_iq[i][r * 2 + 1];
    }
    dsp_push(&state, (int64_t) bench_dt_us[i], re, im);
  }
}

static void report();

void setup() {
  Serial.setTxBufferSize(4096);
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
  delay(1500);
}

/* Repeated rather than run once at boot: on native USB-CDC the port only
 * appears after the host enumerates it, so a one-shot report at startup is
 * usually printed into a void. */
void loop() {
  report();
  delay(8000);
}

static void report() {
  Serial.printf("\n=== MR60 breathing DSP benchmark ===\n");
  Serial.printf("chip           %s, %d MHz, %d core(s)\n",
                ESP.getChipModel(), (int) getCpuFrequencyMhz(),
                (int) ESP.getChipCores());
  Serial.printf("state size     %u bytes\n", (unsigned) sizeof(dsp_state_t));
  Serial.printf("free heap      %u bytes\n", (unsigned) ESP.getFreeHeap());

  dsp_config_defaults(&cfg);

  int64_t t0 = esp_timer_get_time();
  fill();
  int64_t t_fill = esp_timer_get_time() - t0;
  Serial.printf("ingest         %lld us for %d sets (%.1f us/set)\n",
                (long long) t_fill, BENCH_SETS,
                (double) t_fill / BENCH_SETS);

  dsp_result_t r;
  const int reps = 5;
  int64_t best = 0, total = 0;
  for (int i = 0; i < reps; i++) {
    int64_t a = esp_timer_get_time();
    dsp_analyze(&state, &cfg, &r);
    int64_t d = esp_timer_get_time() - a;
    total += d;
    if (i == 0 || d < best) best = d;
    Serial.printf("analyze #%d     %lld us\n", i + 1, (long long) d);
  }
  Serial.printf("analyze best   %lld us  (mean %lld us)\n",
                (long long) best, (long long) (total / reps));

  Serial.printf("\nresult: %s  rate %.2f /min (td %.2f)  snr %.1f dB  "
                "stability %.0f %%  depth %.0f um\n",
                dsp_status_name(r.status), r.rate_bpm, r.rate_td_bpm,
                r.snr_db, r.stability_pct, r.depth_um);
  Serial.printf("rows %d (%d flipped), %d samples at %.3f Hz, %d windows\n",
                r.n_rows, r.n_flipped, r.n_samples, r.fs_hz, r.n_windows);
  Serial.printf("host reference: ok  rate 28.84 /min  snr 13.6 dB  "
                "stability 67 %%\n");
  Serial.printf("free heap      %u bytes\n\n", (unsigned) ESP.getFreeHeap());
}

