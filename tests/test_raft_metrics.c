#include "raft_metrics.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static unsigned checks;
#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); return 1; \
} ++checks; } while (0)

static void *exercise(void *ctx)
{
    raft_metrics_t *m = ctx;
    for (int i=0; i<5000; ++i) {
        raft_metrics_increment(m, RAFT_METRIC_VOTE_REQUESTS);
        raft_metrics_connection_open(m);
        raft_metrics_connection_close(m);
    }
    return NULL;
}

int main(void)
{
    raft_metrics_t m;
    raft_metrics_init(&m, 1234);
    CHECK(m.started_ms == 1234);
    CHECK(raft_metrics_name(RAFT_METRIC_NODE_ID) != NULL);
    CHECK(raft_metrics_name(RAFT_METRIC_COUNT) == NULL);
    CHECK(raft_metrics_name((raft_metric_id_t)-1) == NULL);
    for (int i = 0; i < RAFT_METRIC_COUNT; ++i) {
        CHECK(raft_metrics_name((raft_metric_id_t)i) != NULL);
        CHECK(strstr(raft_metrics_name((raft_metric_id_t)i), "secret") == NULL);
    }
    pthread_t threads[4];
    for (size_t i=0; i<4; ++i)
        CHECK(pthread_create(&threads[i], NULL, exercise, &m) == 0);
    for (size_t i=0; i<4; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    raft_metrics_sample_t sample;
    raft_metrics_capture(&m, &sample);
    CHECK(sample.value[RAFT_METRIC_VOTE_REQUESTS] == 20000);
    CHECK(sample.value[RAFT_METRIC_CONNECTIONS_ACTIVE] == 0);
    CHECK(sample.value[RAFT_METRIC_CONNECTIONS_PEAK] >= 1);
    CHECK(sample.value[RAFT_METRIC_CONNECTIONS_PEAK] <= 4);
    raft_metrics_add(&m, RAFT_METRIC_ENTRIES_SENT, 17);
    raft_metrics_increment(&m, RAFT_METRIC_ENTRIES_SENT);
    raft_metrics_increment(&m, RAFT_METRIC_COUNT);
    raft_metrics_capture(&m, &sample);
    CHECK(sample.value[RAFT_METRIC_ENTRIES_SENT] == 18);
    sample.value[RAFT_METRIC_NODE_ID] = 3;
    sample.value[RAFT_METRIC_TERM] = UINT64_C(0x0102030405060708);
    sample.value[RAFT_METRIC_ROLE] = 2;
    sample.value[RAFT_METRIC_LAST_LOG_INDEX] = 10;
    sample.value[RAFT_METRIC_COMMIT_INDEX] = 8;
    sample.value[RAFT_METRIC_LAST_APPLIED] = 7;
    uint8_t bytes[RAFT_METRICS_WIRE_BYTES+1];
    memset(bytes, 0x5a, sizeof(bytes));
    CHECK(raft_metrics_encode(&sample, bytes, sizeof(bytes)) == 0);
    CHECK(memcmp(bytes, "RMET", 4) == 0);
    CHECK(bytes[4] == 0 && bytes[5] == 1 && bytes[6] == 0 &&
          bytes[7] == RAFT_METRIC_COUNT);
    CHECK(bytes[sizeof(bytes)-1] == 0x5a); /* no write past schema */
    size_t start = 8 + RAFT_METRIC_TERM * 8;
    const uint8_t big_endian[] = {1,2,3,4,5,6,7,8};
    CHECK(memcmp(bytes + start, big_endian, 8) == 0);
    raft_metrics_sample_t output;
    memset(&output, 0xcd, sizeof(output));
    CHECK(raft_metrics_decode(bytes, RAFT_METRICS_WIRE_BYTES, &output) == 0);
    CHECK(memcmp(&sample, &output, sizeof(output)) == 0);
    raft_metrics_sample_t original = output;
    CHECK(raft_metrics_decode(bytes, RAFT_METRICS_WIRE_BYTES - 1, &output) != 0);
    CHECK(memcmp(&output, &original, sizeof(output)) == 0);
    bytes[5] = 2;
    CHECK(raft_metrics_decode(bytes, RAFT_METRICS_WIRE_BYTES, &output) != 0);
    bytes[5] = 1;
    bytes[7]++;
    CHECK(raft_metrics_decode(bytes, RAFT_METRICS_WIRE_BYTES, &output) != 0);
    bytes[7]--;
    CHECK(memcmp(&output, &original, sizeof(output)) == 0);
    bytes[8 + RAFT_METRIC_ROLE * 8 + 7] = 9;
    CHECK(raft_metrics_decode(bytes, RAFT_METRICS_WIRE_BYTES, &output) != 0);
    CHECK(memcmp(&output, &original, sizeof(output)) == 0);
    bytes[8 + RAFT_METRIC_ROLE * 8 + 7] = 2;
    sample.value[RAFT_METRIC_COMMIT_INDEX] = 11;
    memset(bytes, 0xa4, sizeof(bytes));
    CHECK(raft_metrics_encode(&sample, bytes, sizeof(bytes)) != 0);
    CHECK(bytes[0] == 0xa4); /* invalid sample never partially encoded */
    CHECK(raft_metrics_encode(NULL, bytes, sizeof(bytes)) != 0);
    CHECK(raft_metrics_decode(NULL, RAFT_METRICS_WIRE_BYTES, &output) != 0);
    CHECK(raft_metrics_decode(bytes, RAFT_METRICS_WIRE_BYTES, NULL) != 0);
    CHECK(raft_metrics_encode(&original, bytes, RAFT_METRICS_WIRE_BYTES-1) != 0);
    printf("PASS: %u Phase 28 atomic metrics, schema and defensive decoding checks\n", checks);
    return 0;
}
