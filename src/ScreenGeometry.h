#pragma once

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace mwb {

struct ScreenGeometry {
    int x{0};
    int y{0};
    int width{0};
    int height{0};
};

inline std::string StripAnsiEscapeCodes(std::string_view text) {
    std::string output;
    output.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '\x1b') {
            output.push_back(text[index]);
            continue;
        }

        if (index + 1 >= text.size() || text[index + 1] != '[') {
            continue;
        }

        index += 2;
        while (index < text.size()) {
            const unsigned char ch = static_cast<unsigned char>(text[index]);
            if (ch >= 0x40 && ch <= 0x7e) {
                break;
            }
            ++index;
        }
    }

    return output;
}

inline std::optional<int> ParsePositiveIntToken(std::string_view token, bool allowZero = false) {
    if (token.empty()) {
        return std::nullopt;
    }

    int value = 0;
    for (const char ch : token) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return std::nullopt;
        }
        value = value * 10 + (ch - '0');
    }

    if (value < 0 || (!allowZero && value == 0)) {
        return std::nullopt;
    }
    return value;
}

inline std::optional<ScreenGeometry> ParseKScreenDoctorGeometryLine(std::string_view line) {
    constexpr std::string_view kGeometryLabel = "Geometry:";
    const std::size_t labelPos = line.find(kGeometryLabel);
    if (labelPos == std::string_view::npos) {
        return std::nullopt;
    }

    line = line.substr(labelPos + kGeometryLabel.size());
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) {
        line.remove_prefix(1);
    }

    const std::size_t firstComma = line.find(',');
    const std::size_t space = line.find(' ');
    const std::size_t xSep = line.find('x', space == std::string_view::npos ? 0 : space + 1);
    if (firstComma == std::string_view::npos || space == std::string_view::npos || xSep == std::string_view::npos) {
        return std::nullopt;
    }

    const auto x = ParsePositiveIntToken(line.substr(0, firstComma), true);
    const auto y = ParsePositiveIntToken(line.substr(firstComma + 1, space - firstComma - 1), true);
    const auto width = ParsePositiveIntToken(line.substr(space + 1, xSep - space - 1));

    std::size_t heightEnd = xSep + 1;
    while (heightEnd < line.size() && std::isdigit(static_cast<unsigned char>(line[heightEnd]))) {
        ++heightEnd;
    }
    const auto height = ParsePositiveIntToken(line.substr(xSep + 1, heightEnd - xSep - 1));

    if (!x || !y || !width || !height) {
        return std::nullopt;
    }

    return ScreenGeometry{*x, *y, *width, *height};
}

inline std::optional<std::pair<int, int>> ParseKScreenDoctorLogicalScreenSize(std::string_view rawOutput) {
    const std::string output = StripAnsiEscapeCodes(rawOutput);
    std::istringstream stream(output);
    std::string line;

    bool currentOutputSeen = false;
    bool currentEnabled = false;
    bool currentConnected = false;
    std::optional<ScreenGeometry> currentGeometry;

    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int maxX = std::numeric_limits<int>::min();
    int maxY = std::numeric_limits<int>::min();
    int activeOutputs = 0;

    const auto flushCurrent = [&]() {
        if (!currentOutputSeen || !currentEnabled || !currentConnected || !currentGeometry.has_value()) {
            return;
        }

        minX = std::min(minX, currentGeometry->x);
        minY = std::min(minY, currentGeometry->y);
        maxX = std::max(maxX, currentGeometry->x + currentGeometry->width);
        maxY = std::max(maxY, currentGeometry->y + currentGeometry->height);
        ++activeOutputs;
    };

    while (std::getline(stream, line)) {
        if (line.rfind("Output:", 0) == 0) {
            flushCurrent();
            currentOutputSeen = true;
            currentEnabled = false;
            currentConnected = false;
            currentGeometry.reset();
            continue;
        }

        const std::string trimmed = [&]() {
            std::string text = line;
            const auto begin = std::find_if_not(text.begin(), text.end(), [](unsigned char ch) {
                return std::isspace(ch);
            });
            text.erase(text.begin(), begin);
            const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char ch) {
                return std::isspace(ch);
            }).base();
            text.erase(end, text.end());
            return text;
        }();

        if (trimmed == "enabled") {
            currentEnabled = true;
        } else if (trimmed == "connected") {
            currentConnected = true;
        } else if (const auto geometry = ParseKScreenDoctorGeometryLine(trimmed); geometry.has_value()) {
            currentGeometry = geometry;
        }
    }

    flushCurrent();

    if (activeOutputs == 0 || minX >= maxX || minY >= maxY) {
        return std::nullopt;
    }

    return std::pair<int, int>{maxX - minX, maxY - minY};
}

} // namespace mwb
