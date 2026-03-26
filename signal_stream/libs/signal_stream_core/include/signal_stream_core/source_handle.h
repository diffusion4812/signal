#pragma once

#include <memory>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <deque>

#include <arrow/api.h>
#include <signal_stream_core/service-storage.h>

namespace signal_stream {

class SourceHandle {
public:
    // Maximum rows retained in the ring buffer
    static constexpr int64_t kMaxRows = 20000;   // 2x the visible window

    explicit SourceHandle(
        std::shared_ptr<StorageManager::SourceStorage> storage)
        : storage_(std::move(storage))
    {
        StartPrepareThread();
    }

    ~SourceHandle() {
        StopPrepareThread();
    }

    // Non-copyable — owns a thread
    SourceHandle(const SourceHandle&)            = delete;
    SourceHandle& operator=(const SourceHandle&) = delete;

    // Movable
    SourceHandle(SourceHandle&&)                 = default;
    SourceHandle& operator=(SourceHandle&&)      = default;

    // ---------------------------------------------------------------
    // Called by the render thread — zero allocation, zero Arrow work
    // Returns the last N rows as a list of batch views (shared_ptr bumps only)
    // ---------------------------------------------------------------
    std::vector<std::shared_ptr<arrow::RecordBatch>> TailBatches(int64_t n) const
    {
        std::lock_guard lock(ringMtx_);
        std::vector<std::shared_ptr<arrow::RecordBatch>> result;
        int64_t collected = 0;

        for (auto it = ring_.rbegin(); it != ring_.rend(); ++it) {
            result.push_back(*it);
            collected += (*it)->num_rows();
            if (collected >= n) break;
        }

        std::reverse(result.begin(), result.end());
        return result;
    }

    std::shared_ptr<arrow::Schema> schema() const {
        std::lock_guard lock(storage_->mtx);
        return storage_->arrow_schema;
    }

    int64_t total_rows() const {
        std::lock_guard lock(ringMtx_);
        return ringRows_;
    }

private:
    // ---------------------------------------------------------------
    // Prepare thread — runs at ~60Hz, drains new batches from storage
    // into the local ring buffer
    // ---------------------------------------------------------------
    void StartPrepareThread() {
        running_ = true;
        prepareThread_ = std::thread([this] {
            while (running_) {
                DrainNewBatches();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        });
    }

    void StopPrepareThread() {
        running_ = false;
        if (prepareThread_.joinable())
            prepareThread_.join();
    }

    void DrainNewBatches() {
        // Take only what we have not yet seen
        std::vector<std::shared_ptr<arrow::RecordBatch>> newBatches;
        {
            std::lock_guard lock(storage_->mtx);
            if (storage_->batches.size() <= lastSeenBatchCount_) return;

            // Copy only the new tail — shared_ptr bumps, no data copy
            newBatches.assign(
                storage_->batches.begin() + lastSeenBatchCount_,
                storage_->batches.end()
            );
            lastSeenBatchCount_ = storage_->batches.size();
        }

        // Append to ring and evict old batches
        {
            std::lock_guard lock(ringMtx_);
            for (auto& b : newBatches) {
                ringRows_ += b->num_rows();
                ring_.push_back(std::move(b));
            }

            // Evict oldest batches to stay within limit
            while (ringRows_ > kMaxRows) {
                int64_t oldest = ring_.front()->num_rows();
                if (ringRows_ - oldest < kMaxRows / 2) break;
                ringRows_ -= oldest;
                ring_.pop_front();
            }
        }
    }

    // Storage service — not owned, lifetime managed by Orchestrator
    std::shared_ptr<StorageManager::SourceStorage> storage_;

    // Ring buffer — owned by this handle
    mutable std::mutex                              ringMtx_;
    std::deque<std::shared_ptr<arrow::RecordBatch>> ring_;
    int64_t                                         ringRows_           = 0;
    size_t                                          lastSeenBatchCount_ = 0;

    // Prepare thread
    std::thread      prepareThread_;
    std::atomic<bool> running_ { false };
};

} // namespace signal_stream