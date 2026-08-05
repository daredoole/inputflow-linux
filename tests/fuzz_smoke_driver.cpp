#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size);

int main() {
    std::vector<uint8_t> input;
    uint32_t state = 0x9e3779b9U;
    for (std::size_t iteration = 0; iteration < 4096; ++iteration) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const std::size_t size = state % 4097;
        input.resize(size);
        for (std::size_t index = 0; index < input.size(); ++index) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            input[index] = static_cast<uint8_t>(state);
        }
        LLVMFuzzerTestOneInput(input.data(), input.size());
    }
    std::cout << "Deterministic parser fuzz smoke passed\n";
    return 0;
}
