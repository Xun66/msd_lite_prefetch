#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "../src/channel_predict.h"

#define MAX_CHANNELS 512
#define MAX_EDGES 4096
#define PERSIST_MAGIC 0x4d534450U
#define PERSIST_VERSION 1U

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

typedef struct persist_header_s {
	uint32_t magic;
	uint32_t version;
	uint64_t channel_count;
	uint64_t edge_count;
} persist_header_t;

static size_t
channel_add(channel_t *channels, size_t *channel_count, const char *name,
    uint32_t if_index, uint64_t strength, time_t timestamp) {
	struct sockaddr_in *sin;
	char host[INET_ADDRSTRLEN];
	const char *address, *colon;
	size_t i, host_len, name_size;
	unsigned long port;
	char *end;

	name_size = strlen(name);
	if (name_size == 0 || name_size > CH_PREDICT_NAME_MAX)
		return (SIZE_MAX);
	for (i = 0; i < *channel_count; i++) {
		if (channels[i].name_size == name_size &&
		    memcmp(channels[i].name, name, name_size) == 0) {
			channels[i].observations += strength;
			channels[i].last_seen = timestamp;
			return (i);
		}
	}
	if (*channel_count >= MAX_CHANNELS)
		return (SIZE_MAX);
	address = name;
	if (strncmp(address, "/udp/", 5) == 0)
		address += 5;
	colon = strrchr(address, ':');
	if (colon == NULL)
		return (SIZE_MAX);
	host_len = (size_t)(colon - address);
	if (host_len == 0 || host_len >= sizeof(host))
		return (SIZE_MAX);
	memcpy(host, address, host_len);
	host[host_len] = '\0';
	errno = 0;
	port = strtoul(colon + 1, &end, 10);
	if (errno != 0 || (*end != '\0' && *end != '@') ||
	    port == 0 || port > 65535)
		return (SIZE_MAX);

	i = (*channel_count)++;
	memcpy(channels[i].name, name, name_size);
	channels[i].name_size = name_size;
	sin = (struct sockaddr_in *)&channels[i].addr;
	sin->sin_family = AF_INET;
	sin->sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &sin->sin_addr) != 1)
		return (SIZE_MAX);
	channels[i].if_index = if_index;
	channels[i].observations = strength;
	channels[i].last_seen = timestamp;
	return (i);
}

static int
edge_add(edge_t *edges, size_t *edge_count, size_t from, size_t to,
    uint64_t strength, time_t timestamp) {
	size_t i;

	if (from == to)
		return (0);
	for (i = 0; i < *edge_count; i++) {
		if (edges[i].from == from && edges[i].to == to) {
			edges[i].count += strength;
			edges[i].last_seen = timestamp;
			return (0);
		}
	}
	if (*edge_count >= MAX_EDGES)
		return (-1);
	i = (*edge_count)++;
	edges[i].from = from;
	edges[i].to = to;
	edges[i].count = strength;
	edges[i].last_seen = timestamp;
	return (0);
}

int
main(int argc, char **argv) {
	channel_t channels[MAX_CHANNELS] = {0};
	edge_t edges[MAX_EDGES] = {0};
	persist_header_t header;
	FILE *input, *output;
	char line[2048], normalized[CH_PREDICT_NAME_MAX + 1], *path;
	size_t channel_count = 0, edge_count = 0;
	size_t previous = SIZE_MAX, current;
	uint32_t if_index;
	uint64_t strength;
	time_t timestamp;
	ch_predict_settings_t settings;
	ch_predict_p predict = NULL;
	int error;

	if (argc != 8) {
		fprintf(stderr, "usage: %s M3U OUTPUT IFINDEX STRENGTH TIMESTAMP PREFIX SUFFIX\n",
		    argv[0]);
		return (2);
	}
	if_index = (uint32_t)strtoul(argv[3], NULL, 10);
	strength = strtoull(argv[4], NULL, 10);
	timestamp = (time_t)strtoll(argv[5], NULL, 10);
	if (if_index == 0 || strength == 0 || timestamp <= 0)
		return (2);
	input = fopen(argv[1], "r");
	if (input == NULL)
		return (perror(argv[1]), 1);

	while (fgets(line, sizeof(line), input) != NULL) {
		line[strcspn(line, "\r\n")] = '\0';
		path = strstr(line, argv[6]);
		if (path == NULL)
			continue;
		if (snprintf(normalized, sizeof(normalized), "%s%s", path,
		    argv[7]) >= (int)sizeof(normalized)) {
			fprintf(stderr, "channel name too long: %s\n", path);
			return (1);
		}
		current = channel_add(channels, &channel_count, normalized, if_index,
		    strength, timestamp);
		if (current == SIZE_MAX) {
			fprintf(stderr, "invalid or excessive channel: %s\n", line);
			return (1);
		}
		if (previous != SIZE_MAX &&
		    edge_add(edges, &edge_count, previous, current, strength,
		    timestamp) != 0) {
			fprintf(stderr, "too many edges\n");
			return (1);
		}
		previous = current;
	}
	fclose(input);

	output = fopen(argv[2], "wb");
	if (output == NULL)
		return (perror(argv[2]), 1);
	header.magic = PERSIST_MAGIC;
	header.version = PERSIST_VERSION;
	header.channel_count = channel_count;
	header.edge_count = edge_count;
	if (fwrite(&header, sizeof(header), 1, output) != 1 ||
	    fwrite(channels, sizeof(*channels), channel_count, output) !=
	    channel_count ||
	    fwrite(edges, sizeof(*edges), edge_count, output) != edge_count ||
	    fclose(output) != 0) {
		fprintf(stderr, "write failed\n");
		return (1);
	}
	memset(&settings, 0, sizeof(settings));
	settings.max_channels = MAX_CHANNELS;
	settings.max_edges = MAX_EDGES;
	settings.entry_ttl = 604800;
	settings.min_observations = 2;
	error = ch_predict_create(&settings, &predict);
	if (error != 0 || (error = ch_predict_load(predict, argv[2])) != 0) {
		fprintf(stderr, "self-validation failed: %d\n", error);
		ch_predict_destroy(predict);
		return (1);
	}
	ch_predict_destroy(predict);
	printf("channels=%zu edges=%zu strength=%llu timestamp=%lld\n",
	    channel_count, edge_count, (unsigned long long)strength,
	    (long long)timestamp);
	return (0);
}
