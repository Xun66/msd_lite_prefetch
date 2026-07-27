#ifndef __MPEGTS_GOP_H__
#define __MPEGTS_GOP_H__

#include <stddef.h>
#include <stdint.h>

#define MPEGTS_PID_NONE 0x1fff

typedef enum mpegts_video_codec_e {
	MPEGTS_VIDEO_UNKNOWN = 0,
	MPEGTS_VIDEO_H264,
	MPEGTS_VIDEO_H265
} mpegts_video_codec_t;

typedef struct mpegts_gop_parser_s {
	uint16_t pmt_pid;
	uint16_t video_pid;
	mpegts_video_codec_t codec;
	uint32_t nal_shift;
	uint8_t have_pat;
	uint8_t have_pmt;
	uint8_t psi[188 * 2];
	size_t psi_size;
} mpegts_gop_parser_t;

typedef struct mpegts_gop_event_s {
	uint8_t pat;
	uint8_t pmt;
	uint8_t random_access;
	uint8_t video_unit_start;
	uint8_t codec_config;
	size_t video_unit_offset;
} mpegts_gop_event_t;

void mpegts_gop_parser_init(mpegts_gop_parser_t *parser);
void mpegts_gop_parse(mpegts_gop_parser_t *parser, const uint8_t *data,
    size_t data_size, mpegts_gop_event_t *event);

#endif
