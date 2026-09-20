#pragma once

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include "cJSON.h"

using JsonDocument = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

inline JsonDocument parseJson(const char *text) {
  return JsonDocument(cJSON_ParseWithOpts(text, nullptr, true), cJSON_Delete);
}

inline const cJSON *jsonField(const cJSON *object, const char *key) {
  return cJSON_GetObjectItemCaseSensitive(object, key);
}

inline const char *jsonText(const cJSON *object, const char *key) {
  const cJSON *item = jsonField(object, key);
  return cJSON_IsString(item) ? item->valuestring : "";
}

inline int64_t jsonInteger(const cJSON *object, const char *key) {
  const cJSON *item = jsonField(object, key);
  if (!cJSON_IsNumber(item) || item->valuedouble < -9007199254740991.0 ||
      item->valuedouble > 9007199254740991.0) return 0;
  const int64_t value = static_cast<int64_t>(item->valuedouble);
  return value == item->valuedouble ? value : 0;
}

inline bool telegramOk(const cJSON *document) {
  return cJSON_IsTrue(jsonField(document, "ok"));
}

inline bool telegramRejected(const cJSON *document) {
  const int64_t code = jsonInteger(document, "error_code");
  return cJSON_IsFalse(jsonField(document, "ok")) && code >= 400 && code < 500;
}

inline std::string quoteJson(const char *text) {
  JsonDocument value(cJSON_CreateString(text), cJSON_Delete);
  if (!value) return "\"\"";
  char *encoded = cJSON_PrintUnformatted(value.get());
  if (!encoded) return "\"\"";
  std::string result(encoded);
  cJSON_free(encoded);
  return result;
}

struct TelegramUpdate {
  int64_t id = 0;
  int64_t chat = 0;
  int message = 0;
  std::string text;
  std::string callback;
  std::string query;
};

inline TelegramUpdate decodeUpdate(const cJSON *object) {
  TelegramUpdate update;
  update.id = jsonInteger(object, "update_id");
  const cJSON *callback = jsonField(object, "callback_query");
  const cJSON *message = cJSON_IsObject(callback)
      ? jsonField(callback, "message") : jsonField(object, "message");
  update.chat = jsonInteger(jsonField(message, "chat"), "id");
  const int64_t messageId = jsonInteger(message, "message_id");
  if (messageId > 0 && messageId <= INT32_MAX) update.message = int(messageId);
  if (cJSON_IsObject(callback)) {
    update.callback = jsonText(callback, "data");
    update.query = jsonText(callback, "id");
  } else {
    update.text = jsonText(message, "text");
  }
  return update;
}

inline std::string normalizeCommand(const std::string &text, const std::string &bot) {
  const size_t end = text.find(' ');
  const size_t at = text.find('@');
  if (at == std::string::npos || at > end) return text;
  std::string target = text.substr(at + 1, end - at - 1);
  std::string username = bot;
  for (char &c : target) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  for (char &c : username) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  if (username.empty() || target != username) return "";
  return text.substr(0, at) + (end == std::string::npos ? "" : text.substr(end));
}

class HttpResponse {
 public:
  enum class State { Reading, Complete, Invalid };
  State state = State::Reading;
  int status = 0;
  std::string body;

  void feed(char c) {
    if (state != State::Reading) return;
    if (++bytes > 40000) { state = State::Invalid; return; }
    if (!headersDone && bytes > 4096) { state = State::Invalid; return; }
    if (!headersDone || (chunked && chunkRemaining == 0)) {
      line += c;
      if (line.size() > 4096) { state = State::Invalid; return; }
      if (c == '\n') consumeLine();
      return;
    }
    if (chunked && chunkEnding > 0) {
      const char expected = chunkEnding == 2 ? '\r' : '\n';
      if (c != expected) { state = State::Invalid; return; }
      if (--chunkEnding == 0) chunkRemaining = 0;
      return;
    }
    if (body.size() >= 32768) { state = State::Invalid; return; }
    body += c;
    if (chunked) {
      if (--chunkRemaining == 0) {
        chunkRemaining = -1;
        chunkEnding = 2;
      }
    } else if (contentLength >= 0 && int(body.size()) == contentLength) {
      state = State::Complete;
    }
  }

  void close() {
    if (state == State::Reading) {
      state = headersDone && !chunked && contentLength < 0
          ? State::Complete : State::Invalid;
    }
  }

 private:
  std::string line;
  size_t bytes = 0;
  bool headersDone = false;
  bool gotStatus = false;
  bool chunked = false;
  bool trailers = false;
  int contentLength = -1;
  int chunkRemaining = 0;
  int chunkEnding = 0;

  void consumeLine() {
    if (line.size() < 2 || line.substr(line.size() - 2) != "\r\n") {
      state = State::Invalid;
      return;
    }
    line.resize(line.size() - 2);
    if (!headersDone) {
      if (!gotStatus) {
        if ((line.compare(0, 9, "HTTP/1.1 ") != 0 &&
             line.compare(0, 9, "HTTP/1.0 ") != 0) ||
            line.size() < 12 || line[9] < '1' || line[9] > '5' ||
            line[10] < '0' || line[10] > '9' || line[11] < '0' || line[11] > '9' ||
            (line.size() > 12 && line[12] != ' ')) {
          state = State::Invalid;
        } else {
          status = std::atoi(line.c_str() + 9);
          gotStatus = true;
        }
      } else if (line.empty()) {
        headersDone = true;
        if (chunked && contentLength >= 0) state = State::Invalid;
        else if (!chunked && contentLength == 0) state = State::Complete;
      } else {
        std::string lower = line;
        for (char &c : lower) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        while (!lower.empty() && (lower.back() == ' ' || lower.back() == '\t'))
          lower.pop_back();
        if (lower.compare(0, 15, "content-length:") == 0) {
          char *end = nullptr;
          const char *start = lower.c_str() + 15;
          const long length = std::strtol(start, &end, 10);
          if (end == start || *end || length < 0 || length > 32768 ||
              contentLength >= 0) state = State::Invalid;
          else contentLength = int(length);
        }
        if (lower.compare(0, 18, "transfer-encoding:") == 0) {
          const size_t value = lower.find_first_not_of(" \t", 18);
          if (chunked || value == std::string::npos || lower.substr(value) != "chunked")
            state = State::Invalid;
          else chunked = true;
        }
      }
    } else if (trailers) {
      if (line.empty()) state = State::Complete;
    } else {
      char *end = nullptr;
      const char *start = line.c_str();
      const long length = std::strtol(start, &end, 16);
      if (end == start || (*end && *end != ';') || length < 0 ||
          length > 32768 || body.size() + length > 32768) state = State::Invalid;
      else if (length == 0) trailers = true;
      else chunkRemaining = int(length);
    }
    line.clear();
  }
};
