#include "mr60_breathing.h"

#include "esphome/core/log.h"

#include <esp_timer.h>
#include <cinttypes>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstring>

namespace esphome {
namespace mr60_breathing {

static const char *const TAG = "mr60_breathing";

static const size_t HEADER_LEN = 8;
static const size_t MAX_PAYLOAD = 2048;

/*
 * The only frames ever sent to the radar.
 *
 * Provenance: reverse-engineered by akkauppi/mr60bha2-internals against
 * firmware v1.6.12; the header checksums were recomputed independently and
 * agree with the frame format above.
 *
 * All three have a zero-length payload. Version-query is a pure read, and
 * collection on/off set a single byte in RAM - no flash write, and a radar
 * reset clears it. The frame that DOES touch flash (0x3000, firmware update)
 * is deliberately absent, and there is no path here for sending an arbitrary
 * frame. Recovering a bricked radar module needs a J-Link on SWD pads that are
 * probably not exposed on this board, so that command has no business being
 * reachable from a configuration file.
 */
static const uint8_t CMD_COLLECTION_ON[] = {0x01, 0x00, 0x00, 0x00,
                                            0x00, 0x0A, 0x14, 0xE0};
static const uint8_t CMD_COLLECTION_OFF[] = {0x01, 0x00, 0x00, 0x00,
                                             0x00, 0x0A, 0x15, 0xE1};

/* Re-arm collection mode if tiles stop arriving for this long: the enable is a
 * RAM byte, so any radar reset silently reverts to normal reporting. */
static const uint32_t COLLECTION_RETRY_MS = 5000;
static const uint32_t DIAG_PERIOD_MS = 10000;

static inline uint8_t xor_not(const uint8_t *p, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++)
    c ^= p[i];
  return (uint8_t) ~c;
}

static inline uint16_t be16(const uint8_t *p) {
  return (uint16_t) (((uint16_t) p[0] << 8) | p[1]);
}

// ---------------------------------------------------------------- lifecycle

/*
 * Give the rest of the system a turn. Installed as the DSP's yield hook, so a
 * 450 ms analysis does not starve the UART or the idle task.
 */
static void dsp_yield_to_scheduler() { vTaskDelay(1); }

static void analysis_task_entry(void *param) {
  static_cast<MR60Breathing *>(param)->analysis_loop();
}

void MR60Breathing::setup() {
  dsp_init(&this->dsp_);
  dsp_config_defaults(&this->cfg_);
  memset(this->rate_hist_, 0, sizeof(this->rate_hist_));
  this->enable_collection_();

  dsp_set_yield_hook(dsp_yield_to_scheduler);
  this->raw_.setup();

  /*
   * The analysis runs in its own task at the same priority as the ESPHome main
   * loop, relying on the yield hook rather than on priority to share the CPU.
   * A lower priority would sit below the idle task and a higher one would let
   * a 450 ms pass block the UART outright.
   *
   * 8 kB of stack. The DSP keeps its buffers in static storage so it does not
   * need a deep stack, but 4 kB was not enough: the peak detector's working
   * arrays took a stack protection fault on the first analysis, which is what
   * moved them into static storage. The margin here is deliberate.
   */
  BaseType_t ok = xTaskCreate(analysis_task_entry, "mr60_dsp", 8192, this,
                              tskIDLE_PRIORITY + 1, nullptr);
  if (ok != pdPASS) {
    ESP_LOGE(TAG, "could not start the analysis task");
    this->mark_failed();
    return;
  }
  this->task_started_ = true;
}

void MR60Breathing::dump_config() {
  ESP_LOGCONFIG(TAG, "MR60BHA2 breathing:");
  ESP_LOGCONFIG(TAG, "  band            %.1f - %.1f /min",
                this->cfg_.band_lo_hz * 60.0f, this->cfg_.band_hi_hz * 60.0f);
  ESP_LOGCONFIG(TAG, "  window          %.0f s", this->cfg_.window_s);
  ESP_LOGCONFIG(TAG, "  stability       %.0f s windows, +/-%.1f /min, >= %.0f %%",
                this->cfg_.stability_window_s, this->cfg_.stability_tol_bpm,
                this->cfg_.stability_threshold_pct);
  ESP_LOGCONFIG(TAG, "  gates           SNR >= %.1f dB, depth >= %.0f um",
                this->cfg_.min_snr_db, this->cfg_.min_depth_um);
  ESP_LOGCONFIG(TAG, "  range rows      %d - %d", this->cfg_.row_min,
                this->cfg_.row_max);
  ESP_LOGCONFIG(TAG, "  update interval %.0f s%s", this->update_interval_s_,
                this->update_interval_s_ <= 0.0f ? " (as fast as possible)" : "");
  ESP_LOGCONFIG(TAG, "  analysis state  %u bytes", (unsigned) sizeof(dsp_state_t));
  ESP_LOGCONFIG(TAG, "  raw capture     %s, %s mode",
                this->raw_.enabled() ? "listening" : "off",
                this->raw_.mode() == RAW_MODE_TILES ? "tiles" : "phase");
}

// ------------------------------------------------------------- radar link

void MR60Breathing::send_command_(const uint8_t *cmd, size_t len,
                                  const char *name) {
  this->write_array(cmd, len);
  ESP_LOGD(TAG, "sent %s", name);
}

void MR60Breathing::enable_collection_() {
  this->send_command_(CMD_COLLECTION_ON, sizeof(CMD_COLLECTION_ON),
                      "collection-on");
  this->last_collection_cmd_ms_ = millis();
}

/*
 * Sliding-window parser.
 *
 * The rule that matters: on EVERY failure we advance exactly one byte and
 * start looking for 0x01 again. We never skip "the rest of the frame" based on
 * a length field we could not trust, and never jump blindly to the next 0x01
 * without revalidating the header checksum. A 0x01 that happens to sit inside
 * a payload dies on the header checksum and costs us a single byte.
 *
 * Returns how many bytes may be dropped from the front of the buffer.
 */
size_t MR60Breathing::parse_buffer_(size_t len) {
  size_t i = 0;

  while (i < len) {
    if (this->rx_buf_[i] != 0x01) {  // start of frame
      i++;
      this->resync_bytes_++;
      continue;
    }

    if (len - i < HEADER_LEN)
      break;  // wait for more bytes

    const uint8_t *h = this->rx_buf_ + i;

    if (xor_not(h, 7) != h[7]) {  // header checksum over bytes 0..6
      this->header_errors_++;
      i++;
      this->resync_bytes_++;
      continue;
    }

    uint16_t plen = be16(h + 3);
    if (plen > MAX_PAYLOAD) {  // nonsensical length: a false header
      this->header_errors_++;
      i++;
      this->resync_bytes_++;
      continue;
    }

    size_t need = HEADER_LEN + (size_t) plen + 1;
    if (len - i < need)
      break;  // wait for the whole frame

    const uint8_t *payload = h + HEADER_LEN;
    if (xor_not(payload, plen) != payload[plen]) {
      this->payload_errors_++;
      i++;
      this->resync_bytes_++;
      continue;
    }

    this->frames_ok_++;
    this->handle_frame_(esp_timer_get_time(), be16(h + 5), payload, plen);
    i += need;
  }

  return i;
}

void MR60Breathing::handle_frame_(int64_t t_us, uint16_t type,
                                  const uint8_t *payload, uint16_t len) {
  if (type != TYPE_TILE_A && type != TYPE_TILE_C)
    return;
  if (len != TILE_BYTES)
    return;

  this->last_tile_us_ = t_us;
  this->raw_.write_tile(t_us, type, payload, len);

  if (type == TYPE_TILE_A) {
    if (this->have_a_)
      this->unpaired_tiles_++;  // previous A never found its C
    dsp_decode_tile(payload, this->a_re_, this->a_im_);
    this->a_time_ = t_us;
    this->have_a_ = true;
    return;
  }

  // Channel C: complete the set if its partner is recent enough. The two
  // channels arrive back to back but not with identical timestamps.
  if (!this->have_a_ || (t_us - this->a_time_) > 150000) {
    this->unpaired_tiles_++;
    this->have_a_ = false;
    return;
  }

  float re[DSP_ROWS], im[DSP_ROWS];
  memcpy(re, this->a_re_, sizeof(this->a_re_));
  memcpy(im, this->a_im_, sizeof(this->a_im_));
  dsp_decode_tile(payload, re + DSP_ROWS_PER_TILE, im + DSP_ROWS_PER_TILE);

  this->raw_.write_phase(this->a_time_, re, im);

  // Hand the set to the analysis task rather than writing it directly: the
  // task owns the DSP ring while it is working on it.
  uint32_t w = this->staging_write_;
  uint32_t next = (w + 1) % STAGING_SLOTS;
  if (next == this->staging_read_) {
    this->staging_dropped_++;  // task has stalled; better to drop than corrupt
  } else {
    StagedSet &slot = this->staging_[w];
    slot.t_us = this->a_time_;
    memcpy(slot.re, re, sizeof(slot.re));
    memcpy(slot.im, im, sizeof(slot.im));
    this->staging_write_ = next;
  }

  this->have_a_ = false;
  this->tile_sets_++;

  this->rate_hist_[this->rate_head_] = this->a_time_;
  this->rate_head_ = (this->rate_head_ + 1) % RATE_HISTORY;
  if (this->rate_count_ < RATE_HISTORY)
    this->rate_count_++;
  if (this->rate_count_ >= 8) {
    int oldest = (this->rate_head_ - this->rate_count_ + RATE_HISTORY) % RATE_HISTORY;
    int newest = (this->rate_head_ - 1 + RATE_HISTORY) % RATE_HISTORY;
    double span = (double) (this->rate_hist_[newest] - this->rate_hist_[oldest]) / 1e6;
    if (span > 0.0)
      this->sample_rate_hz_ = (float) ((this->rate_count_ - 1) / span);
  }
}

// -------------------------------------------------------------------- loop

void MR60Breathing::loop() {
  // Drain the UART first and unconditionally. With 400 ms of slack there is no
  // room for this to wait behind anything else.
  int avail = this->available();
  while (avail > 0) {
    size_t space = RX_BUF_SIZE - this->rx_len_;
    if (space == 0) {
      /* Only reachable when more than a whole frame of noise sits in front of
       * us. Drop the first half so resynchronisation can happen at all. */
      this->overflows_++;
      this->resync_bytes_ += RX_BUF_SIZE / 2;
      memmove(this->rx_buf_, this->rx_buf_ + RX_BUF_SIZE / 2, RX_BUF_SIZE / 2);
      this->rx_len_ = RX_BUF_SIZE / 2;
      space = RX_BUF_SIZE / 2;
    }
    size_t want = ((size_t) avail < space) ? (size_t) avail : space;
    if (!this->read_array(this->rx_buf_ + this->rx_len_, want))
      break;
    this->rx_len_ += want;
    avail = this->available();
  }

  if (this->rx_len_ > 0) {
    size_t used = this->parse_buffer_(this->rx_len_);
    if (used > 0) {
      this->rx_len_ -= used;
      if (this->rx_len_ > 0)
        memmove(this->rx_buf_, this->rx_buf_ + used, this->rx_len_);
    }
  }

  this->raw_.loop();

  const uint32_t now = millis();

  // Collection mode is a RAM byte; a radar reset silently reverts to normal
  // reporting, and the only symptom is that tiles stop arriving.
  int64_t since_tile_ms = (esp_timer_get_time() - this->last_tile_us_) / 1000;
  if (since_tile_ms > (int64_t) COLLECTION_RETRY_MS &&
      (now - this->last_collection_cmd_ms_) > COLLECTION_RETRY_MS) {
    ESP_LOGW(TAG, "no tiles for %lld ms, re-enabling collection mode",
             (long long) since_tile_ms);
    this->sample_rate_hz_ = NAN;
    this->rate_count_ = 0;
    this->rate_head_ = 0;
    this->enable_collection_();
  }

  // Entity publishing happens here, on the main loop: ESPHome's API and
  // logging are not safe to call from an arbitrary task.
  if (this->result_pending_) {
    dsp_result_t r = this->result_;
    this->result_pending_ = false;
    this->publish_result_(r);
  }

  if (now - this->last_diag_ms_ >= DIAG_PERIOD_MS) {
    this->last_diag_ms_ = now;
    this->publish_diagnostics_();
  }
}

void MR60Breathing::publish_diagnostics_() {
  uint32_t errors = this->header_errors_ + this->payload_errors_ +
                    this->unpaired_tiles_ + this->overflows_ +
                    this->staging_dropped_;

  float buffered = 0.0f;
  if (this->dsp_.count > 1 && !std::isnan(this->sample_rate_hz_))
    buffered = (float) this->dsp_.count / this->sample_rate_hz_;

  ESP_LOGD(TAG,
           "sets=%" PRIu32 " rate=%.3f Hz buffered=%.0f s | frames=%" PRIu32
           " hdr_err=%" PRIu32 " pl_err=%" PRIu32 " unpaired=%" PRIu32
           " resync=%" PRIu32 " ovf=%" PRIu32 " raw=%s/%" PRIu32,
           this->tile_sets_, this->sample_rate_hz_, buffered, this->frames_ok_,
           this->header_errors_, this->payload_errors_, this->unpaired_tiles_,
           this->resync_bytes_, this->overflows_,
           this->raw_.has_client() ? "client" : "idle", this->raw_.dropped());

#ifdef USE_SENSOR
  if (this->sample_rate_sensor_ != nullptr)
    this->sample_rate_sensor_->publish_state(this->sample_rate_hz_);
  if (this->frame_errors_sensor_ != nullptr)
    this->frame_errors_sensor_->publish_state(errors);
  if (this->tile_sets_sensor_ != nullptr)
    this->tile_sets_sensor_->publish_state(this->tile_sets_);
  if (this->buffered_seconds_sensor_ != nullptr)
    this->buffered_seconds_sensor_->publish_state(buffered);
#endif
}

// ----------------------------------------------------------- analysis task

void MR60Breathing::drain_staging_() {
  while (this->staging_read_ != this->staging_write_) {
    const StagedSet &slot = this->staging_[this->staging_read_];
    dsp_push(&this->dsp_, slot.t_us, slot.re, slot.im);
    this->staging_read_ = (this->staging_read_ + 1) % STAGING_SLOTS;
  }
}

void MR60Breathing::analysis_loop() {
  const TickType_t poll = pdMS_TO_TICKS(200);
  int64_t last_run_us = 0;

  for (;;) {
    this->drain_staging_();

    float interval = this->update_interval_s_;
    int64_t now = esp_timer_get_time();
    bool due = (interval <= 0.0f) ||
               ((now - last_run_us) >= (int64_t) (interval * 1e6f));

    if (due && this->dsp_.count >= 32 && !this->result_pending_) {
      dsp_result_t r;
      int64_t t0 = esp_timer_get_time();
      dsp_analyze(&this->dsp_, &this->cfg_, &r);
      int64_t cost = esp_timer_get_time() - t0;
      last_run_us = esp_timer_get_time();

      this->result_ = r;
      this->result_pending_ = true;

      ESP_LOGD(TAG, "analysis took %lld ms -> %s %.1f /min",
               (long long) (cost / 1000), dsp_status_name(r.status),
               r.rate_bpm);
    }

    vTaskDelay(poll);
  }
}

void MR60Breathing::publish_result_(const dsp_result_t &r) {
  /*
   * A rate is published only when the verdict holds. Otherwise NAN goes out,
   * which Home Assistant renders as "unknown".
   *
   * Publishing 0 instead would be worse than useless: it is a plausible
   * number, it would land in history graphs and long-term statistics, and no
   * automation could tell it apart from a real reading. The status text sensor
   * carries the reason, which a 0 never could.
   */
#ifdef USE_SENSOR
  if (this->rate_sensor_ != nullptr)
    this->rate_sensor_->publish_state(r.stable ? r.rate_bpm : NAN);
  if (this->rate_td_sensor_ != nullptr)
    this->rate_td_sensor_->publish_state(r.stable ? r.rate_td_bpm : NAN);
  if (this->stability_sensor_ != nullptr)
    this->stability_sensor_->publish_state(r.stability_pct);
  if (this->snr_sensor_ != nullptr)
    this->snr_sensor_->publish_state(r.snr_db);
  if (this->depth_sensor_ != nullptr)
    this->depth_sensor_->publish_state(r.depth_um);
  if (this->motion_sensor_ != nullptr)
    this->motion_sensor_->publish_state(r.motion_ratio);
#endif
#ifdef USE_BINARY_SENSOR
  if (this->breathing_binary_sensor_ != nullptr)
    this->breathing_binary_sensor_->publish_state(r.stable != 0);
#endif
#ifdef USE_TEXT_SENSOR
  if (this->status_text_sensor_ != nullptr)
    this->status_text_sensor_->publish_state(dsp_status_name(r.status));
#endif
}

// ---------------------------------------------------------------- setters

void MR60Breathing::set_window_s(float v) { this->cfg_.window_s = v; }
void MR60Breathing::set_band_min_bpm(float v) { this->cfg_.band_lo_hz = v / 60.0f; }
void MR60Breathing::set_band_max_bpm(float v) { this->cfg_.band_hi_hz = v / 60.0f; }
void MR60Breathing::set_highpass_hz(float v) { this->cfg_.highpass_hz = v; }
void MR60Breathing::set_stability_window_s(float v) {
  this->cfg_.stability_window_s = v;
}
void MR60Breathing::set_stability_tolerance_bpm(float v) {
  this->cfg_.stability_tol_bpm = v;
}
void MR60Breathing::set_stability_threshold_pct(float v) {
  this->cfg_.stability_threshold_pct = v;
}
void MR60Breathing::set_min_snr_db(float v) { this->cfg_.min_snr_db = v; }
void MR60Breathing::set_min_depth_um(float v) { this->cfg_.min_depth_um = v; }
void MR60Breathing::set_max_motion_ratio(float v) {
  this->cfg_.max_motion_ratio = v;
}
void MR60Breathing::set_range_rows(int lo, int hi) {
  this->cfg_.row_min = lo;
  this->cfg_.row_max = hi;
}

}  // namespace mr60_breathing
}  // namespace esphome
