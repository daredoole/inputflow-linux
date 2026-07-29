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

    virtual std::optional<ClipboardPayload> ReadPayload() = 0;
    virtual bool WritePayload(const ClipboardPayload& payload) = 0;
    virtual std::string Name() const = 0;
    virtual bool WatchPayloadChanges(
        const std::atomic<bool>&,
        const std::function<void(const ClipboardPayload&)>&) {
        return false;
    }
};

class ClipboardManager {
public:
    explicit ClipboardManager(std::unique_ptr<ClipboardBackend> backend);

    static std::unique_ptr<ClipboardManager> CreateDefault();

    std::string BackendName();
    std::optional<ClipboardPayload> GetPayload();
    bool SetPayload(const ClipboardPayload& payload);
    bool WatchPayloadChanges(
        const std::atomic<bool>& running,
        const std::function<void(const ClipboardPayload&)>& onPayloadChange);

    std::function<std::optional<ClipboardPayload>()> MakeProvider();
    std::function<void(const ClipboardPayload&)> MakeConsumer();

    static std::vector<uint8_t> EncodeTextPayload(const std::string& text);
    static std::vector<uint8_t> EncodeImagePayload(const std::vector<uint8_t>& bytes);
    static ClipboardPayload MakeTextPayload(const std::string& text);
    static ClipboardPayload MakeImagePayload(std::string mimeType, std::vector<uint8_t> bytes);
    static std::optional<ClipboardPayload> DecodePayload(const std::vector<uint8_t>& payload);
    static std::optional<std::string> DecodeTextPayload(const std::vector<uint8_t>& payload);
    static std::optional<std::vector<uint8_t>> DecodeImagePayload(const std::vector<uint8_t>& payload);

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
