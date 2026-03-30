
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
}

static_assert(LIBAVCODEC_VERSION_MAJOR == 62 && LIBAVCODEC_VERSION_MINOR >= 28,
              "ff_internal.h requires FFmpeg 8.1+ (libavcodec 62.28+) - "
              "update the FFCodec struct definition for other versions.");

struct FFCodecDefault;

// https://github.com/FFmpeg/FFmpeg/blob/release/8.1/libavcodec/codec_internal.h

typedef struct FFCodec {
	AVCodec p;
	unsigned caps_internal : 24;
	unsigned is_decoder : 1;
	unsigned color_ranges : 2;
	unsigned alpha_modes : 2;
	unsigned cb_type : 3;
	int priv_data_size;
	int (*update_thread_context)(struct AVCodecContext *dst, const struct AVCodecContext *src);
	int (*update_thread_context_for_user)(struct AVCodecContext *dst, const struct AVCodecContext *src);
	const FFCodecDefault *defaults;
	int (*init)(struct AVCodecContext *);

	union {
		int (*decode)(struct AVCodecContext *avctx, struct AVFrame *frame, int *got_frame_ptr, struct AVPacket *avpkt);
		int (*decode_sub)(struct AVCodecContext *avctx, struct AVSubtitle *sub, int *got_frame_ptr,
		                  const struct AVPacket *avpkt);
		int (*receive_frame)(struct AVCodecContext *avctx, struct AVFrame *frame);
		int (*encode)(struct AVCodecContext *avctx, struct AVPacket *avpkt, const struct AVFrame *frame,
		              int *got_packet_ptr);
		int (*encode_sub)(struct AVCodecContext *avctx, uint8_t *buf, int buf_size, const struct AVSubtitle *sub);
		int (*receive_packet)(struct AVCodecContext *avctx, struct AVPacket *avpkt);
	} cb;

	// ..

} FFCodec;

// https://github.com/FFmpeg/FFmpeg/blob/release/8.1/libavcodec/codec_internal.h#L112
static const unsigned FF_CODEC_CB_TYPE_RECEIVE_FRAME = 2;

static av_always_inline const FFCodec *ffcodec(const AVCodec *codec) {
	return (const FFCodec *)codec;
}
