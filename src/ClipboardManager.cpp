#include "ClipboardManager.h"

#include "Protocol.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <codecvt>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <locale>
#include <memory>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <zlib.h>

namespace mwb {
namespace {

struct CommandSpec {
    std::vector<std::string> argv;
};

class ExternalCommandClipboardBackend final : public ClipboardBackend {
public:
    ExternalCommandClipboardBackend(
        std::string name,
        CommandSpec listTypesCommand,
        CommandSpec readTextCommand,
        CommandSpec readHtmlCommand,
        CommandSpec readImageCommand,
        CommandSpec writeCommand,
        CommandSpec watchCommand = {},
        CommandSpec watchReadCommand = {},
        std::string watchSignalNeedle = {})
        : m_name(std::move(name)),
          m_listTypesCommand(std::move(listTypesCommand)),
          m_readTextCommand(std::move(readTextCommand)),
          m_readHtmlCommand(std::move(readHtmlCommand)),
          m_readImageCommand(std::move(readImageCommand)),
          m_writeCommand(std::move(writeCommand)),
          m_watchCommand(std::move(watchCommand)),
          m_watchReadCommand(std::move(watchReadCommand)),
          m_watchSignalNeedle(std::move(watchSignalNeedle)) {}

    std::optional<ClipboardPayload> ReadPayload() override;
    bool WritePayload(const ClipboardPayload& payload) override;
    std::string Name() const override { return m_name; }
    bool WatchPayloadChanges(
        const std::atomic<bool>& running,
        const std::function<void(const ClipboardPayload&)>& onPayloadChange) override;

private:
    std::string m_name;
    CommandSpec m_listTypesCommand;
    CommandSpec m_readTextCommand;
    CommandSpec m_readHtmlCommand;
    CommandSpec m_readImageCommand;
    CommandSpec m_writeCommand;
    CommandSpec m_watchCommand;
    CommandSpec m_watchReadCommand;
    std::string m_watchSignalNeedle;
};

bool writeAll(int fd, const uint8_t* data, std::size_t length) {
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t written = write(fd, data + offset, length - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

bool readAll(int fd, std::vector<uint8_t>& output) {
    std::array<uint8_t, 4096> buffer{};
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
        output.insert(output.end(), buffer.begin(), buffer.begin() + count);
    }
}

std::optional<std::string> findExecutable(const std::string& executable) {
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

std::optional<std::vector<uint8_t>> runReadBytesCommand(const CommandSpec& command) {
    if (command.argv.empty()) {
        return std::nullopt;
    }

    int stdoutPipe[2];
    if (pipe(stdoutPipe) != 0) {
        return std::nullopt;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        return std::nullopt;
    }

    if (pid == 0) {
        dup2(stdoutPipe[1], STDOUT_FILENO);
        const int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            dup2(devNull, STDERR_FILENO);
            close(devNull);
        }
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);

        std::vector<char*> argv;
        argv.reserve(command.argv.size() + 1);
        for (const std::string& arg : command.argv) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execv(argv[0], argv.data());
        _exit(127);
    }

    close(stdoutPipe[1]);

    std::vector<uint8_t> output;
    const bool readOk = readAll(stdoutPipe[0], output);
    close(stdoutPipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    if (!readOk || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }

    return output;
}

std::optional<std::string> runReadCommand(const CommandSpec& command) {
    auto bytes = runReadBytesCommand(command);
    if (!bytes.has_value()) {
        return std::nullopt;
    }
    return std::string(bytes->begin(), bytes->end());
}

void trimTrailingNewlines(std::string& text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
}

bool runWriteCommand(const CommandSpec& command, const std::string& input) {
    if (command.argv.empty()) {
        return false;
    }

    int stdinPipe[2];
    if (pipe(stdinPipe) != 0) {
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        return false;
    }

    if (pid == 0) {
        dup2(stdinPipe[0], STDIN_FILENO);
        close(stdinPipe[0]);
        close(stdinPipe[1]);

        std::vector<char*> argv;
        argv.reserve(command.argv.size() + 1);
        for (const std::string& arg : command.argv) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execv(argv[0], argv.data());
        _exit(127);
    }

    close(stdinPipe[0]);
    const bool wroteOk = writeAll(
        stdinPipe[1],
        reinterpret_cast<const uint8_t*>(input.data()),
        input.size());
    close(stdinPipe[1]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    return wroteOk && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool runWriteBytesCommand(const CommandSpec& command, const std::vector<uint8_t>& input) {
    if (command.argv.empty()) {
        return false;
    }

    int stdinPipe[2];
    if (pipe(stdinPipe) != 0) {
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(stdinPipe[0]);
        close(stdinPipe[1]);
        return false;
    }

    if (pid == 0) {
        dup2(stdinPipe[0], STDIN_FILENO);
        close(stdinPipe[0]);
        close(stdinPipe[1]);

        std::vector<char*> argv;
        argv.reserve(command.argv.size() + 1);
        for (const std::string& arg : command.argv) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execv(argv[0], argv.data());
        _exit(127);
    }

    close(stdinPipe[0]);
    const bool wroteOk = writeAll(
        stdinPipe[1],
        input.data(),
        input.size());
    close(stdinPipe[1]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    return wroteOk && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool runWatchCommand(
    const CommandSpec& command,
    const std::atomic<bool>& running,
    const std::function<void(const ClipboardPayload&)>& onPayloadChange,
    const std::function<std::optional<ClipboardPayload>()>& readPayload) {
    if (command.argv.empty()) {
        return false;
    }

    int stdoutPipe[2];
    if (pipe(stdoutPipe) != 0) {
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        return false;
    }

    if (pid == 0) {
        dup2(stdoutPipe[1], STDOUT_FILENO);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);

        std::vector<char*> argv;
        argv.reserve(command.argv.size() + 1);
        for (const std::string& arg : command.argv) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execv(argv[0], argv.data());
        _exit(127);
    }

    close(stdoutPipe[1]);

    std::string buffered;
    bool exitedUnexpectedly = false;

    while (running) {
        struct pollfd pfd{};
        pfd.fd = stdoutPipe[0];
        pfd.events = POLLIN;

        const int pollRc = poll(&pfd, 1, 250);
        if (pollRc < 0) {
            if (errno == EINTR) {
                continue;
            }
            exitedUnexpectedly = true;
            break;
        }
        if (pollRc == 0) {
            continue;
        }

        if ((pfd.revents & POLLIN) != 0) {
            std::array<char, 4096> chunk{};
            const ssize_t count = read(stdoutPipe[0], chunk.data(), chunk.size());
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                exitedUnexpectedly = true;
                break;
            }
            if (count == 0) {
                exitedUnexpectedly = running;
                break;
            }

            buffered.append(chunk.data(), static_cast<std::size_t>(count));
            std::size_t separator = 0;
            while ((separator = buffered.find('\0')) != std::string::npos) {
                buffered.erase(0, separator + 1);
                auto payload = readPayload();
                if (payload.has_value()) {
                    onPayloadChange(*payload);
                }
            }
        }

        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            exitedUnexpectedly = running;
            break;
        }
    }

    close(stdoutPipe[0]);

    kill(pid, SIGTERM);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    return !exitedUnexpectedly;
}

bool runSignalWatchCommand(
    const CommandSpec& watchCommand,
    const CommandSpec& readCommand,
    const std::string& signalNeedle,
    const std::atomic<bool>& running,
    const std::function<void(const ClipboardPayload&)>& onPayloadChange,
    const std::function<std::optional<ClipboardPayload>()>& readPayload) {
    (void)readCommand; // Not needed if we use readPayload instead
    if (watchCommand.argv.empty() || signalNeedle.empty()) {
        return false;
    }

    int stdoutPipe[2];
    if (pipe(stdoutPipe) != 0) {
        return false;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);
        return false;
    }

    if (pid == 0) {
        dup2(stdoutPipe[1], STDOUT_FILENO);
        close(stdoutPipe[0]);
        close(stdoutPipe[1]);

        std::vector<char*> argv;
        argv.reserve(watchCommand.argv.size() + 1);
        for (const std::string& arg : watchCommand.argv) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execv(argv[0], argv.data());
        _exit(127);
    }

    close(stdoutPipe[1]);

    std::string buffered;
    bool exitedUnexpectedly = false;

    while (running) {
        struct pollfd pfd{};
        pfd.fd = stdoutPipe[0];
        pfd.events = POLLIN;

        const int pollRc = poll(&pfd, 1, 250);
        if (pollRc < 0) {
            if (errno == EINTR) {
                continue;
            }
            exitedUnexpectedly = true;
            break;
        }
        if (pollRc == 0) {
            continue;
        }

        if ((pfd.revents & POLLIN) != 0) {
            std::array<char, 4096> chunk{};
            const ssize_t count = read(stdoutPipe[0], chunk.data(), chunk.size());
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                exitedUnexpectedly = true;
                break;
            }
            if (count == 0) {
                exitedUnexpectedly = running;
                break;
            }

            buffered.append(chunk.data(), static_cast<std::size_t>(count));
            std::size_t newline = 0;
            while ((newline = buffered.find('\n')) != std::string::npos) {
                const std::string line = buffered.substr(0, newline);
                buffered.erase(0, newline + 1);
                if (line.find(signalNeedle) == std::string::npos) {
                    continue;
                }

                auto payload = readPayload();
                if (payload.has_value()) {
                    onPayloadChange(*payload);
                }
            }
        }

        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            exitedUnexpectedly = running;
            break;
        }
    }

    close(stdoutPipe[0]);
    kill(pid, SIGTERM);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    return !exitedUnexpectedly;
}


std::u16string utf8ToUtf16(std::string_view text) {
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
    return converter.from_bytes(text.data(), text.data() + text.size());
}

std::string utf16ToUtf8(const std::u16string& text) {
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> converter;
    return converter.to_bytes(text);
}

std::vector<uint8_t> encodeUtf16Le(std::u16string_view text) {
    std::vector<uint8_t> bytes;
    bytes.reserve(text.size() * 2);
    for (const char16_t ch : text) {
        bytes.push_back(static_cast<uint8_t>(ch & 0x00ff));
        bytes.push_back(static_cast<uint8_t>((ch >> 8) & 0x00ff));
    }
    return bytes;
}

std::u16string decodeUtf16Le(const std::vector<uint8_t>& bytes) {
    std::u16string text;
    text.reserve(bytes.size() / 2);
    for (std::size_t index = 0; index + 1 < bytes.size(); index += 2) {
        const char16_t value = static_cast<char16_t>(bytes[index]) |
                               (static_cast<char16_t>(bytes[index + 1]) << 8);
        text.push_back(value);
    }
    return text;
}

std::string trimNulsAndWhitespace(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '\0'), value.end());

    const auto notSpace = [](unsigned char ch) {
        return !std::isspace(ch);
    };

    const auto begin = std::find_if(value.begin(), value.end(), notSpace);
    if (begin == value.end()) {
        return {};
    }
    const auto end = std::find_if(value.rbegin(), value.rend(), notSpace).base();
    return std::string(begin, end);
}

bool startsWithIgnoreCase(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(value[index])) !=
            std::tolower(static_cast<unsigned char>(prefix[index]))) {
            return false;
        }
    }
    return true;
}

void appendLineBreak(std::string& output) {
    if (output.empty() || output.back() == '\n') {
        return;
    }
    output.push_back('\n');
}

void appendUtf8CodePoint(std::string& output, uint32_t codePoint) {
    if (codePoint <= 0x7F) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | ((codePoint >> 6) & 0x1F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        output.push_back(static_cast<char>(0xE0 | ((codePoint >> 12) & 0x0F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0x10FFFF) {
        output.push_back(static_cast<char>(0xF0 | ((codePoint >> 18) & 0x07)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

std::optional<uint32_t> decodeHtmlEntity(std::string_view entity) {
    if (entity == "nbsp") {
        return static_cast<uint32_t>(' ');
    }
    if (entity == "amp") {
        return static_cast<uint32_t>('&');
    }
    if (entity == "lt") {
        return static_cast<uint32_t>('<');
    }
    if (entity == "gt") {
        return static_cast<uint32_t>('>');
    }
    if (entity == "quot") {
        return static_cast<uint32_t>('"');
    }
    if (entity == "apos") {
        return static_cast<uint32_t>('\'');
    }
    if (!entity.empty() && entity.front() == '#') {
        try {
            std::size_t parsed = 0;
            const bool isHex = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X');
            const uint32_t value = isHex
                ? static_cast<uint32_t>(std::stoul(std::string(entity.substr(2)), &parsed, 16))
                : static_cast<uint32_t>(std::stoul(std::string(entity.substr(1)), &parsed, 10));
            if ((isHex && parsed == entity.size() - 2) || (!isHex && parsed == entity.size() - 1)) {
                return value;
            }
        } catch (...) {
        }
    }
    return std::nullopt;
}

std::string decodeHtmlEntities(std::string_view text) {
    std::string output;
    output.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '&') {
            output.push_back(text[index]);
            continue;
        }

        const std::size_t end = text.find(';', index + 1);
        if (end == std::string_view::npos || end - index > 16) {
            output.push_back(text[index]);
            continue;
        }

        const auto decoded = decodeHtmlEntity(text.substr(index + 1, end - index - 1));
        if (!decoded.has_value()) {
            output.push_back(text[index]);
            continue;
        }

        appendUtf8CodePoint(output, *decoded);
        index = end;
    }

    return output;
}

std::optional<std::string_view> extractCfHtmlFragment(std::string_view text) {
    if (!startsWithIgnoreCase(text, "Version:")) {
        return std::nullopt;
    }

    constexpr std::string_view kStartFragmentComment = "<!--StartFragment-->";
    constexpr std::string_view kEndFragmentComment = "<!--EndFragment-->";
    const std::size_t startComment = text.find(kStartFragmentComment);
    if (startComment != std::string_view::npos) {
        const std::size_t contentStart = startComment + kStartFragmentComment.size();
        const std::size_t endComment = text.find(kEndFragmentComment, contentStart);
        if (endComment != std::string_view::npos) {
            return text.substr(contentStart, endComment - contentStart);
        }
    }

    auto parseOffset = [&](std::string_view label) -> std::optional<std::size_t> {
        const std::size_t labelPos = text.find(label);
        if (labelPos == std::string_view::npos) {
            return std::nullopt;
        }

        std::size_t valueStart = labelPos + label.size();
        while (valueStart < text.size() &&
               (text[valueStart] == ' ' || text[valueStart] == '\t' || text[valueStart] == ':')) {
            ++valueStart;
        }

        std::size_t valueEnd = valueStart;
        while (valueEnd < text.size() && std::isdigit(static_cast<unsigned char>(text[valueEnd]))) {
            ++valueEnd;
        }
        if (valueEnd == valueStart) {
            return std::nullopt;
        }

        try {
            return static_cast<std::size_t>(std::stoull(std::string(text.substr(valueStart, valueEnd - valueStart))));
        } catch (...) {
            return std::nullopt;
        }
    };

    const auto startOffset = parseOffset("StartFragment");
    const auto endOffset = parseOffset("EndFragment");
    if (startOffset.has_value() && endOffset.has_value() &&
        *startOffset < *endOffset && *endOffset <= text.size()) {
        return text.substr(*startOffset, *endOffset - *startOffset);
    }

    return std::nullopt;
}

std::optional<std::size_t> parseCfHtmlOffset(std::string_view text, std::string_view label) {
    if (!startsWithIgnoreCase(text, "Version:")) {
        return std::nullopt;
    }

    const std::size_t labelPos = text.find(label);
    if (labelPos == std::string_view::npos) {
        return std::nullopt;
    }

    std::size_t valueStart = labelPos + label.size();
    while (valueStart < text.size() &&
           (text[valueStart] == ' ' || text[valueStart] == '\t' || text[valueStart] == ':')) {
        ++valueStart;
    }

    std::size_t valueEnd = valueStart;
    while (valueEnd < text.size() && std::isdigit(static_cast<unsigned char>(text[valueEnd]))) {
        ++valueEnd;
    }
    if (valueEnd == valueStart) {
        return std::nullopt;
    }

    try {
        return static_cast<std::size_t>(std::stoull(std::string(text.substr(valueStart, valueEnd - valueStart))));
    } catch (...) {
        return std::nullopt;
    }
}

std::string stripHtmlToPlainText(std::string_view html) {
    std::string output;
    output.reserve(html.size());

    bool inTag = false;
    std::string currentTag;
    for (char ch : html) {
        if (!inTag) {
            if (ch == '<') {
                inTag = true;
                currentTag.clear();
            } else {
                output.push_back(ch);
            }
            continue;
        }

        if (ch == '>') {
            inTag = false;

            std::string tag = trimNulsAndWhitespace(currentTag);
            bool closingTag = false;
            if (!tag.empty() && tag.front() == '/') {
                closingTag = true;
                tag.erase(tag.begin());
            }
            const std::size_t separator = tag.find_first_of(" \t\r\n/");
            if (separator != std::string::npos) {
                tag.erase(separator);
            }
            std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (tag == "br" ||
                (closingTag && (tag == "p" || tag == "div" || tag == "li" || tag == "tr"))) {
                appendLineBreak(output);
            }
            continue;
        }

        currentTag.push_back(ch);
    }

    output = decodeHtmlEntities(output);

    std::string normalized;
    normalized.reserve(output.size());
    bool lastWasSpace = false;
    bool lastWasNewline = false;
    for (char ch : output) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            while (!normalized.empty() && normalized.back() == ' ') {
                normalized.pop_back();
            }
            if (!lastWasNewline) {
                normalized.push_back('\n');
                lastWasNewline = true;
            }
            lastWasSpace = false;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!lastWasSpace && !lastWasNewline) {
                normalized.push_back(' ');
                lastWasSpace = true;
            }
            continue;
        }
        normalized.push_back(ch);
        lastWasSpace = false;
        lastWasNewline = false;
    }

    return trimNulsAndWhitespace(normalized);
}

std::optional<std::string> normalizeHtmlClipboardValue(std::string_view text) {
    const auto fragment = extractCfHtmlFragment(text);
    if (!fragment.has_value()) {
        return std::nullopt;
    }

    std::string normalized = stripHtmlToPlainText(*fragment);
    if (normalized.empty()) {
        return std::nullopt;
    }
    return normalized;
}

std::vector<uint8_t> deflateRaw(const std::vector<uint8_t>& input) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }

    std::vector<uint8_t> output;
    output.reserve(input.size() / 2 + 64);

    std::array<uint8_t, 4096> buffer{};
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    int rc = Z_OK;
    do {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());
        rc = deflate(&stream, Z_FINISH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            deflateEnd(&stream);
            return {};
        }

        const std::size_t produced = buffer.size() - stream.avail_out;
        output.insert(output.end(), buffer.begin(), buffer.begin() + produced);
    } while (rc != Z_STREAM_END);

    deflateEnd(&stream);
    return output;
}

std::optional<std::vector<uint8_t>> inflateRaw(const std::vector<uint8_t>& input, std::size_t maxOutputSize) {
    z_stream stream{};
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        return std::nullopt;
    }

    std::vector<uint8_t> output;
    std::array<uint8_t, 4096> buffer{};

    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());

    while (true) {
        stream.next_out = buffer.data();
        stream.avail_out = static_cast<uInt>(buffer.size());

        const int rc = inflate(&stream, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) {
            inflateEnd(&stream);
            return std::nullopt;
        }

        const std::size_t produced = buffer.size() - stream.avail_out;
        if (produced > maxOutputSize - output.size()) {
            inflateEnd(&stream);
            return std::nullopt;
        }
        output.insert(output.end(), buffer.begin(), buffer.begin() + produced);

        if (rc == Z_STREAM_END) {
            break;
        }

        if (stream.avail_in == 0 && produced == 0) {
            break;
        }
    }

    inflateEnd(&stream);
    return output;
}

ClipboardHtmlPayload parseHtmlClipboardValue(std::string_view value) {
    ClipboardHtmlPayload html;
    html.raw = std::string(value);
    html.startHtml = parseCfHtmlOffset(value, "StartHTML");
    html.endHtml = parseCfHtmlOffset(value, "EndHTML");
    html.startFragment = parseCfHtmlOffset(value, "StartFragment");
    html.endFragment = parseCfHtmlOffset(value, "EndFragment");

    const auto fragment = extractCfHtmlFragment(value);
    if (fragment.has_value()) {
        html.fragment = std::string(*fragment);
    }

    html.plainText = normalizeHtmlClipboardValue(value);
    return html;
}

ClipboardPayload parseTextClipboardPayload(std::string_view payload) {
    ClipboardPayload parsed;
    std::optional<std::string> firstText;
    std::optional<std::string> fallback;
    std::optional<std::string> htmlFallback;
    std::size_t start = 0;

    while (start < payload.size()) {
        std::size_t end = payload.find(kClipboardTextSeparator, start);
        if (end == std::string_view::npos) {
            end = payload.size();
        }

        const std::string rawEntry(payload.substr(start, end - start));
        std::string entry = trimNulsAndWhitespace(rawEntry);
        if (!entry.empty()) {
            if (entry.rfind(kClipboardTextPrefix, 0) == 0) {
                if (!firstText.has_value()) {
                    firstText = entry.substr(std::strlen(kClipboardTextPrefix));
                }
            } else if (entry.rfind(kClipboardHtmlPrefix, 0) == 0) {
                const std::string taggedValue = entry.substr(std::strlen(kClipboardHtmlPrefix));
                if (!parsed.html.has_value()) {
                    if (rawEntry.rfind(kClipboardHtmlPrefix, 0) == 0) {
                        parsed.html = parseHtmlClipboardValue(
                            std::string_view(rawEntry).substr(std::strlen(kClipboardHtmlPrefix)));
                    } else {
                        parsed.html = parseHtmlClipboardValue(taggedValue);
                    }
                }
                if (!htmlFallback.has_value()) {
                    htmlFallback = normalizeHtmlClipboardValue(taggedValue);
                }
                if (!fallback.has_value()) {
                    fallback = taggedValue;
                }
            } else if (entry.size() > 3) {
                const std::string taggedValue = entry.substr(3);
                if (!htmlFallback.has_value()) {
                    htmlFallback = normalizeHtmlClipboardValue(taggedValue);
                }
                if (!fallback.has_value()) {
                    fallback = taggedValue;
                }
            } else if (!fallback.has_value()) {
                fallback = entry;
            }
        }

        if (end == payload.size()) {
            break;
        }
        start = end + std::strlen(kClipboardTextSeparator);
    }

    if (firstText.has_value()) {
        parsed.plainText = firstText;
    } else if (htmlFallback.has_value()) {
        parsed.plainText = htmlFallback;
    } else {
        parsed.plainText = fallback;
    }

    return parsed;
}

std::unique_ptr<ClipboardBackend> createBackend() {
    const bool hasWayland = std::getenv("WAYLAND_DISPLAY") != nullptr;
    const bool hasDisplay = std::getenv("DISPLAY") != nullptr;
    const char* currentDesktop = std::getenv("XDG_CURRENT_DESKTOP");
    const bool hasKdeDesktop = currentDesktop != nullptr &&
                               std::string(currentDesktop).find("KDE") != std::string::npos;
    const auto shell = findExecutable("/bin/sh");
    const auto gdbus = findExecutable("gdbus");
    const auto wlPaste = findExecutable("wl-paste");
    const auto wlCopy = findExecutable("wl-copy");
    const auto xclip = findExecutable("xclip");
    const auto xsel = findExecutable("xsel");

    if (hasWayland && hasKdeDesktop && gdbus && wlPaste && wlCopy) {
        const CommandSpec readCommand{{*wlPaste, "--no-newline"}};
        return std::make_unique<ExternalCommandClipboardBackend>(
            "wl-clipboard-klipper",
            CommandSpec{{*wlPaste, "--list-types"}},
            readCommand,
            CommandSpec{{*wlPaste, "--no-newline", "--type", "text/html"}},
            CommandSpec{{*wlPaste, "--no-newline", "--type", "image/png"}},
            CommandSpec{{*wlCopy}},
            CommandSpec{{*gdbus, "monitor", "--session", "--dest", "org.kde.klipper", "--object-path", "/klipper"}},
            readCommand,
            "clipboardChanged");
    }

    if (hasWayland && shell && wlPaste && wlCopy) {
        return std::make_unique<ExternalCommandClipboardBackend>(
            "wl-clipboard",
            CommandSpec{{*wlPaste, "--list-types"}},
            CommandSpec{{*wlPaste, "--no-newline"}},
            CommandSpec{{*wlPaste, "--no-newline", "--type", "text/html"}},
            CommandSpec{{*wlPaste, "--no-newline", "--type", "image/png"}},
            CommandSpec{{*wlCopy}},
            CommandSpec{{*wlPaste, "--no-newline", "--watch", *shell, "-c", "printf '\\0'"}} );
    }

    if (hasDisplay && xclip) {
        return std::make_unique<ExternalCommandClipboardBackend>(
            "xclip",
            CommandSpec{{*xclip, "-selection", "clipboard", "-t", "TARGETS", "-o"}},
            CommandSpec{{*xclip, "-selection", "clipboard", "-o"}},
            CommandSpec{{*xclip, "-selection", "clipboard", "-t", "text/html", "-o"}},
            CommandSpec{{*xclip, "-selection", "clipboard", "-t", "image/png", "-o"}},
            CommandSpec{{*xclip, "-selection", "clipboard", "-i"}});
    }

    if (hasDisplay && xsel) {
        return std::make_unique<ExternalCommandClipboardBackend>(
            "xsel",
            CommandSpec{}, // xsel doesn't easily list types
            CommandSpec{{*xsel, "--clipboard", "--output"}},
            CommandSpec{},
            CommandSpec{},
            CommandSpec{{*xsel, "--clipboard", "--input"}});
    }

    return nullptr;
}

} // namespace

std::optional<ClipboardPayload> ExternalCommandClipboardBackend::ReadPayload() {
    auto types = runReadCommand(m_listTypesCommand);
    ClipboardPayload payload;

    bool hasHtml = false;
    bool hasImage = false;
    if (types.has_value()) {
        hasHtml = types->find("text/html") != std::string::npos;
        hasImage = types->find("image/png") != std::string::npos || types->find("image/jpeg") != std::string::npos;
    }

    if (hasImage) {
        auto bytes = runReadBytesCommand(m_readImageCommand);
        if (bytes.has_value()) {
            payload.image = ClipboardImagePayload{"image/png", std::move(*bytes), std::nullopt, std::nullopt};
        }
    }

    if (hasHtml) {
        auto html = runReadCommand(m_readHtmlCommand);
        if (html.has_value()) {
            payload.html = parseHtmlClipboardValue(*html);
        }
    }

    payload.plainText = runReadCommand(m_readTextCommand);
    
    if (!payload.plainText && !payload.html && !payload.image) {
        return std::nullopt;
    }
    return payload;
}

bool ExternalCommandClipboardBackend::WritePayload(const ClipboardPayload& payload) {
    if (payload.image) {
        CommandSpec cmd = m_writeCommand;
        if (m_name.find("wl-clipboard") != std::string::npos) {
            cmd.argv.push_back("--type");
            cmd.argv.push_back(payload.image->mimeType);
        } else if (m_name == "xclip") {
            cmd.argv.push_back("-t");
            cmd.argv.push_back(payload.image->mimeType);
        }
        return runWriteBytesCommand(cmd, payload.image->bytes);
    }

    if (payload.html && payload.html->fragment && !payload.html->fragment->empty()) {
        if (m_name.find("wl-clipboard") != std::string::npos) {
            CommandSpec htmlCmd = m_writeCommand;
            htmlCmd.argv.push_back("--type");
            htmlCmd.argv.push_back("text/html");
            return runWriteCommand(htmlCmd, *payload.html->fragment);
        } else if (m_name == "xclip") {
            CommandSpec htmlCmd = m_writeCommand;
            htmlCmd.argv.push_back("-t");
            htmlCmd.argv.push_back("text/html");
            return runWriteCommand(htmlCmd, *payload.html->fragment);
        }
    }

    if (payload.plainText) {
        return runWriteCommand(m_writeCommand, *payload.plainText);
    }

    return false;
}

bool ExternalCommandClipboardBackend::WatchPayloadChanges(
    const std::atomic<bool>& running,
    const std::function<void(const ClipboardPayload&)>& onPayloadChange) {
    const auto readFn = [this]() { return ReadPayload(); };

    if (!m_watchReadCommand.argv.empty() && !m_watchSignalNeedle.empty()) {
        return runSignalWatchCommand(
            m_watchCommand,
            m_watchReadCommand,
            m_watchSignalNeedle,
            running,
            onPayloadChange,
            readFn);
    }

    return runWatchCommand(m_watchCommand, running, onPayloadChange, readFn);
}

ClipboardManager::ClipboardManager(std::unique_ptr<ClipboardBackend> backend)
    : m_backend(std::move(backend)) {}

std::unique_ptr<ClipboardManager> ClipboardManager::CreateDefault() {
    auto backend = createBackend();
    if (!backend) {
        return nullptr;
    }
    return std::make_unique<ClipboardManager>(std::move(backend));
}

std::string ClipboardManager::BackendName() {
    std::lock_guard<std::mutex> lock(m_backendMutex);
    return m_backend ? m_backend->Name() : "unavailable";
}

std::optional<ClipboardPayload> ClipboardManager::GetPayload() {
    std::lock_guard<std::mutex> lock(m_backendMutex);
    if (!m_backend) {
        return std::nullopt;
    }
    return m_backend->ReadPayload();
}

bool ClipboardManager::SetPayload(const ClipboardPayload& payload) {
    std::lock_guard<std::mutex> lock(m_backendMutex);
    return m_backend && m_backend->WritePayload(payload);
}

bool ClipboardManager::WatchPayloadChanges(
    const std::atomic<bool>& running,
    const std::function<void(const ClipboardPayload&)>& onPayloadChange) {
    ClipboardBackend* backend = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_backendMutex);
        backend = m_backend.get();
    }

    return backend && backend->WatchPayloadChanges(running, onPayloadChange);
}

std::function<std::optional<ClipboardPayload>()> ClipboardManager::MakeProvider() {
    return [this]() { return GetPayload(); };
}

std::function<void(const ClipboardPayload&)> ClipboardManager::MakeConsumer() {
    return [this](const ClipboardPayload& payload) {
        (void)SetPayload(payload);
    };
}

std::vector<uint8_t> ClipboardManager::EncodeTextPayload(const std::string& text) {
    const std::u16string taggedText =
        utf8ToUtf16(std::string(kClipboardTextPrefix) + text + kClipboardTextSeparator);
    return deflateRaw(encodeUtf16Le(taggedText));
}

std::vector<uint8_t> ClipboardManager::EncodeImagePayload(const std::vector<uint8_t>& bytes) {
    return bytes; // MWB images are often sent uncompressed or with their own compression over the socket
}

ClipboardPayload ClipboardManager::MakeTextPayload(const std::string& text) {
    ClipboardPayload payload;
    payload.plainText = text;
    return payload;
}

ClipboardPayload ClipboardManager::MakeImagePayload(std::string mimeType, std::vector<uint8_t> bytes) {
    ClipboardPayload payload;
    payload.image = ClipboardImagePayload{std::move(mimeType), std::move(bytes), std::nullopt, std::nullopt};
    return payload;
}

std::optional<ClipboardPayload> ClipboardManager::DecodePayload(const std::vector<uint8_t>& payload) {
    auto inflated = inflateRaw(payload, kClipboardTextInflatedMaxSize);
    if (!inflated.has_value()) {
        return std::nullopt;
    }

    const std::u16string utf16 = decodeUtf16Le(*inflated);
    const std::string utf8 = utf16ToUtf8(utf16);
    return parseTextClipboardPayload(utf8);
}

std::optional<std::string> ClipboardManager::DecodeTextPayload(const std::vector<uint8_t>& payload) {
    auto decoded = DecodePayload(payload);
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    return decoded->plainText;
}

std::optional<std::vector<uint8_t>> ClipboardManager::DecodeImagePayload(const std::vector<uint8_t>& payload) {
    return payload; // Identity for now
}


std::vector<uint8_t> ClipboardManager::EncodeSocketHeader(std::size_t payloadSize, const std::string& kind) {
    const std::u16string header = utf8ToUtf16(std::to_string(payloadSize) + "*" + kind);
    std::vector<uint8_t> encoded = encodeUtf16Le(header);
    encoded.resize(kClipboardSocketHeaderSize, 0);
    return encoded;
}

bool ClipboardManager::DecodeSocketHeader(
    const std::vector<uint8_t>& header,
    std::size_t& payloadSize,
    std::string& kind) {
    if (header.size() < kClipboardSocketHeaderSize) {
        return false;
    }

    std::string decoded = trimNulsAndWhitespace(utf16ToUtf8(decodeUtf16Le(header)));
    const std::size_t separator = decoded.find('*');
    if (separator == std::string::npos) {
        return false;
    }

    try {
        payloadSize = static_cast<std::size_t>(std::stoull(decoded.substr(0, separator)));
    } catch (...) {
        return false;
    }

    kind = decoded.substr(separator + 1);
    return !kind.empty();
}

} // namespace mwb
