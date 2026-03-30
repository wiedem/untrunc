#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

struct AnalyzeMismatch {
	int track_idx;
	int sample_idx;
	std::string offset; // formatted, e.g. "0x1234 / 0x5678"
	std::string description;
};

enum class AnalyzeStatus { Success, Mismatch };

class AnalyzeReport {
  public:
	AnalyzeReport();

	// Event interface (called during analysis)
	void onFrameAnalyzed();
	void onMismatch(int track_idx, int sample_idx, const std::string &offset, const std::string &description);

	// Output (call once at end of run)
	void finish() const;
	int exitCode() const;

	// Accessors for testing
	AnalyzeStatus status() const;
	int64_t framesAnalyzed() const { return frames_analyzed_; }
	const std::vector<AnalyzeMismatch> &mismatches() const { return mismatches_; }

  private:
	int64_t frames_analyzed_ = 0;
	std::vector<AnalyzeMismatch> mismatches_;
	std::chrono::steady_clock::time_point start_;

	void renderSummary() const;

	static std::string durationStr(int64_t ms);
};
