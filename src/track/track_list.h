// Free functions for looking up tracks by codec name.
// These replace the equivalent instance methods on Mp4, enabling use
// in any context where a std::vector<Track> is available.

#pragma once
#include <vector>
#include <string>
#include <stdexcept>

#include "track.h"

namespace TrackList {

// Returns the index of the first track with codec name, or -1 if not found.
inline int findIdx(const std::vector<Track> &tracks, const std::string &name) {
	for (int i = 0; i < (int)tracks.size(); i++)
		if (tracks[i].codec_.name_ == name) return i;
	return -1;
}

inline bool has(const std::vector<Track> &tracks, const std::string &name) {
	return findIdx(tracks, name) >= 0;
}

// Returns pointer to the first track with codec name, or nullptr.
inline Track *find(std::vector<Track> &tracks, const std::string &name) {
	int i = findIdx(tracks, name);
	return i >= 0 ? &tracks[i] : nullptr;
}

// Returns the codec name of track at idx. Returns "????" for out-of-range idx.
inline const std::string &nameOf(const std::vector<Track> &tracks, int idx) {
	static const std::string kUnknown = "????";
	if (idx < 0 || (size_t)idx >= tracks.size()) return kUnknown;
	return tracks[idx].codec_.name_;
}

} // namespace TrackList
