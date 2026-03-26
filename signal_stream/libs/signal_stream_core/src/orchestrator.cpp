#pragma once

#include <boost/json.hpp>

#include <signal_stream_core/orchestrator.h>

namespace signal_stream {
    // ============================================================================
    // Orchestrator Implementation
    // ============================================================================

    Orchestrator::Orchestrator(ServiceBus& bus)
        : bus_(bus)
        , registry_(bus)
        , storage_()
    {
        //std::string err;
        //load_project_from_file(path_, false, err);
    }

    Orchestrator::~Orchestrator() {
        stop_all_sources();
    }

    bool Orchestrator::load_project() {
        TRACE_FUNCTION_SCOPE(bus_);
        std::scoped_lock lock(mtx_);

        // Clear existing
        stop_all_services_locked();
        //registry_.clear();
        sources_.clear();

        // Create sources using add_source (WITHOUT taking lock - already locked)
        for (const SourceData& source_data : data_.sources) {
            if (!add_source_locked(source_data)) {
                return false;
            }
        }
        return true;
    }

    bool Orchestrator::load_project(const ProjectData& pdata) {
        TRACE_FUNCTION_SCOPE(bus_);

        data_ = pdata;
        load_project();

        return true;
    }

    // ============================================================================
    // Registry Access
    // ============================================================================

    const SourceRegistry& Orchestrator::get_registry() const {
        return registry_;
    }

    bool Orchestrator::is_source_registered(const std::string& name) const {
        return registry_.is_registered(name);
    }

    // ============================================================================
    // Source Access
    // ============================================================================

    std::shared_ptr<Source> Orchestrator::get_source(const std::string& name) const {
        std::scoped_lock lock(mtx_);
        auto it = sources_.find(name);
        return (it != sources_.end()) ? it->second : nullptr;
    }

    std::unordered_map<std::string, std::shared_ptr<Source>> Orchestrator::get_all_sources() const {
        std::scoped_lock lock(mtx_);
        return sources_;  // Returns copy of map
    }

    std::vector<std::string> Orchestrator::get_all_source_names() const {
        std::shared_lock lock(mtx_);
        std::vector<std::string> names;
        names.reserve(sources_.size());
        
        // We iterate the live sources map
        for (const auto& [name, _] : sources_) {
            names.push_back(name);
        }
        return names;
    }

    const SourceData& Orchestrator::get_data_for_source(const std::string& name) const {
        std::shared_lock lock(mtx_);
        return registry_.get_source_data(name);
    }

    std::shared_ptr<arrow::Schema> Orchestrator::get_schema_for_source(const std::string& name) const {
        std::shared_lock lock(mtx_);
        return registry_.get_arrow_schema(name);
    }

    // ============================================================================
    // Source Management
    // ============================================================================

    bool Orchestrator::add_source(const SourceData& source) {
        std::scoped_lock lock(mtx_);
        return add_source_locked(source);
    }

    // Internal implementation without locking (assumes caller holds lock)
    bool Orchestrator::add_source_locked(const SourceData& source) {
        registry_.register_source(source.name, source);

        auto arrow_schema = registry_.get_arrow_schema(source.name);
        auto avro_schema  = registry_.get_avro_batch_schema(source.name);

        storage_.create_source(source.name, avro_schema, arrow_schema);

        auto token = storage_.get_producer_token(source.name);

        SourceContext ctx = {
            bus_,
            token
        };

        // Step 3: Create source instance
        auto source_ptr = create_source_by_type(
            source.type,
            ctx,
            avro_schema
        );

        if (!source_ptr) {
            storage_.remove_source(source.name);
            registry_.unregister_source(source.name);
            return false;
        }

        sources_.emplace(source.name, std::move(source_ptr));

        // Add to project data (only if not already present - for load_project case)
        auto it = std::find_if(data_.sources.begin(), data_.sources.end(),
            [&source](const SourceData& s) { return s.name == source.name; });
        if (it == data_.sources.end()) {
            data_.sources.push_back(source);
        }

        return true;
    }

    bool Orchestrator::remove_source(const std::string& name) {
        std::scoped_lock lock(mtx_);

        auto it = sources_.find(name);
        if (it == sources_.end()) {
            return false;
        }

        try {
            it->second->stop();
        }
        catch (const std::exception& ex) {
            return false;
        }

        storage_.remove_source(name);
        registry_.unregister_source(name);

        sources_.erase(it);

        // Remove from project data
        auto data_it = std::find_if(data_.sources.begin(), data_.sources.end(),
            [&name](const SourceData& s) { return s.name == name; });
        if (data_it != data_.sources.end()) {
            data_.sources.erase(data_it);
        }
        return true;
    }

    bool Orchestrator::rename_source(const std::string& oldName, const std::string& newName) {
        

        return true;
    }

    // ============================================================================
    // Service Lifecycle
    // ============================================================================

    bool Orchestrator::start_source(const std::string& name) {
        std::scoped_lock lock(mtx_);

        auto it = sources_.find(name);
        if (it == sources_.end()) {
            return false;
        }

        try {
            it->second->start();
            return true;
        }
        catch (const std::exception& ex) {
            return false;
        }
    }

    bool Orchestrator::stop_source(const std::string& name) {
        std::scoped_lock lock(mtx_);

        auto it = sources_.find(name);
        if (it == sources_.end()) {
            return false;
        }

        try {
            it->second->stop();
        }
        catch (const std::exception& ex) {
            return false;
        }

        return true;
    }

    bool Orchestrator::start_all_sources() {
        std::scoped_lock lock(mtx_);
        return start_all_services_locked();
    }

    void Orchestrator::stop_all_sources() {
        std::scoped_lock lock(mtx_);
        stop_all_services_locked();
    }

    // ============================================================================
    // Source Status
    // ============================================================================
    bool Orchestrator::is_one_source_running() const {
        for (const auto& source : sources_) {
            if (source.second->status() == SourceStatus::Running) {
                return true;
            }
        }
        return false;
    }

    // ============================================================================
    // Storage Access
    // ============================================================================

    std::unique_ptr<SourceHandle> Orchestrator::get_source_handle(const std::string& name) {
        std::scoped_lock lock(mtx_);
        auto it = sources_.find(name);
        if (it == sources_.end()) {
            throw std::invalid_argument("Stream ID does not exist: " + name);
        }

        auto ptr = storage_.sources_[name];

        return std::make_unique<SourceHandle>(std::move(ptr));
    }

    // ============================================================================
    // Project Metadata
    // ============================================================================

    ProjectData Orchestrator::get_project_data() const {
        std::scoped_lock lock(mtx_);
        return data_;
    }

    // ============================================================================
    // Private Helpers
    // ============================================================================

    bool Orchestrator::start_all_services_locked() {
        for (auto& [name, source] : sources_) {
            try {
                if (source->status() == SourceStatus::Stopped ||
                    source->status() == SourceStatus::Error) {
                    source->start();
                }
            }
            catch (const std::exception& ex) {
                return false;
            }
        }
        return true;
    }

    void Orchestrator::stop_all_services_locked() {
        for (auto& [name, source] : sources_) {
            try {
                if (source->status() != SourceStatus::Stopped) {
                    source->stop();
                }
            }
            catch (const std::exception& ex) {
                // Early return on first failure
                return;
            }
        }
        return;
    }
    
} // namespace signal_stream