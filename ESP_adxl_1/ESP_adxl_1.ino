#include <SPI.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>

#define CS_PIN 15  // GPIO15 (D8) for Chip Select
#define POWER_CTL 0x2D // Power-Saving features control
#define DATA_FORMAT 0x31 // Data Format Control
#define BW_RATE 0x2C // Data Rate and power mode control
#define OFSX 0x1E // X-axis offset
#define OFSY 0x1F // Y-axis offset
#define OFSZ 0x20 // Z-axis offset

// Define Wi-Fi and server credentials (moved to a separate configuration)
#include "wifi_config.h" // Assuming wifi_config.h contains ssid, password, server_ip, server_port

WiFiClient client;

const int SAMPLE_BATCH_SIZE = 100;
uint8_t buffer[SAMPLE_BATCH_SIZE * 6];

/**
 * Writes a register in ADXL345.
 * @param reg Register address.
 * @param value Value to write.
 */
void writeRegister(byte reg, byte value) {
    SPI.beginTransaction(SPISettings(5000000, MSBFIRST, SPI_MODE3));
    digitalWrite(CS_PIN, LOW);
    SPI.transfer(reg);
    SPI.transfer(value);
    digitalWrite(CS_PIN, HIGH);
    SPI.endTransaction();
}

/**
 * Reads a single register from ADXL345.
 * @param reg Register address.
 * @return Register value.
 */
uint8_t readRegister(uint8_t reg) {
    SPI.beginTransaction(SPISettings(5000000, MSBFIRST, SPI_MODE3));
    digitalWrite(CS_PIN, LOW);
    SPI.transfer(reg | 0x80);
    uint8_t value = SPI.transfer(0x00);
    digitalWrite(CS_PIN, HIGH);
    SPI.endTransaction();
    return value;
}

/**
 * Reads multiple bytes from ADXL345.
 * @param startReg Starting register address.
 * @param buffer Buffer to store the read data.
 * @param numBytes Number of bytes to read.
 */
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

/**
 * Connects to Wi-Fi and TCP server.
 */
void connectToServer() {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) {
      // Serial.println("Lost WiFi Connection...");
      delay(500);
    }
    while (!client.connect(SERVER_IP, SERVER_PORT)) {
      // Serial.println("Lost server Connection...");
      delay(1000);
    }
}

void setup() {
    Serial.begin(1000000);
    Serial.setDebugOutput(true);

    SPI.begin();
    pinMode(CS_PIN, OUTPUT);
    digitalWrite(CS_PIN, HIGH);

    WiFi.mode(WIFI_STA);
    connectToServer();

    writeRegister(POWER_CTL, 0x08); // Enable measurement mode
    writeRegister(DATA_FORMAT, 0x08); // Full resolution, ±2g
    writeRegister(BW_RATE, 0x0F); // 3200Hz
    writeRegister(OFSX, 0);
    writeRegister(OFSY, -3);
    writeRegister(OFSZ, -6);
}

unsigned long previous = 0;
const long interval = 312.5;
int sampleCount = 0;

void loop() {
    unsigned long current = micros();

    // Serial.println(WiFi.localIP());
    if (current - previous >= interval) {
        previous = current;
        readAllData(0x32, &buffer[int(sampleCount) * 6], 6);
        sampleCount++;
        if (sampleCount >= SAMPLE_BATCH_SIZE) {
          if (client.connected()) {
              client.write(buffer, sizeof(buffer));
            //   Serial.print("RSSI: ");
            //   Serial.println(WiFi.RSSI()); //print rssi value.
          } else {
              client.stop();
              connectToServer();
          }
          sampleCount = 0;
        }
    }
}
