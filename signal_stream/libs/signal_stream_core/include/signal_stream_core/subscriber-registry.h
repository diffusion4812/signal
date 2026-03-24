#pragma once

#include <functional>
#include <unordered_map>
#include <mutex>

namespace signal_stream {

    struct IRegistry {
        virtual ~IRegistry() = default;
    };

    template <typename EventT>
    class SubscriberRegistry : public IRegistry {
    public:
        using Subscriber = std::function<void(const EventT&)>;

        int Subscribe(Subscriber subscriber) {
            std::lock_guard<std::mutex> lock(mtx_);
            int token = nextToken_++;
            subscribers_[token] = std::move(subscriber);
            return token;
        }

        void Unsubscribe(int token) {
            std::lock_guard<std::mutex> lock(mtx_);
            subscribers_.erase(token);
        }

        void Publish(const EventT& event) {
            std::lock_guard<std::mutex> lock(mtx_);
            for (auto& [_, subscriber] : subscribers_) {
                subscriber(event);
            }
        }

    private:
        std::unordered_map<int, Subscriber> subscribers_;
        int nextToken_{ 0 };
        mutable std::mutex mtx_;
    };

} // namespace signal_stream