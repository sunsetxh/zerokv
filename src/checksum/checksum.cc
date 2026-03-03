#include "zerokv/checksum.h"

namespace zerokv {

// CRC32 lookup table
static uint32_t crc32_table[256] = {0};
static bool crc32_table_init = false;

void init_crc32_table() {
    if (crc32_table_init) return;

    const uint32_t polynomial = 0xEDB88320;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (uint32_t j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ polynomial;
            } else {
                crc = crc >> 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_init = true;
}

uint32_t CRC32::calculate(const void* data, size_t length) {
    init_crc32_table();

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < length; i++) {
        uint8_t index = (crc ^ bytes[i]) & 0xFF;
        crc = (crc >> 8) ^ crc32_table[index];
    }

    return crc ^ 0xFFFFFFFF;
}

uint32_t CRC32::calculate(const std::string& str) {
    return calculate(str.data(), str.size());
}

uint32_t CRC32::update(uint32_t crc, const void* data, size_t length) {
    init_crc32_table();

    const uint8_t* bytes = static_cast<const uint8_t*>(data);

    for (size_t i = 0; i < length; i++) {
        uint8_t index = (crc ^ bytes[i]) & 0xFF;
        crc = (crc >> 8) ^ crc32_table[index];
    }

    return crc;
}

uint32_t IntegrityChecker::checksum(const void* data, size_t size) {
    return CRC32::calculate(data, size);
}

IntegrityChecker::CheckResult IntegrityChecker::verify(const void* data, size_t size, uint32_t expected_crc) {
    CheckResult result;
    result.expected_crc = expected_crc;
    result.actual_crc = CRC32::calculate(data, size);
    result.valid = (result.expected_crc == result.actual_crc);
    return result;
}

} // namespace zerokv
