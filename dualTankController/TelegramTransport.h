#pragma once

#include <WiFiClientSecure.h>
#include <lwip/dns.h>
#include <lwip/tcpip.h>
#include "TelegramProtocol.h"

class TelegramTransport {
 public:
  bool written = false;

  String post(const String &token, const String &method, const String &body) {
    written = false;
    IPAddress address;
    if (!lookup(address)) return "";
    WiFiClientSecure connection;
    connection.setInsecure();
    connection.setConnectionTimeout(1500);
    connection.setHandshakeTimeout(3);
    if (!connection.connect(address, 443, "api.telegram.org", nullptr, nullptr, nullptr))
      return "";
    String request = "POST /bot" + token + "/" + method + " HTTP/1.1\r\n"
        "Host: api.telegram.org\r\nContent-Type: application/json\r\n"
        "Connection: close\r\nContent-Length: " + String(body.length()) + "\r\n\r\n" + body;
    written = true;
    size_t sent = connection.write(
        reinterpret_cast<const uint8_t *>(request.c_str()), request.length());
    if (sent != request.length()) {
      connection.stop();
      return "";
    }
    HttpResponse response;
    uint32_t started = millis();
    while (response.state == HttpResponse::State::Reading &&
           uint32_t(millis() - started) < 5000) {
      int available = connection.available();
      if (available > 0) {
        uint8_t buffer[256];
        size_t count = available < 256 ? available : 256;
        int received = connection.read(buffer, count);
        for (int i = 0; i < received; i++) response.feed(char(buffer[i]));
      } else if (!connection.connected()) {
        response.close();
      } else {
        delay(1);
      }
    }
    connection.stop();
    if (response.state != HttpResponse::State::Complete) return "";
    JsonDocument document = parseJson(response.body.c_str());
    if (!document || !cJSON_IsObject(document.get())) return "";
    if (response.status < 200 || response.status >= 500) return "";
    return String(response.body.c_str());
  }

 private:
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  bool pending = false;
  bool ready = false;
  uint32_t ipv4 = 0;
  uint32_t resolvedAt = 0;

  static void resolved(const char *, const ip_addr_t *address, void *argument) {
    auto *self = static_cast<TelegramTransport *>(argument);
    portENTER_CRITICAL(&self->mux);
    self->ready = address && IP_IS_V4(address);
    if (self->ready) self->ipv4 = ip4_addr_get_u32(ip_2_ip4(address));
    self->resolvedAt = millis();
    self->pending = false;
    portEXIT_CRITICAL(&self->mux);
  }

  static void resolve(void *argument) {
    ip_addr_t address;
    err_t result = dns_gethostbyname_addrtype(
        "api.telegram.org", &address, resolved, argument, LWIP_DNS_ADDRTYPE_IPV4);
    if (result == ERR_OK) resolved(nullptr, &address, argument);
    else if (result != ERR_INPROGRESS) resolved(nullptr, nullptr, argument);
  }

  bool lookup(IPAddress &address) {
    portENTER_CRITICAL(&mux);
    if (ready && uint32_t(millis() - resolvedAt) >= 300000) ready = false;
    bool launch = !ready && !pending;
    if (launch) pending = true;
    portEXIT_CRITICAL(&mux);
    if (launch && tcpip_try_callback(resolve, this) != ERR_OK) {
      resolved(nullptr, nullptr, this);
      return false;
    }
    uint32_t started = millis();
    while (uint32_t(millis() - started) < 2000) {
      portENTER_CRITICAL(&mux);
      bool done = !pending;
      bool found = ready;
      uint32_t value = ipv4;
      portEXIT_CRITICAL(&mux);
      if (done) {
        if (found) address = IPAddress(value);
        return found;
      }
      delay(1);
    }
    return false;
  }
};
