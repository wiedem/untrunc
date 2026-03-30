#pragma once
#include "util/common.h"

/* H265 NAL unit types (H.265 Table 7-1) */
enum {
	// Trailing slice types (non-IRAP)
	NAL_TRAIL_N = 0,
	NAL_TRAIL_R = 1,
	NAL_TSA_N = 2,
	NAL_TSA_R = 3,
	NAL_STSA_N = 4,
	NAL_STSA_R = 5,
	NAL_RADL_N = 6,
	NAL_RADL_R = 7,
	NAL_RASL_N = 8,
	NAL_RASL_R = 9,
	// IRAP slice types (keyframes / random access points)
	NAL_BLA_W_LP = 16,
	NAL_BLA_W_RADL = 17,
	NAL_BLA_N_LP = 18,
	NAL_IDR_W_RADL = 19,
	NAL_IDR_N_LP = 20,
	NAL_CRA_NUT = 21,
	NAL_RSV_IRAP_22 = 22,
	NAL_RSV_IRAP_23 = 23,
	// Non-VCL
	NAL_AUD = 35,
	NAL_EOB_NUT = 37,
	NAL_FILLER_DATA = 38,
};

class H265NalInfo {
  public:
	H265NalInfo() = default;
	H265NalInfo(const uchar *start, int max_size, int nal_length_size = 4);

	uint length_ = 0;
	int nuh_layer_id_ = 0;
	int nal_type_ = 0;
	int nuh_temporal_id_plus1 = 0;

	bool is_ok = false; // did parsing work
	bool is_forbidden_set_ = false;
	const uchar *data_ = nullptr;
	bool parseNal(const uchar *start, uint32_t max_size, int nal_length_size);
};

bool h265IsSlice(int nal_type);
bool h265IsKeyframe(int nal_type);
