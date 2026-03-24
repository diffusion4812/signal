#pragma once

#include "service-bus.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"

#define COMPILED_TRACE_LEVEL SPDLOG_LEVEL_TRACE

namespace signal_stream {

    class Logger {
    public:
        struct Event {
            enum class Severity : int {
                Trace = SPDLOG_LEVEL_TRACE,
                Debug = SPDLOG_LEVEL_DEBUG,
                Info = SPDLOG_LEVEL_INFO,
                Warning = SPDLOG_LEVEL_WARN,
                Error = SPDLOG_LEVEL_ERROR,
                Critical = SPDLOG_LEVEL_CRITICAL,
                Off = SPDLOG_LEVEL_OFF
            };
            Severity severity;
            std::string message;
        };
        Logger(ServiceBus& bus) :
            bus_(bus) {

            log_ = spdlog::basic_logger_mt("signal-stream logger", "logs/signal-stream.log");

            log_->set_level((spdlog::level::level_enum)COMPILED_TRACE_LEVEL);
            log_->set_pattern("[%Y-%m-%d %H:%M:%S] [%l] %v");
            spdlog::flush_every(std::chrono::seconds(30));

            token_ = bus_.Subscribe<Event>([&](const Event& ev) {
                log_->log((spdlog::level::level_enum)ev.severity, ev.message);
                });

            bus_.Publish<Event>(Event{ Event::Severity::Info, "Application logging service started" });
        }

    private:
        ServiceBus& bus_;
        SubscriptionToken token_;
        std::shared_ptr<spdlog::logger> log_;
    };

    template <size_t N>
    struct FunctionNameExtractor {
        constexpr FunctionNameExtractor(const char(&func)[N]) {
            // Store the function signature as a string_view
            funcName = std::string_view(func, N - 1);
        }

        constexpr std::string_view classAndMethod() const {
            // Find the last space (to skip return type)
            auto lastSpace = funcName.find_last_of(' ');
            auto afterSpace = (lastSpace == std::string_view::npos) ? 0 : lastSpace + 1;

            // Find the opening parenthesis for parameters
            auto parenPos = funcName.find('(', afterSpace);
            if (parenPos == std::string_view::npos) return funcName.substr(afterSpace);

            // Extract substring between last space and '('
            return funcName.substr(afterSpace, parenPos - afterSpace);
        }

        std::string_view funcName;
    };

#if COMPILED_TRACE_LEVEL <= SPDLOG_LEVEL_TRACE
#define TRACE_FUNCTION_SCOPE(bus) \
        FunctionTraceScope traceScope(bus, __FUNCTION__)
#else
#define TRACE_FUNCTION_SCOPE(bus) ((void)0)
#endif

#if !defined(__PRETTY_FUNCTION__) && !defined(__GNUC__)
#define __PRETTY_FUNCTION__ __FUNCSIG__
#endif

#define CLASS_METHOD_NAME() \
    FunctionNameExtractor(__PRETTY_FUNCTION__).classAndMethod()

#define LOG_INFO(bus, msg) \
    bus.Publish<Logger::Event>(Logger::Event{ \
        Logger::Event::Severity::Info, \
        std::string("[") + __FUNCTION__ + "] " + msg \
    })

#define LOG_ERROR(bus, msg) \
    bus.Publish<Logger::Event>(Logger::Event{ \
        Logger::Event::Severity::Error, \
        std::string("[") + __FUNCTION__ + "] " + msg \
    })

    class FunctionTraceScope {
    public:
        FunctionTraceScope(ServiceBus& bus, const std::string& function) :
            bus_(bus),
            function_(function) {
            bus_.Publish<Logger::Event>(Logger::Event{ (Logger::Event::Severity)COMPILED_TRACE_LEVEL, std::string(">> Entered ") + function_ });
        }

        ~FunctionTraceScope() {
            bus_.Publish<Logger::Event>(Logger::Event{ (Logger::Event::Severity)COMPILED_TRACE_LEVEL, std::string("<< Left ") + function_ });
        }

    private:
        ServiceBus& bus_;
        std::string function_;
    };

} // namespace signal_stream