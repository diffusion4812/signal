#include "adapter_random/random_adapter.h"
#include "perlin.h"

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

    struct RandomAdapter::Impl {
        std::mt19937_64 gen{std::random_device{}()};
        std::vector<detail::Perlin1D> p1d;
        std::chrono::nanoseconds start_ns{0};

        // Avro internals — completely hidden
        avro::ValidSchema schema;
        avro::NodePtr record_node;

        void initPerlinGenerators() {
            avro::GenericRecord record(record_node);
            for (size_t i = 0; i < record.fieldCount(); ++i) {
                p1d.emplace_back(gen());
            }
        }

        void populateRandomDatum(avro::GenericDatum& datum,
                                 size_t idx,
                                 std::chrono::nanoseconds now_ns) {
            auto delta = now_ns - start_ns;
            double seconds = std::chrono::duration<double>(delta).count();
            double frequency = 0.05;
            double value = p1d[idx].fractalNoise(seconds * frequency) * 2.0;
            double raw_noise = p1d[idx].noise(seconds);

            switch (datum.type()) {
                case avro::AVRO_DOUBLE:
                    datum.value<double>() = value;
                    break;
                case avro::AVRO_FLOAT:
                    datum.value<float>() = static_cast<float>(value);
                    break;
                case avro::AVRO_INT:
                    datum.value<int32_t>() = static_cast<int32_t>(raw_noise);
                    break;
                case avro::AVRO_LONG:
                    datum.value<int64_t>() = static_cast<int64_t>(raw_noise);
                    break;
                case avro::AVRO_STRING:
                    datum.value<std::string>() =
                        "val_" + std::to_string(raw_noise);
                    break;
                case avro::AVRO_RECORD:
                    populateRandomRecord(
                        datum.value<avro::GenericRecord>(), now_ns);
                    break;
                default:
                    break;
            }
        }

        void populateRandomRecord(avro::GenericRecord& record,
                                  std::chrono::nanoseconds now_ns) {
            for (size_t i = 0; i < record.fieldCount(); ++i) {
                auto& field = record.fieldAt(i);

                // Timestamp field
                if (i == 0) {
                    if (field.isUnion()) {
                        auto& au = field.value<avro::GenericUnion>();
                        au.selectBranch(1);
                        au.datum().value<int64_t>() =
                            static_cast<int64_t>(now_ns.count());
                    } else {
                        field.value<int64_t>() =
                            static_cast<int64_t>(now_ns.count());
                    }
                    continue;
                }

                if (field.isUnion()) {
                    auto& au = field.value<avro::GenericUnion>();
                    au.selectBranch(1);
                    populateRandomDatum(au.datum(), i, now_ns);
                } else {
                    populateRandomDatum(field, i, now_ns);
                }
            }
        }
    };

    // ──────────────────────────────────────────
    // Public methods
    // ──────────────────────────────────────────
    std::shared_ptr<RandomAdapter> RandomAdapter::Create(const SourceContext& ctx, const avro::ValidSchema& schema) {
        return std::make_shared<RandomAdapter>(ctx, schema);
    }

    RandomAdapter::RandomAdapter(const SourceContext& ctx, const avro::ValidSchema& schema)
        : Source(ctx, schema)
        , impl_(std::make_unique<Impl>())
    {
        impl_->schema = schema;
        impl_->record_node = impl_->schema.root()->leafAt(0);
    }

    RandomAdapter::~RandomAdapter() = default;

    bool RandomAdapter::do_on_start() {
        impl_->start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch());

        impl_->initPerlinGenerators();
        return true;
    }

    bool RandomAdapter::do_on_stop() {
        return true;
    }

    void RandomAdapter::run_once() {
        avro::GenericRecord record(impl_->record_node);

        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch());

        impl_->populateRandomRecord(record, now_ns);

        // Encode and submit via base class
        auto out = avro::memoryOutputStream();
        auto encoder = avro::binaryEncoder();
        encoder->init(*out);
        avro::encode(*encoder, avro::GenericDatum(impl_->record_node, record));
        encoder->flush();

        size_t len = out->byteCount();
        std::vector<uint8_t> buf(len);
        auto in = avro::memoryInputStream(*out);
        avro::StreamReader reader(*in);
        reader.readBytes(buf.data(), len);

        append_to_buffer(buf.data(), buf.size());
        
        // throttle
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

} // namespace signal_stream