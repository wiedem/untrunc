#include "hvc-config.h"

#include "util/common.h"
#include "atom/atom.h"

using namespace std;

// HEVCDecoderConfigurationRecord (ISO 14496-15):
//   byte  0:    configurationVersion (must be 1)
//   bytes 1-11: general profile/tier/level/constraint flags
//   byte 12:    general_level_idc
//   bytes 13-14: reserved(4) + min_spatial_segmentation_idc(12)
//   byte 15:    reserved(6) + parallelismType(2)
//   byte 16:    reserved(6) + chromaFormat(2)
//   byte 17:    reserved(5) + bitDepthLumaMinus8(3)
//   byte 18:    reserved(5) + bitDepthChromaMinus8(3)
//   bytes 19-20: avgFrameRate
//   byte 21:    constantFrameRate(2) + numTemporalLayers(3) + temporalIdNested(1) + lengthSizeMinusOne(2)
static constexpr int kMinRecordSize = 22;
static constexpr int kLengthSizeByteOffset = 21;

HvcConfig::HvcConfig(const Atom *stsd) {
	// find hvcC payload inside the stsd atom content
	const uchar *start = stsd->content_.data() + 12;
	char pattern[5] = "hvcC";
	int found = 0;
	int limit = stsd->length_ - 16;
	while (limit--) {
		if (*start++ == pattern[found])
			found++;
		else if (found)
			found = 0;
		if (found == 4) break;
	}
	if (found != 4) {
		logg(V, "hvcC signature not found\n");
		is_ok = false;
		return;
	}
	int off = start - stsd->content_.data();
	int remaining = stsd->length_ - off;
	logg(V, "found hvcC after: ", off, '\n');
	is_ok = decode(start, remaining);
}

bool HvcConfig::decode(const uchar *start, int len) {
	if (len < kMinRecordSize) {
		logg(W, "hvcC record too short (", len, " bytes)\n");
		return false;
	}
	if (start[0] != 1) {
		logg(V, "hvcC configurationVersion != 1\n");
		return false;
	}
	// HEVCDecoderConfigurationRecord byte 1:
	//   bits 7-6: general_profile_space (ignored)
	//   bit  5:   general_tier_flag
	//   bits 4-0: general_profile_idc
	profile_idc = start[1] & 0x1F;
	tier_flag = (start[1] >> 5) & 0x01;
	level_idc = start[12]; // general_level_idc
	nal_length_size = (start[kLengthSizeByteOffset] & 0x03) + 1;
	logg(V, "hvcC nal_length_size: ", nal_length_size, "\n");
	return true;
}

std::optional<HvcConfig> HvcConfig::fromHvcCPayload(const uchar *payload, int len) {
	HvcConfig cfg;
	cfg.is_ok = cfg.decode(payload, len);
	if (!cfg.is_ok) return std::nullopt;
	return cfg;
}
