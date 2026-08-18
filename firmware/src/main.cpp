/**
 * ATEM Web Tally - https://github.com/YOURNAME/atem-web-tally
 * MIT License - Built by Jithin Mathew (https://jithinmathew.com)
 *
 * ATEM Tally - M5StickS3 firmware
 * --------------------------------
 * States:
 *   PROGRAM      full red screen, big "L", cam # + battery in corner (pgmBright)
 *   PREVIEW      full green screen, big "P", cam # + battery in corner (pvwBright)
 *   IDLE         screen OFF, heartbeat blink every HEARTBEAT_PERIOD_MS
 *   LOST SIGNAL  no broadcast for LOST_SIGNAL_MS: amber "?" + unit name/MAC
 *   ASSIGN       main button: press to enter, press to cycle input 1..N..unassigned,
 *                4 s of no presses locks it in and informs the server (saved by MAC)
 *   BATTERY      overlay every 15 min (5 min + ETA when <= LOW_BATT_PCT)
 *
 * Power: WiFi modem sleep (WIFI_PS_MAX_MODEM). Tally arrives as UDP broadcast.
 * Config (camera, name, brightness, input names) is server-side, keyed by MAC.
 */

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include "config.h"

enum class TallyState { BOOT, IDLE, PREVIEW, PROGRAM, LOST };

static WiFiUDP tallyUdp;
static WiFiUDP statusUdp;

static TallyState state = TallyState::BOOT;
static TallyState drawnState = TallyState::BOOT;

static uint8_t  myCamera   = 0;        // 0 = unassigned
static uint8_t  maxCam     = DEFAULT_CAM_COUNT;
static uint8_t  pgmBright  = 255;
static uint8_t  pvwBright  = 60;
static char     myName[32] = "unnamed";
static char     macStr[18] = {0};
static char     inputNames[MAX_CAM_SLOTS][20] = {{0}};  // 1-based labels from server

static uint32_t lastPacketMs   = 0;
static uint32_t lastStatusMs   = 0;
static uint32_t lastHbMs       = 0;
static uint32_t lastBattSample = 0;
static uint32_t lastBattShowMs = 0;
static uint32_t overlayUntilMs = 0;

// Assign mode (main button)
static bool     assignMode   = false;
static int      pendingCam   = 0;
static uint32_t assignUntilMs = 0;

// Battery drain-rate EMA (percent per minute) for ETA estimation.
static float    battPct     = -1;
static float    drainPerMin = 0;
static int      etaMin      = -1;
static const float DRAIN_ALPHA = 0.25f;

static bool     screenOn = false;

// ---------------------------------------------------------------------------
// Display helpers
// ---------------------------------------------------------------------------
static void screenOff() {
  if (!screenOn && drawnState != TallyState::BOOT) return;
  M5.Display.setBrightness(0);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.sleep();
  screenOn = false;
}

static void screenWake(uint8_t brightness) {
  M5.Display.wakeup();
  M5.Display.setBrightness(brightness);
  screenOn = true;
}

static const char* camLabel(int cam) {
  if (cam < 1 || cam >= MAX_CAM_SLOTS) return "";
  return inputNames[cam][0] ? inputNames[cam] : "";
}

static void drawCorner() {
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(2);
  M5.Display.setTextDatum(bottom_left);
  M5.Display.setCursor(4, M5.Display.height() - 20);
  const char* lbl = camLabel(myCamera);
  if (lbl[0]) M5.Display.printf("%d %s", myCamera, lbl);
  else        M5.Display.printf("CAM %d", myCamera);
  if (battPct >= 0) {
    M5.Display.setCursor(M5.Display.width() - 60, M5.Display.height() - 20);
    M5.Display.printf("%d%%", (int)battPct);
  }
  M5.Display.setTextSize(1);
}

static void drawBig(const char* letter, uint16_t bg, uint16_t fg, uint8_t brightness) {
  screenWake(brightness);
  M5.Display.fillScreen(bg);
  M5.Display.setTextColor(fg, bg);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setTextSize(1);
  M5.Display.setFont(&fonts::Font8);
  M5.Display.drawString(letter, M5.Display.width() / 2, M5.Display.height() / 2 - 6);
  drawCorner();
}

static void drawIdentity(int y) {
  // Small "who am I" footer: name + MAC tail.
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(bottom_center);
  char line[48];
  snprintf(line, sizeof(line), "%s  [%s]", myName, macStr + 9); // last 3 MAC octets
  M5.Display.drawString(line, M5.Display.width() / 2, y);
}

static void drawLost() {
  screenWake(60);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_ORANGE, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::Font8);
  M5.Display.drawString("?", M5.Display.width() / 2, M5.Display.height() / 2 - 10);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  const char* why = (myCamera == 0) ? "unassigned - press button" : "no signal";
  M5.Display.setTextDatum(bottom_center);
  M5.Display.drawString(why, M5.Display.width() / 2, M5.Display.height() - 14);
  drawIdentity(M5.Display.height() - 2);
}

static void drawAssign() {
  screenWake(150);
  M5.Display.fillScreen(TFT_NAVY);
  M5.Display.setTextColor(TFT_WHITE, TFT_NAVY);
  M5.Display.setTextDatum(middle_center);
  if (pendingCam == 0) {
    M5.Display.setFont(&fonts::Font4);
    M5.Display.drawString("UNASSIGN", M5.Display.width() / 2, M5.Display.height() / 2 - 10);
  } else {
    M5.Display.setFont(&fonts::Font8);
    char n[4]; snprintf(n, sizeof(n), "%d", pendingCam);
    M5.Display.drawString(n, M5.Display.width() / 2, M5.Display.height() / 2 - 14);
    const char* lbl = camLabel(pendingCam);
    if (lbl[0]) {
      M5.Display.setFont(&fonts::Font2);
      M5.Display.drawString(lbl, M5.Display.width() / 2, M5.Display.height() / 2 + 26);
    }
  }
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(bottom_center);
  M5.Display.drawString("press = next   wait = save", M5.Display.width() / 2, M5.Display.height() - 12);
  drawIdentity(M5.Display.height() - 2);
}

static void drawBatteryOverlay() {
  screenWake(40);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(battPct <= LOW_BATT_PCT ? TFT_RED : TFT_WHITE, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::Font4);
  char line1[24];
  snprintf(line1, sizeof(line1), "BATT %d%%", (int)battPct);
  M5.Display.drawString(line1, M5.Display.width() / 2, M5.Display.height() / 2 - 18);
  if (battPct <= LOW_BATT_PCT && etaMin > 0) {
    char line2[24];
    snprintf(line2, sizeof(line2), "~%d min left", etaMin);
    M5.Display.drawString(line2, M5.Display.width() / 2, M5.Display.height() / 2 + 12);
  }
  M5.Display.setFont(&fonts::Font0);
  drawIdentity(M5.Display.height() - 2);
  overlayUntilMs = millis() + BATT_OVERLAY_MS;
}

// ---------------------------------------------------------------------------
// Heartbeat (LED if available, otherwise brief dim screen pulse)
// ---------------------------------------------------------------------------
static void hbPulse(uint8_t times) {
  for (uint8_t i = 0; i < times; i++) {
#if LED_PIN >= 0
    digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? HIGH : LOW);
    delay(HEARTBEAT_ON_MS);
    digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? LOW : HIGH);
#else
    screenWake(8);
    M5.Display.fillScreen(state == TallyState::LOST ? TFT_ORANGE : TFT_DARKGREEN);
    delay(HEARTBEAT_ON_MS);
    screenOff();
#endif
    if (i + 1 < times) delay(120);
  }
}

// ---------------------------------------------------------------------------
// Battery sampling + ETA
// ---------------------------------------------------------------------------
static void sampleBattery() {
  int lvl = M5.Power.getBatteryLevel();   // VERIFY: M5Unified support for StickS3
  if (lvl < 0) return;
  uint32_t now = millis();
  if (battPct >= 0 && lastBattSample > 0) {
    float minutes = (now - lastBattSample) / 60000.0f;
    if (minutes > 0.5f) {
      float rate = (battPct - lvl) / minutes;
      if (rate >= 0 && rate < 5) {
        drainPerMin = (drainPerMin == 0) ? rate : (DRAIN_ALPHA * rate + (1 - DRAIN_ALPHA) * drainPerMin);
      }
    }
  }
  battPct = lvl;
  lastBattSample = now;
  etaMin = (drainPerMin > 0.05f) ? (int)(battPct / drainPerMin) : -1;
}

// ---------------------------------------------------------------------------
// Networking
// ---------------------------------------------------------------------------
static void sendStatus(int setCam = -1) {
  JsonDocument doc;
  doc["mac"]  = macStr;
  doc["batt"] = (battPct >= 0) ? (int)battPct : (int)-1;
  if (battPct >= 0 && battPct <= LOW_BATT_PCT && etaMin > 0) doc["eta"] = etaMin;
  doc["rssi"] = WiFi.RSSI();
  doc["up"]   = millis() / 1000;
  doc["fw"]   = FW_VERSION;
  doc["cam"]  = myCamera;
  if (setCam >= 0) doc["setCam"] = setCam;   // device-initiated assignment (button)
  char buf[224];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  statusUdp.beginPacket(IPAddress(255, 255, 255, 255), STATUS_PORT);
  statusUdp.write((const uint8_t*)buf, n);
  statusUdp.endPacket();
  lastStatusMs = millis();
}

static void handleConfigReply() {
  int sz = statusUdp.parsePacket();
  if (sz <= 0) return;
  char buf[512];
  int n = statusUdp.read(buf, sizeof(buf) - 1);
  if (n <= 0) return;
  buf[n] = 0;
  JsonDocument doc;
  if (deserializeJson(doc, buf)) return;
  uint8_t newCam = doc["cam"] | 0;
  if (!assignMode && newCam != myCamera) { myCamera = newCam; drawnState = TallyState::BOOT; }
  pgmBright = doc["pgmBright"] | 255;
  pvwBright = doc["pvwBright"] | 60;
  maxCam    = doc["maxCam"]    | DEFAULT_CAM_COUNT;
  if (maxCam >= MAX_CAM_SLOTS) maxCam = MAX_CAM_SLOTS - 1;
  if (doc["name"].is<const char*>()) strlcpy(myName, doc["name"], sizeof(myName));
  if (doc["inputs"].is<JsonObject>()) {
    for (int i = 1; i <= maxCam; i++) {
      char key[4]; snprintf(key, sizeof(key), "%d", i);
      if (doc["inputs"][key].is<const char*>())
        strlcpy(inputNames[i], doc["inputs"][key], sizeof(inputNames[i]));
    }
  }
}

static void handleTallyPacket() {
  int sz = tallyUdp.parsePacket();
  if (sz < 4) { if (sz > 0) tallyUdp.flush(); return; }
  uint8_t buf[64];
  int n = tallyUdp.read(buf, sizeof(buf));
  if (n < 4 || buf[0] != 'T' || buf[1] != '1') return;
  uint8_t camCount = buf[3];
  if (n < 4 + camCount) return;
  lastPacketMs = millis();

  uint8_t bits = (myCamera >= 1 && myCamera <= camCount) ? buf[3 + myCamera] : 0;
  TallyState next = (bits & 0x01) ? TallyState::PROGRAM
                  : (bits & 0x02) ? TallyState::PREVIEW
                  : TallyState::IDLE;
  if (myCamera == 0) next = TallyState::LOST;
  state = next;
}

// ---------------------------------------------------------------------------
// Assign mode (main button)
// ---------------------------------------------------------------------------
static void drawAssignSaved() {
  screenWake(150);
  M5.Display.fillScreen(TFT_DARKGREEN);
  M5.Display.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::Font4);
  M5.Display.drawString("SAVED", M5.Display.width() / 2, M5.Display.height() / 2 - 22);
  char line[32];
  if (myCamera == 0) snprintf(line, sizeof(line), "unassigned");
  else {
    const char* lbl = camLabel(myCamera);
    if (lbl[0]) snprintf(line, sizeof(line), "%d: %s", myCamera, lbl);
    else        snprintf(line, sizeof(line), "Input %d", myCamera);
  }
  M5.Display.setFont(&fonts::Font2);
  M5.Display.drawString(line, M5.Display.width() / 2, M5.Display.height() / 2 + 12);
  M5.Display.setFont(&fonts::Font0);
  drawIdentity(M5.Display.height() - 2);
  overlayUntilMs = millis() + ASSIGN_CONFIRM_MS;   // reuse overlay hold-off
}

static void handleAssignButton() {
  if (M5.BtnA.wasPressed()) {
    if (!assignMode) {
      assignMode = true;
      pendingCam = myCamera;
    } else {
      pendingCam = (pendingCam + 1) % (maxCam + 1);   // 1..maxCam then 0 (unassign)
      if (pendingCam == 0 && myCamera == 0) pendingCam = 1; // skip no-op for fresh units
    }
    assignUntilMs = millis() + ASSIGN_TIMEOUT_MS;
    drawAssign();
  }
  if (assignMode && millis() > assignUntilMs) {
    assignMode = false;
    if (pendingCam != myCamera) {
      myCamera = (uint8_t)pendingCam;
      sendStatus(pendingCam);                          // persist on server by MAC
    }
    drawAssignSaved();                                 // visible confirmation, then normal state
    drawnState = TallyState::BOOT;                     // force redraw of real state afterwards
  }
}

// ---------------------------------------------------------------------------
static void render() {
  if (assignMode) return;                              // assign screen owns display
  if (millis() < overlayUntilMs) return;               // battery overlay owns display
  if (state == drawnState) return;
  switch (state) {
    case TallyState::PROGRAM: drawBig("L", TFT_RED,   TFT_WHITE, pgmBright); break;
    case TallyState::PREVIEW: drawBig("P", TFT_GREEN, TFT_BLACK, pvwBright); break;
    case TallyState::IDLE:    screenOff(); break;
    case TallyState::LOST:    drawLost(); break;
    default: break;
  }
  drawnState = state;
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  screenWake(80);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString("Connecting WiFi...", M5.Display.width() / 2, M5.Display.height() / 2);

#if LED_PIN >= 0
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_ACTIVE_HIGH ? LOW : HIGH);
#endif

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(200); M5.update(); }
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

  snprintf(macStr, sizeof(macStr), "%s", WiFi.macAddress().c_str());

  // Boot identity screen: lets the swap crew see who this unit is immediately.
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setFont(&fonts::Font4);
  M5.Display.drawString(myName, M5.Display.width() / 2, M5.Display.height() / 2 - 12);
  M5.Display.setFont(&fonts::Font0);
  drawIdentity(M5.Display.height() - 2);

  tallyUdp.begin(TALLY_PORT);
  statusUdp.begin(0);

  sampleBattery();
  sendStatus();                     // registers us + fetches config (incl. our name)
  lastPacketMs = millis();
  state = TallyState::LOST;
  delay(1200);                      // let the identity screen be readable
}

void loop() {
  M5.update();
  handleAssignButton();
  handleTallyPacket();
  handleConfigReply();

  uint32_t now = millis();

  if (now - lastPacketMs > LOST_SIGNAL_MS) state = TallyState::LOST;

  if (now - lastStatusMs > STATUS_PERIOD_MS) sendStatus();
  if (now - lastBattSample > BATT_SAMPLE_MS) sampleBattery();

  uint32_t period = ((battPct >= 0 && battPct <= LOW_BATT_PCT)
                    ? BATT_OVERLAY_PERIOD_LOW_MIN : BATT_OVERLAY_PERIOD_NORMAL_MIN) * 60000UL;
  if (!assignMode && battPct >= 0 && now - lastBattShowMs > period) {
    lastBattShowMs = now;
    drawBatteryOverlay();
    drawnState = TallyState::BOOT;
  }
  if (overlayUntilMs && now >= overlayUntilMs) { overlayUntilMs = 0; drawnState = TallyState::BOOT; }

  if (!assignMode && (state == TallyState::IDLE || state == TallyState::LOST) && now - lastHbMs > HEARTBEAT_PERIOD_MS) {
    lastHbMs = now;
    bool lowBatt = battPct >= 0 && battPct <= LOW_BATT_PCT;
    if (state == TallyState::IDLE) hbPulse(lowBatt ? 3 : 1);
#if LED_PIN >= 0
    if (state == TallyState::LOST) hbPulse(2);
#endif
  }

  render();
  delay(20);
}
