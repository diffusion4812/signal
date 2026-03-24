#include <wx/wx.h>
#include <wx/aui/aui.h>
#include <wx/stc/stc.h>
#include <wx/process.h>
#include <wx/txtstrm.h>
#include <wx/wfstream.h>
#include <wx/propgrid/propgrid.h>
#include <wx/sckipc.h>
#include <memory>
#include <functional>
#include <unordered_map>
#include "signal_forge_graph/node_canvas.h"
#include "signal_forge_graph/property_adapter.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

// Define a new sink class
class wxTextCtrlSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    // Constructor takes a pointer to the wxTextCtrl
    explicit wxTextCtrlSink(wxTextCtrl* tc) : text_ctrl_(tc) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        // Use the sink's formatter to format the message
        formatter_->format(msg, formatted);
        
        if (text_ctrl_ && wxApp::GetInstance()) {
            text_ctrl_->AppendText(wxString(fmt::to_string(formatted)));
        }
    }

    void flush_() override {
        // No buffering, so nothing to flush
    }

private:
    wxTextCtrl* text_ctrl_;
};

class ExternalProcess : public wxProcess {
public:
    using TerminateCallback = std::function<void(int status, std::shared_ptr<spdlog::logger> logger)>;

    // Constructor: Now only takes the logger, the callback, and an optional polling interval
    ExternalProcess(std::shared_ptr<spdlog::logger> logger, 
                    TerminateCallback onExit = nullptr,
                    int intervalMs = 100)
        : wxProcess()
        , logger_(logger)
        , onExitCallback_(std::move(onExit))
        , timer_(this) // 'this' is the owner of the timer
    {
        Redirect();

        // Connect the timer to our ReadStreams function
        // This tells the timer to call ReadStreams every time it ticks
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) { ReadStreams(); }, timer_.GetId());

        // Start the polling timer
        timer_.Start(intervalMs);
    }

    void ReadStreams() {
        // Read Standard Output
        while (IsInputAvailable()) {
            wxTextInputStream tis(*GetInputStream());
            wxString line = tis.ReadLine();
            if (!line.IsEmpty()) logger_->info("{}", line.ToStdString());
        }
        // Read Standard Error
        while (IsErrorAvailable()) {
            wxTextInputStream tis(*GetErrorStream());
            wxString line = tis.ReadLine();
            if (!line.IsEmpty()) logger_->error("{}", line.ToStdString());
        }
    }

    void OnTerminate(int pid, int status) override {
        // Stop the timer immediately so it doesn't fire during destruction
        timer_.Stop();

        // Final flush of remaining output
        ReadStreams();

        // Run the custom callback logic
        if (onExitCallback_) {
            onExitCallback_(status, logger_);
        }

        // IMPORTANT: Because 'timer_' is a member object (not a pointer),
        // it is destroyed automatically when 'delete this' is called.
        delete this;
    }

private:
    std::shared_ptr<spdlog::logger> logger_;
    TerminateCallback onExitCallback_;
    wxTimer timer_; // Moved inside: managed automatically by the class lifetime
};

class MainFrame : public wxFrame {
public:
    MainFrame();
    ~MainFrame();

private:
    void AddViewToggle(wxMenu* menu, int id, const wxString& label, const wxString& paneName);

    void OnGenerate(wxCommandEvent& WXUNUSED(evt));
    void OnCompile(wxCommandEvent&  WXUNUSED(evt));
    void OnTransfer(wxCommandEvent& WXUNUSED(evt));
    void OnGoOnline(wxCommandEvent& WXUNUSED(evt));
    void OnOpen(wxCommandEvent&     WXUNUSED(evt));
    void OnSave(wxCommandEvent&     WXUNUSED(evt));
    void OnSaveAs(wxCommandEvent&   WXUNUSED(evt));
    void OnExit(wxCommandEvent&     WXUNUSED(evt));
    void OnNodeSelected(wxCommandEvent& evt);
    void OnPropertyGridChanged(wxPropertyGridEvent& evt);

    wxString m_currentFilePath;

    wxAuiManager aui_mgr_;

    signal_forge::NodeCanvas* canvas_;

    wxStyledTextCtrl* c_source_ctrl_;
    wxTextCtrl*       log_ctrl_;
    wxPropertyGrid*   node_prop_ctrl_;
    std::vector<signal_forge::Property> node_prop_ctrl_properties_;
    std::unordered_map<wxPGProperty*, size_t> node_prop_ctrl_grid_to_properties_;
    std::shared_ptr<wxTextCtrlSink> log_sink_;
    std::shared_ptr<spdlog::logger> logger_;

    wxSocketClient* socket_client_;
    wxTimer* socket_timer_;
    bool CheckOrMakeConnection(wxSocketClient* socket_client);
    void OnSocketTimer(wxTimerEvent& WXUNUSED(evt));

    void SetupCppHighlighting(wxStyledTextCtrl* ctrl);
};
