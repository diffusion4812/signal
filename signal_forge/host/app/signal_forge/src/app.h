#pragma once

#include <wx/wx.h>
#include <filesystem>

#include "signal_forge_graph/graph.h"
#include "signal_forge_generator/generator.h"

class App : public wxApp {
public:
    bool OnInit() override;

    struct Config {
        std::filesystem::path zig;
        std::filesystem::path putty_scp;
        struct Library {
            std::string name;
            std::filesystem::path library;
            std::filesystem::path header;
        };
        std::vector<Library> libraries;

        void load(const std::filesystem::path& path);
    };

    Config get_config() const { return config_; }

private:
    Config config_;
};

wxDECLARE_APP(App);