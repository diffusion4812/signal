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

// =====================================================
// Core fill function — works for any numeric Arrow array
// =====================================================
template <typename ArrowArrayT>
void fill_series(PlotSeries& ps,
                 const std::shared_ptr<arrow::Array>& chunk,
                 const int64_t* raw_times,
                 int64_t length)
{
    auto arr = std::static_pointer_cast<ArrowArrayT>(chunk);
    const auto* raw = arr->raw_values();

    ps.xs.resize(length);
    ps.ys.resize(length);

    for (int64_t i = 0; i < length; ++i) {
        ps.xs[i] = static_cast<double>(raw_times[i]);
        ps.ys[i] = static_cast<double>(raw[i]);
    }
}

// =====================================================
// Boolean specialization — maps true/false to 1.0/0.0
// =====================================================
template <>
inline void fill_series<arrow::BooleanArray>(PlotSeries& ps,
                                             const std::shared_ptr<arrow::Array>& chunk,
                                             const int64_t* raw_times,
                                             int64_t length)
{
    auto arr = std::static_pointer_cast<arrow::BooleanArray>(chunk);

    ps.xs.resize(length);
    ps.ys.resize(length);
    ps.isBoolean = true;

    for (int64_t i = 0; i < length; ++i) {
        ps.xs[i] = static_cast<double>(raw_times[i]);
        ps.ys[i] = arr->Value(i) ? 1.0 : 0.0;
    }
}

// =====================================================
// Dispatcher — routes Arrow type_id to correct template
// =====================================================
inline bool dispatch_fill(PlotSeries& ps,
                          const std::shared_ptr<arrow::Array>& chunk,
                          const int64_t* raw_times,
                          int64_t length)
{
    switch (chunk->type_id()) {
        // Floating point
        case arrow::Type::FLOAT:
            fill_series<arrow::FloatArray>(ps, chunk, raw_times, length);
            return true;
        case arrow::Type::DOUBLE:
            fill_series<arrow::DoubleArray>(ps, chunk, raw_times, length);
            return true;
        case arrow::Type::HALF_FLOAT:
            fill_series<arrow::HalfFloatArray>(ps, chunk, raw_times, length);
            return true;

        // Signed integers
        case arrow::Type::INT8:
            fill_series<arrow::Int8Array>(ps, chunk, raw_times, length);
            return true;
        case arrow::Type::INT16:
            fill_series<arrow::Int16Array>(ps, chunk, raw_times, length);
            return true;
        case arrow::Type::INT32:
            fill_series<arrow::Int32Array>(ps, chunk, raw_times, length);
            return true;
        case arrow::Type::INT64:
            fill_series<arrow::Int64Array>(ps, chunk, raw_times, length);
            return true;

        // Unsigned integers
        case arrow::Type::UINT8:
            fill_series<arrow::UInt8Array>(ps, chunk, raw_times, length);
            return true;
        case arrow::Type::UINT16:
            fill_series<arrow::UInt16Array>(ps, chunk, raw_times, length);
            return true;
        case arrow::Type::UINT32:
            fill_series<arrow::UInt32Array>(ps, chunk, raw_times, length);
            return true;
        case arrow::Type::UINT64:
            fill_series<arrow::UInt64Array>(ps, chunk, raw_times, length);
            return true;

        // Boolean
        case arrow::Type::BOOL:
            fill_series<arrow::BooleanArray>(ps, chunk, raw_times, length);
            return true;

        // Timestamp (treat as int64 nanoseconds)
        case arrow::Type::TIMESTAMP:
            fill_series<arrow::TimestampArray>(ps, chunk, raw_times, length);
            return true;

        return false;
    }
}

void SSPlot::OnTimer(wxTimerEvent& event) {
    series_.clear();

    for (auto& [source_name, source] : sources_) {
        if (signals_[source_name].empty()) continue;

        auto table = source.snapshot();
        if (!table || table->num_rows() == 0) return;

        // Windowing
        int64_t total_rows = table->num_rows();
        int64_t length = std::min<int64_t>(10000, total_rows);
        int64_t offset = total_rows - length;

        auto result = table->Slice(offset, length)->CombineChunks();
        if (!result.ok()) return;
        auto contiguous = result.ValueOrDie();

        // Timestamp
        auto time_col = contiguous->GetColumnByName("_timestamp");
        if (!time_col || time_col->num_chunks() == 0) return;

        auto time_array = std::static_pointer_cast<arrow::TimestampArray>(
            time_col->chunk(0)
        );
        const int64_t* raw_times = time_array->raw_values();

        // Signals
        int idx = 0;
        for (auto& signal : signals_[source_name]) {
            auto val_col = contiguous->GetColumnByName(signal);
            if (!val_col || val_col->num_chunks() == 0) continue;

            PlotSeries ps;
            ps.name = signal;
            ps.yAxisIndex = idx++;

            if (dispatch_fill(ps, val_col->chunk(0), raw_times, length)) {
                series_.push_back(std::move(ps));
            }
        }
    }

    // Ensure enough Y axes exist for all series
    int requiredAxes = 0;
    for (auto& ps : series_) {
        requiredAxes = std::max(requiredAxes, ps.yAxisIndex + 1);
    }

    int currentAxes = plot_->GetYAxisCount();
    while (currentAxes < requiredAxes) {
        plot_->AddYAxis();
        ++currentAxes;
    }

    plot_->SetSeries(series_);
    plot_->Refresh();
}