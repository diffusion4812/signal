#pragma once

#include <string>
#include <vector>
#include <avro/Generic.hh>
#include <arrow/api.h>

namespace signal_stream {

    struct SignalData {
        std::string name;
        std::string type;
        std::string unit;
        std::unordered_map<std::string, std::string> metadata;
    };

    struct SourceData {
        std::string name;
        std::string type;
        std::unordered_map<std::string, std::string> metadata;
        std::vector<SignalData> signals;

        // Populated after deserialization
        avro::ValidSchema avro_batch;
        std::shared_ptr<arrow::Schema> arrow;
    };

    struct ProjectData {
        std::string name;
        std::vector<SourceData> sources;
    };

} // namespace signal_stream
