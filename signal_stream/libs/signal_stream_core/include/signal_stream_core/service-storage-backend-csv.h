#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>

#include "service-storage-backend-file-mixin.h"
#include "schema.h"

namespace signal_stream {

    class CSVBackend : public FileRotatingBackend {
    public:
        struct Config {
            FileRotationConfig rotation;
            char delimiter = ',';
            bool include_header = true;
            int precision = 6;  // Floating point precision
        };

        explicit CSVBackend(const Schema& schema, Config config = {})
            : FileRotatingBackend(config.rotation)
            , schema_(schema)
            , delimiter_(config.delimiter)
            , include_header_(config.include_header)
            , precision_(config.precision) {
        }

        ~CSVBackend() noexcept {
            try {
                close();
            }
            catch (...) {
                // Suppress exceptions in destructor
            }
        }

    protected:
        bool write_to_current_file(const StreamBuffer::BatchChunks& chunks) override {
            if (!file_.is_open()) {
                return false;
            }

            // Helper to get value from chunk at specific index
            auto get_value = [](const ISignalBuffer::ContiguousChunk& chunk, size_t idx) -> std::string {
                switch (chunk.kind) {
                case Kind::Int32: {
                    const auto* data = reinterpret_cast<const int32_t*>(chunk.data);
                    return std::to_string(data[idx]);
                }
                case Kind::Int64: {
                    const auto* data = reinterpret_cast<const int64_t*>(chunk.data);
                    return std::to_string(data[idx]);
                }
                case Kind::Float: {
                    const auto* data = reinterpret_cast<const float*>(chunk.data);
                    return std::to_string(data[idx]);
                }
                case Kind::Double: {
                    const auto* data = reinterpret_cast<const double*>(chunk.data);
                    return std::to_string(data[idx]);
                }
                default:
                    return "?";
                }
                };

            // Write first chunk rows
            for (size_t row = 0; row < chunks.first_chunk.begin()->second.count; ++row) {
                bool first_field = true;

                for (const auto& field : schema_.fields()) {
                    if (!first_field) file_ << delimiter_;
                    first_field = false;

                    const auto& chunk = chunks.first_chunk.at(field.idx);
                    if (chunk.is_valid) {
                        file_ << get_value(chunk, row);
                    }
                }
                file_ << '\n';
            }

            // Write wrapped chunk rows if present
            if (chunks.has_wrap()) {
                for (size_t row = 0; row < chunks.second_chunk.begin()->second.count; ++row) {
                    bool first_field = true;

                    for (const auto& field : schema_.fields()) {
                        if (!first_field) file_ << delimiter_;
                        first_field = false;

                        const auto& chunk = chunks.second_chunk.at(field.idx);
                        if (chunk.is_valid) {
                            file_ << get_value(chunk, row);
                        }
                    }
                    file_ << '\n';
                }
            }

            return true;
        }

        void open_new_file(const std::filesystem::path& filepath) override {

            // Close existing file if open
            if (file_.is_open()) {
                file_.close();
            }

            // Open new file
            file_.open(filepath, std::ios::out | std::ios::trunc);

            if (!file_.is_open()) {
                throw std::runtime_error("Failed to open CSV file: " + filepath.string());
            }

            // Set floating point precision
            file_ << std::fixed << std::setprecision(precision_);

            // Write header row
            if (include_header_) {
                bool first = true;
                for (const auto& field : schema_.fields()) {
                    if (!first) file_ << delimiter_;
                    first = false;
                    file_ << field.name;
                }
                file_ << '\n';
            }
        }

        void close_file() override {
            if (file_.is_open()) {
                file_.flush();
                file_.close();
            }
        }

        void flush_file() override {
            if (file_.is_open()) {
                file_.flush();
            }
        }

        bool is_file_open() const override {
            return file_.is_open();
        }

        size_t get_current_file_size() const override {
            if (!file_.is_open()) {
                return 0;
            }

            // Get current position as estimate of file size
            auto& file = const_cast<std::ofstream&>(file_);
            auto pos = file.tellp();
            return pos >= 0 ? static_cast<size_t>(pos) : 0;
        }

        std::string get_file_extension() const override {
            return "csv";
        }

    private:
        const Schema& schema_;
        char delimiter_;
        bool include_header_;
        int precision_;

        std::ofstream file_;
    };

}