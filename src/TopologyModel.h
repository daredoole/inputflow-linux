#pragma once

#include <optional>
#include <string>
#include <vector>

namespace mwb {

enum class EdgeDirection {
    Left,
    Right,
    Up,
    Down,
};

enum class WrapPolicy {
    None,
    Horizontal,
    Vertical,
    Both,
};

struct Machine {
    std::string id;
};

struct Display {
    std::string id;
    std::string machineId;
    int x{0};
    int y{0};
    int width{0};
    int height{0};
};

struct BorderLink {
    std::string sourceDisplayId;
    EdgeDirection exitEdge{EdgeDirection::Right};
    std::string targetDisplayId;
    EdgeDirection entryEdge{EdgeDirection::Left};
};

struct TransitionResult {
    std::string targetDisplayId;
    EdgeDirection entryEdge{EdgeDirection::Left};
    int coordinate{0};
};

enum class TopologyIssueCode {
    DuplicateMachine,
    DuplicateDisplay,
    MissingMachine,
    InvalidDisplayBounds,
    OverlappingDisplays,
    MissingSourceDisplay,
    MissingTargetDisplay,
    DuplicateEdgeLink,
    ContradictoryDuplicateEdge,
    ImpossibleEdgeMapping,
    AmbiguousEdgeMapping,
};

struct TopologyIssue {
    TopologyIssueCode code{TopologyIssueCode::ImpossibleEdgeMapping};
    std::string message;
};

class TopologyModel {
public:
    void addMachine(Machine machine);
    void addDisplay(Display display);
    void addBorderLink(BorderLink link);
    void setWrapPolicy(WrapPolicy policy);

    const std::vector<Machine>& machines() const;
    const std::vector<Display>& displays() const;
    const std::vector<BorderLink>& borderLinks() const;
    WrapPolicy wrapPolicy() const;

    std::vector<TopologyIssue> validate() const;

    std::optional<TransitionResult> transitionFromEdge(
        const std::string& displayId,
        EdgeDirection direction,
        int coordinate) const;

private:
    std::vector<Machine> machines_;
    std::vector<Display> displays_;
    std::vector<BorderLink> borderLinks_;
    WrapPolicy wrapPolicy_{WrapPolicy::None};
};

EdgeDirection oppositeEdge(EdgeDirection direction);
const char* edgeDirectionName(EdgeDirection direction);
const char* topologyIssueCodeName(TopologyIssueCode code);

} // namespace mwb
