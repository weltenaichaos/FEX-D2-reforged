// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

#include <FEXCore/fextl/string.h>

namespace FEX::Windows {
// The FEX_* switches of the Windows frontend (stats log, native redirects, unpack cache, thread placer). A switch is the
// environment variable when it is set and not empty, otherwise this build's default for the running program (see
// Switches.cpp), otherwise unset (nullptr).
//
// Call InitSwitches once the executable's name is known, before anything reads a switch.
void InitSwitches(std::string_view AppName);
const char* Switch(const char* Name);

// The stats log's "# switches:" line: every switch with its value, "-" when unset, and "(default)" after the ones that
// come from this build's defaults rather than the environment.
fextl::string SwitchSummary();
} // namespace FEX::Windows
