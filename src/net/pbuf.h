#pragma once

#include <sys/queue.h>
#include <stddef.h>
#include <stdint.h>

#ifndef PBUF_DATA_SIZE
#define PBUF_DATA_SIZE 512
#endif

STAILQ_HEAD(pbuf_queue, pbuf);

#define PBUF_SOP   0x1
#define PBUF_EOP   0x2
#define PBUF_SEQ   0x8
#define PBUF_BCAST 0x10
#define PBUF_MCAST 0x20

typedef struct pbuf {

  STAILQ_ENTRY(pbuf) pb_link;
#define pb_next pb_link.stqe_next

  uint8_t pb_flags;
  uint8_t pb_credits;
  uint16_t pb_pktlen;
  uint16_t pb_offset;
  uint16_t pb_buflen;

  void *pb_data;

#ifdef ENABLE_NET_TIMESTAMPING
  uint64_t pb_timestamp;
#endif

} pbuf_t;

void pbuf_reset(pbuf_t *pb, size_t header_size, size_t len);

static inline void *pbuf_data(pbuf_t *pb, size_t offset) {
  return pb->pb_data + pb->pb_offset + offset;
}

static inline const void *pbuf_cdata(const pbuf_t *pb, size_t offset) {
  return pb->pb_data + pb->pb_offset + offset;
}

// Remove from head
__attribute__((warn_unused_result))
pbuf_t *pbuf_drop(pbuf_t *pb, size_t bytes);

// Remove from tail
void pbuf_trim(pbuf_t *pb, size_t bytes);

__attribute__((warn_unused_result))
pbuf_t *pbuf_prepend(pbuf_t *pb, size_t bytes, int wait, size_t extra_offset);

// returns number of bytes missing (0 == ok)
__attribute__((warn_unused_result))
size_t pbuf_pullup(pbuf_t *pb, size_t bytes);

void pbuf_free(pbuf_t *pb);

__attribute__((warn_unused_result))
pbuf_t *pbuf_make(int offset, int wait);

__attribute__((warn_unused_result))
pbuf_t *pbuf_copy(const pbuf_t *src, int wait);

__attribute__((warn_unused_result))
pbuf_t *pbuf_copy_pkt(const pbuf_t *src, int wait);

__attribute__((warn_unused_result))
void *pbuf_append(pbuf_t *pb, size_t bytes);

__attribute__((warn_unused_result))
pbuf_t *pbuf_splice(struct pbuf_queue *pq);

__attribute__((warn_unused_result))
pbuf_t *pbuf_read(pbuf_t *pb, void *ptr, size_t len);

struct pushpull;
__attribute__((warn_unused_result))
pbuf_t *pbuf_write(pbuf_t *pb, const void *ptr, size_t len,
                   const struct pushpull *p);

__attribute__((warn_unused_result))
int pbuf_read_at(pbuf_t *pb, void *out, size_t offset, size_t len);

__attribute__((warn_unused_result))
int pbuf_memcmp_at(pbuf_t *pb, const void *data, size_t offset, size_t len);

int pbuf_buffer_avail(void);

int pbuf_buffer_total(void);


// =========================================================
// Debug helpers
// =========================================================

struct stream;
void pbuf_status(struct stream *st);

void pbuf_dump(const char *prefix, const pbuf_t *pb, int full);

struct stream;
void pbuf_dump_stream(const char *prefix, const pbuf_t *pb, int full,
                      struct stream *st);

// =========================================================
// All functions below here assume irq_forbid(IRQ_LEVEL_NET)
// =========================================================

void pbuf_data_add(void *start, void *end);

__attribute__((warn_unused_result, malloc))
void *pbuf_data_get(int wait);

void pbuf_data_put(void *ptr);

void pbuf_alloc(size_t count);

__attribute__((warn_unused_result,malloc))
pbuf_t *pbuf_get(int wait);

void pbuf_put(pbuf_t *pb);

void pbuf_free_irq_blocked(pbuf_t *pb);

__attribute__((warn_unused_result))
pbuf_t *pbuf_make_irq_blocked(int offset, int wait);

void pbuf_free_queue_irq_blocked(struct pbuf_queue *pq);
