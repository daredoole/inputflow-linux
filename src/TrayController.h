#pragma once

#include <string>
#include <vector>

namespace mwb {

struct AppConfig;

// Runs the combined tray + GUI + daemon. GTK must already be initialised
// (gtk_init / gtk_init_check called by caller). Blocks until gtk_main_quit().
int RunTrayAndGui(const std::string& binary,
                  const std::vector<std::string>& args,
                  const AppConfig& config,
                  const std::string& configPath,
                  const std::string& statePath);

} // namespace mwb
