#include "nal.h"

#include <iostream>
#include <cstdint>
#include <cstring>

#include "util/common.h"

using namespace std;

NalInfo::NalInfo(const uchar *start, int max_size, int nal_length_size) {
	is_ok = parseNal(start, max_size, nal_length_size);
}

// Returns false when start does not look like a valid NAL unit.
bool NalInfo::parseNal(const uchar *buffer, uint32_t maxlength, int nal_length_size) {
	// For 4-byte length fields: the MSB must be 0 for any reasonable frame size (< 16 MB).
	// This early check avoids misidentifying random data as a valid NAL.
	if (nal_length_size == 4 && buffer[0] != 0) {
		logg(V, "First byte expected 0\n");
		return false;
	}

	// FIXIT: only true if 'avcc' bytestream standard used, not 'Annex B'!
	//        https://stackoverflow.com/a/24890903
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
		logg(W2, "buffer exceeded by: ", length_ - maxlength, '\n');
		return false;
	}
	buffer += nal_length_size;
	if (*buffer & (1 << 7)) {
		logg(V, "Warning: Forbidden first bit 1\n");
		is_forbidden_set_ = true;
		// sometimes the length is still correct
		if (!g_options.ignore_forbidden_nal_bit) return false;
	}
	ref_idc_ = *buffer >> 5;
	logg(V, "Ref idc: ", ref_idc_, "\n");

	nal_type_ = *buffer & 0x1f;
	logg(V, "Nal type: ", nal_type_, "\n");
	if (nal_type_ == 0) {
		logg(W2, "0-type NAL-unit (len=", len, ", type=", nal_type_, ")\n");
		if (len == 0) return false;
	}
	if (nal_type_ != NAL_SLICE && nal_type_ != NAL_IDR_SLICE && nal_type_ != NAL_SPS) return true;

	// check size is reasonable
	if (len < 8) {
		logg(W2, "very short NAL-unit! (len=", len, ", type=", nal_type_, ")\n");
	}

	buffer++; // skip nal header
	data_ = buffer;
	return true;
}
