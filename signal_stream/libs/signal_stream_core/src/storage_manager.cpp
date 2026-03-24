#include <signal_stream_core/service-storage.h>

namespace signal_stream {

    // ============================================================================
    // StorageManager Implementation
    // ============================================================================

    StorageManager::StorageManager()
        : stopFlag_(false),
        running_(false) {
    }

    StorageManager::~StorageManager() {
    }

    void StorageManager::create_source(const std::string& name,
                                       avro::ValidSchema avro_batch_schema,
                                       std::shared_ptr<arrow::Schema> arrow_schema) {
        std::scoped_lock lk(streamsMtx_); // Global lock ONLY for map modification

        auto source_ptr = std::make_shared<SourceStorage>(avro_batch_schema, arrow_schema);

        sources_[name] = std::move(source_ptr);
    }

    void StorageManager::remove_source(const std::string& streamId) {
        std::scoped_lock<std::mutex> lk(streamsMtx_);
    }

    ProducerToken StorageManager::get_producer_token(const std::string& name) {
        std::scoped_lock lk(streamsMtx_); // Global lock for lookup
        auto it = sources_.find(name);
        if (it == sources_.end()) throw std::invalid_argument("Source not found: " + name);

        // Hand out a token with a shared_ptr to the specific storage
        return ProducerToken(it->second);
    }
} // namespace signal_stream