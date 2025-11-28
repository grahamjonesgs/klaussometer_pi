#include "globals.h"

extern Solar solar;
extern std::mutex dataMutex;
static char dataDirectory[512] = {0};

uint8_t calculateChecksum(const void* data_ptr, size_t size) {
    uint8_t sum = 0;
    const uint8_t* bytePtr = (const uint8_t*)data_ptr;
    for (size_t i = 0; i < size; ++i) {
        sum ^= bytePtr[i];
    }
    return sum;
}

bool saveDataBlock(const char* filename, const void* data_ptr, size_t size) {
    char filepath[512];
    getDataFilePath(filename, filepath, sizeof(filepath));

    // Allocate temporary buffer for data copy
    void* buffer = malloc(size);
    if (!buffer) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to allocate memory for %s", filename);
        logAndPublish(log_message);
        return false;
    }

    // Copy data under lock (fast operation)
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        memcpy(buffer, data_ptr, size);
    }

    // Prepare header (no lock needed, working with local buffer)
    DataHeader header;
    header.size = size;
    header.checksum = calculateChecksum(buffer, size);

    // Open file and write (no lock needed - all I/O outside critical section)
    FILE* dataFile = fopen(filepath, "wb");
    if (!dataFile) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Error opening file %s for writing", filename);
        logAndPublish(log_message);
        free(buffer);
        return false;
    }

    // Write header
    size_t headerWritten = fwrite(&header, 1, sizeof(DataHeader), dataFile);
    if (headerWritten != sizeof(DataHeader)) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to write header to %s", filename);
        logAndPublish(log_message);
        fclose(dataFile);
        free(buffer);
        return false;
    }

    // Write data from buffer
    size_t bytesWritten = fwrite(buffer, 1, size, dataFile);
    fclose(dataFile);
    free(buffer);

    if (bytesWritten != size) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to write all data to %s. Wrote %zu of %zu bytes", filename, bytesWritten, size);
        logAndPublish(log_message);
        return false;
    }

    return true;
}

bool loadDataBlock(const char* filename, void* data_ptr, size_t expected_size) {
   char filepath[512];
    getDataFilePath(filename, filepath, sizeof(filepath));

    FILE* dataFile = fopen(filepath, "rb");
    printf("Loading data block from %s\n", filepath);
    if (!dataFile) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Error opening file %s for reading", filename);
        logAndPublish(log_message);
        return false;
    }

    // 1. Read the header
    DataHeader header;
    size_t headerRead = fread(&header, 1, sizeof(DataHeader), dataFile);

    if (headerRead != sizeof(DataHeader)) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to read header from %s", filename);
        logAndPublish(log_message);
        fclose(dataFile);
        return false;
    }

    // 2. Verify size matches what we expect
    if (header.size != expected_size) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Data size mismatch in %s. Header: %zu, Expected: %zu", filename, header.size, expected_size);
        logAndPublish(log_message);
        fclose(dataFile);
        return false;
    }

    // 3. Allocate temporary buffer and read data (no lock during I/O)
    void* buffer = malloc(expected_size);
    if (!buffer) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to allocate memory for %s", filename);
        logAndPublish(log_message);
        fclose(dataFile);
        return false;
    }

    size_t bytesRead = fread(buffer, 1, expected_size, dataFile);
    fclose(dataFile);

    if (bytesRead != expected_size) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to read all data from %s. Read %zu of %zu bytes", filename, bytesRead, expected_size);
        logAndPublish(log_message);
        free(buffer);
        return false;
    }

    // 4. Verify checksum (working with local buffer, no lock needed)
    uint8_t calculated = calculateChecksum(buffer, expected_size);

    if (header.checksum != calculated) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Checksum failed for %s! Stored: 0x%02X, Calculated: 0x%02X", filename, header.checksum, calculated);
        logAndPublish(log_message);
        free(buffer);
        return false;
    }

    // 5. Copy validated data to destination under lock (fast operation)
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        memcpy(data_ptr, buffer, expected_size);
    }

    free(buffer);
    return true;
}

bool initDataDirectory() {
    // Get home directory
    const char* home = getenv("HOME");
    if (!home) {
        // Fallback to passwd entry
        struct passwd* pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }
    
    if (!home) {
        logAndPublish("Could not determine home directory");
        return false;
    }
    
    // Build path: ~/.klaussometer/
    snprintf(dataDirectory, sizeof(dataDirectory), "%s/.klaussometer", home);
    
    // Create directory if it doesn't exist
    struct stat st;
    if (stat(dataDirectory, &st) == -1) {
        if (mkdir(dataDirectory, 0755) == -1) {
            char log_message[CHAR_LEN];
            snprintf(log_message, sizeof(log_message), "Failed to create directory %s", dataDirectory);
            logAndPublish(log_message);
            return false;
        }
        logAndPublish("Created data directory");
    }
    
    return true;
}

void getDataFilePath(const char* filename, char* fullpath, size_t fullpath_size) {
    snprintf(fullpath, fullpath_size, "%s/%s", dataDirectory, filename);
}