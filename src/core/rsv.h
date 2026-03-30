#pragma once

#include <string>
#include "util/common.h"
#include "io/file.h"

bool isPointingAtRtmdHeader(FileRead &file);
bool isRtmdHeader(const uchar *buff);

class Mp4;

class RsvRepairer {
  public:
	explicit RsvRepairer(Mp4 &mp4);
	void repair(const std::string &filename);

  private:
	Mp4 &mp4_;

	void detectStructure(FileRead &file_read, off_t file_size, bool is_hevc, int &rtmd_packet_size,
	                     int &frames_per_gop);
	void processGops(FileRead &file_read, off_t file_size, bool is_hevc, int video_track_idx,
	                 const std::vector<int> &audio_track_indices, int rtmd_track_idx, int rtmd_packet_size,
	                 int frames_per_gop, int audio_sample_size, int total_audio_chunk_size);
};
