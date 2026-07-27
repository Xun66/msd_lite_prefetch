#include <sys/types.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "channel_predict.h"

typedef struct channel_s {
	uint8_t name[CH_PREDICT_NAME_MAX + 1];
	size_t name_size;
	struct sockaddr_storage addr;
	uint32_t if_index;
	uint32_t rejoin_time;
	uint64_t observations;
	time_t last_seen;
} channel_t;

typedef struct edge_s {
	size_t from;
	size_t to;
	uint64_t count;
	time_t last_seen;
} edge_t;

typedef struct edge_rank_s {
	size_t index;
	size_t from;
	uint64_t count;
	time_t last_seen;
} edge_rank_t;

typedef struct viewer_s {
	struct sockaddr_storage addr;
	size_t channel;
	time_t last_seen;
} viewer_t;

struct ch_predict_s {
	pthread_mutex_t lock;
	ch_predict_settings_t settings;
	channel_t *channels;
	size_t channel_count;
	edge_t *edges;
	size_t edge_count;
	viewer_t *viewers;
	size_t viewer_count;
	size_t viewer_cap;
	int dirty;
};

#define CH_PERSIST_MAGIC 0x4d534450U /* "MSDP" */
#define CH_PERSIST_VERSION 1

typedef struct persist_header_s {
	uint32_t magic;
	uint32_t version;
	uint64_t channel_count;
	uint64_t edge_count;
} persist_header_t;

static int
addr_equal(const struct sockaddr_storage *a,
    const struct sockaddr_storage *b) {
	if (a->ss_family != b->ss_family)
		return (0);
	if (AF_INET == a->ss_family)
		return (0 == memcmp(&((const struct sockaddr_in *)a)->sin_addr,
		    &((const struct sockaddr_in *)b)->sin_addr,
		    sizeof(struct in_addr)));
	if (AF_INET6 == a->ss_family)
		return (0 == memcmp(&((const struct sockaddr_in6 *)a)->sin6_addr,
		    &((const struct sockaddr_in6 *)b)->sin6_addr,
		    sizeof(struct in6_addr)));
	return (0);
}

static void
target_set(ch_predict_target_t *dst, const channel_t *src,
    uint64_t observations, time_t last_seen) {
	memset(dst, 0, sizeof(*dst));
	memcpy(dst->name, src->name, src->name_size);
	dst->name_size = src->name_size;
	memcpy(&dst->addr, &src->addr, sizeof(dst->addr));
	dst->if_index = src->if_index;
	dst->rejoin_time = src->rejoin_time;
	dst->observations = observations;
	dst->last_seen = last_seen;
}

static size_t
channel_find(struct ch_predict_s *p, const uint8_t *name, size_t name_size) {
	size_t i;
	for (i = 0; i < p->channel_count; i++) {
		if (p->channels[i].name_size == name_size &&
		    0 == memcmp(p->channels[i].name, name, name_size))
			return (i);
	}
	return (SIZE_MAX);
}

static int
edge_rank_cmp(const void *a_ptr, const void *b_ptr) {
	const edge_rank_t *a = a_ptr, *b = b_ptr;

	if (a->from != b->from)
		return (a->from < b->from ? -1 : 1);
	if (a->count != b->count)
		return (a->count > b->count ? -1 : 1);
	if (a->last_seen != b->last_seen)
		return (a->last_seen > b->last_seen ? -1 : 1);
	if (a->index == b->index)
		return (0);
	return (a->index < b->index ? -1 : 1);
}

static edge_rank_t *
edge_ranks_make(const struct ch_predict_s *p) {
	edge_rank_t *ranks;
	size_t i;

	if (0 == p->edge_count)
		return (NULL);
	ranks = malloc(p->edge_count * sizeof(*ranks));
	if (NULL == ranks)
		return (NULL);
	for (i = 0; i < p->edge_count; i++) {
		ranks[i].index = i;
		ranks[i].from = p->edges[i].from;
		ranks[i].count = p->edges[i].count;
		ranks[i].last_seen = p->edges[i].last_seen;
	}
	qsort(ranks, p->edge_count, sizeof(*ranks), edge_rank_cmp);
	return (ranks);
}

/*
 * Rank outgoing edges independently for every source channel. This keeps
 * popular channels from consuming another channel's permanent slots.
 */
static uint8_t *
edge_protection_make(const struct ch_predict_s *p) {
	edge_rank_t *ranks;
	uint8_t *protected;
	size_t i, group_pos = 0, previous_from = SIZE_MAX;

	if (0 == p->edge_count)
		return (NULL);
	protected = calloc(p->edge_count, sizeof(*protected));
	ranks = edge_ranks_make(p);
	if (NULL == protected || NULL == ranks) {
		free(ranks);
		free(protected);
		return (NULL);
	}
	for (i = 0; i < p->edge_count; i++) {
		if (ranks[i].from != previous_from) {
			previous_from = ranks[i].from;
			group_pos = 0;
		}
		if (group_pos < p->settings.protected_per_channel)
			protected[ranks[i].index] = 1;
		group_pos++;
	}
	free(ranks);
	return (protected);
}

static uint64_t
edge_effective_ttl(const struct ch_predict_s *p, uint64_t count) {
	uint64_t ttl, multiplier = 1, scaled_count = count;

	while (scaled_count >= 2) {
		multiplier++;
		scaled_count >>= 1;
	}
	if (p->settings.entry_ttl > (UINT64_MAX / multiplier))
		ttl = UINT64_MAX;
	else
		ttl = p->settings.entry_ttl * multiplier;
	if (0 != p->settings.max_entry_ttl &&
	    ttl > p->settings.max_entry_ttl)
		ttl = p->settings.max_entry_ttl;
	return (ttl);
}

static void
prune(struct ch_predict_s *p, time_t now) {
	uint8_t *protected;
	size_t i = 0, dst = 0;
	uint64_t ttl;

	if (0 == p->settings.entry_ttl)
		return;
	protected = edge_protection_make(p);
	if (NULL != protected) {
		for (i = 0; i < p->edge_count; i++) {
			ttl = edge_effective_ttl(p, p->edges[i].count);
			if (!protected[i] && now > p->edges[i].last_seen &&
			    (uint64_t)(now - p->edges[i].last_seen) > ttl) {
				p->dirty = 1;
				continue;
			}
			if (dst != i)
				p->edges[dst] = p->edges[i];
			dst++;
		}
		p->edge_count = dst;
	}
	free(protected);
	i = 0;
	while (i < p->viewer_count) {
		if (now > p->viewers[i].last_seen &&
		    (uint64_t)(now - p->viewers[i].last_seen) >
		    p->settings.entry_ttl) {
			p->viewers[i] = p->viewers[p->viewer_count - 1];
			p->viewer_count--;
		} else {
			i++;
		}
	}
}

static void
edge_record(struct ch_predict_s *p, size_t from, size_t to, time_t now) {
	uint8_t *protected;
	size_t i, victim = SIZE_MAX;
	if (from == to)
		return;
	for (i = 0; i < p->edge_count; i++) {
		if (p->edges[i].from == from && p->edges[i].to == to) {
			if (0 == p->settings.observation_cap ||
			    p->edges[i].count < p->settings.observation_cap)
				p->edges[i].count++;
			p->edges[i].last_seen = now;
			p->dirty = 1;
			return;
		}
	}
	if (p->edge_count < p->settings.max_edges) {
		i = p->edge_count++;
	} else if (p->edge_count) {
		/*
		 * Never evict a per-channel protected edge. Among all remaining
		 * entries, replace the weakest and stalest one.
		 */
		protected = edge_protection_make(p);
		if (NULL == protected)
			return;
		for (i = 0; i < p->edge_count; i++) {
			if (protected[i])
				continue;
			if (SIZE_MAX == victim ||
			    p->edges[i].count < p->edges[victim].count ||
			    (p->edges[i].count == p->edges[victim].count &&
			    p->edges[i].last_seen < p->edges[victim].last_seen))
				victim = i;
		}
		free(protected);
		if (SIZE_MAX == victim)
			return;
		i = victim;
	} else {
		return;
	}
	p->edges[i].from = from;
	p->edges[i].to = to;
	p->edges[i].count = 1;
	p->edges[i].last_seen = now;
	p->dirty = 1;
}

int
ch_predict_create(const ch_predict_settings_t *settings,
    ch_predict_p *predict_ret) {
	struct ch_predict_s *p;
	if (NULL == settings || NULL == predict_ret ||
	    0 == settings->max_channels || 0 == settings->max_edges)
		return (EINVAL);
	p = calloc(1, sizeof(*p));
	if (NULL == p)
		return (ENOMEM);
	pthread_mutex_init(&p->lock, NULL);
	p->settings = *settings;
	p->channels = calloc(settings->max_channels, sizeof(channel_t));
	p->edges = calloc(settings->max_edges, sizeof(edge_t));
	if (NULL == p->channels || NULL == p->edges) {
		ch_predict_destroy(p);
		return (ENOMEM);
	}
	*predict_ret = p;
	return (0);
}

void
ch_predict_destroy(ch_predict_p p) {
	if (NULL == p)
		return;
	pthread_mutex_destroy(&p->lock);
	free(p->viewers);
	free(p->edges);
	free(p->channels);
	free(p);
}

int
ch_predict_load(ch_predict_p p, const char *path) {
	FILE *fp;
	persist_header_t hdr;
	channel_t *channels = NULL;
	edge_t *edges = NULL;
	size_t i;
	int error = 0, clamped = 0;

	if (NULL == p || NULL == path || 0 == path[0])
		return (EINVAL);
	fp = fopen(path, "rb");
	if (NULL == fp)
		return (ENOENT == errno ? 0 : errno);
	if (1 != fread(&hdr, sizeof(hdr), 1, fp) ||
	    CH_PERSIST_MAGIC != hdr.magic || CH_PERSIST_VERSION != hdr.version ||
	    hdr.channel_count > p->settings.max_channels ||
	    hdr.edge_count > p->settings.max_edges) {
		error = EINVAL;
		goto out;
	}
	channels = calloc(p->settings.max_channels, sizeof(channel_t));
	edges = calloc(p->settings.max_edges, sizeof(edge_t));
	if (NULL == channels || NULL == edges) {
		error = ENOMEM;
		goto out;
	}
	if ((hdr.channel_count && hdr.channel_count !=
	    fread(channels, sizeof(channel_t), hdr.channel_count, fp)) ||
	    (hdr.edge_count && hdr.edge_count !=
	    fread(edges, sizeof(edge_t), hdr.edge_count, fp))) {
		error = EINVAL;
		goto out;
	}
	for (i = 0; i < hdr.channel_count; i++) {
		if (0 == channels[i].name_size ||
		    channels[i].name_size > CH_PREDICT_NAME_MAX) {
			error = EINVAL;
			goto out;
		}
		if (0 != p->settings.observation_cap &&
		    channels[i].observations > p->settings.observation_cap) {
			channels[i].observations = p->settings.observation_cap;
			clamped = 1;
		}
	}
	for (i = 0; i < hdr.edge_count; i++) {
		if (edges[i].from >= hdr.channel_count ||
		    edges[i].to >= hdr.channel_count) {
			error = EINVAL;
			goto out;
		}
		if (0 != p->settings.observation_cap &&
		    edges[i].count > p->settings.observation_cap) {
			edges[i].count = p->settings.observation_cap;
			clamped = 1;
		}
	}
	pthread_mutex_lock(&p->lock);
	free(p->channels);
	free(p->edges);
	p->channels = channels;
	p->edges = edges;
	p->channel_count = (size_t)hdr.channel_count;
	p->edge_count = (size_t)hdr.edge_count;
	p->dirty = clamped;
	pthread_mutex_unlock(&p->lock);
	channels = NULL;
	edges = NULL;
out:
	free(channels);
	free(edges);
	fclose(fp);
	return (error);
}

int
ch_predict_save(ch_predict_p p, const char *path) {
	FILE *fp;
	persist_header_t hdr;
	char *tmp_path;
	size_t path_len;
	int error = 0, fd;

	if (NULL == p || NULL == path || 0 == path[0])
		return (EINVAL);
	pthread_mutex_lock(&p->lock);
	if (!p->dirty) {
		pthread_mutex_unlock(&p->lock);
		return (0);
	}
	path_len = strlen(path);
	tmp_path = malloc(path_len + 5);
	if (NULL == tmp_path) {
		pthread_mutex_unlock(&p->lock);
		return (ENOMEM);
	}
	memcpy(tmp_path, path, path_len);
	memcpy(tmp_path + path_len, ".tmp", 5);
	fp = fopen(tmp_path, "wb");
	if (NULL == fp) {
		error = errno;
		goto out;
	}
	hdr.magic = CH_PERSIST_MAGIC;
	hdr.version = CH_PERSIST_VERSION;
	hdr.channel_count = p->channel_count;
	hdr.edge_count = p->edge_count;
	if (1 != fwrite(&hdr, sizeof(hdr), 1, fp) ||
	    (p->channel_count && p->channel_count !=
	    fwrite(p->channels, sizeof(channel_t), p->channel_count, fp)) ||
	    (p->edge_count && p->edge_count !=
	    fwrite(p->edges, sizeof(edge_t), p->edge_count, fp))) {
		error = EIO;
	}
	if (0 == error && 0 != fflush(fp))
		error = errno;
	fd = fileno(fp);
	if (0 == error && 0 != fsync(fd))
		error = errno;
	if (0 != fclose(fp) && 0 == error)
		error = errno;
	fp = NULL;
	if (0 == error && 0 != rename(tmp_path, path))
		error = errno;
	if (0 == error)
		p->dirty = 0;
out:
	if (NULL != fp)
		fclose(fp);
	if (0 != error)
		unlink(tmp_path);
	free(tmp_path);
	pthread_mutex_unlock(&p->lock);
	return (error);
}

int
ch_predict_observe(ch_predict_p p, const struct sockaddr_storage *viewer,
    const uint8_t *name, size_t name_size,
    const struct sockaddr_storage *source_addr,
    uint32_t if_index, uint32_t rejoin_time,
    ch_predict_target_t next[2], size_t *next_count,
    ch_predict_target_t *prev, int *have_prev) {
	size_t i, current, viewer_pos = SIZE_MAX, first = SIZE_MAX, second = SIZE_MAX;
	size_t reverse = SIZE_MAX;
	uint64_t first_count = 0, second_count = 0, reverse_count = 0;
	time_t first_seen = 0, second_seen = 0, reverse_seen = 0;
	time_t now = time(NULL);
	viewer_t *tmp;

	if (NULL == p || NULL == viewer || NULL == name || 0 == name_size ||
	    name_size > CH_PREDICT_NAME_MAX)
		return (EINVAL);
	*next_count = 0;
	*have_prev = 0;
	pthread_mutex_lock(&p->lock);
	prune(p, now);
	current = channel_find(p, name, name_size);
	if (SIZE_MAX == current) {
		if (p->channel_count >= p->settings.max_channels) {
			pthread_mutex_unlock(&p->lock);
			return (ENOSPC);
		}
		current = p->channel_count++;
		memcpy(p->channels[current].name, name, name_size);
		p->channels[current].name_size = name_size;
		p->dirty = 1;
	}
	memcpy(&p->channels[current].addr, source_addr, sizeof(*source_addr));
	p->channels[current].if_index = if_index;
	p->channels[current].rejoin_time = rejoin_time;
	if (0 == p->settings.observation_cap ||
	    p->channels[current].observations < p->settings.observation_cap)
		p->channels[current].observations++;
	p->channels[current].last_seen = now;

	for (i = 0; i < p->viewer_count; i++) {
		if (addr_equal(&p->viewers[i].addr, viewer)) {
			viewer_pos = i;
			break;
		}
	}
	if (SIZE_MAX != viewer_pos) {
		edge_record(p, p->viewers[viewer_pos].channel, current, now);
	} else {
		if (p->viewer_count >= p->settings.max_channels) {
			size_t victim = 0;
			for (i = 1; i < p->viewer_count; i++) {
				if (p->viewers[i].last_seen <
				    p->viewers[victim].last_seen)
					victim = i;
			}
			viewer_pos = victim;
			memcpy(&p->viewers[viewer_pos].addr, viewer,
			    sizeof(*viewer));
			goto viewer_ready;
		}
		if (p->viewer_count == p->viewer_cap) {
			size_t cap = p->viewer_cap ? p->viewer_cap * 2 : 32;
			tmp = realloc(p->viewers, cap * sizeof(viewer_t));
			if (NULL == tmp) {
				pthread_mutex_unlock(&p->lock);
				return (ENOMEM);
			}
			p->viewers = tmp;
			p->viewer_cap = cap;
		}
		viewer_pos = p->viewer_count++;
		memcpy(&p->viewers[viewer_pos].addr, viewer, sizeof(*viewer));
	}
viewer_ready:
	p->viewers[viewer_pos].channel = current;
	p->viewers[viewer_pos].last_seen = now;

	for (i = 0; i < p->edge_count; i++) {
		edge_t *e = &p->edges[i];
		if (e->from == current && e->count >= p->settings.min_observations) {
			if (SIZE_MAX == first || e->count > first_count ||
			    (e->count == first_count &&
			    e->last_seen > first_seen)) {
				second = first; second_count = first_count;
				second_seen = first_seen;
				first = e->to; first_count = e->count;
				first_seen = e->last_seen;
			} else if (SIZE_MAX == second || e->count > second_count ||
			    (e->count == second_count &&
			    e->last_seen > second_seen)) {
				second = e->to; second_count = e->count;
				second_seen = e->last_seen;
			}
		}
		if (e->to == current && e->count >= p->settings.min_observations &&
		    (SIZE_MAX == reverse || e->count > reverse_count ||
		    (e->count == reverse_count &&
		    e->last_seen > reverse_seen))) {
			reverse = e->from;
			reverse_count = e->count;
			reverse_seen = e->last_seen;
		}
	}
	if (SIZE_MAX != first)
		target_set(&next[(*next_count)++], &p->channels[first],
		    first_count, first_seen);
	if (SIZE_MAX != second)
		target_set(&next[(*next_count)++], &p->channels[second],
		    second_count, second_seen);
	if (SIZE_MAX != reverse) {
		target_set(prev, &p->channels[reverse], reverse_count,
		    reverse_seen);
		*have_prev = 1;
	}
	pthread_mutex_unlock(&p->lock);
	return (0);
}
