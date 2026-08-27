#pragma once

/*
 * Raw capture over TCP, so analysis can continue on recordings taken through a
 * deployed device rather than only on a USB-tethered bench.
 *
 * Two design choices worth stating.
 *
 * First, the wire format is not new. Both modes emit line formats that already
 * have parsers on the PC side, so a capture drops straight into the existing
 * tooling with nothing to write:
 *
 *   tiles - "R,<t_us>,<type>,<len>,<offset>,<hex>" lines, the same framing the
 *           original USB bridge produced. About 20 kB/s. Byte-for-byte the
 *           payload the radar sent, so nothing is lost.
 *   phase - "<t_us>,<re0>,<im0>,...,<re15>,<im15>" lines, the same CSV the
 *           regression fixtures use. About 330 B/s, and enough to reproduce
 *           every analysis this component performs.
 *
 * Second, it costs nothing when unused. Serialisation only happens while a
 * client is actually connected; with nobody attached the per-tile cost is a
 * single boolean test, and the only standing cost is one non-blocking accept
 * per loop. That matters here more than usual, because collection mode already
 * runs the radar UART at about 88 % of capacity.
 *
 * One client at a time. A second connection is accepted and closed
 * immediately rather than queued, so a forgotten session cannot silently
 * block a new one.
 */

#include <memory>

#include "esphome/components/socket/socket.h"
#include "esphome/core/helpers.h"

extern "C" {
#include "dsp.h"
}

namespace esphome {
namespace mr60_breathing {

enum RawMode {
  RAW_MODE_PHASE = 0,
  RAW_MODE_TILES = 1,
};

class RawStream {
 public:
  void set_port(uint16_t port) { this->port_ = port; }
  void set_mode(RawMode mode) { this->mode_ = mode; }
  void set_enabled(bool enabled);

  bool enabled() const { return this->enabled_; }
  bool has_client() const { return this->client_ != nullptr; }
  RawMode mode() const { return this->mode_; }
  uint32_t dropped() const { return this->dropped_; }

  void setup();
  void loop();

  /* Both return immediately when nobody is listening. */
  void write_phase(int64_t t_us, const float *re, const float *im);
  void write_tile(int64_t t_us, uint16_t type, const uint8_t *payload,
                  uint16_t len);

 protected:
  void open_listener_();
  void close_listener_();
  void send_(const char *data, size_t len);

  uint16_t port_{6060};
  RawMode mode_{RAW_MODE_PHASE};
  bool enabled_{true};

  std::unique_ptr<socket::Socket> listener_;
  std::unique_ptr<socket::Socket> client_;
  uint32_t dropped_{0};
};

}  // namespace mr60_breathing
}  // namespace esphome
