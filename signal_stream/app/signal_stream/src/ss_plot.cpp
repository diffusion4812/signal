#include "ss_plot.h"

wxBEGIN_EVENT_TABLE(SSPlot, wxWindow)
    EVT_TIMER(wxID_ANY, SSPlot::OnTimer)
wxEND_EVENT_TABLE()

SSPlot::SSPlot(wxWindow* parent, signal_stream::Orchestrator* pm)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize),
    pm_(pm) {
        plot_ = new LinePlot(this);
        plot_->SetAutoscaleX(true);

        plot_sizer_ = new wxBoxSizer(wxVERTICAL);
        plot_sizer_->Add(plot_, 1, wxEXPAND | wxALL, 0);
        SetSizer(plot_sizer_);

        SetDropTarget(new DropTarget(this));

        timer_ = new wxTimer(this, wxID_ANY);
        timer_->Start(10);
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

bool SSPlot::FillSeriesFromBatches(
    PlotSeries& ps,
    const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
    int ts_idx,
    int col_idx)
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
            continue;
        }

        auto floatArr = std::static_pointer_cast<arrow::FloatArray>(valArr);

        ChunkView cv;
        cv.xs        = timeArr->raw_values();
        cv.ys        = floatArr->raw_values();
        cv.len       = valArr->length();

        // Bridge from tail of previous chunk
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

void SSPlot::OnTimer(wxTimerEvent& event) {
    bool any_updated = false;
    series_.clear();

    for (auto& [source_name, handle] : sources_) {
        const auto& sigs = signals_[source_name];
        if (sigs.empty()) continue;

        int64_t totalRows = handle->total_rows();
        if (last_row_counts_[source_name] == totalRows) continue;
        last_row_counts_[source_name] = totalRows;
        any_updated = true;

        // Zero allocation — shared_ptr bumps only
        auto batches = handle->TailBatches(10000);
        if (batches.empty()) continue;

        auto schema = handle->schema();
        int ts_idx  = schema->GetFieldIndex("_timestamp");
        if (ts_idx < 0) continue;

        int idx = 0;
        for (const auto& signal : sigs) {
            int col_idx = schema->GetFieldIndex(signal);
            if (col_idx < 0) { ++idx; continue; }

            PlotSeries ps;
            ps.name       = signal;
            ps.yAxisIndex = idx++;

            // Fill directly from RecordBatches — no Table construction
            if (FillSeriesFromBatches(ps, batches, ts_idx, col_idx))
                series_.push_back(std::move(ps));
        }
    }

    if (!any_updated) return;

    plot_->SyncYAxes();
    plot_->SetSeries(series_);
    plot_->Refresh();
}