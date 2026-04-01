#pragma once
#include <memory>
#include <optional>
#include "util/common.h"
#include "sps-info.h"

class Atom;

class AvcConfig {
  public:
	AvcConfig() = default;
	AvcConfig(const Atom *stsd);
	~AvcConfig();
	AvcConfig(AvcConfig &&) = default;
	bool is_ok = false;
	int nal_length_size = 4; // bytes per NAL length prefix (from avcC lengthSizeMinusOne)
	int profile_idc = 0;     // H.264 profile (e.g., 66=Baseline, 100=High)
	int level_idc = 0;       // H.264 level * 10 (e.g., 31 = Level 3.1)
	std::unique_ptr<SpsInfo> sps_info_;

	// Scans up to scan_limit bytes of a length-prefixed NAL stream for the first SPS NAL
	// unit and returns the parsed SpsInfo. Returns nullopt if none is found.
	// Note: does not work for real MP4 mdat (AVCC format stores SPS in the avcC box, not
	// in mdat); intended for raw NAL streams such as Annex B or synthetic test streams.
	static std::optional<SpsInfo> findSpsInStream(const uchar *data, int size, int nal_length_size,
	                                              int scan_limit = 65536);

	// Parses the raw payload bytes of an avcC box (the bytes immediately after the
	// "avcC" four-character box name). Returns nullopt if the payload is invalid.
	static std::optional<AvcConfig> fromAvcCPayload(const uchar *payload, int len);

  private:
	bool decode(const uchar *start);
};
