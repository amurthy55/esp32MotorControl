#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <stdlib.h>

/* ===================== CONFIG ===================== */

// NORMAL WPA2 WIFI (HOTSPOT)
#define WIFI_SSID     "Surya"
#define WIFI_PASSWORD "YOUR_HOTSPOT_PASSWORD"

#define BOT_TOKEN "8446451270:AAEjcmM2yGiLiZsVHJ_3UvZpA6_dHA75MeY"

#define RELAY_PIN     9    // ACTIVE LOW
#define BUZZER_PIN    7
#define TANK_LOW_PIN 16
#define AC_FB_PIN    11
#define SETUP_BTN_PIN 4

#define RELAY_PULSE_MS 1000
#define WIFI_RESET_MS 10000

/* ================================================= */

WiFiClientSecure client;
WiFiManager wm;
Preferences prefs;

long lastUpdateId = 0;

int64_t registeredChatId = 0;
int64_t pendingChatId    = 0;

bool tankLowReported = false;
bool motorOffReported = false;
bool motorEverStarted = false;

unsigned long btnPressStart = 0;
bool btnWasPressed = false;

/* ---------------- Relay ---------------- */

void relayIdle() {
  digitalWrite(RELAY_PIN, HIGH);
}

void relayPulseStart() {
  Serial.println("[RELAY] START pulse");
  digitalWrite(RELAY_PIN, LOW);
  delay(RELAY_PULSE_MS);
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println("[RELAY] RELEASE");
}

/* ---------------- Telegram send ---------------- */

void sendTelegram(int64_t chatId, const String &msg) {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("[TG] Sending → " + msg);

  client.setInsecure();
  if (!client.connect("api.telegram.org", 443)) {
    Serial.println("[TG] Connection failed");
    return;
  }

  String payload =
    "chat_id=" + String((long long)chatId) +
    "&text=" + msg;

  client.println("POST /bot" + String(BOT_TOKEN) + "/sendMessage HTTP/1.1");
  client.println("Host: api.telegram.org");
  client.println("Content-Type: application/x-www-form-urlencoded");
  client.println("Content-Length: " + String(payload.length()));
  client.println("Connection: close");
  client.println();
  client.print(payload);
  client.stop();
}

/* -------- MOTOR ON inline button -------- */

void sendMotorOnButton(int64_t chatId) {
  if (WiFi.status() != WL_CONNECTED) return;

  Serial.println("[TG] Sending MOTOR ON button");

  client.setInsecure();
  if (!client.connect("api.telegram.org", 443)) return;

  String payload =
    "chat_id=" + String((long long)chatId) +
    "&text=MOTOR+CONTROLLER+READY"
    "&reply_markup=%7B%22inline_keyboard%22%3A%5B%5B%7B%22text%22%3A%22MOTOR+ON%22%2C%22callback_data%22%3A%22MOTOR_ON%22%7D%5D%5D%7D";

  client.println("POST /bot" + String(BOT_TOKEN) + "/sendMessage HTTP/1.1");
  client.println("Host: api.telegram.org");
  client.println("Content-Type: application/x-www-form-urlencoded");
  client.println("Content-Length: " + String(payload.length()));
  client.println("Connection: close");
  client.println();
  client.print(payload);
  client.stop();
}

/* ---------------- Telegram poll ---------------- */

void pollTelegram() {
  if (WiFi.status() != WL_CONNECTED) return;

  client.setInsecure();
  if (!client.connect("api.telegram.org", 443)) return;

  client.println(
    "GET /bot" + String(BOT_TOKEN) +
    "/getUpdates?offset=" + String(lastUpdateId + 1) +
    " HTTP/1.1");
  client.println("Host: api.telegram.org");
  client.println("Connection: close");
  client.println();

  String resp;
  while (client.connected() || client.available()) {
    if (client.available()) resp += client.readString();
  }
  client.stop();

  if (resp.indexOf("\"update_id\":") < 0) return;

  int u = resp.lastIndexOf("\"update_id\":");
  lastUpdateId = resp.substring(u + 12).toInt();

  int c = resp.lastIndexOf("\"chat\":{\"id\":");
  if (c < 0) return;

  int start = c + 13;
  int end   = resp.indexOf(",", start);
  if (end < 0) return;

  int64_t chatId = strtoll(resp.substring(start, end).c_str(), nullptr, 10);

  if (registeredChatId == 0 &&
      (resp.indexOf("/register") > 0 || resp.indexOf("REGISTER") > 0)) {

    pendingChatId = chatId;
    sendTelegram(chatId, "Press SETUP button on device to confirm ownership");
    return;
  }

  if (registeredChatId != chatId) return;

  if (resp.indexOf("MOTOR_ON") > 0) {
    motorEverStarted = true;
    motorOffReported = false;
    relayPulseStart();
    sendTelegram(chatId, "MOTOR START COMMAND SENT");
  }
}

/* ---------------- SETUP button ---------------- */

void checkSetupButton() {
  bool pressed = (digitalRead(SETUP_BTN_PIN) == LOW);

  if (pressed && !btnWasPressed) {
    btnWasPressed = true;
    btnPressStart = millis();
  }

  if (!pressed && btnWasPressed) {
    btnWasPressed = false;

    if (pendingChatId != 0) {
      registeredChatId = pendingChatId;
      prefs.putLong64("chat_id", registeredChatId);
      sendTelegram(registeredChatId, "Device registered successfully");
      sendMotorOnButton(registeredChatId);
      pendingChatId = 0;
    }
  }
}

/* ---------------- Sensors ---------------- */

void checkTankLow() {
  if (!registeredChatId) return;

  if (digitalRead(TANK_LOW_PIN) == LOW) {
    if (!tankLowReported) {
      tankLowReported = true;
      sendTelegram(registeredChatId, "WATER LEVEL LOW");
    }
  } else tankLowReported = false;
}

void checkACFeedback() {
  if (!registeredChatId || !motorEverStarted) return;

  if (digitalRead(AC_FB_PIN) == HIGH) {
    if (!motorOffReported) {
      motorOffReported = true;
      sendTelegram(registeredChatId, "TANK FULL. MOTOR OFF");
    }
  } else motorOffReported = false;
}

/* ---------------- WiFi ---------------- */

void setupWiFi() {
  WiFi.mode(WIFI_STA);

  // Try saved credentials first
  if (WiFi.SSID().length() > 0) {
    WiFi.begin();
    unsigned long t0 = millis();
    while (millis() - t0 < 15000) {
      if (WiFi.status() == WL_CONNECTED) return;
      delay(500);
    }
  }

  // Fallback to captive portal
  wm.autoConnect("PumpControl");
}

/* ---------------- Setup ---------------- */

void setup() {
  delay(2000);
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== BOOT ===");

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(TANK_LOW_PIN, INPUT_PULLUP);
  pinMode(AC_FB_PIN, INPUT);
  pinMode(SETUP_BTN_PIN, INPUT_PULLUP);

  relayIdle();

  prefs.begin("tg", false);
  registeredChatId = prefs.getLong64("chat_id", 0);

  setupWiFi();

  if (registeredChatId) {
    sendMotorOnButton(registeredChatId);
    sendTelegram(registeredChatId,
      "MOTOR CONTROLLER ONLINE — AUTO MODE READY");
  }
}

/* ---------------- Loop ---------------- */

void loop() {
  pollTelegram();
  checkSetupButton();
  checkTankLow();
  checkACFeedback();
  relayIdle();
  delay(5000);
}