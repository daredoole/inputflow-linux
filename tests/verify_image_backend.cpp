#include "ClipboardManager.h"
#include <iostream>
#include <vector>

int main() {
    auto manager = mwb::ClipboardManager::CreateDefault();
    if (!manager) {
        std::cerr << "Failed to create clipboard manager" << std::endl;
        return 1;
    }

    std::cout << "Using backend: " << manager->BackendName() << std::endl;

    auto payload = manager->GetPayload();
    if (!payload) {
        std::cout << "Clipboard is empty" << std::endl;
        return 0;
    }

    if (payload->image) {
        std::cout << "SUCCESS: Detected image payload!" << std::endl;
        std::cout << "MIME type: " << payload->image->mimeType << std::endl;
        std::cout << "Size: " << payload->image->bytes.size() << " bytes" << std::endl;
    } else if (payload->plainText) {
        std::cout << "Detected text: " << *payload->plainText << std::endl;
        std::cout << "No image found in clipboard." << std::endl;
    } else {
        std::cout << "Detected unknown payload type." << std::endl;
    }

    return 0;
}
