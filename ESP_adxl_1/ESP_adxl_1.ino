#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <SPI.h>
#include "wifi_config.h"

#define CS_PIN 15
#define POWER_CTL 0x2D
#define DATA_FORMAT 0x31
#define BW_RATE 0x2C
#define DEVID 0x00

#define SAMPLE_SIZE 10
#define RUN_DURATION 30
#define TARGET_SAMPLES (3200 * RUN_DURATION)
#define TCP_PORT 5001
#define HTTP_PORT 5000

// Static IP configuration
// IPAddress static_ip(192, 168, 11, 193);
// IPAddress gateway(192, 168, 11, 1);
// IPAddress subnet(255, 255, 255, 0);

uint8_t* dataBuffer = nullptr;
volatile uint32_t writeIndex = 0;
volatile bool samplingDone = false;
uint32_t start_time = 0; 

WiFiClient tcpClient;
WiFiClient httpClient;
HTTPClient http;

hw_timer_t* timer = nullptr;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;

const WiFiCredential* currentNetwork = nullptr;

// ------------------- SPI Communication ------------------- //
void writeRegister(uint8_t reg, uint8_t value) {
  SPI.beginTransaction(SPISettings(5000000, MSBFIRST, SPI_MODE3));
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(reg);
  SPI.transfer(value);
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
}

uint8_t readRegister(uint8_t reg) {
  SPI.beginTransaction(SPISettings(5000000, MSBFIRST, SPI_MODE3));
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(reg | 0x80);
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
  return value;
}

void readXYZ(uint8_t* buf) {
  SPI.beginTransaction(SPISettings(5000000, MSBFIRST, SPI_MODE3));
  digitalWrite(CS_PIN, LOW);
  SPI.transfer(0x32 | 0xC0); // Burst read from 0x32
  for (int i = 0; i < 6; i++) buf[i] = SPI.transfer(0x00);
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
}

void setupADXL345() {
  writeRegister(POWER_CTL, 0x08);      // Enable measurement
  writeRegister(DATA_FORMAT, 0x08);    // Full resolution, ±2g
  writeRegister(BW_RATE, 0x0F);        // 3200 Hz output rate
  Serial.print("ADXL345 DEVID: ");
  Serial.println(readRegister(DEVID), HEX);
}

// ------------------- Sampling Timer ------------------- //
// Timer ISR for sampling
void IRAM_ATTR onSampleTimer() {
  portENTER_CRITICAL_ISR(&timerMux);

  uint32_t timestamp = micros() - start_time; // Get the current timestamp
  uint8_t* ptr = dataBuffer + (writeIndex * SAMPLE_SIZE);
  // uint32_t timestampUs = timerRead(timer); // 2 MHz = 0.5 µs per tick
  readXYZ(ptr);
  memcpy(ptr + 6, &timestamp, 4);
  
  writeIndex++;
  if (writeIndex >= TARGET_SAMPLES) {
    samplingDone = true;
    timerStop(timer);
    timerDetachInterrupt(timer); // Detach to prevent stray interrupts
    portEXIT_CRITICAL_ISR(&timerMux);
    return;
  }
  portEXIT_CRITICAL_ISR(&timerMux);
}

void startSampling() {
  Serial.println("Starting sampling...");
  writeIndex = 0;
  samplingDone = false;

  timerRestart(timer); // Restart the timer
  timerWrite(timer, 0);  // reset counter
  timerAlarm(timer, 625, true, 0);  // 3200 Hz ≈ 312.5 µs interval
  start_time = micros();
  timerStart(timer);
}

// ------------------- Wi-Fi and TCP ------------------- //
bool connectToWiFi() {
  Serial.println("Scanning for Wi-Fi networks...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  // Wait for any ongoing scan to complete
  while (WiFi.scanComplete() == -1) {
    Serial.println("Waiting for previous scan to complete...");
    delay(1000);
  }

  int n = WiFi.scanNetworks();
  delay(200);
  Serial.printf("WiFi.scanNetworks() returned %d\n", n);
  if (n == WIFI_SCAN_FAILED) {
    Serial.println("Scan failed with error code -2");
    return false;
  }
  if (n == WIFI_SCAN_RUNNING) {
    Serial.println("Scan is still running, please wait...");
    delay(1000);
    return false;
  }
  if (n == 0) {
    Serial.println("No networks found");
    return false;
  }

  for (int i = 0; i < knownNetworkCount; i++) {
    for (int j = 0; j < n; j++) {
      if (WiFi.SSID(j) == knownNetworks[i].ssid) {
        Serial.printf("Connecting to SSID: %s\n", knownNetworks[i].ssid);
        // WiFi.config(static_ip, gateway, subnet);
        WiFi.begin(knownNetworks[i].ssid, knownNetworks[i].password);
        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
          delay(200);
          Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
          Serial.printf("\nConnected to Wi-Fi: %s, IP: %s\n", knownNetworks[i].ssid, WiFi.localIP().toString().c_str());
          currentNetwork = &knownNetworks[i];
          return true;
        } else {
          Serial.println("\nWi-Fi connection timeout");
        }
      }
    }
  }
  Serial.println("No known networks found or failed to connect.");
  return false;
}

// TCP sync
bool connectToTCPServer() {
  Serial.printf("Connecting to TCP server at %s:%d...\n",
                currentNetwork->server_ip.toString().c_str(),
                currentNetwork->tcp_port);
  bool result = tcpClient.connect(currentNetwork->server_ip, currentNetwork->tcp_port, 10000);
  if (result)
    Serial.println("Connected to TCP server.");
  else
    Serial.println("Failed to connect to TCP server.");
  return result;
}

bool waitForStartCommand() {
  Serial.println("Waiting for CMD:START:<delay> command...");
  String msg;
  unsigned long start = millis();
  while (millis() - start < 15000) {
    while (tcpClient.available()) {
      char c = tcpClient.read();
      msg += c;
      if (msg.endsWith("\n")) {
        Serial.printf("Received message: %s", msg.c_str());
        if (msg.startsWith("CMD:START:")) {
          int delayMs = msg.substring(10).toInt();
          Serial.printf("Start command received, delaying %d ms\n", delayMs);
          delay(delayMs);
          return true;
        }
        msg = "";
      }
    }
  }
  Serial.println("Timeout waiting for start command.");
  return false;
}

// ------------------- Upload ------------------- //
void uploadToServer() {
  Serial.println("Uploading to server...");
  String url = "http://" + currentNetwork->server_ip.toString() + ":5000/upload";

  const int maxRetries = 3;
  int attempt = 0;
  int httpCode = -1;

  while (attempt < maxRetries) {
    Serial.printf("Upload attempt %d...\n", attempt + 1);
    
    if (!http.begin(httpClient, url)) {
      Serial.println("Failed to initiate HTTP client.");
      http.end();
      attempt++;
      delay(2000);
      continue;
    }

    http.setTimeout(10000);  // Set a 10-second timeout for HTTP connection
    http.addHeader("Content-Type", "application/octet-stream");
    http.addHeader("Content-Length", String(writeIndex * SAMPLE_SIZE));
    http.addHeader("Connection", "close");

    httpCode = http.sendRequest("POST", dataBuffer, writeIndex * SAMPLE_SIZE);
    http.end();

    if (httpCode > 0) {
      Serial.printf("Upload successful, HTTP status: %d\n", httpCode);
      break;
    } else {
      Serial.printf("Upload failed, HTTP error: %d. Retrying...\n", httpCode);
      attempt++;
      delay(3000);  // Wait before retrying
    }
  }

  if (httpCode <= 0) {
    Serial.println("Upload failed after multiple attempts. Aborting.");
    // Optionally, you can store the buffer or trigger a reset here.
  }
}


// ------------------- Setup & Loop ------------------- //
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("Setup started");

  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin(14, 12, 13, 15);  // SCK, MISO, MOSI, CS

  setupADXL345();
  Serial.println("ADXL345 setup done");

  dataBuffer = (uint8_t*)ps_malloc(TARGET_SAMPLES * SAMPLE_SIZE);
  if (!dataBuffer) {
    Serial.println("Failed to allocate PSRAM buffer");
    while (true);
  }

  Serial.println("PSRAM allocated");

  // Clean up any previous timer if it exists
  if (timer != nullptr) {
    timerStop(timer);
    timerDetachInterrupt(timer); // Detach the interrupt
    timerEnd(timer);         // Deinitialize the timer
    timer = nullptr;
    delay(50);               // Increased delay to ensure hardware settles
  }

  timer = timerBegin(2000000); // 2 MHz -> 1 tick = 0.5 µs
  timerAttachInterrupt(timer, &onSampleTimer);
  Serial.println("Setup complete");
}


void loop() {
  static bool isIdle = true; // Track idle state to prevent reconnection during sampling

  if (WiFi.status() != WL_CONNECTED || currentNetwork == nullptr) {
    Serial.println("Wi-Fi disconnected or uninitialized, attempting to reconnect...");
    while (!connectToWiFi()) {
      delay(2000);
    }
    isIdle = true; // Reset to idle after Wi-Fi reconnect
  }

  // Only attempt TCP connection if idle (not sampling and buffer empty)
  if (isIdle && !tcpClient.connected() && !samplingDone && writeIndex == 0) {
    while (!connectToTCPServer()) {
      delay(1000);
    }
    if (waitForStartCommand()) {
      tcpClient.stop(); // Close TCP connection after receiving command
      Serial.println("TCP connection closed, starting sampling...");
      isIdle = false; // Enter sampling state
      startSampling();
    } else {
      tcpClient.stop();
      isIdle = true;
      return;
    }
  }

  if (samplingDone) {
    Serial.println("Sampling complete, disconnecting and uploading...");
    uploadToServer();

    portENTER_CRITICAL(&timerMux);
    writeIndex = 0;
    samplingDone = false;
    portEXIT_CRITICAL(&timerMux);
    
    isIdle = true; // Return to idle state
    Serial.println("Ready for next command...");
  }
}
