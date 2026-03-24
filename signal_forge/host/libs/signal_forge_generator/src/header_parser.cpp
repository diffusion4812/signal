#include "signal_forge_generator/header_parser.h"

#include <fstream>
#include <sstream>
#include <regex>
#include <cstring>
#include <stdexcept>
#include <filesystem>

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

static signal_forge::Field::FieldType MapCType(const std::string& ctype) {
    if (ctype == "float" || ctype == "double") return signal_forge::Field::FieldType::REAL;
    if (ctype == "int"   || ctype == "int32_t"
     || ctype == "uint32_t" || ctype == "int16_t"
     || ctype == "uint16_t") return signal_forge::Field::FieldType::REAL; // extend as needed
    return signal_forge::Field::FieldType::REAL;
}

// ─────────────────────────────────────────────────────────────────────────────
// ParseFields
//
// Walks every line of a struct body and extracts Pin (input/output) and
// Static fields.  Three annotation styles are supported:
//
//   Macro:    INPUT  float speed;
//             OUTPUT float torque;
//             STATIC float integral;
//
//   Comment:  float speed;    // input
//             float torque;   // output
//             float integral; // static
//             float integral; // static readonly
//             float value;    // static writeonly
//             float cfg;      // static rw
// ─────────────────────────────────────────────────────────────────────────────

static void ParseFields(const std::string& body, signal_forge::BlockTemplate& tmpl) {

    // Macro style:  (INPUT|OUTPUT|STATIC)  <ctype>  <name>  ;
    static const std::regex macro_re(
        R"(\b(INPUT|OUTPUT|STATIC)\s+(\w+)\s+(\w+)\s*;)",
        std::regex::ECMAScript
    );

    // Comment style:  <ctype>  <name>  ;  //  (input|output|static) [qualifier]
    static const std::regex comment_re(
        R"((\w+)\s+(\w+)\s*;\s*//\s*(input|output|static)(?:\s+(readonly|writeonly|rw))?)",
        std::regex::icase
    );

    std::istringstream stream(body);
    std::string line;
    int pin_id    = 0;
    int static_id = 0;

    while (std::getline(stream, line)) {
        std::smatch m;

        if (std::regex_search(line, m, macro_re)) {
            // m[1]=INPUT|OUTPUT|STATIC  m[2]=ctype  m[3]=name
            const std::string dir   = m[1].str();
            const std::string ctype = m[2].str();
            const std::string name  = m[3].str();

            if (dir == "INPUT" || dir == "OUTPUT") {
                signal_forge::Pin pin;
                pin.id        = pin_id++;
                pin.name      = name;
                pin.type      = MapCType(ctype);
                pin.direction = (dir == "INPUT")
                              ? signal_forge::PinDirection::INPUT
                              : signal_forge::PinDirection::OUTPUT;
                if (pin.direction == signal_forge::PinDirection::INPUT)
                    tmpl.inputs.push_back(pin);
                else
                    tmpl.outputs.push_back(pin);
            } else { // STATIC
                signal_forge::Static sf;
                sf.id   = static_id++;
                sf.name = name;
                sf.type = MapCType(ctype);
                // Macro style STATIC has no host-access qualifier by default.
                tmpl.statics.push_back(sf);
            }
        }
        else if (std::regex_search(line, m, comment_re)) {
            // m[1]=ctype  m[2]=name  m[3]=input|output|static  m[4]=qualifier (optional)
            const std::string ctype     = m[1].str();
            const std::string name      = m[2].str();
            std::string       dir       = m[3].str();
            std::string       qualifier = (m[4].matched) ? m[4].str() : "";

            // Normalise to lower-case for comparison
            for (char& c : dir)       c = (char)std::tolower((unsigned char)c);
            for (char& c : qualifier) c = (char)std::tolower((unsigned char)c);

            if (dir == "input" || dir == "output") {
                signal_forge::Pin pin;
                pin.id        = pin_id++;
                pin.name      = name;
                pin.type      = MapCType(ctype);
                pin.direction = (dir == "input")
                              ? signal_forge::PinDirection::INPUT
                              : signal_forge::PinDirection::OUTPUT;
                if (pin.direction == signal_forge::PinDirection::INPUT)
                    tmpl.inputs.push_back(pin);
                else
                    tmpl.outputs.push_back(pin);
            } else { // static
                signal_forge::Static sf;
                sf.id           = static_id++;
                sf.name         = name;
                sf.type         = MapCType(ctype);
                sf.host_readable = (qualifier == "readonly" || qualifier == "rw");
                sf.host_writable = (qualifier == "writeonly" || qualifier == "rw");
                tmpl.statics.push_back(sf);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FindEntryFunc
//
// Returns the name of the first function in the translation unit that takes a
// pointer to struct_name as any argument.
// ─────────────────────────────────────────────────────────────────────────────

static std::string FindEntryFunc(const std::string& content,
                                 const std::string& struct_name)
{
    std::regex func_re(
        R"([\w:<>]+\s+(\w+)\s*\([^)]*\b)" + struct_name + R"(\s*\*)",
        std::regex::ECMAScript
    );
    std::smatch m;
    if (std::regex_search(content, m, func_re))
        return m[1].str();
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// magic_from_string — FNV-1a 64-bit hash of a raw struct body
// ─────────────────────────────────────────────────────────────────────────────

static uint64_t magic_from_string(const std::string& s) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

// ─────────────────────────────────────────────────────────────────────────────
// parse_header  (public)
// ─────────────────────────────────────────────────────────────────────────────

std::vector<signal_forge::BlockTemplate>
parse_header(const std::filesystem::path& path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
        throw std::runtime_error("Cannot open: " + path.string());

    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());

    // Match every  typedef struct { … } Name;  block (non-greedy, multiline).
    static const std::regex struct_re(
        R"(typedef\s+struct\s*\{\s*([\s\S]*?)\}\s*(\w+)\s*;)",
        std::regex::ECMAScript
    );

    std::vector<signal_forge::BlockTemplate> blocks;

    for (auto it = std::sregex_iterator(content.begin(), content.end(), struct_re);
         it != std::sregex_iterator(); ++it)
    {
        const std::string body        = (*it)[1].str();
        const std::string struct_name = (*it)[2].str();

        signal_forge::BlockTemplate tmpl;
        tmpl.struct_name        = struct_name;
        tmpl.type               = struct_name;
        tmpl.description        = struct_name + " function block";
        tmpl.signature          = magic_from_string(body);
        tmpl.is_function_block  = true;
        tmpl.is_builtin         = false;
        tmpl.entry_func         = FindEntryFunc(content, struct_name);

        ParseFields(body, tmpl);

        blocks.push_back(std::move(tmpl));
    }

    if (blocks.empty())
        throw std::runtime_error("No typedef structs with pins found in " + path.string());

    return blocks;
}