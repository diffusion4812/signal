#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <wx/socket.h>
#include "task_host_core/sf_protocol.h"

class SfProtocolClient {
public:
    explicit SfProtocolClient(std::shared_ptr<spdlog::logger> logger)
        : logger_(logger), sequence_(0) {}

    // ── Connect / disconnect ──────────────────────────────────────
    bool connect(const std::string &host, uint16_t port) {
        addr_.Hostname(host);
        addr_.Service(port);
        socket_ = std::make_unique<wxSocketClient>(wxSOCKET_BLOCK);
        socket_->SetTimeout(5);
        if (!socket_->Connect(addr_, true)) {
            logger_->error("[sf_client] connect to {}:{} failed", host, port);
            return false;
        }
        logger_->info("[sf_client] connected to {}:{}", host, port);
        return true;
    }

    void disconnect() {
        if (socket_ && socket_->IsConnected())
            socket_->Close();
    }

    bool is_connected() const {
        return socket_ && socket_->IsConnected();
    }

    // ── Send a framed packet ──────────────────────────────────────
    bool send_packet(uint16_t command,
                     const void *payload = nullptr,
                     uint32_t payload_len = 0) {
        sf_header_t hdr{};
        hdr.magic        = SF_MAGIC;
        hdr.version      = SF_PROTOCOL_VERSION;
        hdr.flags        = 0;
        hdr.command      = command;
        hdr.sequence     = sequence_++;
        hdr.payload_len  = payload_len;
        hdr.header_crc   = 0;
        hdr.header_crc   = crc16(reinterpret_cast<const uint8_t*>(&hdr),
                                  sizeof(hdr));

        if (!send_exact(&hdr, sizeof(hdr))) return false;
        if (payload && payload_len > 0)
            if (!send_exact(payload, payload_len)) return false;

        return true;
    }

    // ── Receive a framed response ─────────────────────────────────
    // Returns false on disconnect or malformed packet.
    // Fills command_out and payload_out.
    bool recv_packet(uint16_t &command_out,
                     std::vector<uint8_t> &payload_out) {
        sf_header_t hdr{};
        if (!recv_exact(&hdr, sizeof(hdr))) return false;

        if (!validate_header(hdr)) return false;

        command_out = hdr.command;
        payload_out.resize(hdr.payload_len);

        if (hdr.payload_len > 0)
            if (!recv_exact(payload_out.data(), hdr.payload_len))
                return false;

        return true;
    }

    // ── Wait for a specific ACK command ──────────────────────────
    // Returns false if NACK received or timeout.
    bool wait_ack(uint16_t expected_cmd,
                  uint16_t nack_cmd,
                  std::string &error_out) {
        uint16_t cmd;
        std::vector<uint8_t> payload;

        if (!recv_packet(cmd, payload)) {
            error_out = "connection lost waiting for ACK";
            return false;
        }

        if (cmd == expected_cmd) return true;

        if (cmd == nack_cmd && payload.size() >= sizeof(sf_nack_t)) {
            sf_nack_t nack{};
            memcpy(&nack, payload.data(), sizeof(nack));
            error_out = std::string(nack.message);
            return false;
        }

        error_out = fmt::format("unexpected response 0x{:04X}", cmd);
        return false;
    }

private:
    std::shared_ptr<spdlog::logger> logger_;
    std::unique_ptr<wxSocketClient> socket_;
    wxIPV4address                   addr_;
    uint32_t                        sequence_;

    bool send_exact(const void *buf, size_t len) {
        socket_->Write(buf, len);
        return !socket_->Error() && socket_->LastCount() == len;
    }

    bool recv_exact(void *buf, size_t len) {
        socket_->SetFlags(wxSOCKET_WAITALL | wxSOCKET_BLOCK); 
        socket_->Read(buf, len);
        return !socket_->Error() && socket_->LastCount() == len;
    }

    bool validate_header(const sf_header_t &hdr) {
        if (hdr.magic != SF_MAGIC) {
            logger_->error("[sf_client] bad magic: 0x{:04X}", hdr.magic);
            return false;
        }
        sf_header_t copy = hdr;
        uint16_t    rx   = copy.header_crc;
        copy.header_crc  = 0;
        if (crc16(reinterpret_cast<const uint8_t*>(&copy),
                  sizeof(copy)) != rx) {
            logger_->error("[sf_client] header CRC mismatch");
            return false;
        }
        return true;
    }

    static uint16_t crc16(const uint8_t *data, size_t len) {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < len; i++) {
            crc ^= static_cast<uint16_t>(data[i] << 8);
            for (int b = 0; b < 8; b++)
                crc = (crc & 0x8000u)
                    ? static_cast<uint16_t>((crc << 1) ^ SF_HEADER_CRC_POLY)
                    : static_cast<uint16_t>(crc << 1);
        }
        return crc;
    }

    static uint32_t crc32(const uint8_t *data, size_t len) {
        uint32_t crc = ~0u;
        for (size_t i = 0; i < len; i++) {
            crc ^= data[i];
            for (int b = 0; b < 8; b++)
                crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
        return ~crc;
    }

    // Make crc32 accessible to OnTransfer
    friend class MainFrame;
};