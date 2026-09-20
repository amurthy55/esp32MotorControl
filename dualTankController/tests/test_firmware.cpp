#include "host_types.h"
#include "firmware.inc"

void reset() {
  clockMs = 10000;
  feedback = {false, 0, 0};
  duringDelay = nullptr;
  motorRun = MotorRun{};
  valveState = ValveRecord{};
  prefs = FakePreferences{};
  WiFi.connected = true;
  registeredChatId = -123;
  valveStateDirty = false;
  valveReminderDueMillis = 0;
  lastValveSaveAttempt = 0;
  relayInProgress = false;
  motorStartMillis = 0;
  longRunReported = false;
  telegramRequestWritten = false;
  lastUpdateId = 0;
  acceptMotorCommands = false;
  lastTelegramPoll = 0;
  lastSuccessfulPoll = 0;
  dispatched.clear();
  requests.clear();
  responses.clear();
  messages.clear();
  outputs.clear();
  outputs[TANK_SELECTOR_PIN] = HIGH;
  relayPulses = 0;
  schedCount = 0;
  localTime = {};
}

void success(int id = 501) {
  responses.push_back({String("{\"ok\":true,\"result\":{\"message_id\":") + String(id) + "}}", true});
}

void due() {
  clockMs = valveReminderDueMillis;
  checkValveReminder();
}

int main() {
  reset();
  checkACFeedback();
  assert(messages.empty() && prefs.blobs.empty());
  startTankMotorFlow(true, registeredChatId);
  assert(relayPulses == 1 && !motorRun.active && outputs[TANK_SELECTOR_PIN] == HIGH);
  assert(valveState.lastFull == 0);
  startTankMotorFlow(false, registeredChatId);
  assert(relayPulses == 2 && !motorRun.active);
  std::cout << "PASS failed GF/FF starts release lock without phantom full\n";

  reset();
  duringDelay = [] {
    if (relayPulses) feedback = {true, clockMs, 1};
  };
  startTankMotorFlow(true, registeredChatId);
  duringDelay = nullptr;
  assert(motorRun.active && motorStartMillis);
  feedback = {false, clockMs, 1};
  delay(2000);
  checkACFeedback();
  assert(!motorRun.active && valveState.lastFull == TANK_POSITION_GF &&
         valveState.pending && outputs[TANK_SELECTOR_PIN] == HIGH);
  assert(prefs.getBytesLength("valve_state") == sizeof(ValveRecord));
  std::cout << "PASS startup-confirmed run remains tracked before first feedback-loop LOW\n";

  reset();
  feedback = {true, clockMs, 1};
  checkACFeedback();
  startTankMotorFlow(true, registeredChatId);
  assert(relayPulses == 0 && outputs[TANK_SELECTOR_PIN] == HIGH);
  feedback = {false, clockMs, 1};
  delay(1000);
  startTankMotorFlow(true, registeredChatId);
  assert(relayPulses == 0);
  schedCount = 1;
  schedules[0] = {0, 0, 0};
  lastFiredYday[0] = -1;
  checkScheduledMotorStart();
  assert(relayPulses == 0);
  delay(1000);
  checkACFeedback();
  assert(valveState.lastFull == TANK_POSITION_FF && !valveState.pending);
  std::cout << "PASS manual FF blocks Telegram and schedule through pending completion\n";

  reset();
  duringDelay = [] { feedback = {true, clockMs, 1}; };
  startTankMotorFlow(true, registeredChatId);
  duringDelay = nullptr;
  assert(relayPulses == 0 && motorRun.active);
  feedback = {false, clockMs, 1};
  delay(2000);
  checkACFeedback();
  assert(!motorRun.active && outputs[TANK_SELECTOR_PIN] == HIGH);
  std::cout << "PASS run arriving during selection is tracked without an extra relay pulse\n";

  reset();
  schedCount = 1;
  schedules[0] = {0, 0, 0};
  lastFiredYday[0] = -1;
  outputs[TANK_SELECTOR_PIN] = LOW;
  checkScheduledMotorStart();
  assert(relayPulses == 1 && outputs[TANK_SELECTOR_PIN] == HIGH);
  checkScheduledMotorStart();
  assert(relayPulses == 1);
  prefs.getBytes("sched_fired", lastFiredYday, sizeof(lastFiredYday));
  checkScheduledMotorStart();
  assert(relayPulses == 1);
  reset();
  schedCount = 1;
  schedules[0] = {0, 0, 0};
  lastFiredYday[0] = -1;
  prefs.fail = true;
  checkScheduledMotorStart();
  assert(relayPulses == 0 && lastFiredYday[0] == -1);
  std::cout << "PASS schedules explicitly select FF, persist firing, and fail closed on NVS error\n";

  reset();
  recordTankFull(true);
  assert(valveState.pending && valveState.lastFull == TANK_POSITION_GF);
  assert(relayPulses == 0);
  clockMs = valveReminderDueMillis - 1;
  checkValveReminder();
  assert(requests.empty());
  success();
  due();
  assert(valveState.message == 501 && requests.back().first == "sendMessage");
  const uint32_t originalCycle = valveState.cycle;
  success();
  due();
  assert(requests.size() == 2 && requests.back().first == "editMessageText");
  valveState = ValveRecord{};
  loadValveReminderState();
  assert(valveState.pending && valveState.message == 501 &&
         valveState.cycle == originalCycle);
  assert(isCurrentValveReminder(-123, 501, "VALVE_FF:" + String(originalCycle)));
  assert(!isCurrentValveReminder(-999, 501, "VALVE_FF:" + String(originalCycle)));
  assert(!isCurrentValveReminder(-123, 999, "VALVE_FF:" + String(originalCycle)));
  recordTankFull(true);
  assert(!isCurrentValveReminder(-123, 501, "VALVE_FF:" + String(originalCycle)));
  assert(isCurrentValveReminder(-123, 501, "VALVE_FF:" + String(valveState.cycle)));
  std::cout << "PASS reminder timing, single-message edits, reload, chat/message/cycle validation\n";

  WiFi.connected = false;
  updateValvePosition(TANK_POSITION_FF, false);
  assert(!valveState.pending && valveState.cleanup && valveState.lastFull == TANK_POSITION_GF);
  due();
  assert(valveState.message == 501 && valveState.cleanup);
  valveState = ValveRecord{};
  loadValveReminderState();
  assert(valveState.cleanup && valveState.message == 501);
  WiFi.connected = true;
  success();
  due();
  assert(!valveState.cleanup && valveState.message == 501);
  assert(requests.back().second.value.find("\"inline_keyboard\":[]") != std::string::npos);
  recordTankFull(true);
  success();
  due();
  assert(requests.back().first == "editMessageText");
  std::cout << "PASS offline completion retains cleanup across reboot and reuses the same message\n";

  reset();
  recordTankFull(true);
  for (int i = 0; i < 5; i++) {
    responses.push_back({"", false});
    due();
  }
  assert(valveState.publishAttempts == 0);
  responses.push_back({"", true});
  due();
  assert(valveState.publishAttempts == 1 && valveState.message == 0);
  assert(isCurrentValveReminder(-123, 800, "VALVE_FF:" + String(valveState.cycle)));
  responses.push_back({"", true});
  due();
  size_t sent = requests.size();
  due();
  assert(requests.size() == sent && valveState.publishAttempts == 2);
  std::cout << "PASS pre-send failures retry; uncertain sends are bounded and Yes can recover an unknown ID\n";

  reset();
  prefs.legacy = {{"last_full", 1}, {"valve_pos", 1}, {"valve_wait", 1}, {"valve_msg", 42}};
  loadValveReminderState();
  assert(valveState.pending && valveState.message == 42 && valveState.lastFull == 1);
  prefs.fail = true;
  updateValvePosition(TANK_POSITION_FF, false);
  assert(valveStateDirty);
  due();
  assert(requests.empty());
  prefs.fail = false;
  delay(1000);
  checkValveReminder();
  assert(!valveStateDirty);
  std::cout << "PASS legacy migration and NVS-write retry before network work\n";

  reset();
  recordTankFull(true);
  responses.push_back({"{\"unexpected\":\"reply\"}", true});
  due();
  assert(valveState.publishAttempts == 1);
  responses.push_back({"{\"ok\":false,\"error_code\":429,\"description\":\"retry later\"}", true});
  due();
  assert(valveState.publishAttempts == 1);
  success();
  due();
  responses.push_back({"{\"ok\":false,\"error_code\":400,\"description\":\"Bad Request: message to edit not found\"}", true});
  due();
  assert(valveState.message == 0 && valveState.publishAttempts == 0);
  success(502);
  due();
  assert(valveState.message == 502);
  std::cout << "PASS ambiguous replies count, definite rejections retry, deleted messages recover\n";

  reset();
  recordTankFull(true);
  success();
  due();
  registeredChatId = -456;
  responses.push_back({"", false});
  due();
  assert(valveState.cleanup && valveState.message == 501 && valveState.chat == -123);
  success();
  due();
  assert(!valveState.cleanup && valveState.message == 0 && valveState.chat == -456);
  assert(valveState.pending);
  std::cout << "PASS registration change retains old-chat cleanup until it succeeds\n";

  reset();
  responses.push_back({R"({"ok":true,"result":[
    {"update_id":1,"callback_query":{"id":"q1","data":"MOTOR_ON_GF","message":{"message_id":77,"chat":{"id":-123}}}},
    {"update_id":2,"message":{"message_id":78,"chat":{"id":-456},"text":"/commands"}}
  ]})", true});
  pollTelegram();
  assert(lastUpdateId == 2 && prefs.legacy["tg_update"] == 2 && dispatched.size() == 2);
  assert(dispatched[0].first.chat == -123 && dispatched[1].first.chat == -456);
  assert(!dispatched[0].second && !dispatched[1].second);
  delay(1000);
  responses.push_back({"{\"ok\":true,\"result\":[]}", true});
  pollTelegram();
  assert(acceptMotorCommands);
  delay(1000);
  responses.push_back({R"({"ok":true,"result":[{"update_id":3,"message":{"chat":{"id":-123},"text":"/commands"}}]})", true});
  pollTelegram();
  assert(lastUpdateId == 3 && dispatched.back().second);
  delay(1000);
  responses.push_back({"{\"ok\":true,\"result\":[{\"update_id\":4", true});
  pollTelegram();
  assert(lastUpdateId == 3 && dispatched.size() == 3);
  delay(1000);
  prefs.fail = true;
  responses.push_back({R"({"ok":true,"result":[{"update_id":4,"message":{"chat":{"id":-123},"text":"/commands"}}]})", true});
  pollTelegram();
  assert(lastUpdateId == 3 && dispatched.size() == 3);
  prefs.fail = false;
  delay(31000);
  responses.push_back({R"({"ok":true,"result":[{"update_id":4,"message":{"chat":{"id":-123},"text":"/commands"}}]})", true});
  pollTelegram();
  assert(lastUpdateId == 4 && !dispatched.back().second);
  std::cout << "PASS actual polling: ordered updates, persisted offsets, incomplete replies, storage errors, reconnect gating\n";
}
