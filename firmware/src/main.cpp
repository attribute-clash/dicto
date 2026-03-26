#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <SD.h>
#include <driver/i2s_std.h>

// =========================
// Compile-time configuration
// =========================
static constexpr const char *WIFI_SSID = "YOUR_WIFI_SSID";
static constexpr const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
static constexpr const char *SERVER_URL = "https://your-server.example.com/api/v1/audio/upload";
static constexpr const char *API_TOKEN = "YOUR_API_TOKEN";
static constexpr const char *DEVICE_ID = "DCT-001";

// GPIO mapping (adjust for your board)
static constexpr int PIN_BUTTON = 3;
static constexpr int PIN_LED = 2;
static constexpr int PIN_SD_CS = 10;

// I2S mic pins
static constexpr int PIN_I2S_SCK = 4;
static constexpr int PIN_I2S_WS = 5;
static constexpr int PIN_I2S_SD = 6;

// Audio profile
static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr uint16_t BITS_PER_SAMPLE = 16;
static constexpr uint16_t CHANNELS = 1;

// Limits & timers
static constexpr uint32_t MAX_RECORD_MS = 5UL * 60UL * 1000UL;
static constexpr uint32_t DEBOUNCE_MS = 35;
static constexpr uint32_t WIFI_RETRY_MS = 30UL * 1000UL;
static constexpr uint32_t UPLOAD_INTERVAL_MS = 10UL * 1000UL;

// Directories
static constexpr const char *DIR_AUDIO = "/audio";

struct WavHeader {
  char riff[4];
  uint32_t chunkSize;
  char wave[4];
  char fmt[4];
  uint32_t subchunk1Size;
  uint16_t audioFormat;
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char data[4];
  uint32_t subchunk2Size;
};

enum class DeviceState {
  IDLE,
  RECORDING
};

DeviceState state = DeviceState::IDLE;

File activeFile;
String activePath;
uint32_t activeDataBytes = 0;
uint32_t recordStartedAtMs = 0;
uint32_t lastButtonEdgeMs = 0;
bool lastButtonStablePressed = false;

uint32_t sequenceCounter = 0;
unsigned long lastWifiAttemptMs = 0;
unsigned long lastUploadTickMs = 0;

// ---------- helpers ----------
String isoUtcNow() {
  time_t now = time(nullptr);
  if (now <= 100000) return "1970-01-01T00:00:00Z";
  struct tm t;
  gmtime_r(&now, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
  return String(buf);
}

String nowFileStamp() {
  time_t now = time(nullptr);
  struct tm t;
  if (now > 100000) {
    gmtime_r(&now, &t);
  } else {
    uint32_t secs = millis() / 1000;
    t = {};
    t.tm_year = 126; // 2026
    t.tm_mday = 1;
    now = mktime(&t) + secs;
    gmtime_r(&now, &t);
  }

  char buf[32];
  strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &t);
  return String(buf);
}

String markerPath(const String &audioPath) {
  return audioPath + ".sent";
}

bool isSent(const String &audioPath) {
  return SD.exists(markerPath(audioPath));
}

void markSent(const String &audioPath) {
  File f = SD.open(markerPath(audioPath), FILE_WRITE);
  if (f) {
    f.println("sent");
    f.close();
  }
}

void writeWavHeader(File &f, uint32_t dataBytes) {
  WavHeader h;
  memcpy(h.riff, "RIFF", 4);
  h.chunkSize = 36 + dataBytes;
  memcpy(h.wave, "WAVE", 4);
  memcpy(h.fmt, "fmt ", 4);
  h.subchunk1Size = 16;
  h.audioFormat = 1;
  h.numChannels = CHANNELS;
  h.sampleRate = SAMPLE_RATE;
  h.byteRate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
  h.blockAlign = CHANNELS * (BITS_PER_SAMPLE / 8);
  h.bitsPerSample = BITS_PER_SAMPLE;
  memcpy(h.data, "data", 4);
  h.subchunk2Size = dataBytes;

  f.seek(0);
  f.write(reinterpret_cast<uint8_t *>(&h), sizeof(WavHeader));
}

void connectWiFiIfNeeded() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiAttemptMs < WIFI_RETRY_MS) return;

  lastWifiAttemptMs = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void initTimeOnceConnected() {
  static bool done = false;
  if (done) return;
  if (WiFi.status() != WL_CONNECTED) return;

  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");
  done = true;
}

bool isButtonPressedDebounced() {
  bool rawPressed = (digitalRead(PIN_BUTTON) == LOW);
  unsigned long nowMs = millis();

  if (rawPressed != lastButtonStablePressed && (nowMs - lastButtonEdgeMs) >= DEBOUNCE_MS) {
    lastButtonStablePressed = rawPressed;
    lastButtonEdgeMs = nowMs;
  }

  return lastButtonStablePressed;
}

bool setupI2S() {
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chanCfg.auto_clear = true;

  i2s_chan_handle_t tx = nullptr;
  static i2s_chan_handle_t rx = nullptr;

  if (i2s_new_channel(&chanCfg, &tx, &rx) != ESP_OK) return false;

  i2s_std_config_t stdCfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
      .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
      .gpio_cfg = {.mclk = I2S_GPIO_UNUSED,
                   .bclk = static_cast<gpio_num_t>(PIN_I2S_SCK),
                   .ws = static_cast<gpio_num_t>(PIN_I2S_WS),
                   .dout = I2S_GPIO_UNUSED,
                   .din = static_cast<gpio_num_t>(PIN_I2S_SD),
                   .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}}};

  if (i2s_channel_init_std_mode(rx, &stdCfg) != ESP_OK) return false;
  if (i2s_channel_enable(rx) != ESP_OK) return false;

  return true;
}

void startRecording() {
  if (state == DeviceState::RECORDING) return;

  String fileName = String(DIR_AUDIO) + "/" + DEVICE_ID + "_" + nowFileStamp() + "_" + String(sequenceCounter++) + ".wav";
  File f = SD.open(fileName, FILE_WRITE);
  if (!f) {
    Serial.println("[ERR] failed to open wav file");
    return;
  }

  WavHeader empty{};
  f.write(reinterpret_cast<uint8_t *>(&empty), sizeof(empty));

  activeFile = f;
  activePath = fileName;
  activeDataBytes = 0;
  recordStartedAtMs = millis();

  digitalWrite(PIN_LED, HIGH);
  state = DeviceState::RECORDING;
  Serial.printf("[REC] started: %s\n", activePath.c_str());
}

void stopRecording() {
  if (state != DeviceState::RECORDING) return;

  writeWavHeader(activeFile, activeDataBytes);
  activeFile.flush();
  activeFile.close();

  digitalWrite(PIN_LED, LOW);
  Serial.printf("[REC] stopped: %s (%lu bytes)\n", activePath.c_str(), static_cast<unsigned long>(activeDataBytes));

  activePath = "";
  activeDataBytes = 0;
  state = DeviceState::IDLE;
}

void captureAudioTick() {
  if (state != DeviceState::RECORDING) return;

  static uint8_t buffer[1024];
  size_t bytesRead = 0;

  // Read from I2S0 RX using legacy convenience call for Arduino core compatibility.
  esp_err_t err = i2s_read(I2S_NUM_0, buffer, sizeof(buffer), &bytesRead, 0);
  if (err == ESP_OK && bytesRead > 0) {
    activeFile.write(buffer, bytesRead);
    activeDataBytes += bytesRead;
  }

  if (millis() - recordStartedAtMs >= MAX_RECORD_MS) {
    Serial.println("[REC] max duration reached, auto-stop");
    stopRecording();
  }
}

bool uploadFile(const String &path) {
  File file = SD.open(path, FILE_READ);
  if (!file) {
    Serial.printf("[UP] open failed: %s\n", path.c_str());
    return false;
  }

  size_t fileSize = file.size();
  if (fileSize == 0) {
    file.close();
    markSent(path);
    return true;
  }

  bool ok = false;
  if (String(SERVER_URL).startsWith("https://")) {
    WiFiClientSecure client;
    client.setInsecure(); // Replace with certificate pinning in production

    HTTPClient http;
    if (http.begin(client, SERVER_URL)) {
      http.setConnectTimeout(8000);
      http.addHeader("Authorization", String("Bearer ") + API_TOKEN);
      http.addHeader("Content-Type", "audio/wav");
      http.addHeader("X-Device-Id", DEVICE_ID);
      http.addHeader("X-Recorded-At", isoUtcNow());

      int code = http.sendRequest("POST", &file, fileSize);
      ok = (code >= 200 && code < 300);
      Serial.printf("[UP] %s -> HTTP %d\n", path.c_str(), code);
      http.end();
    }
  } else {
    WiFiClient client;
    HTTPClient http;
    if (http.begin(client, SERVER_URL)) {
      http.setConnectTimeout(8000);
      http.addHeader("Authorization", String("Bearer ") + API_TOKEN);
      http.addHeader("Content-Type", "audio/wav");
      http.addHeader("X-Device-Id", DEVICE_ID);
      http.addHeader("X-Recorded-At", isoUtcNow());

      int code = http.sendRequest("POST", &file, fileSize);
      ok = (code >= 200 && code < 300);
      Serial.printf("[UP] %s -> HTTP %d\n", path.c_str(), code);
      http.end();
    }
  }

  file.close();

  if (ok) {
    markSent(path);
  }

  return ok;
}

void uploadTick() {
  if (state == DeviceState::RECORDING) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastUploadTickMs < UPLOAD_INTERVAL_MS) return;
  lastUploadTickMs = millis();

  File dir = SD.open(DIR_AUDIO);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  File file = dir.openNextFile();
  while (file) {
    String path = String(file.path());
    bool wav = path.endsWith(".wav");
    file.close();

    if (wav && !isSent(path)) {
      if (!uploadFile(path)) {
        break;
      }
    }

    file = dir.openNextFile();
  }

  dir.close();
}

bool setupSdCard() {
  SPI.begin();
  if (!SD.begin(PIN_SD_CS)) {
    Serial.println("[ERR] SD init failed");
    return false;
  }

  if (!SD.exists(DIR_AUDIO)) {
    SD.mkdir(DIR_AUDIO);
  }

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  if (!setupSdCard()) {
    while (true) {
      digitalWrite(PIN_LED, !digitalRead(PIN_LED));
      delay(250);
    }
  }

  if (!setupI2S()) {
    Serial.println("[ERR] I2S init failed");
    while (true) {
      digitalWrite(PIN_LED, !digitalRead(PIN_LED));
      delay(100);
    }
  }

  connectWiFiIfNeeded();
}

void loop() {
  connectWiFiIfNeeded();
  initTimeOnceConnected();

  bool pressed = isButtonPressedDebounced();

  if (pressed && state == DeviceState::IDLE) {
    startRecording();
  } else if (!pressed && state == DeviceState::RECORDING) {
    stopRecording();
  }

  captureAudioTick();
  uploadTick();

  delay(1);
}
