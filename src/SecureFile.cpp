#include "SecureFile.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace mwb {
namespace {

std::string ErrorText(const std::string& action, const std::filesystem::path& path, int errorNumber) {
    return action + " '" + path.string() + "': " + std::strerror(errorNumber);
}

} // namespace

bool WritePrivateFileAtomically(const std::filesystem::path& path,
                                std::string_view contents,
                                std::string& errorMessage) {
    if (path.empty() || path.filename().empty()) {
        errorMessage = "Private file path must include a file name.";
        return false;
    }

    const std::filesystem::path parent =
        path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
    std::error_code filesystemError;
    const bool parentExisted = std::filesystem::exists(parent, filesystemError);
    if (filesystemError) {
        errorMessage = "Could not inspect private file directory '" + parent.string() +
                       "': " + filesystemError.message();
        return false;
    }

    std::filesystem::create_directories(parent, filesystemError);
    if (filesystemError) {
        errorMessage = "Could not create private file directory '" + parent.string() +
                       "': " + filesystemError.message();
        return false;
    }
    if (!parentExisted) {
        std::filesystem::permissions(
            parent,
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            filesystemError);
        if (filesystemError) {
            errorMessage = "Could not secure private file directory '" + parent.string() +
                           "': " + filesystemError.message();
            return false;
        }
    }

    const auto destinationStatus = std::filesystem::symlink_status(path, filesystemError);
    if (filesystemError &&
        filesystemError != std::make_error_code(std::errc::no_such_file_or_directory)) {
        errorMessage = "Could not inspect private file destination '" + path.string() +
                       "': " + filesystemError.message();
        return false;
    }
    if (!filesystemError && std::filesystem::exists(destinationStatus) &&
        !std::filesystem::is_regular_file(destinationStatus)) {
        errorMessage = "Refusing to replace non-regular private file destination: " + path.string();
        return false;
    }

    static std::atomic<unsigned long> sequence{0};
    const std::filesystem::path temporary =
        parent / ("." + path.filename().string() + ".tmp." + std::to_string(getpid()) + "." +
                  std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
    const int descriptor = open(
        temporary.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        errorMessage = ErrorText("Could not create temporary private file", temporary, errno);
        return false;
    }

    bool success = true;
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t written = write(descriptor, contents.data() + offset, contents.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            errorMessage = ErrorText("Could not write temporary private file", temporary, errno);
            success = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (success && fsync(descriptor) != 0) {
        errorMessage = ErrorText("Could not sync temporary private file", temporary, errno);
        success = false;
    }
    if (close(descriptor) != 0 && success) {
        errorMessage = ErrorText("Could not close temporary private file", temporary, errno);
        success = false;
    }

    if (!success) {
        unlink(temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        errorMessage = ErrorText("Could not atomically replace private file", path, errno);
        unlink(temporary.c_str());
        return false;
    }

    const int parentDescriptor = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parentDescriptor >= 0) {
        (void)fsync(parentDescriptor);
        (void)close(parentDescriptor);
    }
    return true;
}

} // namespace mwb
