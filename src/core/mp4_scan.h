// Pure byte-level scanning helpers for MP4 mdat traversal.
// These functions inspect raw bytes only and have no side effects.

#pragma once
#include <cstring>
#include "util/common.h"
#include "atom/atom.h" // isValidAtomName, swap32

// Returns 8 if the 8 bytes at start form an mdat atom header, 0 otherwise.
inline int mdatHeaderSkipSize(const uchar *start) {
	return (start[4] == 'm' && start[5] == 'd' && start[6] == 'a' && start[7] == 't') ? 8 : 0;
}

// Returns the size of a skippable MP4 atom at start, given remaining bytes
// available in the buffer. Returns 0 if the bytes should not be skipped.
//
// Rules:
//   - The 4-byte name field must be a valid atom name.
//   - Atoms >= 1 MiB are not skipped (likely not a real container atom).
//   - The atom must fit within remaining bytes.
//   - "tmcd" atoms are never skipped: they may appear as normal video payload.
inline int atomSkipSize(const uchar *start, int64_t remaining) {
	if (!isValidAtomName(start + 4)) return 0;
	uint atom_len;
	memcpy(&atom_len, start, sizeof(atom_len));
	atom_len = swap32(atom_len);
	if (atom_len == 0 || atom_len >= (1u << 20)) return 0;
	if ((int64_t)atom_len > remaining) return 0;
	if (start[4] == 't' && start[5] == 'm' && start[6] == 'c' && start[7] == 'd') return 0;
	return (int)atom_len;
}
