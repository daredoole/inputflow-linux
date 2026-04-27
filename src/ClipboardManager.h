#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mwb {

struct ClipboardHtmlPayload {
    std::string raw;
    std::optional<std::size_t> startHtml;
    std::optional<std::size_t> endHtml;
    std::optional<std::size_t> startFragment;
    std::optional<std::size_t> endFragment;
    std::optional<std::string> fragment;
    std::optional<std::string> plainText;
};

struct ClipboardImagePayload {
    std::string mimeType;
    std::vector<uint8_t> bytes;
    std::optional<uint32_t> width;
    std::optional<uint32_t> height;
};

struct ClipboardPayload {
    std::optional<std::string> plainText;
    std::optional<ClipboardHtmlPayload> html;
    std::optional<ClipboardImagePayload> image;
};

class ClipboardBackend {
public:
    virtual ~ClipboardBackend() = default;

    virtual std::optional<std::string> ReadText() = 0;
    virtual bool WriteText(const std::string& text) = 0;
    virtual std::string Name() const = 0;
    virtual bool WatchTextChanges(
        const std::atomic<bool>& running,
        const std::function<void(const std::string&)>& onTextChange) {
        return false;
    }
};

class ClipboardManager {
public:
    explicit ClipboardManager(std::unique_ptr<ClipboardBackend> backend);

    static std::unique_ptr<ClipboardManager> CreateDefault();

    std::string BackendName();
    std::optional<std::string> GetText();
    bool SetText(const std::string& text);
    bool WatchTextChanges(
        const std::atomic<bool>& running,
        const std::function<void(const std::string&)>& onTextChange);

    std::function<std::optional<std::string>()> MakeProvider();
    std::function<void(const std::string&)> MakeConsumer();

    static std::vector<uint8_t> EncodeTextPayload(const std::string& text);
    static ClipboardPayload MakeTextPayload(const std::string& text);
    static ClipboardPayload MakeImagePayload(std::string mimeType, std::vector<uint8_t> bytes);
    static std::optional<ClipboardPayload> DecodePayload(const std::vector<uint8_t>& payload);
    static std::optional<std::string> DecodeTextPayload(const std::vector<uint8_t>& payload);

    static std::vector<uint8_t> EncodeSocketHeader(std::size_t payloadSize, const std::string& kind);
    static bool DecodeSocketHeader(
        const std::vector<uint8_t>& header,
        std::size_t& payloadSize,
        std::string& kind);

private:
    std::mutex m_backendMutex;
    std::unique_ptr<ClipboardBackend> m_backend;
};

} // namespace mwb
