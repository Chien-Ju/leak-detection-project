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

WiFiClient client;
const WiFiCredential* currentNetwork = nullptr;

const int SAMPLE_BATCH_SIZE = 100;
uint8_t buffer[SAMPLE_BATCH_SIZE * 6];
bool startSignalReceived = false;

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
  SPI.transfer(reg | 0x80);
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

void connectToServer() {
  Serial.println("Scanning for Wi-Fi networks...");
  int n = WiFi.scanNetworks();
  if (n == 0) {
    Serial.println("No networks found.");
    return;
  }

  for (int i = 0; i < knownNetworkCount; i++) {
    for (int j = 0; j < n; j++) {
      if (WiFi.SSID(j) == knownNetworks[i].ssid) {
        Serial.printf("Connecting to: %s\n", knownNetworks[i].ssid);
        WiFi.begin(knownNetworks[i].ssid, knownNetworks[i].password);
        while (WiFi.status() != WL_CONNECTED) {
          delay(500);
          Serial.print(".");
        }
        Serial.println("\nConnected to Wi-Fi!");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());

        currentNetwork = &knownNetworks[i];

        while (!client.connect(currentNetwork->server_ip, currentNetwork->server_port)) {
          Serial.println("Connecting to server...");
          delay(1000);
        }
        Serial.println("Connected to server!");
        return;
      }
    }
  }

  Serial.println("No known Wi-Fi networks found nearby.");
}

bool waitForStartSignal(unsigned long timeoutMillis = 15000) {
  String incomingMessage;
  unsigned long startWait = millis();

  while (!startSignalReceived && (millis() - startWait < timeoutMillis)) {
    while (client.available()) {
      char c = client.read();
      incomingMessage += c;

      if (incomingMessage.endsWith("\n")) {
        Serial.print("Received: ");
        Serial.println(incomingMessage);

        if (incomingMessage.startsWith("CMD:START:")) {
          unsigned long delayMs = incomingMessage.substring(10).toInt();
          Serial.print("Delaying for ms: ");
          Serial.println(delayMs);

          delay(delayMs);
          startSignalReceived = true;
          return true;
        }
        incomingMessage = "";
      }
    }
  }

  Serial.println("Timeout waiting for CMD:START signal.");
  return false;
}

float getBatteryVoltage() {
  int raw = analogRead(A0);
  float voltage = raw * 2.0 * 3.3 / 1023.0;
  return voltage;
}

void setup() {
  Serial.begin(1000000);
  Serial.setDebugOutput(false);

  SPI.begin();
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);

  // float battery = getBatteryVoltage();
  // Serial.print("Battery Voltage: ");
  // Serial.print(battery);
  // Serial.println(" V");
  // delay(2000);

  WiFi.mode(WIFI_STA);
  connectToServer();

  waitForStartSignal();

  writeRegister(POWER_CTL, 0x08);
  writeRegister(DATA_FORMAT, 0x08);
  writeRegister(BW_RATE, 0x0F);
  writeRegister(OFSX, 0);
  writeRegister(OFSY, -3);
  writeRegister(OFSZ, -6);
}

unsigned long previous = 0;
const long interval = 312.5;
int sampleCount = 0;
unsigned long startMicros = 0;

void loop() {
  unsigned long current = micros();

  if (!startSignalReceived) return;

  if (startMicros == 0) {
    startMicros = current;
  }

  if (current - startMicros >= RUN_DURATION * 1000000UL) {
      Serial.printf("Run complete. Samples collected: %d\n", sampleCount);
      
      if (sampleCount > 0) {
          client.write(buffer, sampleCount * 6);
          Serial.printf("Sent final partial batch of %d samples.\n", sampleCount);
      }

      client.stop();
      ESP.restart();
  }


  if (!client.connected()) {
    Serial.println("Disconnected from server. Attempting to reconnect...");
    client.stop();
    connectToServer();
    waitForStartSignal();
    startMicros = micros();
    sampleCount = 0;
    return;
  }

  if (current - previous >= interval) {
    previous = current;
    readAllData(0x32, &buffer[sampleCount * 6], 6);
    sampleCount++;

    if (sampleCount >= SAMPLE_BATCH_SIZE) {
      Serial.printf("Sending %d bytes to server...\n", sizeof(buffer));
      client.write(buffer, sizeof(buffer));
      sampleCount = 0;
    }
  }

  yield();
}