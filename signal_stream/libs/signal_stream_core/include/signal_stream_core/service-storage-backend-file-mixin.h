#pragma once
#include "service-storage-backend.h"
#include <chrono>
#include <filesystem>
#include <functional>
#include <sstream>
#include <iomanip>

namespace signal_stream {

    struct FileRotationConfig {
        // Rotation triggers (any can trigger rotation)
        size_t max_records_per_file = 100'000;
        size_t max_file_size_bytes = 100 * 1024 * 1024;  // 100 MB
        std::chrono::milliseconds max_file_duration{ 5'000 };

        // File organization
        std::filesystem::path output_directory = "./data";
        std::string filename_pattern = "{stream}_{timestamp}_{sequence}";  // No extension

        // Callbacks for integration with cataloging/indexing systems
        std::function<void(const std::filesystem::path&)> on_file_created;
        std::function<void(const std::filesystem::path&)> on_file_closed;
    };

    class FileRotatingBackend : public IStorageBackend {
    public:
        explicit FileRotatingBackend(FileRotationConfig config)
            : config_(std::move(config)) {
            std::filesystem::create_directories(config_.output_directory);
        }

        bool write(const std::string& streamId,
            const StreamBuffer::BatchChunks& chunks) override {
            std::lock_guard<std::mutex> lock(write_mtx_);

            // Check rotation before write
            if (should_rotate(chunks.total_count)) {
                rotate_file();
            }

            // Delegate to format-specific write
            bool success = write_to_current_file(chunks);

            if (success) {
                // Update metrics
                metrics_.current_file_records += chunks.total_count;
                metrics_.total_records += chunks.total_count;
            }

            return success;
        }

        void flush() override {
            std::lock_guard<std::mutex> lock(write_mtx_);
            if (is_file_open()) {
                flush_file();
            }
        }

        void close() override {
            std::lock_guard<std::mutex> lock(write_mtx_);
            if (is_file_open()) {
                close_file();
                if (config_.on_file_closed) {
                    config_.on_file_closed(metrics_.current_filepath);
                }
            }
        }

        // Metrics
        size_t get_total_records() const {
            return metrics_.total_records;
        }

        std::filesystem::path get_current_file() const {
            return metrics_.current_filepath;
        }

        size_t get_file_count() const {
            return metrics_.file_count;
        }

    protected:
        // ========================================================================
        // FORMAT-SPECIFIC METHODS - Subclasses must implement these
        // ========================================================================

        // Write batch to current file for given stream
        virtual bool write_to_current_file(const StreamBuffer::BatchChunks& chunks) = 0;

        // Open new file for stream
        virtual void open_new_file(const std::filesystem::path& filepath) = 0;

        // Close file for specific stream
        virtual void close_file() = 0;

        // Flush buffered data for specific stream
        virtual void flush_file() = 0;

        // Check if file is open for stream
        virtual bool is_file_open() const = 0;

        // Get current file size for stream
        virtual size_t get_current_file_size() const = 0;

        // File extension (e.g., "parquet", "csv", "json")
        virtual std::string get_file_extension() const = 0;

        // Access to config
        const FileRotationConfig& config() const { return config_; }

    private:
        struct StreamMetrics {
            std::filesystem::path current_filepath;
            size_t current_file_records = 0;
            size_t total_records = 0;
            size_t file_count = 0;
            std::chrono::steady_clock::time_point current_file_opened;
        };

        bool should_rotate(size_t incoming_records) const {
            // First write
            if (!is_file_open()) {
                return true;
            }

            // Record count threshold
            if (metrics_.current_file_records + incoming_records >= config_.max_records_per_file) {
                return true;
            }

            // Time threshold
            auto age = std::chrono::steady_clock::now() - metrics_.current_file_opened;
            if (age >= config_.max_file_duration) {
                return true;
            }

            // File size threshold
            if (get_current_file_size() >= config_.max_file_size_bytes) {
                return true;
            }

            return false;
        }

        void rotate_file() {
            // Close current file if open
            if (is_file_open()) {
                close_file();
                if (config_.on_file_closed) {
                    config_.on_file_closed(metrics_.current_filepath);
                }
            }

            // Generate new filepath
            metrics_.current_filepath = generate_filepath(metrics_.file_count + 1);
            metrics_.current_file_records = 0;
            metrics_.file_count++;
            metrics_.current_file_opened = std::chrono::steady_clock::now();

            // Open new file (format-specific)
            open_new_file(metrics_.current_filepath);

            if (config_.on_file_created) {
                config_.on_file_created(metrics_.current_filepath);
            }
        }

        std::filesystem::path generate_filepath(size_t sequence) const {
            // Timestamp
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            std::tm tm;
#ifdef _WIN32
            localtime_s(&tm, &time_t);
#else
            localtime_r(&time_t, &tm);
#endif

            std::ostringstream timestamp;
            timestamp << std::put_time(&tm, "%Y%m%d_%H%M%S");

            // Build filename from pattern
            std::string filename = config_.filename_pattern;
            replace_token(filename, "{stream}", stream_name_);
            replace_token(filename, "{timestamp}", timestamp.str());
            replace_token(filename, "{sequence}", std::to_string(sequence));

            // Add extension
            filename += "." + get_file_extension();

            return config_.output_directory / filename;
        }

        static void replace_token(std::string& str,
            const std::string& token,
            const std::string& value) {
            size_t pos = 0;
            while ((pos = str.find(token, pos)) != std::string::npos) {
                str.replace(pos, token.length(), value);
                pos += value.length();
            }
        }

        std::string stream_name_;
        FileRotationConfig config_;
        mutable std::mutex write_mtx_;
        StreamMetrics metrics_;
    };

}