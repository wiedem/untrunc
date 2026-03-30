#pragma once
#include <functional>
#include <vector>
#include "util/common.h"

class Codec;
class SampleSizeStats;

struct Hvc1LenResult {
	std::vector<int> alternative_lengths;
	int length = 0;
};

// Exposed for unit tests. Scans AVCC-format H265 NAL units starting at start
// (maxlength bytes available) and returns the detected length of the current
// access unit. load_after(offset) must return a pointer to start+offset (used
// to refresh the read window after each NAL; in production this calls
// mp4->loadFragment).
Hvc1LenResult hvc1GetLengths(const uchar *start, uint maxlength, SampleSizeStats *ss_stats, bool &was_keyframe,
                             std::function<const uchar *(off_t)> load_after, int nal_length_size = 4);

int getSizeHvc1(Codec *self, const uchar *start, uint maxlength);
