#include "ss_plot.h"

wxBEGIN_EVENT_TABLE(SSPlot, wxWindow)
    EVT_TIMER(wxID_ANY, SSPlot::OnTimer)
wxEND_EVENT_TABLE()

SSPlot::SSPlot(wxWindow* parent, signal_stream::Orchestrator* pm)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
    , pm_(pm)
    , xSeconds_(60) {
        plot_ = new LinePlot(this);
        plot_->SetAutoscaleX(false);
        plot_->onXAxisScroll = [this](int delta)
        {
            if (delta > 0)
                xSeconds_ = (xSeconds_ > 5) ? xSeconds_ - 5 : 1; // zoom in
            else
                xSeconds_ += 5;                                  // zoom out
        };


        plot_sizer_ = new wxBoxSizer(wxVERTICAL);
        plot_sizer_->Add(plot_, 1, wxEXPAND | wxALL, 0);
        SetSizer(plot_sizer_);

        SetDropTarget(new DropTarget(this));

        timer_ = new wxTimer(this, wxID_ANY);
        timer_->Start(100);
    }

void SSPlot::RebuildColumnIndices(
    const std::string&                    source_name,
    const std::shared_ptr<arrow::Schema>& schema)
{
    auto& indices = column_indices_[source_name];
    indices.clear();

    for (const auto& signal : signals_[source_name]) {
        int idx = schema->GetFieldIndex(signal);
        // idx == -1 if the signal is not present in this schema
        indices[signal] = idx;
    }
}

/// Slices a ChunkView to only include rows where xs[i] >= cutoffTime.
/// Zero-copy: advances raw pointers, reduces length. No allocation.
static ChunkView SliceChunkToWindow(const ChunkView& cv, int64_t cutoffTime)
{
    ChunkView sliced = cv;

    // Binary search for first index where xs[i] >= cutoffTime
    const int64_t* begin = cv.xs;
    const int64_t* end   = cv.xs + cv.len;
    const int64_t* it    = std::lower_bound(begin, end, cutoffTime);

    int64_t offset = std::distance(begin, it);

    sliced.xs  = cv.xs + offset;
    sliced.ys  = cv.ys + offset;
    sliced.len = cv.len - offset;

    // Bridge is only meaningful if we kept the full chunk from the start
    if (offset > 0) {
        sliced.hasBridge   = true;
        sliced.bridgeTime  = cv.xs[offset - 1];
        sliced.bridgeValue = cv.ys[offset - 1];
    }

    return sliced;
}

bool SSPlot::FillSeriesFromBatches(
    PlotSeries& ps,
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
    int ts_idx,
    int col_idx,
    int64_t windowCutoff)   // ← new: earliest timestamp to include
{
    ps.chunks.clear();

    for (const auto& batch : batches)
    {
        if (col_idx >= batch->num_columns()) continue;
        if (ts_idx  >= batch->num_columns()) continue;

        auto valArr  = batch->column(col_idx);
        auto timeArr = std::static_pointer_cast<arrow::TimestampArray>(
                           batch->column(ts_idx));

        if (!valArr || !timeArr || valArr->length() == 0) continue;

        // Enforce float32 at boundary — reject all other value types
        if (valArr->type_id() != arrow::Type::FLOAT) {
            wxLogWarning("Signal '%s' has type '%s'. Only float32 is accepted.",
                ps.name, valArr->type()->ToString());
            return false;
        }

        auto floatArr = std::static_pointer_cast<arrow::FloatArray>(valArr);

        ChunkView cv;
        cv.xs  = timeArr->raw_values();
        cv.ys  = floatArr->raw_values();
        cv.len = valArr->length();

        // Zero-copy window slice: advance pointer, shrink length
        if (windowCutoff > 0) {
            cv = SliceChunkToWindow(cv, windowCutoff);
        }

        if (cv.len == 0) continue;

        // Bridge from tail of previous chunk (within-window continuity)
        if (!ps.chunks.empty()) {
            const auto& prev = ps.chunks.back();
            cv.hasBridge   = true;
            cv.bridgeTime  = prev.xs[prev.len - 1];
            cv.bridgeValue = prev.ys[prev.len - 1];
        }

        ps.chunks.push_back(cv);
    }

    return !ps.chunks.empty();
}

void SSPlot::OnTimer(wxTimerEvent& event)
{
    bool any_updated = false;
    series_.clear();

    // --- Compute time window bounds (uint64_t nanoseconds or your epoch unit) ---
    const uint64_t xMax      = static_cast<uint64_t>(
                                   std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::system_clock::now().time_since_epoch()
                                   ).count());
    const uint64_t windowNs  = static_cast<int64_t>(xSeconds_) * 1'000'000'000ULL;
    const uint64_t xMin      = (xMax > windowNs) ? (xMax - windowNs) : 0ULL;

    for (auto& [source_name, handle] : sources_)
    {
        const auto& sigs = signals_[source_name];
        if (sigs.empty()) continue;

        int64_t totalRows = handle->total_rows();
        if (last_row_counts_[source_name] == totalRows) continue;
        last_row_counts_[source_name] = totalRows;
        any_updated = true;

        // Zero allocation — shared_ptr ref-count bumps only
        auto batches = handle->TailBatches(20000);
        if (batches.empty()) continue;

        auto schema = handle->schema();
        int ts_idx  = schema->GetFieldIndex("_timestamp");
        if (ts_idx < 0) continue;

        int idx = 0;
        for (const auto& signal : sigs)
        {
            int col_idx = schema->GetFieldIndex(signal);
            if (col_idx < 0) { ++idx; continue; }

            PlotSeries ps;
            ps.name       = signal;
            ps.yAxisIndex = idx++;

            // Fill directly from RecordBatches — no Table construction
            // Pass windowCutoff so only [xMin, xMax] data is rendered
            if (FillSeriesFromBatches(ps, batches, ts_idx, col_idx, xMin))
                series_.push_back(std::move(ps));
        }
    }

    if (!any_updated) return;

    // --- Apply fixed x-axis window to the plot ---
    plot_->SetXAxisLimits(xMin, xMax);

    plot_->SyncYAxes();
    plot_->SetSeries(series_);
    plot_->Refresh();
}