#include "nal.h"

#include <iostream>
#include <cstdint>
#include <cstring>

#include "util/common.h"

using namespace std;

H265NalInfo::H265NalInfo(const uchar *start, int max_size, int nal_length_size) {
	is_ok = parseNal(start, max_size, nal_length_size);
}

bool h265IsSlice(int nal_type) {
	// Trailing/sub-layer/leading slices (0-9) and all IRAP types (16-23)
	return (nal_type >= NAL_TRAIL_N && nal_type <= NAL_RASL_R) ||
	       (nal_type >= NAL_BLA_W_LP && nal_type <= NAL_RSV_IRAP_23);
}

bool h265IsKeyframe(int nal_type) {
	// All IRAP types are random access points: BLA, IDR, CRA, reserved IRAP
	return nal_type >= NAL_BLA_W_LP && nal_type <= NAL_RSV_IRAP_23;
}

// see avc1/nal.cpp for more detailed comments
bool H265NalInfo::parseNal(const uchar *buffer, uint32_t maxlength, int nal_length_size) {
	// For 4-byte length fields: the MSB must be 0 for any reasonable frame size (< 16 MB).
	if (nal_length_size == 4 && buffer[0] != 0) {
		logg(V, "First byte expected 0\n");
		return false;
	}

	// following only works with 'avcc' bytestream, see avc1/nal.cpp
	uint32_t len;
	switch (nal_length_size) {
	case 1:
		len = buffer[0];
		break;
	case 2:
		len = (uint32_t(buffer[0]) << 8) | buffer[1];
		break;
	default: // 4
		memcpy(&len, buffer, sizeof(len));
		len = swap32(len);
		break;
	}
	length_ = len + nal_length_size;
	logg(V, "Length: ", len, "+", nal_length_size, "\n");

	if (length_ > maxlength) {
		logg(W2, "buffer exceeded by: ", length_ - maxlength, " | ");
		if (g_options.log_mode >= W2) printBuffer(buffer, 32);
		return false;
	}
	buffer += nal_length_size;
	if (*buffer & (1 << 7)) {
		logg(V, "Warning: Forbidden first bit 1\n");
		is_forbidden_set_ = true;
		// sometimes the length is still correct
		if (!g_options.ignore_forbidden_nal_bit) return false;
	}
	nal_type_ = *buffer >> 1;
	logg(V, "Nal type: ", nal_type_, "\n");

	if (nal_type_ > 40) {
		logg(V, "nal_type_ too big (> 40)\n");
		return false;
	}

	nuh_layer_id_ = (*buffer & 1) << 6 | (*(buffer + 1) >> 5);
	logg(V, "nuh_layer_id: ", nuh_layer_id_, "\n");

	nuh_temporal_id_plus1 = (*(buffer + 1) & 0b111);
	logg(V, "nuh_temporal_id_plus1: ", nuh_temporal_id_plus1, "\n");

	if ((nal_type_ == NAL_EOB_NUT && nuh_temporal_id_plus1) || (nal_type_ != NAL_EOB_NUT && !nuh_temporal_id_plus1)) {
		logg(V, "Warning: nuh_temporal_id_plus1 is wrong\n");
		return false;
	}

	if (h265IsSlice(nal_type_)) {
		// check size is reasonable
		if (len < 8) {
			logg(W2, "very short NAL-unit! (len=", len, ", type=", nal_type_, ")\n");
		}
		data_ = buffer + 2;
	}
	return true;
}
