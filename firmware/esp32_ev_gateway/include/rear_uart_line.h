#pragma once

#include <stddef.h>

// A complete extended Rear record can exceed 160 bytes. Never parse the
// tail of an oversized/corrupted record as a new packet.
class RearUartLine {
public:
    enum Result { Pending, Ready, Dropped };
    Result push(char value) {
        if (value == '\n') {
            const bool invalid = dropping_;
            buffer_[length_] = '\0';
            length_ = 0;
            dropping_ = false;
            return invalid ? Dropped : Ready;
        }
        if (value == '\r' || dropping_) return Pending;
        if (value == '\0' || length_ + 1 >= sizeof(buffer_)) {
            dropping_ = true;
            return Pending;
        }
        buffer_[length_++] = value;
        return Pending;
    }
    const char* data() const { return buffer_; }
private:
    char buffer_[768] = {};
    size_t length_ = 0;
    bool dropping_ = false;
};
