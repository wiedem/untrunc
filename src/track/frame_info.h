#pragma once
#include <iosfwd>
#include "util/common.h"

class Codec;

class FrameInfo {
  public:
	FrameInfo() = default;
	FrameInfo(int track_idx, Codec &c, off_t offset, uint length);
	FrameInfo(int track_idx, bool was_keyframe, uint audio_duration, off_t offset, uint length);
	explicit operator bool() const;
	int track_idx_ = -1;

	bool keyframe_ = false;
	uint audio_duration_ = 0;
	off_t offset_ = 0;
	uint length_ = 0;
	bool should_dump_ = false;
	uint pad_afterwards_ = 0;
};

bool operator==(const FrameInfo &lhs, const FrameInfo &rhs);
bool operator!=(const FrameInfo &lhs, const FrameInfo &rhs);
std::ostream &operator<<(std::ostream &out, const FrameInfo &fi);
