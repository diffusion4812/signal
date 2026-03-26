#include <boost/asio.hpp>

#include <wx/wx.h>
#include <wx/aui/aui.h>
#include <wx/dataobj.h>
#include <wx/dnd.h>
#include <wx/timer.h>
#include <wx/splitter.h>
#include <wx/treectrl.h>
#include <wx/app.h>
#include <cmath>

#include "ss_plot.h"
#include "project_serialization.h"
#include "source_config.h"
#include "signal_stream_app/source_action_factory.h"

#include <memory>
#include <thread>
#include <string>

#include <signal_stream_core/service-bus.h>
#include <signal_stream_core/orchestrator.h>
#include <signal_stream_core/service-storage.h>

#include <signal_stream_core/source_factory.h>

#include <adapter_random/random_adapter.h>
#include <adapter_random/random_adapter_actions.h>

#include <adapter_mqtt/mqtt_adapter.h>

#include <adapter_signalforge/signalforge_adapter.h>
#include <adapter_signalforge/signalforge_adapter_actions.h>

//#include <adapter_opcua/opcua_adapter.h>

#include <boost/throw_exception.hpp>

// Only needed if a dependency erroneously defines BOOST_NO_EXCEPTIONS
#ifdef BOOST_NO_EXCEPTIONS
void boost::throw_exception(std::exception const& e) {
    throw e;
}
void boost::throw_exception(std::exception const& e,
    boost::source_location const&) {
    throw e;
}
#endif

namespace signal_stream {
    REGISTER_SOURCE_TYPE_WITH_META("Random", RandomAdapter);
    REGISTER_SOURCE_TYPE_WITH_META("MQTT", MQTTSource);
    REGISTER_SOURCE_TYPE_WITH_META("SignalForge", SignalForgeAdapter);
    //REGISTER_SOURCE_TYPE_WITH_META("OPCUA", OPCUASource);
}

class MyApp : public wxApp {
public:
    virtual bool OnInit();
    virtual int OnExit();
};

const int ID_SOURCE_CONFIG = wxWindow::NewControlId();

class MyFrame : public wxFrame {
public:
    MyFrame();

private:
    class MyItemData : public wxTreeItemData {
    public:
        MyItemData(const std::string& srcId, const std::string& sigId)
            : source_id(srcId), signal_id(sigId) {}

        std::string source_id;
        std::string signal_id;
    };


    wxAuiManager aui_mgr_;
    wxTreeCtrl* tree_ctrl_;
    wxPanel* plot_panel_;
    wxBoxSizer* plot_sizer_;
    SSPlot* ss_plot_{nullptr};

    std::unique_ptr<signal_stream::ServiceBus> bus_;
    std::unique_ptr<boost::asio::io_context> ioc_;
    std::unique_ptr<std::jthread> work_thread_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_;

    std::unique_ptr<signal_stream::Orchestrator> pm_;

    // Handlers
    void OnTreeBeginDrag(wxTreeEvent& event);
    void OnTreeItemRightClick(wxTreeEvent& event);
    void OnConfigSource(wxTreeItemId sourceItem);
    void OnOpen(wxCommandEvent& event);
    void OnExit(wxCommandEvent& event);
    void OnClose(wxCloseEvent& event);

    SourceActionFactory action_factory_;
    wxString m_currentFilePath;

    // Helpers
    void RebuildTree();
    void RecreatePlot();

    wxDECLARE_EVENT_TABLE();
};

wxBEGIN_EVENT_TABLE(MyFrame, wxFrame)
    EVT_CLOSE(MyFrame::OnClose)
wxEND_EVENT_TABLE()

bool MyApp::OnInit() {
    wxApp::MSWEnableDarkMode(wxApp::DarkMode_Auto);
    SetAppearance(wxApp::Appearance::System);
    MyFrame* frame = new MyFrame();
    frame->Show(true);
    return true;
}

int MyApp::OnExit() {
    return wxApp::OnExit(); 
}


MyFrame::MyFrame()
    : wxFrame(nullptr, wxID_ANY, "Signal Stream", wxDefaultPosition, wxSize(800, 600)),
      aui_mgr_(this) {

    // Core services
    bus_ = std::make_unique<signal_stream::ServiceBus>();
    pm_ = std::make_unique<signal_stream::Orchestrator>(*bus_);

    // Menu bar
    wxMenuBar* menuBar = new wxMenuBar();
    wxMenu* fileMenu = new wxMenu();
    // ─── File ─────────────────────────────────────────
    fileMenu->Append(wxID_NEW,    "New\tCtrl+N");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_OPEN,   "Open\tCtrl+O");
    fileMenu->Append(wxID_SAVE,   "Save\tCtrl+S");
    fileMenu->Append(wxID_SAVEAS, "Save As\tCtrl+Shift+S");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT,   "Exit\tAlt+F4");
    menuBar->Append(fileMenu, "&File");
    SetMenuBar(menuBar);

    Bind(wxEVT_MENU, &MyFrame::OnOpen,     this, wxID_OPEN);
    //Bind(wxEVT_MENU, &MyFrame::OnSave,     this, wxID_SAVE);
    //Bind(wxEVT_MENU, &MyFrame::OnSaveAs,   this, wxID_SAVEAS);
    Bind(wxEVT_MENU, &MyFrame::OnExit,     this, wxID_EXIT);

    // UI: Tree and plot panels
    tree_ctrl_ = new wxTreeCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(250, -1), wxTR_DEFAULT_STYLE);

    plot_panel_ = new wxPanel(this, wxID_ANY);
    plot_sizer_ = new wxBoxSizer(wxVERTICAL);
    plot_panel_->SetSizer(plot_sizer_);
    plot_panel_->Layout();

    aui_mgr_.AddPane(tree_ctrl_,
        wxAuiPaneInfo()
            .Left()
            .Caption("Sources & Signals")
            .MinSize(150, -1)
            .Resizable(true));

    aui_mgr_.AddPane(plot_panel_,
        wxAuiPaneInfo()
            .Center()
            .Caption("Plot")
            .MinSize(150, -1)
            .Resizable(true));

    aui_mgr_.Update();

    tree_ctrl_->Bind(wxEVT_TREE_BEGIN_DRAG, &MyFrame::OnTreeBeginDrag, this);
    tree_ctrl_->Bind(wxEVT_TREE_ITEM_RIGHT_CLICK, &MyFrame::OnTreeItemRightClick, this);

    action_factory_.Register("Random", []() {
        return std::make_unique<RandomAdapterActions>();
    });
    action_factory_.Register("SignalForge", []() {
        return std::make_unique<SignalForgeAdapterActions>();
    });
}

void pretty_print(std::ostream& os, boost::json::value const& jv, std::string* indent = nullptr) {
    std::string indent_;
    if(! indent)
        indent = &indent_;
    switch(jv.kind())
    {
    case boost::json::kind::object:
    {
        os << "{\n";
        indent->append(4, ' ');
        auto const& obj = jv.get_object();
        if(! obj.empty())
        {
            auto it = obj.begin();
            for(;;)
            {
                os << *indent << boost::json::serialize(it->key()) << " : ";
                pretty_print(os, it->value(), indent);
                if(++it == obj.end())
                    break;
                os << ",\n";
            }
        }
        os << "\n";
        indent->resize(indent->size() - 4);
        os << *indent << "}";
        break;
    }

    case boost::json::kind::array:
    {
        os << "[\n";
        indent->append(4, ' ');
        auto const& arr = jv.get_array();
        if(! arr.empty())
        {
            auto it = arr.begin();
            for(;;)
            {
                os << *indent;
                pretty_print( os, *it, indent);
                if(++it == arr.end())
                    break;
                os << ",\n";
            }
        }
        os << "\n";
        indent->resize(indent->size() - 4);
        os << *indent << "]";
        break;
    }

    case boost::json::kind::string:
    {
        os << boost::json::serialize(jv.get_string());
        break;
    }

    case boost::json::kind::uint64:
        os << jv.get_uint64();
        break;

    case boost::json::kind::int64:
        os << jv.get_int64();
        break;

    case boost::json::kind::double_:
        os << jv.get_double();
        break;

    case boost::json::kind::bool_:
        if(jv.get_bool())
            os << "true";
        else
            os << "false";
        break;

    case boost::json::kind::null:
        os << "null";
        break;
    }

    if(indent->empty())
        os << "\n";
}

void MyFrame::OnOpen(wxCommandEvent&) {
    wxFileDialog openFileDialog(
        this,
        "Open project file",
        "",
        "",
        "JSON files (*.json)|*.json|All files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST
    );

    if (openFileDialog.ShowModal() == wxID_CANCEL) {
        return;
    }

    m_currentFilePath = openFileDialog.GetPath();

    std::ifstream in(m_currentFilePath.ToStdString());
    if (!in.is_open())
    {
        wxMessageBox("Unable to open file: " + m_currentFilePath,
                     "Error", wxICON_ERROR | wxOK, this);
        m_currentFilePath.Clear();
        return;
    }
    
    try
    {
        std::string content(
            (std::istreambuf_iterator<char>(in)),
             std::istreambuf_iterator<char>());
        in.close();

        auto jv = boost::json::parse(content);
        signal_stream::ProjectData project_data;
        signal_stream::deserialize_project(jv, project_data);
        // Ask user to confirm before reloading
        if (pm_->is_one_source_running()) {
            wxMessageDialog dlg(this, "Stop acquisition and reload project?", "Confirm", wxYES_NO | wxICON_QUESTION);
            if (dlg.ShowModal() != wxID_YES) {
            return;
            }
        }
        if (ss_plot_) ss_plot_->timer_->Stop();
        pm_->load_project(std::move(project_data));
        pm_->start_all_sources();

        RecreatePlot();
        RebuildTree();
    }
    catch (const std::exception& e)
    {
        wxMessageBox(
            wxString::Format("Failed to parse graph file: %s", e.what()),
            "Error", wxICON_ERROR | wxOK, this);
        m_currentFilePath.Clear();
        return;
    }
}

void MyFrame::OnExit(wxCommandEvent& event) {
    Close(true);
}

void MyFrame::OnClose(wxCloseEvent& event) {
    if (ss_plot_) {
        ss_plot_->timer_->Stop();
        ss_plot_->pm_->stop_all_sources();
    }
    event.Skip();
}

void MyFrame::RecreatePlot() {
    if (!plot_sizer_) return;

    if (ss_plot_) {
        plot_sizer_->Detach(ss_plot_);
        ss_plot_->Destroy();
        ss_plot_ = nullptr;
    }

    if (pm_) {
        ss_plot_ = new SSPlot(plot_panel_, pm_.get());
        plot_sizer_->Add(ss_plot_, 1, wxEXPAND | wxALL, 0);
        plot_panel_->Layout();
    }
}

void MyFrame::RebuildTree() {
    tree_ctrl_->DeleteAllItems();
    wxTreeItemId root = tree_ctrl_->AddRoot("Sources");

    if (!pm_) return;

    // 1. Get the list of names from the ProjectData (the "Plan")
    // This is faster than copying the map of live objects.
    auto project_data = pm_->get_project_data();

    for (const auto& source_info : project_data.sources) {
        const std::string& name = source_info.name;
        wxTreeItemId srcItem = tree_ctrl_->AppendItem(root, name);

        // 2. Encapsulated call to get schema
        auto schema = pm_->get_schema_for_source(name);
        
        if (schema) {
            // Use arrow::Schema built-in iterator for clarity
            for (const auto& field : schema->fields()) {
                wxTreeItemId sigItem = tree_ctrl_->AppendItem(srcItem, field->name());
                
                // MyItemData is great; wxWidgets will delete this for you
                tree_ctrl_->SetItemData(sigItem, new MyItemData(name, field->name()));
            }
        }
    }

    tree_ctrl_->ExpandAll();
}


void MyFrame::OnTreeBeginDrag(wxTreeEvent& event) {
    wxTreeItemId item = event.GetItem();
    if (!item.IsOk() || tree_ctrl_->ItemHasChildren(item)) {
        // Only allow signals (leaf nodes)
        return;
    }

    // Get signal data
    MyItemData* sigData = static_cast<MyItemData*>(tree_ctrl_->GetItemData(item));
    if (!sigData) return;

    SSPlot::DropTarget::Payload payload{ sigData->source_id, sigData->signal_id };

    // Pack payload: [src length][src string][sig length][sig string]
    size_t srcLen = payload.source_id.size();
    size_t sigLen = payload.signal_id.size();
    std::string buf;
    buf.resize(sizeof(size_t) + srcLen + sizeof(size_t) + sigLen);

    char* p = buf.data();
    memcpy(p, &srcLen, sizeof(size_t)); p += sizeof(size_t);
    memcpy(p, payload.source_id.data(), srcLen); p += srcLen;
    memcpy(p, &sigLen, sizeof(size_t)); p += sizeof(size_t);
    memcpy(p, payload.signal_id.data(), sigLen);

    wxCustomDataObject data(wxDataFormat("application/x-ssplot-payload"));
    data.SetData(buf.size(), buf.data());

    wxDropSource dragSource(data, tree_ctrl_);
    dragSource.DoDragDrop(wxDrag_CopyOnly);
}

void MyFrame::OnTreeItemRightClick(wxTreeEvent& event)
{
    wxTreeItemId clickedItem = event.GetItem();
    if (!clickedItem.IsOk())
        return;

    // Select the item so visual feedback matches the context menu target
    tree_ctrl_->SelectItem(clickedItem);

    // Determine if this is a source node (i.e., a direct child of root)
    wxTreeItemId root = tree_ctrl_->GetRootItem();
    bool isSourceNode = root.IsOk() && (tree_ctrl_->GetItemParent(clickedItem) == root);

    if (isSourceNode) {
        wxMenu contextMenu;
        contextMenu.Append(ID_SOURCE_CONFIG, "Config...");

        // Bind the menu item to the handler (using the transient menu)
        contextMenu.Bind(wxEVT_MENU, [this, clickedItem](wxCommandEvent&) {
            OnConfigSource(clickedItem);
        }, ID_SOURCE_CONFIG);

        wxPoint screenPos = tree_ctrl_->ClientToScreen(event.GetPoint());
        PopupMenu(&contextMenu, screenPos);
    }
}

void MyFrame::OnConfigSource(wxTreeItemId sourceItem)
{
    wxString sourceName = tree_ctrl_->GetItemText(sourceItem);
    auto& original = pm_->get_data_for_source(sourceName.ToStdString());

    // Caller makes the copy
    signal_stream::SourceData copy = original;

    SourceConfigDialog dlg(this, copy, action_factory_);

    if (dlg.ShowModal() == wxID_OK) {
        ss_plot_->timer_->Stop();
        pm_->stop_source(sourceName.ToStdString());
        pm_->remove_source(sourceName.ToStdString());
        pm_->add_source(copy);
        pm_->start_source(sourceName.ToStdString());
        
        RecreatePlot();
        RebuildTree();
    }
    // Cancel: copy goes out of scope, original untouched
}

wxIMPLEMENT_APP(MyApp);