#ifndef SS_PLOT_H
#define SS_PLOT_H

#include <vector>
#include <unordered_map>
#include <memory>
#include <algorithm>

#include <wx/wx.h>
#include <wx/dnd.h>
#include <wx/timer.h>

#include "line_plot.h"

#include <signal_stream_core/orchestrator.h>
#include <signal_stream_core/source_handle.h>
#include <signal_stream_core/service-storage.h>

class SSPlot : public wxWindow {
public:
    wxTimer*                      timer_;
    signal_stream::Orchestrator*  pm_;

    SSPlot(wxWindow* parent, signal_stream::Orchestrator* pm);

    // ----------------------------------------------------------------
    // Drop target
    // ----------------------------------------------------------------
    class DropTarget : public wxDropTarget {
    public:
        struct Payload {
            std::string source_id;
            std::string signal_id;
        };

        explicit DropTarget(SSPlot* ss_plot)
            : wxDropTarget(
                new wxCustomDataObject(
                    wxDataFormat("application/x-ssplot-payload")))
            , ss_plot_(ss_plot)
        {}

        wxDragResult OnData(wxCoord x, wxCoord y, wxDragResult def) override {
            if (!GetData()) return wxDragNone;

            auto* dataObj = dynamic_cast<wxCustomDataObject*>(GetDataObject());
            if (!dataObj) return wxDragNone;
            if (dataObj->GetFormat() !=
                wxDataFormat("application/x-ssplot-payload"))
                return wxDragNone;

            const char*  buf   = static_cast<const char*>(dataObj->GetData());
            const size_t total = dataObj->GetSize();
            if (!buf || total < sizeof(size_t) * 2) return wxDragNone;

            const char* p   = buf;
            const char* end = buf + total;

            // Read source ID
            size_t srcLen = 0;
            memcpy(&srcLen, p, sizeof(size_t)); p += sizeof(size_t);
            if (p + srcLen > end) return wxDragNone;
            std::string sourceId(p, srcLen);    p += srcLen;

            // Read signal name
            if (p + sizeof(size_t) > end) return wxDragNone;
            size_t sigLen = 0;
            memcpy(&sigLen, p, sizeof(size_t)); p += sizeof(size_t);
            if (p + sigLen > end) return wxDragNone;
            std::string signalName(p, sigLen);

            if (ss_plot_)
                ss_plot_->AddSignal(sourceId, signalName);

            return wxDragCopy;
        }

    private:
        SSPlot* ss_plot_;
    };

private:
    // ----------------------------------------------------------------
    // Signal/source management — called from DropTarget via friend
    // ----------------------------------------------------------------
    void AddSignal(const std::string& sourceId,
                   const std::string& signalName)
    {
        // Add source handle if not already present
        if (sources_.find(sourceId) == sources_.end()) {
            auto handle = pm_->get_source_handle(sourceId);
            if (!handle) return;

            sources_.emplace(sourceId, std::move(handle));

            // Build column index cache for this source
            if (auto schema = sources_.at(sourceId)->schema())
                RebuildColumnIndices(sourceId, schema);
        }

        // Guard against duplicate signals on the same source
        auto& sigs = signals_[sourceId];
        if (std::find(sigs.begin(), sigs.end(), signalName) == sigs.end())
            sigs.push_back(signalName);
    }

    // ----------------------------------------------------------------
    // Data
    // ----------------------------------------------------------------
    std::unordered_map<std::string,
        std::unique_ptr<signal_stream::SourceHandle>>   sources_;
    std::unordered_map<std::string,
        std::vector<std::string>>                       signals_;

    std::vector<PlotSeries>                             series_;
    std::unordered_map<std::string, int64_t>            last_row_counts_;
    std::unordered_map<std::string,
        std::unordered_map<std::string, int>>           column_indices_;

    // ----------------------------------------------------------------
    // UI
    // ----------------------------------------------------------------
    wxBoxSizer* plot_sizer_;
    LinePlot*   plot_;
    int64_t xSeconds_;

    // ----------------------------------------------------------------
    // Internal helpers
    // ----------------------------------------------------------------
    void RebuildColumnIndices(
        const std::string&                    source_name,
        const std::shared_ptr<arrow::Schema>& schema);

    bool FillSeriesFromBatches(
        PlotSeries& ps,
        const std::vector<std::shared_ptr<arrow::RecordBatch>>& batches,
        int ts_idx,
        int col_idx,
        int64_t windowCutoff);

    void OnTimer(wxTimerEvent& event);

    friend class DropTarget;
    wxDECLARE_EVENT_TABLE();
};

#endif // SS_PLOT_H