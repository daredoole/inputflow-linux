#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mwb {

// Emulating PowerToys Mouse Without Borders message format.
// Reconstructed from Microsoft PowerToys Common.cs and Network structs.
enum class PackageType : uint8_t {
    Hi = 2,
    Hello = 3,
    ByeBye = 4,
    Heartbeat = 20,
    Awake = 21,
    Heartbeat_ex = 51,
    Heartbeat_ex_l2 = 52,
    Heartbeat_ex_l3 = 53,
    Clipboard = 69,
    ClipboardDataEnd = 76,
    ClipboardAsk = 78,
    ClipboardPush = 79,
    Heartbeat_v2 = 80,
    Keyboard = 122,
    Mouse = 123,
    ClipboardText = 124,
    ClipboardImage = 125,
    Handshake = 126,
    HandshakeAck = 127,
    Matrix = 128
};

constexpr std::size_t kSmallPacketSize = 32;
constexpr std::size_t kBigPacketSize = 64;
constexpr std::size_t kHandshakeChallengeSize = 16;
constexpr std::size_t kClipboardChunkSize = 48;
constexpr std::size_t kClipboardSocketHeaderSize = 1024;
constexpr std::size_t kClipboardInlineMaxSize = 16 * 1024 * 1024;
constexpr std::size_t kClipboardSocketMaxSize = 16 * 1024 * 1024;
constexpr std::size_t kClipboardTextInflatedMaxSize = 16 * 1024 * 1024;
constexpr int kClipboardPortOffset = -1;
constexpr uint32_t kBroadcastMachineId = 0x000000ff;
constexpr const char* kClipboardTextPrefix = "TXT";
constexpr const char* kClipboardHtmlPrefix = "HTM";
constexpr const char* kClipboardRtfPrefix = "RTF";
constexpr const char* kClipboardTextSeparator = "{4CFF57F7-BEDD-43d5-AE8F-27A61E886F2F}";
constexpr const char* kClipboardSocketTextLabel = "TXT";
constexpr const char* kClipboardSocketImageLabel = "IMG";
constexpr uint32_t kLlkhfExtended = 0x01;
constexpr uint32_t kLlkhfInjected = 0x10;
constexpr uint32_t kLlkhfAltDown = 0x20;
constexpr uint32_t kLlkhfUp = 0x80;
constexpr uint32_t kKnownKeyboardFlagsMask = kLlkhfExtended | kLlkhfInjected | kLlkhfAltDown | kLlkhfUp;

inline bool isBigPackage(uint8_t type) {
    if (type == static_cast<uint8_t>(PackageType::Hello) ||
        type == static_cast<uint8_t>(PackageType::Awake) ||
        type == static_cast<uint8_t>(PackageType::Heartbeat) ||
        type == static_cast<uint8_t>(PackageType::Heartbeat_ex) ||
        type == static_cast<uint8_t>(PackageType::Handshake) ||
        type == static_cast<uint8_t>(PackageType::HandshakeAck) ||
        type == static_cast<uint8_t>(PackageType::ClipboardPush) ||
        type == static_cast<uint8_t>(PackageType::Clipboard) ||
        type == static_cast<uint8_t>(PackageType::ClipboardAsk) ||
        type == static_cast<uint8_t>(PackageType::ClipboardImage) ||
        type == static_cast<uint8_t>(PackageType::ClipboardText) ||
        type == static_cast<uint8_t>(PackageType::ClipboardDataEnd))
    {
        return true;
    }
    return type == static_cast<uint8_t>(PackageType::Matrix);
}

inline bool bigPacketCarriesMachineName(uint8_t type) {
    return type == static_cast<uint8_t>(PackageType::Hello) ||
           type == static_cast<uint8_t>(PackageType::Awake) ||
           type == static_cast<uint8_t>(PackageType::Heartbeat) ||
           type == static_cast<uint8_t>(PackageType::Heartbeat_ex) ||
           type == static_cast<uint8_t>(PackageType::Clipboard) ||
           type == static_cast<uint8_t>(PackageType::ClipboardAsk) ||
           type == static_cast<uint8_t>(PackageType::ClipboardPush) ||
           type == static_cast<uint8_t>(PackageType::Handshake) ||
           type == static_cast<uint8_t>(PackageType::HandshakeAck);
}

inline bool isRecognizedMouseMessage(uint32_t wParam) {
    switch (wParam) {
        case 0x0200:
        case 0x0201:
        case 0x0202:
        case 0x0203:
        case 0x0204:
        case 0x0205:
        case 0x0206:
        case 0x0207:
        case 0x0208:
        case 0x0209:
        case 0x020A:
        case 0x020B:
        case 0x020C:
        case 0x020D:
        case 0x020E:
            return true;
        default:
            return false;
    }
}

inline uint16_t decodeXButtonPayload(int32_t mouseData) {
    const uint32_t raw = static_cast<uint32_t>(mouseData);
    const uint16_t high = static_cast<uint16_t>((raw >> 16) & 0xFFFFu);
    if (high == 0x0001u || high == 0x0002u) {
        return high;
    }

    const uint16_t low = static_cast<uint16_t>(raw & 0xFFFFu);
    if (high == 0u && (low == 0x0001u || low == 0x0002u)) {
        return low;
    }

    return 0u;
}

inline bool hasOnlyKnownKeyboardFlags(uint32_t flags) {
    return (flags & ~kKnownKeyboardFlagsMask) == 0;
}

inline bool packetTargetsMachine(uint32_t destination, uint32_t machineId) {
    return destination == 0 ||
           destination == machineId ||
           destination == kBroadcastMachineId ||
           destination == 0xFFFFFFFF;
}

inline bool isPeerAssignedMachineId(uint32_t machineId) {
    return machineId != 0 &&
           machineId != kBroadcastMachineId &&
           machineId != 0xFFFFFFFF;
}

inline bool packetTargetsEstablishedSession(
    uint32_t destination,
    uint32_t source,
    uint32_t machineId,
    uint32_t remoteMachineId) {
    return remoteMachineId != 0 &&
           source == remoteMachineId &&
           packetTargetsMachine(destination, machineId);
}

#pragma pack(push, 1)

// Unified 64-byte memory structure.
// Small packages only use and transmit the first 32 bytes.
struct MWBPacket {
    uint8_t type;         // Offset 0
    uint8_t checksum;     // Offset 1
    uint8_t magic0;       // Offset 2
    uint8_t magic1;       // Offset 3
    uint32_t id;          // Offset 4
    uint32_t src;         // Offset 8
    uint32_t des;         // Offset 12
    uint8_t data[48];     // Offset 16 onwards
};

// 16-byte mouse structure at Offset 16 (overlaps Machine1..4 in the DATA union).
// PowerToys sends either absolute normalized coordinates or relative-motion
// sentinel values when "Move relatively" is enabled:
//   data[0..3]  = x          (int32)
//   data[4..7]  = y          (int32)
//   data[8..11] = mouseData  (int32 wheel delta / xbutton payload)
//   data[12..15]= wParam     (uint32 WM_* message code:
//                          0x0200=MOUSEMOVE, 0x0201=LBUTTONDOWN, 0x0202=LBUTTONUP,
//                          0x0204=RBUTTONDOWN, 0x0205=RBUTTONUP,
//                          0x0207=MBUTTONDOWN, 0x0208=MBUTTONUP,
//                          0x020B=XBUTTONDOWN, 0x020C=XBUTTONUP, 0x020D=XBUTTONDBLCLK,
//                          0x020A=MOUSEWHEEL, 0x020E=MOUSEHWHEEL)
struct MouseData {
    int32_t  x;           // data[0..3]
    int32_t  y;           // data[4..7]
    int32_t  mouseData;   // data[8..11]
    uint32_t wParam;      // data[12..15] — WM_* mouse message code
};

// Compact 8-byte keyboard structure at Offset 24.
// PowerToys sends the low-level keyboard hook vkCode and LLKHF_* flags.
struct KeyboardData {
    int32_t  vkCode;      // data[8..11]
    uint32_t flags;       // data[12..15]
};

inline bool handshakeAckMatchesChallenge(
    const MWBPacket& packet,
    const std::array<uint8_t, kHandshakeChallengeSize>& challenge) {
    if (packet.type != static_cast<uint8_t>(PackageType::HandshakeAck)) {
        return false;
    }

    for (std::size_t index = 0; index < challenge.size(); ++index) {
        if (packet.data[index] != static_cast<uint8_t>(~challenge[index])) {
            return false;
        }
    }
    return true;
}

#pragma pack(pop)

static_assert(sizeof(MWBPacket) == kBigPacketSize, "MWBPacket wire size must stay 64 bytes");
static_assert(sizeof(MouseData) == 16, "MouseData wire size must stay 16 bytes");
static_assert(sizeof(KeyboardData) == 8, "KeyboardData wire size must stay 8 bytes");

} // namespace mwb
