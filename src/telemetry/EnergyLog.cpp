#include "EnergyLog.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
constexpr uint8_t LOG_MAGIC = 0xAA;
constexpr uint8_t LOG_HEADER = 0;
constexpr uint8_t LOG_RECORD = 1;
constexpr size_t HEADER_SIZE = 32;
constexpr size_t RECORD_SIZE = 16;
constexpr uint32_t SYNTHETIC_UID[3] = {0x45564552, 0x474D5431, 0x46454445};

uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}
int16_t ReadI16(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<int16_t>(ReadU16(bytes, offset));
}
uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset) {
    return static_cast<uint32_t>(ReadU16(bytes, offset)) |
           (static_cast<uint32_t>(ReadU16(bytes, offset + 2)) << 16);
}
bool ValidChecksum(const std::vector<uint8_t>& bytes, size_t offset, size_t size) {
    uint16_t checksum = 0;
    for (size_t i = 0; i < size; i += 2) checksum ^= ReadU16(bytes, offset + i);
    return checksum == 0;
}
}

void EnergyLog::Clear() {
    samples.clear(); filePath.clear(); error.clear(); invalidPackets = 0;
    uid[0] = uid[1] = uid[2] = 0; virtualSource = false;
    driveEnergyKWh = regenEnergyKWh = netEnergyKWh = 0.0;
    minVoltage = maxVoltage = minCurrent = maxCurrent = averageRateHz = 0.0f;
    durationSeconds = 0.0;
}

bool EnergyLog::Load(const wchar_t* path) {
    Clear();
    std::ifstream stream(std::filesystem::path(path), std::ios::binary);
    if (!stream) { error = "Cannot open the selected file."; return false; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    if (bytes.size() < HEADER_SIZE || bytes[0] != LOG_MAGIC || bytes[1] != LOG_HEADER) {
        error = "Not an FSK-EEM log header."; return false;
    }
    if (!ValidChecksum(bytes, 0, HEADER_SIZE)) {
        error = "Header checksum is invalid."; return false;
    }
    for (int i = 0; i < 3; ++i) uid[i] = ReadU32(bytes, 8 + i * 4);
    virtualSource = uid[0] == SYNTHETIC_UID[0] && uid[1] == SYNTHETIC_UID[1] && uid[2] == SYNTHETIC_UID[2];

    for (size_t offset = HEADER_SIZE; offset + RECORD_SIZE <= bytes.size(); offset += RECORD_SIZE) {
        if (bytes[offset] != LOG_MAGIC || bytes[offset + 1] != LOG_RECORD ||
            !ValidChecksum(bytes, offset, RECORD_SIZE)) {
            ++invalidPackets; continue;
        }
        EnergyLogSample sample;
        sample.timestampMs = ReadU32(bytes, offset + 4);
        sample.hvVoltage = ReadI16(bytes, offset + 8) / 10.0f;
        sample.hvCurrent = ReadI16(bytes, offset + 10) / 10.0f;
        sample.lvVoltage = ReadI16(bytes, offset + 12) / 100.0f;
        sample.temperature = ReadI16(bytes, offset + 14) / 100.0f;
        sample.powerKW = sample.hvVoltage * sample.hvCurrent / 1000.0f;
        samples.push_back(sample);
    }
    if (samples.empty()) { error = "No valid measurement records found."; return false; }
    filePath = path;
    CalculateMetadata();
    return true;
}

void EnergyLog::CalculateMetadata() {
    minVoltage = maxVoltage = samples.front().hvVoltage;
    minCurrent = maxCurrent = samples.front().hvCurrent;
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        minVoltage = std::min(minVoltage, sample.hvVoltage);
        maxVoltage = std::max(maxVoltage, sample.hvVoltage);
        minCurrent = std::min(minCurrent, sample.hvCurrent);
        maxCurrent = std::max(maxCurrent, sample.hvCurrent);
        if (i == 0) continue;
        const double dtHours = (samples[i].timestampMs - samples[i - 1].timestampMs) / 3600000.0;
        const double energyKWh = (samples[i - 1].powerKW + samples[i].powerKW) * 0.5 * dtHours;
        if (energyKWh >= 0.0) driveEnergyKWh += energyKWh;
        else regenEnergyKWh += -energyKWh;
    }
    netEnergyKWh = driveEnergyKWh - regenEnergyKWh;
    durationSeconds = (samples.back().timestampMs - samples.front().timestampMs) / 1000.0;
    if (durationSeconds > 0.0 && samples.size() > 1)
        averageRateHz = static_cast<float>((samples.size() - 1) / durationSeconds);
}
