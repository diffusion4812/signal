#pragma once

#include <memory>
#include <thread>
#include <vector>
#include <string>

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include "source.h"

namespace signal_stream {

    class UDPSource : public Source {
    public:
        static std::shared_ptr<UDPSource> Create(
            ServiceBus& bus,
            const std::string& name,
            avro::ValidSchema avro_batch_schema,
            StorageManager& storage) {
            return std::make_shared<UDPSource>(bus, name, avro_batch_schema, storage);
        }

        UDPSource(
            ServiceBus&       bus,
            const std::string& name,
            avro::ValidSchema  schema,
            StorageManager&    storage)
            : Source(bus, name, schema, storage),
              ioc_(),
              work_guard_(boost::asio::make_work_guard(ioc_)),
              socket_(ioc_)
        {
            // Pre-calculate the expected packet size by summing
            // the native width of each Avro field.
            size_t expected_size = 0;
            const std::size_t n = schema.root()->leaves();
            for (std::size_t i = 1; i < n; ++i) {
                expected_size += avroFieldSize(schema.root()->leafAt(i)->type());
            }
            recv_buffer_(expected_size);


            boost::json::object::iterator it = metadata_.find("meta.host");
            if (it == metadata_.end()) {
                throw std::runtime_error("Required metadata key 'host' not found.");
            }
            if (!it->value().is_string()) {
                throw std::runtime_error("Required metadata key 'host' wrong type.");
            }
            host_ = it->value().as_string();

            it = metadata_.find("meta.port");
            if (it == metadata_.end()) {
                throw std::runtime_error("Required metadata key 'port' not found.");
            }
            if (!it->value().is_string()) {
                throw std::runtime_error("Required metadata key 'port' wrong type.");
            }
            port_ = it->value().as_string();
        }

        ~UDPSource() override { Stop(); }

    protected:
        bool DoOnStart() override
        {
            using boost::asio::ip::udp;
            try {
                udp::resolver resolver(ioc_);
                udp::endpoint ep =
                    *resolver.resolve(udp::v4(), host_, port_).begin();

                socket_.open(ep.protocol());
                socket_.set_option(udp::socket::reuse_address(true));
                socket_.bind(ep);

                doReceive();
                io_thread_ = std::thread([this]() { ioc_.run(); });
                return true;
            }
            catch (const std::exception& e) {
                LOG_ERROR(bus_, "UDPSource bind failed: " +
                    std::string(e.what()));
                return false;
            }
        }

        bool DoOnStop() override
        {
            boost::system::error_code ec;
            socket_.cancel(ec);
            socket_.close(ec);
            work_guard_.reset();
            ioc_.stop();
            if (io_thread_.joinable()) io_thread_.join();
            return true;
        }

        void RunOnce() override {}

    private:
        void doReceive()
        {
            socket_.async_receive_from(
                boost::asio::buffer(recv_buffer_),
                remote_ep_,
                [this](auto ec, auto n) { onReceive(ec, n); });
        }

        template <typename T>
        T read_field(const std::byte* buf, size_t& offs) {
            T val;
            std::memcpy(&val, buf + offs, sizeof(T));
            offs += sizeof(T);
            return val;
        }

        void onReceive(const boost::system::error_code& ec, std::size_t bytes) {
            if (ec == boost::asio::error::operation_aborted) return;

            if (!ec && bytes > 0) {
                avro::NodePtr recordNode = avro_batch_schema_.root()->leafAt(0);
                avro::GenericRecord record(recordNode);

                size_t offs = 0;
                for (size_t i = 0; i < record.fieldCount(); ++i) {
                    auto& field = record.fieldAt(i);
                    
                    // --- SPECIAL HANDLING FOR TIMESTAMP (FIELD 1) ---
                    if (i == 0) {
                        if (field.isUnion()) {
                            auto& au = field.value<avro::GenericUnion>();
                            au.selectBranch(1); // Select the non-null branch
                            // Avro 'long' maps to int64_t
                            au.datum().value<int64_t>() = static_cast<int64_t>(now_ns_.count());
                        } else {
                            field.value<int64_t>() = static_cast<int64_t>(now_ns_.count());
                        }
                        continue;
                    }
                    // ------------------------------------------------

                    avro::GenericDatum* datum;
                    // Standard population for all other fields
                    if (field.isUnion()) {
                        auto& au = field.value<avro::GenericUnion>();
                        au.selectBranch(1);
                        datum = &au.datum();
                    } else {
                        datum = &field;
                    }

                    switch (datum->type()) {
                        case avro::AVRO_DOUBLE:
                            datum->value<double>() = read_field<double>(recv_buffer_.data(), offs);
                            break;
                        case avro::AVRO_FLOAT:
                            datum->value<float>() = read_field<float>(recv_buffer_.data(), offs);
                            break;
                        case avro::AVRO_INT:
                            datum->value<int32_t>() = read_field<int32_t>(recv_buffer_.data(), offs);
                            break;
                        case avro::AVRO_LONG:
                            datum->value<int64_t>() = read_field<int64_t>(recv_buffer_.data(), offs);
                            break;
                    }
                }

                buffer_.push_back(avro::GenericDatum(recordNode, record));
            }

            doReceive();
        }

    private:
        std::string       host_;
        std::string       port_;
        SimpleTranscoder  transcoder_;

        boost::asio::io_context ioc_;
        boost::asio::executor_work_guard<
            boost::asio::io_context::executor_type> work_guard_;
        boost::asio::ip::udp::socket               socket_;
        boost::asio::ip::udp::endpoint             remote_ep_;
        std::thread              io_thread_;
        std::vector<std::byte>   recv_buffer_;
    };

} // namespace signal_stream