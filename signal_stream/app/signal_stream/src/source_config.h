#pragma once

#include <wx/wx.h>
#include <wx/treectrl.h>
#include <wx/splitter.h>
#include <wx/propgrid/propgrid.h>

#include "signal_stream_app/source_config_propgrid_registry.h"
#include "signal_stream_app/source_action_factory.h"

#include <signal_stream_core/project.h>

class SourceConfigDialog : public wxDialog {
public:
    SourceConfigDialog(wxWindow* parent, signal_stream::SourceData& data, SourceActionFactory& actionFactory);

private:
    void BuildLayout();
    void BuildTree();
    void RebuildTree(size_t selectIndex);

    void OnTreeSelChanged(wxTreeEvent& event);
    void OnAddSignal(wxCommandEvent& event);
    void OnDeleteSignal(wxCommandEvent& event);
    void OnOK(wxCommandEvent& event);

    void SaveCurrentGrid();
    void ShowSourceProperties();
    void ShowSignalProperties(size_t index);
    void UpdateButtons();

    void AppendProperty(const std::string& key,
                        const std::string& value,
                        bool isSignal);

    void UpdateDeleteButton();
    void ImportSignals(const std::vector<signal_stream::SignalData>& signals);
    std::unordered_map<std::string, std::string> ReadCurrentMetadata();

    wxTreeCtrl*     tree_       = nullptr;
    wxPropertyGrid* grid_       = nullptr;
    wxButton*       add_btn_    = nullptr;
    wxButton*       delete_btn_ = nullptr;
    wxPanel*        action_host_= nullptr;
    wxTreeItemId    root_id_;

    signal_stream::SourceData&         data_;
    PropertyTypeRegistry               registry_;
    std::unique_ptr<ISourceActions>    actions_;

    // Track which node is currently displayed
    enum class Selection { None, Source, Signal };
    Selection       current_sel_   = Selection::None;
    size_t          current_index_ = 0;

    wxWindowIDRef ID_ADD_SIGNAL;
    wxWindowIDRef ID_DELETE_SIGNAL;
};

class SignalTreeItemData : public wxTreeItemData {
public:
    explicit SignalTreeItemData(size_t index) : index_(index) {}
    size_t GetIndex() const { return index_; }
private:
    size_t index_;
};