#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace mwb {

// Atomically replaces a regular file with owner-only permissions. Symlink and
// special-file destinations are rejected so config/state writes cannot be
// redirected outside the intended application directory.
bool WritePrivateFileAtomically(const std::filesystem::path& path,
                                std::string_view contents,
                                std::string& errorMessage);

} // namespace mwb
