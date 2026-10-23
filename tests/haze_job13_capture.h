#ifndef HAZE_JOB13_CAPTURE_H
#define HAZE_JOB13_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>
#include "spu_context.h"

enum {
    HAZE_JOB13_CAPTURE_VERSION = 2,
    HAZE_JOB13_CAPTURE_HEADER_SIZE = 128,
    HAZE_JOB13_CAPTURE_EVENT_SIZE = 64,
    HAZE_JOB13_CAPTURE_LS_SIZE = 256 * 1024,
    HAZE_JOB13_CAPTURE_GPR_BYTES = 128 * 16,
    HAZE_JOB13_CAPTURE_MAX_BATCHES = 8,
    HAZE_JOB13_CAPTURE_MAX_EVENTS = 8192,
    HAZE_JOB13_CAPTURE_MAX_PAYLOAD = 4 * 1024 * 1024
};

enum haze_job13_event_kind {
    HAZE_JOB13_EVENT_WRCH = 1,
    HAZE_JOB13_EVENT_RDCH = 2,
    HAZE_JOB13_EVENT_RCHCNT = 3
};

enum haze_job13_invalid_reason {
    HAZE_JOB13_VALID = 0,
    HAZE_JOB13_INVALID_UNSUPPORTED_CHANNEL = 1,
    HAZE_JOB13_INVALID_UNSUPPORTED_COMMAND = 2,
    HAZE_JOB13_INVALID_LOCAL_STORE_RANGE = 3,
    HAZE_JOB13_INVALID_EVENT_OVERFLOW = 4,
    HAZE_JOB13_INVALID_PAYLOAD_OVERFLOW = 5,
    HAZE_JOB13_INVALID_CONTEXT = 6,
    HAZE_JOB13_INVALID_INTERRUPT_STATE = 7,
    HAZE_JOB13_INVALID_CLOCK = 8,
    HAZE_JOB13_INVALID_RESERVATION_STATE = 9
};

/* Version 2 is a little-endian, fixed-width capture file. Header fields are
 * followed by initial_gpr, final_gpr, initial_ls, final_ls, events, payload.
 * Event payload offsets are relative to the final payload section. Invalid batches are
 * still emitted for diagnosis but must never be accepted by a replay tool. */
typedef struct haze_job13_capture_header {
    uint8_t magic[8];                 /* "HZJOB13\0" */
    uint32_t version;
    uint32_t header_size;
    uint32_t valid;
    uint32_t invalid_reason;
    uint32_t image_id;
    uint32_t initial_pc;
    uint32_t final_pc;
    uint32_t initial_status;
    uint32_t final_status;
    uint32_t initial_mxcsr;
    uint32_t final_mxcsr;
    uint32_t event_count;
    uint32_t event_calls;
    uint32_t payload_size;
    uint32_t initial_lsa;
    uint32_t initial_eah;
    uint32_t initial_eal;
    uint32_t initial_size;
    uint32_t initial_tag;
    uint32_t initial_tag_mask;
    uint32_t initial_tag_status;
    uint32_t host_tid;
    uint32_t batch_index;
    uint32_t initial_atomic_stat;
    uint64_t qpc_frequency;
    uint64_t begin_qpc;
    uint64_t end_qpc;
} haze_job13_capture_header;

/* One row for each wrapped channel operation. value is the exact 16-byte
 * u128 for WRCH/RDCH; RCHCNT stores the returned u32 in its first four bytes.
 * DMA bytes (when any) are in the payload section at payload_offset. */
typedef struct haze_job13_capture_event {
    uint32_t sequence;
    uint32_t kind;
    uint32_t channel;
    uint32_t command;
    uint32_t lsa;
    uint32_t eah;
    uint32_t eal;
    uint32_t size;
    uint32_t tag;
    uint32_t payload_offset;
    uint32_t payload_length;
    uint8_t value[16];
    uint32_t atomic_status;          /* post-operation status for GETLLAR/PUTLLC, else 0 */
} haze_job13_capture_event;

/* Begin/end are paired around one image-32 invocation. Begin returns false
 * when capture is inactive or cannot reserve a bounded slot. Channel wrappers
 * always invoke the real SDK operation exactly once, whether capturing or not. */
#ifdef __cplusplus
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif
extern "C" {
#endif
bool haze_job13_capture_begin(struct spu_context* ctx);
void haze_job13_capture_end(struct spu_context* ctx);
void haze_job13_capture_wrch(struct spu_context* ctx, uint32_t channel, u128 value);
u128 haze_job13_capture_rdch(struct spu_context* ctx, uint32_t channel);
uint32_t haze_job13_capture_rchcnt(struct spu_context* ctx, uint32_t channel);
#ifdef __cplusplus
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

#if defined(__cplusplus)
static_assert(sizeof(haze_job13_capture_header) == HAZE_JOB13_CAPTURE_HEADER_SIZE,
              "job13 capture header ABI changed");
static_assert(sizeof(haze_job13_capture_event) == HAZE_JOB13_CAPTURE_EVENT_SIZE,
              "job13 capture event ABI changed");
#else
_Static_assert(sizeof(haze_job13_capture_header) == HAZE_JOB13_CAPTURE_HEADER_SIZE,
               "job13 capture header ABI changed");
_Static_assert(sizeof(haze_job13_capture_event) == HAZE_JOB13_CAPTURE_EVENT_SIZE,
               "job13 capture event ABI changed");
#endif

#endif
