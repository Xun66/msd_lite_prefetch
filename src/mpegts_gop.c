#include <string.h>

#include "mpegts_gop.h"

#define TS_SIZE 188
#define TS_SYNC 0x47

static const uint8_t *
ts_payload(const uint8_t *pkt, size_t *size, int *pusi) {
	size_t off = 4;
	uint8_t afc;

	if (pkt[0] != TS_SYNC || (pkt[1] & 0x80))
		return (NULL);
	*pusi = !!(pkt[1] & 0x40);
	afc = (pkt[3] >> 4) & 3;
	if (0 == afc || 2 == afc)
		return (NULL);
	if (3 == afc) {
		off += 1 + pkt[4];
		if (off >= TS_SIZE)
			return (NULL);
	}
	*size = TS_SIZE - off;
	return (pkt + off);
}

static int
psi_section(const uint8_t *payload, size_t payload_size, int pusi,
    const uint8_t **section, size_t *section_size) {
	size_t off, len;

	if (!pusi || 0 == payload_size)
		return (0);
	off = 1 + payload[0];
	if ((off + 3) > payload_size)
		return (0);
	len = 3 + (((size_t)payload[off + 1] & 0x0f) << 8) + payload[off + 2];
	if ((off + len) > payload_size)
		return (0); /* The tiny parser waits for a non-split PAT/PMT. */
	*section = payload + off;
	*section_size = len;
	return (1);
}

static void
parse_pat(mpegts_gop_parser_t *p, const uint8_t *sec, size_t len) {
	size_t pos, end;
	uint16_t program;

	if (len < 12 || 0x00 != sec[0])
		return;
	end = len - 4;
	for (pos = 8; (pos + 4) <= end; pos += 4) {
		program = ((uint16_t)sec[pos] << 8) | sec[pos + 1];
		if (0 == program)
			continue;
		p->pmt_pid = ((uint16_t)(sec[pos + 2] & 0x1f) << 8) | sec[pos + 3];
		p->have_pat = 1;
		return;
	}
}

static void
parse_pmt(mpegts_gop_parser_t *p, const uint8_t *sec, size_t len) {
	size_t pos, end, es_len;
	uint8_t stream_type;
	uint16_t pid;

	if (len < 16 || 0x02 != sec[0])
		return;
	pos = 12 + ((((size_t)sec[10]) & 0x0f) << 8) + sec[11];
	end = len - 4;
	while ((pos + 5) <= end) {
		stream_type = sec[pos];
		pid = ((uint16_t)(sec[pos + 1] & 0x1f) << 8) | sec[pos + 2];
		es_len = (((size_t)sec[pos + 3] & 0x0f) << 8) | sec[pos + 4];
		if (0x1b == stream_type || 0x24 == stream_type) {
			p->video_pid = pid;
			p->codec = (0x1b == stream_type) ?
			    MPEGTS_VIDEO_H264 : MPEGTS_VIDEO_H265;
			p->have_pmt = 1;
			return;
		}
		pos += 5 + es_len;
	}
}

static unsigned
scan_nals(mpegts_gop_parser_t *p, const uint8_t *data, size_t len) {
	size_t i;
	uint8_t nal_type;
	unsigned events = 0;

	for (i = 0; i < len; i++) {
		p->nal_shift = (p->nal_shift << 8) | data[i];
		if ((p->nal_shift & 0x00ffffffU) != 0x000001U || (i + 1) >= len)
			continue;
		if (MPEGTS_VIDEO_H264 == p->codec) {
			nal_type = data[i + 1] & 0x1f;
			if (5 == nal_type)
				events |= 1;
			else if (7 == nal_type || 8 == nal_type)
				events |= 2;
		} else if (MPEGTS_VIDEO_H265 == p->codec) {
			nal_type = (data[i + 1] >> 1) & 0x3f;
			/* BLA, IDR and CRA are all valid HEVC random access points. */
			if (nal_type >= 16 && nal_type <= 21)
				events |= 1;
			else if (nal_type >= 32 && nal_type <= 34)
				events |= 2;
		}
	}
	return (events);
}

void
mpegts_gop_parser_init(mpegts_gop_parser_t *p) {
	memset(p, 0, sizeof(*p));
	p->pmt_pid = MPEGTS_PID_NONE;
	p->video_pid = MPEGTS_PID_NONE;
}

void
mpegts_gop_parse(mpegts_gop_parser_t *p, const uint8_t *data,
    size_t data_size, mpegts_gop_event_t *ev) {
	size_t off, payload_size, section_size;
	const uint8_t *pkt, *payload, *section;
	uint16_t pid;
	int pusi;

	memset(ev, 0, sizeof(*ev));
	for (off = 0; (off + TS_SIZE) <= data_size; off += TS_SIZE) {
		pkt = data + off;
		payload = ts_payload(pkt, &payload_size, &pusi);
		if (NULL == payload)
			continue;
		pid = ((uint16_t)(pkt[1] & 0x1f) << 8) | pkt[2];
		if (0 == pid && psi_section(payload, payload_size, pusi,
		    &section, &section_size)) {
			parse_pat(p, section, section_size);
			memcpy(p->psi, pkt, TS_SIZE);
			p->psi_size = p->have_pmt ? (TS_SIZE * 2) : TS_SIZE;
			ev->pat = 1;
		} else if (pid == p->pmt_pid && psi_section(payload, payload_size,
		    pusi, &section, &section_size)) {
			parse_pmt(p, section, section_size);
			memcpy(p->psi + TS_SIZE, pkt, TS_SIZE);
			p->psi_size = TS_SIZE * 2;
			ev->pmt = 1;
		} else if (pid == p->video_pid) {
			unsigned nal_events;
			if (pusi) {
				ev->video_unit_start = 1;
				ev->video_unit_offset = off;
			}
			nal_events = scan_nals(p, payload, payload_size);
			if (nal_events & 1)
				ev->random_access = 1;
			if (nal_events & 2)
				ev->codec_config = 1;
		}
	}
}
