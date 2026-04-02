#include <iostream>
#include <fstream>
#include <cstdlib>

#include "app.h"
#include "main_frame.h"
#include "graph_serialization.h"
#include "sf_protocol_client.h"

#include "task_host_core/interface.h"
#include "task_host_core/manifest.h"

const int ID_NODE_CANVAS_GENERATE             = wxWindow::NewControlId();
const int ID_NODE_CANVAS_COMPILE              = wxWindow::NewControlId();
const int ID_NODE_CANVAS_TRANSFER             = wxWindow::NewControlId();
const int ID_GO_ONLINE                        = wxWindow::NewControlId();
const int ID_MENU_VIEW_NODE_PROPERTIES_CHECK  = wxWindow::NewControlId();
const int ID_MENU_VIEW_C_SOURCE_CHECK         = wxWindow::NewControlId();
const int ID_MENU_VIEW_LOG_CHECK              = wxWindow::NewControlId();
const int ID_MENU_VIEW_PROJECT_CHECK          = wxWindow::NewControlId();
const int ID_COMPILE_TIMER                    = wxWindow::NewControlId();
const int ID_SOCKET_TIMER                     = wxWindow::NewControlId();

MainFrame::MainFrame()
    : wxFrame(nullptr, wxID_ANY, "Signal Forge", wxDefaultPosition, wxSize(1280, 800))
{
    aui_mgr_.SetManagedWindow(this);
    aui_mgr_.SetFlags(
        wxAUI_MGR_RECTANGLE_HINT |
        wxAUI_MGR_LIVE_RESIZE |
        wxAUI_MGR_ALLOW_ACTIVE_PANE |
        wxAUI_MGR_ALLOW_FLOATING
    );

    editor_notebook_ = new wxAuiNotebook(
        this,
        wxID_ANY,
        wxDefaultPosition,
        wxDefaultSize,
        wxAUI_NB_DEFAULT_STYLE |
        wxAUI_NB_TAB_MOVE |
        wxAUI_NB_CLOSE_ON_ACTIVE_TAB |
        wxAUI_NB_SCROLL_BUTTONS |
        wxAUI_NB_TAB_SPLIT
    );

    editor_notebook_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &MainFrame::OnEditorPageChanged, this);
    editor_notebook_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &MainFrame::OnEditorPageClose, this);

    log_ctrl_ = new wxTextCtrl(
        this,
        wxID_ANY,
        wxEmptyString,
        wxDefaultPosition,
        wxDefaultSize,
        wxTE_MULTILINE | wxTE_READONLY
    );

    log_sink_ = std::make_shared<wxTextCtrlSink>(log_ctrl_);
    logger_ = std::make_shared<spdlog::logger>("Signal Forge", log_sink_);
    logger_->set_level(spdlog::level::trace);

    c_source_ctrl_ = new wxStyledTextCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize);
    SetupCppHighlighting(c_source_ctrl_);

    node_prop_ctrl_ = new wxPropertyGrid(
        this,
        wxID_ANY,
        wxDefaultPosition,
        wxSize(300, -1),
        wxPG_BOLD_MODIFIED | wxPG_SPLITTER_AUTO_CENTER
    );
    node_prop_ctrl_->Bind(wxEVT_PG_CHANGED, &MainFrame::OnPropertyGridChanged, this);

    project_tree_ctrl_ = new wxTreeCtrl(
        this,
        wxID_ANY,
        wxDefaultPosition,
        wxDefaultSize,
        wxTR_HAS_BUTTONS |
        wxTR_LINES_AT_ROOT |
        wxTR_SINGLE
    );
    project_tree_ctrl_->Bind(wxEVT_TREE_ITEM_ACTIVATED, &MainFrame::OnProjectTreeItemActivated, this);

    aui_mgr_.AddPane(
        project_tree_ctrl_,
        wxAuiPaneInfo()
            .Name("Project")
            .Left()
            .Caption("Project")
            .MinSize(250, -1)
            .BestSize(280, -1)
    );

    aui_mgr_.AddPane(
        editor_notebook_,
        wxAuiPaneInfo()
            .Name("Editors")
            .CenterPane()
            .Caption("Editors")
    );

    aui_mgr_.AddPane(
        node_prop_ctrl_,
        wxAuiPaneInfo()
            .Name("Properties")
            .Right()
            .Caption("Properties")
    );

    aui_mgr_.AddPane(
        c_source_ctrl_,
        wxAuiPaneInfo()
            .Name("Source")
            .Right()
            .Caption("Source")
            .Hide()
    );

    aui_mgr_.AddPane(
        log_ctrl_,
        wxAuiPaneInfo()
            .Name("Log")
            .Bottom()
            .Caption("Log")
            .MinSize(-1, 150)
    );

    aui_mgr_.Update();

    auto* menuBar   = new wxMenuBar();
    auto* fileMenu  = new wxMenu();
    auto* buildMenu = new wxMenu();
    auto* viewMenu  = new wxMenu();

    fileMenu->Append(wxID_NEW,    "New\tCtrl+N");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_OPEN,   "Open Project\tCtrl+O");
    fileMenu->Append(wxID_SAVE,   "Save\tCtrl+S");
    fileMenu->Append(wxID_SAVEAS, "Save As\tCtrl+Shift+S");
    fileMenu->AppendSeparator();
    fileMenu->Append(wxID_EXIT,   "Exit\tAlt+F4");

    buildMenu->Append(ID_NODE_CANVAS_GENERATE, "Generate\tF5");
    buildMenu->Append(ID_NODE_CANVAS_COMPILE,  "Compile\tF6");
    buildMenu->Append(ID_NODE_CANVAS_TRANSFER, "Transfer\tF7");
    buildMenu->AppendSeparator();
    buildMenu->Append(ID_GO_ONLINE,            "Go Online\tF8");

    AddViewToggle(viewMenu, ID_MENU_VIEW_PROJECT_CHECK,         "Project",          "Project");
    AddViewToggle(viewMenu, ID_MENU_VIEW_NODE_PROPERTIES_CHECK, "Node Properties",  "Properties");
    AddViewToggle(viewMenu, ID_MENU_VIEW_C_SOURCE_CHECK,        "Generated Source", "Source");
    AddViewToggle(viewMenu, ID_MENU_VIEW_LOG_CHECK,             "Log",              "Log");

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
    Bind(wxEVT_MENU, &MainFrame::OnSaveAs,   this, wxID_SAVEAS);
    Bind(wxEVT_MENU, &MainFrame::OnExit,     this, wxID_EXIT);

    socket_client_ = new wxSocketClient();
    socket_timer_ = new wxTimer(this, ID_SOCKET_TIMER);
    Bind(wxEVT_TIMER, &MainFrame::OnSocketTimer, this, ID_SOCKET_TIMER);

    Bind(EVT_NODE_SELECTED, &MainFrame::OnNodeSelected, this);
    Bind(EVT_NODE_PROPERTY_CHANGED, &MainFrame::OnGraphModified, this);
}

MainFrame::~MainFrame() {
    aui_mgr_.UnInit();
}

bool MainFrame::LoadProjectManifest(const wxString& projectFilePath)
{
    std::ifstream in(projectFilePath.ToStdString());
    if (!in.is_open())
    {
        wxMessageBox("Unable to open project file: " + projectFilePath,
                     "Error", wxICON_ERROR | wxOK, this);
        return false;
    }

    try
    {
        std::string content(
            (std::istreambuf_iterator<char>(in)),
             std::istreambuf_iterator<char>());
        in.close();

        auto jv = boost::json::parse(content);
        const auto& obj = jv.as_object();

        m_projectManifest.block_defs.clear();
        m_projectManifest.tasks.clear();

        if (auto* defs = obj.if_contains("block_defs"))
        {
            for (const auto& v : defs->as_array())
                m_projectManifest.block_defs.push_back(v.as_string().c_str());
        }

        if (auto* tasks = obj.if_contains("tasks"))
        {
            for (const auto& v : tasks->as_array())
                m_projectManifest.tasks.push_back(v.as_string().c_str());
        }

        m_currentProjectFile = projectFilePath;
        wxFileName fn(projectFilePath);
        m_currentProjectPath = fn.GetPath();

        return true;
    }
    catch (const std::exception& e)
    {
        wxMessageBox(
            wxString::Format("Failed to parse project file: %s", e.what()),
            "Error", wxICON_ERROR | wxOK, this);
        return false;
    }
}

void MainFrame::PopulateProjectTree()
{
    if (!project_tree_ctrl_)
        return;

    project_tree_ctrl_->DeleteAllItems();

    wxFileName projectPath(m_currentProjectPath);
    const wxString projectName = projectPath.GetFullName();

    wxTreeItemId rootId = project_tree_ctrl_->AddRoot(
        projectName,
        -1,
        -1,
        new ProjectTreeItemData(ProjectTreeItemData::ItemType::Folder, m_currentProjectPath)
    );

    wxTreeItemId blockDefsId = project_tree_ctrl_->AppendItem(
        rootId,
        "Block Definitions",
        -1,
        -1,
        new ProjectTreeItemData(ProjectTreeItemData::ItemType::Folder)
    );

    wxTreeItemId tasksId = project_tree_ctrl_->AppendItem(
        rootId,
        "Tasks",
        -1,
        -1,
        new ProjectTreeItemData(ProjectTreeItemData::ItemType::Folder)
    );

    for (const auto& relPath : m_projectManifest.block_defs)
    {
        wxFileName fullPath(m_currentProjectPath, relPath);
        project_tree_ctrl_->AppendItem(
            blockDefsId,
            wxFileName(relPath).GetFullName(),
            -1,
            -1,
            new ProjectTreeItemData(
                ProjectTreeItemData::ItemType::BlockDefinitionFile,
                fullPath.GetFullPath()
            )
        );
    }

    for (const auto& relPath : m_projectManifest.tasks)
    {
        wxFileName fullPath(m_currentProjectPath, relPath);
        project_tree_ctrl_->AppendItem(
            tasksId,
            wxFileName(relPath).GetFullName(),
            -1,
            -1,
            new ProjectTreeItemData(
                ProjectTreeItemData::ItemType::TaskFile,
                fullPath.GetFullPath()
            )
        );
    }

    project_tree_ctrl_->Expand(rootId);
    project_tree_ctrl_->Expand(blockDefsId);
    project_tree_ctrl_->Expand(tasksId);
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
//    const int pageIndex = editor_notebook_->GetSelection();
//    if (pageIndex == wxNOT_FOUND)
//        return;
//
//    wxWindow* page = editor_notebook_->GetPage(pageIndex);
//    if (!page)
//        return;
//
//    GraphDocument* doc = FindDocumentByPage(page);
//    if (!doc)
//        return;
//    
//    signal_forge::Generator gen(*doc->graph);
//    doc->result_ = gen.generate();
//    std::ofstream outfile("output.c");
//    outfile << doc->result_.c_source;
//    if (c_source_ctrl_) {
//        c_source_ctrl_->SetReadOnly(false);
//        c_source_ctrl_->SetText(doc->result_.c_source);
//        c_source_ctrl_->SetReadOnly(true);
//    }
//    outfile.close();
}

void MainFrame::OnCompile(wxCommandEvent& WXUNUSED(evt)) {
//    logger_->info("Starting compilation...");
//
//    // Build the command (Same as before, but without the "echo" redirection string)
//    wxString timestamp = wxDateTime::Now().Format("%Y%m%d_%H%M%S");
//    std::string output = "libmy_task_" + timestamp.ToStdString() + ".so";
//
//    std::string zig_path = wxGetApp().get_config().zig.string();
//    std::string command = "\"" + zig_path + "\" cc -v -v -target x86_64-linux-gnu -shared -fPIC ";
//    command += "-o " + output + " output.c";
//    command += " \"-IC:/Users/LOAR02/Source/signal/signal_forge/target/libs/task_host_core/include\"";
//
//    for (const std::string& header : wxGetApp().get_generation_result().link_headers) {
//        size_t last_slash = header.find_last_of("/\\");
//        if (last_slash != std::string::npos) {
//            command += " -I\"" + header.substr(0, last_slash) + "\"";
//        }
//    }
//
//    for (const std::string& obj_path : wxGetApp().get_generation_result().link_objects) {
//        command += " \"" + obj_path + "\"";
//    }
//
//    auto* process = new ExternalProcess(logger_,
//        [output](int status, std::shared_ptr<spdlog::logger> logger) {
//            if (status == 0) {
//                logger->info("Compilation successful!");
//                wxGetApp().set_compiled_filename(output);
//            } else {
//                logger->critical("Compilation failure with code {}", status);
//            }
//        }
//    );
//
//    if (wxExecute(command, wxEXEC_ASYNC, process) == 0) {
//        logger_->error("Failed to launch compile process.");
//        delete process;
//    }
}

void MainFrame::OnTransfer(wxCommandEvent& WXUNUSED(evt)) {
//    const std::string compiled_file = wxGetApp().get_compiled_filename();
//    if (compiled_file.empty()) {
//        logger_->info("Nothing to transfer...");
//        return;
//    }
//
//    // ── Read the .so file ─────────────────────────────────────────
//    std::ifstream file(compiled_file, std::ios::binary | std::ios::ate);
//    if (!file.is_open()) {
//        logger_->error("Cannot open file: {}", compiled_file);
//        return;
//    }
//
//    const std::streamsize file_size = file.tellg();
//    file.seekg(0, std::ios::beg);
//
//    std::vector<uint8_t> file_data(static_cast<size_t>(file_size));
//    if (!file.read(reinterpret_cast<char*>(file_data.data()), file_size)) {
//        logger_->error("Failed to read file: {}", compiled_file);
//        return;
//    }
//    file.close();
//
//    logger_->info("Starting transfer: {} ({} bytes)",
//                  compiled_file, file_size);
//
//    // ── Compute CRC-32 of the file ────────────────────────────────
//    const uint32_t file_crc = SfProtocolClient::crc32(
//        file_data.data(), file_data.size());
//
//    logger_->info("File CRC-32: 0x{:08X}", file_crc);
//
//    // ── Run transfer on a worker thread — never block the UI ──────
//    std::shared_ptr<spdlog::logger> logger  = logger_;
//    int   slot    = 0;//wxGetApp().get_target_slot();    // which task slot to swap //TODO: Allow multiple tasks
//    const std::string host     = "127.0.0.1";//wxGetApp().get_target_host();
//    const uint16_t    port     = 7600;//wxGetApp().get_target_port();
//    const std::string filename = std::filesystem::path(compiled_file)
//                                     .filename().string();
//
//    std::thread([=, data = std::move(file_data)]() mutable {
//
//        SfProtocolClient client(logger);
//
//        // ── Connect ───────────────────────────────────────────────
//        if (!client.connect(host, port)) {
//            logger->error("Transfer failed: cannot connect to "
//                          "{}:{}", host, port);
//            return;
//        }
//
//        std::string error;
//
//        // ── SF_CMD_TRANSFER_BEGIN ─────────────────────────────────
//        sf_transfer_begin_t begin{};
//        strncpy(begin.filename, filename.c_str(),
//                sizeof(begin.filename) - 1);
//        begin.target_slot = static_cast<uint8_t>(slot);
//        begin.total_size  = static_cast<uint32_t>(data.size());
//        begin.crc32       = file_crc;
//
//        logger->info("[transfer] → TRANSFER_BEGIN  file={}  "
//                     "size={}  slot={}  crc=0x{:08X}",
//                     filename, data.size(), slot, file_crc);
//
//        if (!client.send_packet(SF_CMD_TRANSFER_BEGIN,
//                                &begin, sizeof(begin))) {
//            logger->error("[transfer] failed to send TRANSFER_BEGIN");
//            return;
//        }
//
//        if (!client.wait_ack(SF_CMD_TRANSFER_ACK,
//                             SF_CMD_TRANSFER_NACK, error)) {
//            logger->error("[transfer] TRANSFER_BEGIN rejected: {}", error);
//            return;
//        }
//
//        // ── SF_CMD_TRANSFER_CHUNK — send file in chunks ───────────
//        constexpr size_t CHUNK_SIZE = SF_MAX_CHUNK_SIZE;
//        size_t           offset     = 0;
//        size_t           chunk_num  = 0;
//        size_t           total_chunks =
//            (data.size() + CHUNK_SIZE - 1) / CHUNK_SIZE;
//
//        while (offset < data.size()) {
//            size_t this_chunk = std::min(CHUNK_SIZE,
//                                          data.size() - offset);
//
//            // Build chunk packet — header fields + inline data
//            // We send header and data separately to avoid copying
//            // into the fixed-size sf_transfer_chunk_t::data array
//            sf_transfer_chunk_t chunk{};
//            chunk.offset = static_cast<uint32_t>(offset);
//            chunk.length = static_cast<uint16_t>(this_chunk);
//
//            // Send header part of the chunk struct
//            // then the data slice directly from the file buffer.
//            // Use a single allocation to keep it one send call.
//            const size_t chunk_payload_size =
//                offsetof(sf_transfer_chunk_t, data) + this_chunk;
//
//            std::vector<uint8_t> chunk_payload(chunk_payload_size);
//            memcpy(chunk_payload.data(), &chunk,
//                   offsetof(sf_transfer_chunk_t, data));
//            memcpy(chunk_payload.data() +
//                   offsetof(sf_transfer_chunk_t, data),
//                   data.data() + offset, this_chunk);
//
//            if (!client.send_packet(SF_CMD_TRANSFER_CHUNK,
//                                    chunk_payload.data(),
//                                    static_cast<uint32_t>(
//                                        chunk_payload_size))) {
//                logger->error("[transfer] failed to send chunk {} of {}",
//                              chunk_num + 1, total_chunks);
//                return;
//            }
//
//            if (!client.wait_ack(SF_CMD_TRANSFER_ACK,
//                                 SF_CMD_TRANSFER_NACK, error)) {
//                logger->error("[transfer] chunk {} rejected: {}",
//                              chunk_num + 1, error);
//                return;
//            }
//
//            offset    += this_chunk;
//            chunk_num++;
//
//            // Progress log every 10% 
//            if (total_chunks >= 10 &&
//                chunk_num % (total_chunks / 10) == 0) {
//                logger->info("[transfer] {}% ({}/{}  bytes={}/{})",
//                             (chunk_num * 100) / total_chunks,
//                             chunk_num, total_chunks,
//                             offset, data.size());
//            }
//        }
//
//        // ── SF_CMD_TRANSFER_END ───────────────────────────────────
//        logger->info("[transfer] → TRANSFER_END");
//
//        if (!client.send_packet(SF_CMD_TRANSFER_END)) {
//            logger->error("[transfer] failed to send TRANSFER_END");
//            return;
//        }
//
//        if (!client.wait_ack(SF_CMD_TRANSFER_ACK,
//                             SF_CMD_TRANSFER_NACK, error)) {
//            logger->error("[transfer] TRANSFER_END rejected: {}", error);
//            return;
//        }
//
//        logger->info("[transfer] complete — {} bytes transferred, "
//                     "CRC verified, slot {} will swap",
//                     data.size(), slot);
//
//        client.disconnect();
//
//    }).detach();   // detach — progress is reported via logger
}

void MainFrame::OnGoOnline(wxCommandEvent& WXUNUSED(evt))
{
//    auto &graph = wxGetApp().get_graph();
//
//    /*
//     * Toggle behaviour: if already running, go offline.
//     */
//    if (socket_timer_->IsRunning()) {
//        socket_timer_->Stop();
//        logger_->info("Went offline — timer stopped.");
//
//        if (sub_) {
//            dbg_sub_destroy(sub_);
//            sub_ = nullptr;
//        }
//
//        probe_field_map_.clear();
//        inject_field_map_.clear();
//        return;
//    }
//
//    /*
//     * Build field_ids and types arrays by walking the graph.
//     * Both ProbeNode (reads its input) and InjectNode (monitors its
//     * output) participate as read-only subscribers here.
//     */
//    std::vector<uint64_t>          field_ids;
//    std::vector<dbg_value_type_t>  types;
//
//    probe_field_map_.clear();
//    inject_field_map_.clear();
//
//    for (const std::unique_ptr<signal_forge::Node> &node : graph.Nodes()) {
//        // TODO: keep probenodes separate if they are being driven from the same output pin
//
//        /* ── ProbeNode: field_id comes from the pin driving input[0] ── */
//        if (auto *probe = dynamic_cast<signal_forge::ProbeNode*>(node.get())) {
//            uint64_t link_id = graph.FindLinkToPin(probe->inputs[0].id);
//            if (link_id == (uint64_t)-1) continue;
//
//            auto *link = graph.FindLink(link_id);
//            if (!link) continue;
//
//            uint64_t field_id = link->from_pin; /* pin id == field_id */
//
//            field_ids.push_back(field_id);
//            types.push_back(DBG_VT_F32);
//            probe_field_map_[field_id] = probe;
//
//            logger_->info("Subscribing ProbeNode field_id={}", field_id);
//            continue;
//        }
//
//        /* ── InjectNode: field_id comes from its own output pin ── */
//        if (auto *inject = dynamic_cast<signal_forge::InjectNode*>(node.get())) {
//            if (inject->outputs.empty()) continue;
//
//            uint64_t field_id = inject->outputs[0].id;
//
//            field_ids.push_back(field_id);
//            types.push_back(DBG_VT_F32);
//            inject_field_map_[field_id] = inject;
//
//            logger_->info("Subscribing InjectNode field_id={}", field_id);
//            continue;
//        }
//    }
//
//    if (field_ids.empty()) {
//        logger_->warn("No subscribable nodes found in graph — not going online.");
//        return;
//    }
//
//    auto count = static_cast<uint16_t>(field_ids.size());
//
//    /* ── Create subscriber ── */
//    cfg_             = DBG_SUB_CONFIG_DEFAULT;
//    cfg_.host        = "127.0.0.1";
//    cfg_.data_port   = 9500;
//    cfg_.config_port = 9501;
//
//    sub_ = dbg_sub_create(&cfg_);
//    if (!sub_) {
//        logger_->error("Failed to create subscriber.");
//        return;
//    }
//
//    /* ── Subscribe — let the publisher assign an ID ── */
//    dbg_status_t rc = dbg_sub_subscribe(
//        sub_,
//        DBG_SUB_ID_AUTO,
//        field_ids.data(),
//        types.data(),
//        count,
//        250'000,      /* interval_us */
//        &layout_);
//
//    if (rc != DBG_OK) {
//        logger_->error("dbg_sub_subscribe failed: {}", (int)rc);
//        dbg_sub_destroy(sub_);
//        sub_ = nullptr;
//        return;
//    }
//
//    /*
//     * Capture the effective sub_id assigned by the publisher.
//     * This must be used for all future poll/unsubscribe calls.
//     */
//    effective_sub_id_ = layout_.sub_id;
//    logger_->info("Subscribed OK — assigned sub_id={}, {} field(s), "
//                  "frame_size={}, actual_interval={}us",
//                  effective_sub_id_,
//                  layout_.field_count,
//                  layout_.frame_size,
//                  layout_.actual_interval_us);
//
//    /* ── Start the poll timer to match the subscription interval ── */
//    constexpr int kTimerMs = 250;
//    socket_timer_->Start(kTimerMs);
//    logger_->info("Went online — timer started ({} ms).", kTimerMs);
}

wxString MainFrame::MakeProjectRelativePath(const wxString& absolutePath) const
{
    if (m_currentProjectPath.IsEmpty())
        return absolutePath;

    wxFileName fileName(absolutePath);
    fileName.MakeRelativeTo(m_currentProjectPath);
    return fileName.GetFullPath();
}

bool MainFrame::ManifestContainsPath(const std::vector<wxString>& entries, const wxString& relPath) const
{
    return std::find(entries.begin(), entries.end(), relPath) != entries.end();
}

bool MainFrame::EnsureDocumentInProjectManifest(GraphDocument* doc)
{
    if (!doc)
        return false;

    if (m_currentProjectPath.IsEmpty() || m_currentProjectFile.IsEmpty())
        return false;

    const wxString relPath = MakeProjectRelativePath(doc->file_path);

    bool added = false;

    switch (doc->type)
    {
    case GraphDocument::DocumentType::BlockDefinition:
        if (!ManifestContainsPath(m_projectManifest.block_defs, relPath))
        {
            m_projectManifest.block_defs.push_back(relPath);
            added = true;
        }
        break;

    case GraphDocument::DocumentType::Task:
        if (!ManifestContainsPath(m_projectManifest.tasks, relPath))
        {
            m_projectManifest.tasks.push_back(relPath);
            added = true;
        }
        break;

    default:
        break;
    }

    if (added)
    {
        if (!SaveProjectManifest())
            return false;

        PopulateProjectTree();
    }

    return true;
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

bool MainFrame::SaveProjectManifest()
{
    if (m_currentProjectFile.IsEmpty())
        return false;

    try
    {
        boost::json::object obj;

        boost::json::array blockDefs;
        for (const auto& path : m_projectManifest.block_defs)
            blockDefs.emplace_back(path.ToStdString());

        boost::json::array tasks;
        for (const auto& path : m_projectManifest.tasks)
            tasks.emplace_back(path.ToStdString());

        obj["block_defs"] = std::move(blockDefs);
        obj["tasks"] = std::move(tasks);

        std::ofstream out(m_currentProjectFile.ToStdString());
        if (!out.is_open())
        {
            wxMessageBox("Unable to save project file: " + m_currentProjectFile,
                         "Error", wxICON_ERROR | wxOK, this);
            return false;
        }

        std::string indent("  ");
        pretty_print(out, boost::json::value(obj), &indent);
        out.close();

        return true;
    }
    catch (const std::exception& e)
    {
        wxMessageBox(
            wxString::Format("Failed to save project file: %s", e.what()),
            "Error", wxICON_ERROR | wxOK, this);
        return false;
    }
}

void MainFrame::OnOpen(wxCommandEvent& WXUNUSED(evt))
{
    wxFileDialog openDialog(
        this,
        "Open Project",
        "",
        "project.json",
        "Project Files (project.json)|project.json|JSON Files (*.json)|*.json",
        wxFD_OPEN | wxFD_FILE_MUST_EXIST
    );

    if (openDialog.ShowModal() == wxID_CANCEL)
        return;

    if (LoadProjectManifest(openDialog.GetPath()))
        PopulateProjectTree();
}

bool MainFrame::SaveDocument(GraphDocument* doc)
{
    if (!doc)
        return false;

    if (doc->file_path.IsEmpty())
        return SaveDocumentAs(doc);

    try
    {
        auto jv = signal_forge::serialize_graph(*doc->graph);

        std::ofstream out(doc->file_path.ToStdString());
        if (!out.is_open())
        {
            wxMessageBox("Unable to save file: " + doc->file_path,
                         "Error", wxICON_ERROR | wxOK, this);
            return false;
        }

        std::string indent("  ");
        pretty_print(out, jv, &indent);
        out.close();

        SetDocumentDirty(doc, false);
        UpdateDocumentTabTitle(doc);
        return true;
    }
    catch (const std::exception& e)
    {
        wxMessageBox(
            wxString::Format("Failed to save graph file: %s", e.what()),
            "Error", wxICON_ERROR | wxOK, this);
        return false;
    }
}

bool MainFrame::SaveDocumentAs(GraphDocument* doc)
{
    if (!doc)
        return false;

    wxString initialDir;
    wxString initialFile = doc->display_name;

    if (!doc->file_path.IsEmpty())
    {
        wxFileName fn(doc->file_path);
        initialDir = fn.GetPath();
        initialFile = fn.GetFullName();
    }

    wxFileDialog saveDialog(
        this,
        "Save Graph As",
        initialDir,
        initialFile,
        "Graph Files (*.json)|*.json|All Files (*.*)|*.*",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT
    );

    if (saveDialog.ShowModal() == wxID_CANCEL)
        return false;

    const wxString oldPath = doc->file_path;
    const wxString newPath = saveDialog.GetPath();

    try
    {
        auto jv = signal_forge::serialize_graph(*doc->graph);

        std::ofstream out(newPath.ToStdString());
        if (!out.is_open())
        {
            wxMessageBox("Unable to save file: " + newPath,
                         "Error", wxICON_ERROR | wxOK, this);
            return false;
        }

        std::string indent("  ");
        pretty_print(out, jv, &indent);
        out.close();

        doc->file_path = newPath;
        doc->display_name = wxFileName(newPath).GetFullName();

        SetDocumentDirty(doc, false);
        UpdateDocumentTabTitle(doc);

        const wxString relPath = MakeProjectRelativePath(newPath);

        bool alreadyInManifest = false;
        switch (doc->type)
        {
        case GraphDocument::DocumentType::BlockDefinition:
            alreadyInManifest = ManifestContainsPath(m_projectManifest.block_defs, relPath);
            break;

        case GraphDocument::DocumentType::Task:
            alreadyInManifest = ManifestContainsPath(m_projectManifest.tasks, relPath);
            break;
        }

        if (!alreadyInManifest)
        {
            if (!EnsureDocumentInProjectManifest(doc))
                return false;
        }

        return true;
    }
    catch (const std::exception& e)
    {
        wxMessageBox(
            wxString::Format("Failed to save graph file: %s", e.what()),
            "Error", wxICON_ERROR | wxOK, this);
        return false;
    }
}

void MainFrame::OnSave(wxCommandEvent& WXUNUSED(evt))
{
    const int pageIndex = editor_notebook_->GetSelection();
    if (pageIndex == wxNOT_FOUND)
        return;

    wxWindow* page = editor_notebook_->GetPage(pageIndex);
    if (!page)
        return;

    GraphDocument* doc = FindDocumentByPage(page);
    if (!doc)
        return;

    SaveDocument(doc);
}

void MainFrame::OnSaveAs(wxCommandEvent& WXUNUSED(evt))
{
    const int pageIndex = editor_notebook_->GetSelection();
    if (pageIndex == wxNOT_FOUND)
        return;

    wxWindow* page = editor_notebook_->GetPage(pageIndex);
    if (!page)
        return;

    GraphDocument* doc = FindDocumentByPage(page);
    if (!doc)
        return;

    SaveDocumentAs(doc);
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
        addr.Service(7600);

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
}

void MainFrame::OpenGraphDocument(const wxString& filePath)
{
    for (size_t i = 0; i < open_documents_.size(); ++i)
    {
        if (open_documents_[i]->file_path == filePath)
        {
            const int pageIndex = editor_notebook_->GetPageIndex(open_documents_[i]->panel);
            if (pageIndex != wxNOT_FOUND)
                editor_notebook_->SetSelection(pageIndex);
                m_currentGraphFilePath = filePath;
            return;
        }
    }

    std::ifstream in(filePath.ToStdString());
    if (!in.is_open())
    {
        wxMessageBox("Unable to open file: " + filePath,
                     "Error", wxICON_ERROR | wxOK, this);
        return;
    }

    try
    {
        std::string content(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>()
        );
        in.close();

        auto doc = std::make_unique<GraphDocument>();
        doc->file_path = filePath;
        doc->display_name = wxFileName(filePath).GetFullName();
        doc->dirty = false;
        doc->graph = std::make_unique<signal_forge::Graph>();

        auto jv = boost::json::parse(content);
        signal_forge::deserialize_graph(jv, *doc->graph);

        doc->panel = new wxPanel(editor_notebook_);
        auto* sizer = new wxBoxSizer(wxVERTICAL);

        doc->canvas = new signal_forge::NodeCanvas(doc->panel, doc->graph.get());
        sizer->Add(doc->canvas, 1, wxEXPAND);

        doc->panel->SetSizer(sizer);

        const wxString tabTitle = wxFileName(filePath).GetFullName();
        editor_notebook_->AddPage(doc->panel, doc->display_name, true);
        m_currentGraphFilePath = filePath;

        open_documents_.push_back(std::move(doc));
    }
    catch (const std::exception& e)
    {
        wxMessageBox(
            wxString::Format("Failed to parse graph file: %s", e.what()),
            "Error",
            wxICON_ERROR | wxOK,
            this
        );
    }
}

MainFrame::GraphDocument* MainFrame::FindDocumentByPage(wxWindow* page)
{
    for (auto& doc : open_documents_)
    {
        if (doc->panel == page)
            return doc.get();
    }
    return nullptr;
}

MainFrame::GraphDocument* MainFrame::FindDocumentByCanvas(signal_forge::NodeCanvas* canvas)
{
    for (auto& doc : open_documents_)
    {
        if (doc->canvas == canvas)
            return doc.get();
    }
    return nullptr;
}

void MainFrame::UpdateDocumentTabTitle(GraphDocument* doc)
{
    if (!doc || !editor_notebook_ || !doc->panel)
        return;

    const int pageIndex = editor_notebook_->GetPageIndex(doc->panel);
    if (pageIndex == wxNOT_FOUND)
        return;

    wxString title = doc->display_name;
    if (doc->dirty)
        title += "*";

    editor_notebook_->SetPageText(pageIndex, title);
}

void MainFrame::SetDocumentDirty(GraphDocument* doc, bool dirty)
{
    if (!doc)
        return;

    if (doc->dirty == dirty)
        return;

    doc->dirty = dirty;
    UpdateDocumentTabTitle(doc);
}

void MainFrame::OnEditorPageClose(wxAuiNotebookEvent& evt)
{
    wxWindow* page = editor_notebook_->GetPage(evt.GetSelection());
    if (!page)
        return;

    auto it = std::find_if(
        open_documents_.begin(),
        open_documents_.end(),
        [page](const std::unique_ptr<GraphDocument>& doc)
        {
            return doc->panel == page;
        }
    );

    if (it != open_documents_.end())
        open_documents_.erase(it);
}

void MainFrame::OnEditorPageChanged(wxAuiNotebookEvent& evt)
{
    wxWindow* page = editor_notebook_->GetPage(evt.GetSelection());
    if (!page)
        return;

    auto it = std::find_if(
        open_documents_.begin(),
        open_documents_.end(),
        [page](const std::unique_ptr<GraphDocument>& doc)
        {
            return doc->panel == page;
        }
    );

    if (it == open_documents_.end())
        return;

    // Update property grid / source view / active document state here
}

void MainFrame::OnProjectTreeItemActivated(wxTreeEvent& evt)
{
    const wxTreeItemId item = evt.GetItem();
    if (!item.IsOk())
        return;

    auto* data = dynamic_cast<ProjectTreeItemData*>(project_tree_ctrl_->GetItemData(item));
    if (!data)
        return;

    if (data->type_ == ProjectTreeItemData::ItemType::BlockDefinitionFile) {
        OpenGraphDocument(data->path_);
    }
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
    const int pageIndex = editor_notebook_->GetSelection();
    if (pageIndex == wxNOT_FOUND)
        return;

    wxWindow* page = editor_notebook_->GetPage(pageIndex);
    if (!page)
        return;

    GraphDocument* doc = FindDocumentByPage(page);
    if (!doc)
        return;

    signal_forge::Node* node = doc->graph->FindNode(id);
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

void MainFrame::OnGraphModified(wxCommandEvent& evt)
{
    auto* canvas = dynamic_cast<signal_forge::NodeCanvas*>(evt.GetEventObject());
    if (!canvas)
        return;

    GraphDocument* doc = FindDocumentByCanvas(canvas);
    SetDocumentDirty(doc, true);
}

void MainFrame::OnPropertyGridChanged(wxPropertyGridEvent& e)
{
    wxPGProperty *pg = e.GetProperty();
    if (!pg) return;

    auto it = node_prop_ctrl_grid_to_properties_.find(pg);
    if (it == node_prop_ctrl_grid_to_properties_.end()) return;

    signal_forge::Property &p = node_prop_ctrl_properties_[it->second];
    if (p.readOnly) return;

    const int pageIndex = editor_notebook_->GetSelection();
    if (pageIndex == wxNOT_FOUND)
        return;

    wxWindow* page = editor_notebook_->GetPage(pageIndex);
    if (!page)
        return;

    GraphDocument* doc = FindDocumentByPage(page);
    if (!doc)
        return;

    signal_forge::Node *node = doc->graph->FindNode(p.node_id);
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