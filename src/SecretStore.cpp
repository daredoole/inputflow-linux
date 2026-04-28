#include "SecretStore.h"

#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* kSecretToolName = "secret-tool";
constexpr const char* kApplicationAttribute = "application";
constexpr const char* kApplicationValue = "mwb-client-linux";
constexpr const char* kSecretIdAttribute = "secret-id";

struct CommandResult {
    bool launched{false};
    int exitCode{-1};
    std::string stdoutText;
    std::string stderrText;
};

std::string_view TrimWhitespace(std::string_view value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

std::optional<std::string> FindExecutable(const std::string& executable) {
    if (executable.empty()) {
        return std::nullopt;
    }

    if (executable.find('/') != std::string::npos) {
        if (!executable.empty() && executable.front() == '/' && access(executable.c_str(), X_OK) == 0) {
            return executable;
        }
        return std::nullopt;
    }

    const std::array<std::string, 3> trustedPrefixes = {
        "/usr/bin/",
        "/bin/",
        "/usr/local/bin/",
    };
    for (const auto& prefix : trustedPrefixes) {
        const std::string candidate = prefix + executable;
        if (access(candidate.c_str(), X_OK) == 0) {
            return candidate;
        }
    }

    return std::nullopt;
}

bool WriteAll(int fd, const std::string& text) {
    size_t offset = 0;
    while (offset < text.size()) {
        const ssize_t written = write(fd, text.data() + offset, text.size() - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool ReadAll(int fd, std::string& output) {
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return true;
        }
        output.append(buffer.data(), static_cast<size_t>(count));
    }
}

std::optional<std::string> ResolveSecretToolPath() {
    static const std::optional<std::string> cached = FindExecutable(kSecretToolName);
    return cached;
}

CommandResult RunCommand(const std::vector<std::string>& argv, const std::string* input = nullptr) {
    CommandResult result;
    if (argv.empty()) {
        result.stderrText = "empty command";
        return result;
    }

    int stdoutPipe[2];
    int stderrPipe[2];
    int stdinPipe[2];
    if (pipe(stdoutPipe) != 0 || pipe(stderrPipe) != 0) {
        result.stderrText = std::strerror(errno);
        return result;
    }

    bool useStdin = input != nullptr;
    if (useStdin && pipe(stdinPipe) != 0) {
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        close(stderrPipe[0]);
        close(stderrPipe[1]);
        result.stderrText = std::strerror(errno);
        return result;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        close(stderrPipe[0]);
        close(stderrPipe[1]);
        if (useStdin) {
            close(stdinPipe[0]);
            close(stdinPipe[1]);
        }
        result.stderrText = std::strerror(errno);
        return result;
    }

    if (pid == 0) {
        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stderrPipe[1], STDERR_FILENO);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        close(stderrPipe[0]);
        close(stderrPipe[1]);

        if (useStdin) {
            dup2(stdinPipe[0], STDIN_FILENO);
            close(stdinPipe[0]);
            close(stdinPipe[1]);
        }

        std::vector<char*> execArgv;
        execArgv.reserve(argv.size() + 1);
        for (const std::string& arg : argv) {
            execArgv.push_back(const_cast<char*>(arg.c_str()));
        }
        execArgv.push_back(nullptr);

        execv(execArgv[0], execArgv.data());
        _exit(127);
    }

    result.launched = true;

    close(stdoutPipe[1]);
    close(stderrPipe[1]);

    if (useStdin) {
        close(stdinPipe[0]);
        (void)WriteAll(stdinPipe[1], *input);
        close(stdinPipe[1]);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    (void)ReadAll(stdoutPipe[0], result.stdoutText);
    (void)ReadAll(stderrPipe[0], result.stderrText);
    close(stdoutPipe[0]);
    close(stderrPipe[0]);

    if (!WIFEXITED(status)) {
        result.exitCode = -1;
        if (result.stderrText.empty()) {
            result.stderrText = "subprocess did not exit cleanly";
        }
        return result;
    }

    result.exitCode = WEXITSTATUS(status);
    return result;
}

std::string FormatCommandFailure(std::string_view action, const CommandResult& result) {
    const std::string stderrTrimmed(TrimWhitespace(result.stderrText));
    if (!stderrTrimmed.empty()) {
        return std::string(action) + " failed: " + stderrTrimmed;
    }
    if (!result.stdoutText.empty()) {
        return std::string(action) + " failed: " + std::string(TrimWhitespace(result.stdoutText));
    }
    return std::string(action) + " failed with exit code " + std::to_string(result.exitCode) + ".";
}

std::string BuildSecretLabel(const std::string& secretId) {
    return "InputFlow key (" + secretId + ")";
}

bool ValidateSecretId(std::string_view secretId, std::string* errorMessage) {
    if (TrimWhitespace(secretId).empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Secret id must not be empty.";
        }
        return false;
    }
    return true;
}

} // namespace

namespace mwb {

bool SecretStoreIsAvailable(std::string* errorMessage) {
    if (!ResolveSecretToolPath().has_value()) {
        if (errorMessage != nullptr) {
            *errorMessage = "secret-tool is not available. Install libsecret tools or use key_file instead.";
        }
        return false;
    }
    return true;
}

bool LookupSecretKey(const std::string& secretId, std::string& outKey, std::string* errorMessage) {
    if (!ValidateSecretId(secretId, errorMessage)) {
        return false;
    }
    if (!SecretStoreIsAvailable(errorMessage)) {
        return false;
    }

    const auto secretTool = ResolveSecretToolPath();
    const CommandResult result = RunCommand({
        *secretTool,
        "lookup",
        kApplicationAttribute,
        kApplicationValue,
        kSecretIdAttribute,
        secretId,
    });

    if (!result.launched || result.exitCode != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = "No security key found in the desktop keyring for secret id '" + secretId + "'.";
            if (!result.stderrText.empty()) {
                *errorMessage += " " + std::string(TrimWhitespace(result.stderrText));
            }
        }
        return false;
    }

    const std::string resolvedKey(TrimWhitespace(result.stdoutText));
    if (resolvedKey.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Desktop keyring entry '" + secretId + "' is empty.";
        }
        return false;
    }

    outKey = resolvedKey;
    return true;
}

bool StoreSecretKey(const std::string& secretId, const std::string& key, std::string* errorMessage) {
    if (!ValidateSecretId(secretId, errorMessage)) {
        return false;
    }
    const std::string trimmedKey(TrimWhitespace(key));
    if (trimmedKey.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = "Security key must not be empty.";
        }
        return false;
    }
    if (!SecretStoreIsAvailable(errorMessage)) {
        return false;
    }

    const auto secretTool = ResolveSecretToolPath();
    const CommandResult result = RunCommand({
        *secretTool,
        "store",
        "--label=" + BuildSecretLabel(secretId),
        kApplicationAttribute,
        kApplicationValue,
        kSecretIdAttribute,
        secretId,
    }, &trimmedKey);

    if (!result.launched || result.exitCode != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = FormatCommandFailure("Storing desktop keyring entry", result);
        }
        return false;
    }

    return true;
}

bool ClearSecretKey(const std::string& secretId, std::string* errorMessage) {
    if (!ValidateSecretId(secretId, errorMessage)) {
        return false;
    }
    if (!SecretStoreIsAvailable(errorMessage)) {
        return false;
    }

    const auto secretTool = ResolveSecretToolPath();
    const CommandResult result = RunCommand({
        *secretTool,
        "clear",
        kApplicationAttribute,
        kApplicationValue,
        kSecretIdAttribute,
        secretId,
    });

    if (!result.launched || result.exitCode != 0) {
        if (errorMessage != nullptr) {
            *errorMessage = FormatCommandFailure("Clearing desktop keyring entry", result);
        }
        return false;
    }

    return true;
}

} // namespace mwb
