#pragma once
#include <functional>
#include <memory>

namespace signal_stream {

    class SubscriptionToken {
    public:
        using UnsubscribeFn = std::function<void()>;

        SubscriptionToken() : unsubscribeFn_(nullptr) {}
        SubscriptionToken(UnsubscribeFn fn) : unsubscribeFn_(std::move(fn)) {}
        ~SubscriptionToken() { if (unsubscribeFn_) unsubscribeFn_(); }

        SubscriptionToken(const SubscriptionToken&) = delete;
        SubscriptionToken& operator=(const SubscriptionToken&) = delete;

        SubscriptionToken(SubscriptionToken&& other) noexcept
            : unsubscribeFn_(std::move(other.unsubscribeFn_)) {
            other.unsubscribeFn_ = nullptr;
        }

        SubscriptionToken& operator=(SubscriptionToken&& other) noexcept {
            if (this != &other) {
                unsubscribeFn_ = std::move(other.unsubscribeFn_);
                other.unsubscribeFn_ = nullptr;
            }
            return *this;
        }

    private:
        UnsubscribeFn unsubscribeFn_;
    };

} // namespace signal_stream