#include <iostream>
#include <string>
#include <vector>

#include "TopologyModel.h"

namespace {

int g_failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

void ExpectEqual(const std::string& actual, const std::string& expected, const std::string& message) {
    if (actual != expected) {
        ++g_failures;
        std::cerr << "FAIL: " << message
                  << " expected=" << expected
                  << " actual=" << actual << std::endl;
    }
}

void ExpectEqual(int actual, int expected, const std::string& message) {
    if (actual != expected) {
        ++g_failures;
        std::cerr << "FAIL: " << message
                  << " expected=" << expected
                  << " actual=" << actual << std::endl;
    }
}

bool HasIssue(const std::vector<mwb::TopologyIssue>& issues, mwb::TopologyIssueCode code) {
    for (const auto& issue : issues) {
        if (issue.code == code) {
            return true;
        }
    }
    return false;
}

mwb::TopologyModel BaseModel() {
    mwb::TopologyModel model;
    model.addMachine({"A"});
    model.addMachine({"B"});
    return model;
}

void TestAABFixtureRoutesFromSecondADisplayToB() {
    auto model = BaseModel();
    model.addDisplay({"A1", "A", 0, 0, 1920, 1080});
    model.addDisplay({"A2", "A", 1920, 0, 1920, 1080});
    model.addDisplay({"B1", "B", 0, 0, 1920, 1080});
    model.addBorderLink({"A2", mwb::EdgeDirection::Right, "B1", mwb::EdgeDirection::Left});
    model.addBorderLink({"B1", mwb::EdgeDirection::Left, "A2", mwb::EdgeDirection::Right});

    Expect(model.validate().empty(), "AAB fixture should validate");
    const auto transition = model.transitionFromEdge("A2", mwb::EdgeDirection::Right, 500);
    Expect(transition.has_value(), "AAB right edge should transition");
    if (transition.has_value()) {
        ExpectEqual(transition->targetDisplayId, "B1", "AAB target display");
        Expect(transition->entryEdge == mwb::EdgeDirection::Left, "AAB should enter B1 left edge");
        ExpectEqual(transition->coordinate, 500, "AAB coordinate should be preserved");
    }
}

void TestBAAFixtureRoutesFromBToFirstADisplay() {
    auto model = BaseModel();
    model.addDisplay({"B1", "B", 0, 0, 1920, 1080});
    model.addDisplay({"A1", "A", 0, 0, 1920, 1080});
    model.addDisplay({"A2", "A", 1920, 0, 1920, 1080});
    model.addBorderLink({"B1", mwb::EdgeDirection::Right, "A1", mwb::EdgeDirection::Left});
    model.addBorderLink({"A1", mwb::EdgeDirection::Left, "B1", mwb::EdgeDirection::Right});

    Expect(model.validate().empty(), "BAA fixture should validate");
    const auto transition = model.transitionFromEdge("B1", mwb::EdgeDirection::Right, 1079);
    Expect(transition.has_value(), "BAA right edge should transition");
    if (transition.has_value()) {
        ExpectEqual(transition->targetDisplayId, "A1", "BAA target display");
        ExpectEqual(transition->coordinate, 1079, "BAA bottom coordinate should be preserved");
    }
}

void TestABAFixtureRoutesThroughMiddleMachine() {
    auto model = BaseModel();
    model.addDisplay({"A1", "A", 0, 0, 1280, 1024});
    model.addDisplay({"B1", "B", 0, 0, 1280, 1024});
    model.addDisplay({"A2", "A", 1280, 0, 1280, 1024});
    model.addBorderLink({"A1", mwb::EdgeDirection::Right, "B1", mwb::EdgeDirection::Left});
    model.addBorderLink({"B1", mwb::EdgeDirection::Right, "A2", mwb::EdgeDirection::Left});

    Expect(model.validate().empty(), "ABA fixture should validate");
    const auto first = model.transitionFromEdge("A1", mwb::EdgeDirection::Right, 100);
    const auto second = model.transitionFromEdge("B1", mwb::EdgeDirection::Right, 100);
    Expect(first.has_value(), "ABA A1 to B1 transition should exist");
    Expect(second.has_value(), "ABA B1 to A2 transition should exist");
    if (first.has_value()) {
        ExpectEqual(first->targetDisplayId, "B1", "ABA first target");
    }
    if (second.has_value()) {
        ExpectEqual(second->targetDisplayId, "A2", "ABA second target");
    }
}

void TestStackedFixtureRoutesVertically() {
    mwb::TopologyModel model;
    model.addMachine({"A"});
    model.addDisplay({"A-top", "A", 0, 0, 1600, 900});
    model.addDisplay({"A-bottom", "A", 0, 900, 1600, 900});
    model.addBorderLink({"A-top", mwb::EdgeDirection::Down, "A-bottom", mwb::EdgeDirection::Up});
    model.addBorderLink({"A-bottom", mwb::EdgeDirection::Up, "A-top", mwb::EdgeDirection::Down});

    Expect(model.validate().empty(), "stacked fixture should validate");
    const auto transition = model.transitionFromEdge("A-top", mwb::EdgeDirection::Down, 1200);
    Expect(transition.has_value(), "stacked bottom edge should transition");
    if (transition.has_value()) {
        ExpectEqual(transition->targetDisplayId, "A-bottom", "stacked target display");
        ExpectEqual(transition->coordinate, 1200, "stacked x coordinate should be preserved");
    }
}

void TestAsymmetricResolutionsScaleEdgeCoordinate() {
    auto model = BaseModel();
    model.addDisplay({"A1", "A", 0, 0, 1920, 1080});
    model.addDisplay({"B1", "B", 0, 0, 2560, 1440});
    model.addBorderLink({"A1", mwb::EdgeDirection::Right, "B1", mwb::EdgeDirection::Left});

    Expect(model.validate().empty(), "asymmetric fixture should validate");
    const auto transition = model.transitionFromEdge("A1", mwb::EdgeDirection::Right, 540);
    Expect(transition.has_value(), "asymmetric right edge should transition");
    if (transition.has_value()) {
        ExpectEqual(transition->targetDisplayId, "B1", "asymmetric target display");
        ExpectEqual(transition->coordinate, 720, "1080-high edge midpoint should scale to 1440-high edge");
    }
}

void TestWrapPolicyOnOff() {
    mwb::TopologyModel off;
    off.addMachine({"A"});
    off.addDisplay({"A1", "A", 0, 0, 1920, 1080});
    Expect(!off.transitionFromEdge("A1", mwb::EdgeDirection::Right, 42).has_value(),
           "wrap disabled should not transition");

    mwb::TopologyModel horizontal;
    horizontal.addMachine({"A"});
    horizontal.addDisplay({"A1", "A", 0, 0, 1920, 1080});
    horizontal.setWrapPolicy(mwb::WrapPolicy::Horizontal);
    const auto wrapped = horizontal.transitionFromEdge("A1", mwb::EdgeDirection::Right, 42);
    Expect(wrapped.has_value(), "horizontal wrap should transition");
    if (wrapped.has_value()) {
        ExpectEqual(wrapped->targetDisplayId, "A1", "single display horizontal wrap target");
        Expect(wrapped->entryEdge == mwb::EdgeDirection::Left,
               "right-edge wrap should enter left edge");
        ExpectEqual(wrapped->coordinate, 42, "horizontal wrap should preserve y coordinate");
    }
    Expect(!horizontal.transitionFromEdge("A1", mwb::EdgeDirection::Down, 42).has_value(),
           "horizontal wrap should not wrap vertically");
}

void TestValidationRejectsOverlappingDisplaysOnSameMachine() {
    mwb::TopologyModel model;
    model.addMachine({"A"});
    model.addDisplay({"A1", "A", 0, 0, 100, 100});
    model.addDisplay({"A2", "A", 50, 50, 100, 100});

    Expect(HasIssue(model.validate(), mwb::TopologyIssueCode::OverlappingDisplays),
           "overlapping same-machine displays should be reported");
}

void TestValidationRejectsMissingDisplaysForLinks() {
    mwb::TopologyModel model;
    model.addMachine({"A"});
    model.addDisplay({"A1", "A", 0, 0, 100, 100});
    model.addBorderLink({"A1", mwb::EdgeDirection::Right, "missing", mwb::EdgeDirection::Left});
    model.addBorderLink({"missing-source", mwb::EdgeDirection::Right, "A1", mwb::EdgeDirection::Left});
    const auto issues = model.validate();

    Expect(HasIssue(issues, mwb::TopologyIssueCode::MissingTargetDisplay),
           "missing link target should be reported");
    Expect(HasIssue(issues, mwb::TopologyIssueCode::MissingSourceDisplay),
           "missing link source should be reported");
}

void TestValidationRejectsContradictoryDuplicateEdgeLinks() {
    auto model = BaseModel();
    model.addDisplay({"A1", "A", 0, 0, 100, 100});
    model.addDisplay({"B1", "B", 0, 0, 100, 100});
    model.addDisplay({"B2", "B", 100, 0, 100, 100});
    model.addBorderLink({"A1", mwb::EdgeDirection::Right, "B1", mwb::EdgeDirection::Left});
    model.addBorderLink({"A1", mwb::EdgeDirection::Right, "B2", mwb::EdgeDirection::Left});
    const auto issues = model.validate();

    Expect(HasIssue(issues, mwb::TopologyIssueCode::ContradictoryDuplicateEdge),
           "contradictory duplicate edge link should be reported");
    Expect(HasIssue(issues, mwb::TopologyIssueCode::AmbiguousEdgeMapping),
           "multiple targets from one edge should be ambiguous");
}

void TestValidationRejectsImpossibleEdgeMappings() {
    mwb::TopologyModel model;
    model.addMachine({"A"});
    model.addDisplay({"A1", "A", 0, 0, 100, 100});
    model.addDisplay({"A2", "A", 200, 200, 100, 100});
    model.addBorderLink({"A1", mwb::EdgeDirection::Right, "A2", mwb::EdgeDirection::Right});
    model.addBorderLink({"A2", mwb::EdgeDirection::Left, "A2", mwb::EdgeDirection::Right});

    Expect(HasIssue(model.validate(), mwb::TopologyIssueCode::ImpossibleEdgeMapping),
           "incompatible, diagonal, or self edge mappings should be reported");
}

void TestParseTopologyConfigAcceptsLineBasedFormat() {
    mwb::TopologyModel model;
    std::string error;
    const std::string text =
        "# simple two-machine layout\n"
        "machine=A\n"
        "machine=B\n"
        "display=A1,A,0,0,1920,1080\n"
        "display=B1,B,1920,0,2560,1440\n"
        "link=A1,right,B1,left\n"
        "wrap=none\n";

    Expect(mwb::ParseTopologyConfig(text, model, &error), "ParseTopologyConfig should accept valid text");
    Expect(error.empty(), "Valid topology parse should not set an error");
    Expect(model.machines().size() == 2, "Parsed topology should keep machines");
    Expect(model.displays().size() == 2, "Parsed topology should keep displays");
    Expect(model.borderLinks().size() == 1, "Parsed topology should keep links");
    Expect(model.validate().empty(), "Parsed topology should validate");

    const auto transition = model.transitionFromEdge("A1", mwb::EdgeDirection::Right, 540);
    Expect(transition.has_value(), "Parsed topology should route configured link");
    if (transition.has_value()) {
        ExpectEqual(transition->targetDisplayId, "B1", "Parsed topology target display");
        Expect(transition->entryEdge == mwb::EdgeDirection::Left, "Parsed topology entry edge");
    }
}

void TestParseTopologyConfigRejectsInvalidLines() {
    mwb::TopologyModel model;
    std::string error;
    const std::string text =
        "machine=A\n"
        "display=A1,A,0,0,not-a-width,1080\n";

    Expect(!mwb::ParseTopologyConfig(text, model, &error), "ParseTopologyConfig should reject invalid display values");
    Expect(error.find("line 2") != std::string::npos, "Invalid topology parse should report line number");
}

void TestPointerTransitionResolverUsesAbsoluteEdges() {
    mwb::TopologyModel model = BaseModel();
    model.addDisplay({"A1", "A", 0, 0, 1920, 1080});
    model.addDisplay({"B1", "B", 1920, 0, 2560, 1440});
    model.addBorderLink({"A1", mwb::EdgeDirection::Right, "B1", mwb::EdgeDirection::Left});

    const auto transition = mwb::ResolveTopologyPointerTransition(model, "A1", 65535, 32767);
    Expect(transition.has_value(), "Absolute right edge should resolve topology transition");
    if (transition.has_value()) {
        ExpectEqual(transition->sourceDisplayId, "A1", "Pointer transition source display");
        Expect(transition->exitEdge == mwb::EdgeDirection::Right, "Pointer transition exit edge");
        ExpectEqual(transition->targetDisplayId, "B1", "Pointer transition target display");
        Expect(transition->entryEdge == mwb::EdgeDirection::Left, "Pointer transition entry edge");
    }

    Expect(!mwb::ResolveTopologyPointerTransition(model, "A1", 32000, 32767).has_value(),
           "Non-edge absolute pointer move should not resolve transition");
}

} // namespace

int main() {
    TestAABFixtureRoutesFromSecondADisplayToB();
    TestBAAFixtureRoutesFromBToFirstADisplay();
    TestABAFixtureRoutesThroughMiddleMachine();
    TestStackedFixtureRoutesVertically();
    TestAsymmetricResolutionsScaleEdgeCoordinate();
    TestWrapPolicyOnOff();
    TestValidationRejectsOverlappingDisplaysOnSameMachine();
    TestValidationRejectsMissingDisplaysForLinks();
    TestValidationRejectsContradictoryDuplicateEdgeLinks();
    TestValidationRejectsImpossibleEdgeMappings();
    TestParseTopologyConfigAcceptsLineBasedFormat();
    TestParseTopologyConfigRejectsInvalidLines();
    TestPointerTransitionResolverUsesAbsoluteEdges();

    if (g_failures == 0) {
        std::cout << "Topology model tests passed." << std::endl;
        return 0;
    }

    std::cerr << g_failures << " topology model test(s) failed." << std::endl;
    return 1;
}
