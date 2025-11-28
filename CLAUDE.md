# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Klaussometer is a Raspberry Pi-based home environmental monitoring and solar power display system. Originally designed for ESP32, this version has been migrated to Raspberry Pi Zero 2. The application displays room temperatures, humidity levels, battery status from wireless sensors, weather data, UV index, and solar power system information on an LVGL-based GUI running via SDL2.

## Build System

### Building the Project

```bash
make                # Build the project (default target)
make debug          # Build with debug symbols (-g3 -O0)
make release        # Build optimized release (-O3)
```

### Cleaning

```bash
make clean          # Remove ALL build artifacts
make clean-project  # Remove only project files (keep LVGL compiled objects)
make clean-lvgl     # Remove only LVGL compiled objects
```

### Running

```bash
make run            # Build and run the application
./build/klaussometer  # Run the built executable directly
```

### Installing Dependencies

```bash
make install-deps   # Install required system libraries via apt-get
```

Required libraries: SDL2, curl, mosquitto, jsoncpp, ssl, crypto, json-c

## Architecture

### Threading Model

The application uses pthreads for concurrent operations with the following threads:

- **thread_mqtt**: Handles MQTT connection and message processing
- **thread_weather**: Fetches weather data from Weatherbit API
- **thread_uv**: Fetches UV index data
- **thread_solar_token**: Manages authentication token for solar API
- **thread_current_solar**: Fetches current solar power statistics
- **thread_daily_solar**: Fetches daily solar statistics
- **thread_monthly_solar**: Fetches monthly solar statistics
- **thread_display_status**: Displays status messages on screen
- **thread_connectivity_manager**: Manages network connectivity and reconnection

### Data Synchronization

- **dataMutex**: Protects shared data structures (weather, uv, solar, readings) (std::mutex)
- **statusQueueMutex**: Protects the status message queue (std::mutex)
- **tokenMutex** (static in APIs.cpp): Protects solar API authentication token (std::mutex)

All mutex operations use std::lock_guard<std::mutex> for RAII-based locking, which automatically unlocks when going out of scope and provides exception safety. Always acquire locks in consistent order to prevent deadlocks.

### Core Data Structures

Located in `src/globals.h`:

- **Readings**: Array of sensor readings (temperature, humidity, battery) from MQTT topics
- **Weather**: Current weather data from Weatherbit API
- **UV**: UV index information
- **Solar**: Solar power system data (battery charge, power flows, daily/monthly stats)

All structures are `__attribute__((packed))` for binary serialization to disk.

### Persistent Storage

Data is saved to binary files in `~/.local/share/klaussometer/`:

- `solar_data.bin`: Solar system state
- `weather_data.bin`: Weather information
- `uv_data.bin`: UV index data
- `readings_data.bin`: Sensor readings array

Each file has a DataHeader with size and XOR checksum for validation. See `src/saveload.cpp` for implementation.

### MQTT Integration

- Broker: Configured in `src/config.h`
- Subscribes to sensor topics matching pattern in READINGS_ARRAY
- Processes messages in `process_mqtt_message()` (src/mqtt.cpp:10)
- Updates readings array and triggers UI updates

### API Integrations

1. **Weatherbit API**: Weather data including temperature, wind, and conditions
2. **UV Index API**: Current UV index for location
3. **Solarman API**: Solar inverter data (requires token authentication)

API calls use libcurl with JSON parsing via jsoncpp/json-c. All API threads have retry logic with delays on failure (API_FAIL_DELAY_SEC = 30s).

### UI Architecture

The UI is built with LVGL 9.4.0 and designed in SquareLine Studio (project in `SL/` directory).

- **UI files**: Generated code in `src/UI/`
- **Main screen**: `ui_Screen1.c` with room displays, weather, solar power widgets
- **Display**: 1024x600 SDL2 window
- **Custom fonts**: Battery icons, arrows, Montserrat

UI updates are called from main loop via `lv_task_handler()` and `lv_timer_handler()`. Screen updates are in `src/ScreenUpdates.cpp`.

### Room Display System

Displays 5 rooms with temperature, humidity, battery status, and trend indicators:

```cpp
#define ROOM_COUNT 5
```

Arrays of UI object pointers (defined in `src/constants.h`):
- `roomNames[]`: Room name labels
- `tempArcs[]`: Temperature arc indicators
- `tempLabels[]`: Temperature value labels
- `batteryLabels[]`: Battery status icons
- `directionLabels[]`: Trend arrows (up/down/same)
- `humidityLabels[]`: Humidity percentage labels

Readings are mapped from the `readings[]` array (15 entries: 5 rooms × 3 data types).

## Code Organization

### Main Entry Point

`src/main.cpp` contains:
- `setup()`: Initialization (LVGL, SDL, data restoration, thread creation)
- `loop()`: Main event loop (LVGL handlers, screen updates, data persistence)
- `signal_handler()`: Graceful shutdown on SIGINT/SIGTERM

### File Responsibilities

- `src/connections.cpp`: MQTT connection, time initialization, connectivity management
- `src/mqtt.cpp`: MQTT callbacks and message processing
- `src/APIs.cpp`: All API thread functions (weather, UV, solar)
- `src/ScreenUpdates.cpp`: UI update functions (solar display, colors, formatting)
- `src/saveload.cpp`: Binary data persistence with checksums
- `src/config.h`: API keys, MQTT credentials, constants (EDIT WITH CARE)
- `src/constants.h`: System constants, color definitions, sensor topic mappings
- `src/globals.h`: Data structure definitions and function declarations

### Platform Migration Notes

This codebase was migrated from ESP32 to Raspberry Pi. Key differences:

- ESP32 used TFT_eSPI display driver; Pi uses SDL2
- ESP32 used Arduino framework; Pi uses standard C++17/pthreads
- Time handling: ESP32 used NTP via WiFi; Pi uses system time
- File paths: Pi uses XDG Base Directory (~/.local/share/)

## Configuration

Edit `src/config.h` to configure:
- MQTT server credentials
- Weatherbit API key and city ID
- Solar API credentials (Solarman)
- Geographic coordinates (lat/long)
- Battery capacity and electricity pricing

**WARNING**: `src/config.h` contains API keys and passwords. Do not commit changes to this file.

## Testing and Debugging

### Debug Output

The application uses printf for logging. Status messages are also queued for on-screen display and published to MQTT topic for logging.

Key functions:
- `logAndPublish(const char* messageBuffer)`: Log and show on screen (src/main.cpp:423)
- `errorPublish(const char* messageBuffer)`: Log errors

### Common Issues

1. **Display not showing**: Check SDL2 installation and DISPLAY environment variable
2. **MQTT not connecting**: Verify network, broker address, credentials in config.h
3. **API data stale**: Check thread status, network connectivity, API credentials
4. **Segfault on startup**: Usually LVGL initialization or UI object null pointers

### Data Invalidation

Sensor readings older than MAX_NO_MESSAGE_SEC (1800s/30min) are marked with CHAR_NO_MESSAGE ('#'). This is checked in `invalidateOldReadings()` (src/main.cpp:366).

## Important Constants

Located in `src/constants.h`:

- `STORED_READING = 6`: Historical data points per sensor
- `CHAR_LEN = 255`: Maximum string buffer size
- `WEATHER_UPDATE_INTERVAL_SEC = 300`: 5 minutes
- `UV_UPDATE_INTERVAL_SEC = 3600`: 1 hour
- `SOLAR_CURRENT_UPDATE_INTERVAL_SEC = 60`: 1 minute

## Code Style

The codebase uses clang-format with configuration in `.clang-format`. Format code before committing:

```bash
clang-format -i src/*.cpp src/*.h
```

Excluded files are listed in `.clang-format-ignore`.
