#include <SPI.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <Arduino.h> // For String class and basic functions

// Define ADXL345 register addresses
#define POWER_CTL   0x2D
#define DATA_FORMAT 0x31
#define BW_RATE     0x2C
#define DATAX0      0x32

// Define Wi-Fi and server credentials (moved to a separate configuration)
#include "wifi_config.h" // Assuming wifi_config.h contains ssid, password, server_ip, server_port

// Define ADXL345 configuration
#define CS_PIN      15      // GPIO15 (D8) for Chip Select
#define SPI_SPEED   5000000 // SPI clock speed

// Define data acquisition parameters
#define SAMPLE_RATE 3200
#define SAMPLE_INTERVAL_US (1000000 / SAMPLE_RATE)
#define SAMPLE_BATCH_SIZE 100
#define BYTES_PER_SAMPLE 6

// Global buffer for accelerometer data
uint8_t sensor_buffer[SAMPLE_BATCH_SIZE * BYTES_PER_SAMPLE];

// Forward declarations
class SensorReader;
class NetworkClient;

/**
 * @brief Interface for reading sensor data.
 */
class SensorReader {
public:
    virtual ~SensorReader() = default;
    virtual bool initialize() = 0;
    virtual size_t read_data(uint8_t* buffer, size_t num_bytes) = 0;
};

/**
 * @brief Implementation for reading data from the ADXL345 accelerometer.
 */
class ADXL345Reader : public SensorReader {
private:
    const uint8_t cs_pin_;

    /**
     * @brief Writes a single byte to an ADXL345 register.
     * @param reg The register address.
     * @param value The value to write.
     */
    void write_register(uint8_t reg, uint8_t value) {
        SPI.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE3));
        digitalWrite(cs_pin_, LOW);
        SPI.transfer(reg);
        SPI.transfer(value);
        digitalWrite(cs_pin_, HIGH);
        SPI.endTransaction();
    }

    /**
     * @brief Reads a single byte from an ADXL345 register.
     * @param reg The register address.
     * @return The value read from the register.
     */
    uint8_t read_register(uint8_t reg) {
        SPI.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE3));
        digitalWrite(cs_pin_, LOW);
        SPI.transfer(reg | 0x80);
        uint8_t value = SPI.transfer(0x00);
        digitalWrite(cs_pin_, HIGH);
        SPI.endTransaction();
        return value;
    }

    /**
     * @brief Reads multiple bytes from ADXL345 starting at a given register.
     * @param start_reg The starting register address.
     * @param buffer The buffer to store the read data.
     * @param num_bytes The number of bytes to read.
     */
    void read_multiple_registers(uint8_t start_reg, uint8_t* buffer, size_t num_bytes) {
        SPI.beginTransaction(SPISettings(SPI_SPEED, MSBFIRST, SPI_MODE3));
        digitalWrite(cs_pin_, LOW);
        SPI.transfer(start_reg | 0x80 | 0x40); // Read multiple bytes
        for (size_t i = 0; i < num_bytes; ++i) {
            buffer[i] = SPI.transfer(0x00);
        }
        digitalWrite(cs_pin_, HIGH);
        SPI.endTransaction();
    }

public:
    /**
     * @brief Constructor for ADXL345Reader.
     * @param cs_pin The chip select pin for the ADXL345.
     */
    explicit ADXL345Reader(uint8_t cs_pin) : cs_pin_(cs_pin) {}

    /**
     * @brief Initializes the ADXL345 sensor.
     * @return True if initialization was successful, false otherwise.
     */
    bool initialize() override {
        digitalWrite(cs_pin_, HIGH); // Ensure CS is high initially
        write_register(POWER_CTL, 0x08);   // Enable measurement mode
        write_register(DATA_FORMAT, 0x08); // Full resolution, ±2g
        write_register(BW_RATE, 0x0F);     // 3200Hz
        // Optional: Configure offsets if needed
        // write_register(OFSX, 0);
        // write_register(OFSY, -3);
        // write_register(OFSZ, -6);
        return true; // Assume initialization is successful for now
    }

    /**
     * @brief Reads a batch of accelerometer data.
     * @param buffer The buffer to store the read data.
     * @param num_bytes The number of bytes to read (must be a multiple of BYTES_PER_SAMPLE).
     * @return The number of bytes actually read.
     */
    size_t read_data(uint8_t* buffer, size_t num_bytes) override {
        if (num_bytes % BYTES_PER_SAMPLE != 0) {
            return 0; // Invalid number of bytes requested
        }
        read_multiple_registers(DATAX0, buffer, num_bytes);
        return num_bytes;
    }
};

/**
 * @brief Interface for network client operations.
 */
class NetworkClient {
public:
    virtual ~NetworkClient() = default;
    virtual bool connect(const char* ssid, const char* password, const char* server_ip, uint16_t server_port) = 0;
    virtual bool is_connected() = 0;
    virtual size_t send_data(const uint8_t* data, size_t length) = 0;
    virtual void disconnect() = 0;
};

/**
 * @brief Implementation for a Wi-Fi network client using ESP8266WiFi.
 */
class WiFiNetworkClient : public NetworkClient {
private:
    WiFiClient client_;

public:
    /**
     * @brief Constructor for WiFiNetworkClient.
     */
    WiFiNetworkClient() = default;

    /**
     * @brief Connects to the specified Wi-Fi network and TCP server.
     * @param ssid The Wi-Fi network SSID.
     * @param password The Wi-Fi network password.
     * @param server_ip The IP address of the server.
     * @param server_port The port number of the server.
     * @return True if the connection was successful, false otherwise.
     */
    bool connect(const char* ssid, const char* password, const char* server_ip, uint16_t server_port) override {
        WiFi.begin(ssid, password);
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
            Serial.print(".");
        }
        Serial.println("\nWiFi connected");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());

        if (!client_.connect(server_ip, server_port)) {
            Serial.println("Server connection failed");
            return false;
        }
        Serial.println("Connected to server");
        return true;
    }

    /**
     * @brief Checks if the client is currently connected to the server.
     * @return True if connected, false otherwise.
     */
    bool is_connected() override {
        return client_.connected();
    }

    /**
     * @brief Sends data to the connected server.
     * @param data The buffer containing the data to send.
     * @param length The length of the data to send in bytes.
     * @return The number of bytes actually sent.
     */
    size_t send_data(const uint8_t* data, size_t length) override {
        if (client_.connected()) {
            return client_.write(data, length);
        }
        return 0;
    }

    /**
     * @brief Disconnects from the server and the Wi-Fi network.
     */
    void disconnect() override {
        client_.stop();
        WiFi.disconnect();
        Serial.println("Disconnected from server and Wi-Fi");
    }
};

// Global instances of the core components
ADXL345Reader accelerometer(CS_PIN);
WiFiNetworkClient network_client;

unsigned long previous_time = 0;
uint32_t sample_count = 0;

void setup() {
    Serial.begin(1000000);
    SPI.begin();

    if (accelerometer.initialize()) {
        Serial.println("ADXL345 initialized successfully");
    } else {
        Serial.println("ADXL345 initialization failed");
    }

    if (network_client.connect(WIFI_SSID, WIFI_PASSWORD, SERVER_IP, SERVER_PORT)) {
        Serial.println("Network client connected");
    } else {
        Serial.println("Network client connection failed");
    }
}

void loop() {
    unsigned long current_time = micros();

    if (current_time - previous_time >= SAMPLE_INTERVAL_US) {
        previous_time = current_time;
        if (accelerometer.read_data(sensor_buffer + (sample_count % SAMPLE_BATCH_SIZE) * BYTES_PER_SAMPLE, BYTES_PER_SAMPLE) == BYTES_PER_SAMPLE) {
            sample_count++;
            if (sample_count % SAMPLE_BATCH_SIZE == 0) {
                if (network_client.is_connected()) {
                    size_t sent_bytes = network_client.send_data(sensor_buffer, sizeof(sensor_buffer));
                    if (sent_bytes == sizeof(sensor_buffer)) {
                        // Data sent successfully
                    } else {
                        Serial.print("Error sending data: ");
                        Serial.println(sent_bytes);
                        network_client.disconnect();
                        if (network_client.connect(WIFI_SSID, WIFI_PASSWORD, SERVER_IP, SERVER_PORT)) {
                            Serial.println("Network client reconnected");
                        }
                    }
                } else {
                    Serial.println("Network client not connected, attempting to reconnect");
                    network_client.disconnect();
                    if (network_client.connect(WIFI_SSID, WIFI_PASSWORD, SERVER_IP, SERVER_PORT)) {
                        Serial.println("Network client reconnected");
                    }
                }
            }
        } else {
            Serial.println("Error reading data from accelerometer");
        }
    }
    delay(1); // Small delay to prevent busy-waiting
}