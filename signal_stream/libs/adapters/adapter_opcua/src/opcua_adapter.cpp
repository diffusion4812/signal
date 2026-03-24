#include "adapter_opcua/opcua_adapter.h"

// ── open62541pp ──
#include <open62541pp/client.hpp>
#include <open62541pp/monitoreditem.hpp>
#include <open62541pp/subscription.hpp>
#include <open62541pp/types.hpp>

// ── Avro ──
#include <avro/Schema.hh>
#include <avro/ValidSchema.hh>
#include <avro/Generic.hh>
#include <avro/Encoder.hh>
#include <avro/Stream.hh>
#include <avro/Specific.hh>
#include <avro/Compiler.hh>

#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <mutex>
#include <unordered_map>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace signal_stream {

// ─────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────

static opcua::NodeId node_id_from_string(const std::string& s)
{
    uint16_t ns = 0;
    auto ns_pos = s.find("ns=");
    if (ns_pos != std::string::npos) {
        ns = static_cast<uint16_t>(std::stoi(s.substr(ns_pos + 3)));
    }

    auto i_pos = s.find(";i=");
    if (i_pos != std::string::npos) {
        uint32_t numeric = static_cast<uint32_t>(std::stoul(s.substr(i_pos + 3)));
        return opcua::NodeId(ns, numeric);
    }

    auto s_pos = s.find(";s=");
    if (s_pos != std::string::npos) {
        return opcua::NodeId(ns, s.substr(s_pos + 3));
    }

    throw std::invalid_argument("Cannot parse OPC UA NodeId from: " + s);
}

/// Read a scalar value from a UA_Variant via its raw data pointer,
/// safely checking the type index against the UA_TYPES table.
static void set_avro_field_from_variant(avro::GenericDatum& field,
                                        const opcua::Variant& variant)
{
    // Access the underlying UA_Variant for type inspection and data read
    const UA_Variant* raw = variant.handle();
    if (raw == nullptr || raw->data == nullptr || !UA_Variant_isScalar(raw))
        return;

    auto set_value = [&](avro::GenericDatum& datum) {
        const UA_DataType* type = raw->type;

        if (type == &UA_TYPES[UA_TYPES_BOOLEAN]) {
            datum.value<bool>() = *static_cast<const UA_Boolean*>(raw->data);
        } else if (type == &UA_TYPES[UA_TYPES_BYTE]) {
            datum.value<int32_t>() = *static_cast<const UA_Byte*>(raw->data);
        } else if (type == &UA_TYPES[UA_TYPES_SBYTE]) {
            datum.value<int32_t>() = *static_cast<const UA_SByte*>(raw->data);
        } else if (type == &UA_TYPES[UA_TYPES_UINT16]) {
            datum.value<int32_t>() = *static_cast<const UA_UInt16*>(raw->data);
        } else if (type == &UA_TYPES[UA_TYPES_INT16]) {
            datum.value<int32_t>() = *static_cast<const UA_Int16*>(raw->data);
        } else if (type == &UA_TYPES[UA_TYPES_UINT32]) {
            datum.value<int64_t>() = *static_cast<const UA_UInt32*>(raw->data);
        } else if (type == &UA_TYPES[UA_TYPES_INT32]) {
            datum.value<int32_t>() = *static_cast<const UA_Int32*>(raw->data);
        } else if (type == &UA_TYPES[UA_TYPES_UINT64]) {
            datum.value<int64_t>() =
                static_cast<int64_t>(*static_cast<const UA_UInt64*>(raw->data));
        } else if (type == &UA_TYPES[UA_TYPES_INT64]) {
            datum.value<int64_t>() = *static_cast<const UA_Int64*>(raw->data);
        } else if (type == &UA_TYPES[UA_TYPES_FLOAT]) {
            datum.value<float>() = *static_cast<const UA_Float*>(raw->data);
        } else if (type == &UA_TYPES[UA_TYPES_DOUBLE]) {
            datum.value<double>() = *static_cast<const UA_Double*>(raw->data);
        } else if (type == &UA_TYPES[UA_TYPES_STRING]) {
            const UA_String* ua_str = static_cast<const UA_String*>(raw->data);
            datum.value<std::string>() =
                std::string(reinterpret_cast<const char*>(ua_str->data), ua_str->length);
        }
    };

    if (field.isUnion()) {
        auto& au = field.value<avro::GenericUnion>();
        au.selectBranch(1);
        set_value(au.datum());
    } else {
        set_value(field);
    }
}

// ─────────────────────────────────────────────────────────
//  Implementation (PIMPL)
// ─────────────────────────────────────────────────────────

struct OPCUASource::Impl {
    OPCUASource* owner = nullptr;

    avro::ValidSchema schema;
    avro::NodePtr     record_node;

    std::unique_ptr<opcua::Client> client;

    std::unordered_map<uint32_t, size_t> mon_id_to_field_index;
    std::vector<opcua::NodeId>           field_node_ids;

    std::mutex                                 values_mutex;
    std::unordered_map<size_t, opcua::Variant> latest_values;
    bool                                       has_new_data = false;

    // Maintain lifetime of the subscription and items
    std::optional<opcua::Subscription<opcua::Client>> subscription;
    std::vector<opcua::MonitoredItem<opcua::Client>> monitored_items;

    double publishing_interval_ms = 100.0;
    double sampling_interval_ms   = 50.0;

    void flush_record()
    {
        std::lock_guard<std::mutex> lock(values_mutex);
        if (!has_new_data)
            return;

        avro::GenericRecord record(record_node);

        // Field 0: _timestamp (nanoseconds since epoch)
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch());

        auto& ts_field = record.fieldAt(0);
        if (ts_field.isUnion()) {
            auto& au = ts_field.value<avro::GenericUnion>();
            au.selectBranch(1);
            au.datum().value<int64_t>() = static_cast<int64_t>(now_ns.count());
        } else {
            ts_field.value<int64_t>() = static_cast<int64_t>(now_ns.count());
        }

        // Fields 1..N
        for (auto& [field_index, variant] : latest_values) {
            auto& field = record.fieldAt(field_index);
            set_avro_field_from_variant(field, variant);
        }

        has_new_data = false;

        // Avro binary encode
        auto out     = avro::memoryOutputStream();
        auto encoder = avro::binaryEncoder();
        encoder->init(*out);
        avro::encode(*encoder, avro::GenericDatum(record_node, record));
        encoder->flush();

        size_t len = out->byteCount();
        std::vector<uint8_t> buf(len);
        auto in = avro::memoryInputStream(*out);
        avro::StreamReader reader(*in);
        reader.readBytes(buf.data(), len);

        owner->append_to_buffer(buf.data(), buf.size());
    }

    void process_subscription()
    {
        // Now safe to call .emplace() because we are calling this 
        // from our own thread/loop, not a library callback.
        subscription.emplace(*client);

        opcua::SubscriptionParameters subParams{};
        subParams.publishingInterval = publishing_interval_ms;
        subscription->setSubscriptionParameters(subParams);
        subscription->setPublishingMode(true);

        monitored_items.clear();

        for (size_t i = 0; i < field_node_ids.size(); ++i) {
            const size_t fi = i + 1;
            const opcua::NodeId& node_id = field_node_ids[i];

            Impl* self;
            auto mon = subscription->subscribeDataChange(
                node_id,
                opcua::AttributeId::Value,
                [self, fi](opcua::IntegerId, opcua::IntegerId, const opcua::DataValue& dv) {
                    if (dv.hasValue()) {
                        std::lock_guard<std::mutex> lock(self->values_mutex);
                        self->latest_values[fi] = dv.value();
                        self->has_new_data = true;
                    }
                }
            );

            opcua::MonitoringParametersEx monParams{};
            monParams.samplingInterval = sampling_interval_ms;
            monParams.queueSize = 1;
            mon.setMonitoringParameters(monParams);
            mon.setMonitoringMode(opcua::MonitoringMode::Reporting);

            monitored_items.push_back(std::move(mon));
        }
    }

};

// ─────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────

std::shared_ptr<OPCUASource> OPCUASource::Create(const SourceContext& ctx,
                                                   const avro::ValidSchema& schema)
{
    return std::make_shared<OPCUASource>(ctx, schema);
}

OPCUASource::OPCUASource(const SourceContext& ctx, const avro::ValidSchema& schema)
    : Source(ctx, schema)
    , impl_(std::make_unique<Impl>())
{
    impl_->owner       = this;
    impl_->schema      = schema;
    impl_->record_node = impl_->schema.root()->leafAt(0);

    const size_t num_leaves = impl_->record_node->leaves();
    impl_->field_node_ids.reserve(num_leaves);

    for (size_t i = 1; i < num_leaves; ++i) {
        std::string field_name = impl_->record_node->nameAt(i);

        auto node_id_str = get_signal_metadata_value(field_name, "node_id");
        if (!node_id_str) {
            throw std::runtime_error(
                "OPC UA adapter: missing 'node_id' metadata for field '" +
                field_name + "'");
        }

        impl_->field_node_ids.push_back(node_id_from_string(*node_id_str));
    }
}

OPCUASource::~OPCUASource() = default;

bool OPCUASource::do_on_start()
{
    std::string endpoint_url = get_metadata_value("endpoint_url")
                                   .value_or("opc.tcp://127.0.0.1:4840");

    impl_->publishing_interval_ms = std::stod(get_metadata_value("publishing_interval_ms").value_or("100"));
    impl_->sampling_interval_ms   = std::stod(get_metadata_value("sampling_interval_ms").value_or("50"));

    impl_->client = std::make_unique<opcua::Client>();

    // Use the state callback pattern from your example
    impl_->client->onSessionActivated([this] {
        // 1. Create Subscription
        // Note: Assigning to the member variable impl_->subscription keeps it alive
        impl_->subscription.emplace(*impl_->client);

        opcua::SubscriptionParameters subParams{};
        subParams.publishingInterval = impl_->publishing_interval_ms;
        
        impl_->subscription->setSubscriptionParameters(subParams);
        impl_->subscription->setPublishingMode(true);

        // 2. Clear old monitored items if this is a reconnect
        impl_->monitored_items.clear();

        // 3. Create monitored items for each field
        for (size_t i = 0; i < impl_->field_node_ids.size(); ++i) {
            const size_t record_field_index = i + 1;
            const opcua::NodeId& node_id    = impl_->field_node_ids[i];

            Impl* self = impl_.get();
            const size_t fi = record_field_index;

            // Use the signature from your example
            auto mon = impl_->subscription->subscribeDataChange(
                node_id,
                opcua::AttributeId::Value,
                [self, fi](opcua::IntegerId /*subId*/, opcua::IntegerId /*monId*/, const opcua::DataValue& dv) {
                    if (dv.hasValue()) {
                        std::lock_guard<std::mutex> lock(self->values_mutex);
                        self->latest_values[fi] = dv.value();
                        self->has_new_data      = true;
                    }
                }
            );

            // Set parameters as shown in example
            opcua::MonitoringParametersEx monParams{};
            monParams.samplingInterval = impl_->sampling_interval_ms;
            monParams.queueSize = 1;
            mon.setMonitoringParameters(monParams);
            mon.setMonitoringMode(opcua::MonitoringMode::Reporting);

            // Store to keep alive
            impl_->monitored_items.push_back(std::move(mon));
        }
    });

    // Start the connection
    try {
        impl_->client->connect(endpoint_url);
    } catch (const opcua::BadStatus& e) {
        // Log error: e.what()
        return false;
    }

    return true;
}

bool OPCUASource::do_on_stop()
{
    if (impl_->client) {
        impl_->client->disconnect();
        impl_->client.reset();
    }
    return true;
}

void OPCUASource::run_once()
{
    impl_->client->runIterate(/* timeoutMs = */ 10);
    impl_->flush_record();
}

} // namespace signal_stream