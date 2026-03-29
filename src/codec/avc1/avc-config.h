#pragma once
#include <memory>
#include "util/common.h"

class Atom;
class SpsInfo;

class AvcConfig {
  public:
	AvcConfig() = default;
	AvcConfig(const Atom *stsd);
	~AvcConfig();
	bool is_ok = false;
	std::unique_ptr<SpsInfo> sps_info_;

  private:
	bool decode(const uchar *start);
};
