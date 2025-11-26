#include "globals.h"

extern Solar solar;

uint8_t calculateChecksum(const void* data_ptr, size_t size) {
    uint8_t sum = 0;
    const uint8_t* bytePtr = (const uint8_t*)data_ptr;
    for (size_t i = 0; i < size; ++i) {
        sum ^= bytePtr[i];
    }
    return sum;
}

bool saveDataBlock(const char* filename, const void* data_ptr, size_t size) {
    FILE* dataFile = fopen(filename, "wb");
    if (!dataFile) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Error opening file %s for writing", filename);
        logAndPublish(log_message);
        return false;
    }

    // 1. Prepare header
    DataHeader header;
    header.size = size;
    header.checksum = calculateChecksum(data_ptr, size);

    // 2. Write header
    size_t headerWritten = fwrite(&header, 1, sizeof(DataHeader), dataFile);
    if (headerWritten != sizeof(DataHeader)) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to write header to %s", filename);
        logAndPublish(log_message);
        fclose(dataFile);
        return false;
    }

    // 3. Write data
    size_t bytesWritten = fwrite(data_ptr, 1, size, dataFile);
    fclose(dataFile);

    if (bytesWritten != size) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to write all data to %s. Wrote %zu of %zu bytes", filename, bytesWritten, size);
        logAndPublish(log_message);
        return false;
    }

    return true;
}

bool loadDataBlock(const char* filename, void* data_ptr, size_t expected_size) {
    FILE* dataFile = fopen(filename, "rb");
    printf("Loading data block from %s\n", filename);
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

    // 3. Read the data block
    size_t bytesRead = fread(data_ptr, 1, expected_size, dataFile);
    fclose(dataFile);

    if (bytesRead != expected_size) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Failed to read all data from %s. Read %zu of %zu bytes", filename, bytesRead, expected_size);
        logAndPublish(log_message);
        return false;
    }

    // 4. Verify checksum
    uint8_t calculated = calculateChecksum(data_ptr, expected_size);

    if (header.checksum != calculated) {
        char log_message[CHAR_LEN];
        snprintf(log_message, sizeof(log_message), "Checksum failed for %s! Stored: 0x%02X, Calculated: 0x%02X", filename, header.checksum, calculated);
        logAndPublish(log_message);
        return false;
    }

    return true;
}