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

#define RUN_DURATION 30
#include "wifi_config.h"

// Static IP configuration
// IPAddress static_ip(192, 168, 11, 193);
// IPAddress gateway(192, 168, 11, 1);
// IPAddress subnet(255, 255, 255, 0);

WiFiClient client;
const WiFiCredential* currentNetwork = nullptr;

const int SAMPLE_BATCH_SIZE = 100;
const int SAMPLE_SIZE = 10; // 6 bytes for XYZ + 4 bytes timestamp
uint8_t buffer[SAMPLE_BATCH_SIZE * SAMPLE_SIZE];
volatile bool startSignalReceived = false;

bool wifiConnected = false;
bool serverConnected = false;

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
  if (!currentNetwork) return false;

  unsigned long serverStart = millis();
  while (!client.connect(currentNetwork->server_ip, currentNetwork->server_port)) {
    Serial.println("Connecting to server...");
    delay(500);
    yield();
    if (millis() - serverStart > 10000) {
      Serial.println("Server connection timeout.");
      return false;
    }
  }

  Serial.println("Connected to server!");
  return true;
}

bool waitForStartSignal(unsigned long timeoutMillis = 15000) {
  String incomingMessage;
  unsigned long startWait = millis();

  while (!startSignalReceived && (millis() - startWait < timeoutMillis)) {
    while (client.available()) {
      char c = client.read();
      incomingMessage += c;

      if (incomingMessage.endsWith("\n")) {
        if (incomingMessage.startsWith("CMD:START:")) {
          unsigned long delayMs = incomingMessage.substring(10).toInt();
          delay(delayMs);
          startSignalReceived = true;
          return true;
        }
        incomingMessage = "";
      }
    }
    yield();
  }

  return false;
}

void setupADXL345() {
  writeRegister(POWER_CTL, 0x08);
  Serial.println(readRegister(POWER_CTL));
  writeRegister(DATA_FORMAT, 0x08);
  Serial.println(readRegister(DATA_FORMAT));
  writeRegister(BW_RATE, 0x0F);
  Serial.println(readRegister(BW_RATE));
  writeRegister(OFSX, 0);
  writeRegister(OFSY, -3);
  writeRegister(OFSZ, -6);
}

void setup() {
  Serial.begin(115200);
  Serial.println("Setup starting...");
  delay(1000);
  Serial.setDebugOutput(false);

  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);
  SPI.begin();
  Serial.print("DEVID: ");
  Serial.println(readRegister(0x00), HEX);  // Should be 0xE5


  WiFi.mode(WIFI_STA);

  // Retry Wi-Fi until connected
  while (!connectToWiFi()) {
    Serial.println("Retrying Wi-Fi...");
    delay(2000);
    yield();
  }

  setupADXL345();
}

unsigned long previousMicros = 0;
const unsigned long intervalMicros = 312;  // ~3205 Hz
int sampleCount = 0;
unsigned long startMicros = 0;

void loop() {
  // Reconnect to server if not connected
  if (!client.connected()) {
    startSignalReceived = false;
    Serial.println("Client disconnected. Attempting server reconnection...");

    while (!connectToServer()) {
      Serial.println("Retrying server...");
      delay(2000);
      yield();
    }

    waitForStartSignal();
    startMicros = micros();
    previousMicros = startMicros;
    sampleCount = 0;
  }

  if (!startSignalReceived) return;

  unsigned long now = micros();

  if (startMicros == 0) {
    startMicros = now;
    previousMicros = now;
  }

  if ((now - startMicros) >= RUN_DURATION * 1000000UL) {
    if (sampleCount > 0) {
      client.write(buffer, sampleCount * SAMPLE_SIZE);
    }
    client.stop();  // Disconnect without reset
    startSignalReceived = false;
    sampleCount = 0;
    return;
  }

  if ((now - previousMicros) >= intervalMicros) {
    previousMicros += intervalMicros;
    int offset = sampleCount * SAMPLE_SIZE;
    readAllData(0x32, &buffer[offset], 6);
    unsigned long timestamp = now - startMicros;
    memcpy(&buffer[offset + 6], &timestamp, 4);
    sampleCount++;

    if (sampleCount >= SAMPLE_BATCH_SIZE) {
      client.write(buffer, SAMPLE_BATCH_SIZE * SAMPLE_SIZE);
      sampleCount = 0;
    }
  }

  yield();
}
