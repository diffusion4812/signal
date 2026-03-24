#include "adapter_signalforge/signalforge_adapter_actions.h"

SignalForgeAdapterActions::SignalForgeAdapterActions()
    : panel_(nullptr)
    , fetch_btn_(nullptr)
    , spinner_(nullptr)
    , status_(nullptr)
    , ID_TEST(wxWindow::NewControlId())
    , ID_FETCH(wxWindow::NewControlId())
{
}

wxPanel* SignalForgeAdapterActions::CreateActionPanel(wxWindow* parent)
{
    panel_ = new wxPanel(parent);
    auto* box = new wxStaticBoxSizer(wxVERTICAL, panel_, "Actions");

    auto* row = new wxBoxSizer(wxHORIZONTAL);

    fetch_btn_ = new wxButton(panel_, ID_FETCH, "Fetch Signals");
    row->Add(fetch_btn_, 0, wxRIGHT, 4);

    spinner_ = new wxActivityIndicator(panel_, wxID_ANY,
        wxDefaultPosition, wxSize(20, 20));
    spinner_->Hide();
    row->Add(spinner_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);

    status_ = new wxStaticText(panel_, wxID_ANY, "");
    row->Add(status_, 1, wxALIGN_CENTER_VERTICAL);

    box->Add(row, 0, wxEXPAND | wxALL, 4);
    panel_->SetSizer(box);

    panel_->Bind(wxEVT_BUTTON, &SignalForgeAdapterActions::OnFetchSignals,   this, ID_FETCH);

    return panel_;
}

void SignalForgeAdapterActions::OnSourcePropertiesShown()
{
    // Show action buttons when source is selected
    if (panel_) panel_->Show();
}

void SignalForgeAdapterActions::OnSignalPropertiesShown(size_t)
{
    // Hide actions when viewing individual signals
    if (panel_) panel_->Hide();
    if (panel_) panel_->GetParent()->Layout();
}

void SignalForgeAdapterActions::OnFetchSignals(wxCommandEvent&)
{
    if (!meta_reader_ || !import_cb_) return;

    auto meta = meta_reader_();
    status_->SetLabel("Fetching...");
    spinner_->Show();
    spinner_->Start();
    fetch_btn_->Disable();

    std::thread([this, meta]() {
        dbg_sub_config_t cfg = DBG_SUB_CONFIG_DEFAULT;
        cfg.host        = meta_util::GetString(meta, "host", "127.0.0.1").c_str();
        cfg.data_port   = meta_util::GetInt(meta, "data_port", 9500);
        cfg.config_port = meta_util::GetInt(meta, "config_port", 9501);

        sub_ = dbg_sub_create(&cfg);
        dbg_sub_request_field_list(sub_, OnFieldListCallback, this);
    }).detach();
}

void SignalForgeAdapterActions::OnFieldListCallback(
    const dbg_field_descriptor_t* fields,
    uint16_t count,
    uint16_t total,
    void* ctx)
{
    // Recover `this` from the context pointer
    auto* self = static_cast<SignalForgeAdapterActions*>(ctx);
    self->HandleFieldList(fields, count, total);
}

static inline std::string sf_type_from_dbg(dbg_value_type_t vt)
{
    switch (vt) {
    case DBG_VT_BOOL: return "bool";
    case DBG_VT_U8:   return "uint8";
    case DBG_VT_I8:   return "int8";
    case DBG_VT_U16:  return "uint16";
    case DBG_VT_I16:  return "int16";
    case DBG_VT_U32:  return "uint32";
    case DBG_VT_I32:  return "int32";
    case DBG_VT_U64:  return "uint64";
    case DBG_VT_I64:  return "int64";
    case DBG_VT_F32:  return "float32";
    case DBG_VT_F64:  return "float64";
    default:          return "unknown";
    }
}

void SignalForgeAdapterActions::HandleFieldList(
    const dbg_field_descriptor_t* fields,
    uint16_t count,
    uint16_t total)
{
    std::vector<signal_stream::SignalData> signals;
    signals.reserve(count);

    for (uint16_t i = 0; i < count; ++i) {
        signal_stream::SignalData sig;
        sig.name = fields[i].name;
        sig.type = sf_type_from_dbg((dbg_value_type_t)fields[i].value_type);
        sig.metadata["field_id"]   = std::to_string(fields[i].field_id);
        sig.metadata["field_type"] = dbg_value_type_str((dbg_value_type_t)fields[i].value_type);
        sig.metadata["access"]     = "RO";
        signals.push_back(std::move(sig));
    }

    // Marshal back to GUI thread if called from C library thread
    if (panel_) {
        panel_->CallAfter([this, signals = std::move(signals), total]() {
            spinner_->Stop();
            spinner_->Hide();
            fetch_btn_->Enable();
            status_->SetLabel(wxString::Format(
                "Fetched %zu / %u signal(s)",
                signals.size(), total));
            status_->SetForegroundColour(*wxGREEN);

            if (import_cb_) import_cb_(signals);
        });
    }
}