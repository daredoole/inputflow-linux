#include "ClipboardManager.h"
#include "Protocol.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    if (data == nullptr || size > mwb::kClipboardSocketMaxSize) {
        return 0;
    }

    const std::vector<uint8_t> input(data, data + size);
    (void)mwb::ClipboardManager::DecodePayload(input);
    (void)mwb::ClipboardManager::DecodeTextPayload(input);
    (void)mwb::ClipboardManager::DecodeImagePayload(input);

    std::vector<uint8_t> header(mwb::kClipboardSocketHeaderSize, 0);
    const std::size_t headerBytes = std::min(header.size(), input.size());
    std::copy_n(input.begin(), headerBytes, header.begin());
    std::size_t payloadSize = 0;
    std::string kind;
    (void)mwb::ClipboardManager::DecodeSocketHeader(header, payloadSize, kind);

    if (!input.empty()) {
        const uint8_t type = input.front();
        (void)mwb::isBigPackage(type);
        (void)mwb::bigPacketCarriesMachineName(type);
    }
    return 0;
}
