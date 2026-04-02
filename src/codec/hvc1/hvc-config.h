#pragma once
#include <optional>
#include "util/common.h"

class Atom;

class HvcConfig {
  public:
	HvcConfig() = default;
	HvcConfig(const Atom *stsd);
	static std::optional<HvcConfig> fromHvcCPayload(const uchar *payload, int len);
	bool is_ok = false;
	int nal_length_size = 4; // bytes per NAL length prefix (from hvcC lengthSizeMinusOne)
	int profile_idc = 0;     // general_profile_idc (1=Main, 2=Main10, 3=Main Still, 4=Rext, ...)
	int tier_flag = 0;       // general_tier_flag (0=Main tier, 1=High tier)
	int level_idc = 0;       // general_level_idc (e.g. 120=L4.0, 123=L4.1, 150=L5.0)

  private:
	bool decode(const uchar *start, int len);
};
