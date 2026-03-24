#pragma once

#include <filesystem>
#include <arrow/api.h>
#include <parquet/arrow/writer.h>
#include <arrow/io/file.h>

#include "service-storage-backend-file-mixin.h"
#include "schema.h"
#include "instance.h"

namespace signal_stream {

#define timestamp_field_name "timestamp"

    struct ParquetBackend : public FileRotatingBackend {
    public:
        struct Config {
            FileRotationConfig rotation;
        };

        explicit ParquetBackend(const Schema& schema, Config config)
            : FileRotatingBackend(config.rotation)
            , schema_(schema) {
            initialise_builders(schema_);
            arrowSchema_ = create_arrow_schema();
        }

        ~ParquetBackend() noexcept {
            try {
                // Close file writer (handles Parquet metadata finalization)
                if (filewriter_) {
                    auto status = filewriter_->Close();
                    if (!status.ok()) {
                        std::cerr << "[ParquetBackend] Failed to close file writer: "
                            << status.ToString() << std::endl;
                    }
                    filewriter_.reset();  // Explicitly release
                }

                // Close output file stream
                if (outfile_) {
                    auto status = outfile_->Close();
                    if (!status.ok()) {
                        std::cerr << "[ParquetBackend] Failed to close output file: "
                            << status.ToString() << std::endl;
                    }
                    outfile_.reset();  // Explicitly release
                }
            }
            catch (const std::exception& e) {
                // Catch any unexpected exceptions from Arrow/Parquet internals
                std::cerr << "[ParquetBackend] Exception in destructor: "
                    << e.what() << std::endl;
            }
            catch (...) {
                std::cerr << "[ParquetBackend] Unknown exception in destructor" << std::endl;
            }
        }

    protected:
        bool write_to_current_file(const StreamBuffer::BatchChunks& chunks) override {
            std::lock_guard<std::mutex> lock(mtx_);

            if (chunks.total_count == 0) return true;

            // Create Arrow array from contiguous chunk (created by SignalBuffer)
            auto create_arrow_array = [](const ISignalBuffer::ContiguousChunk& chunk)
                -> std::shared_ptr<arrow::Array> {

                auto buffer = arrow::Buffer::Wrap(reinterpret_cast<const std::uint8_t*>(chunk.data), chunk.size_bytes());

                switch (chunk.kind) {
                case Kind::Int32:
                    return std::make_shared<arrow::Int32Array>(
                        chunk.count,          // length
                        buffer,               // data buffer
                        nullptr,              // null bitmap (none)
                        0                     // null count
                    );
                case Kind::Int64:
                    return std::make_shared<arrow::Int64Array>(chunk.count, buffer, nullptr, 0);
                case Kind::Float:
                    return std::make_shared<arrow::FloatArray>(chunk.count, buffer, nullptr, 0);
                case Kind::Double:
                    return std::make_shared<arrow::DoubleArray>(chunk.count, buffer, nullptr, 0);
                default:
                    throw std::runtime_error("Unsupported type");
                }
                };

            // PASS 1: Write first contiguous chunk
            {
                std::vector<std::shared_ptr<arrow::Array>> arrays;
                arrays.reserve(chunks.first_chunk.size()); // Reserve space for all columns (fields in the schema)

                // Data columns (maintain schema order)
                for (const auto& field : schema_.fields()) {
                    auto first_chunk = chunks.first_chunk.at(field.idx); // Locate fields through index
                    if (!first_chunk.is_valid) {
                        throw std::runtime_error("Missing field in first chunk: " + field.name);
                    }
                    arrays.push_back(create_arrow_array(first_chunk));
                }

                auto batch1 = arrow::RecordBatch::Make(
                    arrowSchema_,
                    chunks.first_chunk.begin()->second.count,
                    arrays
                );

                PARQUET_THROW_NOT_OK(filewriter_->WriteRecordBatch(*batch1));
            }

            // PASS 2: Write wrapped chunk if present
            if (chunks.has_wrap()) {
                std::vector<std::shared_ptr<arrow::Array>> arrays;
                arrays.reserve(chunks.second_chunk.size());

                // Data columns (maintain schema order)
                for (const auto& field : schema_.fields()) {
                    auto second_chunk = chunks.second_chunk.at(field.idx); // Locate fields through index
                    if (!second_chunk.is_valid) {
                        throw std::runtime_error("Missing field in first chunk: " + field.name);
                    }
                    arrays.push_back(create_arrow_array(second_chunk));
                }

                auto batch2 = arrow::RecordBatch::Make(
                    arrowSchema_,
                    chunks.second_chunk.begin()->second.count,
                    arrays
                );

                PARQUET_THROW_NOT_OK(filewriter_->WriteRecordBatch(*batch2));
            }

            return true;
        }

        void open_new_file(const std::filesystem::path& filepath) override {
            PARQUET_ASSIGN_OR_THROW(outfile_, arrow::io::FileOutputStream::Open(filepath.string()))
                PARQUET_ASSIGN_OR_THROW(filewriter_, parquet::arrow::FileWriter::Open(*arrowSchema_, arrow::default_memory_pool(), outfile_));
        }

        void close_file() override {
            if (filewriter_) {
                PARQUET_THROW_NOT_OK(filewriter_->Close());
                filewriter_.reset();
            }
            if (outfile_) {
                PARQUET_THROW_NOT_OK(outfile_->Close());
                outfile_.reset();
            }
        }

        void flush_file() override {
            filewriter_->NewBufferedRowGroup();
        }

        bool is_file_open() const override {
            return outfile_ != nullptr && filewriter_ != nullptr;
        }

        size_t get_current_file_size() const override {
            if (outfile_) {
                auto result = outfile_->Tell();
                if (result.ok()) {
                    return static_cast<size_t>(*result);
                }
                else {
                    throw std::runtime_error("Failed to get current file size: " + result.status().ToString());
                }
            }
            return 0;
        }

        constexpr std::string get_file_extension() const override {
            return "parquet";
        }

    private:
        struct SignalBuilder {
            std::shared_ptr<arrow::ArrayBuilder> builder;
            std::shared_ptr<arrow::Field> field; // includes name, type, and metadata
        };

        void initialise_builders(const Schema& schema) {
            for (const auto& field : schema.fields()) {
                if (field.name == timestamp_field_name) throw std::runtime_error("Field name reserved: " + std::string(timestamp_field_name));
                auto builder = create_builder(field.kind);
                auto arrowfield = arrow::field(field.name, to_arrow_type(field.kind), false);
                builders_.push_back(SignalBuilder{ builder, arrowfield });
            }
        }

        std::shared_ptr<arrow::Schema> create_arrow_schema() const {
            std::vector<std::shared_ptr<arrow::Field>> fields;
            for (const auto& builder : builders_) {
                fields.push_back(builder.field);
            }
            return arrow::schema(fields);
        }

        std::shared_ptr<arrow::ArrayBuilder> create_builder(const Kind kind) {
            if (kind == Kind::Int32) return std::make_shared<arrow::Int32Builder>();
            if (kind == Kind::Int64) return std::make_shared<arrow::Int64Builder>();
            if (kind == Kind::Float) return std::make_shared<arrow::FloatBuilder>();
            if (kind == Kind::Double) return std::make_shared<arrow::DoubleBuilder>();
            throw std::runtime_error("Unsupported type: " + std::string(kindToString(kind)));
        }

        std::shared_ptr<arrow::DataType> to_arrow_type(const Kind kind) {
            if (kind == Kind::Int32) return arrow::int32();
            if (kind == Kind::Int64) return arrow::int64();
            if (kind == Kind::Float) return arrow::float32();
            if (kind == Kind::Double) return arrow::float64();
            throw std::runtime_error("Unsupported type: " + std::string(kindToString(kind)));
        }

        const Schema& schema_;
        std::shared_ptr<arrow::Schema> arrowSchema_;
        std::mutex mtx_;
        std::vector<SignalBuilder> builders_;
        std::shared_ptr<arrow::io::FileOutputStream> outfile_;
        std::shared_ptr<parquet::arrow::FileWriter> filewriter_;
    };

} // namespace signal_stream