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
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>

// Create network objects
pthread_mutex_t mqttMutex = PTHREAD_MUTEX_INITIALIZER;
struct mosquitto *mosq = NULL;
bool mqtt_connected = false;

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
static uint32_t screenWidth = LCD_WIDTH;
static uint32_t screenHeight = LCD_HEIGHT;
static lv_display_t* disp = NULL;
static lv_indev_t* mouse = NULL;
static lv_color_t* disp_draw_buf;

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
    
    // Initialize LVGL and SDL display
    lv_init();
    disp = lv_sdl_window_create(1024, 600);
    mouse = lv_sdl_mouse_create();
    
    // Setup queues and mutexes
    // Using std::queue with mutex and condition_variable instead of FreeRTOS queue

    // Initialize the SD card
    /*SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    if (!SD_MMC.begin("/sdcard", true, true)) {
        logAndPublish("SD Card initialization failed!");
    } else {
        logAndPublish("SD Card initialized");
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
    }*/
    mosquitto_lib_init();
    
    // Create mosquitto client instance
    mosq = mosquitto_new(NULL, true, NULL);
    if(!mosq) {
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

    // Allocate display buffer
    size_t bufferSize = sizeof(lv_color_t) * screenWidth * 10;

    disp_draw_buf = (lv_color_t*)malloc(bufferSize);

    // Create LVGL display
    disp = lv_display_create(screenWidth, screenHeight);
    if (!disp) {
        while (1) {
            usleep(100000);
        }
    }
    lv_display_set_flush_cb(disp, my_disp_flush);
    lv_display_set_buffers(disp, disp_draw_buf, NULL, bufferSize, LV_DISPLAY_RENDER_MODE_PARTIAL);
    ui_init();

    // Set initial UI values
    std::string versionText = "V " + std::string("Dummy Version");
    lv_label_set_text(ui_Version, versionText.c_str());

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
    pthread_create(&thread_solar_token, NULL, get_solar_token_t, NULL);
}

void loop() {
    char tempString[CHAR_LEN];
    char batteryIcon;
    lv_color_t batteryColour;


    usleep((200000));
    lv_timer_handler(); // Run GUI


    // Update values
    for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
        lv_arc_set_value(*tempArcs[i], readings[i].currentValue);
        lv_label_set_text(*tempLabels[i], readings[i].output);
        if (readings[i].changeChar != CHAR_NO_MESSAGE) {
            lv_obj_clear_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(*tempArcs[i], LV_OBJ_FLAG_HIDDEN);
        }
        if (readings[i].changeChar == CHAR_NO_MESSAGE) {
            snprintf(tempString, CHAR_LEN, "%c", CHAR_SAME);
        } else {
            snprintf(tempString, CHAR_LEN, "%c", readings[i].changeChar);
        }
        lv_label_set_text(*directionLabels[i], tempString);
        lv_label_set_text(*humidityLabels[i], readings[i + ROOM_COUNT].output);
    }

    // Battery updates
    for (unsigned char i = 0; i < ROOM_COUNT; ++i) {
        getBatteryStatus(readings[i + 2 * ROOM_COUNT].currentValue, readings[i + 2 * ROOM_COUNT].readingIndex, &batteryIcon, &batteryColour);
        snprintf(tempString, CHAR_LEN, "%c", batteryIcon);
        lv_label_set_text(*batteryLabels[i], tempString);
        lv_obj_set_style_text_color(*batteryLabels[i], batteryColour, LV_PART_MAIN);
    }

    // Update UV
    if (uv.updateTime > 0) {
        lv_obj_clear_flag(ui_UVArc, LV_OBJ_FLAG_HIDDEN);
        if (weather.isDay) {
            snprintf(tempString, CHAR_LEN, "Updated %s", uv.time_string);
        } else {
            snprintf(tempString, CHAR_LEN, "");
        }
        lv_label_set_text(ui_UVUpdateTime, tempString);
        snprintf(tempString, CHAR_LEN, "%i", uv.index);
        lv_label_set_text(ui_UVLabel, tempString);
        lv_arc_set_value(ui_UVArc, uv.index * 10);

        lv_obj_set_style_arc_color(ui_UVArc, lv_color_hex(uv_color(uv.index)),
                                   LV_PART_INDICATOR | LV_STATE_DEFAULT); // Set arc to color
        lv_obj_set_style_bg_color(ui_UVArc, lv_color_hex(uv_color(uv.index)),
                                  LV_PART_KNOB | LV_STATE_DEFAULT); // Set arc to color
    }

    // Update weather values
    if (weather.updateTime > 0) {
        lv_label_set_text(ui_FCConditions, weather.description);
        snprintf(tempString, CHAR_LEN, "Updated %s", weather.time_string);
        lv_label_set_text(ui_FCUpdateTime, tempString);
        snprintf(tempString, CHAR_LEN, "Wind %2.0f km/h %s", weather.windSpeed, weather.windDir);
        lv_label_set_text(ui_FCWindSpeed, tempString);

        lv_arc_set_value(ui_TempArcFC, weather.temperature);

        snprintf(tempString, CHAR_LEN, "%2.0f", weather.temperature);
        lv_label_set_text(ui_TempLabelFC, tempString);

        // Set min max if outside the expected values
        if (weather.temperature < weather.minTemp) {
            weather.minTemp = weather.temperature;
        }
        if (weather.temperature > weather.maxTemp) {
            weather.maxTemp = weather.temperature;
        }
    }

    if (weather.updateTime > 0) {
        snprintf(tempString, CHAR_LEN, "%2.0f°C", weather.minTemp);
        lv_label_set_text(ui_FCMin, tempString);
        snprintf(tempString, CHAR_LEN, "%2.0f°C", weather.maxTemp);
        lv_label_set_text(ui_FCMax, tempString);
        lv_obj_clear_flag(ui_TempArcFC, LV_OBJ_FLAG_HIDDEN);
        lv_arc_set_range(ui_TempArcFC, weather.minTemp, weather.maxTemp);
    }

    // Update solar values
    set_solar_values();
    if (time(NULL) - solar.currentUpdateTime > 2 * SOLAR_CURRENT_UPDATE_INTERVAL_SEC) {
        lv_obj_set_style_text_color(ui_SolarStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(ui_SolarStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
    }
    if (time(NULL) - weather.updateTime > 2 * WEATHER_UPDATE_INTERVAL_SEC) {
        lv_obj_set_style_text_color(ui_WeatherStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(ui_WeatherStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
    }

    /*if (WiFi.status() == WL_CONNECTED) {
        lv_obj_set_style_text_color(ui_WiFiStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(ui_WiFiStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    }

    if (mqttClient.connected()) {
        lv_obj_set_style_text_color(ui_ServerStatus, lv_color_hex(COLOR_GREEN), LV_PART_MAIN);
    } else {
        lv_obj_set_style_text_color(ui_ServerStatus, lv_color_hex(COLOR_RED), LV_PART_MAIN);
    }

    if (!getLocalTime(&timeinfo)) {
        lv_label_set_text(ui_Time, "Syncing");
    } else {
        char timeString[CHAR_LEN];
        strftime(timeString, sizeof(timeString), "%H:%M:%S", &timeinfo);
        lv_label_set_text(ui_Time, timeString);
    }*/

    if (!weather.isDay) {
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

    // Invalidate readings if too old
    invalidateOldReadings();
}

void invalidateOldReadings() {
    if (time(NULL) > TIME_SYNC_THRESHOLD) {
        for (int i = 0; i < sizeof(readings) / sizeof(readings[0]); i++) {
            if ((time(NULL) > readings[i].lastMessageTime + (MAX_NO_MESSAGE_SEC))) {
                printf("Invalidating reading %d\n", i);
                readings[i].changeChar = CHAR_NO_MESSAGE;
                snprintf(readings[i].output, 10, NO_READING);
                readings[i].currentValue = 0.0;
            }
        }
    }
}

void getBatteryStatus(float batteryValue, int readingIndex, char* iconCharacterPtr, lv_color_t* colorPtr) {
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
    StatusMessage receivedMsg;

    while (true) {
        {
            std::unique_lock<std::mutex> lock(statusQueueMutex);
            statusQueueCV.wait(lock, []{ return !statusMessageQueue.empty(); });
            receivedMsg = statusMessageQueue.front();
            statusMessageQueue.pop();
        }
        snprintf(statusMessageValue, CHAR_LEN, "%s", receivedMsg.text);
        // Wait for the specified duration before clearing the message.
        std::this_thread::sleep_for(std::chrono::seconds(receivedMsg.duration_s));
        // Clear the label after the duration has passed.
        snprintf(statusMessageValue, CHAR_LEN, "");
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

