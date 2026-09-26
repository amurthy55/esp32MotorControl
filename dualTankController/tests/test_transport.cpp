#include "../TelegramTransport.h"

const std::string complete =
    "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n{\"ok\":true}";

int main() {
  network = FakeNetwork{};
  network.wire = complete;
  TelegramTransport successful;
  assert(successful.post("test-token", "sendMessage", "{}") == "{\"ok\":true}");
  assert(successful.written && network.stopped);
  assert(network.socketTimeout == 1500 && network.tlsTimeout == 3);

  network = FakeNetwork{};
  network.wire = complete;
  network.interval = 500;
  TelegramTransport drip;
  uint32_t started = millis();
  assert(drip.post("test-token", "sendMessage", "{}") == "");
  assert(uint32_t(millis() - started) == 5000 && network.stopped && drip.written);

  network = FakeNetwork{};
  network.wire = "HTTP/1.1 200 OK\r\nContent-Length: 11\r\n\r\n{\"ok\":";
  network.closeAfterBody = false;
  TelegramTransport truncated;
  started = millis();
  assert(truncated.post("test-token", "sendMessage", "{}") == "");
  assert(uint32_t(millis() - started) == 5000);

  network = FakeNetwork{};
  network.dnsReady = false;
  TelegramTransport dnsTimeout;
  started = millis();
  assert(dnsTimeout.post("test-token", "sendMessage", "{}") == "");
  assert(uint32_t(millis() - started) == 2000 && !dnsTimeout.written);
  ip_addr_t lateAddress{123};
  dnsCallback(nullptr, &lateAddress, dnsArgument);
  network.wire = complete;
  assert(dnsTimeout.post("test-token", "sendMessage", "{}") == "{\"ok\":true}");

  network = FakeNetwork{};
  network.connectOk = false;
  TelegramTransport connectionFailure;
  assert(connectionFailure.post("test-token", "sendMessage", "{}") == "");
  assert(!connectionFailure.written);

  network = FakeNetwork{};
  network.writeOk = false;
  TelegramTransport writeFailure;
  assert(writeFailure.post("test-token", "sendMessage", "{}") == "");
  assert(writeFailure.written && network.stopped);
  std::cout << "PASS transport: response deadlines, slow drip, truncation, delayed DNS, configured TLS/socket limits, write uncertainty\n";
}
