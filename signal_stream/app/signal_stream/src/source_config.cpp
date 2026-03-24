#include <wx/propgrid/advprops.h>

#include "source_config.h"

SourceConfigDialog::SourceConfigDialog(
    wxWindow* parent,
    signal_stream::SourceData& data,
    SourceActionFactory& actionFactory)
    : wxDialog(parent, wxID_ANY, "Source Configuration",
               wxDefaultPosition, wxSize(640, 560),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      data_(data)
{
    // Create source-specific actions (may be null)
    actions_ = actionFactory.Create(data_.type);

    if (actions_) {
        actions_->SetSignalImportCallback(
            [this](const std::vector<signal_stream::SignalData>& sigs) {
                ImportSignals(sigs);
            });
        actions_->SetMetadataReader(
            [this]() { return ReadCurrentMetadata(); });
    }

    BuildLayout();
    CentreOnParent();
}

void SourceConfigDialog::BuildLayout()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // ── Outer splitter: left (tree) | right (grid + actions) ──
    auto* splitter = new wxSplitterWindow(
        this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
        wxSP_LIVE_UPDATE);

    // ── Left panel: tree + buttons ──
    auto* left = new wxPanel(splitter);
    auto* leftSizer = new wxBoxSizer(wxVERTICAL);

    tree_ = new wxTreeCtrl(left, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxTR_DEFAULT_STYLE | wxTR_HIDE_ROOT);
    leftSizer->Add(tree_, 1, wxEXPAND);

    auto* btnBar = new wxBoxSizer(wxHORIZONTAL);
    add_btn_    = new wxButton(left, ID_ADD_SIGNAL, "+", wxDefaultPosition, wxSize(30, 24));
    delete_btn_ = new wxButton(left, ID_DELETE_SIGNAL, "-", wxDefaultPosition, wxSize(30, 24));
    delete_btn_->Disable();
    btnBar->Add(add_btn_, 0, wxRIGHT, 2);
    btnBar->Add(delete_btn_, 0);
    btnBar->AddStretchSpacer();
    leftSizer->Add(btnBar, 0, wxEXPAND | wxTOP, 2);

    left->SetSizer(leftSizer);

    // ── Right panel: contains the inner splitter ──
    auto* right = new wxPanel(splitter);
    auto* rightSizer = new wxBoxSizer(wxVERTICAL);

    // ── Inner splitter: property grid (top) | action panel (bottom) ──
    auto* splitter2 = new wxSplitterWindow(right, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxSP_LIVE_UPDATE | wxSP_3DSASH);

    grid_ = new wxPropertyGrid(splitter2, wxID_ANY,
        wxDefaultPosition, wxDefaultSize,
        wxPG_SPLITTER_AUTO_CENTER | wxPG_TOOLTIPS);

    wxWindow* actionHost = nullptr;
    if (actions_) {
        actionHost = actions_->CreateActionPanel(splitter2);
    }

    if (actionHost) {
        splitter2->SplitHorizontally(grid_, actionHost, -150);
        splitter2->SetMinimumPaneSize(80);
    } else {
        splitter2->Initialize(grid_);
    }

    rightSizer->Add(splitter2, 1, wxEXPAND);

    right->SetSizer(rightSizer);

    // ── Configure outer splitter ──
    splitter->SplitVertically(left, right, 180);
    splitter->SetMinimumPaneSize(120);

    // ── Dialog-level layout ──
    sizer->Add(splitter, 1, wxEXPAND | wxALL, 8);
    sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL),
               0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
    SetSizer(sizer);

    // ── Populate and bind ──
    BuildTree();
    tree_->SelectItem(root_id_);

    tree_->Bind(wxEVT_TREE_SEL_CHANGED, &SourceConfigDialog::OnTreeSelChanged, this);
    Bind(wxEVT_BUTTON, &SourceConfigDialog::OnAddSignal,    this, ID_ADD_SIGNAL);
    Bind(wxEVT_BUTTON, &SourceConfigDialog::OnDeleteSignal, this, ID_DELETE_SIGNAL);
    Bind(wxEVT_BUTTON, &SourceConfigDialog::OnOK,           this, wxID_OK);
}

// ═══════════════════════════════════════════════════════
//  Tree
// ═══════════════════════════════════════════════════════

void SourceConfigDialog::BuildTree()
{
    tree_->DeleteAllItems();

    wxTreeItemId hidden = tree_->AddRoot("root");

    wxString label = wxString::Format("%s  (%s)", data_.name, data_.type);
    root_id_ = tree_->AppendItem(hidden, label);

    for (size_t i = 0; i < data_.signals.size(); ++i) {
        const auto& sig = data_.signals[i];
        wxString sl = sig.name;
        if (!sig.unit.empty())
            sl += wxString::Format("  [%s]", sig.unit);

        tree_->AppendItem(root_id_, sl, -1, -1,
                           new SignalTreeItemData(i));
    }

    tree_->ExpandAll();
}

void SourceConfigDialog::RebuildTree(size_t selectIndex)
{
    // Preserve expansion state and rebuild
    BuildTree();

    if (selectIndex < data_.signals.size()) {
        // Find and select the signal node at selectIndex
        wxTreeItemIdValue cookie;
        wxTreeItemId child = tree_->GetFirstChild(root_id_, cookie);
        for (size_t i = 0; child.IsOk(); ++i) {
            if (i == selectIndex) {
                tree_->SelectItem(child);
                return;
            }
            child = tree_->GetNextChild(root_id_, cookie);
        }
    }

    // Fallback: select source root
    tree_->SelectItem(root_id_);
}

// ═══════════════════════════════════════════════════════
//  Save / Show properties
// ═══════════════════════════════════════════════════════

void SourceConfigDialog::SaveCurrentGrid()
{
    if (current_sel_ == Selection::Source) {
        data_.name = grid_->GetPropertyValueAsString("source_name").ToStdString();

        for (auto& [key, value] : data_.metadata) {
            wxPGProperty* p = grid_->GetPropertyByName(key);
            if (p) value = p->GetValueAsString().ToStdString();
        }

        tree_->SetItemText(root_id_,
            wxString::Format("%s  (%s)", data_.name, data_.type));
    }
    else if (current_sel_ == Selection::Signal
             && current_index_ < data_.signals.size()) {

        auto& sig = data_.signals[current_index_];
        sig.name = grid_->GetPropertyValueAsString("name").ToStdString();
        sig.type = grid_->GetPropertyValueAsString("type").ToStdString();
        sig.unit = grid_->GetPropertyValueAsString("unit").ToStdString();

        for (auto& [key, value] : sig.metadata) {
            wxPGProperty* p = grid_->GetPropertyByName(key);
            if (p) value = p->GetValueAsString().ToStdString();
        }

        wxTreeItemIdValue cookie;
        wxTreeItemId child = tree_->GetFirstChild(root_id_, cookie);
        for (size_t i = 0; child.IsOk(); ++i) {
            if (i == current_index_) {
                wxString label = sig.name;
                if (!sig.unit.empty()) label += "  [" + sig.unit + "]";
                tree_->SetItemText(child, label);
                break;
            }
            child = tree_->GetNextChild(root_id_, cookie);
        }
    }
}

static wxString PrettyLabel(const std::string& key)
{
    wxString label = key;
    label.Replace("_", " ");
    bool cap = true;
    for (size_t i = 0; i < label.length(); ++i) {
        if (label[i] == ' ')       cap = true;
        else if (cap) { label[i] = wxToupper(label[i]); cap = false; }
    }
    return label;
}

static PropertyHint::Type InferType(const std::string& value)
{
    if (value.empty()) return PropertyHint::Type::String;
    if (value == "true" || value == "false") return PropertyHint::Type::Bool;

    char* end = nullptr;
    std::strtol(value.c_str(), &end, 10);
    if (end != value.c_str() && *end == '\0') return PropertyHint::Type::Int;

    end = nullptr;
    std::strtod(value.c_str(), &end);
    if (end != value.c_str() && *end == '\0') return PropertyHint::Type::Float;

    return PropertyHint::Type::String;
}

void SourceConfigDialog::AppendProperty(const std::string& key,
                                         const std::string& value,
                                         bool isSignal)
{
    const PropertyHint* hint = isSignal
        ? registry_.GetSignalHint(data_.type, key)
        : registry_.GetHint(data_.type, key);

    wxString label = PrettyLabel(key);
    wxString wxKey = key;
    wxString wxVal = value;

    PropertyHint::Type type = hint
        ? hint->type
        : InferType(value);

    wxPGProperty* prop = nullptr;

    switch (type) {
    case PropertyHint::Type::Int: {
        long n = 0; wxVal.ToLong(&n);
        auto* p = new wxIntProperty(label, wxKey, n);
        if (hint) {
            if (hint->min_val) p->SetAttribute(wxPG_ATTR_MIN, (int)*hint->min_val);
            if (hint->max_val) p->SetAttribute(wxPG_ATTR_MAX, (int)*hint->max_val);
            if (hint->step)    p->SetAttribute(wxPG_ATTR_SPINCTRL_STEP, (int)*hint->step);
        }
        prop = p;
        break;
    }
    case PropertyHint::Type::Float: {
        double d = 0; wxVal.ToDouble(&d);
        auto* p = new wxFloatProperty(label, wxKey, d);
        if (hint) {
            if (hint->min_val) p->SetAttribute(wxPG_ATTR_MIN, *hint->min_val);
            if (hint->max_val) p->SetAttribute(wxPG_ATTR_MAX, *hint->max_val);
        }
        prop = p;
        break;
    }
    case PropertyHint::Type::Bool: {
        bool b = (value == "true" || value == "1");
        prop = new wxBoolProperty(label, wxKey, b);
        grid_->SetPropertyAttribute(wxKey, wxPG_BOOL_USE_CHECKBOX, true);
        break;
    }
    case PropertyHint::Type::Enum: {
        if (hint && !hint->choices.empty()) {
            wxArrayString choices;
            wxArrayInt ids;
            for (int i = 0; i < (int)hint->choices.size(); ++i) {
                choices.Add(hint->choices[i]);
                ids.Add(i);
            }
            int sel = choices.Index(wxVal);
            prop = new wxEnumProperty(label, wxKey, choices, ids,
                                       sel >= 0 ? sel : 0);
        } else {
            prop = new wxStringProperty(label, wxKey, wxVal);
        }
        break;
    }
    default:
        prop = new wxStringProperty(label, wxKey, wxVal);
        break;
    }

    if (prop) {
        if (hint && hint->read_only) prop->Enable(false);
        if (hint && !hint->tooltip.empty()) prop->SetHelpString(hint->tooltip);
        grid_->Append(prop);
    }
}

void SourceConfigDialog::ShowSourceProperties()
{
    grid_->Clear();
    current_sel_ = Selection::Source;

    grid_->Append(new wxPropertyCategory("Source"));
    grid_->Append(new wxStringProperty("Name", "source_name", data_.name));

    auto* tp = new wxStringProperty("Type", "source_type", data_.type);
    tp->Enable(false);
    grid_->Append(tp);

    grid_->Append(new wxPropertyCategory("Metadata"));

    for (const auto& [key, value] : data_.metadata) {
        AppendProperty(key, value, false);
    }
}

void SourceConfigDialog::ShowSignalProperties(size_t index)
{
    grid_->Clear();
    current_sel_   = Selection::Signal;
    current_index_ = index;

    const auto& sig = data_.signals[index];

    grid_->Append(new wxPropertyCategory("Signal"));
    grid_->Append(new wxStringProperty("Name", "name", sig.name));
    grid_->Append(new wxStringProperty("Type", "type", sig.type));
    grid_->Append(new wxStringProperty("Unit", "unit", sig.unit));

    grid_->Append(new wxPropertyCategory("Metadata"));

    for (const auto& [key, value] : sig.metadata) {
        AppendProperty(key, value, true);
    }
}

// ═══════════════════════════════════════════════════════
//  Add / Delete signals
// ═══════════════════════════════════════════════════════

void SourceConfigDialog::OnAddSignal(wxCommandEvent& /*event*/)
{
    // Save whatever is currently displayed
    SaveCurrentGrid();

    // Create a new signal with sensible defaults
    signal_stream::SignalData newSig;
    newSig.name = "new_signal";
    newSig.type = "float32";
    newSig.unit = "";

    // Copy metadata keys from an existing signal as a template
    if (!data_.signals.empty()) {
        for (const auto& [key, _] : data_.signals.front().metadata) {
            newSig.metadata[key] = "";
        }
        // Provide a default field_id
        int maxId = 0;
        for (const auto& sig : data_.signals) {
            auto it = sig.metadata.find("field_id");
            if (it != sig.metadata.end()) {
                long val = 0;
                wxString(it->second).ToLong(&val);
                if (val > maxId) maxId = static_cast<int>(val);
            }
        }
        newSig.metadata["field_id"] = std::to_string(maxId + 1);
    }

    data_.signals.push_back(std::move(newSig));

    // Rebuild tree and select the new signal
    size_t newIndex = data_.signals.size() - 1;
    RebuildTree(newIndex);
}

void SourceConfigDialog::OnDeleteSignal(wxCommandEvent& /*event*/)
{
    wxTreeItemId sel = tree_->GetSelection();
    if (!sel.IsOk() || sel == root_id_)
        return;

    auto* itemData = dynamic_cast<SignalTreeItemData*>(
        tree_->GetItemData(sel));
    if (!itemData)
        return;

    size_t index = itemData->GetIndex();
    const auto& sig = data_.signals[index];

    // Confirm deletion
    int answer = wxMessageBox(
        wxString::Format("Delete signal '%s'?", sig.name),
        "Confirm Delete",
        wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
        this);

    if (answer != wxYES)
        return;

    // Remove from data
    data_.signals.erase(data_.signals.begin() + index);

    // Clear grid since the displayed item may be gone
    grid_->Clear();
    current_sel_ = Selection::None;

    // Rebuild tree, select nearest neighbor
    size_t selectNext = index < data_.signals.size()
                            ? index
                            : (data_.signals.empty() ? (size_t)-1 : index - 1);
    RebuildTree(selectNext);
}

void SourceConfigDialog::UpdateDeleteButton()
{
    wxTreeItemId sel = tree_->GetSelection();
    bool isSignal = sel.IsOk()
                 && sel != root_id_
                 && dynamic_cast<SignalTreeItemData*>(tree_->GetItemData(sel));

    delete_btn_->Enable(isSignal);
}

// ═══════════════════════════════════════════════════════
//  Actions
// ═══════════════════════════════════════════════════════

void SourceConfigDialog::ImportSignals(
    const std::vector<signal_stream::SignalData>& signals)
{
    SaveCurrentGrid();

    for (const auto& sig : signals)
        data_.signals.push_back(sig);

    size_t lastIdx = data_.signals.size() - 1;
    BuildTree();

    // Select last imported
    wxTreeItemIdValue cookie;
    wxTreeItemId child = tree_->GetFirstChild(root_id_, cookie);
    for (size_t i = 0; child.IsOk(); ++i) {
        if (i == lastIdx) { tree_->SelectItem(child); break; }
        child = tree_->GetNextChild(root_id_, cookie);
    }
}

std::unordered_map<std::string, std::string>
SourceConfigDialog::ReadCurrentMetadata()
{
    // Save grid first so metadata is up to date
    SaveCurrentGrid();
    return data_.metadata;
}

// ═══════════════════════════════════════════════════════
//  Events
// ═══════════════════════════════════════════════════════

void SourceConfigDialog::UpdateButtons()
{
    wxTreeItemId sel = tree_->GetSelection();
    bool isSig = sel.IsOk() && sel != root_id_
              && dynamic_cast<SignalTreeItemData*>(tree_->GetItemData(sel));
    delete_btn_->Enable(isSig);
}

void SourceConfigDialog::OnTreeSelChanged(wxTreeEvent& event)
{
    wxTreeItemId sel = event.GetItem();
    if (!sel.IsOk()) return;

    SaveCurrentGrid();

    if (sel == root_id_) {
        ShowSourceProperties();
        if (actions_) actions_->OnSourcePropertiesShown();
    } else if (auto* d = dynamic_cast<SignalTreeItemData*>(
                   tree_->GetItemData(sel))) {
        ShowSignalProperties(d->GetIndex());
        if (actions_) actions_->OnSignalPropertiesShown(d->GetIndex());
    }

    UpdateButtons();
}

void SourceConfigDialog::OnOK(wxCommandEvent& /*event*/)
{
    SaveCurrentGrid();
    EndModal(wxID_OK);
}