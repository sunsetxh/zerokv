// CRC32 Checksum utilities
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace zerokv {

// CRC32 implementation
class CRC32 {
public:
    static uint32_t calculate(const void* data, size_t length);
    static uint32_t calculate(const std::string& str);

    // Update CRC with more data
    static uint32_t update(uint32_t crc, const void* data, size_t length);
};

// Data integrity checker
class IntegrityChecker {
public:
    struct CheckResult {
        bool valid;
        uint32_t expected_crc;
        uint32_t actual_crc;
    };

    // Calculate CRC for data
    static uint32_t checksum(const void* data, size_t size);

    // Verify data with expected CRC
    static CheckResult verify(const void* data, size_t size, uint32_t expected_crc);

    // Verify string
    static CheckResult verify(const std::string& str, uint32_t expected_crc) {
        return verify(str.data(), str.size(), expected_crc);
    }
};

// Data with checksum wrapper
class ChecksummedData {
public:
    ChecksummedData() = default;

    ChecksummedData(const void* data, size_t size) {
        set_data(data, size);
    }

    void set_data(const void* data, size_t size) {
        data_.assign(static_cast<const char*>(data),
                     static_cast<const char*>(data) + size);
        checksum_ = CRC32::calculate(data_.data(), data_.size());
    }

    const std::vector<char>& data() const { return data_; }
    uint32_t checksum() const { return checksum_; }

    bool verify() const {
        auto result = IntegrityChecker::verify(data_.data(), data_.size(), checksum_);
        return result.valid;
    }

private:
    std::vector<char> data_;
    uint32_t checksum_ = 0;
};

} // namespace zerokv
