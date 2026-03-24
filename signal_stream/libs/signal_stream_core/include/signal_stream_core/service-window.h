#pragma once

#include <any>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "service-bus.h"

namespace signal_stream {

    struct AppState;
    struct IWindow;

    // Forward declarations of window classes
    class Window_Console;
    class Window_SignalBrowser;
    class Window_FileBrowser;
    class Window_FPS;
    class Window_Live;
    class Window_OpenProject;

    class WindowManager {
    public:
        struct Event {
            enum class Type { OpenWindow, DockWindow, CloseWindow };
            Type type;
            std::string windowType;
            std::any payload;
        };

        struct DockPayload {
            std::string sourceWindowId;
            std::string targetWindowId;
            std::string dockPosition; // "left", "right", "top", "bottom", "tab"
        };

        struct ClosePayload {
            std::string id;
        };

        WindowManager(ServiceBus& bus, AppState& state);
        IWindow* FindWindowById(const std::string& id);
        IWindow* OpenWindow(const std::string& type, const std::any& payload);
        void DockWindow(const DockPayload& payload);
        void CloseWindow(const ClosePayload& payload);
        void RenderAll();

        template<typename EventT>
        void Publish(EventT&& ev) {
            using CleanT = std::decay_t<EventT>;
            bus_.Publish<CleanT>(std::forward<EventT>(ev));
        }

    private:
        struct DockSpace {
            std::string id;
            std::vector<IWindow*> windows;
        };

        ServiceBus& bus_;
        SubscriptionToken token_;
        AppState& state_;

        uint64_t idCounter_ = 0;
        std::string GenerateUniqueId(const std::string& type) {
            return type + "_" + std::to_string(++idCounter_);
        }

        std::unordered_map<std::string, std::unique_ptr<IWindow>> windows_;
        std::unordered_map<std::string, DockSpace> dockspaces_;
        DockSpace& GetOrCreateDockSpace(const std::string& dockspaceId);
    };

} // namespace signal_stream