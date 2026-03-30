#include "test_harness.h"
#include "codec/avc1/avc-config.h"
#include "atom/atom.h"

#include <iostream>
#include <vector>

// Build a minimal stsd atom containing an avcC box with the given
// lengthSizeMinusOne value (0-3). Returns the Atom by value.
static Atom makeStsdWithAvcC(int length_size_minus_one) {
	// avcC payload: configVersion(1) + profile(0x42) + compat(0xC0) + level(0x28)
	//             + 0b111111xx (reserved | lengthSizeMinusOne)
	//             + 0b111xxxxx (reserved | numSPS=1)
	//             + SPS length (25 bytes) + minimal valid SPS
	uint8_t byte4 = 0xFC | (length_size_minus_one & 0x03); // 0b111111xx

	// Minimal SPS: nal header(0x67) + profile(66) + compat(0xC0) + level(40)
	// + seq_parameter_set_id(0) + log2_max_frame_num(0) + pic_order_cnt_type(0)
	// + log2_max_pic_order_cnt_lsb(0) + max_num_ref_frames(0) + gaps + dimensions
	std::vector<uint8_t> sps = {
	    0x67, 0x42, 0xC0, 0x28, 0xDA, 0x01, 0xE0, 0x08,
	    0x9F, 0x97, 0x01, 0x10, 0x00, 0x00, 0x03, 0x00,
	    0x10, 0x00, 0x00, 0x03, 0x03, 0x20, 0xF1, 0x62, 0xE4
	};

	// Build avcC box content
	std::vector<uint8_t> avcc;
	avcc.push_back(0x01);                            // configVersion
	avcc.push_back(0x42);                            // AVCProfileIndication
	avcc.push_back(0xC0);                            // profile_compatibility
	avcc.push_back(0x28);                            // AVCLevelIndication
	avcc.push_back(byte4);                           // reserved + lengthSizeMinusOne
	avcc.push_back(0xE1);                            // reserved + numSPS=1
	avcc.push_back((sps.size() >> 8) & 0xFF);        // SPS length high
	avcc.push_back(sps.size() & 0xFF);               // SPS length low
	avcc.insert(avcc.end(), sps.begin(), sps.end()); // SPS data

	// Build stsd content: 12 bytes prefix + sample entry header + "avcC" + avcC payload
	// AvcConfig constructor starts scanning at content_.data() + 12
	std::vector<uint8_t> content;
	content.resize(12, 0); // stsd prefix (version + flags + entry count)

	// Minimal sample entry before avcC (78 bytes for video sample entry)
	std::vector<uint8_t> sample_entry(78, 0);
	content.insert(content.end(), sample_entry.begin(), sample_entry.end());

	// "avcC" signature
	uint32_t box_size = 8 + avcc.size();
	content.push_back((box_size >> 24) & 0xFF);
	content.push_back((box_size >> 16) & 0xFF);
	content.push_back((box_size >> 8) & 0xFF);
	content.push_back(box_size & 0xFF);
	content.push_back('a');
	content.push_back('v');
	content.push_back('c');
	content.push_back('C');
	content.insert(content.end(), avcc.begin(), avcc.end());

	Atom stsd;
	stsd.name_ = "stsd";
	stsd.content_.assign(content.begin(), content.end());
	stsd.length_ = 8 + content.size();
	return stsd;
}

void test_avc_config() {
	std::cout << "test_avc_config:\n";

	// nal_length_size = 4 (lengthSizeMinusOne = 3, the common case)
	{
		Atom stsd = makeStsdWithAvcC(3);
		AvcConfig cfg(&stsd);
		CHECK_EQ(cfg.nal_length_size, 4);
	}

	// nal_length_size = 2 (lengthSizeMinusOne = 1)
	{
		Atom stsd = makeStsdWithAvcC(1);
		AvcConfig cfg(&stsd);
		CHECK_EQ(cfg.nal_length_size, 2);
	}

	// nal_length_size = 1 (lengthSizeMinusOne = 0)
	{
		Atom stsd = makeStsdWithAvcC(0);
		AvcConfig cfg(&stsd);
		CHECK_EQ(cfg.nal_length_size, 1);
	}
}
