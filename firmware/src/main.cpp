/**
 * ATEM Web Tally - https://github.com/jithin001/atem-web-tally
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
 * Power: WiFi modem sleep (WIFI_PS_MIN_MODEM). Tally arrives as UDP broadcast.
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

// Portrait layout (SCREEN_ROTATION 0/2 -> 135 x 240):
//   top-right: battery %      center: big letter      bottom: camera name
static void drawChrome(uint16_t fg, uint16_t bg) {
  M5.Display.setTextColor(fg, bg);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(2);
  if (battPct >= 0) {
    char b[8]; snprintf(b, sizeof(b), "%d%%", (int)battPct);
    M5.Display.setTextDatum(top_right);
    M5.Display.drawString(b, M5.Display.width() - 4, 4);
  }
  char cam[32];
  const char* lbl = camLabel(myCamera);
  if (myCamera == 0)  snprintf(cam, sizeof(cam), "--");
  else if (lbl[0])    snprintf(cam, sizeof(cam), "%s", lbl);
  else                snprintf(cam, sizeof(cam), "Cam %d", myCamera);
  M5.Display.setTextDatum(bottom_center);
  M5.Display.drawString(cam, M5.Display.width() / 2, M5.Display.height() - 6);
  M5.Display.setTextSize(1);
}

static void drawBigLetter(const char* letter, uint16_t fg, uint16_t bg, uint8_t scale) {
  // fonts::Font8 is 7-segment numerals only (no letters) -> use a real typeface.
  M5.Display.setTextColor(fg, bg);
  M5.Display.setFont(&fonts::FreeSansBold24pt7b);
  M5.Display.setTextSize(scale);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(letter, M5.Display.width() / 2, M5.Display.height() / 2);
  M5.Display.setTextSize(1);
}

static void drawBig(const char* letter, uint16_t bg, uint16_t fg, uint8_t brightness) {
  screenWake(brightness);
#if TALLY_STYLE == 1
  // Dot style: black field, large colored disc, NO letter. Same power as
  // full-field on this LCD (backlight is the cost), but far less light spill.
  M5.Display.fillScreen(TFT_BLACK);
  int r = (M5.Display.width() / 2) - DOT_MARGIN;
  M5.Display.fillCircle(M5.Display.width() / 2, M5.Display.height() / 2, r, bg);
  (void)letter;   // dot-only by design; letter is used in style 0
  drawChrome(TFT_WHITE, TFT_BLACK);
#else
  M5.Display.fillScreen(bg);
  drawBigLetter(letter, fg, bg, 3);
  drawChrome(fg, bg);
#endif
}

static void drawIdentity(int y) {
  // Small "who am I" line: name + MAC tail.
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(bottom_center);
  char line[48];
  snprintf(line, sizeof(line), "%s [%s]", myName, macStr + 9);
  M5.Display.drawString(line, M5.Display.width() / 2, y);
}

static void drawLost() {
  screenWake(60);
  M5.Display.fillScreen(TFT_BLACK);
  drawBigLetter("?", TFT_ORANGE, TFT_BLACK, 2);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString((myCamera == 0) ? "unassigned" : "no signal",
                        M5.Display.width() / 2, M5.Display.height() / 2 + 52);
  if (myCamera == 0) M5.Display.drawString("press button", M5.Display.width() / 2, M5.Display.height() / 2 + 64);
  drawChrome(TFT_WHITE, TFT_BLACK);
  drawIdentity(M5.Display.height() - 28);
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
    char n[4]; snprintf(n, sizeof(n), "%d", pendingCam);
    drawBigLetter(n, TFT_WHITE, TFT_NAVY, 2);
    const char* lbl = camLabel(pendingCam);
    if (lbl[0]) {
      M5.Display.setFont(&fonts::Font2);
      M5.Display.drawString(lbl, M5.Display.width() / 2, M5.Display.height() / 2 + 48);
    }
  }
  M5.Display.setFont(&fonts::Font0);
  M5.Display.setTextSize(1);
  M5.Display.setTextDatum(bottom_center);
  M5.Display.drawString("press = next", M5.Display.width() / 2, M5.Display.height() - 26);
  M5.Display.drawString("wait = save",  M5.Display.width() / 2, M5.Display.height() - 16);
  drawIdentity(M5.Display.height() - 4);
}

static void drawBatteryOverlay() {
  screenWake(40);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(battPct <= LOW_BATT_PCT ? TFT_RED : TFT_WHITE, TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.setFont(&fonts::Font4);
  M5.Display.drawString("BATT", M5.Display.width() / 2, M5.Display.height() / 2 - 40);
  char pct[8]; snprintf(pct, sizeof(pct), "%d%%", (int)battPct);
  M5.Display.drawString(pct, M5.Display.width() / 2, M5.Display.height() / 2 - 10);
  if (battPct <= LOW_BATT_PCT && etaMin > 0) {
    char line2[24];
    snprintf(line2, sizeof(line2), "~%d min", etaMin);
    M5.Display.setFont(&fonts::Font2);
    M5.Display.drawString(line2, M5.Display.width() / 2, M5.Display.height() / 2 + 24);
  }
  M5.Display.setFont(&fonts::Font0);
  drawIdentity(M5.Display.height() - 4);
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
  doc["mv"]   = M5.Power.getBatteryVoltage();          // battery millivolts
  doc["chg"]  = (int)M5.Power.isCharging();            // 0=no, 1=yes, 2=unknown
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
  // Brightness changes apply immediately if the screen is currently lit.
  if (state == TallyState::PROGRAM || state == TallyState::PREVIEW) drawnState = TallyState::BOOT;
  // One-shot remote commands from the admin page.
  const char* cmd = doc["cmd"] | "";
  if (strcmp(cmd, "off") == 0) {
    Serial.println("[cmd] power off requested by admin");
    screenWake(80);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setFont(&fonts::Font4);
    M5.Display.setTextDatum(middle_center);
    M5.Display.drawString("POWER", M5.Display.width() / 2, M5.Display.height() / 2 - 16);
    M5.Display.drawString("OFF",   M5.Display.width() / 2, M5.Display.height() / 2 + 16);
    sendStatus();               // final report so the admin sees a fresh lastSeen
    delay(1500);
    M5.Power.powerOff();        // NOTE: with USB plugged, the PMIC may restart instead
  } else if (strcmp(cmd, "reboot") == 0) {
    Serial.println("[cmd] reboot requested by admin");
    delay(200);
    ESP.restart();
  }
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
  M5.Display.drawString("SAVED", M5.Display.width() / 2, M5.Display.height() / 2 - 30);
  char line[32];
  if (myCamera == 0) snprintf(line, sizeof(line), "unassigned");
  else {
    const char* lbl = camLabel(myCamera);
    if (lbl[0]) snprintf(line, sizeof(line), "%d: %s", myCamera, lbl);
    else        snprintf(line, sizeof(line), "Input %d", myCamera);
  }
  M5.Display.setFont(&fonts::Font2);
  M5.Display.drawString(line, M5.Display.width() / 2, M5.Display.height() / 2 + 10);
  M5.Display.setFont(&fonts::Font0);
  drawIdentity(M5.Display.height() - 4);
  overlayUntilMs = millis() + ASSIGN_CONFIRM_MS;
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
  Serial.begin(115200);
  delay(1500);
  Serial.println("[boot] alive, before M5.begin");
  auto cfg = M5.config();
  M5.begin(cfg);   // board selection: -DM5GFX_BOARD in platformio.ini (see README)
  Serial.printf("[boot] fw %s board=%d batt=%d%%\n", FW_VERSION, (int)M5.getBoard(), M5.Power.getBatteryLevel());
  Serial.printf("[boot] display %dx%d\n", M5.Display.width(), M5.Display.height());
  Serial.printf("[boot] batt %dmV charging=%d\n", M5.Power.getBatteryVoltage(), (int)M5.Power.isCharging());
  M5.Display.setRotation(SCREEN_ROTATION);
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
  Serial.printf("[wifi] connected ip=%s rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // MAX_MODEM skips beacons and misses broadcasts
  setCpuFrequencyMhz(80);              // 240 MHz -> 80 MHz: ~30 mA saved; WiFi needs >= 80
  Serial.printf("[boot] cpu %lu MHz\n", (unsigned long)getCpuFrequencyMhz());

  snprintf(macStr, sizeof(macStr), "%s", WiFi.macAddress().c_str());

  // Boot identity screen: lets the swap crew see who this unit is immediately.
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setFont(&fonts::Font2);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(myName, M5.Display.width() / 2, M5.Display.height() / 2 - 12);
  M5.Display.setFont(&fonts::Font0);
  drawIdentity(M5.Display.height() - 4);

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
