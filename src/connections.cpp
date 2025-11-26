#include "globals.h"
#include <mosquitto.h>

extern struct mosquitto* mosq;
extern Readings readings[];
extern int numberOfReadings;
extern struct tm timeinfo;
extern bool mqtt_connected;

// Callback when connection is established
void on_connect_callback(struct mosquitto* mosq, void* obj, int rc) {
    (void)obj;
    if (rc == 0) {
        mqtt_connected = true;
        logAndPublish("Connected to the MQTT broker");

        // Subscribe to all topics
        for (int i = 0; i < numberOfReadings; i++) {
            mosquitto_subscribe(mosq, NULL, readings[i].topic, 0);
        }
    } else {
        mqtt_connected = false;
        char log_msg[CHAR_LEN];
        snprintf(log_msg, CHAR_LEN, "MQTT connection failed with code: %d", rc);
        logAndPublish(log_msg);
    }
}

// Callback when connection is lost
void on_disconnect_callback(struct mosquitto* mosq, void* obj, int rc) {
    (void)obj;
    (void)mosq;
    mqtt_connected = false;
    if (rc != 0) {
        logAndPublish("MQTT connection lost unexpectedly");
    }
}

// Callback when message is received
void on_message_callback(struct mosquitto* mosq, void* obj, const struct mosquitto_message* msg) {
    (void)mosq;
    (void)obj;
    // This will be handled in mqtt.cpp - just forward to processing function
    process_mqtt_message(msg->topic, (char*)msg->payload, msg->payloadlen);
}

void mqtt_connect() {
    char messageBuffer[CHAR_LEN];
    snprintf(messageBuffer, CHAR_LEN, "Connecting to MQTT broker %s:%d", MQTT_SERVER, MQTT_PORT);
    logAndPublish(messageBuffer);

    if (mosq == NULL) {
        logAndPublish("MQTT client not initialized");
        return;
    }

    // Set username and password
    mosquitto_username_pw_set(mosq, MQTT_USER, MQTT_PASSWORD);

    // Connect to broker
    int rc = mosquitto_connect(mosq, MQTT_SERVER, MQTT_PORT, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
        char log_msg[CHAR_LEN];
        snprintf(log_msg, CHAR_LEN, "MQTT connect failed: %s", mosquitto_strerror(rc));
        logAndPublish(log_msg);
    }
}

void* connectivity_manager_t(void* pvParameters) {
    (void)pvParameters;
    bool wasDisconnected;

    while (true) {
        wasDisconnected = false;

        // Check MQTT connection
        if (!mqtt_connected) {
            wasDisconnected = true;
            logAndPublish("MQTT is reconnecting");
            mqtt_connect();
            usleep(1000000);
        }

        if (wasDisconnected) {
            logAndPublish("MQTT reconnected successfully");
        }

        usleep(5000000); // Check connection status every 5 seconds
    }
}