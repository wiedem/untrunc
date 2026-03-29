#pragma once
#include <cmath>
#include <iomanip>
#include <limits>
#include <ostream>

#include "util/common.h"

// Z-score for a 99% confidence interval (one-tailed): P(Z < 2.326) = 0.99
static const double kZScore99 = 2.326;

struct SSTats {
	uint min = std::numeric_limits<uint>::max(), max = 0, upper_bound = 0, lower_bound = 0,
	     lower_bound_2 = 0; // even lower
	uint64_t n = 0;
	double avg = 0, m2 = 0, dev = 0, rel_dev = 0, z_score_max = 0, z_score_min = 0;

	void updateStat(uint sz) {
		n++;
		double delta = sz - avg;
		avg += delta / n;
		m2 += delta * (sz - avg);

		min = std::min(min, sz);
		max = std::max(max, sz);
	};

	void onFinished() {
		if (!n) {
			min = 0;
			return;
		}
		dev = sqrt((double)m2 / n);
		rel_dev = dev / avg;

		z_score_max = (max - avg) / dev;
		z_score_min = (avg - min) / dev;

		double z_score;
		if ((z_score_max > 2.5 && rel_dev > 0.025) || (z_score_max > 2 && n < 35)) {
			upper_bound = avg + 7 * (max - min);
			dbgg("fallback to save upper_bound", upper_bound);
		} else {
			z_score = std::max(std::max(kZScore99, z_score_max + 1), z_score_max * 1.5);
			upper_bound = ceil(avg + dev * z_score);
		}

		if ((z_score_min > 2.5 && rel_dev > 0.025) || (z_score_min > 1 && n < 35)) {
			lower_bound = 0.8 * min;
			dbgg("fallback to save lower_bound", lower_bound);
		} else {
			z_score = std::max(std::max(kZScore99, z_score_min + 1), z_score_min * 1.5);
			lower_bound = ceil(std::max(0., avg - dev * z_score));
		}

		lower_bound_2 = lower_bound >> 5;
	}

	void onConstant(uint sz) {
		min = avg = max = upper_bound = lower_bound = lower_bound_2 = sz;
		n = 100;
	}
};

inline std::ostream &operator<<(std::ostream &out, const SSTats &ss) {
	return out << std::right << std::setw(8) << ss.min << " " << std::setw(8) << int(round(ss.avg)) << " "
	           << std::setw(8) << ss.max << " |" << std::setw(8) << ss.lower_bound << " " << std::setw(8)
	           << ss.upper_bound << " |" << std::setw(8) << std::fixed << std::setprecision(5) << ss.rel_dev << " "
	           << std::setw(8) << ss.z_score_min << " " << std::setw(8) << ss.z_score_max << " " << std::setw(8) << ss.n
	           << " ";
}

struct SampleSizeStats {
	SSTats normal, keyframe, effective_keyframe;

	void updateStat(int sz, bool is_keyframe) {
		if (is_keyframe)
			keyframe.updateStat(sz);
		else
			normal.updateStat(sz);
	}

	void onFinished() {
		normal.onFinished();
		keyframe.onFinished();

		if (!keyframe.n || normal.upper_bound > keyframe.upper_bound) {
			effective_keyframe = normal;
		} else {
			effective_keyframe = keyframe;
		}
	}

	void onConstant(int sz) {
		normal.onConstant(sz);
		keyframe.onConstant(sz);
		effective_keyframe.onConstant(sz);
	}

	// for estimating chunk size
	int averageSize() {
		auto n = normal.n + keyframe.n;
		return normal.avg * ((double)normal.n / n) + keyframe.avg * ((double)keyframe.n / n);
	}

	uint maxAllowedPktSz() { return std::max(normal.upper_bound, keyframe.upper_bound); }

	SSTats &effectiveStat(bool is_keyframe) { return is_keyframe ? effective_keyframe : normal; }

	uint getUpperLimit(bool is_keyframe) { return effectiveStat(is_keyframe).upper_bound; }

	uint getLowerLimit(bool is_keyframe) { return effectiveStat(is_keyframe).lower_bound; }

	bool exceedsAllowed(uint sz, bool is_keyframe) {
		uint limit = getUpperLimit(is_keyframe);
		// dbgg("exceedsAllowed? ", limit, sz, is_keyframe, keyframe.upper_bound, effective_keyframe.upper_bound);
		return limit && sz > limit;
	}

	bool isBigEnough(uint sz, bool is_keyframe) {
		uint limit = getLowerLimit(is_keyframe);
		return limit && sz >= limit;
	}

	bool likelyTooSmall(uint sz) {
		auto &s = normal;
		if (sz < s.lower_bound_2 && s.n > 20 && s.rel_dev < 1) {
			dbgg("likelyTooSmall", sz, s.lower_bound_2);
			return true;
		}
		return false;
	}

	bool wouldExceed(const char *label, uint length, uint additional, bool is_keyframe) {
		if (exceedsAllowed(length + additional, is_keyframe)) {
			logg(V, "new partial ", label, "-length would exceed upper_bound: ", length, " + ", additional, " = ",
			     length + additional, " > ", getUpperLimit(is_keyframe), "  // is_keyframe=", is_keyframe, "\n");
			return true;
		}
		return false;
	}
};
