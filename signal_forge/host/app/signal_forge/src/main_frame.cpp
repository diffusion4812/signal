#include <iostream>
#include <fstream>
#include <cstdlib>

#include "app.h"
#include "main_frame.h"
#include "graph_serialization.h"

#include "task_host_core/interface.h"
#include "task_host_core/manifest.h"

const int ID_NODE_CANVAS_GENERATE             = wxWindow::NewControlId();
const int ID_NODE_CANVAS_COMPILE              = wxWindow::NewControlId();
const int ID_NODE_CANVAS_TRANSFER             = wxWindow::NewControlId();
const int ID_GO_ONLINE                        = wxWindow::NewControlId();
const int ID_MENU_VIEW_NODE_PROPERTIES_CHECK  = wxWindow::NewControlId();
const int ID_MENU_VIEW_C_SOURCE_CHECK         = wxWindow::NewControlId();
const int ID_MENU_VIEW_LOG_CHECK              = wxWindow::NewControlId();
const int ID_COMPILE_TIMER                    = wxWindow::NewControlId();
const int ID_SOCKET_TIMER                     = wxWindow::NewControlId();

MainFrame::MainFrame()
: wxFrame(nullptr, wxID_ANY, "Signal Forge", wxDefaultPosition, wxSize(1280, 800)) {
    aui_mgr_.SetManagedWindow(this);
    aui_mgr_.SetFlags(wxAUI_MGR_RECTANGLE_HINT | wxAUI_MGR_LIVE_RESIZE | wxAUI_MGR_ALLOW_ACTIVE_PANE | wxAUI_MGR_ALLOW_FLOATING);

    canvas_ = new signal_forge::NodeCanvas(this, &wxGetApp().get_graph());
    Bind(EVT_NODE_SELECTED, &MainFrame::OnNodeSelected, this);

    log_ctrl_    = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_MULTILINE | wxTE_READONLY);
    log_sink_    = std::make_shared<wxTextCtrlSink>(log_ctrl_);
    logger_      = std::make_shared<spdlog::logger>("Signal Forge", log_sink_);
    logger_->set_level(spdlog::level::trace);

    c_source_ctrl_ = new wxStyledTextCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    SetupCppHighlighting(c_source_ctrl_);

    node_prop_ctrl_ = new wxPropertyGrid(this, wxID_ANY, wxDefaultPosition, wxSize(300, -1), wxPG_BOLD_MODIFIED | wxPG_SPLITTER_AUTO_CENTER);
    node_prop_ctrl_->Bind(wxEVT_PG_CHANGED, &MainFrame::OnPropertyGridChanged, this);

    aui_mgr_.AddPane(canvas_, wxAuiPaneInfo()
        .CenterPane().Caption("Editor"));

    aui_mgr_.AddPane(node_prop_ctrl_, wxAuiPaneInfo()
        .Name("Properties").Right()
        .Caption("Properties"));
    aui_mgr_.AddPane(c_source_ctrl_, wxAuiPaneInfo()
        .Name("Source").Right()
        .Caption("Source").Hide());

    aui_mgr_.AddPane(log_ctrl_, wxAuiPaneInfo()
        .Name("Log").Bottom()
        .Caption("Log").MinSize(-1, 150));

    aui_mgr_.Update();

    auto *menuBar   = new wxMenuBar();
    auto *fileMenu  = new wxMenu();
    auto *buildMenu = new wxMenu();
    auto *viewMenu  = new wxMenu();

    // ─── File ─────────────────────────────────────────
    fileMenu->Append(wxID_NEW,    "New\tCtrl+N");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_OPEN,   "Open\tCtrl+O");
    fileMenu->Append(wxID_SAVE,   "Save\tCtrl+S");
    fileMenu->Append(wxID_SAVEAS, "Save As\tCtrl+Shift+S");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT,   "Exit\tAlt+F4");

    // ─── Build ────────────────────────────────────────
    buildMenu->Append(ID_NODE_CANVAS_GENERATE, "Generate\tF5");
    buildMenu->Append(ID_NODE_CANVAS_COMPILE,  "Compile\tF6");
    buildMenu->Append(ID_NODE_CANVAS_TRANSFER, "Transfer\tF7");
    buildMenu->AppendSeparator();
    buildMenu->Append(ID_GO_ONLINE,            "Go Online\tF8");

    // ─── View ─────────────────────────────────────────
    AddViewToggle(viewMenu, ID_MENU_VIEW_NODE_PROPERTIES_CHECK, "Node Properties", "Properties");
    AddViewToggle(viewMenu, ID_MENU_VIEW_C_SOURCE_CHECK,        "Generated Source", "Source");
    AddViewToggle(viewMenu, ID_MENU_VIEW_LOG_CHECK,             "Log",              "Log");

    // ─── Assemble ─────────────────────────────────────
    menuBar->Append(fileMenu,  "File");
    menuBar->Append(buildMenu, "Build");
    menuBar->Append(viewMenu,  "View");
    SetMenuBar(menuBar);

    Bind(wxEVT_MENU, &MainFrame::OnGenerate, this, ID_NODE_CANVAS_GENERATE);
    Bind(wxEVT_MENU, &MainFrame::OnCompile,  this, ID_NODE_CANVAS_COMPILE);
    Bind(wxEVT_MENU, &MainFrame::OnTransfer, this, ID_NODE_CANVAS_TRANSFER);
    Bind(wxEVT_MENU, &MainFrame::OnGoOnline, this, ID_GO_ONLINE);
    Bind(wxEVT_MENU, &MainFrame::OnOpen,     this, wxID_OPEN);
    Bind(wxEVT_MENU, &MainFrame::OnSave,     this, wxID_SAVE);
    Bind(wxEVT_MENU, &MainFrame::OnSaveAs,     this, wxID_SAVEAS);
    Bind(wxEVT_MENU, &MainFrame::OnExit,     this, wxID_EXIT);

    socket_client_ = new wxSocketClient();
    socket_timer_ = new wxTimer(this, ID_SOCKET_TIMER);
    Bind(wxEVT_TIMER, &MainFrame::OnSocketTimer, this, ID_SOCKET_TIMER);
}

MainFrame::~MainFrame() {
    aui_mgr_.UnInit();
}

void MainFrame::AddViewToggle(wxMenu* menu, int id, const wxString& label, const wxString& paneName)
{
    menu->Append(id, label, "", wxITEM_CHECK);

    // Toggle visibility when menu item is activated
    Bind(wxEVT_MENU, [this, paneName](wxCommandEvent& evt) {
        wxAuiPaneInfo& pane = aui_mgr_.GetPane(paneName); // IMPORTANT: reference
        if (!pane.IsOk()) return;
        pane.Show(evt.IsChecked());
        aui_mgr_.Update();
    }, id);

    // Keep the checkbox in sync with the pane's visibility
    Bind(wxEVT_UPDATE_UI, [this, paneName](wxUpdateUIEvent& ue) {
        // Bound to the same id, so this will only be called for the matching UI update
        wxAuiPaneInfo& pane = aui_mgr_.GetPane(paneName);
        ue.Check(pane.IsOk() && pane.IsShown());
    }, id);
}

void MainFrame::OnGenerate(wxCommandEvent& WXUNUSED(evt)) {
    signal_forge::Generator gen(wxGetApp().get_graph());
    wxGetApp().set_generation_result(gen.generate());
    std::ofstream outfile("output.c");
    outfile << wxGetApp().get_generation_result().c_source;
    if (c_source_ctrl_) {
        c_source_ctrl_->SetReadOnly(false);
        c_source_ctrl_->SetText(wxGetApp().get_generation_result().c_source);
        c_source_ctrl_->SetReadOnly(true);
    }
    outfile.close();
}

void MainFrame::OnCompile(wxCommandEvent& WXUNUSED(evt)) {
    logger_->info("Starting compilation...");

    // Build the command (Same as before, but without the "echo" redirection string)
    wxString timestamp = wxDateTime::Now().Format("%Y%m%d_%H%M%S");
    std::string output = "libmy_task_" + timestamp.ToStdString() + ".so";

    std::string zig_path = wxGetApp().get_config().zig.string();
    std::string command = "\"" + zig_path + "\" cc -v -v -target x86_64-linux-gnu -shared -fPIC ";
    command += "-o " + output + " output.c";
    command += " \"-IC:/Users/LOAR02/Source/signal/signal_forge/target/libs/task_host_core/include\"";

    for (const std::string& header : wxGetApp().get_generation_result().link_headers) {
        size_t last_slash = header.find_last_of("/\\");
        if (last_slash != std::string::npos) {
            command += " -I\"" + header.substr(0, last_slash) + "\"";
        }
    }

    for (const std::string& obj_path : wxGetApp().get_generation_result().link_objects) {
        command += " \"" + obj_path + "\"";
    }

    auto* process = new ExternalProcess(logger_,
        [output](int status, std::shared_ptr<spdlog::logger> logger) {
            if (status == 0) {
                logger->info("Compilation successful!");
                wxGetApp().set_compiled_filename(output);
            } else {
                logger->critical("Compilation failure with code {}", status);
            }
        }
    );

    if (wxExecute(command, wxEXEC_ASYNC, process) == 0) {
        logger_->error("Failed to launch compile process.");
        delete process;
    }
}

void MainFrame::OnTransfer(wxCommandEvent& WXUNUSED(evt)) {
    const std::string compiled_file = wxGetApp().get_compiled_filename();
    if (!compiled_file.length()) {
        logger_->info("Nothing to transfer...");
        return;
    }
    logger_->info("Starting transfer...");

    std::string putty_scp_path = wxGetApp().get_config().putty_scp.string();
    std::string command = "\"" + putty_scp_path + "\" -batch -q ";
    command += "-pw \"password\" \"" + compiled_file +"\" ";
    command += "loar02@127.0.0.1:\"/mnt/c/Users/LOAR02/Source/signal/signal_forge/target/build/app/task_host\"";

    auto* process = new ExternalProcess(logger_,
        [compiled_file](int status, std::shared_ptr<spdlog::logger> lg) {
            if (status == 0) {
                lg->info("Transfer successful! Connecting to task host...");
                wxSocketClient socket;
                wxIPV4address addr;
                addr.Hostname("127.0.0.1");
                addr.Service(9000);
                if (socket.Connect(addr, true)) {
                    socket.Write(std::string("./" + compiled_file).c_str(), compiled_file.length() + 2);
                    lg->info("Successfully signaled task host");
                } else {
                    lg->error("Transfer succeeded, but could not connect to task host on port 9000");
                }
            } else {
                lg->error("PSCP transfer failed with exit code {}", status);
            }
        }
    );

    if (wxExecute(command, wxEXEC_ASYNC, process) == 0) {
        logger_->error("Failed to launch PSCP");
        delete process;
    }
}

void MainFrame::OnGoOnline(wxCommandEvent& WXUNUSED(evt))
{
    auto &graph = wxGetApp().get_graph();

    /*
     * Toggle behaviour: if already running, go offline.
     */
    if (socket_timer_->IsRunning()) {
        socket_timer_->Stop();
        logger_->info("Went offline — timer stopped.");

        if (sub_) {
            dbg_sub_destroy(sub_);
            sub_ = nullptr;
        }

        probe_field_map_.clear();
        inject_field_map_.clear();
        return;
    }

    /*
     * Build field_ids and types arrays by walking the graph.
     * Both ProbeNode (reads its input) and InjectNode (monitors its
     * output) participate as read-only subscribers here.
     */
    std::vector<uint64_t>          field_ids;
    std::vector<dbg_value_type_t>  types;

    probe_field_map_.clear();
    inject_field_map_.clear();

    for (const std::unique_ptr<signal_forge::Node> &node : graph.Nodes()) {
        // TODO: keep probenodes separate if they are being driven from the same output pin

        /* ── ProbeNode: field_id comes from the pin driving input[0] ── */
        if (auto *probe = dynamic_cast<signal_forge::ProbeNode*>(node.get())) {
            uint64_t link_id = graph.FindLinkToPin(probe->inputs[0].id);
            if (link_id == (uint64_t)-1) continue;

            auto *link = graph.FindLink(link_id);
            if (!link) continue;

            uint64_t field_id = link->from_pin; /* pin id == field_id */

            field_ids.push_back(field_id);
            types.push_back(DBG_VT_F32);
            probe_field_map_[field_id] = probe;

            logger_->info("Subscribing ProbeNode field_id={}", field_id);
            continue;
        }

        /* ── InjectNode: field_id comes from its own output pin ── */
        if (auto *inject = dynamic_cast<signal_forge::InjectNode*>(node.get())) {
            if (inject->outputs.empty()) continue;

            uint64_t field_id = inject->outputs[0].id;

            field_ids.push_back(field_id);
            types.push_back(DBG_VT_F32);
            inject_field_map_[field_id] = inject;

            logger_->info("Subscribing InjectNode field_id={}", field_id);
            continue;
        }
    }

    if (field_ids.empty()) {
        logger_->warn("No subscribable nodes found in graph — not going online.");
        return;
    }

    auto count = static_cast<uint16_t>(field_ids.size());

    /* ── Create subscriber ── */
    cfg_             = DBG_SUB_CONFIG_DEFAULT;
    cfg_.host        = "127.0.0.1";
    cfg_.data_port   = 9500;
    cfg_.config_port = 9501;

    sub_ = dbg_sub_create(&cfg_);
    if (!sub_) {
        logger_->error("Failed to create subscriber.");
        return;
    }

    /* ── Subscribe — let the publisher assign an ID ── */
    dbg_status_t rc = dbg_sub_subscribe(
        sub_,
        DBG_SUB_ID_AUTO,
        field_ids.data(),
        types.data(),
        count,
        250'000,      /* interval_us */
        &layout_);

    if (rc != DBG_OK) {
        logger_->error("dbg_sub_subscribe failed: {}", (int)rc);
        dbg_sub_destroy(sub_);
        sub_ = nullptr;
        return;
    }

    /*
     * Capture the effective sub_id assigned by the publisher.
     * This must be used for all future poll/unsubscribe calls.
     */
    effective_sub_id_ = layout_.sub_id;
    logger_->info("Subscribed OK — assigned sub_id={}, {} field(s), "
                  "frame_size={}, actual_interval={}us",
                  effective_sub_id_,
                  layout_.field_count,
                  layout_.frame_size,
                  layout_.actual_interval_us);

    /* ── Start the poll timer to match the subscription interval ── */
    constexpr int kTimerMs = 250;
    socket_timer_->Start(kTimerMs);
    logger_->info("Went online — timer started ({} ms).", kTimerMs);
}

void MainFrame::OnOpen(wxCommandEvent& WXUNUSED(evt))
{
    wxFileDialog openDialog(
        this,
        "Open Graph",
        "",
        "",
        "Graph Files (*.json)|*.json|All Files (*.*)|*.*",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST
    );

    if (openDialog.ShowModal() == wxID_CANCEL)
        return;

    m_currentFilePath = openDialog.GetPath();

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
        signal_forge::deserialize_graph(jv, wxGetApp().get_graph());
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

void MainFrame::OnSave(wxCommandEvent& WXUNUSED(evt))
{
    if (m_currentFilePath.IsEmpty())
    {
        wxFileDialog saveDialog(
            this,
            "Save Graph",
            "",
            "",
            "Graph Files (*.json)|*.json|All Files (*.*)|*.*",
            wxFD_SAVE | wxFD_OVERWRITE_PROMPT
        );

        if (saveDialog.ShowModal() == wxID_CANCEL)
            return;

        m_currentFilePath = saveDialog.GetPath();
    }

    try
    {
        auto jv = signal_forge::serialize_graph(wxGetApp().get_graph());

        std::ofstream out(m_currentFilePath.ToStdString());
        if (!out.is_open())
        {
            wxMessageBox("Unable to save file: " + m_currentFilePath,
                         "Error", wxICON_ERROR | wxOK, this);
            return;
        }

        std::string indent("  ");
        pretty_print(out, jv, &indent);
        out.close();
    }
    catch (const std::exception& e)
    {
        wxMessageBox(
            wxString::Format("Failed to save graph file:%s", e.what()),
            "Error", wxICON_ERROR | wxOK, this);
        return;
    }
}

void MainFrame::OnSaveAs(wxCommandEvent& WXUNUSED(evt))
{
    wxFileDialog saveDialog(
        this,
        "Save Graph",
        "",
        "",
        "Graph Files (*.json)|*.json|All Files (*.*)|*.*",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT
    );

    if (saveDialog.ShowModal() == wxID_CANCEL)
        return;

    m_currentFilePath = saveDialog.GetPath();

    // Save as normal
    wxCommandEvent evt(wxEVT_MENU, wxID_SAVE);
    GetEventHandler()->AddPendingEvent(evt);
}

void MainFrame::OnExit(wxCommandEvent& WXUNUSED(evt))
{
    Close(true); 
}

bool MainFrame::CheckOrMakeConnection(wxSocketClient* socket_client) {
    // Only attempt to connect if the socket is NOT currently connected
    if (!socket_client->IsConnected()) {
        wxIPV4address addr;
        addr.Hostname("127.0.0.1");
        addr.Service(9000);

        // Attempt to connect
        socket_client->Connect(addr, false);

        // Wait for the connection to establish
        if (!socket_client->WaitOnConnect(0, 500)) {
            logger_->error("Connection timeout or failed to connect.");
            return false;
        }
        // If we reach here, connection was attempted and successfully established within the timeout
        logger_->info("Successfully established new connection.");
    } else {
        // Log that we are already connected (optional, but good for debugging)
        logger_->debug("Socket already connected, no new connection attempt needed.");
    }
    return true; // Return true if already connected or if a new connection was successful
}

void MainFrame::OnSocketTimer(wxTimerEvent& WXUNUSED(evt))
{
    if (!sub_) return;

    dbg_frame_result_t result;
    int rc = dbg_sub_poll_iter(sub_, &layout_, &result);

    if (rc < 0) {
        logger_->warn("dbg_sub_poll_iter error: {}", rc);
        return;
    }

    if (rc == 0) {
        /* No frame available yet — normal during startup or a brief gap */
        return;
    }

    /* ── Walk the iterator and update node values ── */
    dbg_value_t val;
    while (dbg_frame_iter_next(&result.iter, &val) == DBG_OK) {

        if (result.iter.current_type != DBG_VT_F32) {
            logger_->warn("Unexpected field type {} for field_id={}",
                          (int)result.iter.current_type,
                          result.iter.current_field_id);
            continue;
        }

        const uint64_t field_id = result.iter.current_field_id;
        const float    value    = val.f32;

        /* ── ProbeNode ── */
        auto probe_it = probe_field_map_.find(field_id);
        if (probe_it != probe_field_map_.end()) {
            probe_it->second->value = value;
            continue;
        }

        /* ── InjectNode (monitor only — writes handled elsewhere) ── */
        auto inject_it = inject_field_map_.find(field_id);
        if (inject_it != inject_field_map_.end()) {
            inject_it->second->observed_value = value;
            continue;
        }

        logger_->warn("Frame contained unregistered field_id={}", field_id);
    }

    canvas_->Refresh();
}

void MainFrame::OnNodeSelected(wxCommandEvent& evt) {
    wxString s = evt.GetString();
    unsigned long long ull = 0;
    if (!s.ToULongLong(&ull)) {
        // handle parse error
        return;
    }
    uint64_t id = static_cast<uint64_t>(ull);

    // Lookup node by id in host-side graph/store
    signal_forge::Node* node = wxGetApp().get_graph().FindNode(id);
    if (!node) return;

    node_prop_ctrl_properties_.clear();
    if (auto* probe = dynamic_cast<signal_forge::ProbeNode*>(node)) {
        node_prop_ctrl_properties_.push_back(signal_forge::Property{probe->id, "ID", signal_forge::PropType::UInt64, &probe->id, 0, 0, true});
        node_prop_ctrl_properties_.push_back(signal_forge::Property{probe->id, "Position X", signal_forge::PropType::Float, &probe->x, -1e6, 1e6, false});
        node_prop_ctrl_properties_.push_back(signal_forge::Property{probe->id, "Position Y", signal_forge::PropType::Float, &probe->y, -1e6, 1e6, false});
    } else if (auto* inject = dynamic_cast<signal_forge::InjectNode*>(node)) {
        node_prop_ctrl_properties_.push_back(signal_forge::Property{inject->id, "ID", signal_forge::PropType::UInt64, &inject->id, 0, 0, true});
        node_prop_ctrl_properties_.push_back(signal_forge::Property{inject->id, "Position X", signal_forge::PropType::Float, &inject->x, -1e6, 1e6, false});
        node_prop_ctrl_properties_.push_back(signal_forge::Property{inject->id, "Position Y", signal_forge::PropType::Float, &inject->y, -1e6, 1e6, false});
        node_prop_ctrl_properties_.push_back(signal_forge::Property{inject->id, "Active", signal_forge::PropType::Bool, &inject->forcing_active, -1e6, 1e6, false});
        node_prop_ctrl_properties_.push_back(signal_forge::Property{inject->id, "Value", signal_forge::PropType::Float, &inject->forced_value, -1e6, 1e6, false});
    } else {
        node_prop_ctrl_properties_.push_back(signal_forge::Property{node->id, "ID", signal_forge::PropType::UInt64, &node->id, 0, 0, true});
        node_prop_ctrl_properties_.push_back(signal_forge::Property{node->id, "Position X", signal_forge::PropType::Float, &node->x, -1e6, 1e6, false});
        node_prop_ctrl_properties_.push_back(signal_forge::Property{node->id, "Position Y", signal_forge::PropType::Float, &node->y, -1e6, 1e6, false});
    }

    node_prop_ctrl_->Clear();
    node_prop_ctrl_grid_to_properties_.clear();

    for (size_t i = 0; i < node_prop_ctrl_properties_.size(); ++i) {
        const signal_forge::Property& p = node_prop_ctrl_properties_[i];
        wxPGProperty* pg = nullptr;

        switch (p.type) {
        case signal_forge::PropType::Float: {
            float v = *static_cast<float*>(p.ptr);
            pg = node_prop_ctrl_->Append(new wxFloatProperty(p.name, wxPG_LABEL, static_cast<double>(v)));
            pg->SetAttribute(wxPG_ATTR_MIN, p.min);
            pg->SetAttribute(wxPG_ATTR_MAX, p.max);
            break;
        }
        case signal_forge::PropType::Int: {
            int v = *static_cast<int*>(p.ptr);
            pg = node_prop_ctrl_->Append(new wxIntProperty(p.name, wxPG_LABEL, v));
            pg->SetAttribute(wxPG_ATTR_MIN, p.min);
            pg->SetAttribute(wxPG_ATTR_MAX, p.max);
            break;
        }
        case signal_forge::PropType::Bool: {
            bool v = *static_cast<bool*>(p.ptr);
            pg = node_prop_ctrl_->Append(new wxBoolProperty(p.name, wxPG_LABEL, v));
            break;
        }
        case signal_forge::PropType::String: {
            auto& v = *static_cast<std::string*>(p.ptr);
            pg = node_prop_ctrl_->Append(new wxStringProperty(p.name, wxPG_LABEL, wxString::FromUTF8(v.c_str())));
            break;
        }
        case signal_forge::PropType::UInt64: {
            uint64_t v = *static_cast<uint64_t*>(p.ptr);
            wxString s = wxString::Format("%llu", static_cast<unsigned long long>(v));
            pg = node_prop_ctrl_->Append(new wxStringProperty(p.name, wxPG_LABEL, s));
            pg->ChangeFlag(wxPG_PROP_READONLY, true);
            break;
        }
        }

        if (!pg) continue;

        if (p.readOnly)
            pg->ChangeFlag(wxPG_PROP_READONLY, true);

        node_prop_ctrl_grid_to_properties_.emplace(pg, i);
    }

    node_prop_ctrl_->ExpandAll();
}

void MainFrame::WriteInjectNodeFields(signal_forge::InjectNode *inject)
{
    if (!sub_) {
        logger_->warn("WriteInjectNodeFields called with no active subscriber.");
        return;
    }

    uint64_t value_id  = (uint64_t)-1;
    uint64_t enable_id = (uint64_t)-1;

    for (const auto &s : inject->statics) {
        if (s.name == "forced_value")  { value_id  = s.id; }
        if (s.name == "force_enable")  { enable_id = s.id; }
    }

    /* ── Write forced_value ── */
    if (value_id != (uint64_t)-1) {
        dbg_value_t v;
        memset(&v, 0, sizeof(v));
        v.f32 = inject->forced_value;

        dbg_status_t rc = dbg_sub_write(sub_, value_id, DBG_VT_F32, &v);
        if (rc != DBG_OK) {
            logger_->error("dbg_sub_write failed for forced_value "
                           "field_id={} rc={}", value_id, (int)rc);
        } else {
            logger_->debug("forced_value written: {} -> field_id={}",
                           inject->forced_value, value_id);
        }
    } else {
        logger_->warn("InjectNode has no 'forced_value' static.");
    }

    /* ── Write force_enable ── */
    if (enable_id != (uint64_t)-1) {
        dbg_value_t v;
        memset(&v, 0, sizeof(v));
        v.f32 = inject->forcing_active ? 1.0f : 0.0f;

        dbg_status_t rc = dbg_sub_write(sub_, enable_id, DBG_VT_F32, &v);
        if (rc != DBG_OK) {
            logger_->error("dbg_sub_write failed for force_enable "
                           "field_id={} rc={}", enable_id, (int)rc);
        } else {
            logger_->debug("force_enable written: {} -> field_id={}",
                           inject->forcing_active, enable_id);
        }
    } else {
        logger_->warn("InjectNode has no 'force_enable' static.");
    }
}

void MainFrame::OnPropertyGridChanged(wxPropertyGridEvent& e)
{
    wxPGProperty *pg = e.GetProperty();
    if (!pg) return;

    auto it = node_prop_ctrl_grid_to_properties_.find(pg);
    if (it == node_prop_ctrl_grid_to_properties_.end()) return;

    signal_forge::Property &p = node_prop_ctrl_properties_[it->second];
    if (p.readOnly) return;

    signal_forge::Node *node = wxGetApp().get_graph().FindNode(p.node_id);
    if (!node) return;

    const wxVariant &val = e.GetPropertyValue();

    switch (p.type) {

    case signal_forge::PropType::Float: {
        double d = val.GetDouble();
        *static_cast<float *>(p.ptr) = static_cast<float>(d);

        if (p.name == "Value") {
            if (auto *inject = dynamic_cast<signal_forge::InjectNode *>(node)) {
                WriteInjectNodeFields(inject);
            }
        }

        /* Keep UI consistent if value was clamped */
        if (d != val.GetDouble())
            node_prop_ctrl_->ChangePropertyValue(pg, d);
        break;
    }

    case signal_forge::PropType::Int: {
        long l   = val.GetLong();
        double d = static_cast<double>(l);
        int out  = static_cast<int>(d);
        *static_cast<int *>(p.ptr) = out;

        if (out != l)
            node_prop_ctrl_->ChangePropertyValue(pg, out);
        break;
    }

    case signal_forge::PropType::Bool: {
        *static_cast<bool *>(p.ptr) = val.GetBool();

        if (p.name == "Active") {
            if (auto *inject = dynamic_cast<signal_forge::InjectNode *>(node)) {
                WriteInjectNodeFields(inject);
            }
        }
        break;
    }

    case signal_forge::PropType::String: {
        std::string &s = *static_cast<std::string *>(p.ptr);
        s = std::string(val.GetString().ToUTF8());
        break;
    }

    case signal_forge::PropType::UInt64:
        /* Keep read-only for now */
        break;
    }

    canvas_->Refresh();
}

void MainFrame::SetupCppHighlighting(wxStyledTextCtrl* ctrl) {
    ctrl->SetLexer(wxSTC_LEX_CPP);


    wxFont font(10, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL);
    ctrl->StyleSetFont(wxSTC_STYLE_DEFAULT, font);

    ctrl->StyleClearAll(); 

    ctrl->SetKeyWords(0, "int char float double if else while for return void static const struct union typedef enum");


    ctrl->StyleSetForeground(wxSTC_C_COMMENT,       wxColour(0, 128, 0));   // Green comments
    ctrl->StyleSetForeground(wxSTC_C_COMMENTLINE,   wxColour(0, 128, 0));
    ctrl->StyleSetForeground(wxSTC_C_WORD,          wxColour(0, 0, 255));   // Blue keywords
    ctrl->StyleSetBold(wxSTC_C_WORD, true);
    ctrl->StyleSetForeground(wxSTC_C_STRING,        wxColour(163, 21, 21)); // Red strings
    ctrl->StyleSetForeground(wxSTC_C_NUMBER,        wxColour(0, 128, 128)); // Teal numbers
    ctrl->StyleSetForeground(wxSTC_C_PREPROCESSOR,  wxColour(128, 128, 128)); // Grey #defines

    ctrl->SetMarginType(0, wxSTC_MARGIN_NUMBER);
    ctrl->SetMarginWidth(0, 30);

    ctrl->SetReadOnly(true);
}