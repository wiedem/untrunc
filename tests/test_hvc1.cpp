#include "test_harness.h"
#include <cstring>

#include "codec/hvc1/hvc1.h"
#include "track/sample_stats.h"

// Each synthetic NAL unit: 4-byte big-endian length prefix + 2-byte NAL header
// + kDataBytes bytes of zeroed payload.
static constexpr int kDataBytes = 8;               // >= 8 satisfies the slice minimum-size check
static constexpr int kNalPayload = 2 + kDataBytes; // NAL header + data
static constexpr int kNalSize = 4 + kNalPayload;   // length prefix + payload

// Write one AVCC-format H265 NAL unit into buf.
// nal_type: H265 NAL unit type (e.g. 1=TRAIL_R, 32=VPS, 33=SPS, 34=PPS, 19=IDR_W_RADL)
// first_slice_flag: sets first_slice_segment_in_pic_flag (bit 7 of first data byte)
static void write_nal(uchar *buf, int nal_type, int first_slice_flag = 0) {
	// 4-byte big-endian length (number of bytes after the length field)
	buf[0] = 0;
	buf[1] = 0;
	buf[2] = 0;
	buf[3] = (uchar)kNalPayload;
	// NAL header byte 0: nal_unit_type occupies bits 6:1
	buf[4] = (uchar)((nal_type << 1) & 0xfe);
	// NAL header byte 1: nuh_temporal_id_plus1 must be non-zero for non-EOB NALs
	buf[5] = 0x01;
	// First data byte encodes first_slice_segment_in_pic_flag in bit 7
	buf[6] = first_slice_flag ? 0x80 : 0x00;
	memset(buf + 7, 0, kDataBytes - 1);
}

// BUG-001: getLengths() in hvc1.cpp overshoots into the next keyframe's inline
// VPS/SPS/PPS NAL units when no AUD precedes the keyframe.
//
// This test builds a buffer with two logical access units:
//   Sample N:   [TRAIL_R slice]
//   Sample N+1: [VPS][SPS][PPS][IDR_W_RADL]  (no AUD before the keyframe)
//
// hvc1GetLengths() is called with the full buffer (maxlength covers both
// samples). The correct result is length == kNalSize (one TRAIL_R NAL).
// Without the fix it returns length == 4 * kNalSize (TRAIL_R + VPS + SPS + PPS).
void test_hvc1() {
	std::cout << "test_hvc1:\n";

	// Build the buffer: TRAIL_R | VPS | SPS | PPS | IDR_W_RADL
	static constexpr int kNumNals = 5;
	uchar buf[kNumNals * kNalSize];

	write_nal(buf + 0 * kNalSize, /*TRAIL_R*/ 1, /*first_slice_flag=*/1);
	write_nal(buf + 1 * kNalSize, /*VPS*/ 32);
	write_nal(buf + 2 * kNalSize, /*SPS*/ 33);
	write_nal(buf + 3 * kNalSize, /*PPS*/ 34);
	write_nal(buf + 4 * kNalSize, /*IDR*/ 19, /*first_slice_flag=*/1);

	// Default SampleSizeStats: upper_bound=0 and lower_bound=0, so
	// wouldExceed() and isBigEnough() always return false. The loop therefore
	// only stops at an explicit return inside getLengths.
	SampleSizeStats stats;

	bool was_keyframe = false;
	// loadAfter callback: the buffer is contiguous, so just return buf+offset.
	auto load_after = [&](off_t offset) -> const uchar * { return buf + offset; };

	auto r = hvc1GetLengths(buf, sizeof(buf), &stats, was_keyframe, load_after);

	// Without the fix: r.length == 4 * kNalSize (TRAIL_R + VPS + SPS + PPS).
	// With the fix:    r.length == kNalSize      (TRAIL_R only).
	CHECK_EQ(r.length, kNalSize);
}
