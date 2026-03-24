#pragma once
#include <signal_stream_core/source.h>
#include <string>
#include <memory>

namespace signal_stream {

    class OPCUASource : public Source {
    public:
        static std::shared_ptr<OPCUASource> Create(const SourceContext& ctx, const avro::ValidSchema& schema);

        OPCUASource(const SourceContext& ctx, const avro::ValidSchema& schema);
        ~OPCUASource() override;

    protected:
        bool do_on_start() override;
        bool do_on_stop() override;
        void run_once() override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace signal_stream


