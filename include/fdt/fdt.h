#pragma once

#include <string.h>
#include <sys/queue.h>
#include <stdint.h>
#include <mios/error.h>

#define FDT_MAGIC        0xd00dfeed
#define FDT_BEGIN_NODE   0x1
#define FDT_END_NODE     0x2
#define FDT_PROP         0x3
#define FDT_NOP          0x4
#define FDT_END          0x9

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;  /* since v2 */
    uint32_t size_dt_strings;  /* since v3 */
    uint32_t size_dt_struct;   /* since v3 */
};

STAILQ_HEAD(fde_nodelink_queue, fdt_nodelink);

typedef struct fdt_walkctx {
  // If callback returns 1 entire node will be removed
  int (*node_cb)(void *opaque, struct fdt_walkctx *ctx);

  // Return new size of data (or same as len) for no change. 0 = erase property
  size_t (*prop_cb)(void *opaque, struct fdt_walkctx *ctx, const char *name,
                    void *data, size_t len);
  void *opaque;
  const char *strings;
  struct fde_nodelink_queue stack;
  uint32_t *data;
} fdt_walkctx_t;


uint32_t *fdt_walk(fdt_walkctx_t *ctx);

int fdt_walk_match_node_name(const char *name, fdt_walkctx_t *ctx);

error_t fdt_init_walkctx(fdt_walkctx_t *ctx, const void *blob);
