/* Set ESP32 type to ESP-S3 Dev Module
 lvgl 9.4.0, GFX Lib Arduino 1.5.0, TAMC GT911 1.0.2, Squareline 1.54.0 and
esp32 3.3.

Arduino IDE Tools menu:
Board: ESP32 Dev Module
Flash Size: 16M Flash
Flash Mode QIO 80Mhz
Partition Scheme: 16MB (2MB App/...)
PSRAM: OPI PSRAM
Events Core 1
Arduino Core 0
*/

#include "globals.h"
#include <SDL2/SDL.h>
#include <condition_variable>
#include <queue>
#include <signal.h>

// Create network objects
std::mutex dataMutex;
struct mosquitto* mosq = NULL;
bool mqtt_connected = false;
volatile bool running = true;

// Threads
pthread_t thread_mqtt, thread_weather, thread_uv, thread_solar_token, thread_current_solar, thread_daily_solar, thread_monthly_solar, thread_display_status,
    thread_connectivity_manager;

// Global variables
struct tm timeinfo;
Weather weather = {0.0, 0.0, 0.0, 0.0, false, 0, "", "", "--:--:--"};
UV uv = {0, 0, "--:--:--"};
Solar solar = {0, 0, 0, 0.0, 0.0, 0.0, 0.0, 0.0, "--:--:--", 100, 0, false, 0.0, 0.0};
Readings readings[]{READINGS_ARRAY};
std::queue<StatusMessage> statusMessageQueue;
std::mutex statusQueueMutex;
std::condition_variable statusQueueCV;
int numberOfReadings = sizeof(readings) / sizeof(readings[0]);
char chip_id[CHAR_LEN];

// Status messages
char statusMessageValue[CHAR_LEN];

// Screen setting
static lv_display_t* disp = NULL;
static lv_indev_t* mouse = NULL;

// Arrays of UI objects
#define ROOM_COUNT 5
static lv_obj_t** roomNames[ROOM_COUNT] = ROOM_NAME_LABELS;
static lv_obj_t** tempArcs[ROOM_COUNT] = TEMP_ARC_LABELS;
static lv_obj_t** tempLabels[ROOM_COUNT] = TEMP_LABELS;
static lv_obj_t** batteryLabels[ROOM_COUNT] = BATTERY_LABELS;
static lv_obj_t** directionLabels[ROOM_COUNT] = DIRECTION_LABELS;
static lv_obj_t** humidityLabels[ROOM_COUNT] = HUMIDITY_LABELS;

void setup() {
    // delay one second to enabling monitoring
    snprintf(chip_id, CHAR_LEN, "Pi5");
    printf("Starting Klaussometer 4.0 Display %s\n", chip_id);
      if (!initDataDirectory()) {
        printf("Warning: Could not initialize data directory, using current directory\n");
    }

    // Initialize LVGL and SDL display
    lv_init();
    disp = lv_sdl_window_create(1024, 600);
    mouse = lv_sdl_mouse_create();

    SDL_Window* window = SDL_GetWindowFromID(1);
    if (window) {
        SDL_SetWindowTitle(window, "Klaussometer");
    }

    if (loadDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar))) {
        logAndPublish("Solar state restored OK");
    } else {
        logAndPublish("Solar state restore failed");
    }

    if (loadDataBlock(WEATHER_DATA_FILENAME, &weather, sizeof(weather))) {
        logAndPublish("Weather state restored OK");
    } else {
        logAndPublish("Weather state restore failed");
    }

    if (loadDataBlock(UV_DATA_FILENAME, &uv, sizeof(uv))) {
        logAndPublish("UV state restored OK");
    } else {
        logAndPublish("UV state restore failed");
    }

    if (loadDataBlock(READINGS_DATA_FILENAME, &readings, sizeof(readings))) {
        logAndPublish("Readings state restored OK");
        invalidateOldReadings();
    } else {
        logAndPublish("Readings state restore failed");
    }

    mosquitto_lib_init();

    // Create mosquitto client instance
    mosq = mosquitto_new(NULL, true, NULL);
    if (!mosq) {
        logAndPublish("Failed to create mosquitto client");
        exit(1);
    }

    // Set callbacks
    mosquitto_connect_callback_set(mosq, on_connect_callback);
    mosquitto_disconnect_callback_set(mosq, on_disconnect_callback);
    mosquitto_message_callback_set(mosq, on_message_callback);

    // Connect to MQTT broker
    mqtt_connect();

    // Start mosquitto network loop in a separate thread
    mosquitto_loop_start(mosq);

    ui_init();

    // Set initial UI values
    lv_label_set_text(ui_Version, "");

    for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
        lv_label_set_text(*roomNames[i], readings[i].description);
        lv_arc_set_value(*tempArcs[i], readings[i].currentValue);
        lv_obj_add_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(*tempLabels[i], readings[i].output);
        lv_label_set_text(*directionLabels[i], "");
        lv_label_set_text(*humidityLabels[i], readings[i + ROOM_COUNT].output);
        lv_label_set_text(*batteryLabels[i], "");
    }

    lv_label_set_text(ui_FCConditions, "");
    lv_label_set_text(ui_FCWindSpeed, "");
    lv_label_set_text(ui_FCUpdateTime, "");
    lv_label_set_text(ui_FCMin, "");
    lv_label_set_text(ui_FCMax, "");
    lv_label_set_text(ui_UVUpdateTime, "");
    lv_label_set_text(ui_TempLabelFC, "--");
    lv_label_set_text(ui_UVLabel, "--");

    lv_obj_add_flag(ui_TempArcFC, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_UVArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_BatteryArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_SolarArc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_UsingArc, LV_OBJ_FLAG_HIDDEN);

    lv_arc_set_value(ui_BatteryArc, 0);
    lv_label_set_text(ui_BatteryLabel, "--");
    lv_arc_set_value(ui_SolarArc, 0);
    lv_label_set_text(ui_SolarLabel, "--");
    lv_arc_set_value(ui_UsingArc, 0);
    lv_label_set_text(ui_UsingLabel, "--");
    lv_label_set_text(ui_ChargingLabel, "");
    lv_label_set_text(ui_AsofTimeLabel, "");
    lv_label_set_text(ui_ChargingTime, "");
    lv_label_set_text(ui_SolarMinMax, "");

    lv_obj_set_style_text_color(ui_WiFiStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_ServerStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_WeatherStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_SolarStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);

    // Set to night settings at first
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Container1, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Container2, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);

    lv_label_set_text(ui_GridBought, "Bought\nToday - Pending\nThis Month - Pending");

    lv_timer_handler();

    // Get old battery min and max
    /*storage.begin("KO");
    solar.today_battery_min = storage.getFloat("solarmin");
    if (isnan(solar.today_battery_min)) {
        solar.today_battery_min = 100;
    }
    solar.today_battery_max = storage.getFloat("solarmax");
    if (isnan(solar.today_battery_max)) {
        solar.today_battery_max = 0;
    }
    storage.end();*/

    // Start tasks
    pthread_create(&thread_weather, NULL, get_weather_t, NULL);
    pthread_create(&thread_uv, NULL, get_uv_t, NULL);
    pthread_create(&thread_solar_token, NULL, get_solar_token_t, NULL);
    pthread_create(&thread_daily_solar, NULL, get_daily_solar_t, NULL);
    pthread_create(&thread_monthly_solar, NULL, get_monthly_solar_t, NULL);
    pthread_create(&thread_current_solar, NULL, get_current_solar_t, NULL);
    pthread_create(&thread_display_status, NULL, displayStatusMessages_t, NULL);
    pthread_create(&thread_connectivity_manager, NULL, connectivity_manager_t, NULL);
}

void loop() {
    char tempString[CHAR_LEN];
    char batteryIcon;
    lv_color_t batteryColour;

    // Update the global timeinfo struct
    time_t now = time(NULL);
    localtime_r(&now, &timeinfo);

    usleep(200000);
    lv_timer_handler(); // Run GUI

    // ===== Take snapshot of shared data under lock =====
    Weather weather_copy;
    UV uv_copy;
    Solar solar_copy;
    Readings readings_copy[sizeof(readings) / sizeof(readings[0])];

    {
        std::lock_guard<std::mutex> lock(dataMutex);
        memcpy(&weather_copy, &weather, sizeof(Weather));
        memcpy(&uv_copy, &uv, sizeof(UV));
        memcpy(&solar_copy, &solar, sizeof(Solar));
        memcpy(readings_copy, readings, sizeof(readings));
    }
    // ===== End snapshot =====

    for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
        lv_arc_set_value(*tempArcs[i], readings_copy[i].currentValue);
        lv_label_set_text(*tempLabels[i], readings_copy[i].output);
        if (readings_copy[i].changeChar != CHAR_NO_MESSAGE) {
            lv_obj_clear_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (readings_copy[i].changeChar == CHAR_NO_MESSAGE) {
            snprintf(tempString, CHAR_LEN, "%c", CHAR_SAME);
        } else {
            snprintf(tempString, CHAR_LEN, "%c", readings_copy[i].changeChar);
        }
        lv_label_set_text(*directionLabels[i], tempString);
        lv_label_set_text(*humidityLabels[i], readings_copy[i + ROOM_COUNT].output);
    }

    // Battery updates - use readings_copy
    for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
        getBatteryStatus(readings_copy[i + 2 * ROOM_COUNT].currentValue, &batteryIcon, &batteryColour);
        snprintf(tempString, CHAR_LEN, "%c", batteryIcon);
        lv_label_set_text(*batteryLabels[i], tempString);
        lv_obj_set_style_text_color(*batteryLabels[i], batteryColour, LV_PART_MAIN);
    }

    // Update UV - use uv_copy and weather_copy
    if (uv_copy.updateTime > 0) {
        lv_obj_clear_flag(ui_UVArc, LV_OBJ_FLAG_HIDDEN);
        if (weather_copy.isDay) {
            snprintf(tempString, sizeof(tempString), "Updated %.238s", uv_copy.time_string);
        } else {
            tempString[0] = '\0';
        }
        lv_label_set_text(ui_UVUpdateTime, tempString);
        snprintf(tempString, CHAR_LEN, "%i", uv_copy.index);
        lv_label_set_text(ui_UVLabel, tempString);
        lv_arc_set_value(ui_UVArc, uv_copy.index * 10);

        lv_obj_set_style_arc_color(ui_UVArc, lv_color_hex(uv_color(uv_copy.index)),
                                   LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(ui_UVArc, lv_color_hex(uv_color(uv_copy.index)),
                                  LV_PART_KNOB | LV_STATE_DEFAULT);
    }

    // Update weather values - use weather_copy
    if (weather_copy.updateTime > 0) {
        lv_label_set_text(ui_FCConditions, weather_copy.description);
        snprintf(tempString, CHAR_LEN, "Updated %.238s", weather_copy.time_string);
        lv_label_set_text(ui_FCUpdateTime, tempString);
        char windString[CHAR_LEN + 20];
        snprintf(windString, sizeof(windString), "Wind %2.0f km/h %s", weather_copy.windSpeed, weather_copy.windDir);
        lv_label_set_text(ui_FCWindSpeed, windString);

        lv_arc_set_value(ui_TempArcFC, weather_copy.temperature);

        snprintf(tempString, CHAR_LEN, "%2.0f", weather_copy.temperature);
        lv_label_set_text(ui_TempLabelFC, tempString);

        // Update min/max on the COPY first, then write back if changed
        bool minmax_changed = false;
        if (weather_copy.temperature < weather_copy.minTemp) {
            weather_copy.minTemp = weather_copy.temperature;
            minmax_changed = true;
        }
        if (weather_copy.temperature > weather_copy.maxTemp) {
            weather_copy.maxTemp = weather_copy.temperature;
            minmax_changed = true;
        }
        
        // Write back min/max changes under lock
        if (minmax_changed) {
            std::lock_guard<std::mutex> lock(dataMutex);
            weather.minTemp = weather_copy.minTemp;
            weather.maxTemp = weather_copy.maxTemp;
        }
    }

    if (weather_copy.updateTime > 0) {
        snprintf(tempString, CHAR_LEN, "%2.0f°C", weather_copy.minTemp);
        lv_label_set_text(ui_FCMin, tempString);
        snprintf(tempString, CHAR_LEN, "%2.0f°C", weather_copy.maxTemp);
        lv_label_set_text(ui_FCMax, tempString);
        lv_obj_clear_flag(ui_TempArcFC, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_range(ui_TempArcFC, weather_copy.minTemp, weather_copy.maxTemp);
    }

    // Update solar values - use solar_copy
    set_solar_values(&solar_copy);  // Pass copy to function
    
    if (time(NULL) - solar_copy.currentUpdateTime > 2 * SOLAR_CURRENT_UPDATE_INTERVAL_SEC) {
        lv_obj_set_style_text_color(ui_SolarStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(ui_SolarStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
    }
    if (time(NULL) - weather_copy.updateTime > 2 * WEATHER_UPDATE_INTERVAL_SEC) {
        lv_obj_set_style_text_color(ui_WeatherStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(ui_WeatherStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
    }

    if (mqtt_connected) {
        lv_obj_set_style_text_color(ui_WiFiStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(ui_WiFiStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    }

    if (mqtt_connected) {
        lv_obj_set_style_text_color(ui_ServerStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(ui_ServerStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    }

    char timeString[CHAR_LEN];
    strftime(timeString, sizeof(timeString), "%H:%M:%S", &timeinfo);
    lv_label_set_text(ui_Time, timeString);

    if (!weather_copy.isDay) {
        set_basic_text_color(lv_color_hex(COLOR_WHITE));
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container1, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container2, lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
    } else {
        set_basic_text_color(lv_color_hex(COLOR_BLACK));
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(COLOR_WHITE), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container1, lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(ui_Container2, lv_color_hex(COLOR_BLACK), LV_STATE_DEFAULT);
    }

    // Update status message
    lv_label_set_text(ui_StatusMessage, statusMessageValue);

    // Invalidate readings if too old - needs lock since it modifies readings
    invalidateOldReadings();
}

void invalidateOldReadings() {
    std::lock_guard<std::mutex> lock(dataMutex);
    for (size_t i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
        if ((time(NULL) > readings[i].lastMessageTime + (MAX_NO_MESSAGE_SEC))) {
            readings[i].changeChar = CHAR_NO_MESSAGE;
            snprintf(readings[i].output, 10, NO_READING);
            readings[i].currentValue = 0.0;
        }
    }
}

void getBatteryStatus(float batteryValue, char* iconCharacterPtr, lv_color_t* colorPtr) {
    if (batteryValue > BATTERY_OK) {
        // Battery is ok
        *iconCharacterPtr = CHAR_BATTERY_GOOD;
        *colorPtr = lv_color_hex(COLOR_GREEN);
    } else if (batteryValue > BATTERY_BAD) {
        // Battery is ok
        *iconCharacterPtr = CHAR_BATTERY_OK;
        *colorPtr = lv_color_hex(COLOR_GREEN);
    } else if (batteryValue > BATTERY_CRITICAL) {
        // Battery is low, but not critical
        *iconCharacterPtr = CHAR_BATTERY_BAD;
        *colorPtr = lv_color_hex(COLOR_YELLOW);
    } else if (batteryValue > 0.0) {
        // Battery is critical
        *iconCharacterPtr = CHAR_BATTERY_CRITICAL;
        *colorPtr = lv_color_hex(COLOR_RED);
    } else {
        *iconCharacterPtr = CHAR_BLANK;
        *colorPtr = lv_color_hex(COLOR_GREEN);
    }
}

void* displayStatusMessages_t(void* pvParameters) {
    (void)pvParameters;
    StatusMessage receivedMsg;

    while (true) {
        {
            std::unique_lock<std::mutex> lock(statusQueueMutex);
            statusQueueCV.wait(lock, [] {
                return !statusMessageQueue.empty();
            });
            receivedMsg = statusMessageQueue.front();
            statusMessageQueue.pop();
        }
        snprintf(statusMessageValue, CHAR_LEN, "%s", receivedMsg.text);
        // Wait for the specified duration before clearing the message.
        usleep(receivedMsg.duration_s * 1000000);
        // Clear the label after the duration has passed.
        statusMessageValue[0] = '\0';
    }
    return NULL;
}

void logAndPublish(const char* messageBuffer) {
    printf("LOG: %s\n", messageBuffer);

    StatusMessage msg;
    snprintf(msg.text, CHAR_LEN, "%s", messageBuffer);
    msg.duration_s = STATUS_MESSAGE_TIME;
    {
        std::lock_guard<std::mutex> lock(statusQueueMutex);
        statusMessageQueue.push(msg);
    }
    statusQueueCV.notify_one();
}

void errorPublish(const char* messageBuffer) {
    printf("ERROR: %s\n", messageBuffer);
}

void signal_handler(int sig) {
    (void)sig;
    running = false;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    setup();
    
    while (running) {
        loop();
        usleep(5000);
    }
    
    // Cleanup
    mosquitto_loop_stop(mosq, true);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    
    printf("Klaussometer shutdown complete\n");
    return 0;
}