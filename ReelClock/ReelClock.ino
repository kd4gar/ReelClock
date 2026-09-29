/*
 * Bally EM score reel clock — ESP32
 *
 * 4 reels: H-tens, H-units, M-tens, M-units
 * Each reel: 1 relay (solenoid advance), 1 switch read by the ESP32 (zero) - wired w/
 * INPUT_PULLUP (or external pull-up on input-only pins, see below). The zero switch is
 * normally CLOSED and OPENS when the reel reaches the zero position (opposite of a
 * typical normally-open switch) — see isAtZero(), which reads HIGH as "at zero".
 *
 * The original machine's "nine" switch (mechanically fires the next reel when the
 * current one rolls 9->0, e.g. odometer-style carry) isn't used here — each reel's
 * target digit is computed independently from NTP time, so no mechanical carry is needed.
 *
 * The reel's native EOS (End Of Stroke) switch is NOT wired to the ESP32. It stays wired
 * in series with the coil itself (relay -> EOS switch -> coil -> GND), same as it works
 * natively in the original machine: it physically cuts the coil's own power the instant
 * the stroke completes, independent of relay/software timing.
 *
 * Because of that, the relay's hold time (PULSE_TIMEOUT_MS per reel) must be tuned
 * shorter than the reel's natural EOS-open-then-reclose cycle time: energize -> plunger
 * travels -> hits EOS (opens, coil cuts off) -> spring retracts plunger -> EOS recloses.
 * If the relay is still closed when EOS recloses, the coil fires again on the same
 * software-intended step — an undetected double-fire, since the ESP32 has no visibility
 * into EOS and just assumes one relay pulse = one step. Bench-test each reel individually
 * (they're ~40 years old and won't all behave the same) and tune its entry in
 * PULSE_TIMEOUT_MS down until stepping is reliable with no double-fires.
 *
 * Wire a flyback diode across every solenoid coil. Do this in hardware, not software.
 *
 * Pin assignments below are finalized (avoid ESP32 strapping pins 0/2/12/15). Zero
 * switches on H-tens/H-units (pins 36/39) are ESP32 input-only pins with no internal
 * pull-up hardware — those two need an external 10k resistor to 3.3V. Everything else
 * uses INPUT_PULLUP. Wire to match REEL_PINS exactly, or update REEL_PINS to match
 * your actual wiring.
 */

#include <WiFi.h>
#include <time.h>

// ---------------- TEMPORARY: bench test modes (only one should be defined at a time) ----------------
// RELAY_CLICK_TEST: cycles the 4 relays one at a time, 1s on/1s off, for wiring/LED checks.
// ZERO_HOME_TEST: pulses ONLY the Min 1 (M-units) reel until its zero switch trips, then
// stops completely and does nothing else -- no WiFi, no other reels, no calibration offset
// applied. Isolates whether the zero switch itself lines up with the drum's printed "0",
// independent of timing/calibration/other reels. Read the physical digit after it stops.
// #define RELAY_CLICK_TEST
// #define ZERO_HOME_TEST

// ---------------- Config ----------------
const char* WIFI_SSID     = "your-ssid";
const char* WIFI_PASSWORD = "your-password";
const char* NTP_SERVER = "pool.ntp.org";
// POSIX TZ string for US Eastern — encodes both the UTC offset and the actual annual DST
// transition rule (2nd Sunday of March to 1st Sunday of November), so DST is handled
// correctly year-round instead of a fixed offset that goes an hour wrong twice a year.
// Adjust if you're not in US Eastern — e.g. Central: "CST6CDT,M3.2.0,M11.1.0",
// Mountain: "MST7MDT,M3.2.0,M11.1.0", Pacific: "PST8PDT,M3.2.0,M11.1.0".
const char* TZ_STRING = "EST5EDT,M3.2.0,M11.1.0";

// If WiFi hasn't connected within this long, reboot rather than wait forever. Reels have
// already homed to zero by this point, so a hung connect otherwise leaves the clock
// sitting there indefinitely until someone power-cycles it by hand.
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 10000;

// Per-reel relay energize time (ms), in REEL_PINS order (H-tens, H-units, M-tens, M-units).
// Start at these values and tune each one down individually during bench testing — see
// the file header for why this must stay shorter than each reel's natural EOS cycle time.
uint32_t PULSE_TIMEOUT_MS[4] = { 110, 60, 60, 60 };  // H-tens (Hour 10) settled at 110ms: chattered at 120ms, worked at 110ms
const uint32_t SETTLE_MS   = 60;     // pause after de-energizing before next pulse
const uint32_t DEBOUNCE_MS = 5;

// 4-channel relay board (JQC-3FF-S-Z relays, per-channel hi/low jumpers) — confirmed via
// RELAY_CLICK_TEST that as currently jumpered, this board is active-HIGH: pulling the IN
// pin HIGH energizes the relay, LOW de-energizes it. (The earlier 8-channel board was the
// opposite, active-LOW with no jumper — if you ever swap boards again, re-run
// RELAY_CLICK_TEST rather than assuming polarity carries over.)
const uint8_t RELAY_ON  = HIGH;
const uint8_t RELAY_OFF = LOW;

// ---------------- Pins ----------------
struct ReelPins {
  uint8_t relay;
  uint8_t zero;
};

// Order: H-tens, H-units, M-tens, M-units
// Note: M-tens/M-units zero pins are swapped from the original plan (16/4) to match
// actual wiring -- M-units' zero switch was found wired to D4 instead of D16.
ReelPins REEL_PINS[4] = {
  {25, 36},
  {26, 39},
  {27, 16},
  {14, 4},
};

// Calibration: the digit actually showing on the drum at the instant isAtZero() trips,
// per reel. Should be 0 if the zero switch's cam is mechanically aligned with the
// printed "0". A nonzero value compensates for a switch that trips a fixed number of
// positions past true zero — determined empirically by comparing the displayed digit
// to the correct time after stepping is confirmed reliable (not before, since a
// double-firing reel gives a moving, not fixed, offset).
// M-units (Min 1) was previously calibrated to "4", but that was derived from a zero
// switch reading taken before the D4/D16 wiring mix-up was fixed -- i.e. from the wrong
// pin. The isolated ZERO_HOME_TEST on the corrected wiring found true zero with no
// compensation needed, so this is back to 0 like the others.
uint8_t ZERO_SWITCH_DIGIT[4] = { 0, 0, 0, 0 };

// Per-reel enable flag. A disabled reel is never homed, never given a target, and never
// stepped -- it's left alone entirely.
bool REEL_ENABLED[4] = { true, true, true, true };  // H-tens re-enabled: new coil installed

const char* REEL_NAMES[4] = { "H-tens", "H-units", "M-tens", "M-units" };

// 12hr/24hr display mode toggle. Any free GPIO works; avoid strapping pins (0,2,12,15).
const uint8_t MODE_SWITCH_PIN = 23; // LOW = 12-hour mode, HIGH (floating/open) = 24-hour

enum ReelState { IDLE, ENERGIZED, SETTLING, HOMING };

struct Reel {
  ReelPins pins;
  ReelState state;
  int position;         // 0-9, tracked in software
  int stepsRemaining;
  uint32_t stateStartMs;
};

Reel reels[4];

// GPIOs 34/35/36/39 are input-only and have no internal pull-up hardware — pinMode()
// with INPUT_PULLUP on them is a silent no-op (ESP32 core logs a gpio_pullup_en error).
// Those pins need an external 10k pull-up to 3.3V; this just picks the right pinMode
// so the pin isn't left fully floating with no pull-up at all.
bool isInputOnlyPin(uint8_t pin) {
  return pin == 34 || pin == 35 || pin == 36 || pin == 39;
}

void pinModeSwitch(uint8_t pin) {
  pinMode(pin, isInputOnlyPin(pin) ? INPUT : INPUT_PULLUP);
}

// ---------------- Zero switch reading (debounced) ----------------
// The zero switch is normally CLOSED and OPENS when the reel reaches the zero position
// (opposite of a typical normally-open momentary switch) — so "at zero" is a HIGH read,
// not LOW. Debounce is applied to the HIGH state since that's the meaningful transition.
bool isAtZero(uint8_t pin) {
  if (digitalRead(pin) != HIGH) return false;
  delay(DEBOUNCE_MS);
  return digitalRead(pin) == HIGH;
}

// ---------------- Homing (blocking, boot-time only) ----------------
void homeReel(Reel &r, int index) {
  Serial.printf("Homing reel on relay pin %d...\n", r.pins.relay);

  // Already at zero?
  if (isAtZero(r.pins.zero)) {
    r.position = ZERO_SWITCH_DIGIT[index];
    return;
  }

  // Pulse until the zero switch fires, or bail after too many attempts (jammed reel,
  // or switch not wired yet). Fixed-duration pulse — the reel's own EOS switch (wired
  // in series with the coil, not read by the ESP32) is what actually bounds coil-on time.
  for (int attempts = 0; attempts < 15; attempts++) {
    digitalWrite(r.pins.relay, RELAY_ON);
    delay(PULSE_TIMEOUT_MS[index]);
    digitalWrite(r.pins.relay, RELAY_OFF);
    delay(SETTLE_MS);

    if (isAtZero(r.pins.zero)) {
      r.position = ZERO_SWITCH_DIGIT[index];
      return;
    }
  }

  Serial.printf("WARNING: reel on relay pin %d failed to home — check switches/coil.\n", r.pins.relay);
}

#if !defined(RELAY_CLICK_TEST) && !defined(ZERO_HOME_TEST)

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < 4; i++) {
    reels[i].pins = REEL_PINS[i];
    reels[i].state = IDLE;
    reels[i].position = -1;
    reels[i].stepsRemaining = 0;

    pinMode(reels[i].pins.relay, OUTPUT);
    digitalWrite(reels[i].pins.relay, RELAY_OFF);
    pinModeSwitch(reels[i].pins.zero);
  }

  for (int i = 0; i < 4; i++) {
    if (REEL_ENABLED[i]) homeReel(reels[i], i);
  }
  Serial.printf("Homing complete at %lu ms\n", millis());

  pinMode(MODE_SWITCH_PIN, INPUT_PULLUP);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  uint32_t wifiStartMs = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wifiStartMs > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println();
      Serial.println("WiFi connect timed out -- rebooting.");
      delay(100);  // let the Serial line above actually get out before reset
      ESP.restart();
    }
    delay(500);
    Serial.print(".");
  }
  Serial.printf(" connected at %lu ms.\n", millis());

  configTzTime(TZ_STRING, NTP_SERVER);
}

// ---------------- Target setting ----------------
void setReelTarget(Reel &r, int targetDigit) {
  if (r.position < 0) return; // not homed yet
  int steps = (targetDigit - r.position + 10) % 10;
  r.stepsRemaining = steps;
}

bool is12HourMode() {
  return digitalRead(MODE_SWITCH_PIN) == LOW;
}

void applyTimeToTargets(int hh24, int mm) {
  int displayHour = hh24;

  if (is12HourMode()) {
    displayHour = hh24 % 12;
    if (displayHour == 0) displayHour = 12; // 12-hour clocks show 12, not 0
  }

  int digits[4] = { displayHour / 10, displayHour % 10, mm / 10, mm % 10 };
  for (int i = 0; i < 4; i++) {
    if (REEL_ENABLED[i]) setReelTarget(reels[i], digits[i]);
  }
}

// ---------------- Per-reel non-blocking update ----------------
void updateReel(Reel &r, int index) {
  uint32_t now = millis();

  switch (r.state) {
    case IDLE:
      if (r.stepsRemaining > 0) {
        int nextPosition = (r.position + 1) % 10;
        Serial.printf("Firing Relay %d (%s) to increment from %d to %d\n",
                      index + 1, REEL_NAMES[index], r.position, nextPosition);
        digitalWrite(r.pins.relay, RELAY_ON);
        r.stateStartMs = now;
        r.state = ENERGIZED;
      }
      break;

    case ENERGIZED:
      if (now - r.stateStartMs > PULSE_TIMEOUT_MS[index]) {
        digitalWrite(r.pins.relay, RELAY_OFF);
        r.position = (r.position + 1) % 10;
        r.stepsRemaining--;
        r.stateStartMs = now;
        r.state = SETTLING;
      }
      break;

    case SETTLING:
      if (now - r.stateStartMs > SETTLE_MS) {
        r.state = IDLE;

        // Ground-truth check: whenever a reel finishes a stepping sequence and should
        // now be sitting at the digit the zero switch trips on, confirm the physical
        // switch agrees. A mismatch means a double-fire or missed step drifted the
        // tracked position — re-home this one reel to correct it, rather than silently
        // trusting the open-loop step count.
        if (r.stepsRemaining == 0 && r.position == ZERO_SWITCH_DIGIT[index] && !isAtZero(r.pins.zero)) {
          Serial.printf("Reel %d (%s) drift detected: expected zero but switch disagrees — re-homing.\n",
                        index + 1, REEL_NAMES[index]);
          homeReel(r, index);
        }
      }
      break;

    case HOMING:
      // homing is only done blocking at boot in this version; unused here
      break;
  }
}

// ---------------- Main loop ----------------
int lastMinuteApplied = -1;
bool firstSyncLogged = false;

void loop() {
  // Advance reels sequentially, not concurrently: only the first reel (in H-tens,
  // H-units, M-tens, M-units order) that still has pending steps or is mid-pulse gets
  // serviced this pass. The rest are left untouched until it's fully idle again, so a
  // multi-digit change (e.g. 12:59 -> 1:00) visibly cascades left to right one reel at
  // a time instead of all reels clicking together.
  for (int i = 0; i < 4; i++) {
    if (!REEL_ENABLED[i]) continue;
    if (reels[i].stepsRemaining > 0 || reels[i].state != IDLE) {
      updateReel(reels[i], i);
      break;
    }
  }

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 10)) {
    if (!firstSyncLogged) {
      Serial.printf("First NTP sync at %lu ms\n", millis());
      firstSyncLogged = true;
    }
    if (timeinfo.tm_min != lastMinuteApplied) {
      char timeStr[32];
      strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
      Serial.printf("NTP local time: %s\n", timeStr);
      applyTimeToTargets(timeinfo.tm_hour, timeinfo.tm_min);
      lastMinuteApplied = timeinfo.tm_min;
    }
  }
}

#elif defined(RELAY_CLICK_TEST)

// ---------------- TEMPORARY: relay bench test ----------------
// Cycles through the 4 relays in use, one at a time: energize for 1s, de-energize for 1s,
// then move to the next. No WiFi, no switches, no homing — just relay clicks and LEDs.
int testReelIndex = 0;

void setup() {
  Serial.begin(115200);
  Serial.println("RELAY_CLICK_TEST mode — cycling relays one at a time.");

  for (int i = 0; i < 4; i++) {
    pinMode(REEL_PINS[i].relay, OUTPUT);
    digitalWrite(REEL_PINS[i].relay, RELAY_OFF);
  }
}

void loop() {
  uint8_t pin = REEL_PINS[testReelIndex].relay;

  Serial.printf("Energizing %s (pin %d)\n", REEL_NAMES[testReelIndex], pin);
  digitalWrite(pin, RELAY_ON);
  delay(1000);

  Serial.printf("De-energizing %s (pin %d)\n", REEL_NAMES[testReelIndex], pin);
  digitalWrite(pin, RELAY_OFF);
  delay(1000);

  testReelIndex = (testReelIndex + 1) % 4;
}

#elif defined(ZERO_HOME_TEST)

// ---------------- TEMPORARY: isolated zero-home test (all 4 reels) ----------------
// Homes each reel to zero in turn, at PULSE_INTERVAL_MS (250ms) -- the base testing speed
// confirmed reliable on Min 1, Min 10, and Hour 1. No WiFi, no ZERO_SWITCH_DIGIT
// calibration applied. Runs once at boot then stops; re-run by rebooting/reflashing as
// often as needed while working on Hour 10 mechanically.
const uint32_t PULSE_INTERVAL_MS = 250;  // established base testing speed

void testHomeReel(int index) {
  ReelPins pins = REEL_PINS[index];

  pinMode(pins.relay, OUTPUT);
  digitalWrite(pins.relay, RELAY_OFF);
  pinModeSwitch(pins.zero);

  Serial.printf("--- Homing %s ---\n", REEL_NAMES[index]);

  if (isAtZero(pins.zero)) {
    Serial.println("0 found (already at zero before any pulses).");
    return;
  }

  for (int attempts = 0; attempts < 15; attempts++) {
    Serial.println("Pulsing");
    digitalWrite(pins.relay, RELAY_ON);
    delay(PULSE_TIMEOUT_MS[index]);
    digitalWrite(pins.relay, RELAY_OFF);

    // Wait out the rest of the interval before checking, giving the switch more
    // settle time than a tight back-to-back pulse cycle would.
    delay(PULSE_INTERVAL_MS - PULSE_TIMEOUT_MS[index]);

    if (isAtZero(pins.zero)) {
      Serial.println("0 found");
      return;
    }
    Serial.println("0 not found");
  }

  Serial.println("FAILED: zero switch never tripped after 15 pulses.");
}

void setup() {
  Serial.begin(115200);
  Serial.println("ZERO_HOME_TEST mode — homing all 4 reels, nothing else.");

  testHomeReel(3);  // Min 1 / M-units
  testHomeReel(2);  // Min 10 / M-tens
  testHomeReel(1);  // Hour 1 / H-units
  testHomeReel(0);  // Hour 10 / H-tens

  Serial.println("Done. No further pulses will occur.");
}

void loop() {
  // Intentionally empty — this mode only runs once at boot.
}

#endif
