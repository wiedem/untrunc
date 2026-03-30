// RepairReport: collects structured events during repair and renders
// a human-readable summary to stderr.

#include "repair_report.h"

#include <iomanip>
#include <iostream>
#include <sstream>

#include "util/common.h" // pretty_bytes

std::string RepairReport::durationStr(int64_t ms) {
	if (ms < 1000) return std::to_string(ms) + "ms";
	std::ostringstream oss;
	oss << std::fixed << std::setprecision(1) << (ms / 1000.0) << "s";
	return oss.str();
}

// Public interface

RepairReport::RepairReport() : start_(std::chrono::steady_clock::now()) {}

void RepairReport::onProgress(int64_t current, int64_t total) {
	if (total <= 0) return;
	int64_t pct10 = (current * 1000) / total;
	if (pct10 == last_progress_pct10_) return;
	last_progress_pct10_ = pct10;
	std::cerr << "  " << (pct10 / 10) << '.' << (pct10 % 10) << "%\r" << std::flush;
}

void RepairReport::onTrackStats(std::vector<TrackStat> stats) {
	track_stats_ = std::move(stats);
}

void RepairReport::onUnknownSequences(std::vector<UnknownSeqDetail> sequences, double percentage) {
	unknown_seqs_ = std::move(sequences);
	unknown_seq_pct_ = percentage;
}

void RepairReport::onPrematureEnd(double percentage) {
	premature_pct_ = percentage;
}

void RepairReport::onWarning(const std::string &message, const std::string &offset) {
	warnings_.push_back({message, offset});
}

void RepairReport::onFatalError(const std::string &message) {
	fatal_ = true;
	errors_.push_back(message);
}

void RepairReport::onOutputFile(const std::string &path) {
	output_file_ = path;
}

int64_t RepairReport::chunksRepaired() const {
	int64_t total = 0;
	for (const auto &t : track_stats_) total += t.samples;
	return total;
}

int64_t RepairReport::unknownSeqBytes() const {
	int64_t total = 0;
	for (const auto &s : unknown_seqs_) total += s.length;
	return total;
}

RepairStatus RepairReport::status() const {
	if (fatal_) return RepairStatus::Error;
	if (premature_pct_ >= 0.0 || !unknown_seqs_.empty()) return RepairStatus::Partial;
	return RepairStatus::Success;
}

int RepairReport::exitCode() const {
	switch (status()) {
	case RepairStatus::Success: return 0;
	case RepairStatus::Partial: return 2;
	case RepairStatus::Error:   return 1;
	}
	return 1;
}

void RepairReport::renderSummary() const {
	auto now = std::chrono::steady_clock::now();
	int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count();

	// Clear any leftover progress line.
	std::cerr << "                    \r";

	auto s = status();
	if (s == RepairStatus::Error) {
		std::cerr << "Repair failed";
		if (!errors_.empty()) std::cerr << ": " << errors_.front();
		std::cerr << '\n';
		return;
	}

	std::cerr << "Repair " << (s == RepairStatus::Partial ? "partial" : "complete");
	std::cerr << " in " << durationStr(ms) << '\n';

	if (!track_stats_.empty()) {
		std::cerr << "  Tracks:            ";
		for (size_t i = 0; i < track_stats_.size(); i++) {
			const auto &t = track_stats_[i];
			if (i) std::cerr << ", ";
			std::cerr << t.name << ": " << t.samples;
			if (t.keyframes > 0) std::cerr << " (" << t.keyframes << " keyframes)";
		}
		std::cerr << '\n';
	}

	if (!unknown_seqs_.empty()) {
		int64_t total_bytes = unknownSeqBytes();
		std::ostringstream pct;
		pct << std::fixed << std::setprecision(4) << unknown_seq_pct_;
		std::cerr << "  Unknown sequences: " << unknown_seqs_.size()
		          << " (" << pretty_bytes(total_bytes) << ", " << pct.str() << "%)\n";
		for (const auto &seq : unknown_seqs_)
			std::cerr << "    at " << seq.offset << ": " << pretty_bytes(seq.length) << '\n';
	}
	if (premature_pct_ >= 0.0) {
		std::ostringstream pct;
		pct << std::fixed << std::setprecision(2) << premature_pct_;
		std::cerr << "  Premature end at:  " << pct.str() << "%\n";
	}
	if (!output_file_.empty())
		std::cerr << "  Output:            " << output_file_ << '\n';
	if (!warnings_.empty())
		std::cerr << "  Warnings:          " << warnings_.size() << '\n';
}

void RepairReport::finish() const {
	renderSummary();
}
