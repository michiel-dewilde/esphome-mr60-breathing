#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/components/uart/uart.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

extern "C" {
#include "dsp.h"
}

#include "raw_stream.h"

namespace esphome {
namespace mr60_breathing {

/*
 * Frame types. The module reuses type numbers per direction: 0x0A14 outbound
 * is "breathing rate", but inbound with an empty payload it means "enable
 * collection mode".
 */
static const uint16_t TYPE_TILE_A = 0x0A32;  // collection mode, channel A
static const uint16_t TYPE_TILE_C = 0x0A34;  // collection mode, channel C
static const uint16_t TYPE_BREATH_RATE = 0x0A14;
static const uint16_t TYPE_PRESENCE = 0x0F09;

static const size_t TILE_BYTES = 1024;

class MR60Breathing : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  // --- configuration, all runtime-settable so HA can expose them as knobs ---
  void set_window_s(float v);
  void set_band_min_bpm(float v);
  void set_band_max_bpm(float v);
  void set_highpass_hz(float v);
  void set_stability_window_s(float v);
  void set_stability_tolerance_bpm(float v);
  void set_stability_threshold_pct(float v);
  void set_min_snr_db(float v);
  void set_min_depth_um(float v);
  void set_max_motion_ratio(float v);
  void set_range_rows(int lo, int hi);
  void set_update_interval_s(float v) { this->update_interval_s_ = v; }

  void set_raw_port(uint16_t v) { this->raw_.set_port(v); }
  void set_raw_mode(int v) { this->raw_.set_mode((RawMode) v); }
  void set_raw_enabled(bool v) { this->raw_.set_enabled(v); }
  bool raw_enabled() const { return this->raw_.enabled(); }

#ifdef USE_SENSOR
  void set_breathing_rate_sensor(sensor::Sensor *s) { this->rate_sensor_ = s; }
  void set_rate_time_domain_sensor(sensor::Sensor *s) { this->rate_td_sensor_ = s; }
  void set_stability_sensor(sensor::Sensor *s) { this->stability_sensor_ = s; }
  void set_snr_sensor(sensor::Sensor *s) { this->snr_sensor_ = s; }
  void set_depth_sensor(sensor::Sensor *s) { this->depth_sensor_ = s; }
  void set_motion_sensor(sensor::Sensor *s) { this->motion_sensor_ = s; }
  void set_sample_rate_sensor(sensor::Sensor *s) { this->sample_rate_sensor_ = s; }
  void set_frame_errors_sensor(sensor::Sensor *s) { this->frame_errors_sensor_ = s; }
  void set_tile_sets_sensor(sensor::Sensor *s) { this->tile_sets_sensor_ = s; }
  void set_buffered_seconds_sensor(sensor::Sensor *s) {
    this->buffered_seconds_sensor_ = s;
  }
#endif
#ifdef USE_BINARY_SENSOR
  void set_breathing_binary_sensor(binary_sensor::BinarySensor *s) {
    this->breathing_binary_sensor_ = s;
  }
#endif
#ifdef USE_TEXT_SENSOR
  void set_status_text_sensor(text_sensor::TextSensor *s) {
    this->status_text_sensor_ = s;
  }
#endif

  // Called from the analysis task. Public only so the task trampoline can
  // reach it.
  void analysis_loop();

 protected:
  // --- radar link -----------------------------------------------------------
  void send_command_(const uint8_t *cmd, size_t len, const char *name);
  void enable_collection_();
  size_t parse_buffer_(size_t len);
  void handle_frame_(int64_t t_us, uint16_t type, const uint8_t *payload,
                     uint16_t len);
  void publish_diagnostics_();

  /*
   * Receive buffer. Must hold more than two complete frames: a collection-mode
   * frame is 8 header bytes plus a 1024-byte payload plus a checksum, and both
   * channels arrive back to back.
   *
   * Collection mode is close to the limit of the link. Two 1033-byte frames at
   * 4.88 sets per second is about 10.1 kB/s against a 11.52 kB/s UART, so 88 %
   * of capacity with roughly 400 ms of slack in this buffer. Anything that
   * blocks the main loop for longer than that loses data, which is why the
   * analysis does not run here.
   */
  static const size_t RX_BUF_SIZE = 4096;
  uint8_t rx_buf_[RX_BUF_SIZE];
  size_t rx_len_{0};

  // Pending channel-A tile, waiting for its channel-C partner.
  bool have_a_{false};
  int64_t a_time_{0};
  float a_re_[DSP_ROWS_PER_TILE];
  float a_im_[DSP_ROWS_PER_TILE];

  /* Scratch for assembling a complete set. Members rather than locals for the
   * same reason as the stream's line buffers: the main task's stack is small
   * and shared with everything ESPHome does. */
  float set_re_[DSP_ROWS];
  float set_im_[DSP_ROWS];

  // --- statistics -----------------------------------------------------------
  uint32_t frames_ok_{0};
  uint32_t header_errors_{0};
  uint32_t payload_errors_{0};
  uint32_t resync_bytes_{0};
  uint32_t overflows_{0};
  uint32_t tile_sets_{0};
  uint32_t unpaired_tiles_{0};
  uint32_t repeated_sets_{0};

  /* Previous decoded set, for the one check that catches a radar which keeps
   * streaming while its content has stopped changing. Every other test passes
   * in that state: the frames are well formed, the checksums are right and the
   * set rate is nominal. */
  bool have_prev_set_{false};
  float prev_re_[DSP_ROWS];
  float prev_im_[DSP_ROWS];

  // Rolling estimate of the tile-set rate, the health check on the UART.
  static const int RATE_HISTORY = 32;
  int64_t rate_hist_[RATE_HISTORY];
  int rate_count_{0};
  int rate_head_{0};
  float sample_rate_hz_{NAN};

  int64_t last_tile_us_{0};
  uint32_t last_collection_cmd_ms_{0};
  uint32_t last_diag_ms_{0};

  /* When the analysis task last took a sample out of the staging ring. Kept
   * here rather than read back from the ring, because declaring the history
   * stale is what clears the ring, and the two must not erase each other. */
  int64_t last_sample_us_{0};

  /* When an analysis result last reached the entities. The watchdog in loop()
   * measures from here: the analysis stopping while tiles keep arriving is a
   * failure the entities themselves cannot show, because they simply hold
   * their last values. */
  uint32_t last_result_ms_{0};

  void publish_result_(const dsp_result_t &r);
  void drain_staging_();

  /*
   * Staging ring between the UART loop and the analysis task.
   *
   * Only the main loop writes it and only the analysis task reads it, so a
   * single-producer single-consumer ring needs no lock. The point is that the
   * task owns dsp_state_ outright: an analysis takes about 450 ms, during
   * which two or three new tile sets arrive, and letting the loop write into
   * the ring being analysed would corrupt samples. The task drains this every
   * cycle, so it only ever has to hold a few hundred milliseconds of sets.
   */
  struct StagedSet {
    int64_t t_us;
    float re[DSP_ROWS];
    float im[DSP_ROWS];
  };
  static const int STAGING_SLOTS = 32;
  StagedSet staging_[STAGING_SLOTS];
  volatile uint32_t staging_write_{0};
  volatile uint32_t staging_read_{0};
  uint32_t staging_dropped_{0};

  // --- analysis -------------------------------------------------------------
  RawStream raw_;

  dsp_state_t dsp_{};
  dsp_config_t cfg_{};
  volatile float update_interval_s_{10.0f};
  dsp_result_t result_{};
  volatile bool result_pending_{false};
  bool task_started_{false};

#ifdef USE_SENSOR
  sensor::Sensor *rate_sensor_{nullptr};
  sensor::Sensor *rate_td_sensor_{nullptr};
  sensor::Sensor *stability_sensor_{nullptr};
  sensor::Sensor *snr_sensor_{nullptr};
  sensor::Sensor *depth_sensor_{nullptr};
  sensor::Sensor *motion_sensor_{nullptr};
  sensor::Sensor *sample_rate_sensor_{nullptr};
  sensor::Sensor *frame_errors_sensor_{nullptr};
  sensor::Sensor *tile_sets_sensor_{nullptr};
  sensor::Sensor *buffered_seconds_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *breathing_binary_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *status_text_sensor_{nullptr};
#endif
};

#ifdef USE_SWITCH
class RawCaptureSwitch : public Component, public switch_::Switch {
 public:
  void set_parent(MR60Breathing *parent) { this->parent_ = parent; }

  /*
   * Applying the restore mode needs an explicit setup(): without one the switch
   * never publishes an initial state at all.
   *
   * The published state is read back from the stream rather than assumed from
   * the requested one. An earlier version published what it had been asked for,
   * which drifted from reality the moment any path failed to apply - Home
   * Assistant reported the capture port off while it was serving connections.
   * Reporting the request instead of the result is how a control ends up lying
   * about what it controls, and no amount of care at the call sites fixes that;
   * only reading back does.
   */
  void setup() override {
    this->apply_(this->get_initial_state_with_restore_mode().value_or(true));
  }

 protected:
  void write_state(bool state) override { this->apply_(state); }

  void apply_(bool wanted) {
    if (this->parent_ == nullptr) {
      // Nothing to control yet; say so rather than inventing a state.
      this->publish_state(false);
      return;
    }
    this->parent_->set_raw_enabled(wanted);
    this->publish_state(this->parent_->raw_enabled());
  }

  MR60Breathing *parent_{nullptr};
};
#endif

}  // namespace mr60_breathing
}  // namespace esphome
