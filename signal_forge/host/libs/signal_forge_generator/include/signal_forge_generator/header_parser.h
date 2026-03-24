#pragma once

#include <string>
#include <vector>
#include <filesystem>

#include "signal_forge_graph/graph.h"

/// Parse all typedef-struct blocks in a C header file and return one
/// BlockTemplate per struct found.
///
/// Recognised pin/field annotations (per struct member line):
///
///   Macro style          Comment style
///   ─────────────────    ───────────────────────────────────────
///   INPUT  <type> <nm>;  <type> <nm>;  // input
///   OUTPUT <type> <nm>;  <type> <nm>;  // output
///   STATIC <type> <nm>;  <type> <nm>;  // static
///
/// Optional qualifier keywords after "static" in comment style:
///   // static readonly   → host_readable  = true
///   // static writeonly  → host_writable  = true
///   // static rw         → both flags true
///
std::vector<signal_forge::BlockTemplate>
parse_header(const std::filesystem::path& path);