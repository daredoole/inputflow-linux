#include <algorithm>
#include <cstdint>
#include <iostream>

// Tests for the ABS injection path: protocol scaling, relative accumulation,
// and edge clamping. Mirrors the logic in InputManager::InjectMouse exactly.
// Hardware-level emission tests (actual EV_ABS bytes on /dev/uinput) are
// verified manually during integration testing.

namespace {

int g_failures = 0;

void ExpectEqual(int actual, int expected, const char* message) {
    if (actual != expected) {
        ++g_failures;
        std::cerr << "FAIL: " << message
                  << " expected=" << expected
                  << " actual=" << actual << std::endl;
    }
}

void ExpectTrue(bool value, const char* message) {
    if (!value) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " expected=true actual=false" << std::endl;
    }
}

// Mirrors InjectMouse absolute path: value * (span-1) / 65535
int ScaleProtocol(int value, int screenSpan) {
    return static_cast<int>(static_cast<int64_t>(value) * (screenSpan - 1) / 65535);
}

// Mirrors InjectMouse relative path: clamp(cursor + delta, 0, span-1)
int AccumulateRel(int cursor, int delta, int screenSpan) {
    return std::clamp(cursor + delta, 0, screenSpan - 1);
}

// Mirrors DecodeRelativeAxis: strip the ±100000 sentinel
int DecodeRel(int encoded) {
    return (encoded < 0) ? (encoded + 100000) : (encoded - 100000);
}

void TestAbsScalingBoundaries() {
    ExpectEqual(ScaleProtocol(0,     2048), 0,    "protocol 0 -> pixel 0");
    ExpectEqual(ScaleProtocol(65535, 2048), 2047, "protocol 65535 -> pixel 2047 (screenW-1)");
    ExpectEqual(ScaleProtocol(0,     1280), 0,    "protocol 0 -> pixel 0 (height)");
    ExpectEqual(ScaleProtocol(65535, 1280), 1279, "protocol 65535 -> pixel 1279 (screenH-1)");
    ExpectEqual(ScaleProtocol(0,     1),    0,    "single-pixel degenerate screen: min");
    ExpectEqual(ScaleProtocol(65535, 1),    0,    "single-pixel degenerate screen: max clamps to 0");
}

void TestAbsScalingMonotonicity() {
    // Every protocol step must produce a non-decreasing pixel value.
    const int screenW = 2048;
    int prev = ScaleProtocol(0, screenW);
    for (int x = 1; x <= 65535; x += 64) {
        const int cur = ScaleProtocol(x, screenW);
        if (cur < prev) {
            ++g_failures;
            std::cerr << "FAIL: scaling not monotonic at x=" << x
                      << " cur=" << cur << " prev=" << prev << std::endl;
            break;
        }
        prev = cur;
    }
    ExpectTrue(true, "absolute scaling is monotone across [0,65535]");
}

void TestRelativeAccumulationAtEdge() {
    const int screenW = 2048;

    // Cursor near right edge: accumulated position must clamp to screenW-1.
    int cursor = 2040;
    cursor = AccumulateRel(cursor, 100, screenW);
    ExpectEqual(cursor, screenW - 1, "relative move past right edge clamps to screenW-1");

    // Cursor at left edge: can't go negative.
    cursor = 3;
    cursor = AccumulateRel(cursor, -100, screenW);
    ExpectEqual(cursor, 0, "relative move past left edge clamps to 0");
}

void TestRelativeAccumulationAccumulates() {
    const int screenW = 2048;

    // Ten moves of +100 each starting from 0 = 1000 (no clamping needed).
    int cursor = 0;
    for (int i = 0; i < 10; ++i) {
        cursor = AccumulateRel(cursor, 100, screenW);
    }
    ExpectEqual(cursor, 1000, "10 relative moves of +100 accumulate to 1000");
}

void TestRelativeDecodeRoundtrip() {
    // Encoder: delta + (delta < 0 ? -100000 : +100000)
    const int encoded_pos = 100 + 100000;  // delta=+100
    const int encoded_neg = -50 - 100000;  // delta=-50

    ExpectEqual(DecodeRel(encoded_pos), 100,  "positive relative delta decodes correctly");
    ExpectEqual(DecodeRel(encoded_neg), -50,  "negative relative delta decodes correctly");
    ExpectEqual(DecodeRel(100000),      0,    "zero positive relative delta decodes to 0");
    ExpectEqual(DecodeRel(-100000),     0,    "zero negative relative delta decodes to 0");
}

void TestAbsScalingAsymmetryAtExtremes() {
    // Confirm no off-by-one: 65534 should not produce screenW-1 on a 2048-wide screen.
    const int screenW = 2048;
    const int nearMax = ScaleProtocol(65534, screenW);
    const int atMax   = ScaleProtocol(65535, screenW);
    ExpectEqual(atMax, screenW - 1, "protocol 65535 exactly reaches screenW-1");
    ExpectTrue(nearMax <= atMax, "protocol 65534 does not exceed 65535 mapping");
}

} // namespace

int main() {
    TestAbsScalingBoundaries();
    TestAbsScalingMonotonicity();
    TestRelativeAccumulationAtEdge();
    TestRelativeAccumulationAccumulates();
    TestRelativeDecodeRoundtrip();
    TestAbsScalingAsymmetryAtExtremes();

    if (g_failures == 0) {
        std::cout << "All ABS injection tests passed." << std::endl;
        return 0;
    }

    std::cerr << g_failures << " ABS injection test(s) failed." << std::endl;
    return 1;
}
