#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <fstream>
#include <optional>
#include <stdexcept>

#include <signal_stream_core/service-bus.h>
#include <signal_stream_core/service-logger.h>
#include <signal_stream_core/service-source-registry.h>
#include <signal_stream_core/service-storage.h>
#include <signal_stream_core/source_factory.h>
#include <signal_stream_core/source.h>
#include <signal_stream_core/project.h>
#include <signal_stream_core/hash.h>

namespace signal_stream {

    class Orchestrator {
    public:
        explicit Orchestrator(ServiceBus& bus);

        ~Orchestrator();

        // Project lifecycle
        bool load_project();
        bool load_project(const ProjectData& pdata);

        // Registry access
        const SourceRegistry& get_registry() const;
        bool is_source_registered(const std::string& name) const;
        size_t get_source_count() const;

        // Source access
        std::shared_ptr<Source> get_source(const std::string& name) const;
        std::unordered_map<std::string, std::shared_ptr<Source>> get_all_sources() const;
        std::vector<std::string> get_all_source_names() const;
        const SourceData& get_data_for_source(const std::string& name) const;
        std::shared_ptr<arrow::Schema> get_schema_for_source(const std::string& name) const;

        // Source management
        bool add_source(const SourceData& desc);
        bool remove_source(const std::string& name);
        bool rename_source(const std::string& oldName, const std::string& newName);

        // Source lifecycle
        bool start_source(const std::string& name);
        bool stop_source(const std::string& name);
        bool start_all_sources();
        void stop_all_sources();

        // Source status
        bool is_one_source_running() const;

        class SourceHandle {
        private:
            std::shared_ptr<StorageManager::SourceStorage> source_internal; // Points to the map entry

        public:
            SourceHandle() : source_internal(nullptr) {}
            SourceHandle(std::shared_ptr<StorageManager::SourceStorage> ptr) : source_internal(ptr) {}

            // The UI calls this to get the most recent data
            std::shared_ptr<arrow::Table> snapshot() {
                if (!source_internal) {
                    throw std::runtime_error("SourceHandle: Attempted to snapshot an uninitialized or null source.");
                }

                std::lock_guard<std::recursive_mutex> lock(source_internal->mtx);

                // 1. Ensure the schema exists
                if (!source_internal->arrow_schema) {
                    throw std::runtime_error("SourceHandle: Schema is null. Cannot build Table.");
                }

                // 2. Attempt to create the logical table
                // Passing the explicit schema prevents the "At least one record batch" error
                auto result = arrow::Table::FromRecordBatches(
                    source_internal->arrow_schema, 
                    source_internal->batches
                );

                // 3. If Arrow returns an error (e.g. Schema Mismatch), throw it
                if (!result.ok()) {
                    throw std::runtime_error("Arrow Table Creation Failed: " + result.status().ToString());
                }

                return result.MoveValueUnsafe();
            }
        };

        // Storage access
        SourceHandle get_source_handle(const std::string& name);

        // Project metadata
        ProjectData get_project_data() const;

    private:
        // Internal helpers
        bool start_all_services_locked();
        void stop_all_services_locked();

        // Source management helpers
        bool add_source_locked(const SourceData& source);

        // Members
        ServiceBus& bus_;

        std::string path_;
        uint32_t hash_;

        mutable std::shared_mutex mtx_;
        SourceRegistry registry_;
        StorageManager storage_;
        std::unordered_map<std::string, std::shared_ptr<Source>> sources_;
        ProjectData data_;
    };

} // namespace signal_stream