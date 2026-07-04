// StreamPluginAdapter.h — in-process acquisition of the streaming-source plugin
// (Phase 4 slice b). The adapter (src/StreamPluginAdapter.cpp) wraps the in-tree
// StreamSource behind the RemoctPlugin C table.
//
// In slice (b) the host calls this DIRECTLY (compiled in) — no dlopen, no new
// binary, pure host plumbing. In slice (c) the same descriptor becomes the
// .so/.dll's exported remoct_plugin_query reached via core::loadPlugin(); the
// host-driving code (core::PluginSource) is byte-identical across the flip, so
// only where the descriptor comes from changes.
#pragma once
#include "core/remoct_plugin.h"

// The streaming source's plugin descriptor (a static, module-lifetime table).
const RemoctPlugin* remoct_stream_plugin_query();
