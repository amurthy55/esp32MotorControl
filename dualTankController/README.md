# Dual-tank controller

This sketch is based on the supplied v4.3 valve-reminder firmware. The older
single-tank sketch at the repository root is a separate application.

## Build and upload

Use Arduino ESP32 core **3.3.0**, **WiFiManager 2.0.17**, and the
**LOLIN S2 Mini** board. cJSON is provided by this ESP32 core; no additional
Arduino JSON library is required.

Copy `config.h.template` to `config.h`, then enter the existing Telegram and OTA
configuration. `config.h` is ignored by Git. Open `dualTankController.ino` in
Arduino IDE, keeping the three helper headers in the same directory.

```sh
arduino-cli compile --warnings all --fqbn esp32:esp32:lolin_s2_mini dualTankController
```

Upload without erasing flash to retain Wi-Fi credentials, registration, schedules,
and NVS reminder state. Existing v4.3 reminder keys migrate to the versioned
`valve_state` record in the same `tg` namespace. Downgrading to v4.3 does not
recover newer reminder changes because the legacy keys are no longer updated.

## Operating behavior

- FF can start manually or from Telegram. Scheduled starts select FF.
- GF starts from Telegram only.
- AC feedback LOW records a running motor. HIGH must remain stable for two
  seconds before completion is classified and the selector returns to FF.
  Idle HIGH at boot produces no completion.
- Interrupts retain feedback transitions during network operations. Start requests
  are rejected while running or while a completion remains unprocessed.
- A start with no observed running feedback releases its lock and returns the
  selector to FF. If feedback arrives during selector settling, no additional
  relay pulse is sent; the operator receives a valve-check warning.
- The relay pulse remains one second. Confirmed Telegram/scheduled starts retain
  two 200 ms beeps; completion retains three 100 ms beeps. Manual starts remain
  silent. Long-run timing still covers Telegram/scheduled starts only.
- Completion is saved before its Telegram notification. Feedback monitoring and
  NVS recording also work while disconnected or unregistered. Ordinary completion
  notifications are best effort; this is not a durable notification queue.

### Valve reminder

GF completion starts a two-minute timer. The prompt is:

> Did you change the valve position back to the default FF?

After publication, the controller edits the same message every two minutes.
Each logical GF cycle has a persistent callback identifier. Yes validates the
registered chat, message, and cycle, records the valve as FF, and removes the
button without changing the historical GF completion.

FF completion or `/update_valve_position FF` cancels the reminder. Failed edits
that remove the button retry after connectivity returns, including across reboot.
The retained message can be reused by a later GF cycle. A confirmed deleted or
uneditable message is replaced.

`/update_valve_position GF` corrects the last-full classification and starts a new
reminder cycle. `/update_valve_position FF` corrects the classification and clears
it. Both accept the `@YourBotName` form and leave the relay/selector untouched.

The timer restarts at two minutes after reboot; remaining elapsed time is not
stored. NVS writes use one versioned record and failed writes retry before reminder
network work.

### Lost Telegram acknowledgements

Telegram `sendMessage` returns the new message ID, but its documented API provides
no idempotency key or method to retrieve an arbitrary outgoing message by request.
If Telegram accepts a send and its response is lost, delivery is uncertain.

The firmware persists a send reservation before transmitting and allows **at most
two uncertain send attempts per GF cycle**, including across reboot. Thus an
uncertain send can cause at most one automatic duplicate, rather than an endless
stream. Definite connection failures before writing and explicit Telegram
rejections may retry normally.

If both attempts remain unconfirmed, automatic publication pauses. Tap Yes on an
existing prompt (its message ID can be recovered from the callback), or issue
`/update_valve_position GF` to retry. A successfully acknowledged message ID is
always edited instead of posting another prompt.

### Reconnect and scheduling

Update IDs are saved before dispatch. After boot or a polling outage longer than
30 seconds, the controller drains queued updates and rejects motor-start callbacks
until it reaches an empty queue. Tap the start button again after reconnect.
Correction commands and reminder acknowledgements can still be processed.

Schedules persist their last firing date before attempting a start, so rebooting
within the same minute cannot repeat the relay pulse. A busy scheduled start is
skipped for that date, as in the previous controller. Wi-Fi setup and OTA handling
are deferred while running or awaiting completion.

### Network and hardware limits

Telegram DNS waits are capped at two seconds, TCP/socket operations use 1.5-second
timeouts, TLS negotiation uses a three-second timeout, and the incremental response
reader has a five-second deadline. These are separate limits, not a five-second
limit for the entire request. Headers are limited to 4 KiB and bodies to 32 KiB.
Incomplete HTTP/JSON responses are rejected.

The existing insecure TLS setting remains; certificate validation was not added.
GPIO feedback alone cannot prove a tank is full or identify an externally changed
physical valve. The user's installation permits manual FF only and powers the ESP32
with the starter. A reset during a continuing GF run is not recovered as GF.
Unobserved separate physical runs and feedback noise need hardware validation.

## Host regression tests

Requirements: Bash, curl, tar, sha256sum, Python 3, and a C/C++17 compiler.

```sh
bash dualTankController/tests/run_tests.sh
python3 -m py_compile dualTankController/tests/extract_firmware.py
```

The runner downloads checksum-pinned cJSON 1.7.18 into the user's cache, matching
the API version bundled with ESP32 core 3.3.0. It compiles the protocol/state
helpers and extracts the actual motor, schedule, polling, and reminder functions
from the sketch into a host harness. GPIO, time, Wi-Fi, NVS, and Telegram responses
are simulated. No bot credentials, real Telegram traffic, or motor actuation are
used.

Run the board build as well: host tests do not execute ESP32 interrupts, flash
transactions, TLS sockets, or the physical starter.
