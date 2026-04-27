#pragma once

#include <optional>
#include <string>
#include <vector>

namespace mwb {

enum class MediaKeyAction {
    Previous,
    Stop,
    PlayPause,
    Next
};

std::optional<MediaKeyAction> MediaActionForVirtualKey(int vkCode);
const char* PlayerctlCommandForAction(MediaKeyAction action);
std::vector<std::string> BuildPlayerctlArguments(MediaKeyAction action, const std::string& playerName);
void ConfigureMediaKeyBridge(bool mprisEnabled, const std::string& playerName);
bool DispatchMediaKeyAction(MediaKeyAction action);

} // namespace mwb
