#include "adapter_random/random_adapter_actions.h"

RandomAdapterActions::RandomAdapterActions()
    : panel_(nullptr)
    , test_btn_(nullptr)
    , fetch_btn_(nullptr)
    , spinner_(nullptr)
    , status_(nullptr)
    , ID_TEST(wxWindow::NewControlId())
    , ID_FETCH(wxWindow::NewControlId())
{
}

wxPanel* RandomAdapterActions::CreateActionPanel(wxWindow* parent)
{
    panel_ = new wxPanel(parent);
    auto* box = new wxStaticBoxSizer(wxVERTICAL, panel_, "Actions");

    auto* row = new wxBoxSizer(wxHORIZONTAL);

    test_btn_ = new wxButton(panel_, ID_TEST, "Test Connection");
    row->Add(test_btn_, 0, wxRIGHT, 4);

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

    panel_->Bind(wxEVT_BUTTON, &RandomAdapterActions::OnTestConnection, this, ID_TEST);
    panel_->Bind(wxEVT_BUTTON, &RandomAdapterActions::OnFetchSignals,   this, ID_FETCH);

    return panel_;
}

void RandomAdapterActions::OnSourcePropertiesShown()
{
    // Show action buttons when source is selected
    if (panel_) panel_->Show();
}

void RandomAdapterActions::OnSignalPropertiesShown(size_t)
{
    // Hide actions when viewing individual signals
    if (panel_) panel_->Hide();
    if (panel_) panel_->GetParent()->Layout();
}

void RandomAdapterActions::OnTestConnection(wxCommandEvent&)
{
    if (!meta_reader_) return;

    auto meta = meta_reader_();
    std::string host = meta["host"];
    std::string port = meta["data_port"];

    status_->SetLabel("Testing...");
    spinner_->Show();
    spinner_->Start();

    std::thread([this, host, port]() {
        // Simulate connection test
        std::this_thread::sleep_for(std::chrono::seconds(1));
        bool ok = true; // Replace with real test

        panel_->CallAfter([this, ok]() {
            spinner_->Stop();
            spinner_->Hide();
            if (ok) {
                status_->SetLabel("Connected");
                status_->SetForegroundColour(*wxGREEN);
            } else {
                status_->SetLabel("Failed");
                status_->SetForegroundColour(*wxRED);
            }
        });
    }).detach();
}

void RandomAdapterActions::OnFetchSignals(wxCommandEvent&)
{
    if (!meta_reader_ || !import_cb_) return;

    auto meta = meta_reader_();
    status_->SetLabel("Fetching...");
    spinner_->Show();
    spinner_->Start();
    fetch_btn_->Disable();

    std::thread([this, meta]() {
        // Simulate remote fetch
        std::this_thread::sleep_for(std::chrono::seconds(1));

        std::vector<signal_stream::SignalData> signals;
        signal_stream::SignalData s1;
        s1.name = "remote_temp";
        s1.type = "float32";
        s1.unit = "°C";
        s1.metadata["field_id"]   = "10";
        s1.metadata["field_type"] = "f32";
        s1.metadata["access"]     = "RO";
        signals.push_back(s1);

        panel_->CallAfter([this, signals]() {
            spinner_->Stop();
            spinner_->Hide();
            fetch_btn_->Enable();
            status_->SetLabel(wxString::Format("Fetched %zu signal(s)", signals.size()));
            status_->SetForegroundColour(*wxGREEN);

            if (import_cb_)
                import_cb_(signals);
        });
    }).detach();
}