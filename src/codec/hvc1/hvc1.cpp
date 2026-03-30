#include "hvc1.h"

#include <functional>
#include <iostream>

#include "codec/codec.h"
#include "hvc-config.h"
#include "track/sample_stats.h"

#include "nal-slice.h"
#include "nal.h"

using namespace std;

Hvc1LenResult hvc1GetLengths(const uchar *start, uint maxlength, SampleSizeStats *ss_stats, bool &was_keyframe,
                             std::function<const uchar *(off_t)> load_after, int nal_length_size) {
	Hvc1LenResult r;
	int &length = r.length;
	const uchar *pos = start;

	bool seen_slice = false;
	H265NalInfo previous_nal;
	was_keyframe = false;

	while (1) {
		logg(V, "---\n");
		logg(V, "pos offset: ", length, "\n");
		H265NalInfo nal_info(pos, maxlength, nal_length_size);
		if (!nal_info.is_ok) {
			logg(V, "failed parsing h256 nal-header\n");
			return r;
		}

		if (h265IsKeyframe(nal_info.nal_type_)) was_keyframe = true;
		if (h265IsSlice(nal_info.nal_type_)) {
			H265SliceInfo slice_info(nal_info);
			if (previous_nal.is_ok) {
				if (seen_slice && slice_info.isInNewFrame()) return r;
				if (previous_nal.nuh_layer_id_ != nal_info.nuh_layer_id_) {
					logg(W, "Different nuh_layer_id_ idc\n");
					return r;
				}
				if (previous_nal.nuh_temporal_id_plus1 != nal_info.nuh_temporal_id_plus1) {
					logg(W, "Different nuh_temporal_id_plus1 idc\n");
					return r;
				}
			}
			seen_slice = true;
		} else
			switch (nal_info.nal_type_) {
			case NAL_AUD: // Access unit delimiter
				if (!previous_nal.is_ok) break;
				return r;
			case NAL_FILLER_DATA:
				if (g_options.log_mode >= V) {
					logg(V, "found filler data: ");
					printBuffer(pos, 30);
				}
				break;
			default:
				vector<int> dont_warn = {20, 32, 33, 34, 39};
				if (!contains(dont_warn, nal_info.nal_type_))
					logg(W2, "unhandled nal_type: ", nal_info.nal_type_, "\n");
				if (nal_info.is_forbidden_set_) {
					logg(W2, "got forbidden bit.. ", nal_info.nal_type_, ")\n");
					return r;
				}
				// VPS/SPS/PPS always start a new access unit if a slice was already seen
				if (seen_slice && (nal_info.nal_type_ == 32 || nal_info.nal_type_ == 33 || nal_info.nal_type_ == 34))
					return r;
				break;
			}

		if (ss_stats->wouldExceed("hvc1", length, nal_info.length_, g_options.allow_large_sample ? 1 : was_keyframe)) {
			return r;
		}
		if (ss_stats->isBigEnough(length, was_keyframe)) {
			r.alternative_lengths.push_back(length);
		}

		pos += nal_info.length_;
		length += nal_info.length_;
		maxlength -= nal_info.length_;
		if (maxlength == 0) // we made it
			return r;

		pos = load_after(length);
		previous_nal = std::move(nal_info);
		logg(V, "Partial hvc1-length: ", length, "\n");
	}
	return r;
}

int getSizeHvc1(Codec *self, const uchar *start, uint maxlength) {
	int nal_length_size = self->hvc_config_ ? self->hvc_config_->nal_length_size : 4;
	auto r = hvc1GetLengths(
	    start, maxlength, self->ss_stats_, self->was_keyframe_, [self](off_t len) { return self->loadAfter(len); },
	    nal_length_size);
	if (r.alternative_lengths.size()) {
		auto &lens = r.alternative_lengths;
		lens.push_back(r.length);
		return self->find_size_fn_(self->cur_off_, lens);
	}
	return r.length;
}
