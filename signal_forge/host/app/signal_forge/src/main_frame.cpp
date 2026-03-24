#include <iostream>
#include <fstream>
#include <cstdlib>

#include "app.h"
#include "main_frame.h"
#include "graph_serialization.h"

#include "task_host_core/interface.h"
#include "task_host_core/manifest.h"
#include "task_host_core/debug.h"

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

void MainFrame::OnGoOnline(wxCommandEvent& WXUNUSED(evt)) {
    logger_->info("Timer started...");

    if (socket_timer_->IsRunning()) {
        socket_timer_->Stop();
    }
    else {
        socket_timer_->Start(250);
    }
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

void MainFrame::OnSocketTimer(wxTimerEvent& WXUNUSED(evt)) {
    auto &graph = wxGetApp().get_graph();

    for (const std::unique_ptr<signal_forge::Node> &node : graph.Nodes()) {
        // Only handle ProbeNode instances
        auto* probe = dynamic_cast<signal_forge::ProbeNode*>(node.get());
        if (!probe) continue;

        // Find the link feeding the probe input[0]
        uint64_t link_id = graph.FindLinkToPin(probe->inputs[0].id);
        if (link_id == (uint64_t)-1) continue;

        auto *link = graph.FindLink(link_id);
        if (!link) continue;

        uint64_t from_pin = link->from_pin;

        // Map pin -> field_id. Replace this with your registry/lookup function.
        // If you don't have a mapping, fall back to using the pin id as field_id.
        uint64_t field_id = from_pin; // implement/replace in your code
        if (field_id == (uint64_t)-1) field_id = from_pin;

        // Build a READ DebugRequest
        DebugRequest req;
        memset(&req, 0, sizeof(req));
        req.tx_id     = 0;
        req.version   = 1;
        req.op        = DBG_OP_READ;
        req.field_id  = field_id;
        req.value_type = DBG_VT_F32;
        req.flags     = 0;
        req.value_len = 0; // READ: no immediate value

        CheckOrMakeConnection(socket_client_);
        socket_client_->Write(&req, sizeof(req));
        if (socket_client_->Error()) {
            logger_->error("Socket write error when requesting field_id={}", (unsigned long long)field_id);
            continue;
        }

        char buffer[sizeof(DebugReply)];
        socket_client_->Read(buffer, sizeof(buffer));
        int bytes_read = socket_client_->LastReadCount();
        if (bytes_read < (int)sizeof(DebugReply)) {
            logger_->error("Incomplete or no reply for field_id={} (read {} bytes)", (unsigned long long)field_id, bytes_read);
            continue;
        }
        socket_client_->Close();

        DebugReply *reply = reinterpret_cast<DebugReply*>(buffer);

        // Basic validation
        if (reply->tx_id != req.tx_id) {
            logger_->warn("Reply tx_id ({}) does not match request ({}) for field_id={}", reply->tx_id, req.tx_id, (unsigned long long)field_id);
            // We still proceed to check field_id/value type as best-effort
        }

        if (reply->field_id != field_id) {
            logger_->warn("Reply field_id ({}) does not match requested field_id ({})", (unsigned long long)reply->field_id, (unsigned long long)field_id);
            // continue; // optional: skip if you require exact match
        }

        if (reply->value_type != DBG_VT_F32) {
            logger_->warn("Unexpected reply type/length for field_id={}: type={}",
                          (unsigned long long)field_id, (int)reply->value_type);
            continue;
        }

        // Update the probe with the returned float value
        probe->value = reply->value.f32;
    }

    // Refresh canvas to reflect updated values
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

void MainFrame::OnPropertyGridChanged(wxPropertyGridEvent& e)
{
    wxPGProperty* pg = e.GetProperty();
    if (!pg) return;

    auto it = node_prop_ctrl_grid_to_properties_.find(pg);
    if (it == node_prop_ctrl_grid_to_properties_.end()) return;

    signal_forge::Property& p = node_prop_ctrl_properties_[it->second];
    if (p.readOnly) return; // Can't have been changed by the user
    signal_forge::Node* node = wxGetApp().get_graph().FindNode(p.node_id);
    if (!node) return; // No parent

    const wxVariant& val = e.GetPropertyValue();

    switch (p.type) {
    case signal_forge::PropType::Float: {
        double d = val.GetDouble();
        d = d;
        *static_cast<float*>(p.ptr) = static_cast<float>(d);
        
        if (p.name == "Value") {
            if (auto* inject = dynamic_cast<signal_forge::InjectNode*>(node)) {
                int value_id = -1;
                int enable_id = -1;

                for (const auto& s : inject->statics) {
                    if (s.name == "forced_value") {
                        value_id = s.id;
                        break;
                    }
                }
                for (const auto& s : inject->statics) {
                    if (s.name == "force_enable") {
                        enable_id = s.id;
                        break;
                    }
                }

                { // Update the VALUE
                    // Build a WRITE DebugRequest
                    DebugRequest req;
                    memset(&req, 0, sizeof(req));
                    req.tx_id     = 0;
                    req.version   = 1;
                    req.op        = DBG_OP_WRITE;
                    req.field_id  = value_id;
                    req.value_type = DBG_VT_F32;
                    req.flags     = 0;
                    req.value_len = 4;
                    req.value.f32 = inject->forced_value; // Value currently set in the UI

                    CheckOrMakeConnection(socket_client_);

                    socket_client_->Write(&req, sizeof(req));
                    if (socket_client_->Error()) {
                        logger_->error("Socket write error when requesting field_id={}", (unsigned long long)value_id);
                    }

                    char buffer[sizeof(DebugReply)];
                    socket_client_->Read(buffer, sizeof(buffer));
                    int bytes_read = socket_client_->LastReadCount();
                    if (bytes_read < (int)sizeof(DebugReply)) {
                        logger_->error("Incomplete or no reply for field_id={} (read {} bytes)", (unsigned long long)value_id, bytes_read);
                    }
                    socket_client_->Close();

                    DebugReply *reply = reinterpret_cast<DebugReply*>(buffer);

                    // Basic validation
                    if (reply->tx_id != req.tx_id) {
                        logger_->warn("Reply tx_id ({}) does not match request ({}) for field_id={}", reply->tx_id, req.tx_id, (unsigned long long)value_id);
                        // We still proceed to check field_id/value type as best-effort
                    }

                    if (reply->field_id != value_id) {
                        logger_->warn("Reply field_id ({}) does not match requested field_id ({})", (unsigned long long)reply->field_id, (unsigned long long)value_id);
                        // continue; // optional: skip if you require exact match
                    }

                    if (reply->value_type != DBG_VT_F32) {
                        logger_->warn("Unexpected reply type/length for field_id={}: type={}",
                                    (unsigned long long)value_id, (int)reply->value_type);
                    }
                    inject->forced_value = reply->value.f32;
                }

                { // Update the FORCE ENABLE
                    // Build a WRITE DebugRequest
                    DebugRequest req;
                    memset(&req, 0, sizeof(req));
                    req.tx_id     = 0;
                    req.version   = 1;
                    req.op        = DBG_OP_WRITE;
                    req.field_id  = enable_id;
                    req.value_type = DBG_VT_F32;
                    req.flags     = 0;
                    req.value_len = 4;
                    req.value.f32 = (inject->forcing_active) ? 1.0f : 0.0f; // Value currently set in the UI

                    CheckOrMakeConnection(socket_client_);

                    socket_client_->Write(&req, sizeof(req));
                    if (socket_client_->Error()) {
                        logger_->error("Socket write error when requesting field_id={}", (unsigned long long)enable_id);
                    }

                    char buffer[sizeof(DebugReply)];
                    socket_client_->Read(buffer, sizeof(buffer));
                    int bytes_read = socket_client_->LastReadCount();
                    if (bytes_read < (int)sizeof(DebugReply)) {
                        logger_->error("Incomplete or no reply for field_id={} (read {} bytes)", (unsigned long long)enable_id, bytes_read);
                    }
                    socket_client_->Close();

                    DebugReply *reply = reinterpret_cast<DebugReply*>(buffer);

                    // Basic validation
                    if (reply->tx_id != req.tx_id) {
                        logger_->warn("Reply tx_id ({}) does not match request ({}) for field_id={}", reply->tx_id, req.tx_id, (unsigned long long)enable_id);
                        // We still proceed to check field_id/value type as best-effort
                    }

                    if (reply->field_id != enable_id) {
                        logger_->warn("Reply field_id ({}) does not match requested field_id ({})", (unsigned long long)reply->field_id, (unsigned long long)enable_id);
                        // continue; // optional: skip if you require exact match
                    }

                    if (reply->value_type != DBG_VT_F32) {
                        logger_->warn("Unexpected reply type/length for field_id={}: type={}",
                                    (unsigned long long)enable_id, (int)reply->value_type);
                    }
                    inject->forcing_active = (reply->value.f32 != 0.0f) ? true : false;
                }
            }
        }

        // Keep UI consistent if clamped
        if (d != val.GetDouble())
            node_prop_ctrl_->ChangePropertyValue(pg, d);
        break;
    }
    case signal_forge::PropType::Int: {
        long l = val.GetLong();
        double d = static_cast<double>(l);
        int out = static_cast<int>(d);
        *static_cast<int*>(p.ptr) = out;

        if (out != l)
            node_prop_ctrl_->ChangePropertyValue(pg, out);
        break;
    }
    case signal_forge::PropType::Bool: {
        *static_cast<bool*>(p.ptr) = val.GetBool();
        if (p.name == "Active") {
            if (auto* inject = dynamic_cast<signal_forge::InjectNode*>(node)) {
                int value_id = -1;
                int enable_id = -1;

                for (const auto& s : inject->statics) {
                    if (s.name == "forced_value") {
                        value_id = s.id;
                        break;
                    }
                }
                for (const auto& s : inject->statics) {
                    if (s.name == "force_enable") {
                        enable_id = s.id;
                        break;
                    }
                }

                { // Update the VALUE
                    // Build a WRITE DebugRequest
                    DebugRequest req;
                    memset(&req, 0, sizeof(req));
                    req.tx_id     = 0;
                    req.version   = 1;
                    req.op        = DBG_OP_WRITE;
                    req.field_id  = value_id;
                    req.value_type = DBG_VT_F32;
                    req.flags     = 0;
                    req.value_len = 4;
                    req.value.f32 = inject->forced_value; // Value currently set in the UI

                    CheckOrMakeConnection(socket_client_);

                    socket_client_->Write(&req, sizeof(req));
                    if (socket_client_->Error()) {
                        logger_->error("Socket write error when requesting field_id={}", (unsigned long long)value_id);
                    }

                    char buffer[sizeof(DebugReply)];
                    socket_client_->Read(buffer, sizeof(buffer));
                    int bytes_read = socket_client_->LastReadCount();
                    if (bytes_read < (int)sizeof(DebugReply)) {
                        logger_->error("Incomplete or no reply for field_id={} (read {} bytes)", (unsigned long long)value_id, bytes_read);
                    }

                    DebugReply *reply = reinterpret_cast<DebugReply*>(buffer);

                    // Basic validation
                    if (reply->tx_id != req.tx_id) {
                        logger_->warn("Reply tx_id ({}) does not match request ({}) for field_id={}", reply->tx_id, req.tx_id, (unsigned long long)value_id);
                        // We still proceed to check field_id/value type as best-effort
                    }

                    if (reply->field_id != value_id) {
                        logger_->warn("Reply field_id ({}) does not match requested field_id ({})", (unsigned long long)reply->field_id, (unsigned long long)value_id);
                        // continue; // optional: skip if you require exact match
                    }

                    if (reply->value_type != DBG_VT_F32) {
                        logger_->warn("Unexpected reply type/length for field_id={}: type={}",
                                    (unsigned long long)value_id, (int)reply->value_type);
                    }
                    inject->forced_value = reply->value.f32;
                    socket_client_->Close();
                }

                { // Update the FORCE ENABLE
                    // Build a WRITE DebugRequest
                    DebugRequest req;
                    memset(&req, 0, sizeof(req));
                    req.tx_id     = 0;
                    req.version   = 1;
                    req.op        = DBG_OP_WRITE;
                    req.field_id  = enable_id;
                    req.value_type = DBG_VT_F32;
                    req.flags     = 0;
                    req.value_len = 4;
                    req.value.f32 = (inject->forcing_active) ? 1.0f : 0.0f; // Value currently set in the UI

                    CheckOrMakeConnection(socket_client_);

                    socket_client_->Write(&req, sizeof(req));
                    if (socket_client_->Error()) {
                        logger_->error("Socket write error when requesting field_id={}", (unsigned long long)enable_id);
                    }

                    char buffer[sizeof(DebugReply)];
                    socket_client_->Read(buffer, sizeof(buffer));
                    int bytes_read = socket_client_->LastReadCount();
                    if (bytes_read < (int)sizeof(DebugReply)) {
                        logger_->error("Incomplete or no reply for field_id={} (read {} bytes)", (unsigned long long)enable_id, bytes_read);
                    }

                    DebugReply *reply = reinterpret_cast<DebugReply*>(buffer);

                    // Basic validation
                    if (reply->tx_id != req.tx_id) {
                        logger_->warn("Reply tx_id ({}) does not match request ({}) for field_id={}", reply->tx_id, req.tx_id, (unsigned long long)enable_id);
                        // We still proceed to check field_id/value type as best-effort
                    }

                    if (reply->field_id != enable_id) {
                        logger_->warn("Reply field_id ({}) does not match requested field_id ({})", (unsigned long long)reply->field_id, (unsigned long long)enable_id);
                        // continue; // optional: skip if you require exact match
                    }

                    if (reply->value_type != DBG_VT_F32) {
                        logger_->warn("Unexpected reply type/length for field_id={}: type={}",
                                    (unsigned long long)enable_id, (int)reply->value_type);
                    }
                    inject->forcing_active = (reply->value.f32 != 0.0f) ? true : false;
                }
            }
        }
        break;
    }
    case signal_forge::PropType::String: {
        std::string& s = *static_cast<std::string*>(p.ptr);
        s = std::string(val.GetString().ToUTF8());
        break;
    }
    case signal_forge::PropType::UInt64:
        // If you want editable uint64, parse and validate here.
        // Otherwise keep it read-only.
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