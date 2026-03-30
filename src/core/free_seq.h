#pragma once
#include <cstdint>
#include <iosfwd>
#include <string>
#include <sys/types.h>

struct FreeSeq {
	off_t offset; // relative
	int64_t sz;
	int prev_track_idx;
	int64_t last_chunk_sz;
	std::string codec_name; // for display

	bool operator<(const FreeSeq &other) const { return offset < other.offset; }
};

std::ostream &operator<<(std::ostream &out, const FreeSeq &x);
