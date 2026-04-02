#include "app.h"
#include "main_frame.h"
#include "signal_forge_generator/header_parser.h"

#include <boost/json.hpp>
#include <fstream>
#include <sstream>

void App::Config::load(const std::filesystem::path& path) {
    // Read file
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw std::runtime_error(std::string("Cannot open: ") + path.string());
    }

    std::ostringstream oss;
    oss << ifs.rdbuf();

    // Parse JSON
    boost::system::error_code ec;
    auto root = boost::json::parse(oss.str(), ec);
    if (ec) {
        throw std::runtime_error(std::string("JSON error: ") + ec.message());
    }

    if (!root.is_object()) {
        throw std::runtime_error("Root must be a JSON object");
    }

    const auto& obj = root.as_object();

    // zig_cc
    if (auto it = obj.find("zig"); it != obj.end() && it->value().is_string())
        zig = std::string(it->value().as_string());

    // putty_scp
    if (auto it = obj.find("putty_scp"); it != obj.end() && it->value().is_string())
        putty_scp = std::string(it->value().as_string());

    // libraries
    if (auto it = obj.find("libraries"); it != obj.end() && it->value().is_array()) {
        for (const auto& v : it->value().as_array()) {
            if (!v.is_object())
                continue;

            const auto& lib_obj = v.as_object();

            Library lib;

            if (auto n = lib_obj.find("name"); n != lib_obj.end() && n->value().is_string())
                lib.name = std::string(n->value().as_string());

            if (auto l = lib_obj.find("library"); l != lib_obj.end() && l->value().is_string())
                lib.library = std::string(l->value().as_string());

            if (auto h = lib_obj.find("header"); h != lib_obj.end() && h->value().is_string())
                lib.header = std::string(h->value().as_string());

            if (lib.name.empty()) {
                throw std::runtime_error("Library entry missing 'name'");
            }

            libraries.push_back(std::move(lib));
        }
    }
}

bool App::OnInit() {
    config_.load("config.json");

//    for (const auto& lib : config_.libraries) {
//        std::vector<signal_forge::BlockTemplate> block_templates = parse_header(lib.header);
//        for (auto& bt : block_templates) {
//            bt.library = lib.library;
//            bt.header = lib.header;
//            graph_.RegisterBlockTemplate(std::move(bt));
//        }
//    }

    auto *frame = new MainFrame();
    frame->Show(true);
    return true;
}

wxIMPLEMENT_APP(App);