// Mutable scan-state for Mp4::repair() and Mp4::analyze().
// Separated from Mp4 to isolate ephemeral repair state from the parsed
// reference data (tracks_, root_atom_, etc.) that Mp4 owns permanently.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "atom/atom.h"   // BufferedAtom
#include "io/file.h"     // FileRead
#include "util/common.h" // uint

class Track; // forward declaration: only a pointer is stored

struct RepairContext {
	// File access
	std::unique_ptr<FileRead> current_file_;
	std::unique_ptr<BufferedAtom> current_mdat_;
	uint current_maxlength_ = 0;

	// Scan loop state
	bool broken_is_64_ = false;
	int64_t unknown_length_ = 0;
	std::vector<uint> atoms_skipped_;
	uint64_t pkt_idx_ = 0;
	int last_track_idx_ = -1;
	bool done_padding_ = false;
	bool done_padding_after_ = false;
	std::vector<int64_t> unknown_lengths_;
	bool use_offset_map_ = false;
	off_t first_off_rel_ = -1;
	off_t first_off_abs_ = -1;

	// Chunk ordering
	std::vector<std::pair<int, int>> track_order_;
	std::vector<int> track_order_simple_;
	bool trust_simple_track_order_ = false;
	int cycle_size_ = 0;           // (average) size of single track_order_ repetition
	uint64_t next_chunk_idx_ = 0;  // does not count the 'free' track
	bool ignored_chunk_order_ = false;

	// Dynamic patterns
	bool using_dyn_patterns_ = false;

	// Chunk detection helpers
	uint max_part_size_ = 0;
	bool first_chunk_found_ = false;
	int fallback_track_idx_ = -1;

	Track *orig_first_track_ = nullptr;

	// Padding / dummy track
	bool dummy_is_skippable_ = false;
	int dummy_do_padding_skip_ = 0;
	bool has_zero_transitions_ = false;
};
