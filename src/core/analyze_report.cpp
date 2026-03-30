// AnalyzeReport: collects structured events during codec detection accuracy
// testing (-a mode) and renders a human-readable summary to stderr.
// Note: this tests how well untrunc's heuristics detect codec structure from
// raw bytes, not whether the file itself is healthy.

#include "analyze_report.h"

#include <iomanip>
#include <iostream>
#include <sstream>

std::string AnalyzeReport::durationStr(int64_t ms) {
	if (ms < 1000) return std::to_string(ms) + "ms";
	std::ostringstream oss;
	oss << std::fixed << std::setprecision(1) << (ms / 1000.0) << "s";
	return oss.str();
}

// Public interface

AnalyzeReport::AnalyzeReport() : start_(std::chrono::steady_clock::now()) {}

void AnalyzeReport::onFrameAnalyzed() {
	frames_analyzed_++;
}

void AnalyzeReport::onMismatch(int track_idx, int sample_idx, const std::string &offset,
                               const std::string &description) {
	mismatches_.push_back({track_idx, sample_idx, offset, description});
}

AnalyzeStatus AnalyzeReport::status() const {
	return mismatches_.empty() ? AnalyzeStatus::Success : AnalyzeStatus::Mismatch;
}

int AnalyzeReport::exitCode() const {
	return mismatches_.empty() ? 0 : 1;
}

void AnalyzeReport::renderSummary() const {
	auto now = std::chrono::steady_clock::now();
	int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count();

	auto s = status();
	std::cerr << "Detection accuracy test "
	          << (s == AnalyzeStatus::Mismatch ? "complete (mismatches found)" : "complete")
	          << " in " << durationStr(ms) << '\n';
	std::cerr << "  Frames analyzed:      " << frames_analyzed_ << '\n';
	if (!mismatches_.empty()) {
		std::cerr << "  Detection mismatches: " << mismatches_.size() << '\n';
		for (const auto &m : mismatches_)
			std::cerr << "    [track " << m.track_idx << "] sample " << m.sample_idx
			          << " at " << m.offset << ": " << m.description << '\n';
	}
}

void AnalyzeReport::finish() const {
	renderSummary();
}
