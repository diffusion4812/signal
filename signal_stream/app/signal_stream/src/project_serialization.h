#pragma once

#include <boost/json.hpp>
#include "signal_stream_core/project.h"
#include <memory>
#include <vector>

namespace signal_stream {

namespace json = boost::json;

// ─── Project-level serialization ────────────────────────

json::value serialize_project(const ProjectData& project);
void deserialize_project(const boost::json::value& jv, ProjectData& project);

} // namespace signal_stream