#include "CryptoHelper.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

void TestHashCompatibilityAndStability() {
    mwb::CryptoHelper first("production-test-key");
    mwb::CryptoHelper second("production-test-key");
    mwb::CryptoHelper different("production-test-key-2");

    Expect(first.Get24BitHash() == second.Get24BitHash(),
           "The compatibility hash must be stable for the same key");
    Expect(first.Get24BitHash() != different.Get24BitHash(),
           "Different test keys should not share the compatibility hash");
}

void TestStreamRoundTripAndReset() {
    mwb::CryptoHelper encryptor("production-test-key");
    mwb::CryptoHelper decryptor("production-test-key");
    const std::vector<uint8_t> plaintext(64, 0x5a);
    std::vector<uint8_t> ciphertext;
    std::vector<uint8_t> decoded;

    Expect(encryptor.EncryptStream(plaintext, ciphertext),
           "A block-aligned payload should encrypt");
    Expect(ciphertext != plaintext, "Ciphertext should not equal plaintext");
    Expect(decryptor.DecryptStream(ciphertext, decoded),
           "A matching stream should decrypt");
    Expect(decoded == plaintext, "The decrypted stream should match the input");

    encryptor.Reset();
    decryptor.Reset();
    std::vector<uint8_t> resetCiphertext;
    std::vector<uint8_t> resetDecoded;
    Expect(encryptor.EncryptStream(plaintext, resetCiphertext),
           "Encryption should work after resetting stream state");
    Expect(resetCiphertext == ciphertext,
           "Reset must preserve the PowerToys-compatible stream initialization");
    Expect(decryptor.DecryptStream(resetCiphertext, resetDecoded) && resetDecoded == plaintext,
           "Decryption should work after resetting stream state");
}

void TestRejectsInvalidBlockLengths() {
    mwb::CryptoHelper crypto("production-test-key");
    std::vector<uint8_t> output;

    Expect(!crypto.EncryptStream({}, output), "Empty plaintext should be rejected");
    Expect(!crypto.EncryptStream(std::vector<uint8_t>(15, 0), output),
           "Non-block-aligned plaintext should be rejected");
    Expect(!crypto.DecryptStream({}, output), "Empty ciphertext should be rejected");
    Expect(!crypto.DecryptStream(std::vector<uint8_t>(17, 0), output),
           "Non-block-aligned ciphertext should be rejected");
}

} // namespace

int main() {
    TestHashCompatibilityAndStability();
    TestStreamRoundTripAndReset();
    TestRejectsInvalidBlockLengths();

    if (g_failures != 0) {
        std::cerr << g_failures << " crypto helper test(s) failed\n";
        return 1;
    }
    std::cout << "Crypto helper tests passed\n";
    return 0;
}
