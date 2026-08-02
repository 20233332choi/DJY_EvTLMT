#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

class VirtualLogRecorder {
public:
    VirtualLogRecorder() = default;
    ~VirtualLogRecorder();
    VirtualLogRecorder(const VirtualLogRecorder&) = delete;
    VirtualLogRecorder& operator=(const VirtualLogRecorder&) = delete;

    void Start();
    void Stop();
    bool IsRecording() const { return recording.load(); }
    uint64_t RecordCount() const { return recordCount.load(); }
    std::wstring CurrentFilePath() const;
    std::string LastError() const;

private:
    void Worker();
    void SetError(const std::string& message);

    std::atomic<bool> running{false};
    std::atomic<bool> recording{false};
    std::atomic<uint64_t> recordCount{0};
    std::thread worker;
    mutable std::mutex stateMutex;
    std::wstring currentFilePath;
    std::string lastError;
};
