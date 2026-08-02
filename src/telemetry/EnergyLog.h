#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct EnergyLogSample {
    uint32_t timestampMs = 0;
    float hvVoltage = 0.0f;
    float hvCurrent = 0.0f;
    float lvVoltage = 0.0f;
    float temperature = 0.0f;
    float powerKW = 0.0f;
};

class EnergyLog {
public:
    bool Load(const wchar_t* path);
    void Clear();
    bool IsLoaded() const { return !samples.empty(); }
    bool IsVirtual() const { return virtualSource; }

    std::vector<EnergyLogSample> samples;
    std::wstring filePath;
    std::string error;
    uint32_t uid[3] = {};
    size_t invalidPackets = 0;
    double driveEnergyKWh = 0.0;
    double regenEnergyKWh = 0.0;
    double netEnergyKWh = 0.0;
    float minVoltage = 0.0f;
    float maxVoltage = 0.0f;
    float minCurrent = 0.0f;
    float maxCurrent = 0.0f;
    float averageRateHz = 0.0f;
    double durationSeconds = 0.0;

private:
    bool virtualSource = false;
    void CalculateMetadata();
};
