#include "avc-config.h"

#include "iostream"

#include "nal.h"
#include "util/bitreader.h"
#include "util/common.h"
#include "atom/atom.h"

using namespace std;

AvcConfig::AvcConfig(const Atom *stsd) {
	// find avcC payload
	const uchar *start = stsd->content_.data() + 12;
	char pattern[5] = "avcC";
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
		logg(V, "avcC signature not found\n");
		is_ok = false;
		return;
	}
	int off = start - stsd->content_.data();
	int len = stsd->length_ - off;
	logg(V, "found avcC after: ", off, '\n');
	logg(V, "remaining len:", len, '\n');

	is_ok = decode(start);
}

AvcConfig::~AvcConfig() = default;

std::optional<AvcConfig> AvcConfig::fromAvcCPayload(const uchar *payload, int len) {
	// avcC box payload layout (ISO 14496-15):
	//   Byte 0: configurationVersion (must be 1)
	//   Byte 1: AVCProfileIndication (= profile_idc)
	//   Byte 2: profile_compatibility
	//   Byte 3: AVCLevelIndication (= level_idc)
	//   Byte 4: 0b111111xx (reserved | lengthSizeMinusOne)
	if (len < 5) return std::nullopt;
	if (payload[0] != 1) return std::nullopt;             // configurationVersion must be 1
	if ((payload[4] & 0xFC) != 0xFC) return std::nullopt; // top 6 reserved bits must be 1
	AvcConfig cfg;
	cfg.is_ok = true;
	cfg.profile_idc = payload[1];
	cfg.level_idc = payload[3];
	cfg.nal_length_size = (payload[4] & 0x03) + 1;
	return cfg;
}

std::optional<SpsInfo> AvcConfig::findSpsInStream(const uchar *data, int size, int nal_length_size, int scan_limit) {
	int remaining = std::min(size, scan_limit);
	int offset = 0;
	while (remaining > nal_length_size) {
		NalInfo nal(data + offset, remaining, nal_length_size);
		if (!nal.is_ok) break;
		if (nal.nal_type_ == NAL_SPS && nal.data_) {
			SpsInfo sps;
			if (sps.decode(nal.data_)) return sps;
			break;
		}
		if ((int)nal.length_ <= 0) break;
		offset += nal.length_;
		remaining -= nal.length_;
	}
	return std::nullopt;
}

bool AvcConfig::decode(const uchar *start) {
	logg(V, "parsing avcC ...\n");
	// Read lengthSizeMinusOne from byte 4 before readBits advances the pointer.
	nal_length_size = (start[4] & 0x03) + 1;
	logg(V, "avcC nal_length_size: ", nal_length_size, "\n");
	int off = 0;
	int ver = readBits(8, start, off); // config_version, advances start by 1
	if (ver != 1) {
		logg(V, "avcC config version != 1\n");
		return false;
	}
	// start now points to profile_idc (byte 1 of avcC box)
	profile_idc = start[0];
	// start[1] = profile_compatibility (not stored)
	level_idc = start[2];
	start += 3;                              // skip profile_idc, profile_compatibility, level_idc
	uint reserved = readBits(3, start, off); // 111
	if (reserved != 7) {
		logg(V, "avcC - reserved is not reserved: ", reserved, '\n');
		return false;
	}
	uint num_sps = readBits(5, start, off);
	if (num_sps != 1) logg(W, "avcC contains more than 1 SPS");
	uint len_sps = readBits(16, start, off);
	logg(V, "len_sps: ", len_sps, '\n');
	sps_info_ = std::make_unique<SpsInfo>(start);
	return sps_info_->is_ok;
}
