#include "CryptoHelper.h"
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <array>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <limits>

namespace mwb {
namespace {

class CleanseGuard {
public:
    CleanseGuard(void* data, std::size_t size) : m_data(data), m_size(size) {}
    ~CleanseGuard() { OPENSSL_cleanse(m_data, m_size); }

    CleanseGuard(const CleanseGuard&) = delete;
    CleanseGuard& operator=(const CleanseGuard&) = delete;

private:
    void* m_data;
    std::size_t m_size;
};

void Cleanse(std::vector<uint8_t>& value) {
    if (!value.empty()) {
        OPENSSL_cleanse(value.data(), value.size());
    }
}

uint32_t Compute24BitHash(const std::string& securityKey) {
    std::array<uint8_t, 32> bytes{};
    std::array<uint8_t, 64> hashValue{};
    CleanseGuard bytesGuard(bytes.data(), bytes.size());
    CleanseGuard hashGuard(hashValue.data(), hashValue.size());

    for (std::size_t index = 0; index < bytes.size() && index < securityKey.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(securityKey[index]);
    }

    using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    DigestContext context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context) {
        throw std::runtime_error("SHA512 context allocation failed");
    }

    unsigned int length = 0;
    if (EVP_DigestInit_ex(context.get(), EVP_sha512(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1 ||
        EVP_DigestFinal_ex(context.get(), hashValue.data(), &length) != 1 ||
        length != hashValue.size()) {
        throw std::runtime_error("SHA512 hash failed");
    }

    for (int iteration = 0; iteration < 50000; ++iteration) {
        if (EVP_DigestInit_ex(context.get(), EVP_sha512(), nullptr) != 1 ||
            EVP_DigestUpdate(context.get(), hashValue.data(), hashValue.size()) != 1 ||
            EVP_DigestFinal_ex(context.get(), hashValue.data(), &length) != 1 ||
            length != hashValue.size()) {
            throw std::runtime_error("SHA512 hash failed");
        }
    }

    // Match C# Encryption.Get24BitHash exactly.
    return (static_cast<uint32_t>(hashValue[0]) << 23) +
           (static_cast<uint32_t>(hashValue[1]) << 16) +
           (static_cast<uint32_t>(hashValue[63]) << 8) +
           static_cast<uint32_t>(hashValue[2]);
}

} // namespace

CryptoHelper::CryptoHelper(const std::string& securityKey) : m_encryptCtx(nullptr), m_decryptCtx(nullptr) {
    if (securityKey.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("Security key is too large");
    }

    m_magicHash = Compute24BitHash(securityKey);

    std::string ivStr = "1844674407370955";
    m_iv.resize(16);
    std::memcpy(m_iv.data(), ivStr.data(), 16);

    std::string fullIv = "18446744073709551615";
    std::vector<uint8_t> salt;
    for (char c : fullIv) {
        salt.push_back(c);
        salt.push_back(0);
    }

    m_key.resize(32);
    if (!PKCS5_PBKDF2_HMAC(securityKey.c_str(), static_cast<int>(securityKey.length()),
                           salt.data(), static_cast<int>(salt.size()),
                           50000, EVP_sha512(),
                           32, m_key.data())) {
        Cleanse(m_key);
        throw std::runtime_error("PBKDF2 HMAC Failed");
    }

    auto failContextInit = [this](const char* message) {
        if (m_encryptCtx != nullptr) {
            EVP_CIPHER_CTX_free(m_encryptCtx);
            m_encryptCtx = nullptr;
        }
        if (m_decryptCtx != nullptr) {
            EVP_CIPHER_CTX_free(m_decryptCtx);
            m_decryptCtx = nullptr;
        }
        Cleanse(m_key);
        Cleanse(m_iv);
        throw std::runtime_error(message);
    };

    m_encryptCtx = EVP_CIPHER_CTX_new();
    if (m_encryptCtx == nullptr ||
        EVP_EncryptInit_ex(m_encryptCtx, EVP_aes_256_cbc(), nullptr, m_key.data(), m_iv.data()) != 1 ||
        EVP_CIPHER_CTX_set_padding(m_encryptCtx, 0) != 1) {
        failContextInit("AES encrypt context initialization failed");
    }

    m_decryptCtx = EVP_CIPHER_CTX_new();
    if (m_decryptCtx == nullptr ||
        EVP_DecryptInit_ex(m_decryptCtx, EVP_aes_256_cbc(), nullptr, m_key.data(), m_iv.data()) != 1 ||
        EVP_CIPHER_CTX_set_padding(m_decryptCtx, 0) != 1) {
        failContextInit("AES decrypt context initialization failed");
    }
}

CryptoHelper::~CryptoHelper() {
    if (m_encryptCtx) EVP_CIPHER_CTX_free(m_encryptCtx);
    if (m_decryptCtx) EVP_CIPHER_CTX_free(m_decryptCtx);
    Cleanse(m_key);
    Cleanse(m_iv);
}

uint32_t CryptoHelper::Get24BitHash() const {
    return m_magicHash;
}

void CryptoHelper::Reset() {
    if (EVP_EncryptInit_ex(m_encryptCtx, EVP_aes_256_cbc(), nullptr, m_key.data(), m_iv.data()) != 1 ||
        EVP_CIPHER_CTX_set_padding(m_encryptCtx, 0) != 1 ||
        EVP_DecryptInit_ex(m_decryptCtx, EVP_aes_256_cbc(), nullptr, m_key.data(), m_iv.data()) != 1 ||
        EVP_CIPHER_CTX_set_padding(m_decryptCtx, 0) != 1) {
        throw std::runtime_error("AES context reset failed");
    }
}

bool CryptoHelper::EncryptStream(const std::vector<uint8_t>& plaintext, std::vector<uint8_t>& ciphertext) {
    if (m_encryptCtx == nullptr ||
        plaintext.empty() ||
        plaintext.size() % 16 != 0 ||
        plaintext.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    ciphertext.resize(plaintext.size());
    int len = 0;
    if (EVP_EncryptUpdate(m_encryptCtx, ciphertext.data(), &len, plaintext.data(), static_cast<int>(plaintext.size())) != 1 ||
        len != static_cast<int>(plaintext.size())) {
        ciphertext.clear();
        return false;
    }
    return true;
}

bool CryptoHelper::DecryptStream(const std::vector<uint8_t>& ciphertext, std::vector<uint8_t>& plaintext) {
    if (m_decryptCtx == nullptr ||
        ciphertext.empty() ||
        ciphertext.size() % 16 != 0 ||
        ciphertext.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    plaintext.resize(ciphertext.size());
    int len = 0;
    if (EVP_DecryptUpdate(m_decryptCtx, plaintext.data(), &len, ciphertext.data(), static_cast<int>(ciphertext.size())) != 1 ||
        len != static_cast<int>(ciphertext.size())) {
        plaintext.clear();
        return false;
    }
    return true;
}

}
