#pragma once

#include "signal_stream_app/source_action_interface.h"
#include <wx/activityindicator.h>
#include <thread>

class RandomAdapterActions : public ISourceActions {
public:
    RandomAdapterActions();
    wxPanel* CreateActionPanel(wxWindow* parent) override;
    void SetSignalImportCallback(SignalImportCallback cb) override { import_cb_ = std::move(cb); }
    void SetMetadataReader(MetadataReader cb) override { meta_reader_ = std::move(cb); }

    void OnSourcePropertiesShown() override;
    void OnSignalPropertiesShown(size_t index) override;

private:
    void OnFetchSignals(wxCommandEvent& event);
    void OnTestConnection(wxCommandEvent& event);

    wxPanel*             panel_   = nullptr;
    wxButton*            fetch_btn_ = nullptr;
    wxButton*            test_btn_  = nullptr;
    wxStaticText*        status_    = nullptr;
    wxActivityIndicator* spinner_   = nullptr;

    SignalImportCallback import_cb_;
    MetadataReader       meta_reader_;

    wxWindowIDRef ID_TEST;
    wxWindowIDRef ID_FETCH;
};