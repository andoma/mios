#include <fdt/fdt.h>

#include <stdio.h>

typedef struct fdt_nodelink {
  STAILQ_ENTRY(fdt_nodelink) link;
  const char *name;
} fdt_nodelink_t;

static inline uint32_t be32(uint32_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(v);
#else
    return v;
#endif
}


error_t
fdt_init_walkctx(fdt_walkctx_t *ctx, const void *blob)
{
  const struct fdt_header *hdr = (const struct fdt_header *)blob;
  if (be32(hdr->magic) != FDT_MAGIC) {
    return ERR_MALFORMED;
  }

  memset(ctx, 0, sizeof(fdt_walkctx_t));
  ctx->strings = (const char *)(blob + be32(hdr->off_dt_strings));
  ctx->data = (uint32_t *)(blob + be32(hdr->off_dt_struct));
  STAILQ_INIT(&ctx->stack);
  return 0;
}




static void
fdt_erase(uint32_t *start, uint32_t *end)
{
  while(start < end)
    *start++ = be32(FDT_NOP);
}


static uint32_t *
fdt_walk0(uint32_t *p, fdt_walkctx_t *ctx)
{
  uint32_t *erase;

  fdt_nodelink_t nl;
  uint32_t plen;
  uint32_t nameoff;

  while(1) {
    uint32_t tok = be32(*p++);
    switch (tok) {
    case FDT_NOP:
      break;

    case FDT_BEGIN_NODE:
      nl.name = (const char *)p;
      size_t nlen = strlen(nl.name) + 1;

      if(nlen == 1)
        nl.name = "";

      STAILQ_INSERT_TAIL(&ctx->stack, &nl, link);

      if(ctx->node_cb(ctx->opaque, ctx)) {
        erase = p - 1;
      } else {
        erase = NULL;
      }

      p = (uint32_t *)((uint8_t *)p + ((nlen + 3) & ~3));

      p = fdt_walk0(p, ctx);
      STAILQ_REMOVE(&ctx->stack, &nl, fdt_nodelink, link);

      if(p == NULL)
        return NULL;
      if(erase) {
        fdt_erase(erase, p);
      }
      break;

    case FDT_END_NODE:
      return p;

    case FDT_END:
      return NULL;

    case FDT_PROP:
      plen = be32(*p++);
      nameoff = be32(*p++);
      if(ctx->prop_cb) {
        size_t newlen = ctx->prop_cb(ctx->opaque, ctx,
                                     ctx->strings + nameoff,
                                     p, plen);

        if(newlen != plen) {
          if(newlen > plen)
            return NULL; // Not supported for in-place modifications

          if(newlen == 0) {
            // Full wipeout of entire property
            fdt_erase(p - 2, p + ((plen + 3) >> 2));
          } else {
            size_t prev_words = (plen + 3) >> 2;
            size_t new_words = (newlen + 3) >> 2;
            p[-2] = be32(newlen);
            fdt_erase(p + new_words, p + prev_words);
          }
        }
      }
      p += (plen + 3) >> 2;
      break;

    default:
      return NULL;
    }
  }
}

uint32_t *
fdt_walk(fdt_walkctx_t *ctx)
{
  return fdt_walk0(ctx->data, ctx);
}

int
fdt_walk_match_node_name(const char *name, fdt_walkctx_t *ctx)
{
  const fdt_nodelink_t *nl;
  size_t remain = strlen(name);
  STAILQ_FOREACH(nl, &ctx->stack, link) {
    size_t len = strlen(nl->name);

    if(len > remain)
      return 0;

    if(name[len] == '/' && STAILQ_NEXT(nl, link)) {
      if(!memcmp(name, nl->name, len)) {
        name += len + 1;
        remain -= len + 1;
        continue;
      }
    } else if(name[len] == 0 && !STAILQ_NEXT(nl, link)) {
      return !strcmp(name, nl->name);
    }
    return 0;
  }
  return 0;
}


