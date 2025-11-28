#include "globals.h"
#include <mutex>

extern struct mosquitto* mosq;
extern std::mutex mqttMutex;
extern Readings readings[];
extern int numberOfReadings;
extern std::mutex dataMutex;

// Process received MQTT message
void process_mqtt_message(const char* topic, char* payload, int payloadlen) {
    char recMessage[CHAR_LEN];

    // Copy payload and null-terminate
    if (payloadlen >= CHAR_LEN) {
        logAndPublish("MQTT message exceeds buffer size");
        return;
    }

    memcpy(recMessage, payload, payloadlen);
    recMessage[payloadlen] = '\0';

    // Validate message
    if (payloadlen == 0 || recMessage[0] == '\0') {
        char log_msg[CHAR_LEN];
        snprintf(log_msg, CHAR_LEN, "Empty MQTT message on topic: %s", topic);
        logAndPublish(log_msg);
        return;
    }

    // Find matching topic and process
    bool messageProcessed = false;
    for (int i = 0; i < numberOfReadings; i++) {
        if (strcmp(topic, readings[i].topic) == 0) {
            if (readings[i].dataType == DATA_TEMPERATURE || readings[i].dataType == DATA_HUMIDITY || readings[i].dataType == DATA_BATTERY) {
                {
                    std::lock_guard<std::mutex> lock(dataMutex);
                    update_readings(recMessage, i, readings[i].dataType);
                }
                messageProcessed = true;
            }
            break;
        }
    }

    if (!messageProcessed) {
        char log_msg[CHAR_LEN];
        snprintf(log_msg, CHAR_LEN, "Unhandled MQTT topic: %.100s, message: %.100s", topic, recMessage);
        logAndPublish(log_msg);
    }

    saveDataBlock(READINGS_DATA_FILENAME, readings, sizeof(Readings) * numberOfReadings);
}

void update_readings(char* recMessage, int index, int dataType) {
    float averageHistory;
    float totalHistory = 0.0;
    const char* log_message_suffix;
    const char* format_string;

    readings[index].currentValue = atof(recMessage);

    // Set format string and log suffix based on data type
    switch (dataType) {
    case DATA_TEMPERATURE:
        format_string = "%2.1f";
        log_message_suffix = "temperature";
        break;
    case DATA_HUMIDITY:
        format_string = "%2.0f%s";
        log_message_suffix = "humidity";
        break;
    case DATA_BATTERY:
        format_string = "%2.1f";
        log_message_suffix = "battery";
        break;
    default:
        return;
    }

    if (dataType == DATA_HUMIDITY) {
        snprintf(readings[index].output, 10, format_string, readings[index].currentValue, "%");
    } else {
        snprintf(readings[index].output, 10, format_string, readings[index].currentValue);
    }

    if (readings[index].readingIndex == 0) {
        readings[index].changeChar = CHAR_BLANK;
        readings[index].lastValue[0] = readings[index].currentValue;
    } else {
        for (int i = 0; i < readings[index].readingIndex; i++) {
            totalHistory += readings[index].lastValue[i];
        }
        averageHistory = totalHistory / readings[index].readingIndex;

        if (dataType == DATA_TEMPERATURE || dataType == DATA_HUMIDITY) {
            if (readings[index].currentValue > averageHistory) {
                readings[index].changeChar = CHAR_UP;
            } else if (readings[index].currentValue < averageHistory) {
                readings[index].changeChar = CHAR_DOWN;
            } else {
                readings[index].changeChar = CHAR_SAME;
            }
        }
    }

    if (readings[index].readingIndex == STORED_READING) {
        readings[index].readingIndex--;
        readings[index].enoughData = true;
        for (int i = 0; i < STORED_READING - 1; i++) {
            readings[index].lastValue[i] = readings[index].lastValue[i + 1];
        }
    } else {
        readings[index].enoughData = false;
    }

    readings[index].lastValue[readings[index].readingIndex] = readings[index].currentValue;
    readings[index].readingIndex++;
    readings[index].lastMessageTime = time(NULL);

    char log_message[CHAR_LEN];
    snprintf(log_message, CHAR_LEN, "%s %s updated", readings[index].description, log_message_suffix);
    logAndPublish(log_message);
}