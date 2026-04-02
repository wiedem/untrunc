#include "test_harness.h"
#include "core/mp4_repairer.h"
#include <cstdint>
#include <vector>

// collectLikelySizes: pure static function that filters sample sizes by
// relative frequency. It is the core heuristic for AAC frame size fallback.

// Builds a 4-byte big-endian length prefix followed by a 1-byte H.264 NAL header and filler.
static void push_avcc_nal(std::vector<uint8_t> &v, uint8_t nal_header, int payload_bytes) {
	uint32_t len = 1 + payload_bytes; // header + payload
	v.push_back((len >> 24) & 0xFF);
	v.push_back((len >> 16) & 0xFF);
	v.push_back((len >> 8) & 0xFF);
	v.push_back(len & 0xFF);
	v.push_back(nal_header);
	for (int i = 0; i < payload_bytes; i++)
		v.push_back(0x00);
}

// Builds a 4-byte big-endian length prefix followed by a 2-byte H.265 NAL header and filler.
// H.265 header: byte0 = nal_type << 1 (forbidden_zero_bit=0), byte1 = 0x01 (layer_id=0,
// temporal_id_plus1=1), required for all non-EOB types by H265NalInfo validation.
static void push_hvcc_nal(std::vector<uint8_t> &v, uint8_t nal_type, int payload_bytes) {
	uint32_t len = 2 + payload_bytes; // 2-byte header + payload
	v.push_back((len >> 24) & 0xFF);
	v.push_back((len >> 16) & 0xFF);
	v.push_back((len >> 8) & 0xFF);
	v.push_back(len & 0xFF);
	v.push_back(nal_type << 1); // forbidden_zero_bit=0, nal_type in bits 6:1
	v.push_back(0x01);          // nuh_layer_id=0, nuh_temporal_id_plus1=1
	for (int i = 0; i < payload_bytes; i++)
		v.push_back(0x00);
}

void test_repairer() {
	std::cout << "test_repairer:\n";

	// Empty input produces an empty result.
	{
		std::vector<int> result = Mp4Repairer::collectLikelySizes({});
		CHECK(result.empty());
	}

	// Single repeated value: appears 100% of the time, always included.
	{
		std::vector<int> result = Mp4Repairer::collectLikelySizes({256, 256, 256, 256});
		CHECK_EQ((int)result.size(), 1);
		CHECK_EQ(result[0], 256);
	}

	// Two values at 50% each: both exceed the 1% threshold.
	{
		std::vector<int> result = Mp4Repairer::collectLikelySizes({256, 512, 256, 512});
		CHECK_EQ((int)result.size(), 2);
		CHECK_EQ(result[0], 256); // result is sorted ascending
		CHECK_EQ(result[1], 512);
	}

	// Rare value: appears once out of 101 entries (~0.99%) -> excluded (below 1%).
	// The dominant value appears 100/101 times and must be included.
	{
		std::vector<int> sizes(100, 256);
		sizes.push_back(999); // 1/101 ≈ 0.0099 < 0.01
		auto result = Mp4Repairer::collectLikelySizes(sizes);
		CHECK_EQ((int)result.size(), 1);
		CHECK_EQ(result[0], 256);
	}

	// Exactly at the threshold: 1/100 = 0.01 >= 0.01 -> included.
	{
		std::vector<int> sizes(99, 256);
		sizes.push_back(512); // 1/100 = 0.01
		auto result = Mp4Repairer::collectLikelySizes(sizes);
		CHECK_EQ((int)result.size(), 2);
		CHECK_EQ(result[0], 256);
		CHECK_EQ(result[1], 512);
	}

	// Custom threshold: with min_freq=0.5, only values appearing >= 50% are kept.
	{
		// 256: 3/5=60%, 512: 1/5=20%, 768: 1/5=20%
		std::vector<int> sizes = {256, 256, 256, 512, 768};
		auto result = Mp4Repairer::collectLikelySizes(sizes, 0.5);
		CHECK_EQ((int)result.size(), 1);
		CHECK_EQ(result[0], 256);
	}

	// Result is sorted ascending regardless of insertion order.
	{
		std::vector<int> sizes = {768, 256, 512, 768, 256, 512};
		auto result = Mp4Repairer::collectLikelySizes(sizes);
		CHECK_EQ((int)result.size(), 3);
		CHECK_EQ(result[0], 256);
		CHECK_EQ(result[1], 512);
		CHECK_EQ(result[2], 768);
	}

	// findIdrInAvcc: SEI followed by IDR -- finds IDR at the correct offset and length.
	{
		// NAL headers: 0x06 = SEI (ref_idc=0, type=6), 0x65 = IDR (ref_idc=3, type=5).
		// nal_length_size = 4; each call to push_avcc_nal writes 4 + 1 + payload bytes.
		std::vector<uint8_t> stream;
		push_avcc_nal(stream, 0x06, 7); // SEI: total = 4 + 8 = 12 bytes
		int idr_offset = (int)stream.size();
		push_avcc_nal(stream, 0x65, 9); // IDR: total = 4 + 10 = 14 bytes

		auto result = Mp4Repairer::findIdrInAvcc(stream.data(), (int)stream.size(), 4);
		CHECK(result.has_value());
		CHECK_EQ(result->first, idr_offset);
		CHECK_EQ(result->second, 4 + 10); // length prefix + NAL (header + payload)
	}

	// findIdrInAvcc: stream with only a SEI -- returns nullopt.
	{
		std::vector<uint8_t> stream;
		push_avcc_nal(stream, 0x06, 7); // SEI only
		auto result = Mp4Repairer::findIdrInAvcc(stream.data(), (int)stream.size(), 4);
		CHECK(!result.has_value());
	}

	// findIdrInAvcc: IDR is the very first NAL -- offset is 0.
	{
		std::vector<uint8_t> stream;
		push_avcc_nal(stream, 0x65, 9); // IDR first
		auto result = Mp4Repairer::findIdrInAvcc(stream.data(), (int)stream.size(), 4);
		CHECK(result.has_value());
		CHECK_EQ(result->first, 0);
		CHECK_EQ(result->second, 4 + 10);
	}

	// findIdrInAvcc: non-AVCC bytes (simulating audio data) before IDR are skipped.
	{
		std::vector<uint8_t> stream;
		for (int i = 0; i < 8; i++)
			stream.push_back(0xFF); // non-zero first byte: rejected by NalInfo, skipped
		int idr_offset = (int)stream.size();
		push_avcc_nal(stream, 0x65, 9); // IDR after the audio-like data
		auto result = Mp4Repairer::findIdrInAvcc(stream.data(), (int)stream.size(), 4);
		CHECK(result.has_value());
		CHECK_EQ(result->first, idr_offset);
		CHECK_EQ(result->second, 4 + 10);
	}

	// findIdrInHvcc: SEI (type 39) followed by IDR_W_RADL (type 19) -- finds IDR at correct offset.
	{
		// push_hvcc_nal writes 4 (length) + 2 (header) + payload bytes.
		std::vector<uint8_t> stream;
		push_hvcc_nal(stream, 39, 5); // SEI_PREFIX: 4 + 2 + 5 = 11 bytes total
		int idr_offset = (int)stream.size();
		push_hvcc_nal(stream, 19, 9); // IDR_W_RADL: 4 + 2 + 9 = 15 bytes total
		auto result = Mp4Repairer::findIdrInHvcc(stream.data(), (int)stream.size(), 4);
		CHECK(result.has_value());
		CHECK_EQ(result->first, idr_offset);
		CHECK_EQ(result->second, 4 + 2 + 9); // length prefix + 2-byte header + payload
	}

	// findIdrInHvcc: stream with only a SEI -- returns nullopt.
	{
		std::vector<uint8_t> stream;
		push_hvcc_nal(stream, 39, 5); // SEI_PREFIX only
		auto result = Mp4Repairer::findIdrInHvcc(stream.data(), (int)stream.size(), 4);
		CHECK(!result.has_value());
	}

	// findIdrInHvcc: IDR_N_LP (type 20) first -- offset is 0.
	{
		std::vector<uint8_t> stream;
		push_hvcc_nal(stream, 20, 9); // IDR_N_LP first
		auto result = Mp4Repairer::findIdrInHvcc(stream.data(), (int)stream.size(), 4);
		CHECK(result.has_value());
		CHECK_EQ(result->first, 0);
		CHECK_EQ(result->second, 4 + 2 + 9);
	}

	// findIdrInHvcc: non-HVCC bytes (simulating audio data) before IDR are skipped.
	{
		std::vector<uint8_t> stream;
		for (int i = 0; i < 8; i++)
			stream.push_back(0xFF); // non-zero first byte: rejected by H265NalInfo, skipped
		int idr_offset = (int)stream.size();
		push_hvcc_nal(stream, 19, 9); // IDR_W_RADL after the audio-like data
		auto result = Mp4Repairer::findIdrInHvcc(stream.data(), (int)stream.size(), 4);
		CHECK(result.has_value());
		CHECK_EQ(result->first, idr_offset);
		CHECK_EQ(result->second, 4 + 2 + 9);
	}

	// findSpsInHvcc: VPS (type 32) + SPS (type 33) + IDR (type 19) -- finds SPS, stops before IDR.
	// This mirrors how hev1 camera recordings prepend VPS/SPS/PPS before each IDR in the mdat.
	{
		std::vector<uint8_t> stream;
		push_hvcc_nal(stream, 32, 4); // VPS: 4 + 2 + 4 = 10 bytes
		int sps_offset = (int)stream.size();
		push_hvcc_nal(stream, 33, 6); // SPS: 4 + 2 + 6 = 12 bytes
		push_hvcc_nal(stream, 19, 9); // IDR_W_RADL: must not be reached
		auto result = Mp4Repairer::findSpsInHvcc(stream.data(), (int)stream.size(), 4);
		CHECK(result.has_value());
		CHECK_EQ(result->first, sps_offset);
		CHECK_EQ(result->second, 4 + 2 + 6);
	}

	// findSpsInHvcc: VPS followed directly by IDR (no SPS before slice) -- returns nullopt.
	// Simulates a hvc1 file or a hev1 file without in-band parameter sets.
	{
		std::vector<uint8_t> stream;
		push_hvcc_nal(stream, 32, 4); // VPS
		push_hvcc_nal(stream, 19, 9); // IDR_W_RADL: slice stops the search
		auto result = Mp4Repairer::findSpsInHvcc(stream.data(), (int)stream.size(), 4);
		CHECK(!result.has_value());
	}

	// findSpsInHvcc: SPS is the very first NAL -- found at offset 0.
	{
		std::vector<uint8_t> stream;
		push_hvcc_nal(stream, 33, 6); // SPS first
		auto result = Mp4Repairer::findSpsInHvcc(stream.data(), (int)stream.size(), 4);
		CHECK(result.has_value());
		CHECK_EQ(result->first, 0);
		CHECK_EQ(result->second, 4 + 2 + 6);
	}

	// findSpsInHvcc: non-HVCC bytes (audio data) before VPS+SPS are skipped byte-by-byte.
	{
		std::vector<uint8_t> stream;
		for (int i = 0; i < 8; i++)
			stream.push_back(0xFF);   // non-zero first byte: rejected, skipped
		push_hvcc_nal(stream, 32, 4); // VPS
		int sps_offset = (int)stream.size();
		push_hvcc_nal(stream, 33, 6); // SPS
		auto result = Mp4Repairer::findSpsInHvcc(stream.data(), (int)stream.size(), 4);
		CHECK(result.has_value());
		CHECK_EQ(result->first, sps_offset);
		CHECK_EQ(result->second, 4 + 2 + 6);
	}

	// parseSpsH265ProfileLevel: builds a minimal SPS NAL RBSP and verifies profile/level extraction.
	// RBSP layout (bytes after the 2-byte NAL header, zero-indexed):
	//   byte 0: sps_vps_id etc. (don't care)
	//   byte 1: profile_space(2)+tier(1)+profile_idc(5)  -> set profile_idc=2 (Main 10): 0x02
	//   bytes 2-5: profile_compatibility_flags (set to 0)
	//   bytes 6-11: constraint flags (set to 0)
	//   byte 12: general_level_idc -> set to 93 (L3.1)
	{
		// 2-byte NAL header + 13 RBSP bytes = 15 bytes minimum
		std::vector<uint8_t> nal = {
		    0x42, 0x01,             // NAL header: nal_unit_type=33 (SPS), nuh_layer_id=0, nuh_temporal_id_plus1=1
		    0x01,                   // RBSP[0]: sps_vps_id=0, max_sublayers=0, temporal_id_nesting=1
		    0x02,                   // RBSP[1]: profile_space=0, tier=0, profile_idc=2 (Main 10)
		    0x00, 0x00, 0x00, 0x00, // RBSP[2-5]: profile_compatibility_flags
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // RBSP[6-11]: constraint flags
		    93,                                 // RBSP[12]: general_level_idc = 93 (L3.1)
		};
		auto result = Mp4Repairer::parseSpsH265ProfileLevel(nal.data(), (int)nal.size());
		CHECK(result.has_value());
		CHECK_EQ(result->first, 2);   // profile_idc = Main 10
		CHECK_EQ(result->second, 93); // level_idc   = L3.1
	}

	// parseSpsH265ProfileLevel: emulation prevention byte before general_level_idc is stripped.
	// RBSP[2-5] = 0x00 0x00 0x00 0x00 -- two leading zeros allow an EPB at RBSP byte 4.
	// Insert 0x03 after RBSP[3]=0x00 (i.e. after two consecutive zeros in the RBSP output),
	// making the NAL bytestream: ... 0x00 0x00 [0x03] 0x00 ... -- the 0x03 is skipped.
	// Without stripping, 0x03 would be counted as RBSP byte 4, shifting level_idc to byte 13.
	{
		std::vector<uint8_t> nal = {
		    0x42, 0x01,             // NAL header
		    0x01,                   // RBSP[0]
		    0x04,                   // RBSP[1]: profile_idc=4 (Rext)
		    0x00, 0x00,             // RBSP[2-3]: first two profile_compatibility_flags bytes (zeros)
		    0x03,                   // emulation prevention byte (follows 0x00 0x00 in RBSP output)
		    0x00, 0x00, 0x00, 0x00, // RBSP[4-7] (after EPB removal): remaining compat flags
		    0x00, 0x00, 0x00, 0x00, // RBSP[8-11]: constraint flags
		    120,                    // RBSP[12]: general_level_idc = 120 (L4.0)
		};
		auto result = Mp4Repairer::parseSpsH265ProfileLevel(nal.data(), (int)nal.size());
		CHECK(result.has_value());
		CHECK_EQ(result->first, 4);    // profile_idc = Rext
		CHECK_EQ(result->second, 120); // level_idc   = L4.0
	}

	// parseSpsH265ProfileLevel: too short -- returns nullopt.
	{
		std::vector<uint8_t> nal(14, 0x00); // 14 bytes: one short of the 15-byte minimum
		auto result = Mp4Repairer::parseSpsH265ProfileLevel(nal.data(), (int)nal.size());
		CHECK(!result.has_value());
	}
}
