#include <SPI.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>

#define CS_PIN 15
#define POWER_CTL 0x2D
#define DATA_FORMAT 0x31
#define BW_RATE 0x2C
#define OFSX 0x1E
#define OFSY 0x1F
#define OFSZ 0x20

#define TOTAL_SAMPLES 96000 // 3200 Hz * 30s
#define SAMPLES_PER_SEGMENT 2500 // Standard segment
#define FINAL_SEGMENT_SAMPLES 1000 // Last segment for exact 96000
#define TOTAL_SEGMENTS 39 // 38*2500 + 1*1000 = 96000

#include "wifi_config.h"

// Static IP configuration
// IPAddress static_ip(192, 168, 11, 193);
// IPAddress gateway(192, 168, 11, 1);
// IPAddress subnet(255, 255, 255, 0);

WiFiClient client;
const WiFiCredential* currentNetwork = nullptr;

const int SAMPLE_SIZE = 10; // 6 bytes XYZ + 4 bytes timestamp
const int MAX_SAMPLES = 2500; // ~1.56s at 3200 Hz (50 KB)
const int BATCH_SIZE = 200; // Send 100 samples per batch
uint8_t buffer[MAX_SAMPLES * SAMPLE_SIZE];
volatile bool startSignalReceived = false;
int sampleCount = 0;
bool dataCollected = false;
long totalSamplesSent = 0;
int segmentCount = 0;
unsigned long startMicrosGlobal = 0; // Global timestamp reference

// bool wifiConnected = false;
// bool serverConnected = false;

void writeRegister(byte reg, byte value) {
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
  SPI.transfer(reg | 0x80);  // Read flag
  uint8_t value = SPI.transfer(0x00);
  digitalWrite(CS_PIN, HIGH);
  SPI.endTransaction();
  return value;
}

void readAllData(byte startReg, byte *buffer, int numBytes) {
    if (!buffer) {
        Serial.println("Error: Null buffer in readAllData");
        return;
    }
    SPI.beginTransaction(SPISettings(5000000, MSBFIRST, SPI_MODE3));
    digitalWrite(CS_PIN, LOW);
    SPI.transfer(startReg | 0x80 | 0x40);
    for (int i = 0; i < numBytes; i++) {
        buffer[i] = SPI.transfer(0x00);
    }
    digitalWrite(CS_PIN, HIGH);
    SPI.endTransaction();
}

bool connectToWiFi() {
    Serial.println("Scanning for Wi-Fi networks...");
    int n = WiFi.scanNetworks();
    if (n == 0) {
      Serial.println("No networks found.");
      return false;
    }
  
    for (int i = 0; i < knownNetworkCount; i++) {
      for (int j = 0; j < n; j++) {
        if (WiFi.SSID(j) == knownNetworks[i].ssid) {
          Serial.printf("Connecting to: %s\n", knownNetworks[i].ssid);
          // WiFi.config(static_ip, gateway, subnet);
          WiFi.begin(knownNetworks[i].ssid, knownNetworks[i].password);
  
          unsigned long wifiStart = millis();
          while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            yield();
            Serial.print(".");
            if (millis() - wifiStart > 15000) {
              Serial.println("\nWi-Fi connection timed out.");
              return false;
            }
          }
  
          Serial.println("\nConnected to Wi-Fi!");
          Serial.print("IP address: ");
          Serial.println(WiFi.localIP());
          currentNetwork = &knownNetworks[i];
          return true;
        }
      }
    }
  
    Serial.println("No known Wi-Fi networks found nearby.");
    return false;
  }

bool connectToServer() {
    if (!currentNetwork || !currentNetwork->server_ip) {
        Serial.println("Error: Invalid server configuration");
        return false;
    }

    Serial.print("Connecting to server: ");
    Serial.println(currentNetwork->server_ip);
    unsigned long start = millis();
    while (!client.connect(currentNetwork->server_ip, currentNetwork->server_port)) {
        Serial.println("Retrying server...");
        delay(500);
        if (millis() - start > 10000) {
            Serial.println("Server connection timeout");
            return false;
        }
    }
    Serial.println("Connected to server!");
    Serial.print("Free heap: ");
    Serial.println(ESP.getFreeHeap());
    return true;
}

bool waitForStartSignal(unsigned long timeoutMillis = 15000) {
    char incomingBuffer[32];
    int bufferIndex = 0;
    unsigned long start = millis();

    Serial.println("Waiting for CMD:START");
    while (!startSignalReceived && (millis() - start < timeoutMillis)) {
        if (!client.connected()) {
            Serial.println("Server connection lost");
            return false;
        }
        while (client.available()) {
            char c = client.read();
            if (bufferIndex < sizeof(incomingBuffer) - 1) {
                incomingBuffer[bufferIndex++] = c;
            }
            if (c == '\n') {
                incomingBuffer[bufferIndex] = '\0';
                if (strncmp(incomingBuffer, "CMD:START:", 10) == 0) {
                    unsigned long delayMs = atol(incomingBuffer + 10);
                    Serial.print("Received CMD:START with delay: ");
                    Serial.println(delayMs);
                    delay(delayMs);
                    startSignalReceived = true;
                    Serial.println("Starting data collection");
                    return true;
                }
                bufferIndex = 0;
            }
        }
    }
    Serial.println("Timeout waiting for CMD:START");
    return false;
}

void setupADXL345() {
    Serial.println("Initializing ADXL345...");
    writeRegister(POWER_CTL, 0x00); // Reset
    delay(10);
    writeRegister(DATA_FORMAT, 0x08); // Full-resolution, ±2g
    writeRegister(BW_RATE, 0x0F); // 3200 Hz
    writeRegister(OFSX, 0);
    writeRegister(OFSY, -3);
    writeRegister(OFSZ, -6);
    writeRegister(POWER_CTL, 0x08); // Measurement mode
    delay(10);
    uint8_t devid = readRegister(0x00);
    Serial.print("ADXL345 DEVID: ");
    Serial.println(devid, HEX); // Expect 0xE5
    if (devid != 0xE5) {
        Serial.println("Error: ADXL345 not detected");
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("Booting ESP8266");
    delay(1000);
    Serial.setDebugOutput(false);

    pinMode(CS_PIN, OUTPUT);
    digitalWrite(CS_PIN, HIGH);
    SPI.begin();

    WiFi.mode(WIFI_STA);
    while (!connectToWiFi()) {
        Serial.println("Retrying Wi-Fi...");
        delay(2000);
    }

    setupADXL345();
}

void loop() {
    if (!client.connected()) {
        startSignalReceived = false;
        dataCollected = false;
        sampleCount = 0;
        segmentCount = 0;
        totalSamplesSent = 0;
        startMicrosGlobal = 0; // Reset global timestamp
        while (!connectToServer()) {
            Serial.println("Retrying server...");
            delay(2000);
        }
        if (!waitForStartSignal()) {
            client.stop();
            return;
        }
    }

    if (!startSignalReceived) return;

    if (!dataCollected) {
        Serial.print("Segment ");
        Serial.print(segmentCount + 1);
        Serial.println(" start (timestamps global from CMD:START)");
        Serial.print("Free heap before collection: ");
        Serial.println(ESP.getFreeHeap());
        unsigned long startMicros = micros(); // For segment duration only
        if (segmentCount == 0) {
            startMicrosGlobal = startMicros; // Set global timestamp at first segment
        }
        unsigned long previousMicros = startMicros;
        const unsigned long intervalMicros = 312; // Calibrated for 3200 Hz
        sampleCount = 0;
        int targetSamples = (segmentCount == TOTAL_SEGMENTS - 1) ? FINAL_SEGMENT_SAMPLES : SAMPLES_PER_SEGMENT;

        while (sampleCount < targetSamples) {
            unsigned long current = micros();
            // Alternate between 312 and 313 to approximate 312.5us
            unsigned long effectiveInterval = (sampleCount % 2 == 0) ? 312 : 313;
            if (current - previousMicros >= effectiveInterval) {
                previousMicros = current;
                int offset = sampleCount * SAMPLE_SIZE;
                readAllData(0x32, &buffer[offset], 6);
                unsigned long timestamp = current - startMicrosGlobal; // Global timestamp
                memcpy(&buffer[offset + 6], &timestamp, 4);
                sampleCount++;
            }
        }
        dataCollected = true;
        unsigned long durationMicros = micros() - startMicros;
        Serial.print("Collected ");
        Serial.print(sampleCount);
        Serial.println(" samples");
        Serial.print("Segment duration (ms): ");
        Serial.println(durationMicros / 1000.0);
        Serial.print("Sampling rate (Hz): ");
        Serial.println(sampleCount * 1000000.0 / durationMicros);
        Serial.print("Free heap after collection: ");
        Serial.println(ESP.getFreeHeap());
    }

    if (dataCollected && client.connected()) {
        Serial.print("Connection status: ");
        Serial.println(client.connected() ? "Connected" : "Disconnected");
        int sentSamples = 0;
        while (sentSamples < sampleCount) {
            int batchSamples = min(BATCH_SIZE, sampleCount - sentSamples);
            int bytesToSend = batchSamples * SAMPLE_SIZE;
            int offset = sentSamples * SAMPLE_SIZE;
            // Serial.print("Sending batch of ");
            // Serial.print(batchSamples);
            // Serial.println(" samples");
            if (client.write(&buffer[offset], bytesToSend) != bytesToSend) {
                Serial.println("Failed to send batch");
                client.stop();
                break;
            }
            sentSamples += batchSamples;
            delay(10);
        }
        if (sentSamples == sampleCount) {
            Serial.println("Segment sent");
            totalSamplesSent += sampleCount;
            segmentCount++;
            sampleCount = 0;
            dataCollected = false;
            Serial.print("Segment ");
            Serial.print(segmentCount);
            Serial.print("/39, Total samples sent: ");
            Serial.println(totalSamplesSent);
            if (segmentCount >= TOTAL_SEGMENTS) {
                Serial.println("All segments sent, stopping");
                Serial.print("Final total samples: ");
                Serial.println(totalSamplesSent);
                client.stop();
                startSignalReceived = false;
                totalSamplesSent = 0;
                segmentCount = 0;
                startMicrosGlobal = 0; // Reset global timestamp
            }
        }
    }
}
// unsigned long previousMicros = 0;
// const unsigned long intervalMicros = 312;  // ~3205 Hz
// unsigned long startMicros = 0;

// void loop() {
//   // Reconnect to server if not connected
//   if (!client.connected()) {
//     startSignalReceived = false;
//     Serial.println("Client disconnected. Attempting server reconnection...");

//     while (!connectToServer()) {
//       Serial.println("Retrying server...");
//       delay(2000);
//       yield();
//     }

//     waitForStartSignal();
//     startMicros = micros();
//     previousMicros = startMicros;
//     sampleCount = 0;
//   }

//   if (!startSignalReceived) return;

//   unsigned long now = micros();

//   if (startMicros == 0) {
//     startMicros = now;
//     previousMicros = now;
//   }

//   if ((now - startMicros) >= RUN_DURATION * 1000000UL) {
//     if (sampleCount > 0) {
//       client.write(buffer, sampleCount * SAMPLE_SIZE);
//     }
//     client.stop();  // Disconnect without reset
//     startSignalReceived = false;
//     sampleCount = 0;
//     return;
//   }

//   if ((now - previousMicros) >= intervalMicros) {
//     previousMicros += intervalMicros;
//     int offset = sampleCount * SAMPLE_SIZE;
//     readAllData(0x32, &buffer[offset], 6);
//     unsigned long timestamp = now - startMicros;
//     memcpy(&buffer[offset + 6], &timestamp, 4);
//     sampleCount++;

//     if (sampleCount >= SAMPLE_BATCH_SIZE) {
//       client.write(buffer, SAMPLE_BATCH_SIZE * SAMPLE_SIZE);
//       sampleCount = 0;
//     }
//   }

//   yield();
// }
