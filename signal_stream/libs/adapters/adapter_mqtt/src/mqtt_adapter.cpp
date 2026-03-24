#include "adapter_mqtt/mqtt_adapter.h"

#include <avro/Schema.hh>
#include <avro/ValidSchema.hh>
#include <avro/Generic.hh>
#include <avro/Encoder.hh>
#include <avro/Stream.hh>
#include <avro/Specific.hh>
#include <avro/Compiler.hh>

#include <async_mqtt/all.hpp>
#include <boost/asio.hpp>
#include <boost/json.hpp>

#include <vector>
#include <thread>
#include <chrono>
#include <string>
#include <mutex>
#include <queue>
#include <iostream>
#include <optional>
#include <cstring>

namespace signal_stream {

    using mqtt_client_t = async_mqtt::client<
        async_mqtt::protocol_version::v5,
        async_mqtt::protocol::mqtt>;

    // ──────────────────────────────────────────
    // PIMPL — hides MQTT + Avro internals
    // ──────────────────────────────────────────
    struct MQTTSource::Impl {
        // MQTT connection state
        boost::asio::io_context ioc;
        std::unique_ptr<mqtt_client_t> cli;
        boost::asio::steady_timer reconnect_timer;
        std::string host;
        std::string port;
        std::string topic;

        // Avro internals — completely hidden
        avro::ValidSchema schema;
        avro::NodePtr record_node;

        // Thread for io_context
        std::thread ioc_thread;
        std::atomic<bool> running{false};

        // Incoming payload queue (thread-safe)
        std::mutex queue_mutex;
        std::queue<std::string> payload_queue;

        // Back-pointer to owner for append_to_buffer
        MQTTSource* owner = nullptr;

        Impl()
            : reconnect_timer(ioc)
        {}

        ~Impl() {
            stop_io();
        }

        // ── IO lifecycle ──────────────────────

        void start_io() {
            if (running.exchange(true)) return;
            ioc_thread = std::thread([this] {
                auto guard = boost::asio::make_work_guard(ioc);
                ioc.run();
            });
        }

        void stop_io() {
            running = false;
            ioc.stop();
            if (ioc_thread.joinable()) {
                ioc_thread.join();
            }
        }

        // ── Connection lifecycle ──────────────

        void connect() {
            cli->async_underlying_handshake(
                host,
                port,
                [this](async_mqtt::error_code ec) {
                    handle_underlying_handshake(ec);
                }
            );
        }

        void reconnect() {
            reconnect_timer.expires_after(std::chrono::seconds{1});
            reconnect_timer.async_wait(
                [this](async_mqtt::error_code const& ec) {
                    if (!ec) {
                        connect();
                    }
                }
            );
        }

        void handle_underlying_handshake(async_mqtt::error_code ec) {
            if (ec) {
                std::cerr << "[MQTTSource] Handshake error: "
                          << ec.message() << std::endl;
                reconnect();
                return;
            }
            cli->async_start(
                true,                // clean_start
                std::uint16_t(15),   // keep_alive
                "",                  // Client Identifier (broker-generated)
                std::nullopt,        // will
                std::nullopt,
                std::nullopt,
                [this](async_mqtt::error_code ec,
                       std::optional<mqtt_client_t::connack_packet> connack_opt) {
                    handle_start_response(ec, connack_opt);
                }
            );
        }

        void handle_start_response(
            async_mqtt::error_code ec,
            std::optional<mqtt_client_t::connack_packet> connack_opt)
        {
            if (ec) {
                std::cerr << "[MQTTSource] CONNECT error: "
                          << ec.message() << std::endl;
                reconnect();
                return;
            }
            if (connack_opt) {
                std::cout << "[MQTTSource] CONNACK: "
                          << *connack_opt << std::endl;
            }

            std::vector<async_mqtt::topic_subopts> sub_entry{
                {topic, async_mqtt::qos::at_most_once},
            };
            cli->async_subscribe(
                *cli->acquire_unique_packet_id(),
                async_mqtt::force_move(sub_entry),
                [this](async_mqtt::error_code ec,
                       std::optional<mqtt_client_t::suback_packet> suback_opt) {
                    handle_subscribe_response(ec, suback_opt);
                }
            );
        }

        void handle_subscribe_response(
            async_mqtt::error_code ec,
            std::optional<mqtt_client_t::suback_packet> /*suback_opt*/)
        {
            if (ec) {
                std::cerr << "[MQTTSource] SUBSCRIBE error: "
                          << ec.message() << std::endl;
                reconnect();
                return;
            }
            recv_loop();
        }

        void recv_loop() {
            cli->async_recv(
                [this](async_mqtt::error_code ec,
                       std::optional<async_mqtt::packet_variant> pv_opt) {
                    handle_recv(ec, pv_opt);
                }
            );
        }

        void handle_recv(
            async_mqtt::error_code ec,
            std::optional<async_mqtt::packet_variant> pv_opt)
        {
            if (ec) {
                std::cerr << "[MQTTSource] RECV error: "
                          << ec.message() << std::endl;
                reconnect();
                return;
            }
            if (pv_opt &&
                pv_opt->type() == async_mqtt::control_packet_type::publish)
            {
                async_mqtt::v5::publish_packet packet(
                    pv_opt->get<async_mqtt::v5::publish_packet>());
                std::string payload = packet.payload();

                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    payload_queue.push(std::move(payload));
                }
            }
            recv_loop();
        }

        // ── Avro helpers (mirrors RandomSource::Impl) ──

        void populateDatumFromPayload(avro::GenericDatum& datum,
                                       const uint8_t*& cursor,
                                       size_t& remaining)
        {
            switch (datum.type()) {
                case avro::AVRO_DOUBLE: {
                    if (remaining >= sizeof(double)) {
                        double v;
                        std::memcpy(&v, cursor, sizeof(double));
                        datum.value<double>() = v;
                        cursor += sizeof(double);
                        remaining -= sizeof(double);
                    }
                    break;
                }
                case avro::AVRO_FLOAT: {
                    if (remaining >= sizeof(float)) {
                        float v;
                        std::memcpy(&v, cursor, sizeof(float));
                        datum.value<float>() = v;
                        cursor += sizeof(float);
                        remaining -= sizeof(float);
                    }
                    break;
                }
                case avro::AVRO_INT: {
                    if (remaining >= sizeof(int32_t)) {
                        int32_t v;
                        std::memcpy(&v, cursor, sizeof(int32_t));
                        datum.value<int32_t>() = v;
                        cursor += sizeof(int32_t);
                        remaining -= sizeof(int32_t);
                    }
                    break;
                }
                case avro::AVRO_LONG: {
                    if (remaining >= sizeof(int64_t)) {
                        int64_t v;
                        std::memcpy(&v, cursor, sizeof(int64_t));
                        datum.value<int64_t>() = v;
                        cursor += sizeof(int64_t);
                        remaining -= sizeof(int64_t);
                    }
                    break;
                }
                case avro::AVRO_STRING: {
                    std::string s(reinterpret_cast<const char*>(cursor),
                                  remaining);
                    datum.value<std::string>() = std::move(s);
                    cursor += remaining;
                    remaining = 0;
                    break;
                }
                case avro::AVRO_RECORD: {
                    populateRecordFromPayload(
                        datum.value<avro::GenericRecord>(),
                        cursor, remaining);
                    break;
                }
                default:
                    break;
            }
        }

        void populateRecordFromPayload(avro::GenericRecord& record,
                                        const uint8_t*& cursor,
                                        size_t& remaining)
        {
            auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch());

            for (size_t i = 0; i < record.fieldCount(); ++i) {
                auto& field = record.fieldAt(i);

                // First field is always the timestamp
                if (i == 0) {
                    int64_t ts = static_cast<int64_t>(now_ns.count());
                    if (field.isUnion()) {
                        auto& au = field.value<avro::GenericUnion>();
                        au.selectBranch(1);
                        au.datum().value<int64_t>() = ts;
                    } else {
                        field.value<int64_t>() = ts;
                    }
                    continue;
                }

                if (field.isUnion()) {
                    auto& au = field.value<avro::GenericUnion>();
                    au.selectBranch(1);
                    populateDatumFromPayload(au.datum(), cursor, remaining);
                } else {
                    populateDatumFromPayload(field, cursor, remaining);
                }
            }
        }

        void encodeAndSubmit(avro::GenericRecord& record) {
            auto out = avro::memoryOutputStream();
            auto encoder = avro::binaryEncoder();
            encoder->init(*out);
            avro::encode(*encoder,
                         avro::GenericDatum(record_node, record));
            encoder->flush();

            size_t len = out->byteCount();
            std::vector<uint8_t> buf(len);
            auto in = avro::memoryInputStream(*out);
            avro::StreamReader reader(*in);
            reader.readBytes(buf.data(), len);

            owner->append_to_buffer(buf.data(), buf.size());
        }
    };

    // ──────────────────────────────────────────
    // Public methods
    // ──────────────────────────────────────────
    std::shared_ptr<MQTTSource> MQTTSource::Create(
        const SourceContext& ctx,
        const avro::ValidSchema& schema)
    {
        return std::make_shared<MQTTSource>(ctx, schema);
    }

    MQTTSource::MQTTSource(const SourceContext& ctx,
                           const avro::ValidSchema& schema)
        : Source(ctx, schema)
        , impl_(std::make_unique<Impl>())
    {
        impl_->schema      = schema;
        impl_->record_node = impl_->schema.root()->leafAt(0);
        impl_->owner       = this;
        impl_->cli = std::make_unique<mqtt_client_t>(
            impl_->ioc.get_executor());
        impl_->reconnect_timer = boost::asio::steady_timer{
            impl_->cli->get_executor()};
    }

    MQTTSource::~MQTTSource() {
        impl_->stop_io();
    }

    bool MQTTSource::do_on_start() {
        // Extract config from base class metadata — no boost::json in this scope
        impl_->host  = get_metadata_value("host").value_or("localhost");
        impl_->port  = get_metadata_value("port").value_or("1883");
        impl_->topic = get_metadata_value("topic").value_or("");

        publish_event(SourceEvent::Type::Information,
            "MQTT config: host=" + impl_->host +
            " port=" + impl_->port +
            " topic=" + impl_->topic);

        impl_->connect();
        impl_->start_io();
        return true;
    }

    bool MQTTSource::do_on_stop() {
        impl_->stop_io();
        return true;
    }

    void MQTTSource::run_once() {
        std::string payload;
        {
            std::lock_guard<std::mutex> lock(impl_->queue_mutex);
            if (impl_->payload_queue.empty()) {
                return;
            }
            payload = std::move(impl_->payload_queue.front());
            impl_->payload_queue.pop();
        }

        avro::GenericRecord record(impl_->record_node);
        const uint8_t* cursor =
            reinterpret_cast<const uint8_t*>(payload.data());
        size_t remaining = payload.size();

        impl_->populateRecordFromPayload(record, cursor, remaining);
        impl_->encodeAndSubmit(record);
    }

} // namespace signal_stream