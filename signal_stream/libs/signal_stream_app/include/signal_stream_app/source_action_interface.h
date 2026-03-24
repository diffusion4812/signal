#pragma once

#include <wx/wx.h>
#include <string>
#include <vector>
#include <functional>
#include <memory>

#include "signal_stream_core/project.h"
#include "source_action_meta_util.h"

// Callback to push discovered signals into the dialog
using SignalImportCallback = std::function<void(
    const std::vector<signal_stream::SignalData>& signals)>;

// Callback to read current metadata from the grid
using MetadataReader = std::function<
    std::unordered_map<std::string, std::string>()>;

class ISourceActions {
public:
    virtual ~ISourceActions() = default;

    // Create the action panel — attached below the property grid
    virtual wxPanel* CreateActionPanel(wxWindow* parent) = 0;

    // Wire up callbacks so actions can interact with the dialog
    virtual void SetSignalImportCallback(SignalImportCallback cb) = 0;
    virtual void SetMetadataReader(MetadataReader cb) = 0;

    // Lifecycle
    virtual void OnSourcePropertiesShown() {}
    virtual void OnSignalPropertiesShown(size_t /*index*/) {}
    virtual void OnBeforeSave() {}
};