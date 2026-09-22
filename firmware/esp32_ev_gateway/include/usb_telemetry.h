#pragma once
#include "telemetry_queue.h"
#include <stdio.h>

// Same columnar samples as HTTPS, over the existing USB-UART connector.
// Only the main loop owns this queue. Never wait for UART space or a PC ACK.
class UsbTelemetry {
public:
    void start() {
        enabled_ = true;
        schemaSent_ = false;
        // A reopened PC port has forgotten its schema. Retain all unACKed rows.
        if (waiting_) { waiting_ = false; taken_ = 0; next_ = 0; }
    }
    void push(uint32_t sequence, const char* row) {
        if (enabled_) samples_.push(sequence, row);
    }
    void reject() { if (enabled_) samples_.reject(); }
    uint32_t dropped() const { return samples_.dropped(); }
    size_t queued() const { return samples_.count(); }
    void acknowledge(uint32_t sequence) {
        if (!waiting_ || sequence != last_) return;
        samples_.acknowledge(sequence);
        if (frameHasColumns_) schemaSent_ = true;
        waiting_ = false;
        taken_ = 0;
    }

    template<class Writer>
    void poll(Writer& output, uint32_t now, const char* columns, const char* streamId, const char* vehicleId) {
        if (!enabled_) return;
        if (waiting_) {
            if (now - sentAt_ < 1000u) return;
            waiting_ = false;
            taken_ = 0; // Retry retained rows; a lost ACK never removes them.
        }
        if (!taken_) {
            if (!samples_.count() || !columns[0] ||
                (samples_.count() < 32 && static_cast<int32_t>(now - next_) < 0)) return;
            const int n = snprintf(prefix_, sizeof(prefix_),
                "{\"stream_id\":\"%s\",\"vehicle_id\":\"%s\",\"sent_ms\":%lu,\"dropped_samples\":%lu,"
                "\"queue_samples\":%u,\"queue_bytes\":%u,%s%s%s\"samples\":",
                streamId, vehicleId, static_cast<unsigned long>(now),
                static_cast<unsigned long>(samples_.dropped()),
                static_cast<unsigned>(samples_.count()), static_cast<unsigned>(samples_.bytes()),
                schemaSent_ ? "" : "\"columns\":", schemaSent_ ? "" : columns, schemaSent_ ? "" : ",");
            if (n <= 0 || static_cast<size_t>(n) >= sizeof(prefix_)) return;
            prefixLength_ = static_cast<size_t>(n);
            frameHasColumns_ = !schemaSent_;
            taken_ = samples_.batchInfo(60 * 1024 - prefixLength_, 32, last_, arrayLength_);
            if (!taken_) return;
            position_ = 0;
            crc_ = 0xffffu;
            next_ = now + 50u;
        }

        const int available = output.availableForWrite();
        if (available <= 0) return;
        char chunk[256];
        const size_t budget = static_cast<size_t>(available) < sizeof(chunk) ?
                              static_cast<size_t>(available) : sizeof(chunk);
        const size_t jsonLength = prefixLength_ + arrayLength_ + 1;
        size_t length = 0;
        if (position_ < prefixLength_) {
            length = prefixLength_ - position_;
            if (length > budget) length = budget;
            memcpy(chunk, prefix_ + position_, length);
        } else if (position_ < prefixLength_ + arrayLength_) {
            length = samples_.readBatch(chunk, position_ - prefixLength_, budget, taken_);
        } else if (position_ == jsonLength - 1) {
            chunk[0] = '}'; length = 1;
        } else {
            char suffix[7];
            snprintf(suffix, sizeof(suffix), "\t%04x\n", static_cast<unsigned>(crc_));
            const size_t offset = position_ - jsonLength;
            length = 6 - offset;
            if (length > budget) length = budget;
            memcpy(chunk, suffix + offset, length);
        }
        const size_t written = output.write(reinterpret_cast<const uint8_t*>(chunk), length);
        if (position_ < jsonLength) {
            for (size_t i = 0; i < written; ++i) {
                crc_ ^= static_cast<uint16_t>(static_cast<uint8_t>(chunk[i])) << 8;
                for (unsigned bit = 0; bit < 8; ++bit)
                    crc_ = static_cast<uint16_t>((crc_ & 0x8000u) ? (crc_ << 1) ^ 0x1021u : crc_ << 1);
            }
        }
        position_ += written;
        if (position_ == jsonLength + 6) {
            waiting_ = true;
            sentAt_ = now;
        }
    }
private:
    TelemetryQueue<1536, 96, 48 * 1024> samples_;
    char prefix_[4096] = {};
    bool enabled_ = false, waiting_ = false, schemaSent_ = false, frameHasColumns_ = false;
    uint32_t next_ = 0, last_ = 0, sentAt_ = 0;
    size_t taken_ = 0, prefixLength_ = 0, arrayLength_ = 0, position_ = 0;
    uint16_t crc_ = 0xffffu;
};
