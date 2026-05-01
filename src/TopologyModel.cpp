#include "TopologyModel.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace mwb {
namespace {

struct EdgeKey {
    std::string displayId;
    EdgeDirection edge{EdgeDirection::Right};

    bool operator<(const EdgeKey& other) const {
        if (displayId != other.displayId) {
            return displayId < other.displayId;
        }
        return static_cast<int>(edge) < static_cast<int>(other.edge);
    }
};

std::string_view trim(std::string_view value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(start, end - start);
}

std::string toLower(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

void setError(std::string* errorMessage, std::string message) {
    if (errorMessage != nullptr) {
        *errorMessage = std::move(message);
    }
}

std::vector<std::string> splitCommaList(std::string_view value) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        const size_t end = (comma == std::string_view::npos) ? value.size() : comma;
        parts.emplace_back(trim(value.substr(start, end - start)));
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return parts;
}

bool parseInt(std::string_view value, int& out) {
    const std::string text(trim(value));
    if (text.empty()) {
        return false;
    }

    try {
        size_t end = 0;
        const long long parsed = std::stoll(text, &end, 10);
        if (end != text.size() ||
            parsed < std::numeric_limits<int>::min() ||
            parsed > std::numeric_limits<int>::max()) {
            return false;
        }
        out = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<EdgeDirection> parseEdgeDirection(std::string_view value) {
    const std::string lowered = toLower(trim(value));
    if (lowered == "left") {
        return EdgeDirection::Left;
    }
    if (lowered == "right") {
        return EdgeDirection::Right;
    }
    if (lowered == "up") {
        return EdgeDirection::Up;
    }
    if (lowered == "down") {
        return EdgeDirection::Down;
    }
    return std::nullopt;
}

std::optional<WrapPolicy> parseWrapPolicy(std::string_view value) {
    const std::string lowered = toLower(trim(value));
    if (lowered == "none") {
        return WrapPolicy::None;
    }
    if (lowered == "horizontal") {
        return WrapPolicy::Horizontal;
    }
    if (lowered == "vertical") {
        return WrapPolicy::Vertical;
    }
    if (lowered == "both") {
        return WrapPolicy::Both;
    }
    return std::nullopt;
}

bool isAbsolutePointerCoordinate(int value) {
    return value >= 0 && value <= 65535;
}

int mapNormalizedCoordinate(int normalized, int length) {
    if (length <= 1) {
        return 0;
    }
    const int clamped = std::clamp(normalized, 0, 65535);
    return static_cast<int>(static_cast<long long>(clamped) * (length - 1) / 65535);
}

int normalizeCoordinate(int coordinate, int length) {
    if (length <= 1) {
        return 0;
    }
    const int clamped = std::clamp(coordinate, 0, length - 1);
    return static_cast<int>(static_cast<long long>(clamped) * 65535 / (length - 1));
}

int rightOf(const Display& display) {
    return display.x + display.width;
}

int bottomOf(const Display& display) {
    return display.y + display.height;
}

bool rangesOverlap(int startA, int endA, int startB, int endB) {
    return startA < endB && startB < endA;
}

bool displaysOverlap(const Display& a, const Display& b) {
    return rangesOverlap(a.x, rightOf(a), b.x, rightOf(b)) &&
           rangesOverlap(a.y, bottomOf(a), b.y, bottomOf(b));
}

int edgeLength(const Display& display, EdgeDirection edge) {
    switch (edge) {
    case EdgeDirection::Left:
    case EdgeDirection::Right:
        return display.height;
    case EdgeDirection::Up:
    case EdgeDirection::Down:
        return display.width;
    }
    return 0;
}

int mapCoordinate(int coordinate, int sourceLength, int targetLength) {
    if (targetLength <= 1 || sourceLength <= 1) {
        return 0;
    }

    const long long numerator =
        static_cast<long long>(coordinate) * static_cast<long long>(targetLength - 1);
    const long long denominator = sourceLength - 1;
    const int mapped = static_cast<int>((numerator + denominator / 2) / denominator);
    return std::max(0, std::min(mapped, targetLength - 1));
}

std::string describeEdge(const std::string& displayId, EdgeDirection edge) {
    std::ostringstream out;
    out << displayId << "." << edgeDirectionName(edge);
    return out.str();
}

void addIssue(std::vector<TopologyIssue>& issues, TopologyIssueCode code, std::string message) {
    issues.push_back({code, std::move(message)});
}

const Display* findDisplay(const std::vector<Display>& displays, const std::string& id) {
    for (const auto& display : displays) {
        if (display.id == id) {
            return &display;
        }
    }
    return nullptr;
}

bool sameAxis(EdgeDirection a, EdgeDirection b) {
    const bool aHorizontal = a == EdgeDirection::Left || a == EdgeDirection::Right;
    const bool bHorizontal = b == EdgeDirection::Left || b == EdgeDirection::Right;
    return aHorizontal == bHorizontal;
}

bool targetFacesSource(const Display& source, EdgeDirection exitEdge, const Display& target) {
    switch (exitEdge) {
    case EdgeDirection::Left:
        return rightOf(target) <= source.x &&
               rangesOverlap(source.y, bottomOf(source), target.y, bottomOf(target));
    case EdgeDirection::Right:
        return rightOf(source) <= target.x &&
               rangesOverlap(source.y, bottomOf(source), target.y, bottomOf(target));
    case EdgeDirection::Up:
        return bottomOf(target) <= source.y &&
               rangesOverlap(source.x, rightOf(source), target.x, rightOf(target));
    case EdgeDirection::Down:
        return bottomOf(source) <= target.y &&
               rangesOverlap(source.x, rightOf(source), target.x, rightOf(target));
    }
    return false;
}

bool wrapAllows(WrapPolicy policy, EdgeDirection direction) {
    const bool horizontal = direction == EdgeDirection::Left || direction == EdgeDirection::Right;
    switch (policy) {
    case WrapPolicy::None:
        return false;
    case WrapPolicy::Horizontal:
        return horizontal;
    case WrapPolicy::Vertical:
        return !horizontal;
    case WrapPolicy::Both:
        return true;
    }
    return false;
}

} // namespace

void TopologyModel::addMachine(Machine machine) {
    machines_.push_back(std::move(machine));
}

void TopologyModel::addDisplay(Display display) {
    displays_.push_back(std::move(display));
}

void TopologyModel::addBorderLink(BorderLink link) {
    borderLinks_.push_back(std::move(link));
}

void TopologyModel::setWrapPolicy(WrapPolicy policy) {
    wrapPolicy_ = policy;
}

const std::vector<Machine>& TopologyModel::machines() const {
    return machines_;
}

const std::vector<Display>& TopologyModel::displays() const {
    return displays_;
}

const std::vector<BorderLink>& TopologyModel::borderLinks() const {
    return borderLinks_;
}

WrapPolicy TopologyModel::wrapPolicy() const {
    return wrapPolicy_;
}

const Display* TopologyModel::displayById(const std::string& displayId) const {
    return findDisplay(displays_, displayId);
}

std::optional<std::string> TopologyModel::machineIdForDisplay(const std::string& displayId) const {
    const Display* display = findDisplay(displays_, displayId);
    if (display == nullptr) {
        return std::nullopt;
    }
    return display->machineId;
}

std::vector<TopologyIssue> TopologyModel::validate() const {
    std::vector<TopologyIssue> issues;
    std::set<std::string> machineIds;
    std::set<std::string> displayIds;

    for (const auto& machine : machines_) {
        if (!machineIds.insert(machine.id).second) {
            addIssue(issues, TopologyIssueCode::DuplicateMachine,
                     "duplicate machine id: " + machine.id);
        }
    }

    for (const auto& display : displays_) {
        if (!displayIds.insert(display.id).second) {
            addIssue(issues, TopologyIssueCode::DuplicateDisplay,
                     "duplicate display id: " + display.id);
        }
        if (machineIds.find(display.machineId) == machineIds.end()) {
            addIssue(issues, TopologyIssueCode::MissingMachine,
                     "display " + display.id + " references missing machine " + display.machineId);
        }
        if (display.width <= 0 || display.height <= 0) {
            addIssue(issues, TopologyIssueCode::InvalidDisplayBounds,
                     "display " + display.id + " has non-positive dimensions");
        }
    }

    for (std::size_t i = 0; i < displays_.size(); ++i) {
        for (std::size_t j = i + 1; j < displays_.size(); ++j) {
            if (displays_[i].machineId == displays_[j].machineId &&
                displaysOverlap(displays_[i], displays_[j])) {
                addIssue(issues, TopologyIssueCode::OverlappingDisplays,
                         "displays " + displays_[i].id + " and " + displays_[j].id +
                             " overlap on machine " + displays_[i].machineId);
            }
        }
    }

    std::map<EdgeKey, BorderLink> linksBySourceEdge;
    for (const auto& link : borderLinks_) {
        const Display* source = findDisplay(displays_, link.sourceDisplayId);
        const Display* target = findDisplay(displays_, link.targetDisplayId);
        if (source == nullptr) {
            addIssue(issues, TopologyIssueCode::MissingSourceDisplay,
                     "link source display is missing: " + link.sourceDisplayId);
        }
        if (target == nullptr) {
            addIssue(issues, TopologyIssueCode::MissingTargetDisplay,
                     "link target display is missing: " + link.targetDisplayId);
        }

        const EdgeKey key{link.sourceDisplayId, link.exitEdge};
        const auto existing = linksBySourceEdge.find(key);
        if (existing != linksBySourceEdge.end()) {
            if (existing->second.targetDisplayId == link.targetDisplayId &&
                existing->second.entryEdge == link.entryEdge) {
                addIssue(issues, TopologyIssueCode::DuplicateEdgeLink,
                         "duplicate link for " + describeEdge(link.sourceDisplayId, link.exitEdge));
            } else {
                addIssue(issues, TopologyIssueCode::ContradictoryDuplicateEdge,
                         "contradictory links for " +
                             describeEdge(link.sourceDisplayId, link.exitEdge));
                addIssue(issues, TopologyIssueCode::AmbiguousEdgeMapping,
                         "multiple targets for " + describeEdge(link.sourceDisplayId, link.exitEdge));
            }
        } else {
            linksBySourceEdge.emplace(key, link);
        }

        if (source == nullptr || target == nullptr) {
            continue;
        }
        if (source->id == target->id) {
            addIssue(issues, TopologyIssueCode::ImpossibleEdgeMapping,
                     "link maps display " + source->id + " to itself");
        }
        if (link.entryEdge != oppositeEdge(link.exitEdge) || !sameAxis(link.exitEdge, link.entryEdge)) {
            addIssue(issues, TopologyIssueCode::ImpossibleEdgeMapping,
                     "link " + describeEdge(link.sourceDisplayId, link.exitEdge) +
                         " enters through incompatible edge " + edgeDirectionName(link.entryEdge));
        }
        if (source->machineId == target->machineId &&
            !targetFacesSource(*source, link.exitEdge, *target)) {
            addIssue(issues, TopologyIssueCode::ImpossibleEdgeMapping,
                     "same-machine link " + describeEdge(link.sourceDisplayId, link.exitEdge) +
                         " does not face target display " + target->id);
        }
    }

    return issues;
}

std::optional<TransitionResult> TopologyModel::transitionFromEdge(
    const std::string& displayId,
    EdgeDirection direction,
    int coordinate) const {
    const Display* source = findDisplay(displays_, displayId);
    if (source == nullptr) {
        return std::nullopt;
    }

    const int sourceLength = edgeLength(*source, direction);
    if (coordinate < 0 || coordinate >= sourceLength) {
        return std::nullopt;
    }

    const BorderLink* match = nullptr;
    for (const auto& link : borderLinks_) {
        if (link.sourceDisplayId == displayId && link.exitEdge == direction) {
            if (match != nullptr) {
                return std::nullopt;
            }
            match = &link;
        }
    }

    if (match != nullptr) {
        const Display* target = findDisplay(displays_, match->targetDisplayId);
        if (target == nullptr || match->entryEdge != oppositeEdge(direction)) {
            return std::nullopt;
        }
        return TransitionResult{
            target->id,
            match->entryEdge,
            mapCoordinate(coordinate, sourceLength, edgeLength(*target, match->entryEdge)),
        };
    }

    if (!wrapAllows(wrapPolicy_, direction)) {
        return std::nullopt;
    }

    const Display* target = nullptr;
    int targetCoordinate = -1;
    int bestEdge = 0;

    for (const auto& candidate : displays_) {
        if (candidate.machineId != source->machineId) {
            continue;
        }

        int candidateCoordinate = -1;
        int candidateEdge = 0;
        switch (direction) {
        case EdgeDirection::Left: {
            const int globalY = source->y + coordinate;
            if (globalY < candidate.y || globalY >= bottomOf(candidate)) {
                continue;
            }
            candidateCoordinate = globalY - candidate.y;
            candidateEdge = rightOf(candidate);
            if (target != nullptr && candidateEdge < bestEdge) {
                continue;
            }
            break;
        }
        case EdgeDirection::Right: {
            const int globalY = source->y + coordinate;
            if (globalY < candidate.y || globalY >= bottomOf(candidate)) {
                continue;
            }
            candidateCoordinate = globalY - candidate.y;
            candidateEdge = candidate.x;
            if (target != nullptr && candidateEdge > bestEdge) {
                continue;
            }
            break;
        }
        case EdgeDirection::Up: {
            const int globalX = source->x + coordinate;
            if (globalX < candidate.x || globalX >= rightOf(candidate)) {
                continue;
            }
            candidateCoordinate = globalX - candidate.x;
            candidateEdge = bottomOf(candidate);
            if (target != nullptr && candidateEdge < bestEdge) {
                continue;
            }
            break;
        }
        case EdgeDirection::Down: {
            const int globalX = source->x + coordinate;
            if (globalX < candidate.x || globalX >= rightOf(candidate)) {
                continue;
            }
            candidateCoordinate = globalX - candidate.x;
            candidateEdge = candidate.y;
            if (target != nullptr && candidateEdge > bestEdge) {
                continue;
            }
            break;
        }
        }

        if (target != nullptr && candidateEdge == bestEdge) {
            return std::nullopt;
        }
        target = &candidate;
        targetCoordinate = candidateCoordinate;
        bestEdge = candidateEdge;
    }

    if (target == nullptr || targetCoordinate < 0) {
        return std::nullopt;
    }

    return TransitionResult{target->id, oppositeEdge(direction), targetCoordinate};
}

EdgeDirection oppositeEdge(EdgeDirection direction) {
    switch (direction) {
    case EdgeDirection::Left:
        return EdgeDirection::Right;
    case EdgeDirection::Right:
        return EdgeDirection::Left;
    case EdgeDirection::Up:
        return EdgeDirection::Down;
    case EdgeDirection::Down:
        return EdgeDirection::Up;
    }
    return EdgeDirection::Left;
}

const char* edgeDirectionName(EdgeDirection direction) {
    switch (direction) {
    case EdgeDirection::Left:
        return "left";
    case EdgeDirection::Right:
        return "right";
    case EdgeDirection::Up:
        return "up";
    case EdgeDirection::Down:
        return "down";
    }
    return "unknown";
}

const char* topologyIssueCodeName(TopologyIssueCode code) {
    switch (code) {
    case TopologyIssueCode::DuplicateMachine:
        return "duplicate-machine";
    case TopologyIssueCode::DuplicateDisplay:
        return "duplicate-display";
    case TopologyIssueCode::MissingMachine:
        return "missing-machine";
    case TopologyIssueCode::InvalidDisplayBounds:
        return "invalid-display-bounds";
    case TopologyIssueCode::OverlappingDisplays:
        return "overlapping-displays";
    case TopologyIssueCode::MissingSourceDisplay:
        return "missing-source-display";
    case TopologyIssueCode::MissingTargetDisplay:
        return "missing-target-display";
    case TopologyIssueCode::DuplicateEdgeLink:
        return "duplicate-edge-link";
    case TopologyIssueCode::ContradictoryDuplicateEdge:
        return "contradictory-duplicate-edge";
    case TopologyIssueCode::ImpossibleEdgeMapping:
        return "impossible-edge-mapping";
    case TopologyIssueCode::AmbiguousEdgeMapping:
        return "ambiguous-edge-mapping";
    }
    return "unknown";
}

std::optional<TopologyPointerTransition> ResolveTopologyPointerTransition(
    const TopologyModel& model,
    const std::string& sourceDisplayId,
    int normalizedX,
    int normalizedY) {
    const Display* source = findDisplay(model.displays(), sourceDisplayId);
    if (source == nullptr ||
        !isAbsolutePointerCoordinate(normalizedX) ||
        !isAbsolutePointerCoordinate(normalizedY)) {
        return std::nullopt;
    }

    std::optional<EdgeDirection> exitEdge;
    int coordinate = 0;
    if (normalizedX <= 0) {
        exitEdge = EdgeDirection::Left;
        coordinate = mapNormalizedCoordinate(normalizedY, source->height);
    } else if (normalizedX >= 65535) {
        exitEdge = EdgeDirection::Right;
        coordinate = mapNormalizedCoordinate(normalizedY, source->height);
    } else if (normalizedY <= 0) {
        exitEdge = EdgeDirection::Up;
        coordinate = mapNormalizedCoordinate(normalizedX, source->width);
    } else if (normalizedY >= 65535) {
        exitEdge = EdgeDirection::Down;
        coordinate = mapNormalizedCoordinate(normalizedX, source->width);
    }

    if (!exitEdge.has_value()) {
        return std::nullopt;
    }

    const auto transition = model.transitionFromEdge(sourceDisplayId, *exitEdge, coordinate);
    if (!transition.has_value()) {
        return std::nullopt;
    }

    return TopologyPointerTransition{
        sourceDisplayId,
        *exitEdge,
        transition->targetDisplayId,
        transition->entryEdge,
        transition->coordinate,
    };
}

std::optional<TopologyPointerTransition> ResolveTopologyPointerTransitionForMachine(
    const TopologyModel& model,
    const std::string& machineId,
    int desktopWidth,
    int desktopHeight,
    int normalizedX,
    int normalizedY) {
    if (machineId.empty() ||
        desktopWidth <= 0 ||
        desktopHeight <= 0 ||
        !isAbsolutePointerCoordinate(normalizedX) ||
        !isAbsolutePointerCoordinate(normalizedY)) {
        return std::nullopt;
    }

    std::optional<EdgeDirection> exitEdge;
    if (normalizedX <= 0) {
        exitEdge = EdgeDirection::Left;
    } else if (normalizedX >= 65535) {
        exitEdge = EdgeDirection::Right;
    } else if (normalizedY <= 0) {
        exitEdge = EdgeDirection::Up;
    } else if (normalizedY >= 65535) {
        exitEdge = EdgeDirection::Down;
    } else {
        return std::nullopt;
    }

    const int globalX = mapNormalizedCoordinate(normalizedX, desktopWidth);
    const int globalY = mapNormalizedCoordinate(normalizedY, desktopHeight);

    const Display* source = nullptr;
    int edgeCoordinate = -1;
    for (const auto& display : model.displays()) {
        if (display.machineId != machineId) {
            continue;
        }

        bool candidate = false;
        int coordinate = -1;
        switch (*exitEdge) {
        case EdgeDirection::Left:
            candidate = display.x == globalX && globalY >= display.y && globalY < bottomOf(display);
            coordinate = globalY - display.y;
            break;
        case EdgeDirection::Right:
            candidate = rightOf(display) - 1 == globalX && globalY >= display.y && globalY < bottomOf(display);
            coordinate = globalY - display.y;
            break;
        case EdgeDirection::Up:
            candidate = display.y == globalY && globalX >= display.x && globalX < rightOf(display);
            coordinate = globalX - display.x;
            break;
        case EdgeDirection::Down:
            candidate = bottomOf(display) - 1 == globalY && globalX >= display.x && globalX < rightOf(display);
            coordinate = globalX - display.x;
            break;
        }

        if (!candidate) {
            continue;
        }
        if (source != nullptr) {
            return std::nullopt;
        }
        source = &display;
        edgeCoordinate = coordinate;
    }

    if (source == nullptr || edgeCoordinate < 0) {
        return std::nullopt;
    }

    const auto transition = model.transitionFromEdge(source->id, *exitEdge, edgeCoordinate);
    if (!transition.has_value()) {
        return std::nullopt;
    }

    return TopologyPointerTransition{
        source->id,
        *exitEdge,
        transition->targetDisplayId,
        transition->entryEdge,
        transition->coordinate,
    };
}

std::optional<TopologyNormalizedPoint> MapTransitionToTargetNormalizedPoint(
    const TopologyModel& model,
    const TopologyPointerTransition& transition) {
    const Display* target = model.displayById(transition.targetDisplayId);
    if (target == nullptr) {
        return std::nullopt;
    }

    switch (transition.entryEdge) {
    case EdgeDirection::Left:
        return TopologyNormalizedPoint{0, normalizeCoordinate(transition.coordinate, target->height)};
    case EdgeDirection::Right:
        return TopologyNormalizedPoint{65535, normalizeCoordinate(transition.coordinate, target->height)};
    case EdgeDirection::Up:
        return TopologyNormalizedPoint{normalizeCoordinate(transition.coordinate, target->width), 0};
    case EdgeDirection::Down:
        return TopologyNormalizedPoint{normalizeCoordinate(transition.coordinate, target->width), 65535};
    }
    return std::nullopt;
}

bool ParseTopologyConfig(std::string_view text, TopologyModel& outModel, std::string* errorMessage) {
    TopologyModel parsed;
    std::istringstream stream{std::string(text)};
    std::string line;
    size_t lineNumber = 0;

    while (std::getline(stream, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::string_view trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }

        const size_t separator = trimmed.find('=');
        if (separator == std::string_view::npos) {
            setError(errorMessage, "Topology line " + std::to_string(lineNumber) + " is missing '='.");
            return false;
        }

        const std::string key(toLower(trim(trimmed.substr(0, separator))));
        const std::string_view value = trim(trimmed.substr(separator + 1));
        if (key.empty()) {
            setError(errorMessage, "Topology line " + std::to_string(lineNumber) + " has an empty key.");
            return false;
        }

        if (key == "machine") {
            if (value.empty()) {
                setError(errorMessage, "Topology line " + std::to_string(lineNumber) + " has an empty machine id.");
                return false;
            }
            parsed.addMachine({std::string(value)});
            continue;
        }

        if (key == "display") {
            const auto parts = splitCommaList(value);
            if (parts.size() != 6) {
                setError(errorMessage, "Topology line " + std::to_string(lineNumber) + " display expects ID,MACHINE,X,Y,W,H.");
                return false;
            }
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            if (parts[0].empty() || parts[1].empty() ||
                !parseInt(parts[2], x) || !parseInt(parts[3], y) ||
                !parseInt(parts[4], width) || !parseInt(parts[5], height) ||
                width <= 0 || height <= 0) {
                setError(errorMessage, "Topology line " + std::to_string(lineNumber) + " has an invalid display.");
                return false;
            }
            parsed.addDisplay({parts[0], parts[1], x, y, width, height});
            continue;
        }

        if (key == "link") {
            const auto parts = splitCommaList(value);
            if (parts.size() != 4) {
                setError(errorMessage, "Topology line " + std::to_string(lineNumber) + " link expects SRC,EDGE,TGT,ENTRY.");
                return false;
            }
            const auto exitEdge = parseEdgeDirection(parts[1]);
            const auto entryEdge = parseEdgeDirection(parts[3]);
            if (parts[0].empty() || parts[2].empty() || !exitEdge.has_value() || !entryEdge.has_value()) {
                setError(errorMessage, "Topology line " + std::to_string(lineNumber) + " has an invalid link.");
                return false;
            }
            parsed.addBorderLink({parts[0], *exitEdge, parts[2], *entryEdge});
            continue;
        }

        if (key == "wrap") {
            const auto policy = parseWrapPolicy(value);
            if (!policy.has_value()) {
                setError(errorMessage, "Topology line " + std::to_string(lineNumber) + " wrap expects none, horizontal, vertical, or both.");
                return false;
            }
            parsed.setWrapPolicy(*policy);
            continue;
        }

        setError(errorMessage, "Unknown topology key '" + key + "' on line " + std::to_string(lineNumber) + ".");
        return false;
    }

    outModel = std::move(parsed);
    return true;
}

bool LoadTopologyConfig(const std::filesystem::path& path, TopologyModel& outModel, std::string* errorMessage) {
    std::ifstream file(path);
    if (!file) {
        setError(errorMessage, "Failed to open topology file: " + path.string());
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (!file.good() && !file.eof()) {
        setError(errorMessage, "Failed to read topology file: " + path.string());
        return false;
    }

    return ParseTopologyConfig(buffer.str(), outModel, errorMessage);
}

} // namespace mwb
