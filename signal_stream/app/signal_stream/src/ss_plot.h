#ifndef SS_PLOT_H
#define SS_PLOT_H

#include <vector>
#include <unordered_map>

#include <wx/wx.h>
#include <wx/dnd.h>
#include <wx/timer.h>

#include "line_plot.h"

// Signal Stream
#include <signal_stream_core/orchestrator.h>
#include <signal_stream_core/service-storage.h>

class SSPlot : public wxWindow {
public:
    wxTimer* timer_;
    signal_stream::Orchestrator* pm_;
    SSPlot(wxWindow* parent, signal_stream::Orchestrator* pm);

    class DropTarget : public wxDropTarget {
    public:
        struct Payload {
            std::string source_id;
            std::string signal_id;
        };


        DropTarget(SSPlot* ss_plot)
            : wxDropTarget(new wxCustomDataObject(wxDataFormat("application/x-ssplot-payload"))),
            ss_plot_(ss_plot) {}

        wxDragResult OnData(wxCoord x, wxCoord y, wxDragResult def) override {
            if (!GetData()) return wxDragNone;

            auto* dataObj = dynamic_cast<wxCustomDataObject*>(GetDataObject());
            if (!dataObj) return wxDragNone;

            if (dataObj->GetFormat() != wxDataFormat("application/x-ssplot-payload")) {
                return wxDragNone;
            }

            const char* buf = static_cast<const char*>(dataObj->GetData());
            const size_t total = dataObj->GetSize();
            
            // Minimum check: must be able to hold at least two size_t variables
            if (!buf || total < (sizeof(size_t) * 2)) {
                return wxDragNone;
            }

            const char* p = buf;
            const char* end = buf + total;

            // --- Read Source ID ---
            size_t srcLen = 0;
            memcpy(&srcLen, p, sizeof(size_t));
            p += sizeof(size_t);

            if (p + srcLen > end) return wxDragNone; // Buffer overflow check
            std::string sourceId(p, srcLen);
            p += srcLen;

            // --- Read Signal Name (the second string) ---
            if (p + sizeof(size_t) > end) return wxDragNone; // Check for second length field
            size_t sigLen = 0;
            memcpy(&sigLen, p, sizeof(size_t));
            p += sizeof(size_t);

            if (p + sigLen > end) return wxDragNone; // Buffer overflow check
            std::string signalName(p, sigLen);

            // --- Logic ---
            if (ss_plot_) {
                // Ensure the source exists in the plot
                ss_plot_->sources_[sourceId] = ss_plot_->pm_->get_source_handle(sourceId);
                
                // Push the string name instead of an integer ID
                // Assuming signals_[sourceId] is now a std::vector<std::string>
                ss_plot_->signals_[sourceId].push_back(signalName);
            }

            return wxDragCopy;
        }


    private:
        SSPlot* ss_plot_;
    };

private:

    std::unordered_map<std::string, signal_stream::Orchestrator::SourceHandle> sources_;
    std::unordered_map<std::string, std::vector<std::string>> signals_;

    std::vector<PlotSeries> series_;

    wxBoxSizer* plot_sizer_;
    LinePlot* plot_;

    void OnTimer(wxTimerEvent& event);

    friend class DropTarget;
    wxDECLARE_EVENT_TABLE();
};

#endif // SS_PLOT_H