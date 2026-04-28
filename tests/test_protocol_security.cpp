#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <zlib.h>

#include "ClipboardManager.h"
#include "Protocol.h"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

std::vector<uint8_t> DeflateRawForTest(const std::vector<uint8_t>& input) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return {};
    }

    std::vector<uint8_t> output;
    std::array<uint8_t, 256> buffer{};
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

std::vector<uint8_t> EncodeClipboardEntries(std::initializer_list<std::string> entries) {
    std::string combined;
    bool first = true;
    for (const auto& entry : entries) {
        if (!first) {
            combined += mwb::kClipboardTextSeparator;
        }
        combined += entry;
        first = false;
    }

    std::vector<uint8_t> utf16Le;
    utf16Le.reserve(combined.size() * 2);
    for (const unsigned char ch : combined) {
        utf16Le.push_back(ch);
        utf16Le.push_back(0);
    }

    return DeflateRawForTest(utf16Le);
}

std::string FormatOffset(std::size_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%010zu", value);
    return std::string(buffer);
}

std::string MakeCfHtmlPayload(const std::string& fragment) {
    constexpr char kHeaderTemplate[] =
        "Version:1.0\r\n"
        "StartHTML:0000000000\r\n"
        "EndHTML:0000000000\r\n"
        "StartFragment:0000000000\r\n"
        "EndFragment:0000000000\r\n";
    const std::string htmlPrefix = "<html><body>";
    const std::string htmlSuffix = "</body></html>";
    std::string header = kHeaderTemplate;
    const std::string body = htmlPrefix + fragment + htmlSuffix;

    const std::size_t startHtml = header.size();
    const std::size_t endHtml = startHtml + body.size();
    const std::size_t startFragment = startHtml + htmlPrefix.size();
    const std::size_t endFragment = startFragment + fragment.size();

    header.replace(header.find("StartHTML:") + std::strlen("StartHTML:"), 10, FormatOffset(startHtml));
    header.replace(header.find("EndHTML:") + std::strlen("EndHTML:"), 10, FormatOffset(endHtml));
    header.replace(header.find("StartFragment:") + std::strlen("StartFragment:"), 10, FormatOffset(startFragment));
    header.replace(header.find("EndFragment:") + std::strlen("EndFragment:"), 10, FormatOffset(endFragment));

    return "HTM" + header + body;
}

void TestHandshakeAckChallengeMatching() {
    std::array<uint8_t, mwb::kHandshakeChallengeSize> challenge{};
    for (std::size_t index = 0; index < challenge.size(); ++index) {
        challenge[index] = static_cast<uint8_t>(index * 13u);
    }

    mwb::MWBPacket packet{};
    packet.type = static_cast<uint8_t>(mwb::PackageType::HandshakeAck);
    for (std::size_t index = 0; index < challenge.size(); ++index) {
        packet.data[index] = static_cast<uint8_t>(~challenge[index]);
    }

    Expect(mwb::handshakeAckMatchesChallenge(packet, challenge),
           "Handshake ack should match the challenge complement");

    packet.data[3] ^= 0x01;
    Expect(!mwb::handshakeAckMatchesChallenge(packet, challenge),
           "Handshake ack should reject mismatched challenge bytes");
}

void TestEstablishedSessionTargeting() {
    constexpr uint32_t myId = 0x11223344u;
    constexpr uint32_t peerId = 0x55667788u;

    Expect(mwb::packetTargetsEstablishedSession(myId, peerId, myId, peerId),
           "Exact source and destination should match the active session");
    Expect(mwb::packetTargetsEstablishedSession(mwb::kBroadcastMachineId, peerId, myId, peerId),
           "Broadcast packets from the active peer should match the active session");
    Expect(!mwb::packetTargetsEstablishedSession(myId, 0, myId, peerId),
           "Packets without a peer source should not match an established session");
    Expect(!mwb::packetTargetsEstablishedSession(myId, 0x99u, myId, peerId),
           "Packets from another source should not match an established session");
    Expect(!mwb::packetTargetsEstablishedSession(myId, peerId, myId, 0),
           "Packets should not match until the remote machine id is known");
}

void TestPeerAssignedMachineIdDetection() {
    Expect(!mwb::isPeerAssignedMachineId(0), "Zero should not be treated as a peer-assigned machine id");
    Expect(!mwb::isPeerAssignedMachineId(mwb::kBroadcastMachineId),
           "Broadcast machine id should not be treated as peer-assigned");
    Expect(!mwb::isPeerAssignedMachineId(0xFFFFFFFFu),
           "All-ones machine id should not be treated as peer-assigned");
    Expect(mwb::isPeerAssignedMachineId(0x11223344u),
           "Normal machine ids should be treated as peer-assigned");
}

void TestKeyboardAndMouseValidators() {
    Expect(mwb::isRecognizedMouseMessage(0x0200), "Known mouse messages should be accepted");
    Expect(!mwb::isRecognizedMouseMessage(0x9999), "Unknown mouse messages should be rejected");

    Expect(mwb::hasOnlyKnownKeyboardFlags(mwb::kKnownKeyboardFlagsMask),
           "Known keyboard flags should be accepted");
    Expect(!mwb::hasOnlyKnownKeyboardFlags(mwb::kKnownKeyboardFlagsMask | 0x80000000u),
           "Unknown keyboard flags should be rejected");
}

void TestClipboardDecodePrefersPlainTextEntry() {
    const std::string html =
        "HTMVersion:0.9\r\n"
        "StartHTML:0000000105\r\n"
        "EndHTML:0000000199\r\n"
        "StartFragment:0000000137\r\n"
        "EndFragment:0000000168\r\n"
        "<html><body><!--StartFragment--><div>Hello&nbsp;<b>world</b><br>line 2</div><!--EndFragment--></body></html>";

    const auto decoded = mwb::ClipboardManager::DecodeTextPayload(
        EncodeClipboardEntries({html, std::string(mwb::kClipboardTextPrefix) + "plain clipboard text"}));

    Expect(decoded == std::optional<std::string>("plain clipboard text"),
           "Clipboard decode should prefer explicit TXT entries over HTML fallbacks");
}

void TestClipboardPayloadExposesPlainTextEntry() {
    const auto decoded = mwb::ClipboardManager::DecodePayload(
        EncodeClipboardEntries({std::string(mwb::kClipboardTextPrefix) + "plain clipboard text"}));

    Expect(decoded.has_value(), "Clipboard payload decode should accept a valid text payload");
    Expect(decoded.has_value() && decoded->plainText == std::optional<std::string>("plain clipboard text"),
           "Clipboard payload should expose explicit TXT entries as plain text");
    Expect(decoded.has_value() && !decoded->html.has_value(),
           "Clipboard payload should not invent HTML metadata for plain text");
}

void TestClipboardDecodeNormalizesCfHtmlFallback() {
    const std::string html =
        "HTMVersion:0.9\r\n"
        "StartHTML:0000000105\r\n"
        "EndHTML:0000000199\r\n"
        "StartFragment:0000000137\r\n"
        "EndFragment:0000000168\r\n"
        "<html><body><!--StartFragment--><div>Hello&nbsp;<b>world</b><br>line 2</div><!--EndFragment--></body></html>";

    const auto decoded = mwb::ClipboardManager::DecodeTextPayload(EncodeClipboardEntries({html}));

    Expect(decoded == std::optional<std::string>("Hello world\nline 2"),
           "Clipboard decode should convert CF_HTML fallback content into plain text");
}

void TestClipboardPayloadRetainsCfHtmlRawAndMetadata() {
    const std::string fragment = "<p>&#72;ello &amp; &#x41;</p><ul><li>One</li><li>Two</li></ul>";
    const std::string html = MakeCfHtmlPayload(fragment);

    const auto decoded = mwb::ClipboardManager::DecodePayload(EncodeClipboardEntries({html}));

    Expect(decoded.has_value(), "Clipboard payload decode should accept CF_HTML text payloads");
    Expect(decoded.has_value() && decoded->html.has_value(),
           "Clipboard payload should preserve CF_HTML metadata");
    if (!decoded.has_value() || !decoded->html.has_value()) {
        return;
    }

    const auto& htmlPayload = *decoded->html;
    Expect(htmlPayload.raw == html.substr(std::strlen(mwb::kClipboardHtmlPrefix)),
           "Clipboard payload should retain raw CF_HTML bytes after the HTM tag");
    Expect(htmlPayload.startHtml.has_value() && htmlPayload.endHtml.has_value() &&
               htmlPayload.startFragment.has_value() && htmlPayload.endFragment.has_value(),
           "Clipboard payload should parse CF_HTML offsets");
    Expect(htmlPayload.fragment == std::optional<std::string>(fragment),
           "Clipboard payload should preserve the CF_HTML fragment");
    Expect(htmlPayload.plainText == std::optional<std::string>("Hello & A\nOne\nTwo"),
           "Clipboard payload should keep normalized CF_HTML fallback text");
    Expect(decoded->plainText == htmlPayload.plainText,
           "Clipboard payload should expose HTML fallback as legacy plain text");
}

void TestClipboardPayloadPreservesCfHtmlSuffixPadding() {
    const std::string fragment = "<div>Hello&nbsp;world</div>";
    const std::string html = MakeCfHtmlPayload(fragment);
    const std::string suffix(" \r\n\0", 4);

    const auto decoded = mwb::ClipboardManager::DecodePayload(EncodeClipboardEntries({html + suffix}));

    Expect(decoded.has_value() && decoded->html.has_value(),
           "Clipboard payload should decode padded CF_HTML entries");
    if (!decoded.has_value() || !decoded->html.has_value()) {
        return;
    }

    Expect(decoded->html->raw == html.substr(std::strlen(mwb::kClipboardHtmlPrefix)) + suffix,
           "Clipboard payload should preserve CF_HTML suffix padding in raw data");
    Expect(decoded->html->plainText == std::optional<std::string>("Hello world"),
           "Clipboard payload should normalize padded CF_HTML without shifting offsets");
}

void TestClipboardPayloadPrefersPlainTextOverHtml() {
    const std::string html = MakeCfHtmlPayload("<div>HTML only</div>");

    const auto decoded = mwb::ClipboardManager::DecodePayload(
        EncodeClipboardEntries({html, std::string(mwb::kClipboardTextPrefix) + "plain clipboard text"}));

    Expect(decoded.has_value() && decoded->plainText == std::optional<std::string>("plain clipboard text"),
           "Clipboard payload should preserve TXT-over-HTML precedence");
    Expect(decoded.has_value() && decoded->html.has_value(),
           "Clipboard payload should retain HTML even when TXT wins legacy text selection");
}

void TestClipboardDecodeNormalizesOffsetBasedCfHtml() {
    const std::string html = MakeCfHtmlPayload("<p>&#72;ello &amp; &#x41;</p><ul><li>One</li><li>Two</li></ul>");

    const auto decoded = mwb::ClipboardManager::DecodeTextPayload(EncodeClipboardEntries({html}));

    Expect(decoded == std::optional<std::string>("Hello & A\nOne\nTwo"),
           "Clipboard decode should honor CF_HTML fragment offsets and decode HTML entities");
}

void TestClipboardPayloadInvalidDeflateReturnsNullopt() {
    const std::vector<uint8_t> malformed;

    Expect(!mwb::ClipboardManager::DecodePayload(malformed).has_value(),
           "Clipboard payload decode should reject malformed compressed data");
    Expect(!mwb::ClipboardManager::DecodeTextPayload(malformed).has_value(),
           "Clipboard text decode should continue rejecting malformed compressed data");
}

} // namespace

int main() {
    TestHandshakeAckChallengeMatching();
    TestEstablishedSessionTargeting();
    TestPeerAssignedMachineIdDetection();
    TestKeyboardAndMouseValidators();
    TestClipboardDecodePrefersPlainTextEntry();
    TestClipboardPayloadExposesPlainTextEntry();
    TestClipboardDecodeNormalizesCfHtmlFallback();
    TestClipboardPayloadRetainsCfHtmlRawAndMetadata();
    TestClipboardPayloadPreservesCfHtmlSuffixPadding();
    TestClipboardPayloadPrefersPlainTextOverHtml();
    TestClipboardDecodeNormalizesOffsetBasedCfHtml();
    TestClipboardPayloadInvalidDeflateReturnsNullopt();

    if (g_failures != 0) {
        std::cerr << g_failures << " protocol security test(s) failed." << std::endl;
        return 1;
    }

    std::cout << "Protocol security tests passed." << std::endl;
    return 0;
}
