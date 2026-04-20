# Motor Auto Control System

An ESP32-based remote motor control system for water pumps with Telegram bot interface and safety monitoring.

## Features

- **Remote Control**: Start/stop motor via Telegram bot
- **Auto Registration**: Secure user registration through physical button confirmation
- **Safety Monitoring**: 
  - Tank level low sensor
  - AC feedback sensor for tank full detection
- **WiFi Management**: Auto-connect with captive portal fallback
- **Status Notifications**: Real-time alerts via Telegram

## Hardware Requirements

- ESP32 development board
- Relay module (ACTIVE LOW)
- Buzzer for local feedback
- Tank level sensor (digital)
- AC feedback sensor
- Setup button

## Wiring

| ESP32 Pin | Component | Notes |
|-----------|-----------|-------|
| GPIO 9    | Relay     | ACTIVE LOW |
| GPIO 7    | Buzzer    | |
| GPIO 16   | Tank Low  | INPUT_PULLUP |
| GPIO 11   | AC Feedback | INPUT |
| GPIO 4    | Setup Button | INPUT_PULLUP |

## Setup

### 1. Configuration

Copy `config.h.template` to `config.h` and fill in your credentials:

```cpp
#define WIFI_SSID "your_wifi_name"
#define WIFI_PASSWORD "your_wifi_password"
#define BOT_TOKEN "your_telegram_bot_token"
```

**Important**: `config.h` is excluded from git for security.

### 2. Telegram Bot Setup

1. Create a new bot via @BotFather on Telegram
2. Copy the bot token to `config.h`
3. Send `/register` to your bot
4. Press the SETUP button on the device to confirm ownership

### 3. Upload Code

1. Open `anupuramMotorControl.ino` in Arduino IDE
2. Install required libraries:
   - WiFi
   - WiFiManager
   - Preferences
3. Upload to ESP32

## Operation

1. Device boots and connects to WiFi
2. Bot sends "MOTOR ON" button to registered user
3. Press button to send 1-second pulse to relay (simulates start switch)
4. System monitors sensors and sends status updates

## Safety Notes

- Relay contacts are connected across the starter's start switch
- System sends momentary pulse only (1 second)
- AC feedback monitors when tank is full
- Tank low sensor prevents dry running

## Dependencies

- WiFi library (built-in)
- WiFiManager by tzapu
- Preferences library (built-in)

## License

MIT License

## Contributing

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Submit a pull request

## Security Notice

Never commit sensitive information like WiFi passwords or bot tokens. Use the provided `.gitignore` file to exclude `config.h`.
