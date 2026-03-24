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
    signal_forge::Graph& get_graph() { return graph_; }
    void set_generation_result(const signal_forge::Generator::Result& result) { result_ = result; }
    signal_forge::Generator::Result get_generation_result() const { return result_; }
    void set_compiled_filename(const std::string& filename) { compiled_filename_ = filename; }
    const std::string& get_compiled_filename() const { return compiled_filename_; }

private:
    Config config_;

    signal_forge::Graph graph_;
    signal_forge::Generator::Result result_;
    std::string compiled_filename_;
};

wxDECLARE_APP(App);