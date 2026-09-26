#pragma once

constexpr int LOW = 0, HIGH = 1, WL_CONNECTED = 3;
uint32_t clockMs = 10000;
unsigned long millis() { return clockMs; }
FeedbackSnapshot feedback{false, 0, 0};
std::function<void()> duringDelay;
void delay(unsigned long ms) {
  clockMs += ms;
  if (duringDelay) duringDelay();
}
FeedbackSnapshot readFeedback() { return feedback; }
struct FakeWiFi {
  bool connected = true;
  int status() { return connected ? WL_CONNECTED : 0; }
} WiFi;
struct FakeSerial {
  void println(const char *) {}
} Serial;
FakePreferences prefs;
MotorRun motorRun;
ValveRecord valveState;
bool valveStateDirty = false;
unsigned long lastValveSaveAttempt = 0;
unsigned long valveReminderDueMillis = 0;
unsigned long motorStartMillis = 0;
bool longRunReported = false;
bool relayInProgress = false;
int64_t registeredChatId = -123;
bool telegramRequestWritten = false;
int64_t lastUpdateId = 0;
String botUsername = "PumpBot";
bool acceptMotorCommands = false;
unsigned long lastTelegramPoll = 0;
unsigned long lastSuccessfulPoll = 0;
std::vector<std::pair<TelegramUpdate, bool>> dispatched;
void handleTelegramUpdate(const TelegramUpdate &update) {
  dispatched.emplace_back(update, acceptMotorCommands);
}
ScheduleEntry schedules[MAX_SCHEDULES];
int lastFiredYday[MAX_SCHEDULES];
int schedCount = 0;
std::tm localTime{};
bool getLocalTime(std::tm *out, unsigned long) { *out = localTime; return true; }
std::map<int, int> outputs;
std::vector<String> messages;
std::vector<std::pair<String, String>> requests;
std::deque<std::pair<String, bool>> responses;
int relayPulses = 0;
void digitalWrite(int pin, int value) {
  outputs[pin] = value;
  if (pin == RELAY_PIN && value == LOW) relayPulses++;
}
void sendTelegram(int64_t, const String &text) { messages.push_back(text); }
String telegramPostJson(const String &method, const String &body) {
  telegramRequestWritten = false;
  if (!WiFi.connected) return "";
  requests.emplace_back(method, body);
  if (responses.empty()) return "";
  auto response = responses.front();
  responses.pop_front();
  telegramRequestWritten = response.second;
  return response.first;
}
