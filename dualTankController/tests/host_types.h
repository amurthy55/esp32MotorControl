#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include "../ControlState.h"
#include "../TelegramProtocol.h"

class String {
 public:
  std::string value;
  String() = default;
  String(const char *text) : value(text) {}
  String(std::string text) : value(std::move(text)) {}
  template<class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
  String(T number) : value(std::to_string(number)) {}
  int indexOf(const String &text, int from = 0) const {
    const size_t pos = value.find(text.value, from);
    return pos == std::string::npos ? -1 : int(pos);
  }
  size_t length() const { return value.size(); }
  const char *c_str() const { return value.c_str(); }
  friend String operator+(const String &a, const String &b) { return a.value + b.value; }
  friend bool operator==(const String &a, const String &b) { return a.value == b.value; }
};

struct FakePreferences {
  std::map<std::string, std::vector<uint8_t>> blobs;
  std::map<std::string, int64_t> legacy;
  bool fail = false;
  size_t putBytes(const char *key, const void *value, size_t size) {
    if (fail) return 0;
    const auto *bytes = static_cast<const uint8_t *>(value);
    blobs[key] = std::vector<uint8_t>(bytes, bytes + size);
    return size;
  }
  size_t getBytesLength(const char *key) { return blobs[key].size(); }
  size_t getBytes(const char *key, void *value, size_t size) {
    if (blobs[key].size() != size) return 0;
    std::memcpy(value, blobs[key].data(), size);
    return size;
  }
  int getInt(const char *key, int fallback) { return legacy.count(key) ? legacy[key] : fallback; }
  bool getBool(const char *key, bool fallback) { return getInt(key, fallback); }
  uint8_t getUChar(const char *key, uint8_t fallback) { return getInt(key, fallback); }
  size_t putLong64(const char *key, int64_t value) {
    if (fail) return 0;
    legacy[key] = value;
    return sizeof(value);
  }
};
