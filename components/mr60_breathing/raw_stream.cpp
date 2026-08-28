#include "raw_stream.h"

#include "esphome/components/network/util.h"
#include "esphome/core/log.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome {
namespace mr60_breathing {

static const char *const TAG = "mr60_breathing.raw";

static const char HEX[] = "0123456789ABCDEF";

void RawStream::setup() {
  /* Deliberately does not bind here. Component setup runs before the network
   * stack is up, and binding that early cost several boot failures and put the
   * device into safe mode. The listener is opened from loop() once the network
   * reports itself connected, and reopened if the connection is lost. */
}

void RawStream::set_enabled(bool enabled) {
  if (enabled == this->enabled_)
    return;
  this->enabled_ = enabled;
  if (enabled) {
    if (network::is_connected())
      this->open_listener_();  // otherwise loop() picks it up
  } else {
    this->close_listener_();
  }
}

void RawStream::open_listener_() {
  if (this->listener_ != nullptr || this->port_ == 0)
    return;

  this->listener_ = socket::socket_ip(SOCK_STREAM, 0);
  if (this->listener_ == nullptr) {
    ESP_LOGE(TAG, "could not create the listening socket");
    return;
  }

  int one = 1;
  this->listener_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  this->listener_->setblocking(false);

  struct sockaddr_storage addr;
  socklen_t len = socket::set_sockaddr_any((struct sockaddr *) &addr,
                                           sizeof(addr), this->port_);
  if (this->listener_->bind((struct sockaddr *) &addr, len) != 0 ||
      this->listener_->listen(1) != 0) {
    ESP_LOGE(TAG, "could not listen on port %u", (unsigned) this->port_);
    this->listener_ = nullptr;
    return;
  }
  ESP_LOGI(TAG, "raw capture listening on port %u (%s mode)",
           (unsigned) this->port_,
           this->mode_ == RAW_MODE_TILES ? "tiles" : "phase");
}

void RawStream::close_listener_() {
  if (this->client_ != nullptr) {
    this->client_->close();
    this->client_ = nullptr;
  }
  if (this->listener_ != nullptr) {
    this->listener_->close();
    this->listener_ = nullptr;
  }
  ESP_LOGI(TAG, "raw capture stopped");
}

void RawStream::loop() {
  if (this->listener_ == nullptr) {
    if (this->enabled_ && this->port_ != 0 && network::is_connected())
      this->open_listener_();
    return;
  }

  struct sockaddr_storage addr;
  socklen_t len = sizeof(addr);
  auto sock = this->listener_->accept((struct sockaddr *) &addr, &len);
  if (sock != nullptr) {
    if (this->client_ != nullptr) {
      // One at a time. Refuse rather than queue, so a forgotten session cannot
      // silently block a new one - the second client sees an immediate close
      // instead of a connection that never delivers anything.
      ESP_LOGW(TAG, "refusing a second client");
      sock->close();
    } else {
      sock->setblocking(false);
      this->client_ = std::move(sock);
      this->dropped_ = 0;

      char *hdr = this->line_buf_;
      int n = snprintf(hdr, sizeof(this->line_buf_),
                       "#mr60raw stream=1 mode=%s rows=%d\n",
                       this->mode_ == RAW_MODE_TILES ? "tiles" : "phase",
                       DSP_ROWS);
      this->send_(hdr, (size_t) n);
      ESP_LOGI(TAG, "raw capture client connected");
    }
  }

  if (this->client_ != nullptr) {
    // A client that has gone away shows up as a read of zero bytes. We never
    // expect input, so anything readable is either a disconnect or noise.
    uint8_t scratch[32];
    ssize_t r = this->client_->read(scratch, sizeof(scratch));
    if (r == 0) {
      ESP_LOGI(TAG, "raw capture client disconnected (%" PRIu32 " lines dropped)",
               this->dropped_);
      this->client_->close();
      this->client_ = nullptr;
    }
  }
}

void RawStream::send_(const char *data, size_t len) {
  if (this->client_ == nullptr)
    return;
  ssize_t sent = this->client_->write(data, len);
  if (sent < 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      /* The radar does not wait for us. Dropping a line keeps the UART fed;
       * the count is reported so a gappy capture is never mistaken for a
       * complete one. */
      this->dropped_++;
      return;
    }
    ESP_LOGW(TAG, "raw capture write failed, closing");
    this->client_->close();
    this->client_ = nullptr;
  } else if ((size_t) sent != len) {
    this->dropped_++;
  }
}

void RawStream::write_phase(int64_t t_us, const float *re, const float *im) {
  if (this->client_ == nullptr || this->mode_ != RAW_MODE_PHASE)
    return;

  char *line = this->line_buf_;
  const int cap = (int) sizeof(this->line_buf_);
  int o = snprintf(line, cap, "%lld", (long long) t_us);
  for (int r = 0; r < DSP_ROWS && o > 0 && o < cap - 32; r++)
    o += snprintf(line + o, cap - o, ",%.0f,%.0f", re[r], im[r]);
  o += snprintf(line + o, cap - o, "\n");
  this->send_(line, (size_t) o);
}

void RawStream::write_tile(int64_t t_us, uint16_t type, const uint8_t *payload,
                           uint16_t len) {
  if (this->client_ == nullptr || this->mode_ != RAW_MODE_TILES)
    return;

  /* One line per frame, offset 0, whole payload. The reassembler on the other
   * end accepts a single chunk that covers the payload just as happily as the
   * 64-byte chunks the USB bridge had to use. */
  char *head = this->head_buf_;
  int n = snprintf(head, sizeof(this->head_buf_), "R,%lld,%04X,%u,0,",
                   (long long) t_us, type, len);
  this->send_(head, (size_t) n);

  char *hex = this->hex_buf_;
  size_t o = 0;
  for (uint16_t i = 0; i < len; i++) {
    hex[o++] = HEX[payload[i] >> 4];
    hex[o++] = HEX[payload[i] & 0x0F];
    if (o >= sizeof(this->hex_buf_) - 2) {
      this->send_(hex, o);
      o = 0;
    }
  }
  if (o)
    this->send_(hex, o);
  this->send_("\n", 1);
}

}  // namespace mr60_breathing
}  // namespace esphome
