#include "frame_info.h"

#include <iostream>
#include "codec/codec.h"
#include "util/common.h"

FrameInfo::FrameInfo(int track_idx, Codec &c, off_t offset, uint length)
    : track_idx_(track_idx), keyframe_(c.was_keyframe_), audio_duration_(c.audio_duration_), offset_(offset),
      length_(length), should_dump_(c.should_dump_) {}

FrameInfo::FrameInfo(int track_idx, bool was_keyframe, uint audio_duration, off_t offset, uint length)
    : track_idx_(track_idx), keyframe_(was_keyframe), audio_duration_(audio_duration), offset_(offset), length_(length),
      should_dump_(false) {}

FrameInfo::operator bool() const {
	return length_;
}

bool operator==(const FrameInfo &a, const FrameInfo &b) {
	return a.length_ == b.length_ && a.track_idx_ == b.track_idx_ &&
	       (a.keyframe_ == b.keyframe_ || g_options.ignore_keyframe_mismatch);
}

bool operator!=(const FrameInfo &a, const FrameInfo &b) {
	return !(a == b);
}

std::ostream &operator<<(std::ostream &out, const FrameInfo &fi) {
	return out << ss("[", fi.track_idx_, "] ", hexIf(fi.length_), ", ", fi.keyframe_, ", ", fi.audio_duration_);
}
