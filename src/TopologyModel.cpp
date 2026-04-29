#include "TopologyModel.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

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

} // namespace mwb
