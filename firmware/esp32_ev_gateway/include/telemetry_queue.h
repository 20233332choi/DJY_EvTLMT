#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Caller owns synchronization. Pending entries are never overwritten by newer
// samples or removed on HTTP failure. Full queues reject/count new entries.
template<size_t JsonSize, size_t Capacity>
class TelemetryQueue {
public:
    bool push(uint32_t sequence, const char* json) {
        const size_t length = strlen(json);
        if (count_ == Capacity || length + 1 > JsonSize) { ++dropped_; return false; }
        Entry& entry = entries_[(head_ + count_) % Capacity];
        entry.sequence = sequence;
        entry.length = length;
        memcpy(entry.json, json, length + 1);
        ++count_;
        return true;
    }
    size_t batch(char* output, size_t size, size_t maximum, uint32_t& last) const {
        if (size < 3) return 0;
        size_t used = 1, taken = 0;
        output[0] = '[';
        for (; taken < count_ && taken < maximum; ++taken) {
            const Entry& entry = entries_[(head_ + taken) % Capacity];
            if (used + (taken ? 1 : 0) + entry.length + 2 > size) break;
            if (taken) output[used++] = ',';
            memcpy(output + used, entry.json, entry.length);
            used += entry.length;
            last = entry.sequence;
        }
        output[used++] = ']';
        output[used] = '\0';
        return taken;
    }
    void acknowledge(uint32_t last) {
        while (count_ && entries_[head_].sequence <= last) {
            head_ = (head_ + 1) % Capacity;
            --count_;
        }
    }
    size_t count() const { return count_; }
    uint32_t dropped() const { return dropped_; }
private:
    struct Entry { uint32_t sequence; size_t length; char json[JsonSize]; };
    Entry entries_[Capacity] = {};
    size_t head_ = 0, count_ = 0;
    uint32_t dropped_ = 0;
};
