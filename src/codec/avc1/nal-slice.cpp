#include "nal-slice.h"

#include <iostream>

#include "util/bitreader.h"
#include "util/common.h"
#include "sps-info.h"
#include "nal.h"

using namespace std;

SliceInfo::SliceInfo(const NalInfo &nal_info, const SpsInfo &sps) {
	is_ok = decode(nal_info, sps);
}

bool SliceInfo::isInNewFrame(const SliceInfo &previous_slice) {
	// H.264 7.4.1.2.4: detection of the first VCL NAL unit of a primary coded picture.
	// Checks are ordered by frequency of occurrence.

	if (previous_slice.frame_num != frame_num) {
		logg(V, "Different frame number\n");
		return true;
	}
	if (previous_slice.pps_id != pps_id) {
		logg(W, "Different pps_id\n");
		return true;
	}
	if (previous_slice.idr_pic_flag != idr_pic_flag) {
		logg(W2, "Different nal type (5, 1)\n");
		return true;
	}

	// field_pic_flag and bottom_field_flag are normative boundary conditions.
	// They were observed to cause false splits on some real-world files, so they
	// share the strict_nal_frame_check path below in spirit, but for now are kept
	// unconditional because the false-split cases were not reproduced recently.
	if (previous_slice.field_pic_flag != field_pic_flag) {
		logg(W2, "Different field pic flag\n");
		return true;
	}
	if (previous_slice.bottom_pic_flag != bottom_pic_flag && previous_slice.bottom_pic_flag != -1 &&
	    previous_slice.bottom_pic_flag != -1) {
		logg(W2, "Different bottom pic flag\n");
		return true;
	}

	// poc_lsb (poc_type=0): active by default but disabled for Canon CAEP and Sony XAVC
	// via strict_nal_frame_check=false, because those cameras produce streams where this
	// check causes false splits between multi-slice frames.
	// Not implemented: delta_poc_bottom (poc_type=0, field-coded, interlaced-only).
	if (g_options.strict_nal_frame_check && previous_slice.poc_type == 0 && poc_type == 0 &&
	    previous_slice.poc_lsb != poc_lsb) {
		logg(W2, "Different poc lsb\n");
		return true;
	}

	// poc_type=1: delta_pic_order_cnt[0/1] not implemented (extremely rare encoder config).

	if (g_options.strict_nal_frame_check && previous_slice.idr_pic_flag == 1 && idr_pic_flag == 1 &&
	    previous_slice.idr_pic_id != idr_pic_id) {
		logg(W, "Different idr pic id for keyframe\n");
		return true;
	}
	return false;
}

bool SliceInfo::decode(const NalInfo &nal_info, const SpsInfo &sps) {
	const uchar *start = nal_info.data_;
	int offset = 0;
	first_mb = readGolomb(start, offset);
	//TODO is there a max number (so we could validate?)
	logg(VV, "First mb: ", first_mb, '\n');

	slice_type = readGolomb(start, offset);
	if (slice_type > 9) {
		logg(W, "Invalid slice type, probably this is not an avc1 sample\n");
		return false;
	}
	pps_id = readGolomb(start, offset);
	logg(VV, "pic paramter set id: ", pps_id, '\n');
	//pps id: should be taked from master context (h264_slice.c:1257

	//assume separate coloud plane flag is 0
	//otherwise we would have to read colour_plane_id which is 2 bits

	//assuming same sps for all frames:
	frame_num = readBits(sps.log2_max_frame_num, start, offset);
	logg(VV, "Frame num: ", frame_num, '\n');

	//read 2 flags
	field_pic_flag = 0;
	bottom_pic_flag = 0;
	if (!sps.frame_mbs_only_flag) {
		field_pic_flag = readBits(1, start, offset);
		if (field_pic_flag) {
			bottom_pic_flag = readBits(1, start, offset);
		}
	}
	idr_pic_flag = (nal_info.nal_type_ == NAL_IDR_SLICE) ? 1 : 0;
	if (nal_info.nal_type_ == NAL_IDR_SLICE) {
		idr_pic_id = readGolomb(start, offset);
	}

	// poc_lsb is only present in the slice header for poc_type=0.
	// delta_poc_bottom (poc_type=0, field-coded) and delta_pic_order_cnt (poc_type=1)
	// are not parsed: interlaced content and poc_type=1 streams are too rare to warrant
	// the added complexity.
	if (sps.poc_type == 0) {
		poc_lsb = readBits(sps.log2_max_poc_lsb, start, offset);
		logg(VV, "Poc lsb: ", poc_lsb, '\n');
	}
	return true;
}
