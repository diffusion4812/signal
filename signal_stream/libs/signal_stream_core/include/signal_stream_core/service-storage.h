#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <arrow/api.h>

#include "service-source-registry.h"
#include <apache_bridge/apache_bridge.h>

namespace signal_stream {

    // timestamp alias
    using ts_t = std::int64_t;

    // Stream configuration options
    struct StreamStorageOptions {
        size_t capacity_records = 1000;
        size_t flush_batch_size = 1;
        std::chrono::milliseconds flush_interval{ 0 };
    };

    // Result codes for submit
    enum class SubmitResult { Accepted, BackPressure, ConversionError, UnknownSource };

    class ProducerToken;

    class StorageManager {
    public:
        struct SourceStorage {
            std::recursive_mutex mtx;
            std::vector<std::shared_ptr<arrow::RecordBatch>> batches;
            std::shared_ptr<arrow::Schema> arrow_schema;
            avro::ValidSchema avro_batch_schema;
            apache_bridge::AvroToArrowConverter converter;
            avro::DecoderPtr decoder;

            SourceStorage(avro::ValidSchema avro, std::shared_ptr<arrow::Schema> arrow)
            : arrow_schema(std::move(arrow)),
            avro_batch_schema(std::move(avro)),
            converter(avro_batch_schema, arrow_schema),
            decoder(avro::binaryDecoder()) {}
        };

        explicit StorageManager();
        ~StorageManager();

        void create_source(const std::string& name,
                                           avro::ValidSchema avro_batch_schema,
                                           std::shared_ptr<arrow::Schema> arrow_schema);

        void remove_source(const std::string& streamId);

        // Acquire a token for a stream (cheap handle)
        ProducerToken get_producer_token(const std::string& name);

        // Persistence control
        bool flush_stream(const std::string& streamId);

        size_t stream_count() const;
        std::optional<size_t> stream_size(const std::string& servicename) const;

        std::unordered_map<std::string, std::shared_ptr<SourceStorage>> sources_;

    private:
        std::atomic<bool> stopFlag_;
        bool running_;

        mutable std::mutex streamsMtx_;

        friend class ProducerToken;
    };

    class ProducerToken {
    public:
        ProducerToken() = default;
        ProducerToken(std::shared_ptr<StorageManager::SourceStorage> storage)
        : storage_(std::move(storage)) {}

        // Non-blocking submit: attempts to enqueue the batch for async persistence.
        SubmitResult try_submit(std::vector<std::byte>&& batch) const {
            if (!storage_) return SubmitResult::UnknownSource;
            if (batch.empty()) return SubmitResult::Accepted;

            std::lock_guard<std::recursive_mutex> lock(storage_->mtx);

            try {
                auto is = avro::memoryInputStream(
                    reinterpret_cast<const uint8_t*>(batch.data()), batch.size());
                storage_->decoder->init(*is);

                // 1. Decode the single array datum
                avro::GenericDatum arrayDatum(storage_->avro_batch_schema);
                avro::decode(*storage_->decoder, arrayDatum);

                // 2. Extract individual records from the array
                const auto& arr = arrayDatum.value<avro::GenericArray>();
                const auto& elements = arr.value();

                // 3. Convert to Arrow
                auto arrow_batch = storage_->converter.ConvertToRecordBatch(elements);
                if (arrow_batch.ok()) {
                    storage_->batches.push_back(arrow_batch.ValueOrDie());
                    return SubmitResult::Accepted;
                } else {
                    return SubmitResult::ConversionError;
                }
            } catch (const std::exception& e) {
                // Log e.what() for diagnostics
                return SubmitResult::ConversionError;
            }
        }

    private:
        std::shared_ptr<StorageManager::SourceStorage> storage_;
    };

}