from pathlib import Path
import re
import sys


def function(source, name):
    match = re.search(r"^\w+ " + re.escape(name) + r"\([^;]*?\) \{", source, re.M)
    if match is None:
        raise ValueError(name)
    start = source.index("{", match.start())
    depth = 0
    quoted = False
    escaped = False
    for pos in range(start, len(source)):
        char = source[pos]
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
            continue
        if char == '"':
            quoted = True
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():pos + 1]
    raise ValueError(name)


source = Path(sys.argv[1]).read_text()
names = [
    "relayPulseStart", "startTankMotorFlow", "startMotorFlow", "checkACFeedback",
    "checkScheduledMotorStart", "isCurrentValveReminder", "jsonString",
    "telegramRequestSucceeded", "extractTelegramMessageId", "valveReminderText",
    "valveReminderKeyboard", "saveValveState", "editValveReminderMessage",
    "finishValveReminderMessage", "loadValveReminderState", "updateValvePosition",
    "recordTankFull", "checkValveReminder",
    "pollTelegram",
]
functions = [function(source, name) for name in names]
for name in [
    "RELAY_PIN", "BUZZER_PIN", "AC_FB_PIN", "TANK_SELECTOR_PIN",
    "RELAY_PULSE_MS", "MOTOR_CONFIRM_DELAY_MS",
    "VALVE_REMINDER_INTERVAL_MS", "MAX_SCHEDULES",
]:
    print(re.search(r"^#define " + name + r"\b[^\n]*", source, re.M).group())
print(re.search(r"enum TankPosition[^{]*\{[^}]*};", source).group())
print(re.search(r"struct ScheduleEntry[^\n]*", source).group())
for text in functions:
    print(text[:text.index("{")].strip() + ";")
print('#include "firmware_stubs.h"')
for text in functions:
    print(text)
