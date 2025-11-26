# Klaussometer Raspberry Pi Makefile
# Migration from ESP32 to Raspberry Pi Zero 2

# Compiler and flags
CXX := g++
CC := gcc
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra
CFLAGS := -std=c11 -O2 -Wall

# Raspberry Pi specific flags
CXXFLAGS += -DRASPBERRY_PI
CFLAGS += -DRASPBERRY_PI

# Source directory (all project files are here)
SRC_DIR := src

# Include directories
INCLUDES := -I$(SRC_DIR) \
            -I$(SRC_DIR)/UI \
            -I$(SRC_DIR)/lvgl

# Libraries to link
LDFLAGS := -lSDL2 \
           -lpthread \
           -lm \
           -lcurl \
           -lmosquitto \
           -ljsoncpp \
           -lssl \
           -lcrypto

# Build directory
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj

# LVGL sources (find all .c files in src/lvgl/src)
LVGL_SRC := $(shell find $(SRC_DIR)/lvgl/src -name "*.c")
LVGL_OBJ := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(LVGL_SRC))

# Project C++ sources (exclude lvgl directory)
PROJECT_CPP_SRC := $(shell find $(SRC_DIR) -maxdepth 1 -name "*.cpp")
PROJECT_CPP_OBJ := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(PROJECT_CPP_SRC))

# Project C sources (exclude lvgl directory)
PROJECT_C_SRC := $(shell find $(SRC_DIR) -maxdepth 1 -name "*.c")
PROJECT_C_OBJ := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(PROJECT_C_SRC))

# UI sources
UI_CPP_SRC := $(wildcard $(SRC_DIR)/UI/*.cpp)
UI_C_SRC := $(wildcard $(SRC_DIR)/UI/*.c)
UI_CPP_OBJ := $(patsubst $(SRC_DIR)/UI/%.cpp,$(OBJ_DIR)/UI/%.o,$(UI_CPP_SRC))
UI_C_OBJ := $(patsubst $(SRC_DIR)/UI/%.c,$(OBJ_DIR)/UI/%.o,$(UI_C_SRC))

# All objects
ALL_OBJECTS := $(PROJECT_CPP_OBJ) $(PROJECT_C_OBJ) $(UI_CPP_OBJ) $(UI_C_OBJ) $(LVGL_OBJ)

# Output binary
TARGET := $(BUILD_DIR)/klaussometer

# Default target
.PHONY: all
all: directories $(TARGET)

# Create build directories
.PHONY: directories
directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(OBJ_DIR)
	@mkdir -p $(OBJ_DIR)/UI
	@mkdir -p $(dir $(LVGL_OBJ))

# Link the final executable
$(TARGET): $(ALL_OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: $(TARGET)"

# Compile project C++ files from src/
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "Compiling $<..."
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Compile project C files from src/
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "Compiling $<..."
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile UI C++ files
$(OBJ_DIR)/UI/%.o: $(SRC_DIR)/UI/%.cpp
	@echo "Compiling $<..."
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# Compile UI C files
$(OBJ_DIR)/UI/%.o: $(SRC_DIR)/UI/%.c
	@echo "Compiling $<..."
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean build artifacts
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(BUILD_DIR)

# Install dependencies (Debian/Raspbian)
.PHONY: install-deps
install-deps:
	sudo apt-get update
	sudo apt-get install -y \
		build-essential \
		libcurl4-openssl-dev \
		libmosquitto-dev \
		libjsoncpp-dev \
		libssl-dev \
		libsdl2-dev \
		json-c 

# Run the application
.PHONY: run
run: $(TARGET)
	./$(TARGET)

# Debug build
.PHONY: debug
debug: CXXFLAGS += -DDEBUG -g3 -O0
debug: CFLAGS += -DDEBUG -g3 -O0
debug: clean all

# Release build
.PHONY: release
release: CXXFLAGS += -DNDEBUG -O3
release: CFLAGS += -DNDEBUG -O3
release: clean all

# Show help
.PHONY: help
help:
	@echo "Klaussometer Raspberry Pi Build System"
	@echo ""
	@echo "Targets:"
	@echo "  all          - Build the project (default)"
	@echo "  clean        - Remove build artifacts"
	@echo "  install-deps - Install required dependencies"
	@echo "  run          - Build and run the application"
	@echo "  debug        - Build with debug symbols"
	@echo "  release      - Build optimized release version"
	@echo "  help         - Show this help message"

# Print variables for debugging the Makefile
.PHONY: print-vars
print-vars:
	@echo "PROJECT_CPP_SRC: $(PROJECT_CPP_SRC)"
	@echo "PROJECT_C_SRC: $(PROJECT_C_SRC)"
	@echo "UI_CPP_SRC: $(UI_CPP_SRC)"
	@echo "UI_C_SRC: $(UI_C_SRC)"
	@echo "LVGL_SRC count: $(words $(LVGL_SRC))"
	@echo "ALL_OBJECTS count: $(words $(ALL_OBJECTS))"
