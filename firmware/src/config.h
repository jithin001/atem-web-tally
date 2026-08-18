#pragma once

// ---- WiFi (dedicated production router / VLAN recommended) ----
#define WIFI_SSID       "YOUR_TALLY_SSID"
#define WIFI_PASS       "YOUR_TALLY_PASSWORD"

// ---- Ports (must match server config.json) ----
#define TALLY_PORT      7411      // listen: server broadcast
#define STATUS_PORT     7412      // send:   status reports (server replies unicast with our config)
#define SERVER_DISCOVERY_BROADCAST 1  // status reports go to 255.255.255.255 -> no server IP hardcoded

// ---- Behavior tunables ----
#define LOST_SIGNAL_MS      3000  // no tally packet for this long -> lost-signal state
#define HEARTBEAT_PERIOD_MS 5000  // idle heartbeat blink interval
#define HEARTBEAT_ON_MS     60    // blink duration
#define STATUS_PERIOD_MS    30000 // how often we report status / re-fetch config
#define BATT_SAMPLE_MS      60000 // battery sampling for drain-rate EMA
#define BATT_OVERLAY_MS     4000  // battery overlay display time
#define BATT_OVERLAY_PERIOD_NORMAL_MIN 15
#define BATT_OVERLAY_PERIOD_LOW_MIN    5
#define LOW_BATT_PCT        20

// ---- Assignment (main button) ----
#define ASSIGN_TIMEOUT_MS   4000  // no presses for this long -> save selection
#define ASSIGN_CONFIRM_MS   2500  // "SAVED -> input" confirmation screen duration
#define DEFAULT_CAM_COUNT   4     // until the server tells us maxCam
#define MAX_CAM_SLOTS       9     // storage for input name labels (1..8)

// ---- Hardware ----
// VERIFY against the M5StickS3 schematic before flashing:
//   - LED pin: set to the onboard LED GPIO. Set to -1 to use a brief dim
//     screen pulse as the heartbeat instead (slightly more power).
#define LED_PIN             -1
#define LED_ACTIVE_HIGH     1

// Firmware version reported to server
#define FW_VERSION          "1.0.0"
