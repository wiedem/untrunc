#include "test_harness.h"
#include "codec/hvc1/hvc-config.h"
#include <cstdint>
#include <vector>

// Builds a minimal 22-byte hvcC payload (HEVCDecoderConfigurationRecord).
// Byte layout used by HvcConfig::decode():
//   byte  0:    configurationVersion = 1
//   byte  1:    general_profile_space(2) | general_tier_flag(1) | general_profile_idc(5)
//   bytes 2-11: general_profile_compatibility_flags + general_constraint_indicator_flags (ignored)
//   byte 12:    general_level_idc
//   bytes 13-20: other fields (ignored by HvcConfig)
//   byte 21:    constantFrameRate(2) | numTemporalLayers(3) | temporalIdNested(1) | lengthSizeMinusOne(2)
static std::vector<uint8_t> make_hvcc(int profile_idc, int tier_flag, int level_idc,
                                      int length_size_minus_one) {
	std::vector<uint8_t> p(22, 0);
	p[0] = 1; // configurationVersion
	p[1] = (uint8_t)((tier_flag << 5) | (profile_idc & 0x1F));
	p[12] = (uint8_t)level_idc;
	p[21] = (uint8_t)(length_size_minus_one & 0x03);
	return p;
}

void test_hvc_config() {
	std::cout << "test_hvc_config:\n";

	// Main profile (1), Main tier (0), L4.1 (123), nal_length_size=4 (lengthSizeMinusOne=3).
	{
		auto p = make_hvcc(1, 0, 123, 3);
		auto cfg = HvcConfig::fromHvcCPayload(p.data(), (int)p.size());
		CHECK(cfg.has_value());
		CHECK_EQ(cfg->profile_idc, 1);
		CHECK_EQ(cfg->tier_flag, 0);
		CHECK_EQ(cfg->level_idc, 123);
		CHECK_EQ(cfg->nal_length_size, 4);
	}

	// Main10 profile (2), High tier (1), L5.0 (150), nal_length_size=2 (lengthSizeMinusOne=1).
	{
		auto p = make_hvcc(2, 1, 150, 1);
		auto cfg = HvcConfig::fromHvcCPayload(p.data(), (int)p.size());
		CHECK(cfg.has_value());
		CHECK_EQ(cfg->profile_idc, 2);
		CHECK_EQ(cfg->tier_flag, 1);
		CHECK_EQ(cfg->level_idc, 150);
		CHECK_EQ(cfg->nal_length_size, 2);
	}

	// Too-short payload: returns nullopt.
	{
		std::vector<uint8_t> short_p = {0x01, 0x01};
		auto cfg = HvcConfig::fromHvcCPayload(short_p.data(), (int)short_p.size());
		CHECK(!cfg.has_value());
	}

	// Wrong configurationVersion (0): returns nullopt.
	{
		auto p = make_hvcc(1, 0, 123, 3);
		p[0] = 0; // invalid version
		auto cfg = HvcConfig::fromHvcCPayload(p.data(), (int)p.size());
		CHECK(!cfg.has_value());
	}
}
