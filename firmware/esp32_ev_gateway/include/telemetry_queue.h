#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Caller owns synchronization. Pending entries are never overwritten by newer
// samples or removed on HTTP failure. Full queues reject/count new entries.
template<size_t JsonSize, size_t Capacity, size_t ByteCapacity = JsonSize * Capacity>
class TelemetryQueue {
public:
    bool push(uint32_t sequence, const char* json) {
        const size_t length = strlen(json);
        if (count_ == Capacity || length + 1 > JsonSize || length > ByteCapacity - bytes_) {
            ++dropped_; return false;
        }
        Entry& entry = entries_[(head_ + count_) % Capacity];
        entry.sequence = sequence;
        entry.length = length;
        entry.offset = tail_;
        const size_t first = length < ByteCapacity - tail_ ? length : ByteCapacity - tail_;
        memcpy(data_ + tail_, json, first);
        memcpy(data_, json + first, length - first);
        tail_ = (tail_ + length) % ByteCapacity;
        bytes_ += length;
        ++count_;
        return true;
    }
    size_t batch(char* output, size_t size, size_t maximum, uint32_t& last) const {
        if (size < 3) return 0;
        size_t length = 0;
        const size_t taken = batchInfo(size - 1, maximum, last, length);
        readBatch(output, 0, length, taken);
        output[length] = '\0';
        return taken;
    }
    size_t batchInfo(size_t budget, size_t maximum, uint32_t& last, size_t& length) const {
        length = 2; // []
        size_t taken = 0;
        for (; taken < count_ && taken < maximum; ++taken) {
            const Entry& entry = entries_[(head_ + taken) % Capacity];
            if (length + (taken ? 1 : 0) + entry.length > budget) break;
            length += (taken ? 1 : 0) + entry.length;
            last = entry.sequence;
        }
        return taken;
    }
    // Freeze taken with batchInfo() under the producer lock. The single
    // consumer must not ACK until reading finishes. Producer pushes cannot
    // overwrite these entries, so network reads need no long critical section.
    size_t readBatch(char* output, size_t offset, size_t limit, size_t taken) const {
        size_t used = 0;
        auto part = [&](const char* p, size_t n) {
            if (offset >= n) { offset -= n; return; }
            p += offset; n -= offset; offset = 0;
            if (n > limit - used) n = limit - used;
            memcpy(output + used, p, n); used += n;
        };
        part("[", 1);
        for (size_t i = 0; i < taken; ++i) {
            if (i) part(",", 1);
            const Entry& entry = entries_[(head_ + i) % Capacity];
            const size_t first = entry.length < ByteCapacity - entry.offset ? entry.length : ByteCapacity - entry.offset;
            part(data_ + entry.offset, first);
            part(data_, entry.length - first);
        }
        part("]", 1);
        return used;
    }
    void acknowledge(uint32_t last) {
        while (count_ && entries_[head_].sequence <= last) {
            bytes_ -= entries_[head_].length;
            head_ = (head_ + 1) % Capacity;
            --count_;
        }
    }
    size_t count() const { return count_; }
    uint32_t dropped() const { return dropped_; }
    void reject() { ++dropped_; }
    size_t bytes() const { return bytes_; }
private:
    struct Entry { uint32_t sequence; size_t length, offset; };
    Entry entries_[Capacity] = {};
    char data_[ByteCapacity] = {};
    size_t tail_ = 0, bytes_ = 0;
    size_t head_ = 0, count_ = 0;
    uint32_t dropped_ = 0;
};
