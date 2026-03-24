#pragma once

#include "signal_stream_app/source_action_interface.h"
#include <unordered_map>
#include <functional>
#include <memory>

class SourceActionFactory {
public:
    using Creator = std::function<std::unique_ptr<ISourceActions>()>;

    void Register(const std::string& sourceType, Creator creator) {
        creators_[sourceType] = std::move(creator);
    }

    std::unique_ptr<ISourceActions> Create(const std::string& sourceType) const {
        auto it = creators_.find(sourceType);
        if (it != creators_.end())
            return it->second();
        return nullptr;
    }

private:
    std::unordered_map<std::string, Creator> creators_;
};