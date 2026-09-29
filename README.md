# Reel Clock

A working clock built from four **Bally electromechanical pinball score reels**, driven by an ESP32. Each reel is a physical drum solenoid-advanced one digit at a time, homed against its own zero switch at boot, and stepped forward to match real time synced over NTP — no local RTC, no manual setting.

Display format: `H:MM` (hours tens, hours units, minutes tens, minutes units), with a hardware switch for 12/24-hour mode.

See [`Reel_Clock_Wiring.pdf`](./Reel_Clock_Wiring.pdf) for the full wiring schematic — power domains, control signals, and a fully detailed per-reel load circuit.

## Features

- **Self-homing** — every reel finds its own zero position at boot via a dedicated zero switch, so it always starts from a known-good state regardless of where it was left.
- **NTP time sync with automatic DST** — a POSIX TZ string handles the actual U.S. daylight-saving transition dates, not just a fixed offset that goes stale twice a year.
- **Drift self-correction** — every time a reel settles at the digit its zero switch should trip on, the code double-checks the switch agrees. A mismatch (from an occasional double-fire) triggers a re-home of just that one reel.
- **Per-reel tuned timing** — each of the four ~40-year-old solenoids pulls in slightly differently; each has its own bench-tuned relay pulse duration.
- **12/24-hour mode**, read live off a physical switch — no reboot needed to change it.
- **WiFi connect timeout** — reboots automatically if it can't get on the network within 10 seconds, rather than hanging indefinitely.
- **Per-reel enable flag** — a reel with a bad coil can be disabled in software (skipped entirely: never homed, targeted, or pulsed) without affecting the others, for graceful degradation while a replacement part is on order.

## Hardware

- ESP32 DevKit (38-pin)
- 4-channel relay board (5V logic, active-HIGH as jumpered — **confirm polarity on your own board before trusting it**; see build notes)
- 4× Bally score-reel solenoid assemblies, each with its own zero switch and native EOS (End-of-Stroke) switch
- 24V DC power supply
- 12/24V → 5V buck converter (powers the ESP32 and relay logic)
- Flyback diode per coil (e.g. 1N5401)
- 10kΩ resistors (×2) for the two zero switches on ESP32 input-only pins
- 2× #47 incandescent bulbs for backlighting (optional, run underdriven at 5V)

## Pin map

| Reel | Relay # | Relay GPIO | Zero switch GPIO | Zero pull-up | Pulse time |
|---|---|---|---|---|---|
| H-tens (Hour 10s) | 1 | 25 | 36 (VP) | External 10kΩ → 3.3V | 110 ms |
| H-units (Hour 1s) | 2 | 26 | 39 (VN) | External 10kΩ → 3.3V | 60 ms |
| M-tens (Minute 10s) | 3 | 27 | 16 | Internal (`INPUT_PULLUP`) | 60 ms |
| M-units (Minute 1s) | 4 | 14 | 4 | Internal (`INPUT_PULLUP`) | 60 ms |

Plus GPIO 23 for the 12/24-hour mode switch (internal pull-up; closed = 12-hour, open = 24-hour).

Pulse times are starting points from bench-testing this specific set of reels — re-tune per reel if you build your own, since 40-year-old solenoids don't all behave identically.

## Wiring notes — the important gotchas

- **Relay trigger polarity varies by board.** Bench-test with a simple on/off cycle (see `RELAY_CLICK_TEST` in the sketch) before trusting `HIGH`/`LOW` assumptions — a different board or jumper setting can easily be the opposite of what's coded here.
- **The zero switch here is normally CLOSED and opens at the zero position** — the opposite of a typical momentary switch. Confirm with a multimeter before wiring the pull-up logic.
- **GPIO 34/35/36/39 have no internal pull-up hardware.** Any switch on one of these needs an external 10kΩ resistor to 3.3V — not 5V, these pins aren't 5V-tolerant.
- **The EOS switch stays in hardware, not code.** It's wired in series with the coil itself (relay → EOS → coil → 24V+) and never touches the ESP32 — it's what actually protects the coil from continuous power, independent of software timing.
- **Flyback diode across every coil**, cathode toward whichever lug is wired to 24V+.
- **Relay pulse duration is per-reel and found by testing**, not assumed. Too long a pulse and a reel can double-fire on a single intended step before its own EOS switch recloses.

## Setup

1. Install the ESP32 board package in Arduino IDE (Boards Manager → search "esp32").
2. Select **Tools → Board → ESP32 Arduino → ESP32 Dev Module**.
3. Open `ReelClock/ReelClock.ino` and fill in:
   - `WIFI_SSID` / `WIFI_PASSWORD`
   - `TZ_STRING` (defaults to US Eastern — see the comment in the sketch for other US time zones)
4. Wire according to the pin map and schematic above.
5. Flash, then watch Serial Monitor at 115200 baud for homing status, WiFi connect time, and NTP sync confirmation.

### Bench-test modes

Two `#define` flags near the top of the sketch (only one active at a time) support hardware bring-up without running the full clock:

- `RELAY_CLICK_TEST` — cycles the 4 relays one at a time, 1s on/1s off, for wiring and LED verification.
- `ZERO_HOME_TEST` — homes each reel in isolation at a slow, watchable pace (no WiFi, no calibration), printing `Pulsing` / `0 not found` / `0 found` to Serial. Used to bench-tune each reel's pulse timing and verify its zero switch.

## Known limitations

- No battery-backed RTC — time is only as good as the most recent NTP sync, so the clock reads nothing meaningful until WiFi connects.
- Homing is blocking and happens once at boot; it does not re-run automatically except via the drift self-correction check or a reboot.
