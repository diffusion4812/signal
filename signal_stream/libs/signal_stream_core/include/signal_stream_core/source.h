#pragma once
#include <string>
#include <optional>
#include <memory>
#include <cstdint>
#include <functional>

namespace avro {
    class ValidSchema;
}

namespace signal_stream {

    enum class SourceStatus {
        Stopped,
        Starting,
        Running,
        Stopping,
        Error
    };

    struct SourceEvent {
        enum class Type {
            None,
            Information,
            Notification,
            Warning,
            Alarm,
            Critical
        };
        Type type;
        std::string message;
        std::optional<std::string> payload;
    };

    // Forward declarations — no #include needed
    class ServiceBus;
    class ProducerToken;
    struct SourceContext {
        ServiceBus&    bus;
        ProducerToken& token;
    };

    class Source {
    public:
        Source(const SourceContext& ctx, const avro::ValidSchema& schema);

        virtual ~Source();

        // Non-copyable, non-movable
        Source(const Source&) = delete;
        Source& operator=(const Source&) = delete;

        void start();
        void stop();

        SourceStatus       status() const;
        const SourceEvent& last_event() const;

    protected:
        // Subclasses implement these — no avro types needed
        virtual bool do_on_start() = 0;
        virtual bool do_on_stop() = 0;
        virtual void run_once() = 0;

        // Protected interface for subclasses — uses opaque types
        void append_to_buffer(const void* encoded_record, size_t size);
        void publish_event(SourceEvent::Type type, const std::string& message);
        int64_t current_timestamp_ns() const;

        // Schema metadata lookup — returns nullopt if key not found
        std::optional<std::string> get_metadata_value(const std::string& key) const;
        std::optional<std::string> get_signal_metadata_value(const std::string& signal_name, const std::string& key) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace signal_stream