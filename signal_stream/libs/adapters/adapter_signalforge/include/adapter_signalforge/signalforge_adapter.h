#pragma once
#include <signal_stream_core/source.h>
#include <string>
#include <memory>

namespace signal_stream {

    class SignalForgeAdapter : public Source {
    public:
        static std::shared_ptr<SignalForgeAdapter> Create(const SourceContext& ctx, const avro::ValidSchema& schema);

        SignalForgeAdapter(const SourceContext& ctx, const avro::ValidSchema& schema);
        ~SignalForgeAdapter() override;

    protected:
        bool do_on_start() override;
        bool do_on_stop() override;
        void run_once() override;

    private:
        struct Impl;
        friend struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace signal_stream