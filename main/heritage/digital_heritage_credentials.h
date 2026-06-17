/*
 * Local Wi-Fi / MQTT credentials for the Digital Human Research Institute environment.
 * This is the committed default development profile for on-site firmware builds.
 */

#ifndef DIGITAL_HERITAGE_CREDENTIALS_H
#define DIGITAL_HERITAGE_CREDENTIALS_H

#define DH_WIFI_NETWORK_ENTRY(SSID, PASS, MQTT_URI) \
    { (SSID), (PASS), (MQTT_URI) }

/* Digital Human Research Institute: omelette Wi-Fi, controller/MQTT at 192.168.31.12. */
#define DH_WIFI_NETWORK_TABLE \
    DH_WIFI_NETWORK_ENTRY("omelette", "13326228330", "mqtt://192.168.31.12:1883")

#define DH_MQTT_BROKER_URI "mqtt://192.168.31.12:1883"

#endif /* DIGITAL_HERITAGE_CREDENTIALS_H */
