#pragma once
#include <algorithm>
#include "host_types.h"

struct FakeNetwork {
  uint32_t clock = 1;
  uint32_t interval = 0;
  uint32_t socketTimeout = 0;
  uint32_t tlsTimeout = 0;
  bool dnsReady = true;
  bool connectOk = true;
  bool writeOk = true;
  bool closeAfterBody = true;
  std::string wire;
  std::string request;
  bool stopped = false;
};
inline FakeNetwork network;
inline uint32_t millis() { return network.clock; }
inline void delay(uint32_t ms) { network.clock += ms; }
using portMUX_TYPE = int;
constexpr int portMUX_INITIALIZER_UNLOCKED = 0;
inline void portENTER_CRITICAL(int *) {}
inline void portEXIT_CRITICAL(int *) {}

struct IPAddress {
  uint32_t value;
  explicit IPAddress(uint32_t ip = 0) : value(ip) {}
};

class WiFiClientSecure {
 public:
  void setInsecure() {}
  void setConnectionTimeout(uint32_t ms) { network.socketTimeout = ms; }
  void setHandshakeTimeout(uint32_t seconds) { network.tlsTimeout = seconds; }
  bool connect(IPAddress, int, const char *, const char *, const char *, const char *) {
    began = millis();
    return network.connectOk;
  }
  size_t write(const uint8_t *buffer, size_t size) {
    network.request.assign(reinterpret_cast<const char *>(buffer), size);
    return network.writeOk ? size : 0;
  }
  int available() {
    if (offset == network.wire.size()) return 0;
    if (network.interval)
      return uint32_t(millis() - began) >= (offset + 1) * network.interval ? 1 : 0;
    return network.wire.size() - offset;
  }
  int read(uint8_t *buffer, size_t size) {
    size_t count = std::min(size, size_t(available()));
    std::memcpy(buffer, network.wire.data() + offset, count);
    offset += count;
    return count;
  }
  bool connected() { return !network.closeAfterBody || offset < network.wire.size(); }
  void stop() { network.stopped = true; }

 private:
  uint32_t began = 0;
  size_t offset = 0;
};
