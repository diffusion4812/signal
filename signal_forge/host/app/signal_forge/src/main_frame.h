#include <wx/wx.h>
#include <wx/aui/aui.h>
#include <wx/stc/stc.h>
#include <wx/process.h>
#include <wx/txtstrm.h>
#include <wx/wfstream.h>
#include <wx/treectrl.h>
#include <wx/propgrid/propgrid.h>
#include <wx/sckipc.h>
#include <memory>
#include <functional>
#include <unordered_map>
#include "signal_forge_graph/node_canvas.h"
#include "signal_forge_graph/property_adapter.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>

#include "dbg_pubsub.h"
#include "dbg_util.h"

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
    struct ProjectManifest
    {
        std::vector<wxString> block_defs;
        std::vector<wxString> tasks;
    };

    struct GraphDocument
    {
        enum class DocumentType
        {
            BlockDefinition,
            Task
        };
        wxString file_path;
        wxString display_name;
        bool dirty = false;
        DocumentType type = DocumentType::BlockDefinition;

        signal_forge::Generator::Result result_;
        std::string compiled_filename_;

        std::unique_ptr<signal_forge::Graph> graph;
        wxPanel* panel = nullptr;
        signal_forge::NodeCanvas* canvas = nullptr;
    };


    bool LoadProjectManifest(const wxString& projectFilePath);
    void PopulateProjectTree();
    void AddViewToggle(wxMenu* menu, int id, const wxString& label, const wxString& paneName);

    void OnGenerate(wxCommandEvent& WXUNUSED(evt));
    void OnCompile(wxCommandEvent&  WXUNUSED(evt));
    void OnTransfer(wxCommandEvent& WXUNUSED(evt));
    void OnGoOnline(wxCommandEvent& WXUNUSED(evt));
    wxString MakeProjectRelativePath(const wxString& absolutePath) const;
    bool ManifestContainsPath(const std::vector<wxString>& entries, const wxString& relPath) const;
    bool EnsureDocumentInProjectManifest(GraphDocument* doc);
    bool SaveProjectManifest();
    void OnOpen(wxCommandEvent&     WXUNUSED(evt));
    bool SaveDocument(GraphDocument* doc);
    bool SaveDocumentAs(GraphDocument* doc);
    void OnSave(wxCommandEvent&     WXUNUSED(evt));
    void OnSaveAs(wxCommandEvent&   WXUNUSED(evt));
    void OnExit(wxCommandEvent&     WXUNUSED(evt));
    void OpenGraphDocument(const wxString& filePath);
    GraphDocument* FindDocumentByPage(wxWindow* page);
    GraphDocument* FindDocumentByCanvas(signal_forge::NodeCanvas* canvas);
    void UpdateDocumentTabTitle(GraphDocument* doc);
    void SetDocumentDirty(GraphDocument* doc, bool dirty);
    void OnEditorPageClose(wxAuiNotebookEvent& evt);
    void OnEditorPageChanged(wxAuiNotebookEvent& evt);
    void OnProjectTreeItemActivated(wxTreeEvent& evt);
    void OnNodeSelected(wxCommandEvent& evt);
    void WriteInjectNodeFields(signal_forge::InjectNode *inject);
    void OnGraphModified(wxCommandEvent& evt);
    void OnPropertyGridChanged(wxPropertyGridEvent& evt);

    wxString m_currentProjectPath;
    wxString m_currentProjectFile;
    wxString m_currentGraphFilePath;

    ProjectManifest m_projectManifest;

    wxAuiManager aui_mgr_;

    wxAuiNotebook* editor_notebook_ = nullptr;
    std::vector<std::unique_ptr<GraphDocument>> open_documents_;

    class ProjectTreeItemData : public wxTreeItemData
    {
    public:
        enum class ItemType
        {
            Folder,
            BlockDefinitionFile,
            TaskFile
        };

        ProjectTreeItemData(ItemType type, const wxString& path = wxEmptyString)
            : type_(type), path_(path) {}

        ItemType type_;
        wxString path_;
    };
    wxTreeCtrl*       project_tree_ctrl_;

    wxStyledTextCtrl* c_source_ctrl_;
    wxTextCtrl*       log_ctrl_;
    wxPropertyGrid*   node_prop_ctrl_;
    std::vector<signal_forge::Property> node_prop_ctrl_properties_;
    std::unordered_map<wxPGProperty*, size_t> node_prop_ctrl_grid_to_properties_;
    std::shared_ptr<wxTextCtrlSink> log_sink_;
    std::shared_ptr<spdlog::logger> logger_;

    wxSocketClient* socket_client_;

    // Online polling & debug
    wxTimer* socket_timer_;
    bool CheckOrMakeConnection(wxSocketClient* socket_client);
    void OnSocketTimer(wxTimerEvent& WXUNUSED(evt));
    dbg_sub_config_t    cfg_    = {};
    dbg_subscriber_t   *sub_    = nullptr;
    dbg_sub_layout_t    layout_ = {};
    uint16_t            effective_sub_id_ = DBG_SUB_ID_AUTO;
    std::unordered_map<uint64_t, signal_forge::ProbeNode*>  probe_field_map_;
    std::unordered_map<uint64_t, signal_forge::InjectNode*> inject_field_map_;

    void SetupCppHighlighting(wxStyledTextCtrl* ctrl);
};
