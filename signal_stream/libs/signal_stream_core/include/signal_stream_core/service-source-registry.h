#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <optional>
#include <arrow/api.h>

#include <avro/Generic.hh>

#include <signal_stream_core/service-bus.h>
#include <signal_stream_core/project.h>
#include <apache_bridge/apache_bridge.h>

namespace signal_stream {

    // Simplified registry - names only
    class SourceRegistry {
    public:
        // Lightweight events - no metadata needed
        struct Event {
            enum class Type {
                Registered,
                Unregistered,
                Renamed
            };

            Type type;
            std::string source_name;
        };

        explicit SourceRegistry(ServiceBus& bus);
        ~SourceRegistry();

        // Core operations
        bool register_source(const std::string& name, const SourceData& data);
        bool unregister_source(const std::string& name);

        // Query operations
        bool is_registered(const std::string& name) const;

        const SourceData& get_source_data(const std::string& name) const;
        avro::ValidSchema get_avro_batch_schema(const std::string& name) const;
        std::shared_ptr<arrow::Schema> get_arrow_schema(const std::string& name) const;

    private:
        ServiceBus& bus_;
        mutable std::mutex mtx_;
        std::unordered_map<std::string, SourceData> sources_;
    };

}