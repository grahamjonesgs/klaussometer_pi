#include "globals.h"
#include <curl/curl.h>
#include <json-c/json.h>

extern Weather weather;
extern UV uv;
extern Solar solar;
extern std::mutex dataMutex;

// Token management - private to this file
static std::mutex tokenMutex;
static char solar_token[SOLAR_TOKEN_LENGTH+1] = {0};

// Buffer sizes
static const size_t URL_BUFFER_SIZE = 512;
static const size_t POST_BUFFER_SIZE = 1024;

// Callback structure for libcurl
struct MemoryStruct {
    char* memory;
    size_t size;
};

// Callback function to receive HTTP response data
static size_t WriteMemoryCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    struct MemoryStruct* mem = (struct MemoryStruct*)userp;

    char* ptr = (char*)realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        printf("Not enough memory (realloc returned NULL)\n");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

// Token helper functions
static bool get_solar_token_copy(char* dest, size_t dest_size) {
    std::lock_guard<std::mutex> lock(tokenMutex);
    bool has_token = (strlen(solar_token) > 0);
    if (has_token) {
        strncpy(dest, solar_token, dest_size - 1);
        dest[dest_size - 1] = '\0';
    }
    return has_token;
}

static void set_solar_token(const char* token) {
    std::lock_guard<std::mutex> lock(tokenMutex);
    snprintf(solar_token, SOLAR_TOKEN_LENGTH, "bearer %s", token);
}

static void clear_solar_token(void) {
    std::lock_guard<std::mutex> lock(tokenMutex);
    solar_token[0] = '\0';
}

static bool has_solar_token(void) {
    std::lock_guard<std::mutex> lock(tokenMutex);
    bool has_token = (strlen(solar_token) > 0);
    return has_token;
}

// Helper to initialise curl with common settings
static CURL* init_curl_request(const char* url, struct MemoryStruct* chunk) {
    chunk->memory = (char*)malloc(1);
    if (!chunk->memory) {
        return NULL;
    }
    chunk->size = 0;

    CURL* curl = curl_easy_init();
    if (!curl) {
        free(chunk->memory);
        chunk->memory = NULL;
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    return curl;
}

// Helper to get current time string
static void get_current_time_string(char* buffer, size_t buffer_size) {
    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    strftime(buffer, buffer_size, "%H:%M:%S", &local_time);
}

// Get UV from weatherbit.io
void* get_uv_t(void* pvParameters) {
    (void)pvParameters;

    while (true) {
        bool is_day;
        time_t last_update;

        // Read shared state
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            is_day = weather.isDay;
            last_update = uv.updateTime;
        }

        if (is_day) {
            if (time(NULL) - last_update > UV_UPDATE_INTERVAL_SEC) {
                char url_buffer[URL_BUFFER_SIZE];
                snprintf(url_buffer, URL_BUFFER_SIZE,
                         "https://api.weatherbit.io/v2.0/current?city_id=%s&key=%s",
                         WEATHERBIT_CITY_ID, WEATHERBIT_API);

                struct MemoryStruct chunk;
                CURL* curl = init_curl_request(url_buffer, &chunk);

                if (curl) {
                    CURLcode res = curl_easy_perform(curl);

                    if (res == CURLE_OK) {
                        long response_code;
                        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

                        if (response_code == 200 && chunk.size > 0) {
                            struct json_object* root = json_tokener_parse(chunk.memory);

                            if (root != NULL) {
                                struct json_object* data_array;
                                if (json_object_object_get_ex(root, "data", &data_array)) {
                                    struct json_object* first_element = json_object_array_get_idx(data_array, 0);
                                    if (first_element) {
                                        struct json_object* uv_obj;
                                        if (json_object_object_get_ex(first_element, "uv", &uv_obj)) {
                                            float uv_value = json_object_get_double(uv_obj);
                                            char time_string[CHAR_LEN];
                                            get_current_time_string(time_string, sizeof(time_string));

                                            {
                                                std::lock_guard<std::mutex> lock(dataMutex);
                                                uv.index = uv_value;
                                                uv.updateTime = time(NULL);
                                                strncpy(uv.time_string, time_string, CHAR_LEN - 1);
                                                uv.time_string[CHAR_LEN - 1] = '\0';
                                            }

                                            logAndPublish("UV updated");
                                            saveDataBlock(UV_DATA_FILENAME, &uv, sizeof(uv));
                                        }
                                    }
                                }
                                json_object_put(root);
                            } else {
                                logAndPublish("UV update failed: JSON parse error");
                                {
                                    std::lock_guard<std::mutex> lock(dataMutex);
                                    uv.updateTime = time(NULL);
                                }
                            }
                        } else {
                            char log_message[CHAR_LEN];
                            snprintf(log_message, CHAR_LEN, "[HTTP] GET UV failed, response code: %ld", response_code);
                            errorPublish(log_message);
                            logAndPublish("UV update failed");
                            usleep(API_FAIL_DELAY_SEC * 1000000);
                        }
                    } else {
                        char log_message[CHAR_LEN];
                        snprintf(log_message, CHAR_LEN, "[HTTP] GET UV failed: %s", curl_easy_strerror(res));
                        errorPublish(log_message);
                        logAndPublish("UV update failed");
                        usleep(API_FAIL_DELAY_SEC * 1000000);
                    }

                    free(chunk.memory);
                    curl_easy_cleanup(curl);
                }
            }
        } else {
            // Night time - set UV to 0
            time_t weather_update;
            {
                std::lock_guard<std::mutex> lock(dataMutex);
                weather_update = weather.updateTime;
            }

            char time_string[CHAR_LEN];
            get_current_time_string(time_string, sizeof(time_string));

            {
                std::lock_guard<std::mutex> lock(dataMutex);
                uv.index = 0.0;
                if (weather_update > 0) {
                    uv.updateTime = time(NULL);
                }
                strncpy(uv.time_string, time_string, CHAR_LEN - 1);
                uv.time_string[CHAR_LEN - 1] = '\0';
            }

            saveDataBlock(UV_DATA_FILENAME, &uv, sizeof(uv));
        }
        usleep(API_LOOP_DELAY_SEC * 1000000);
    }
    return NULL;
}

void* get_weather_t(void* pvParameters) {
    (void)pvParameters;

    while (true) {
        time_t last_update;
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            last_update = weather.updateTime;
        }

        if (time(NULL) - last_update > WEATHER_UPDATE_INTERVAL_SEC) {
            char url_buffer[URL_BUFFER_SIZE];
            snprintf(url_buffer, URL_BUFFER_SIZE,
                     "https://api.open-meteo.com/v1/"
                     "forecast?latitude=%s&longitude=%s&daily="
                     "temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max"
                     "&models=ukmo_uk_deterministic_2km,ncep_gfs013"
                     "&current=temperature_2m,is_day,weather_code,wind_speed_10m,wind_direction_10m"
                     "&timezone=auto&forecast_days=1",
                     LATITUDE, LONGITUDE);

            struct MemoryStruct chunk;
            CURL* curl = init_curl_request(url_buffer, &chunk);

            if (curl) {
                CURLcode res = curl_easy_perform(curl);

                if (res == CURLE_OK) {
                    long response_code;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

                    if (response_code == 200 && chunk.size > 0) {
                        struct json_object* root = json_tokener_parse(chunk.memory);

                        if (root != NULL) {
                            struct json_object *current_obj, *daily_obj;

                            if (json_object_object_get_ex(root, "current", &current_obj) &&
                                json_object_object_get_ex(root, "daily", &daily_obj)) {

                                struct json_object *temp_obj, *wind_dir_obj, *wind_speed_obj;
                                struct json_object *is_day_obj, *weather_code_obj;

                                json_object_object_get_ex(current_obj, "temperature_2m", &temp_obj);
                                json_object_object_get_ex(current_obj, "wind_direction_10m", &wind_dir_obj);
                                json_object_object_get_ex(current_obj, "wind_speed_10m", &wind_speed_obj);
                                json_object_object_get_ex(current_obj, "is_day", &is_day_obj);
                                json_object_object_get_ex(current_obj, "weather_code", &weather_code_obj);

                                struct json_object *max_temp_array, *min_temp_array;
                                json_object_object_get_ex(daily_obj, "temperature_2m_max", &max_temp_array);
                                json_object_object_get_ex(daily_obj, "temperature_2m_min", &min_temp_array);

                                if (temp_obj && wind_dir_obj && wind_speed_obj && is_day_obj &&
                                    weather_code_obj && max_temp_array && min_temp_array) {

                                    float weatherTemperature = json_object_get_double(temp_obj);
                                    float weatherWindDir = json_object_get_double(wind_dir_obj);
                                    float weatherWindSpeed = json_object_get_double(wind_speed_obj);
                                    bool weatherIsDay = json_object_get_boolean(is_day_obj);
                                    int weatherCode = json_object_get_int(weather_code_obj);

                                    struct json_object* max_temp_0 = json_object_array_get_idx(max_temp_array, 0);
                                    struct json_object* min_temp_0 = json_object_array_get_idx(min_temp_array, 0);
                                    float weatherMaxTemp = json_object_get_double(max_temp_0);
                                    float weatherMinTemp = json_object_get_double(min_temp_0);

                                    const char* description = wmoToText(weatherCode, weatherIsDay);
                                    const char* windDir = degreesToDirection(weatherWindDir);
                                    char time_string[CHAR_LEN];
                                    get_current_time_string(time_string, sizeof(time_string));

                                    {
                                        std::lock_guard<std::mutex> lock(dataMutex);
                                        weather.temperature = weatherTemperature;
                                        weather.windSpeed = weatherWindSpeed;
                                        weather.maxTemp = weatherMaxTemp;
                                        weather.minTemp = weatherMinTemp;
                                        weather.isDay = weatherIsDay;
                                        snprintf(weather.description, CHAR_LEN, "%s", description);
                                        snprintf(weather.windDir, CHAR_LEN, "%s", windDir);
                                        weather.updateTime = time(NULL);
                                        strncpy(weather.time_string, time_string, CHAR_LEN - 1);
                                        weather.time_string[CHAR_LEN - 1] = '\0';
                                    }

                                    logAndPublish("Weather updated");
                                    saveDataBlock(WEATHER_DATA_FILENAME, &weather, sizeof(weather));
                                }
                            }
                            json_object_put(root);
                        } else {
                            logAndPublish("Weather update failed: JSON parse error");
                        }
                    } else {
                        char log_message[CHAR_LEN];
                        snprintf(log_message, CHAR_LEN, "[HTTP] GET weather failed, response code: %ld", response_code);
                        errorPublish(log_message);
                        logAndPublish("Weather update failed");
                        usleep(API_FAIL_DELAY_SEC * 1000000);
                    }
                } else {
                    char log_message[CHAR_LEN];
                    snprintf(log_message, CHAR_LEN, "[HTTP] GET weather failed: %s", curl_easy_strerror(res));
                    errorPublish(log_message);
                    logAndPublish("Weather update failed");
                    usleep(API_FAIL_DELAY_SEC * 1000000);
                }

                free(chunk.memory);
                curl_easy_cleanup(curl);
            }
        }
        usleep(API_LOOP_DELAY_SEC * 1000000);
    }
    return NULL;
}

const char* degreesToDirection(double degrees) {
    degrees = fmod(degrees, 360.0);
    if (degrees < 0) {
        degrees += 360.0;
    }

    double shiftedDegrees = degrees + 22.5;
    if (shiftedDegrees >= 360) {
        shiftedDegrees -= 360;
    }

    if (shiftedDegrees < 45) {
        return "N";
    } else if (shiftedDegrees < 90) {
        return "NE";
    } else if (shiftedDegrees < 135) {
        return "E";
    } else if (shiftedDegrees < 180) {
        return "SE";
    } else if (shiftedDegrees < 225) {
        return "S";
    } else if (shiftedDegrees < 270) {
        return "SW";
    } else if (shiftedDegrees < 315) {
        return "W";
    } else {
        return "NW";
    }
}

const char* wmoToText(int code, bool isDay) {
    switch (code) {
    case 0:
        return isDay ? "Sunny" : "Clear";
    case 1:
        return isDay ? "Mainly sunny" : "Mostly clear";
    case 2:
        return "Partly cloudy";
    case 3:
        return "Overcast";
    case 45:
        return "Fog";
    case 48:
        return "Depositing rime fog";
    case 51:
        return "Light drizzle";
    case 53:
        return "Moderate drizzle";
    case 55:
        return "Dense drizzle";
    case 56:
        return "Light freezing drizzle";
    case 57:
        return "Dense freezing drizzle";
    case 61:
        return "Slight rain";
    case 63:
        return "Moderate rain";
    case 65:
        return "Heavy rain";
    case 66:
        return "Light freezing rain";
    case 67:
        return "Heavy freezing rain";
    case 71:
        return "Slight snow fall";
    case 73:
        return "Moderate snow fall";
    case 75:
        return "Heavy snow fall";
    case 77:
        return "Snow grains";
    case 80:
        return "Slight rain showers";
    case 81:
        return "Moderate rain showers";
    case 82:
        return "Violent rain showers";
    case 85:
        return "Slight snow showers";
    case 86:
        return "Heavy snow showers";
    case 95:
        return "Thunderstorm";
    case 96:
        return "Thunderstorm with slight hail";
    case 99:
        return "Thunderstorm with heavy hail";
    default:
        return "Unknown weather code";
    }
}

void* get_solar_token_t(void* pvParameters) {
    (void)pvParameters;

    while (true) {
        if (!has_solar_token()) {
            char url_buffer[URL_BUFFER_SIZE];
            char post_buffer[POST_BUFFER_SIZE];

            snprintf(url_buffer, sizeof(url_buffer),
                     "https://%s/account/v1.0/token?appId=%s",
                     SOLAR_URL, SOLAR_APPID);

            snprintf(post_buffer, POST_BUFFER_SIZE,
                     "{\"appSecret\":\"%s\",\"email\":\"%s\",\"password\":\"%s\"}",
                     SOLAR_SECRET, SOLAR_USERNAME, SOLAR_PASSHASH);

            struct MemoryStruct chunk;
            CURL* curl = init_curl_request(url_buffer, &chunk);

            if (curl) {
                struct curl_slist* headers = NULL;
                headers = curl_slist_append(headers, "Content-Type: application/json");

                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_buffer);

                CURLcode res = curl_easy_perform(curl);

                if (res == CURLE_OK) {
                    long response_code;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

                    if (response_code == 200 && chunk.size > 0) {
                        struct json_object* root = json_tokener_parse(chunk.memory);

                        if (root != NULL) {
                            struct json_object* token_obj;
                            if (json_object_object_get_ex(root, "access_token", &token_obj)) {
                                if (json_object_is_type(token_obj, json_type_string)) {
                                    const char* rec_token = json_object_get_string(token_obj);
                                    set_solar_token(rec_token);
                                    logAndPublish("Solar token obtained");
                                }
                            } else {
                                struct json_object* msg_obj;
                                if (json_object_object_get_ex(root, "msg", &msg_obj)) {
                                    char log_message[CHAR_LEN];
                                    snprintf(log_message, CHAR_LEN, "Solar token error: %s",
                                             json_object_get_string(msg_obj));
                                    errorPublish(log_message);
                                }
                            }
                            json_object_put(root);
                        }
                    } else {
                        char log_message[CHAR_LEN];
                        snprintf(log_message, CHAR_LEN,
                                 "[HTTP] GET solar token failed, response code: %ld", response_code);
                        errorPublish(log_message);
                    }
                } else {
                    char log_message[CHAR_LEN];
                    snprintf(log_message, CHAR_LEN, "[HTTP] GET solar token failed: %s",
                             curl_easy_strerror(res));
                    errorPublish(log_message);
                }

                free(chunk.memory);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
            }
        }
        usleep(API_LOOP_DELAY_SEC * 1000000);
    }
    return NULL;
}

// Get current solar values from Solarman
void* get_current_solar_t(void* pvParameters) {
    (void)pvParameters;

    while (true) {
        time_t last_update;
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            last_update = solar.currentUpdateTime;
        }

        if (time(NULL) - last_update > SOLAR_CURRENT_UPDATE_INTERVAL_SEC) {
            char local_token[SOLAR_TOKEN_LENGTH];
            if (!get_solar_token_copy(local_token, sizeof(local_token))) {
                usleep(SOLAR_TOKEN_WAIT_SEC * 1000000);
                continue;
            }

            char url_buffer[URL_BUFFER_SIZE];
            char post_buffer[POST_BUFFER_SIZE];

            snprintf(url_buffer, URL_BUFFER_SIZE,
                     "https://%s/station/v1.0/realTime?language=en", SOLAR_URL);

            snprintf(post_buffer, POST_BUFFER_SIZE,
                     "{\"stationId\":\"%s\"}", SOLAR_STATIONID);

            struct MemoryStruct chunk;
            CURL* curl = init_curl_request(url_buffer, &chunk);

            if (curl) {
                struct curl_slist* headers = NULL;
                headers = curl_slist_append(headers, "Content-Type: application/json");

                char auth_header[SOLAR_TOKEN_LENGTH + 20];
                snprintf(auth_header, sizeof(auth_header), "Authorization: %s", local_token);
                headers = curl_slist_append(headers, auth_header);

                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_buffer);

                CURLcode res = curl_easy_perform(curl);

                if (res == CURLE_OK) {
                    long response_code;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

                    if (response_code == 200 && chunk.size > 0) {
                        struct json_object* root = json_tokener_parse(chunk.memory);

                        if (root != NULL) {
                            struct json_object* success_obj;
                            if (json_object_object_get_ex(root, "success", &success_obj)) {
                                bool rec_success = json_object_get_boolean(success_obj);

                                if (rec_success) {
                                    struct json_object *battery_soc_obj, *use_power_obj, *wire_power_obj;
                                    struct json_object *battery_power_obj, *last_update_obj, *generation_power_obj;

                                    json_object_object_get_ex(root, "batterySoc", &battery_soc_obj);
                                    json_object_object_get_ex(root, "usePower", &use_power_obj);
                                    json_object_object_get_ex(root, "wirePower", &wire_power_obj);
                                    json_object_object_get_ex(root, "batteryPower", &battery_power_obj);
                                    json_object_object_get_ex(root, "lastUpdateTime", &last_update_obj);
                                    json_object_object_get_ex(root, "generationPower", &generation_power_obj);

                                    if (battery_soc_obj && use_power_obj && wire_power_obj &&
                                        battery_power_obj && last_update_obj && generation_power_obj) {

                                        float rec_batteryCharge = json_object_get_double(battery_soc_obj);
                                        float rec_usingPower = json_object_get_double(use_power_obj);
                                        float rec_gridPower = json_object_get_double(wire_power_obj);
                                        float rec_batteryPower = json_object_get_double(battery_power_obj);
                                        time_t rec_time = (time_t)json_object_get_int64(last_update_obj);
                                        float rec_solarPower = json_object_get_double(generation_power_obj);

                                        struct tm ts;
                                        char time_buf[CHAR_LEN];
                                        localtime_r(&rec_time, &ts);
                                        strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &ts);

                                        {
                                            std::lock_guard<std::mutex> lock(dataMutex);
                                            solar.currentUpdateTime = time(NULL);
                                            solar.solarPower = rec_solarPower / 1000;
                                            solar.batteryPower = rec_batteryPower / 1000;
                                            solar.usingPower = rec_usingPower / 1000;
                                            solar.batteryCharge = rec_batteryCharge;
                                            solar.gridPower = rec_gridPower / 1000;
                                            snprintf(solar.time, CHAR_LEN, "%s", time_buf);
                                        }

                                        logAndPublish("Solar status updated");
                                        saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
                                    }
                                } else {
                                    struct json_object* msg_obj;
                                    if (json_object_object_get_ex(root, "msg", &msg_obj)) {
                                        const char* msg = json_object_get_string(msg_obj);
                                        if (msg && strcmp(msg, "auth invalid token") == 0) {
                                            logAndPublish("Solar token expired, clearing for refresh");
                                            clear_solar_token();
                                        } else {
                                            char log_message[CHAR_LEN];
                                            snprintf(log_message, CHAR_LEN, "Solar status failed: %s", msg);
                                            errorPublish(log_message);
                                        }
                                    }
                                }
                            }
                            json_object_put(root);
                        } else {
                            logAndPublish("Solar status update failed: JSON parse error");
                        }
                    } else {
                        char log_message[CHAR_LEN];
                        snprintf(log_message, CHAR_LEN,
                                 "[HTTP] GET solar status failed, response code: %ld", response_code);
                        errorPublish(log_message);
                        usleep(API_FAIL_DELAY_SEC * 1000000);
                    }
                } else {
                    char log_message[CHAR_LEN];
                    snprintf(log_message, CHAR_LEN, "[HTTP] GET solar status failed: %s",
                             curl_easy_strerror(res));
                    errorPublish(log_message);
                    usleep(API_FAIL_DELAY_SEC * 1000000);
                }

                free(chunk.memory);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
            }
        }
        usleep(API_LOOP_DELAY_SEC * 1000000);
    }
    return NULL;
}

// Get daily solar values from Solarman
void* get_daily_solar_t(void* pvParameters) {
    (void)pvParameters;

    while (true) {
        time_t last_update;
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            last_update = solar.dailyUpdateTime;
        }

        if (time(NULL) - last_update > SOLAR_DAILY_UPDATE_INTERVAL_SEC) {
            char local_token[SOLAR_TOKEN_LENGTH];
            if (!get_solar_token_copy(local_token, sizeof(local_token))) {
                usleep(SOLAR_TOKEN_WAIT_SEC * 1000000);
                continue;
            }

            char url_buffer[URL_BUFFER_SIZE];
            char post_buffer[POST_BUFFER_SIZE];
            char currentDate[CHAR_LEN];

            // Get current date
            time_t now_time = time(NULL);
            struct tm current_tm;
            localtime_r(&now_time, &current_tm);
            strftime(currentDate, sizeof(currentDate), "%Y-%m-%d", &current_tm);

            snprintf(url_buffer, URL_BUFFER_SIZE,
                     "https://%s/station/v1.0/history?language=en", SOLAR_URL);

            snprintf(post_buffer, POST_BUFFER_SIZE,
                     "{\"stationId\":\"%s\",\"timeType\":2,\"startTime\":\"%s\",\"endTime\":\"%s\"}",
                     SOLAR_STATIONID, currentDate, currentDate);

            struct MemoryStruct chunk;
            CURL* curl = init_curl_request(url_buffer, &chunk);

            if (curl) {
                struct curl_slist* headers = NULL;
                headers = curl_slist_append(headers, "Content-Type: application/json");

                char auth_header[SOLAR_TOKEN_LENGTH + 20];
                snprintf(auth_header, sizeof(auth_header), "Authorization: %s", local_token);
                headers = curl_slist_append(headers, auth_header);

                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_buffer);

                CURLcode res = curl_easy_perform(curl);

                if (res == CURLE_OK) {
                    long response_code;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

                    if (response_code == 200 && chunk.size > 0) {
                        struct json_object* root = json_tokener_parse(chunk.memory);

                        if (root != NULL) {
                            struct json_object* success_obj;
                            if (json_object_object_get_ex(root, "success", &success_obj)) {
                                bool rec_success = json_object_get_boolean(success_obj);

                                if (rec_success) {
                                    struct json_object* station_data_items;
                                    if (json_object_object_get_ex(root, "stationDataItems", &station_data_items)) {
                                        struct json_object* first_item = json_object_array_get_idx(station_data_items, 0);
                                        if (first_item) {
                                            struct json_object* buy_value_obj;
                                            if (json_object_object_get_ex(first_item, "buyValue", &buy_value_obj)) {
                                                float today_buy = json_object_get_double(buy_value_obj);

                                                {
                                                    std::lock_guard<std::mutex> lock(dataMutex);
                                                    solar.today_buy = today_buy;
                                                    solar.dailyUpdateTime = time(NULL);
                                                }

                                                logAndPublish("Solar today's buy value updated");
                                                saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
                                            }
                                        }
                                    }
                                } else {
                                    logAndPublish("Solar today's buy value update failed: No success");
                                }
                            }
                            json_object_put(root);
                        } else {
                            logAndPublish("Solar today's buy value update failed: JSON parse error");
                        }
                    } else {
                        char log_message[CHAR_LEN];
                        snprintf(log_message, CHAR_LEN,
                                 "[HTTP] GET solar today buy value failed, response code: %ld", response_code);
                        errorPublish(log_message);
                        logAndPublish("Getting solar today buy value failed");
                        usleep(API_FAIL_DELAY_SEC * 1000000);
                    }
                } else {
                    char log_message[CHAR_LEN];
                    snprintf(log_message, CHAR_LEN, "[HTTP] GET solar today buy value failed: %s",
                             curl_easy_strerror(res));
                    errorPublish(log_message);
                    logAndPublish("Getting solar today buy value failed");
                    usleep(API_FAIL_DELAY_SEC * 1000000);
                }

                free(chunk.memory);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
            }
        }
        usleep(API_LOOP_DELAY_SEC * 1000000);
    }
    return NULL;
}

// Get monthly solar values from Solarman
void* get_monthly_solar_t(void* pvParameters) {
    (void)pvParameters;

    while (true) {
        time_t last_update;
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            last_update = solar.monthlyUpdateTime;
        }

        if (time(NULL) - last_update > SOLAR_MONTHLY_UPDATE_INTERVAL_SEC) {
            char local_token[SOLAR_TOKEN_LENGTH];
            if (!get_solar_token_copy(local_token, sizeof(local_token))) {
                usleep(SOLAR_TOKEN_WAIT_SEC * 1000000);
                continue;
            }

            char url_buffer[URL_BUFFER_SIZE];
            char post_buffer[POST_BUFFER_SIZE];
            char currentYearMonth[CHAR_LEN];

            // Get current year-month
            time_t now_time = time(NULL);
            struct tm current_tm;
            localtime_r(&now_time, &current_tm);
            strftime(currentYearMonth, sizeof(currentYearMonth), "%Y-%m", &current_tm);

            snprintf(url_buffer, URL_BUFFER_SIZE,
                     "https://%s/station/v1.0/history?language=en", SOLAR_URL);

            snprintf(post_buffer, POST_BUFFER_SIZE,
                     "{\"stationId\":\"%s\",\"timeType\":3,\"startTime\":\"%s\",\"endTime\":\"%s\"}",
                     SOLAR_STATIONID, currentYearMonth, currentYearMonth);

            struct MemoryStruct chunk;
            CURL* curl = init_curl_request(url_buffer, &chunk);

            if (curl) {
                struct curl_slist* headers = NULL;
                headers = curl_slist_append(headers, "Content-Type: application/json");

                char auth_header[SOLAR_TOKEN_LENGTH + 20];
                snprintf(auth_header, sizeof(auth_header), "Authorization: %s", local_token);
                headers = curl_slist_append(headers, auth_header);

                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_buffer);

                CURLcode res = curl_easy_perform(curl);

                if (res == CURLE_OK) {
                    long response_code;
                    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

                    if (response_code == 200 && chunk.size > 0) {
                        struct json_object* root = json_tokener_parse(chunk.memory);

                        if (root != NULL) {
                            struct json_object* success_obj;
                            if (json_object_object_get_ex(root, "success", &success_obj)) {
                                bool rec_success = json_object_get_boolean(success_obj);

                                if (rec_success) {
                                    struct json_object* station_data_items;
                                    if (json_object_object_get_ex(root, "stationDataItems", &station_data_items)) {
                                        struct json_object* first_item = json_object_array_get_idx(station_data_items, 0);
                                        if (first_item) {
                                            struct json_object* buy_value_obj;
                                            if (json_object_object_get_ex(first_item, "buyValue", &buy_value_obj)) {
                                                float month_buy = json_object_get_double(buy_value_obj);

                                                {
                                                    std::lock_guard<std::mutex> lock(dataMutex);
                                                    solar.month_buy = month_buy;
                                                    solar.monthlyUpdateTime = time(NULL);
                                                }

                                                logAndPublish("Solar month's buy value updated");
                                                saveDataBlock(SOLAR_DATA_FILENAME, &solar, sizeof(solar));
                                            }
                                        }
                                    }
                                }
                            }
                            json_object_put(root);
                        } else {
                            logAndPublish("Solar month's buy value update failed: JSON parse error");
                        }
                    } else {
                        char log_message[CHAR_LEN];
                        snprintf(log_message, CHAR_LEN,
                                 "[HTTP] GET solar month buy value failed, response code: %ld", response_code);
                        errorPublish(log_message);
                        logAndPublish("Getting solar month buy value failed");
                        usleep(API_FAIL_DELAY_SEC * 1000000);
                    }
                } else {
                    char log_message[CHAR_LEN];
                    snprintf(log_message, CHAR_LEN, "[HTTP] GET solar month buy value failed: %s",
                             curl_easy_strerror(res));
                    errorPublish(log_message);
                    logAndPublish("Getting solar month buy value failed");
                    usleep(API_FAIL_DELAY_SEC * 1000000);
                }

                free(chunk.memory);
                curl_slist_free_all(headers);
                curl_easy_cleanup(curl);
            }
        }
        usleep(API_LOOP_DELAY_SEC * 1000000);
    }
    return NULL;
}