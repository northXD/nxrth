// Adonai - persist the fleet AutomationConfig (module enables + params like the
// geiger hunt/depot/pickup world lists) to data/automation_config.json so the
// setup survives an app restart. Saved explicitly from the UI; loaded once at
// startup into the FleetState.
#pragma once

#include "bot/fleet_state.h"  // adonai::bot::AutomationConfig

namespace adonai::automation {

// Write the whole config (enabled + params + group assignments) to
// data/automation_config.json. Best-effort; never throws.
void save_automation_config(const adonai::bot::AutomationConfig& cfg);

// Load it back into `out` (merges over whatever `out` already holds). Returns
// false if the file is missing/unreadable (leaves `out` untouched on failure).
bool load_automation_config(adonai::bot::AutomationConfig& out);

}  // namespace adonai::automation
