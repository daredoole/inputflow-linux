#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <openssl/evp.h>

namespace mwb {

class CryptoHelper {
public:
    // Initializes the crypto subsystem using PBKDF2 SHA512 hashing
    CryptoHelper(const std::string& securityKey);
    ~CryptoHelper();
    CryptoHelper(const CryptoHelper&) = delete;
    CryptoHelper& operator=(const CryptoHelper&) = delete;

    // Stream-based AES operations. We feed raw bytes into an active context.
    bool EncryptStream(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& ciphertext);
    bool DecryptStream(const std::vector<uint8_t>& ciphertext, std::vector<uint8_t>& plaintext);

    uint32_t Get24BitHash();
    void Reset();

private:
    std::string m_securityKey;
    std::vector<uint8_t> m_key;
    std::vector<uint8_t> m_iv;

    EVP_CIPHER_CTX* m_encryptCtx;
    EVP_CIPHER_CTX* m_decryptCtx;
};

}
