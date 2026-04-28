#pragma once

#include <string>

namespace mwb {

bool SecretStoreIsAvailable(std::string* errorMessage = nullptr);
bool LookupSecretKey(const std::string& secretId, std::string& outKey, std::string* errorMessage = nullptr);
bool StoreSecretKey(const std::string& secretId, const std::string& key, std::string* errorMessage = nullptr);
bool ClearSecretKey(const std::string& secretId, std::string* errorMessage = nullptr);

} // namespace mwb
