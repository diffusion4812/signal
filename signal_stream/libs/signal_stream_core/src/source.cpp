#include <signal_stream_core/source.h>

#include <boost/json.hpp>
#include <avro/Encoder.hh>
#include <avro/Generic.hh>
#include <avro/ValidSchema.hh>
#include <avro/Stream.hh>
#include <avro/Compiler.hh>

#include <signal_stream_core/service-bus.h>
#include <signal_stream_core/service-logger.h>
#include <signal_stream_core/service-storage.h>

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <iostream>
#include <vector>

namespace signal_stream {

    // ──────────────────────────────────────────────
    // Everything is hidden here
    // ──────────────────────────────────────────────
    struct Source::Impl {
        // Dependencies
        ServiceBus& bus;
        ProducerToken token;

        // Avro internals
        avro::ValidSchema batch_schema;
        avro::EncoderPtr avro_encoder;
        std::vector<avro::GenericDatum> buffer;

        // Boost internals
        boost::json::object metadata;

        // Threading
        std::jthread worker;
        std::atomic<SourceStatus> status{SourceStatus::Stopped};
        std::atomic<bool> running{false};
        std::mutex mtx;
        std::condition_variable cv;

        // Timing
        std::chrono::steady_clock::time_point last_flush{std::chrono::steady_clock::now()};
        std::chrono::nanoseconds now_ns{0};

        // Config
        size_t batch_size_limit;
        std::chrono::milliseconds max_flush_delay;

        // Events
        SourceEvent last_event{SourceEvent::Type::None, ""};

        // ──────────────────────────────────────────
        // Construction
        // ──────────────────────────────────────────
        Impl(const SourceContext& ctx, const avro::ValidSchema& schema)
            : bus(ctx.bus)
            , token(ctx.token)
            , batch_size_limit(50)
            , max_flush_delay(100)
        {
            // Subscribe to events
            bus.Subscribe<SourceEvent>([this](const SourceEvent& ev) {
                last_event = ev;
            });

            batch_schema = schema;
            avro_encoder = avro::binaryEncoder();
            metadata = extractSchemaMetadata(batch_schema);
        }

        // ──────────────────────────────────────────
        // Schema metadata extraction
        // ──────────────────────────────────────────
        static boost::json::object extractSchemaMetadata(
                const avro::ValidSchema& schema) {
            const avro::NodePtr& root = schema.root();
            const avro::NodePtr& record =
                (root->type() == avro::AVRO_ARRAY)
                    ? root->leafAt(0) : root;

            if (record->type() != avro::AVRO_RECORD) {
                throw std::runtime_error(
                    "Expected AVRO_RECORD but got type: " +
                    std::to_string(static_cast<int>(record->type())));
            }

            std::string doc = record->getDoc();
            if (doc.empty()) {
                throw std::runtime_error(
                    "Schema record has no doc string.");
            }

            auto doc_obj = boost::json::parse(doc).as_object();
            auto it = doc_obj.find("schema_metadata");
            if (it == doc_obj.end()) {
                throw std::runtime_error(
                    "Schema doc does not contain 'schema_metadata'.");
            }
            return doc_obj;//it->value().as_object();
        }

        // ──────────────────────────────────────────
        // Flush buffer to storage
        // ──────────────────────────────────────────
        void flush() {
            if (buffer.empty()) return;

            avro::GenericDatum arrayDatum(batch_schema);
            auto& arr = arrayDatum.value<avro::GenericArray>();
            arr.value().reserve(buffer.size());

            for (auto& record : buffer) {
                arr.value().push_back(std::move(record));
            }

            auto out = avro::memoryOutputStream();
            avro_encoder->init(*out);
            avro::encode(*avro_encoder, arrayDatum);
            avro_encoder->flush();

            size_t len = out->byteCount();
            std::vector<std::byte> payload(len);
            auto in = avro::memoryInputStream(*out);
            avro::StreamReader reader(*in);
            reader.readBytes(reinterpret_cast<uint8_t*>(payload.data()), len);

            token.try_submit(std::move(payload));
            buffer.clear();
            last_flush = std::chrono::steady_clock::now();
        }

        // ──────────────────────────────────────────
        // Run loop
        // ──────────────────────────────────────────
        void runLoop(Source& owner) {
            while (running.load()) {
                now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch());

                try {
                    owner.run_once();
                } catch (const std::exception& e) {
                    bus.Publish<Logger::Event>(Logger::Event{
                        Logger::Event::Severity::Critical,
                        std::string("Exception in runOnce: ") + e.what()
                    });
                }

                bool size_reached = (buffer.size() >= batch_size_limit);
                bool time_reached =
                    (std::chrono::steady_clock::now() - last_flush >= max_flush_delay);

                if (size_reached || time_reached) {
                    flush();
                }
            }
        }
    };

    // ──────────────────────────────────────────────
    // Source public methods — thin delegation
    // ──────────────────────────────────────────────
    Source::Source(const SourceContext& ctx, const avro::ValidSchema& schema)
        : impl_(std::make_unique<Impl>(ctx, schema))
    {}

    Source::~Source() { stop(); }

    void Source::start() {
        SourceStatus expected = SourceStatus::Stopped;
        if (!impl_->status.compare_exchange_strong(expected, SourceStatus::Starting)) {
            return;
        }

        try {
            if (!do_on_start()) {
                impl_->status.store(SourceStatus::Error);
                publish_event(SourceEvent::Type::Critical, "Failed to start service");
                return;
            }
            impl_->running.store(true);
            impl_->worker = std::jthread([this] { impl_->runLoop(*this); });
            impl_->status.store(SourceStatus::Running);
            publish_event(SourceEvent::Type::Notification, "Service started successfully");
        } catch (const std::exception& e) {
            impl_->status.store(SourceStatus::Error);
            publish_event(SourceEvent::Type::Critical,
                        std::string("Exception during start: ") + e.what());
        }
    }

    void Source::stop() {
        auto current = impl_->status.load();
        if (current == SourceStatus::Stopped || current == SourceStatus::Stopping)
            return;

        impl_->status.store(SourceStatus::Stopping);
        impl_->running.store(false);
        {
            std::lock_guard lk(impl_->mtx);
            impl_->cv.notify_all();
        }
        if (impl_->worker.joinable())
            impl_->worker.join();

        impl_->flush();
        do_on_stop();
        impl_->status.store(SourceStatus::Stopped);
        publish_event(SourceEvent::Type::Notification, "Service stopped");
    }

    SourceStatus Source::status() const {
        return impl_->status.load();
    }

    const SourceEvent& Source::last_event() const {
        return impl_->last_event;
    }

    // ──────────────────────────────────────────────
    // Protected helpers for subclasses
    // ──────────────────────────────────────────────
    void Source::publish_event(SourceEvent::Type type, const std::string& message) {
        SourceEvent ev{type, message, std::nullopt};
        impl_->last_event = ev;
        impl_->bus.Publish<SourceEvent>(ev);
    }

    int64_t Source::current_timestamp_ns() const {
        return impl_->now_ns.count();
    }

    void Source::append_to_buffer(const void* encoded_record, size_t size) {
        // Decode the raw bytes back into a GenericDatum and buffer it
        auto in = avro::memoryInputStream(
            reinterpret_cast<const uint8_t*>(encoded_record), size);
        auto decoder = avro::binaryDecoder();
        decoder->init(*in);

        avro::GenericDatum datum(impl_->batch_schema.root()->leafAt(0));
        avro::decode(*decoder, datum);
        impl_->buffer.push_back(std::move(datum));
    }

    static std::optional<std::string> json_value_to_string(const boost::json::value& val) {
        if (val.is_string())
            return std::string(val.as_string().c_str());
        if (val.is_int64())
            return std::to_string(val.as_int64());
        if (val.is_uint64())
            return std::to_string(val.as_uint64());
        if (val.is_double())
            return std::to_string(val.as_double());
        if (val.is_bool())
            return val.as_bool() ? std::string("true") : std::string("false");
        return boost::json::serialize(val);
    }

    std::optional<std::string> Source::get_metadata_value(const std::string& key) const {
        auto schema_it = impl_->metadata.find("schema_metadata");
        if (schema_it == impl_->metadata.end() || !schema_it->value().is_object())
            return std::nullopt;

        const auto& schema_meta = schema_it->value().as_object();

        auto it = schema_meta.find("meta." + key);
        if (it == schema_meta.end())
            return std::nullopt;

        return json_value_to_string(it->value());
    }

    std::optional<std::string> Source::get_signal_metadata_value(const std::string& signal_name, const std::string& key) const
    {
        auto fields_it = impl_->metadata.find("fields");
        if (fields_it == impl_->metadata.end() || !fields_it->value().is_object())
            return std::nullopt;

        const auto& fields = fields_it->value().as_object();
        auto sig_it = fields.find(signal_name);
        if (sig_it == fields.end() || !sig_it->value().is_object())
            return std::nullopt;

        const auto& meta_it = sig_it->value().as_object().find("metadata");
        if (meta_it == sig_it->value().as_object().end() || !meta_it->value().is_object())
            return std::nullopt;

        auto val_it = meta_it->value().as_object().find(key);
        if (val_it == meta_it->value().as_object().end())
            return std::nullopt;

        return json_value_to_string(val_it->value());
    }

} // namespace signal_stream