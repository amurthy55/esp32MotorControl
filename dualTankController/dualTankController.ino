#include "config.h"
#include "ControlState.h"
#include "TelegramProtocol.h"
#include "TelegramTransport.h"
#include <WiFi.h>

#include <WiFiClientSecure.h>

#include <WiFiManager.h>

#include <Preferences.h>

#include <stdlib.h>

#include <time.h>

#include <ArduinoOTA.h>

#include <HTTPUpdate.h>


/* CONFIG */





#define RELAY_PIN     9    // ACTIVE LOW

#define BUZZER_PIN    7


#define AC_FB_PIN    11

#define SETUP_BTN_PIN 3
#define TANK_SELECTOR_PIN 12   // HIGH = FF/NC, LOW = GF/NO


#define RELAY_PULSE_MS 1000


#define MOTOR_CONFIRM_DELAY_MS 1500

#define MOTOR_LONGRUN_MS (40UL * 60UL * 1000UL)

#define SCHEDULE_HOUR 6

#define SCHEDULE_WINDOW_MINUTES 60




#define GMT_OFFSET_SEC 19800

#define DAYLIGHT_OFFSET_SEC 0


#define MAX_SCHEDULES 15
#define VALVE_REMINDER_INTERVAL_MS (2UL * 60UL * 1000UL)

enum TankPosition : uint8_t {
  TANK_POSITION_UNKNOWN = 0,
  TANK_POSITION_GF = 1,
  TANK_POSITION_FF = 2
};

struct ScheduleEntry { int wday, hour, minute; };

ScheduleEntry schedules[MAX_SCHEDULES];
int schedCount = 0;
int lastFiredYday[MAX_SCHEDULES];

const char* DAY_NAMES[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};


TelegramTransport telegramTransport;

WiFiManager wm;

Preferences prefs;
Preferences wifiPrefs;
String savedWiFiSSID;
String savedWiFiPassword;


int64_t lastUpdateId = 0;
String botUsername;
bool acceptMotorCommands = false;
unsigned long lastTelegramPoll = 0;
unsigned long lastSuccessfulPoll = 0;
bool telegramRequestWritten = false;


int64_t registeredChatId = 0;

int64_t pendingChatId    = 0;


bool tankLowReported = false;

MotorRun motorRun;
portMUX_TYPE feedbackMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool feedbackRunning = false;
volatile uint32_t feedbackChangedAt = 0;
volatile uint32_t feedbackRuns = 0;


bool relayInProgress = false;

unsigned long motorStartMillis = 0;

bool longRunReported = false;

int longRunMinutes = 0;

ValveRecord valveState;
bool valveStateDirty = false;
unsigned long lastValveSaveAttempt = 0;
unsigned long valveReminderDueMillis = 0;

String deviceName;

int lastScheduledYday = -1;


void sendTelegram(int64_t chatId, const String &msg);
void recordTankFull(bool gf);
void updateValvePosition(TankPosition position, bool updateLastFullTank);
void checkACFeedback();
String telegramPostJson(const String &method, const String &body);

void ARDUINO_ISR_ATTR onFeedbackChange() {
  portENTER_CRITICAL_ISR(&feedbackMux);
  bool running = digitalRead(AC_FB_PIN) == LOW;
  if (running != feedbackRunning) {
    feedbackRunning = running;
    feedbackChangedAt = millis();
    if (running) feedbackRuns++;
  }
  portEXIT_CRITICAL_ISR(&feedbackMux);
}

FeedbackSnapshot readFeedback() {
  portENTER_CRITICAL(&feedbackMux);
  bool running = digitalRead(AC_FB_PIN) == LOW;
  if (running != feedbackRunning) {
    feedbackRunning = running;
    feedbackChangedAt = millis();
    if (running) feedbackRuns++;
  }
  FeedbackSnapshot result{feedbackRunning, feedbackChangedAt, feedbackRuns};
  portEXIT_CRITICAL(&feedbackMux);
  return result;
}

String jsonString(const String &text) {
  return String(quoteJson(text.c_str()).c_str());
}


/* ---------------- Relay ---------------- */


void relayIdle() {

  digitalWrite(RELAY_PIN, HIGH);

}


void relayPulseStart() {

  if (relayInProgress) {
return;

  }


  relayInProgress = true;
digitalWrite(RELAY_PIN, LOW);

  delay(RELAY_PULSE_MS);

  digitalWrite(RELAY_PIN, HIGH);

  relayInProgress = false;
}


void startTankMotorFlow(bool gf, int64_t chatId) {
  startMotorFlow(gf, chatId, "TELEGRAM");
}


void startMotorFlow(bool gf, int64_t chatId, const String &source) {
  if (WiFi.status() != WL_CONNECTED) return;
  FeedbackSnapshot feedback = readFeedback();
  if (!motorRun.begin(gf, feedback, millis())) {
    if (chatId) sendTelegram(chatId, source + " START SKIPPED - MOTOR RUNNING OR STOP NOT CONFIRMED");
    return;
  }
  digitalWrite(TANK_SELECTOR_PIN, gf ? LOW : HIGH);
  delay(250);
  feedback = readFeedback();
  if (feedback.running || feedback.runs != motorRun.completedRuns) {
    motorRun.observe(feedback);
    if (chatId) sendTelegram(chatId, "MOTOR STARTED DURING TANK SELECTION - CHECK VALVE POSITION");
    return;
  }
  longRunReported = false;
  motorStartMillis = 0;
  relayPulseStart();
  delay(MOTOR_CONFIRM_DELAY_MS);
  feedback = readFeedback();
  motorRun.observe(feedback);
  bool ran = feedback.running || feedback.runs != motorRun.completedRuns;
  if (!ran && motorRun.cancelUnobservedStart(feedback)) {
    digitalWrite(TANK_SELECTOR_PIN, HIGH);
  }
  if (ran && feedback.running) motorStartMillis = millis();
  if (chatId) {
    sendTelegram(chatId, source + " COMMAND SENT");
  }
  if (ran) {
    if (chatId) {
      sendTelegram(chatId, feedback.running
          ? (gf ? "GF MOTOR IS RUNNING" : "FF MOTOR IS RUNNING")
          : "MOTOR RAN AND STOPPED - WAITING FOR STOP CONFIRMATION");
    }
    for (int i = 0; i < 2; i++) {

      digitalWrite(BUZZER_PIN, HIGH);

      delay(200);

      digitalWrite(BUZZER_PIN, LOW);

      delay(200);

    }

  } else if (chatId) {

    sendTelegram(chatId, gf ? "TANK-GF MAY BE FULL!!" : "TANK-FF MAY BE FULL!!");

  }

}


/* ---------------- Schedule NVS helpers ---------------- */

void loadSchedules() {
  deviceName = prefs.getString("dev_name", "Dual Tank Controller");
  longRunMinutes = prefs.getInt("lr_min", 0);
  schedCount = prefs.getInt("sc_cnt", 0);
  if (schedCount < 0 || schedCount > MAX_SCHEDULES) schedCount = 0;
  for (int i = 0; i < schedCount; i++) {
    String v = prefs.getString(("sc_" + String(i)).c_str(), "");
    int a = v.indexOf(','), b = v.lastIndexOf(',');
    if (a > 0 && b > a) {
      schedules[i].wday   = v.substring(0, a).toInt();
      schedules[i].hour   = v.substring(a + 1, b).toInt();
      schedules[i].minute = v.substring(b + 1).toInt();
    }
    lastFiredYday[i] = -1;
  }
  if (prefs.getBytesLength("sched_fired") == sizeof(lastFiredYday))
    prefs.getBytes("sched_fired", lastFiredYday, sizeof(lastFiredYday));
}

void saveSchedules() {
  prefs.putInt("sc_cnt", schedCount);
  for (int i = 0; i < schedCount; i++) {
    String v = String(schedules[i].wday) + "," + String(schedules[i].hour) + "," + String(schedules[i].minute);
    prefs.putString(("sc_" + String(i)).c_str(), v.c_str());
  }
  prefs.putBytes("sched_fired", lastFiredYday, sizeof(lastFiredYday));
}

bool addScheduleEntry(int wday, int hour, int minute) {
  if (schedCount >= MAX_SCHEDULES) return false;
  schedules[schedCount] = {wday, hour, minute};
  lastFiredYday[schedCount] = -1;
  schedCount++;
  saveSchedules();
  return true;
}

bool removeScheduleEntry(int n) {
  int idx = n - 1;
  if (idx < 0 || idx >= schedCount) return false;
  for (int i = idx; i < schedCount - 1; i++) {
    schedules[i]      = schedules[i + 1];
    lastFiredYday[i]  = lastFiredYday[i + 1];
  }
  schedCount--;
  saveSchedules();
  return true;
}

int parseDayName(const String &d) {
  const char* dn[] = {"sun","mon","tue","wed","thu","fri","sat"};
  String dl = d; dl.toLowerCase();
  for (int i = 0; i < 7; i++) if (dl.startsWith(dn[i])) return i;
  return -1;
}

void sendScheduleMenu(int64_t chatId) {
  String kb = "{\"inline_keyboard\":[";
  for (int i = 0; i < schedCount; i++) {
    if (i > 0) kb += ",";
    char row[120];
    snprintf(row, sizeof(row),
      "[{\"text\":\"%d) %s %02d:%02d  X remove\",\"callback_data\":\"SCHED_DEL_%d\"}]",
      i + 1, DAY_NAMES[schedules[i].wday], schedules[i].hour, schedules[i].minute, i + 1);
    kb += row;
  }
  if (schedCount > 0) kb += ",";
  kb += "[{\"text\":\"+ Add: /addschedule Sun 06:00\",\"callback_data\":\"SCHED_HELP\"}]";
  kb += "]}";
  String txt = (schedCount == 0)
    ? "No schedules set. Use /addschedule <Day> <HH:MM>"
    : "Schedules (tap entry to remove):";
  String body = "{\"chat_id\":" + String((long long)chatId) +
    ",\"text\":" + jsonString(txt) + ",\"reply_markup\":" + kb + "}";
  telegramPostJson("sendMessage", body);
}


/* ---------------- Telegram send ---------------- */


void sendTelegram(int64_t chatId, const String &msg) {
  telegramPostJson("sendMessage", "{\"chat_id\":" + String((long long)chatId) +
      ",\"text\":" + jsonString(msg) + "}");
}

String telegramPostJson(const String &method, const String &body) {
  telegramRequestWritten = false;
  if (WiFi.status() != WL_CONNECTED) return "";
  String response = telegramTransport.post(BOT_TOKEN, method, body);
  telegramRequestWritten = telegramTransport.written;
  return response;
}

bool telegramRequestSucceeded(const String &response) {
  JsonDocument document = parseJson(response.c_str());
  return document && telegramOk(document.get());
}

int extractTelegramMessageId(const String &response) {
  JsonDocument document = parseJson(response.c_str());
  if (!document || !telegramOk(document.get())) return 0;
  int64_t id = jsonInteger(jsonField(document.get(), "result"), "message_id");
  return id > 0 && id <= INT32_MAX ? int(id) : 0;
}

void answerCallbackQuery(const String &callbackQueryId, const String &text) {
  if (callbackQueryId.length() == 0) return;

  String body =
    "{\"callback_query_id\":" + jsonString(callbackQueryId) +
    ",\"text\":" + jsonString(text) + "}";
  telegramPostJson("answerCallbackQuery", body);
}

String valveReminderText() {
  return
    "Did you change the valve position back to the default FF?"
    "\nReminder cycle " + String(valveState.cycle) +
    ", update " + String(millis() / 1000);
}

String valveReminderKeyboard() {
  return
    "{\"inline_keyboard\":[[{\"text\":\"Yes\","
    "\"callback_data\":\"VALVE_FF:" + String(valveState.cycle) + "\"}]]}";
}

bool saveValveState() {
  lastValveSaveAttempt = millis();
  valveStateDirty = prefs.putBytes("valve_state", &valveState, sizeof(valveState)) != sizeof(valveState);
  if (valveStateDirty) Serial.println("NVS valve state write failed; retry pending");
  return !valveStateDirty;
}

bool editValveReminderMessage(const String &text, bool showYesButton) {
  if (valveState.message == 0) return false;

  String body =
    "{\"chat_id\":" + String((long long)valveState.chat) +
    ",\"message_id\":" + String(valveState.message) +
    ",\"text\":" + jsonString(text) + ",\"reply_markup\":" +
    (showYesButton ? valveReminderKeyboard() : "{\"inline_keyboard\":[]}") +
    "}";

  String response = telegramPostJson("editMessageText", body);
  JsonDocument document = parseJson(response.c_str());
  String error = jsonText(document.get(), "description");
  if (telegramRequestSucceeded(response) ||
      error.indexOf("message is not modified") >= 0) {
    return true;
  }

  if (error.indexOf("message to edit not found") >= 0 ||
      error.indexOf("message can't be edited") >= 0) {
    valveState.message = 0;
    valveState.publishAttempts = 0;
    saveValveState();
  }
  return false;
}

void finishValveReminderMessage(const String &text) {
  if (!valveState.message || editValveReminderMessage(text, false)) {
    valveState.cleanup = 0;
    saveValveState();
  }
}

void loadValveReminderState() {
  if (prefs.getBytesLength("valve_state") == sizeof(valveState) &&
      prefs.getBytes("valve_state", &valveState, sizeof(valveState)) == sizeof(valveState) &&
      valveState.valid()) {
    valveReminderDueMillis = millis() + VALVE_REMINDER_INTERVAL_MS;
    return;
  }
  valveState = ValveRecord{};
  valveState.lastFull = prefs.getUChar("last_full", TANK_POSITION_UNKNOWN);
  valveState.position = prefs.getUChar("valve_pos", TANK_POSITION_FF);
  if (valveState.lastFull > TANK_POSITION_FF) valveState.lastFull = TANK_POSITION_UNKNOWN;
  if (valveState.position != TANK_POSITION_GF && valveState.position != TANK_POSITION_FF)
    valveState.position = TANK_POSITION_FF;
  valveState.pending = valveState.position == TANK_POSITION_GF &&
      prefs.getBool("valve_wait", false);
  int message = prefs.getInt("valve_msg", 0);
  valveState.message = message > 0 ? message : 0;
  valveState.chat = registeredChatId;
  valveState.cycle = 1;
  valveState.cleanup = !valveState.pending && valveState.message != 0;
  saveValveState();
  valveReminderDueMillis = millis() + VALVE_REMINDER_INTERVAL_MS;
}

void updateValvePosition(TankPosition position, bool updateLastFullTank) {
  if (updateLastFullTank) {
    valveState.lastFull = position;
  }

  valveState.position = position;

  if (position == TANK_POSITION_GF) {
    valveState.pending = true;
    valveState.cycle++;
    if (valveState.cycle == 0) valveState.cycle = 1;
    valveState.publishAttempts = 0;
    valveState.cleanup = valveState.message && valveState.chat != registeredChatId;
    valveReminderDueMillis = millis() + VALVE_REMINDER_INTERVAL_MS;
    saveValveState();
    return;
  }

  valveState.pending = false;
  valveState.cleanup = valveState.message != 0;
  valveReminderDueMillis = millis();
  saveValveState();
}

void recordTankFull(bool gf) {
  updateValvePosition(
    gf ? TANK_POSITION_GF : TANK_POSITION_FF,
    true
  );
}

void checkValveReminder() {
  if (valveStateDirty) {
    if (millis() - lastValveSaveAttempt >= 1000) saveValveState();
    return;
  }
  if (registeredChatId == 0 || WiFi.status() != WL_CONNECTED) {
    return;
  }

  unsigned long now = millis();
  if ((long)(now - valveReminderDueMillis) < 0) return;

  valveReminderDueMillis = now + VALVE_REMINDER_INTERVAL_MS;
  if (valveState.cleanup || (valveState.message && valveState.chat != registeredChatId)) {
    valveState.cleanup = true;
    finishValveReminderMessage(valveState.chat == registeredChatId
        ? "Valve position recorded as default FF."
        : "Valve reminder moved to the registered chat.");
    if (!valveState.cleanup && valveState.chat != registeredChatId) {
      valveState.message = 0;
      valveState.chat = registeredChatId;
      saveValveState();
    }
    return;
  }
  if (!valveState.pending || valveState.position != TANK_POSITION_GF) return;
  String text = valveReminderText();
  if (valveState.message == 0) {
    if (valveState.publishAttempts >= 2) {
      Serial.println("Reminder send unconfirmed; use /update_valve_position GF to retry");
      return;
    }
    valveState.chat = registeredChatId;
    valveState.publishAttempts++;
    if (!saveValveState()) {
      valveState.publishAttempts--;
      return;
    }
    String body =
      "{\"chat_id\":" + String((long long)registeredChatId) +
      ",\"text\":" + jsonString(text) +
      ",\"reply_markup\":" + valveReminderKeyboard() + "}";
    String response = telegramPostJson("sendMessage", body);
    int message = extractTelegramMessageId(response);
    JsonDocument document = parseJson(response.c_str());
    if (telegramRequestSucceeded(response)) {
      if (message) valveState.message = message;
    } else if (!telegramRequestWritten || telegramRejected(document.get())) {
      valveState.publishAttempts--;
    }
    saveValveState();
  } else {
    editValveReminderMessage(text, true);
  }
}


/* -------- MOTOR ON inline button -------- */


void sendMotorOnButton(int64_t chatId) {
  String dn = deviceName;
  dn.replace("\"", "'");

  String body =
    "{\"chat_id\":" + String((long long)chatId) +
    ",\"text\":" + jsonString(dn) +
    ",\"reply_markup\":{\"inline_keyboard\":["
    "[{\"text\":\"MOTOR ON- GF\",\"callback_data\":\"MOTOR_ON_GF\"}],"
    "[{\"text\":\"MOTOR ON- FF\",\"callback_data\":\"MOTOR_ON_FF\"}]"
//    "[/{\"text\":\"Commands\",\"callback_data\":\"COMMANDS_MENU\"}]"
    "]}}";

  telegramPostJson("sendMessage", body);
}


void sendCommandsText(int64_t chatId) {
  String lr = (longRunMinutes > 0)
    ? "Current: " + String(longRunMinutes) + " mins"
    : "Currently disabled";
  sendTelegram(chatId,
    "Device: " + deviceName + "\n"
    "Available commands:\n"
    "/commands - Show this command list\n"
    "/listschedule - View auto-start schedules\n"
    "/addschedule Day HH:MM - Add schedule\n"
    "  e.g. /addschedule Sun 06:00\n"
    "/removeschedule N - Remove entry N\n"
    "  e.g. /removeschedule 1\n"
    "/setlongrun N - Long-run alert after N mins\n"
    "  e.g. /setlongrun 40  (0 = disable)\n"
    "/update_valve_position GF|FF - Correct recorded valve position\n"
    "/setname NewName - Rename this device\n"
    "Long-run alert: " + lr + "\n"
    "Note: In a group with multiple devices, all devices respond to commands. Use /setname to give each a unique name (per-device targeting coming in a future update).");
}

void sendStartMessage(int64_t chatId) {
  String body = "{\"chat_id\":" + String((long long)chatId) +
    ",\"text\":\"Welcome! Pair this device to your Telegram.\""
    ",\"reply_markup\":{\"inline_keyboard\":[[{\"text\":\"Register this device\",\"callback_data\":\"REGISTER\"}]]}}";
  telegramPostJson("sendMessage", body);
}


/* ---------------- Telegram poll ---------------- */


void pollTelegram() {
  if (WiFi.status() != WL_CONNECTED) {
    acceptMotorCommands = false;
    return;
  }
  if (millis() - lastTelegramPoll < 1000) return;
  lastTelegramPoll = millis();
  if (!lastSuccessfulPoll || millis() - lastSuccessfulPoll > 30000)
    acceptMotorCommands = false;
  static unsigned long lastIdentityAttempt = 0;
  if (!botUsername.length() &&
      (!lastIdentityAttempt || millis() - lastIdentityAttempt >= 30000)) {
    lastIdentityAttempt = millis();
    JsonDocument identity = parseJson(telegramPostJson("getMe", "{}").c_str());
    if (identity && telegramOk(identity.get()))
      botUsername = jsonText(jsonField(identity.get(), "result"), "username");
  }
  String response = telegramPostJson("getUpdates",
      "{\"offset\":" + String((long long)(lastUpdateId + 1)) +
      ",\"limit\":1,\"timeout\":2,\"allowed_updates\":[\"message\",\"callback_query\"]}");
  JsonDocument document = parseJson(response.c_str());
  const cJSON *updates = jsonField(document.get(), "result");
  if (!document || !telegramOk(document.get()) || !cJSON_IsArray(updates)) return;
  lastSuccessfulPoll = millis();
  if (cJSON_GetArraySize(updates) == 0) acceptMotorCommands = true;
  const cJSON *item = nullptr;
  cJSON_ArrayForEach(item, updates) {
    TelegramUpdate update = decodeUpdate(item);
    if (update.id <= lastUpdateId) continue;
    if (prefs.putLong64("tg_update", update.id) != sizeof(int64_t)) return;
    lastUpdateId = update.id;
    checkACFeedback();
    handleTelegramUpdate(update);
  }
}

bool isCurrentValveReminder(int64_t chatId, int messageId, const String &data) {
  return valveState.pending && valveState.position == TANK_POSITION_GF &&
      valveState.chat == chatId && messageId > 0 &&
      (uint32_t(messageId) == valveState.message ||
       (!valveState.message && valveState.publishAttempts)) &&
      data == "VALVE_FF:" + String(valveState.cycle);
}

void handleTelegramUpdate(const TelegramUpdate &update) {
  int64_t chatId = update.chat;
  if (!chatId) return;
  String cbData(update.callback.c_str());
  String txt(normalizeCommand(update.text, botUsername.c_str()).c_str());
  String callbackQueryId(update.query.c_str());
  int callbackMessageId = update.message;

  if (txt == "/start") {
    sendStartMessage(chatId);
    return;
  }


    if (txt == "/register" || cbData == "REGISTER") {
    if (registeredChatId != 0 && registeredChatId == chatId) {
      if (WiFi.status() == WL_CONNECTED) {
        sendTelegram(
          chatId,
          "You are already registered to this device.\n"
          "Everything is set."
        );
      } else {
        sendTelegram(
          chatId,
          "You are already registered to this device.\n"
          "Wi-Fi is not currently connected.\n"
          "Hold SETUP for 3 seconds to configure Wi-Fi."
        );
      }
      return;
    }

    pendingChatId = chatId;

    if (registeredChatId == 0) {
      sendTelegram(
        chatId,
        "This device is not registered yet.\n"
        "Press SETUP on the device to confirm ownership."
      );
    } else {
      sendTelegram(
        chatId,
        "This device is registered to another Telegram chat.\n"
        "Press SETUP on the device to confirm ownership."
      );
    }

    return;
  }

  if (registeredChatId != chatId) return;

  if (cbData.startsWith("VALVE_FF:") || cbData == "VALVE_RETURNED_FF") {
    if (isCurrentValveReminder(chatId, callbackMessageId, cbData)) {
      valveState.message = callbackMessageId;
      updateValvePosition(TANK_POSITION_FF, false);
      answerCallbackQuery(callbackQueryId, valveStateDirty
          ? "Save failed; the controller will retry."
          : "Valve position saved as FF.");
    } else {
      answerCallbackQuery(callbackQueryId, "This valve reminder is no longer active.");
    }
    return;
  }





  if (cbData == "MOTOR_ON_GF") {
    if (!acceptMotorCommands) {
      answerCallbackQuery(callbackQueryId, "Start ignored during reconnect. Please tap again shortly.");
      return;
    }
    answerCallbackQuery(callbackQueryId, "Checking motor state.");
    startTankMotorFlow(true, chatId);
    return;
  }

  if (cbData == "MOTOR_ON_FF") {
    if (!acceptMotorCommands) {
      answerCallbackQuery(callbackQueryId, "Start ignored during reconnect. Please tap again shortly.");
      return;
    }
    answerCallbackQuery(callbackQueryId, "Checking motor state.");
    startTankMotorFlow(false, chatId);
    return;
  }

  if (cbData == "COMMANDS_MENU") {
    sendCommandsText(chatId);
    return;
  }

  if (cbData == "SCHED_HELP" || txt == "/listschedule" || txt == "/schedule") {

    sendScheduleMenu(chatId);

  } else if (cbData.startsWith("SCHED_DEL_")) {

    int n = cbData.substring(10).toInt();

    if (removeScheduleEntry(n)) { sendTelegram(chatId, "Schedule removed."); sendScheduleMenu(chatId); }

    else sendTelegram(chatId, "Invalid number. Use /listschedule to check.");

  } else if (txt.startsWith("/addschedule ")) {

    String args = txt.substring(13);

    int sp = args.indexOf(' ');

    if (sp > 0) {

      int wday = parseDayName(args.substring(0, sp));

      String ts  = args.substring(sp + 1);

      int cl = ts.indexOf(':');

      if (wday >= 0 && cl > 0) {

        int h = ts.substring(0, cl).toInt(), m = ts.substring(cl + 1).toInt();

        if (h >= 0 && h < 24 && m >= 0 && m < 60) {

          if (addScheduleEntry(wday, h, m)) { sendTelegram(chatId, "Schedule added."); sendScheduleMenu(chatId); }

          else sendTelegram(chatId, "Max 15 schedules reached.");

        } else sendTelegram(chatId, "Invalid time. Use 24h format HH:MM");

      } else sendTelegram(chatId, "Usage: /addschedule Sun 06:00");

    }

  } else if (txt.startsWith("/removeschedule ")) {

    int n = txt.substring(16).toInt();

    if (removeScheduleEntry(n)) { sendTelegram(chatId, "Schedule removed."); sendScheduleMenu(chatId); }

    else sendTelegram(chatId, "Invalid number. Use /listschedule to check.");

  } else if (txt == "/commands") {

    sendCommandsText(chatId);

  } else if (txt.startsWith("/setlongrun ")) {

    int m = txt.substring(12).toInt();

    if (m < 0) { sendTelegram(chatId, "Value must be 0 or more minutes."); }

    else {

      longRunMinutes = m;

      prefs.putInt("lr_min", longRunMinutes);

      if (m == 0) sendTelegram(chatId, "Long-run alert disabled.");

      else sendTelegram(chatId, "Long-run alert set to " + String(m) + " mins.");

    }

  } else if (txt == "/update_valve_position" || txt.startsWith("/update_valve_position ")) {

    String position = txt.substring(String("/update_valve_position").length());
    position.trim();
    position.toUpperCase();

    if (position == "GF") {
      updateValvePosition(TANK_POSITION_GF, true);
      sendTelegram(
        chatId,
        valveStateDirty ? "NVS save failed; retry pending." :
        "Valve position corrected to GF. Reminder will start in 2 minutes."
      );
    } else if (position == "FF") {
      updateValvePosition(TANK_POSITION_FF, true);
      sendTelegram(
        chatId,
        valveStateDirty ? "NVS save failed; retry pending." :
        "Valve position corrected to default FF. Valve reminder cleared."
      );
    } else {
      sendTelegram(chatId, "Usage: /update_valve_position GF or FF");
    }

  } else if (txt == "/update") {
    if (!motorRun.canStart(readFeedback(), millis())) {
      sendTelegram(chatId, "OTA DEFERRED - MOTOR RUNNING OR STOP NOT CONFIRMED");
      return;
    }

    String url = String(OTA_GITHUB_URL);

    if (url.length() == 0) {

      sendTelegram(chatId, "Remote OTA URL not set. Use ArduinoOTA on the local network.");

    } else {

      sendTelegram(chatId, "Starting OTA download...");

      WiFiClientSecure otaClient;

      otaClient.setInsecure();

      httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

      t_httpUpdate_return ret = httpUpdate.update(otaClient, url);

      if (ret != HTTP_UPDATE_OK) {

        sendTelegram(chatId, "OTA failed: " + httpUpdate.getLastErrorString());

      }

    }

  } else if (txt.startsWith("/setname ")) {

    String newName = txt.substring(9);

    newName.trim();

    if (newName.length() == 0) {

      sendTelegram(chatId, "Name cannot be empty.");

    } else if (newName.length() > 32) {

      sendTelegram(chatId, "Name too long. Max 32 characters.");

    } else {

      newName.replace("\"", "'");

      deviceName = newName;

      prefs.putString("dev_name", deviceName);

      sendTelegram(chatId, "Device name set to: " + deviceName);

    }

  }

}


void checkScheduledMotorStart() {

  if (registeredChatId == 0 || WiFi.status() != WL_CONNECTED || schedCount == 0) return;

  struct tm timeinfo;

  if (!getLocalTime(&timeinfo, 100)) return;
  int date = (timeinfo.tm_year + 1900) * 400 + timeinfo.tm_yday;

  for (int i = 0; i < schedCount; i++) {

    if (schedules[i].wday  != timeinfo.tm_wday) continue;

    if (schedules[i].hour  != timeinfo.tm_hour)  continue;

    if (timeinfo.tm_min    <  schedules[i].minute) continue;

    if (lastFiredYday[i] == date) continue;

    int previous = lastFiredYday[i];
    lastFiredYday[i] = date;
    if (prefs.putBytes("sched_fired", lastFiredYday, sizeof(lastFiredYday)) != sizeof(lastFiredYday)) {
      lastFiredYday[i] = previous;
      Serial.println("Schedule skipped: NVS write failed");
      return;
    }

    startMotorFlow(false, registeredChatId, "AUTO SCHEDULE");

    break;

  }

}


/* ---------------- SETUP button ---------------- */

void checkSetupButton() {
  static bool btnWasPressed = false;
  static unsigned long pressStart = 0;

  bool pressed = (digitalRead(SETUP_BTN_PIN) == LOW);

  if (pressed && !btnWasPressed) {
    btnWasPressed = true;
    pressStart = millis();
  }

  if (!pressed && btnWasPressed) {
    btnWasPressed = false;
    unsigned long held = millis() - pressStart;

    if (held >= 3000) {
      if (!motorRun.canStart(readFeedback(), millis())) {
        sendTelegram(registeredChatId, "WI-FI SETUP DEFERRED - MOTOR RUNNING OR STOP NOT CONFIRMED");
        return;
      }
      wm.setConfigPortalTimeout(120);
      bool result = wm.startConfigPortal("PumpControl-Setup");

      if (result && WiFi.status() == WL_CONNECTED) {
        saveWiFiCredentials();

        if (pendingChatId != 0) {
          int64_t newChatId = pendingChatId;

          if (newChatId == registeredChatId) {
            sendTelegram(
              newChatId,
              "This device is already registered to this Telegram chat.\n"
              "Wi-Fi configuration is complete."
            );
          } else {
            registeredChatId = newChatId;
            prefs.putLong64("chat_id", registeredChatId);

            sendTelegram(
              registeredChatId,
              "Device registered successfully.\n"
              "Wi-Fi configuration is complete.\n"
              "This Telegram chat is now the registered owner."
            );

            sendMotorOnButton(registeredChatId);
          }

          pendingChatId = 0;
        } else if (registeredChatId != 0) {
          sendTelegram(
            registeredChatId,
            "Wi-Fi configuration completed successfully.\n"
            "Device is now connected to the network."
          );
        }
      } else if (pendingChatId != 0) {
        sendTelegram(
          pendingChatId,
          "Wi-Fi setup was not completed.\n"
          "Telegram registration has NOT been changed."
        );
      }

    } else {
      if (pendingChatId == 0) {
        if (WiFi.status() == WL_CONNECTED && registeredChatId != 0) {
          sendTelegram(
            registeredChatId,
            "This device is already registered to this Telegram chat.\n"
            "No registration request is pending."
          );
        }
        return;
      }

      if (pendingChatId == registeredChatId) {
        if (WiFi.status() == WL_CONNECTED) {
          sendTelegram(
            pendingChatId,
            "This device is already registered to this Telegram chat.\n"
            "No change was made."
          );
          pendingChatId = 0;
        }
        return;
      }

      if (WiFi.status() != WL_CONNECTED) return;

      registeredChatId = pendingChatId;
      prefs.putLong64("chat_id", registeredChatId);

      sendTelegram(
        registeredChatId,
        "Device registered successfully.\n"
        "This Telegram chat is now the registered owner."
      );

      sendMotorOnButton(registeredChatId);
      pendingChatId = 0;
    }
  }
}

/* Sensors */
void checkACFeedback() {
  FeedbackSnapshot feedback = readFeedback();
  motorRun.observe(feedback);
  bool completedTankGF = false;
  if (motorRun.complete(feedback, millis(), completedTankGF)) {
    motorStartMillis = 0;
    digitalWrite(TANK_SELECTOR_PIN, HIGH);
    recordTankFull(completedTankGF);
    if (registeredChatId)
      sendTelegram(registeredChatId, completedTankGF ? "TANK-GF FULL / MOTOR OFF" : "TANK-FF FULL / MOTOR OFF");
    for (int i = 0; i < 3; i++) {
      digitalWrite(BUZZER_PIN, HIGH);
      delay(100);
      digitalWrite(BUZZER_PIN, LOW);
      delay(100);
    }
  }
}


void checkLongRun() {

  if (longRunReported || longRunMinutes == 0 || !readFeedback().running) return;


  if (motorStartMillis > 0 &&

      millis() - motorStartMillis >= (longRunMinutes * 60UL * 1000UL)) {


    sendTelegram(registeredChatId, "MOTOR RUNNING OVER " + String(longRunMinutes) + " MINS");

    longRunReported = true;

  }

}


/* ---------------- WiFi ---------------- */

void loadWiFiCredentials() {
  wifiPrefs.begin("wifi_cfg", true);
  savedWiFiSSID = wifiPrefs.getString("ssid", "");
  savedWiFiPassword = wifiPrefs.getString("password", "");
  wifiPrefs.end();
}

void saveWiFiCredentials() {
  String ssid = wm.getWiFiSSID();
  String password = wm.getWiFiPass();

  if (ssid.length() == 0) return;

  wifiPrefs.begin("wifi_cfg", false);
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("password", password);
  wifiPrefs.end();

  savedWiFiSSID = ssid;
  savedWiFiPassword = password;
}

void startWiFiConnection() {
  if (savedWiFiSSID.length() == 0) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(savedWiFiSSID.c_str(), savedWiFiPassword.c_str());
}

void checkWiFiReconnect() {
  static unsigned long lastAttempt = 0;

  if (WiFi.status() == WL_CONNECTED) return;
  if (savedWiFiSSID.length() == 0) return;
  if (millis() - lastAttempt < 30000UL) return;

  lastAttempt = millis();

  WiFi.disconnect(false);
  WiFi.begin(savedWiFiSSID.c_str(), savedWiFiPassword.c_str());
}

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  loadWiFiCredentials();
  startWiFiConnection();
}


bool startupNotificationSent = false;


void sendStartupNotification() {
  if (startupNotificationSent) return;
  if (registeredChatId == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;

  sendMotorOnButton(registeredChatId);
  sendTelegram(
    registeredChatId,
    deviceName + " ONLINE"
  );
  sendTelegram(
    registeredChatId,
    "ENSURE THE CORRECT VALVES ARE OPEN/CLOSE BEFORE TURNING ON GF/FF"
  );


  startupNotificationSent = true;
}

/* Setup */


void setup() {

  delay(2000);

  Serial.begin(115200);

  delay(1000);


  pinMode(RELAY_PIN, OUTPUT);

  pinMode(BUZZER_PIN, OUTPUT);


  pinMode(AC_FB_PIN, INPUT);
  feedbackChangedAt = millis();
  feedbackRunning = digitalRead(AC_FB_PIN) == LOW;
  feedbackRuns = feedbackRunning ? 1 : 0;
  attachInterrupt(digitalPinToInterrupt(AC_FB_PIN), onFeedbackChange, CHANGE);

  pinMode(SETUP_BTN_PIN, INPUT_PULLUP);
  pinMode(TANK_SELECTOR_PIN, OUTPUT);
  digitalWrite(TANK_SELECTOR_PIN, HIGH);


  relayIdle();


  prefs.begin("tg", false);

  loadSchedules();
  registeredChatId = prefs.getLong64("chat_id", DEFAULT_CHAT_ID);
  lastUpdateId = prefs.getLong64("tg_update", 0);
  loadValveReminderState();

  setupWiFi();

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");


  ArduinoOTA.setHostname(OTA_HOSTNAME);

  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() { Serial.println("[OTA] Start"); });

  ArduinoOTA.onEnd([]()   { Serial.println("[OTA] Done"); });

  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] Error %u\n", e); });

  ArduinoOTA.begin();



}


/* Loop */


void loop() {
  checkACFeedback();

  checkWiFiReconnect();

  sendStartupNotification();

  pollTelegram();

  checkSetupButton();

//  checkTankLow();

  checkACFeedback();
  checkValveReminder();

  checkLongRun();

  if (motorRun.active) {
    digitalWrite(TANK_SELECTOR_PIN, motorRun.gf ? LOW : HIGH);
  }

  checkScheduledMotorStart();

  if (motorRun.canStart(readFeedback(), millis())) ArduinoOTA.handle();

  relayIdle();


}
