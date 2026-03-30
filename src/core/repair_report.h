#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct RepairWarning {
	std::string message;
	std::string offset; // hex string, empty if not applicable
};

struct TrackStat {
	std::string name;
	int64_t samples;
	int64_t keyframes; // 0 if not applicable
};

struct UnknownSeqDetail {
	std::string offset; // formatted, e.g. "0x1234 / 0x5678"
	int64_t length;
};

enum class RepairStatus { Success, Partial, Error };

class RepairReport {
  public:
	RepairReport();

	// Event interface (called during/after repair)
	void onProgress(int64_t current, int64_t total);
	void onTrackStats(std::vector<TrackStat> stats);
	void onUnknownSequences(std::vector<UnknownSeqDetail> sequences, double percentage);
	void onPrematureEnd(double percentage);
	void onWarning(const std::string &message, const std::string &offset = {});
	void onFatalError(const std::string &message);
	void onOutputFile(const std::string &path);

	// Output (call once at end of run)
	void finish() const;
	int exitCode() const;

	// Accessors for testing
	RepairStatus status() const;
	int64_t chunksRepaired() const;
	size_t unknownSeqCount() const { return unknown_seqs_.size(); }
	int64_t unknownSeqBytes() const;
	bool isPrematureEnd() const { return premature_pct_ >= 0.0; }
	const std::string &outputFile() const { return output_file_; }
	const std::vector<RepairWarning> &warnings() const { return warnings_; }
	const std::vector<std::string> &errors() const { return errors_; }
	const std::vector<TrackStat> &trackStats() const { return track_stats_; }
	const std::vector<UnknownSeqDetail> &unknownSeqs() const { return unknown_seqs_; }

  private:
	std::vector<TrackStat> track_stats_;
	std::vector<UnknownSeqDetail> unknown_seqs_;
	double unknown_seq_pct_ = 0.0;
	double premature_pct_ = -1.0; // -1 means no premature end
	std::vector<RepairWarning> warnings_;
	std::vector<std::string> errors_;
	std::string output_file_;
	bool fatal_ = false;

	std::chrono::steady_clock::time_point start_;
	int64_t last_progress_pct10_ = -1; // tenth-of-percent, for dedup

	void renderSummary() const;

	static std::string durationStr(int64_t ms);
};
