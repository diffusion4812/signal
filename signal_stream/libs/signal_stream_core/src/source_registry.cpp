#include <signal_stream_core/service-source-registry.h>

namespace signal_stream {

    SourceRegistry::SourceRegistry(ServiceBus& bus)
        : bus_(bus) {
    }

    SourceRegistry::~SourceRegistry() = default;

    std::shared_ptr<arrow::DataType> map_arrow_type(const std::string& type_str) {
        static const std::unordered_map<std::string,
            std::shared_ptr<arrow::DataType>> type_map = {
            {"int8",    arrow::int8()},
            {"int16",   arrow::int16()},
            {"int32",   arrow::int32()},
            {"int64",   arrow::int64()},
            {"uint8",   arrow::uint8()},
            {"uint16",  arrow::uint16()},
            {"uint32",  arrow::uint32()},
            {"uint64",  arrow::uint64()},
            {"float",   arrow::float32()},
            {"float32", arrow::float32()},
            {"double",  arrow::float64()},
            {"float64", arrow::float64()},
            {"string",  arrow::utf8()},
            {"utf8",    arrow::utf8()},
            {"bool",    arrow::boolean()},
            {"boolean", arrow::boolean()},
            {"binary",  arrow::binary()},
        };

        if (auto it = type_map.find(type_str); it != type_map.end())
            return it->second;

        // Fallback — preserves the original type string in field metadata
        return arrow::utf8();
    }

    std::shared_ptr<arrow::Field> make_timestamp_field()
    {
        return arrow::field(
            "_timestamp",
            arrow::timestamp(arrow::TimeUnit::NANO, "UTC"),
            /*nullable=*/false,
            arrow::KeyValueMetadata::Make(
                {"role",  "source"},
                {"index", "system"}
            )
        );
    }

    std::shared_ptr<arrow::Field> make_signal_field(const SignalData& sig)
    {
        std::vector<std::string> keys = {"unit", "source_type"};
        std::vector<std::string> vals = {sig.unit, sig.type};

        for (const auto& [k, v] : sig.metadata) {
            keys.push_back(k);
            vals.push_back(v);
        }

        return arrow::field(
            sig.name,
            map_arrow_type(sig.type),
            /*nullable=*/false,
            arrow::KeyValueMetadata::Make(keys, vals)
        );
    }

    std::shared_ptr<arrow::Schema> build_source_schema(const SourceData& src)
    {
        // Schema-level metadata
        std::vector<std::string> keys = {"source.name", "source.type"};
        std::vector<std::string> vals = {src.name,       src.type};

        for (const auto& [k, v] : src.metadata) {
            keys.push_back("meta." + k);
            vals.push_back(v);
        }

        // Fields
        std::vector<std::shared_ptr<arrow::Field>> fields;
        fields.reserve(1 + src.signals.size());
        fields.push_back(make_timestamp_field());

        for (const auto& sig : src.signals)
            fields.push_back(make_signal_field(sig));

        return arrow::schema(std::move(fields),
                            arrow::KeyValueMetadata::Make(keys, vals));
    }

    bool SourceRegistry::register_source(const std::string& name, const SourceData& data) {
        if (name.empty()) {
            return false;
        }

        SourceData to_insert = data;
        to_insert.arrow = build_source_schema(data);
        to_insert.avro_batch = apache_bridge::ArrowSchemaToAvroBatchSchema(to_insert.arrow);

        {
            std::scoped_lock lock(mtx_);

            // Check if already exists
            if (sources_.contains(name)) {
                return false;  // Already registered
            }

            sources_.insert({name, to_insert});
        }

        // Publish event (outside lock)
        bus_.Publish<Event>(Event{
            .type = Event::Type::Registered,
            .source_name = name
            });

        return true;
    }

    bool SourceRegistry::unregister_source(const std::string& name) {
        {
            std::scoped_lock lock(mtx_);

            // Try to erase
            if (sources_.erase(name) == 0) {
                return false;  // Not found
            }
        }

        // Publish event
        bus_.Publish<Event>(Event{
            .type = Event::Type::Unregistered,
            .source_name = name
            });

        return true;
    }

    bool SourceRegistry::is_registered(const std::string& name) const {
        std::scoped_lock lock(mtx_);
        return sources_.contains(name);
    }

    const SourceData& SourceRegistry::get_source_data(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mtx_);

        auto it = sources_.find(name);
        
        if (it == sources_.end()) {
            throw std::invalid_argument("Source does not exist: " + name);
        }
        return it->second;
    }

    avro::ValidSchema SourceRegistry::get_avro_batch_schema(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mtx_);

        auto it = sources_.find(name);
        
        if (it == sources_.end()) {
            throw std::invalid_argument("Source does not exist: " + name);
        }
        return it->second.avro_batch;
    }

    std::shared_ptr<arrow::Schema> SourceRegistry::get_arrow_schema(const std::string& name) const {
        std::lock_guard<std::mutex> lock(mtx_);

        auto it = sources_.find(name);
        
        if (it == sources_.end()) {
            throw std::invalid_argument("Source does not exist: " + name);
        }

        return it->second.arrow;
    }

} // namespace signal_stream