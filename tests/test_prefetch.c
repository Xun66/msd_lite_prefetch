#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "channel_predict.h"
#include "mpegts_gop.h"

static void
make_psi(uint8_t pkt[188], uint16_t pid, const uint8_t *sec, size_t len) {
	memset(pkt, 0xff, 188);
	pkt[0] = 0x47;
	pkt[1] = 0x40 | ((pid >> 8) & 0x1f);
	pkt[2] = pid & 0xff;
	pkt[3] = 0x10;
	pkt[4] = 0;
	memcpy(pkt + 5, sec, len);
}

static void
test_parser(uint8_t stream_type, uint8_t nal_header,
    mpegts_video_codec_t codec) {
	uint8_t pkt[188], sec[32];
	mpegts_gop_parser_t parser;
	mpegts_gop_event_t ev;

	mpegts_gop_parser_init(&parser);
	/* PAT: program 1 -> PMT PID 100. */
	memset(sec, 0, sizeof(sec));
	sec[0] = 0x00; sec[1] = 0xb0; sec[2] = 13;
	sec[6] = 0xc1; sec[8] = 0; sec[9] = 1;
	sec[10] = 0xe0; sec[11] = 100;
	make_psi(pkt, 0, sec, 16);
	mpegts_gop_parse(&parser, pkt, sizeof(pkt), &ev);
	assert(ev.pat && parser.pmt_pid == 100);

	/* PMT: one H.264/H.265 video ES on PID 256. */
	memset(sec, 0, sizeof(sec));
	sec[0] = 0x02; sec[1] = 0xb0; sec[2] = 18;
	sec[3] = 0; sec[4] = 1; sec[5] = 0xc1;
	sec[8] = 0xe1; sec[9] = 0x00;
	sec[10] = 0xf0; sec[11] = 0;
	sec[12] = stream_type; sec[13] = 0xe1; sec[14] = 0;
	sec[15] = 0xf0; sec[16] = 0;
	make_psi(pkt, 100, sec, 21);
	mpegts_gop_parse(&parser, pkt, sizeof(pkt), &ev);
	assert(ev.pmt && parser.video_pid == 256 && parser.codec == codec);

	memset(pkt, 0xff, sizeof(pkt));
	pkt[0] = 0x47; pkt[1] = 0x41; pkt[2] = 0; pkt[3] = 0x10;
	pkt[4] = 0; pkt[5] = 0; pkt[6] = 1; pkt[7] = nal_header;
	mpegts_gop_parse(&parser, pkt, sizeof(pkt), &ev);
	assert(ev.random_access);
}

static void
test_predictor(void) {
	ch_predict_settings_t settings = {
		.max_channels = 16,
		.max_edges = 32,
		.entry_ttl = 3600,
		.max_entry_ttl = 86400,
		.protected_per_channel = 4,
		.observation_cap = 2,
		.min_observations = 2
	};
	ch_predict_p predictor;
	ch_predict_target_t next[2], prev;
	struct sockaddr_storage viewer, source;
	struct sockaddr_in *v4;
	size_t next_count;
	int have_prev;
	const char *seq[] = {"A", "B", "A", "B", "A"};
	char state_path[128];
	size_t i;

	memset(&viewer, 0, sizeof(viewer));
	v4 = (struct sockaddr_in *)&viewer;
	v4->sin_family = AF_INET;
	inet_pton(AF_INET, "192.0.2.10", &v4->sin_addr);
	memset(&source, 0, sizeof(source));
	v4 = (struct sockaddr_in *)&source;
	v4->sin_family = AF_INET;
	inet_pton(AF_INET, "239.1.1.1", &v4->sin_addr);
	assert(0 == ch_predict_create(&settings, &predictor));
	for (i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
		assert(0 == ch_predict_observe(predictor, &viewer,
		    (const uint8_t *)seq[i], strlen(seq[i]), &source,
		    2, 0, next, &next_count, &prev, &have_prev));
	}
	assert(1 == next_count);
	assert(next[0].name_size == 1 && next[0].name[0] == 'B');
	assert(have_prev && prev.name[0] == 'B');
	assert(next[0].observations == 2);
	snprintf(state_path, sizeof(state_path), "/tmp/msd-predict-%ld.state",
	    (long)getpid());
	assert(0 == ch_predict_save(predictor, state_path));
	ch_predict_destroy(predictor);
	assert(0 == ch_predict_create(&settings, &predictor));
	assert(0 == ch_predict_load(predictor, state_path));
	assert(0 == ch_predict_observe(predictor, &viewer,
	    (const uint8_t *)"A", 1, &source, 2, 0,
	    next, &next_count, &prev, &have_prev));
	assert(1 == next_count && next[0].name[0] == 'B');
	/* Viewer position is not persisted, so no fake cross-restart edge. */
	assert(have_prev && prev.name[0] == 'B');
	ch_predict_destroy(predictor);
	unlink(state_path);
}

static int
inspect_file(const char *path) {
	FILE *fp;
	uint8_t buf[188 * 7];
	size_t size, random_access = 0, pat = 0, pmt = 0;
	mpegts_gop_parser_t parser;
	mpegts_gop_event_t ev;

	fp = fopen(path, "rb");
	if (NULL == fp)
		return (1);
	mpegts_gop_parser_init(&parser);
	while (0 != (size = fread(buf, 1, sizeof(buf), fp))) {
		mpegts_gop_parse(&parser, buf, size - (size % 188), &ev);
		pat += ev.pat;
		pmt += ev.pmt;
		random_access += ev.random_access;
	}
	fclose(fp);
	printf("codec=%u video_pid=%u pat=%zu pmt=%zu random_access=%zu\n",
	    (unsigned)parser.codec, (unsigned)parser.video_pid,
	    pat, pmt, random_access);
	return (0 == pat || 0 == pmt || 0 == random_access);
}

int
main(int argc, char **argv) {
	if (2 == argc)
		return (inspect_file(argv[1]));
	test_parser(0x1b, 0x65, MPEGTS_VIDEO_H264);
	test_parser(0x24, (19 << 1), MPEGTS_VIDEO_H265);
	test_predictor();
	puts("prefetch tests passed");
	return (0);
}
