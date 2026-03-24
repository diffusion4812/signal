#include "adapter_signalforge/signalforge_adapter.h"
#include "dbg_pubsub.h"
#include "dbg_util.h"

#include <avro/Schema.hh>
#include <avro/ValidSchema.hh>
#include <avro/Generic.hh>
#include <avro/Encoder.hh>
#include <avro/Stream.hh>
#include <avro/Specific.hh>
#include <avro/Compiler.hh>

#include <random>
#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <mutex>

namespace signal_stream {

    struct SignalForgeAdapter::Impl {
        SignalForgeAdapter* owner;
        avro::ValidSchema schema;
        avro::NodePtr record_node;

        dbg_subscriber_t  *sub;
        dbg_sub_layout_t layout;

        uint64_t*         field_ids;
        dbg_value_type_t* field_types;

        std::unordered_map<uint16_t, size_t> field_id_to_record_index;

        double sensor_voltage;

        void handle_frame(uint16_t sub_id, uint32_t sequence,
                        uint64_t timestamp_us, const uint8_t* payload,
                        uint16_t payload_size)
        {
            avro::GenericRecord record(record_node);

            // Field 0: _timestamp
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

            // Iterate debug frame fields — guaranteed same order as record fields 1..N
            dbg_frame_iter_t it;
            dbg_frame_iter_init(&it, payload, &layout);
            dbg_value_t val;

            size_t dbg_idx = 0; // index into the received debug values
            size_t rec_idx = 1; // skip _timestamp at index 0
            while (dbg_frame_iter_next(&it, &val) == DBG_OK) {
                auto& field = record.fieldAt(rec_idx);

                auto set_value = [&](auto& datum) {
                    switch (field_types[dbg_idx]) {
                    case DBG_VT_BOOL: datum.template value<bool>()    = val.b;     break;
                    case DBG_VT_U8:   datum.template value<int32_t>() = val.u8;    break;
                    case DBG_VT_I8:   datum.template value<int32_t>() = val.i8;    break;
                    case DBG_VT_U16:  datum.template value<int32_t>() = val.u16;   break;
                    case DBG_VT_I16:  datum.template value<int32_t>() = val.i16;   break;
                    case DBG_VT_U32:  datum.template value<int64_t>() = val.u32;   break;
                    case DBG_VT_I32:  datum.template value<int32_t>() = val.i32;   break;
                    case DBG_VT_U64:  datum.template value<int64_t>() = static_cast<int64_t>(val.u64); break;
                    case DBG_VT_I64:  datum.template value<int64_t>() = val.i64;   break;
                    case DBG_VT_F32:  datum.template value<float>()   = val.f32;   break;
                    case DBG_VT_F64:  datum.template value<double>()  = val.f64;   break;
                    default: break;
                    }
                };

                if (field.isUnion()) {
                    auto& au = field.value<avro::GenericUnion>();
                    au.selectBranch(1);
                    set_value(au.datum());
                } else {
                    set_value(field);
                }

                ++dbg_idx;
                ++rec_idx;
            }

            // Encode and submit
            auto out = avro::memoryOutputStream();
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

        /* ── Static trampoline — C-compatible function pointer ── */
        static void on_frame_trampoline(uint16_t sub_id, uint32_t sequence,
                                        uint64_t timestamp_us, const uint8_t *payload,
                                        uint16_t payload_size, void *ctx)
        {
            auto *self = static_cast<Impl *>(ctx);
            self->handle_frame(sub_id, sequence, timestamp_us, payload, payload_size);
        }
    };

    // ──────────────────────────────────────────
    // Public methods
    // ──────────────────────────────────────────
    std::shared_ptr<SignalForgeAdapter> SignalForgeAdapter::Create(const SourceContext& ctx, const avro::ValidSchema& schema) {
        return std::make_shared<SignalForgeAdapter>(ctx, schema);
    }

    SignalForgeAdapter::SignalForgeAdapter(const SourceContext& ctx, const avro::ValidSchema& schema)
        : Source(ctx, schema)
        , impl_(std::make_unique<Impl>())
    {
        impl_->owner = this;
        impl_->schema = schema;
        impl_->record_node = impl_->schema.root()->leafAt(0);

        impl_->field_ids = new uint64_t[impl_->record_node->leaves()];
        impl_->field_types = new dbg_value_type_t[impl_->record_node->leaves()];

        for (size_t i = 0; i < impl_->record_node->leaves(); ++i) {
            // Name comes from the record, not the leaf
            std::string field_name = impl_->record_node->nameAt(i);
            
            auto fid = get_signal_metadata_value(field_name, "field_id");
            if (!fid)
                continue;

            uint16_t field_id = static_cast<uint16_t>(std::stoi(*fid));
            impl_->field_id_to_record_index[field_id] = i;

            impl_->field_ids  [i - 1] = field_id;
            impl_->field_types[i - 1] = dbg_value_type_from_str(get_signal_metadata_value(field_name, "field_type").value().c_str());
        }
    }

    SignalForgeAdapter::~SignalForgeAdapter() = default;

    bool SignalForgeAdapter::do_on_start() {
        dbg_sub_config_t cfg = DBG_SUB_CONFIG_DEFAULT;
        std::string host        = get_metadata_value("host").value_or("127.0.0.1");
        std::string config_host = get_metadata_value("config_host").value_or("127.0.0.1");
        
        cfg.host             = host.c_str();
        cfg.data_port        = std::stoi(get_metadata_value("data_port").value_or("9500"));
        cfg.config_port      = std::stoi(get_metadata_value("config_port").value_or("9501"));
        uint32_t interval_us = std::stoi(get_metadata_value("interval").value_or("1000"));
        uint16_t sub_id      = std::stoi(get_metadata_value("sub_id").value_or("0"));

        impl_->sub = dbg_sub_create(&cfg);

        dbg_status_t rc = dbg_sub_subscribe(
            impl_->sub,
            0,
            impl_->field_ids,
            impl_->field_types,
            impl_->record_node->leaves() - 1,
            interval_us,
            &impl_->layout);

        return true;
    }

    bool SignalForgeAdapter::do_on_stop() {
        return true;
    }

    void SignalForgeAdapter::run_once() {
        dbg_sub_poll(impl_->sub, &Impl::on_frame_trampoline, impl_.get());
        
        // throttle
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

} // namespace signal_stream