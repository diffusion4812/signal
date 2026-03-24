#pragma once

#include "signal_stream_app/source_action_interface.h"
#include <wx/activityindicator.h>
#include <thread>

#include <dbg_pubsub.h>
#include <dbg_util.h>

class SignalForgeAdapterActions : public ISourceActions {
public:
    SignalForgeAdapterActions();
    wxPanel* CreateActionPanel(wxWindow* parent) override;
    void SetSignalImportCallback(SignalImportCallback cb) override { import_cb_ = std::move(cb); }
    void SetMetadataReader(MetadataReader cb) override { meta_reader_ = std::move(cb); }

    void OnSourcePropertiesShown() override;
    void OnSignalPropertiesShown(size_t index) override;

private:
    void OnFetchSignals(wxCommandEvent& event);
    static void OnFieldListCallback(const dbg_field_descriptor_t* fields,
                                     uint16_t count,
                                     uint16_t total,
                                     void* ctx);

    void HandleFieldList(const dbg_field_descriptor_t* fields,
                         uint16_t count,
                         uint16_t total);

    wxPanel*             panel_     = nullptr;
    wxButton*            fetch_btn_ = nullptr;
    wxStaticText*        status_    = nullptr;
    wxActivityIndicator* spinner_   = nullptr;

    SignalImportCallback import_cb_;
    MetadataReader       meta_reader_;

    wxWindowIDRef ID_TEST;
    wxWindowIDRef ID_FETCH;

    /** Maximum number of fields we can discover and subscribe to */
    static constexpr uint16_t MAX_DISCOVERED_FIELDS = 256;

    /** Maximum length of a field name (including null terminator) */
    static constexpr uint16_t MAX_FIELD_NAME_LEN    = 64;

    typedef struct {
        uint64_t            field_id;
        dbg_value_type_t    value_type;
        uint8_t             access;
        char                name[MAX_FIELD_NAME_LEN];
        char                unit[MAX_FIELD_NAME_LEN];
    } discovered_field_t;

    discovered_field_t  g_discovered[MAX_DISCOVERED_FIELDS];
    uint16_t            g_discovered_count = 0;
    uint16_t            g_discovered_total = 0;
    
    dbg_subscriber_t*  sub_ = NULL;
    dbg_sub_layout_t   layout_;
};