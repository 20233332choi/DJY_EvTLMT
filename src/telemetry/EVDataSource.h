#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include "DataSource.h"
#include "EVTelemetry.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

class EVDataSource final : public IDataSource {
public:
    explicit EVDataSource(int port = 9004);
    ~EVDataSource() override;

    void Update(float deltaTime) override;
    bool IsConnected() const override;
    const EVTelemetry* GetEVTelemetry() const override { return &telemetry_; }
    const char* GetSourceName() const override { return "EV STM32 / ESP32"; }

private:
    void ListenLoop();
    void ParsePacket(const char* payload, int length);

    int port_ = 9004;
    SOCKET socket_ = INVALID_SOCKET;
    bool wsaStarted_ = false;
    std::thread listener_;
    std::atomic_bool running_ = false;
    std::atomic<std::uint64_t> lastPacketTick_ = 0;

    mutable std::mutex mutex_;
    EVTelemetry pendingTelemetry_ = {};
    EVTelemetry telemetry_ = {};
    std::deque<std::uint64_t> arrivals_;
    std::uint32_t lastSequence_ = 0;
};
