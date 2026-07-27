#ifndef __CHANNEL_PREDICT_H__
#define __CHANNEL_PREDICT_H__

#include <sys/socket.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define CH_PREDICT_NAME_MAX 511

typedef struct ch_predict_settings_s {
	size_t max_channels;
	size_t max_edges;
	uint64_t entry_ttl;
	uint64_t max_entry_ttl;
	size_t protected_per_channel;
	uint64_t observation_cap;
	uint32_t min_observations;
} ch_predict_settings_t;

typedef struct ch_predict_target_s {
	uint8_t name[CH_PREDICT_NAME_MAX + 1];
	size_t name_size;
	struct sockaddr_storage addr;
	uint32_t if_index;
	uint32_t rejoin_time;
	uint64_t observations;
	time_t last_seen;
} ch_predict_target_t;

typedef struct ch_predict_s *ch_predict_p;

int ch_predict_create(const ch_predict_settings_t *settings,
    ch_predict_p *predict_ret);
void ch_predict_destroy(ch_predict_p predict);
int ch_predict_load(ch_predict_p predict, const char *path);
int ch_predict_save(ch_predict_p predict, const char *path);

/*
 * Records a tune for one viewer (address comparison ignores the TCP port).
 * next[0..1] are the most likely destinations from current.  prev is the
 * most likely source leading into current.
 */
int ch_predict_observe(ch_predict_p predict,
    const struct sockaddr_storage *viewer,
    const uint8_t *name, size_t name_size,
    const struct sockaddr_storage *source_addr,
    uint32_t if_index, uint32_t rejoin_time,
    ch_predict_target_t next[2], size_t *next_count,
    ch_predict_target_t *prev, int *have_prev);

#endif
