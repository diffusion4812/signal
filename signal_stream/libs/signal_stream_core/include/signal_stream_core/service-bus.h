#pragma once

#include <memory>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <any>

#include "subscriber-registry.h"
#include "subscription-token.h"

namespace signal_stream {

    class ServiceBus {
    public:
        template <typename EventT>
        SubscriptionToken Subscribe(std::function<void(const EventT&)> subscriber) {
            auto& registry = GetOrCreateRegistry<EventT>();
            int token = registry.Subscribe(std::move(subscriber));
            return SubscriptionToken([this, token]() { this->Unsubscribe<EventT>(token); });
        }

        SubscriptionToken SubscribeAll(std::function<void(const std::any&)> subscriber) {
            std::lock_guard<std::mutex> lock(mtx_);
            int token = nextWildcardToken_++;
            wildcardSubscribers_[token] = std::move(subscriber);
            return SubscriptionToken([this, token]() { this->UnsubscribeAll(token); });
        }

        template <typename EventT>
        void Publish(const EventT& event) {
            GetOrCreateRegistry<EventT>().Publish(event);
            std::lock_guard<std::mutex> lock(mtx_);
            for (auto& [_, subscriber] : wildcardSubscribers_) {
                subscriber(std::any(event));
            }
        }

    private:
        template <typename EventT>
        SubscriberRegistry<EventT>& GetOrCreateRegistry() {
            std::type_index typeId(typeid(EventT));
            std::lock_guard<std::mutex> lock(mtx_);
            auto it = registries_.find(typeId);
            if (it == registries_.end()) {
                auto registry = std::make_unique<SubscriberRegistry<EventT>>();
                registries_[typeId] = std::move(registry);
                return *static_cast<SubscriberRegistry<EventT>*>(registries_[typeId].get());
            }
            return *static_cast<SubscriberRegistry<EventT>*>(registries_[typeId].get());
        }

        template <typename EventT>
        void Unsubscribe(int token) {
            GetOrCreateRegistry<EventT>().Unsubscribe(token);
        }

        void UnsubscribeAll(int token) {
            std::lock_guard<std::mutex> lock(mtx_);
            wildcardSubscribers_.erase(token);
        }

        std::unordered_map<std::type_index, std::unique_ptr<IRegistry>> registries_;
        std::unordered_map<int, std::function<void(const std::any&)>> wildcardSubscribers_;
        int nextWildcardToken_{ 0 };
        mutable std::mutex mtx_;
    };

} // namespace signal_stream