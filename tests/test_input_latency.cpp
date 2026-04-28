#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <unistd.h>

#include "InputLatencyStats.h"

namespace {

int g_failures = 0;
constexpr std::size_t kResourceWorkloadIterations = 200000;

struct MemorySnapshot {
    long currentRssKb{0};
    long virtualMemoryKb{0};
    long dataMemoryKb{0};
    long stackMemoryKb{0};
    long rssAnonKb{0};
    long rssFileKb{0};
    long rssShmemKb{0};
};

struct ResourceSample {
    std::chrono::steady_clock::time_point wallTime;
    rusage usage{};
    MemorySnapshot memory;
};

struct ResourceDelta {
    uint64_t wallUs{0};
    uint64_t userCpuUs{0};
    uint64_t systemCpuUs{0};
    uint64_t totalCpuUs{0};
    double processCpuPercent{0.0};
    double hostCpuPercent{0.0};
    long onlineCpuCount{0};
    long peakRssKb{0};
    long currentRssKb{0};
    long virtualMemoryKb{0};
    long dataMemoryKb{0};
    long stackMemoryKb{0};
    long rssAnonKb{0};
    long rssFileKb{0};
    long rssShmemKb{0};
    long minorFaults{0};
    long majorFaults{0};
    long voluntaryContextSwitches{0};
    long involuntaryContextSwitches{0};
};

struct MouseLatencyReference {
    const char* name;
    const char* basis;
    double averageMs;
    double maxMs;
};

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << std::endl;
    }
}

void PopulateDeterministicLatencySamples(mwb::InputLatencyStats& stats) {
    stats.RecordEnqueue(mwb::InputLatencyStats::InputKind::Mouse, 3);
    stats.RecordEnqueue(mwb::InputLatencyStats::InputKind::Mouse, 2);
    stats.RecordEnqueue(mwb::InputLatencyStats::InputKind::Keyboard, 1);
    stats.RecordDrop(mwb::InputLatencyStats::InputKind::Mouse, true);
    stats.RecordDrop(mwb::InputLatencyStats::InputKind::Keyboard, false);
    stats.RecordMerge(4);

    stats.RecordQueueWait(mwb::InputLatencyStats::InputKind::Mouse, std::chrono::microseconds(100));
    stats.RecordQueueWait(mwb::InputLatencyStats::InputKind::Mouse, std::chrono::microseconds(300));
    stats.RecordQueueWait(mwb::InputLatencyStats::InputKind::Mouse, std::chrono::microseconds(500));
    stats.RecordInjectDuration(mwb::InputLatencyStats::InputKind::Mouse, std::chrono::microseconds(200));
    stats.RecordInjectDuration(mwb::InputLatencyStats::InputKind::Mouse, std::chrono::microseconds(400));
    stats.RecordQueueWait(mwb::InputLatencyStats::InputKind::Keyboard, std::chrono::microseconds(700));
    stats.RecordInjectDuration(mwb::InputLatencyStats::InputKind::Keyboard, std::chrono::microseconds(900));
}

void TestLatencySnapshotTracksCountsAndPercentiles() {
    mwb::InputLatencyStats stats;
    PopulateDeterministicLatencySamples(stats);

    const auto summary = stats.Snapshot();
    Expect(summary.enabled, "Latency collector should report enabled when constructed normally");
    Expect(summary.enqueuedMouse == 2, "Mouse enqueue count should match");
    Expect(summary.enqueuedKeyboard == 1, "Keyboard enqueue count should match");
    Expect(summary.droppedMouseMoves == 1, "Dropped mouse move count should match");
    Expect(summary.droppedOtherEvents == 1, "Dropped non-mouse-move count should match");
    Expect(summary.mergedMouseMoves == 4, "Merged mouse move count should match");
    Expect(summary.maxQueueDepth == 3, "Max queue depth should track the deepest enqueue");

    Expect(summary.mouseQueueWait.count == 3, "Mouse queue wait count should match");
    Expect(summary.mouseQueueWait.averageUs == 300, "Mouse queue wait average should match");
    Expect(summary.mouseQueueWait.p95Us == 500, "Mouse queue wait p95 should use the highest sample for a tiny set");
    Expect(summary.mouseInject.count == 2, "Mouse inject count should match");
    Expect(summary.keyboardQueueWait.count == 1, "Keyboard queue wait count should match");
    Expect(summary.keyboardInject.maxUs == 900, "Keyboard inject max should match");
}

void TestLatencyReportFormatting() {
    mwb::InputLatencyStats stats;
    stats.RecordEnqueue(mwb::InputLatencyStats::InputKind::Mouse, 1);
    stats.RecordQueueWait(mwb::InputLatencyStats::InputKind::Mouse, std::chrono::microseconds(250));
    stats.RecordInjectDuration(mwb::InputLatencyStats::InputKind::Mouse, std::chrono::microseconds(500));

    const std::string report = mwb::InputLatencyStats::FormatReport(stats.Snapshot());
    Expect(report.find("[LATENCY] Input pipeline summary") != std::string::npos,
           "Formatted report should include the summary header");
    Expect(report.find("mouse queue wait") != std::string::npos,
           "Formatted report should include mouse queue wait stats");
    Expect(report.find("mouse inject") != std::string::npos,
           "Formatted report should include mouse inject stats");
}

void RunResourceUsageWorkload() {
    mwb::InputLatencyStats stats;

    for (std::size_t i = 0; i < kResourceWorkloadIterations; ++i) {
        stats.RecordEnqueue(mwb::InputLatencyStats::InputKind::Mouse, (i % 8) + 1);
        stats.RecordQueueWait(mwb::InputLatencyStats::InputKind::Mouse,
                              std::chrono::microseconds((i * 7) % 1000));
        stats.RecordInjectDuration(mwb::InputLatencyStats::InputKind::Mouse,
                                   std::chrono::microseconds((i * 11) % 1000));
        if ((i % 4) == 0) {
            stats.RecordMerge(1);
        }
    }

    const auto summary = stats.Snapshot();
    Expect(summary.enqueuedMouse == kResourceWorkloadIterations,
           "Resource workload should enqueue the expected number of mouse batches");
    Expect(summary.mouseQueueWait.count == kResourceWorkloadIterations,
           "Resource workload should record the expected number of queue samples");
    Expect(summary.mouseInject.count == kResourceWorkloadIterations,
           "Resource workload should record the expected number of inject samples");
}

uint64_t TimevalToMicroseconds(const timeval& value) {
    return static_cast<uint64_t>(value.tv_sec) * 1000000ULL + static_cast<uint64_t>(value.tv_usec);
}

void CaptureMemoryField(const std::string& line, const std::string& field, long& value) {
    if (line.rfind(field, 0) != 0) {
        return;
    }

    std::istringstream parser(line.substr(field.size()));
    parser >> value;
}

MemorySnapshot CaptureMemorySnapshot() {
    MemorySnapshot memory;
    std::ifstream status("/proc/self/status");
    std::string line;

    while (std::getline(status, line)) {
        CaptureMemoryField(line, "VmRSS:", memory.currentRssKb);
        CaptureMemoryField(line, "VmSize:", memory.virtualMemoryKb);
        CaptureMemoryField(line, "VmData:", memory.dataMemoryKb);
        CaptureMemoryField(line, "VmStk:", memory.stackMemoryKb);
        CaptureMemoryField(line, "RssAnon:", memory.rssAnonKb);
        CaptureMemoryField(line, "RssFile:", memory.rssFileKb);
        CaptureMemoryField(line, "RssShmem:", memory.rssShmemKb);
    }

    return memory;
}

ResourceSample CaptureResourceSample() {
    ResourceSample sample;
    sample.wallTime = std::chrono::steady_clock::now();
    if (getrusage(RUSAGE_SELF, &sample.usage) != 0) {
        Expect(false, "getrusage should succeed");
    }
    sample.memory = CaptureMemorySnapshot();
    return sample;
}

ResourceDelta CalculateDelta(const ResourceSample& before, const ResourceSample& after) {
    ResourceDelta delta;
    delta.wallUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(after.wallTime - before.wallTime).count());
    delta.userCpuUs = TimevalToMicroseconds(after.usage.ru_utime) - TimevalToMicroseconds(before.usage.ru_utime);
    delta.systemCpuUs = TimevalToMicroseconds(after.usage.ru_stime) - TimevalToMicroseconds(before.usage.ru_stime);
    delta.totalCpuUs = delta.userCpuUs + delta.systemCpuUs;
    delta.onlineCpuCount = sysconf(_SC_NPROCESSORS_ONLN);
    if (delta.wallUs > 0) {
        delta.processCpuPercent = (static_cast<double>(delta.totalCpuUs) * 100.0) / static_cast<double>(delta.wallUs);
        if (delta.onlineCpuCount > 0) {
            delta.hostCpuPercent = delta.processCpuPercent / static_cast<double>(delta.onlineCpuCount);
        }
    }
    delta.currentRssKb = after.memory.currentRssKb;
    delta.peakRssKb = (after.usage.ru_maxrss > delta.currentRssKb) ? after.usage.ru_maxrss : delta.currentRssKb;
    delta.virtualMemoryKb = after.memory.virtualMemoryKb;
    delta.dataMemoryKb = after.memory.dataMemoryKb;
    delta.stackMemoryKb = after.memory.stackMemoryKb;
    delta.rssAnonKb = after.memory.rssAnonKb;
    delta.rssFileKb = after.memory.rssFileKb;
    delta.rssShmemKb = after.memory.rssShmemKb;
    delta.minorFaults = after.usage.ru_minflt - before.usage.ru_minflt;
    delta.majorFaults = after.usage.ru_majflt - before.usage.ru_majflt;
    delta.voluntaryContextSwitches = after.usage.ru_nvcsw - before.usage.ru_nvcsw;
    delta.involuntaryContextSwitches = after.usage.ru_nivcsw - before.usage.ru_nivcsw;
    return delta;
}

bool UseColor() {
    const char* mode = std::getenv("MWB_TEST_COLOR");
    if (mode != nullptr) {
        const std::string value(mode);
        if (value == "always") {
            return true;
        }
        if (value == "never") {
            return false;
        }
    }

    return std::getenv("NO_COLOR") == nullptr && isatty(STDOUT_FILENO) == 1;
}

std::string Colorize(const std::string& text, const char* color, bool useColor) {
    if (!useColor) {
        return text;
    }

    return std::string(color) + text + "\033[0m";
}

std::string FormatMilliseconds(uint64_t microseconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << (static_cast<double>(microseconds) / 1000.0);
    return out.str();
}

std::string FormatPercent(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

std::string FormatSignedMilliseconds(double value) {
    std::ostringstream out;
    out << std::showpos << std::fixed << std::setprecision(3) << value;
    return out.str();
}

std::string FormatDecimalMilliseconds(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

void PrintLatencyRow(const std::string& name,
                     const mwb::InputLatencyStats::DurationSummary& duration,
                     uint64_t expectedCount,
                     uint64_t expectedAverageUs,
                     uint64_t expectedP95Us,
                     uint64_t expectedP99Us,
                     uint64_t expectedMaxUs,
                     bool useColor) {
    const bool pass = duration.count == expectedCount &&
        duration.averageUs == expectedAverageUs &&
        duration.p95Us == expectedP95Us &&
        duration.p99Us == expectedP99Us &&
        duration.maxUs == expectedMaxUs;
    const std::string verdict = Colorize(pass ? "PASS" : "FAIL", pass ? "\033[32m" : "\033[31m", useColor);

    std::cout << "| " << std::left << std::setw(21) << name
              << " | " << std::right << std::setw(7) << duration.count
              << " | " << std::setw(8) << FormatDecimalMilliseconds(static_cast<double>(duration.averageUs) / 1000.0)
              << " | " << std::setw(8) << FormatDecimalMilliseconds(static_cast<double>(duration.p95Us) / 1000.0)
              << " | " << std::setw(8) << FormatDecimalMilliseconds(static_cast<double>(duration.p99Us) / 1000.0)
              << " | " << std::setw(8) << FormatDecimalMilliseconds(static_cast<double>(duration.maxUs) / 1000.0)
              << " | " << std::setw(14) << duration.recentSampleCount
              << " | " << std::setw(4) << verdict << " |\n";
}

void PrintComparisonRow(const MouseLatencyReference& reference, double measuredAverageMs, bool useColor) {
    const double deltaMs = measuredAverageMs - reference.averageMs;
    const bool subMillisecondDelta = std::abs(deltaMs) < 1.0;
    const std::string verdict = Colorize(subMillisecondDelta ? "CLOSE" : "DIFF",
                                         subMillisecondDelta ? "\033[32m" : "\033[33m",
                                         useColor);

    std::cout << "| " << std::left << std::setw(24) << reference.name
              << " | " << std::setw(21) << reference.basis
              << " | " << std::right << std::setw(8) << FormatDecimalMilliseconds(reference.averageMs)
              << " | " << std::setw(8) << FormatDecimalMilliseconds(reference.maxMs)
              << " | " << std::setw(12) << FormatSignedMilliseconds(deltaMs)
              << " | " << std::setw(5) << verdict << " |\n";
}

void PrintTerminalReport(const mwb::InputLatencyStats::Summary& summary,
                         const ResourceDelta& resources) {
    const bool useColor = UseColor();
    const bool countersPass = summary.enabled &&
        summary.enqueuedMouse == 2 &&
        summary.enqueuedKeyboard == 1 &&
        summary.droppedMouseMoves == 1 &&
        summary.droppedOtherEvents == 1 &&
        summary.mergedMouseMoves == 4 &&
        summary.maxQueueDepth == 3;
    const bool pass = g_failures == 0 && countersPass;

    std::cout << "\n"
              << Colorize("Input latency collector and resource usage test", "\033[1m", useColor) << "\n"
              << "+----------------------+--------------------------------------------------+\n"
              << "| Verdict              | " << std::left << std::setw(48)
              << Colorize(pass ? "PASS - deterministic latency checks are exact"
                              : "FAIL - see diagnostics above",
                          pass ? "\033[32m" : "\033[31m",
                          useColor)
              << " |\n"
              << "| Scope                | " << std::setw(48)
              << "local collector only; no network or hardware" << " |\n"
              << "+----------------------+--------------------------------------------------+\n\n";

    std::cout << Colorize("Collector latency metrics (milliseconds)", "\033[36m", useColor) << "\n"
              << "+-----------------------+---------+----------+----------+----------+----------+----------------+------+\n"
              << "| Metric                | Samples |   Avg ms |   P95 ms |   P99 ms |   Max ms | Recent samples | Test |\n"
              << "+-----------------------+---------+----------+----------+----------+----------+----------------+------+\n";
    PrintLatencyRow("mouse queue wait", summary.mouseQueueWait, 3, 300, 500, 500, 500, useColor);
    PrintLatencyRow("mouse inject", summary.mouseInject, 2, 300, 400, 400, 400, useColor);
    PrintLatencyRow("keyboard queue wait", summary.keyboardQueueWait, 1, 700, 700, 700, 700, useColor);
    PrintLatencyRow("keyboard inject", summary.keyboardInject, 1, 900, 900, 900, 900, useColor);
    std::cout << "+-----------------------+---------+----------+----------+----------+----------+----------------+------+\n\n";

    const double measuredMouseAverageMs =
        static_cast<double>(summary.mouseQueueWait.averageUs + summary.mouseInject.averageUs) / 1000.0;
    const double measuredMouseMaxMs =
        static_cast<double>(summary.mouseQueueWait.maxUs + summary.mouseInject.maxUs) / 1000.0;
    const MouseLatencyReference references[] = {
        {"MWB measured path", "queue + inject", measuredMouseAverageMs, measuredMouseMaxMs},
        {"wired USB mouse", "1000 Hz report", 0.500, 1.000},
        {"2.4GHz USB wireless", "1000 Hz report", 0.500, 1.000},
        {"Bluetooth LE floor", "7.5 ms interval", 3.750, 7.500},
    };

    std::cout << Colorize("Reference comparison (collector-only; cross-host probe is authoritative)", "\033[36m", useColor) << "\n"
              << "+--------------------------+-----------------------+----------+----------+--------------+-------+\n"
              << "| Path                     | Basis                 |   Avg ms |   Max ms | MWB avg diff | Class |\n"
              << "+--------------------------+-----------------------+----------+----------+--------------+-------+\n";
    for (const auto& reference : references) {
        PrintComparisonRow(reference, measuredMouseAverageMs, useColor);
    }
    std::cout << "+--------------------------+-----------------------+----------+----------+--------------+-------+\n\n";

    std::cout << Colorize("Input pipeline counters", "\033[36m", useColor) << "\n"
              << "+--------------------------+-------+\n"
              << "| Counter                  | Value |\n"
              << "+--------------------------+-------+\n"
              << "| enqueued mouse           | " << std::setw(5) << summary.enqueuedMouse << " |\n"
              << "| enqueued keyboard        | " << std::setw(5) << summary.enqueuedKeyboard << " |\n"
              << "| dropped mouse moves      | " << std::setw(5) << summary.droppedMouseMoves << " |\n"
              << "| dropped other events     | " << std::setw(5) << summary.droppedOtherEvents << " |\n"
              << "| merged mouse moves       | " << std::setw(5) << summary.mergedMouseMoves << " |\n"
              << "| max queue depth          | " << std::setw(5) << summary.maxQueueDepth << " |\n"
              << "+--------------------------+-------+\n\n";

    std::cout << Colorize("Resource usage", "\033[36m", useColor) << "\n"
              << "+-------------------------------+------------+-------+\n"
              << "| Metric                        |      Value | Unit  |\n"
              << "+-------------------------------+------------+-------+\n"
              << "| measured workload iterations  | " << std::setw(10) << kResourceWorkloadIterations << " | count |\n"
              << "| wall time                     | " << std::setw(10) << FormatMilliseconds(resources.wallUs) << " | ms    |\n"
              << "| user CPU                      | " << std::setw(10) << FormatMilliseconds(resources.userCpuUs) << " | ms    |\n"
              << "| system CPU                    | " << std::setw(10) << FormatMilliseconds(resources.systemCpuUs) << " | ms    |\n"
              << "| total CPU                     | " << std::setw(10) << FormatMilliseconds(resources.totalCpuUs) << " | ms    |\n"
              << "| process CPU usage             | " << std::setw(10) << FormatPercent(resources.processCpuPercent) << " | %     |\n"
              << "| host-normalized CPU usage     | " << std::setw(10) << FormatPercent(resources.hostCpuPercent) << " | %     |\n"
              << "| online CPU cores              | " << std::setw(10) << resources.onlineCpuCount << " | count |\n"
              << "| current resident memory       | " << std::setw(10) << resources.currentRssKb << " | KiB   |\n"
              << "| peak resident set size        | " << std::setw(10) << resources.peakRssKb << " | KiB   |\n"
              << "| virtual memory size           | " << std::setw(10) << resources.virtualMemoryKb << " | KiB   |\n"
              << "| data memory                   | " << std::setw(10) << resources.dataMemoryKb << " | KiB   |\n"
              << "| stack memory                  | " << std::setw(10) << resources.stackMemoryKb << " | KiB   |\n"
              << "| anonymous resident memory     | " << std::setw(10) << resources.rssAnonKb << " | KiB   |\n"
              << "| file-backed resident memory   | " << std::setw(10) << resources.rssFileKb << " | KiB   |\n"
              << "| shared resident memory        | " << std::setw(10) << resources.rssShmemKb << " | KiB   |\n"
              << "| minor page faults             | " << std::setw(10) << resources.minorFaults << " | count |\n"
              << "| major page faults             | " << std::setw(10) << resources.majorFaults << " | count |\n"
              << "| voluntary context switches    | " << std::setw(10) << resources.voluntaryContextSwitches << " | count |\n"
              << "| involuntary context switches  | " << std::setw(10) << resources.involuntaryContextSwitches << " | count |\n"
              << "+-------------------------------+------------+-------+\n";
}

} // namespace

int main() {
    const ResourceSample before = CaptureResourceSample();
    TestLatencySnapshotTracksCountsAndPercentiles();
    TestLatencyReportFormatting();
    RunResourceUsageWorkload();
    mwb::InputLatencyStats reportStats;
    PopulateDeterministicLatencySamples(reportStats);
    const auto summary = reportStats.Snapshot();
    const ResourceSample after = CaptureResourceSample();

    PrintTerminalReport(summary, CalculateDelta(before, after));

    if (g_failures == 0) {
        return 0;
    }

    std::cerr << g_failures << " input latency test(s) failed." << std::endl;
    return 1;
}
