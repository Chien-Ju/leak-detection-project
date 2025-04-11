#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

// #define WIFI_SSID       "NTUR101A"
// #define WIFI_PASSWORD   "hecaslab"
// #define SERVER_IP       "192.168.11.43"

struct WiFiCredential {
    const char* ssid;
    const char* password;
    const char* server_ip;
    uint16_t server_port;
};

// List of known Wi-Fi networks
const WiFiCredential knownNetworks[] = {
    {"ChienJu", "jyd726489", "172.17.95.176", 8888},
    {"61-4f", "Qq0981673893", "192.168.68.54", 8888},
    {"Alchoholic_2G", "aSej29)siE%q", "192.168.50.80", 8888},
};

const int knownNetworkCount = sizeof(knownNetworks) / sizeof(knownNetworks[0]);

#endif