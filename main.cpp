/*
  MusicBox (main.cpp) - ESP32 captive-portal music player for a passive piezo buzzer

  - ESP32 starts a Wi-Fi hotspot "MusicBox". Phones that join get the
    "Sign in to network" popup (captive portal) which opens the player page.
  - The page converts any music file (MIDI, MP3, WAV, M4A, OGG...) in the
    BROWSER into a tiny monophonic note list (4 bytes per note), so only a few
    KB are sent to the board, never the original 10 MB file.
  - Notes are stored in LittleFS and played on the buzzer. Play / Pause / Stop.
  - Built-in LED flickers while data is being received.

  Wiring:  buzzer (+) -> GPIO 25     buzzer (-) -> GND
  Board:   ESP32 Dev Module (NodeMCU-32S), Arduino-ESP32 core 2.x or 3.x
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LittleFS.h>

#include "index_html.h"

// ---------- settings ----------
#define BUZZER_PIN 25
#define LED_PIN 2                 // built-in LED on NodeMCU-32S
#define LED_ON HIGH
static const char* AP_SSID = "MusicBox";   // open network, no password
static const char* AP_PASS = "";           // set 8+ chars to require a password
static const uint32_t MAX_SONG_BYTES = 300000;  // 75,000 notes
static const char* SONG_PATH = "/song.bin";

// ---------- buzzer (works on core 2.x and 3.x) ----------
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  void buzzInit() { ledcAttach(BUZZER_PIN, 2000, 10); ledcWriteTone(BUZZER_PIN, 0); }
  void buzz(uint32_t f) { ledcWriteTone(BUZZER_PIN, f); }
#else
  #define BUZZ_CH 0
  void buzzInit() { ledcSetup(BUZZ_CH, 2000, 10); ledcAttachPin(BUZZER_PIN, BUZZ_CH); ledcWriteTone(BUZZ_CH, 0); }
  void buzz(uint32_t f) { ledcWriteTone(BUZZ_CH, f); }
#endif

void led(bool on) { digitalWrite(LED_PIN, on ? LED_ON : !LED_ON); }
void blinkLed(int n) { for (int i = 0; i < n; i++) { led(true); delay(80); led(false); delay(80); } }

// ---------- globals ----------
const IPAddress apIP(192, 168, 4, 1);
DNSServer dnsServer;
WebServer server(80);

enum State { IDLE, PLAYING, PAUSED };
State state = IDLE;
File song;
uint32_t songNotes = 0, songMs = 0;     // info about the stored song
uint32_t idx = 0, playedMs = 0;         // playback position
uint16_t curF = 0, curD = 0;            // current note
uint32_t noteEnd = 0, pausedLeft = 0;

// ---------- song helpers ----------
void scanSong() {
  songNotes = 0; songMs = 0;
  File f = LittleFS.open(SONG_PATH, "r");
  if (!f) return;
  uint8_t b[4];
  while (f.read(b, 4) == 4) { songNotes++; songMs += (uint16_t)(b[2] | (b[3] << 8)); }
  f.close();
}

bool nextNote() {
  uint8_t b[4];
  if (song.read(b, 4) != 4) return false;
  curF = b[0] | (b[1] << 8);
  curD = b[2] | (b[3] << 8);
  if (curD == 0) curD = 1;
  return true;
}

void stopPlayback() {
  buzz(0);
  if (song) song.close();
  state = IDLE; idx = 0; playedMs = 0;
}

void startPlayback() {
  if (songNotes == 0 || state == PLAYING) return;
  if (state == PAUSED) {                       // resume
    buzz(curF);
    noteEnd = millis() + pausedLeft;
    state = PLAYING;
    return;
  }
  song = LittleFS.open(SONG_PATH, "r");
  if (!song) return;
  idx = 0; playedMs = 0;
  if (!nextNote()) { stopPlayback(); return; }
  buzz(curF);
  noteEnd = millis() + curD;
  state = PLAYING;
}

void pausePlayback() {
  if (state != PLAYING) return;
  int32_t left = (int32_t)(noteEnd - millis());
  pausedLeft = left > 0 ? left : 0;
  buzz(0);
  state = PAUSED;
}

void tickPlayback() {
  if (state != PLAYING) return;
  if ((int32_t)(millis() - noteEnd) < 0) return;
  playedMs += curD; idx++;
  uint32_t base = noteEnd;
  if (!nextNote()) { stopPlayback(); return; }
  buzz(curF);
  noteEnd = base + curD;
  if ((int32_t)(millis() - noteEnd) > 0) noteEnd = millis() + curD;  // fell far behind, resync
}

// ---------- upload ----------
File upFile;
uint32_t upBytes = 0;
bool upOk = false;

void handleUpload() {
  HTTPUpload& u = server.upload();
  if (u.status == UPLOAD_FILE_START) {
    stopPlayback();
    LittleFS.remove(SONG_PATH);
    upFile = LittleFS.open(SONG_PATH, "w");
    upBytes = 0;
    upOk = (bool)upFile;
    led(true);
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (upFile && upOk) {
      if (upBytes + u.currentSize > MAX_SONG_BYTES) upOk = false;
      else { upFile.write(u.buf, u.currentSize); upBytes += u.currentSize; }
    }
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));      // flicker per chunk
  } else if (u.status == UPLOAD_FILE_END) {
    if (upFile) upFile.close();
    if (upBytes == 0 || (upBytes % 4) != 0) upOk = false;
    if (!upOk) LittleFS.remove(SONG_PATH);
    scanSong();
    led(false);
    blinkLed(3);
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (upFile) upFile.close();
    LittleFS.remove(SONG_PATH);
    scanSong();
    led(false);
  }
}

void handleUploadDone() {
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"ok\":%s,\"notes\":%lu,\"ms\":%lu}",
           upOk ? "true" : "false", (unsigned long)songNotes, (unsigned long)songMs);
  server.send(upOk ? 200 : 400, "application/json", buf);
}

// ---------- web page ----------

// ---------- routes ----------
void redirectToPortal() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}

void handleStatus() {
  uint32_t elapsed = playedMs;
  if (state == PLAYING) {
    int32_t left = (int32_t)(noteEnd - millis());
    if (left < 0) left = 0;
    elapsed += curD - left;
  } else if (state == PAUSED) {
    elapsed += curD - pausedLeft;
  }
  const char* s = state == PLAYING ? "playing" : (state == PAUSED ? "paused" : "idle");
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"elapsed\":%lu,\"total\":%lu,\"notes\":%lu}",
           s, (unsigned long)elapsed, (unsigned long)songMs, (unsigned long)songNotes);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  led(false);
  buzzInit();
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  scanSong();

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, strlen(AP_PASS) >= 8 ? AP_PASS : nullptr);
  dnsServer.start(53, "*", apIP);        // every hostname -> us (captive portal)

  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", INDEX_HTML);
  });
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/play", HTTP_POST, []() { startPlayback(); handleStatus(); });
  server.on("/pause", HTTP_POST, []() { pausePlayback(); handleStatus(); });
  server.on("/stop", HTTP_POST, []() { stopPlayback(); handleStatus(); });
  server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
  server.onNotFound(redirectToPortal);   // captive-portal detection URLs land here
  server.begin();

  // startup beep so you know the buzzer wiring works
  buzz(1200); delay(120); buzz(1800); delay(120); buzz(0);
  Serial.printf("MusicBox ready. Join Wi-Fi \"%s\" then open http://192.168.4.1\n", AP_SSID);
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  tickPlayback();
  delay(1);
}