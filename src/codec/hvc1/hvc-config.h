#pragma once
#include "util/common.h"

class Atom;

class HvcConfig {
  public:
	HvcConfig() = default;
	HvcConfig(const Atom *stsd);
	bool is_ok = false;
	int nal_length_size = 4; // bytes per NAL length prefix (from hvcC lengthSizeMinusOne)

  private:
	bool decode(const uchar *start, int len);
};
