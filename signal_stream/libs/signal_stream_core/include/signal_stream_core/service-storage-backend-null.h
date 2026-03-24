#pragma once

#include "service-storage-backend.h"

namespace signal_stream {

    class IStorageBackend;

    // Null backend (no-op)
    struct NullBackend : public IStorageBackend {
        bool write(const std::string& streamId, const StreamBuffer::BatchChunks& chunks) {
            return true;
        }

        void flush() {}
        void close() {}
    };

}