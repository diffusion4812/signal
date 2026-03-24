#pragma once

#include <variant>
#include "storage-buffer.h"

namespace signal_stream {

    // Abstract persistence backend
    struct IStorageBackend {
        virtual ~IStorageBackend() = default;
        virtual bool write(const std::string& streamId, const StreamBuffer::BatchChunks& chunks) = 0;
        virtual void flush() = 0;
        virtual void close() = 0;

        // Optional: backends can provide metrics
        virtual size_t get_total_records() const { return 0; }
        virtual size_t get_total_bytes() const { return 0; }
    };

}