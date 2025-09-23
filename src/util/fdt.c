#include <fdt/fdt.h>

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

#define FDT_MAGIC        0xd00dfeed
#define FDT_LAST_COMPVER 0x10

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


static void *
fdt_malloc(size_t len)
{
  return xalloc(len, 0, MEM_MAY_FAIL);
}


static inline uint32_t
be32(uint32_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(v);
#else
    return v;
#endif
}


const char *
fdt_validate(const fdt_t *fdt)
{
  if(fdt->capacity < sizeof(struct fdt_header))
    return "Buffer smaller than header";

  const struct fdt_header *hdr = fdt->buffer;
  if(hdr->magic != be32(FDT_MAGIC))
    return "Invalid Magic";

  if(hdr->last_comp_version != be32(FDT_LAST_COMPVER))
    return "Unsupported version";

  if(be32(hdr->totalsize) > fdt->capacity)
    return "Size in header larger than buffer";

  if(be32(hdr->off_dt_struct) + be32(hdr->size_dt_struct) > fdt->capacity)
    return "struct ends after buffer size";

  if(be32(hdr->off_dt_strings) + be32(hdr->size_dt_strings) > fdt->capacity)
    return "strings ends after buffer size";

  if(be32(hdr->off_mem_rsvmap) < sizeof(struct fdt_header))
    return "rsvmap at invalid offset";

  if(be32(hdr->off_dt_struct) < be32(hdr->off_mem_rsvmap))
    return "struct at invalid offset";

  if(be32(hdr->off_dt_strings) < be32(hdr->off_dt_struct))
    return "strings at invalid offset";

  return NULL;
}


uint32_t
fdt_get_totalsize(const fdt_t *fdt)
{
  const struct fdt_header *hdr = fdt->buffer;
  return be32(hdr->totalsize);
}

typedef struct {
  uint32_t offset;
  uint32_t depth;
} fdtkey_t;


static uint32_t
fdt_key_encode(const fdtkey_t *k)
{
  return k->offset | (k->depth << 24);
}


static fdtkey_t
fdt_key_decode(uint32_t u32)
{
  fdtkey_t k;
  k.offset = u32 & 0xffffff;
  k.depth = u32 >> 24;
  return k;
}


static uint32_t
fdt_rd32(const fdt_t *fdt, uint32_t offset)
{
  const struct fdt_header *hdr = fdt->buffer;
  const uint32_t byte_offset = offset << 2;
  if(byte_offset < be32(hdr->off_dt_struct) ||
     byte_offset >= (be32(hdr->off_dt_struct) + be32(hdr->size_dt_struct)))
    return 0;

  const uint32_t *p32 = fdt->buffer;
  return be32(p32[offset]);
}


static const void *
fdt_cptr(const fdt_t *fdt, uint32_t offset)
{
  return fdt->buffer + (offset << 2);
}


static uint32_t
fdt_consume_u32(const fdt_t *fdt, fdtkey_t *key)
{
  return fdt_rd32(fdt, key->offset++);
}


static int
skip_node(const fdt_t *fdt, fdtkey_t *key)
{
  const char *name;
  size_t namelen;
  uint32_t plen;
  int err;

  while(1) {
    const uint32_t tok = fdt_consume_u32(fdt, key);

    switch(tok) {
    case FDT_NOP:
      break;
    case FDT_BEGIN_NODE:
      name = fdt_cptr(fdt, key->offset);
      namelen = strlen(name);
      key->offset += (namelen + 4) >> 2;
      err = skip_node(fdt, key);
      if(err)
        return err;
      break;
    case FDT_END_NODE:
      return 0;
    default:
      return -1;
    case FDT_PROP:
      plen = fdt_consume_u32(fdt, key);
      key->offset += 1 + ((plen + 3) >> 2);
      break;
    }
  }
}


static int
skip_node_after_token(const fdt_t *fdt, fdtkey_t *key)
{
  const char *name = fdt_cptr(fdt, ++key->offset);
  size_t namelen = strlen(name);
  key->offset += (namelen + 4) >> 2;
  return skip_node(fdt, key);
}


static int
skip_node_at_token(const fdt_t *fdt, fdtkey_t *key)
{
  switch(fdt_consume_u32(fdt, key)) {
  case FDT_BEGIN_NODE:
    return skip_node_after_token(fdt, key);
  case FDT_NOP:
    return 0;
  default:
    return 1;
  }
}


int
fdt_find_begin_node(const fdt_t *fdt, const char *srch, fdtkey_t *key,
                    const char **match)
{
  const char *name = fdt_cptr(fdt, key->offset);
  size_t namelen = strlen(name);
  for(int i = 0; i < key->depth; i++) {
    srch = strchr(srch, '/');
    if(srch == NULL)
      return -1;
    srch++;
  }

  const size_t srchlen = strcspn(srch, "/");
  const int wildcard_match = srchlen == 1 && srch[0] == '*';

  if((namelen == srchlen && !memcmp(srch, name, namelen)) || wildcard_match) {

    if(wildcard_match)
      *match = name;

    if(srch[srchlen] == 0) {
      key->offset--; // Rewind to point at start of node
      return 1;
    } else {
      key->depth++;
      key->offset += (namelen + 4) >> 2;
      return 0;
    }
  }

  key->offset += (namelen + 4) >> 2;
  return skip_node(fdt, key);
}


fdt_node_ref_t
fdt_find_next_node(const fdt_t *fdt, fdt_node_ref_t keyx, const char *srch,
                   const char **match)
{
  const struct fdt_header *hdr = fdt->buffer;

  uint32_t plen;
  if(match)
    *match = NULL;

  fdtkey_t key = fdt_key_decode(keyx);

  if(key.offset == 0) {
    key.offset = be32(hdr->off_dt_struct) >> 2;
  } else {
    if(skip_node_at_token(fdt, &key)) {
      return 0;
    }
  }
  int r = 0;
  while(1) {
    switch (fdt_consume_u32(fdt, &key)) {
    case FDT_NOP:
      break;

    case FDT_BEGIN_NODE:
      r = fdt_find_begin_node(fdt, srch, &key, match);
      if(r == 1) {
        return fdt_key_encode(&key);
      }
      if(r < 0)
        return 0;
      break;
    case FDT_END_NODE:
      if(key.depth == 0)
        return 0;
      key.depth--;
      break;
    case FDT_END:
      return 0;
    case FDT_PROP:
      plen = fdt_consume_u32(fdt, &key);
      key.offset += 1 + ((plen + 3) >> 2);
      break;

    default:
      return 0;
    }
  }
}


static void
write_nops(const fdt_t *fdt, uint32_t start, uint32_t count)
{
  uint32_t *p32 = fdt->buffer;
  for(uint32_t i = 0; i < count; i++) {
    p32[start + i] = be32(FDT_NOP);
  }
}


void
fdt_erase_node(const fdt_t *fdt, fdt_node_ref_t keyx)
{
  if(keyx == 0)
    return;

  fdtkey_t key = fdt_key_decode(keyx);
  const uint32_t start = key.offset;

  if(skip_node_at_token(fdt, &key))
    return;

  write_nops(fdt, start, key.offset - start);
}


static const char *
fdt_pname(const fdt_t *fdt, fdtkey_t *key)
{
  const struct fdt_header *hdr = fdt->buffer;
  const uint32_t offset = fdt_consume_u32(fdt, key) + be32(hdr->off_dt_strings);

  if(offset < be32(hdr->off_dt_strings) ||
     offset >= (be32(hdr->off_dt_strings) + be32(hdr->size_dt_strings)))
    return 0;
  return fdt->buffer + offset;
}


static int
skip_begin_node(const fdt_t *fdt, fdtkey_t *key)
{
  if(fdt_consume_u32(fdt, key) != FDT_BEGIN_NODE)
    return 1;
  const char *nodename = fdt_cptr(fdt, key->offset);
  const size_t namelen = strlen(nodename);
  key->offset += (namelen + 4) >> 2;
  return 0;
}


uint32_t
fdt_find_property(const fdt_t *fdt, fdt_node_ref_t keyx, const char *name,
                  void **outp, size_t *lenp)
{
  uint32_t plen, retval;
  fdtkey_t key = fdt_key_decode(keyx);

  if(skip_begin_node(fdt, &key))
    return 0;

  while(1) {
    const uint32_t tok = fdt_consume_u32(fdt, &key);
    switch (tok) {
    case FDT_NOP:
      break;

    case FDT_BEGIN_NODE:
      if(skip_node_after_token(fdt, &key))
        return 0;
      break;

    case FDT_PROP:
      retval = key.offset;
      plen = fdt_consume_u32(fdt, &key);
      const char *pname = fdt_pname(fdt, &key);
      if(pname != NULL && !strcmp(pname, name)) {
        *outp = fdt->buffer + (key.offset << 2);
        *lenp = plen;
        return retval;
      }
      key.offset += ((plen + 3) >> 2);
      break;

    default:
      return 0;
    }
  }
  return 0;
}


const void *
fdt_get_property(const fdt_t *fdt, fdt_node_ref_t key,
                 const char *name, size_t *lenp)
{
  void *rval = NULL;
  return fdt_find_property(fdt, key, name, &rval, lenp) ? rval : NULL;
}


int
fdt_insert(fdt_t *fdt, uint32_t offset, uint32_t words, size_t strbytes)
{
  struct fdt_header *hdr = fdt->buffer;
  const size_t bytes_needed = words * 4;
  const uint32_t totalsize = be32(hdr->totalsize);
  const size_t tail_bytes = totalsize - offset * 4;

  if(totalsize + bytes_needed + strbytes > fdt->capacity) {
    const size_t newsize = fdt->capacity + bytes_needed + strbytes + 1024;
    void *newbuf = fdt_malloc(newsize);
    if(newbuf == NULL)
      return -1;

    memcpy(newbuf, fdt->buffer, offset * 4);
    memcpy(newbuf + (offset + words) * 4, fdt->buffer + totalsize -
           tail_bytes, tail_bytes);
    fdt->capacity = newsize;

    free(fdt->buffer);
    fdt->buffer = newbuf;
    hdr = fdt->buffer;
  } else {
    memmove(fdt->buffer + (offset + words) * 4,
            fdt->buffer + offset * 4,
            tail_bytes);
  }
  hdr->off_dt_strings = be32(be32(hdr->off_dt_strings) + bytes_needed);
  hdr->totalsize = be32(totalsize + bytes_needed + strbytes);

  hdr->size_dt_strings = be32(be32(hdr->size_dt_strings) + strbytes);
  hdr->size_dt_struct = be32(be32(hdr->size_dt_struct) + bytes_needed);
  return 0;
}


static void
memcpy_and_zeropad(void *dst, const void *src, size_t len)
{
  memcpy(dst, src, len);
  if(len & 3) {
    memset(dst + len, 0, 4 - (len & 3));
  }
}


static int
fdt_find_string(const fdt_t *fdt, const char *search, size_t searchlen)
{
  const struct fdt_header *hdr = fdt->buffer;
  const uint32_t strings = be32(hdr->off_dt_strings);
  const uint32_t totalsize = be32(hdr->totalsize);

  uint32_t offset = strings;
  while(offset < totalsize) {
    const char *str = fdt->buffer + offset;
    const size_t len = strlen(str);
    if(len == searchlen && !memcmp(str, search, len))
      return offset - strings;

    offset += len + 1;
  }
  return -1;
}


int
fdt_set_property(fdt_t *fdt, fdt_node_ref_t keyx, const char *name,
                 const void *data, size_t len)
{
  void *curptr = NULL;
  size_t curlen;
  uint32_t offset = fdt_find_property(fdt, keyx, name, &curptr, &curlen);
  uint32_t *u32p = curptr;

  if(offset) {
    // Property currently exist in tree
    const uint32_t prev_words = (curlen + 3) >> 2;
    const uint32_t new_words = (len + 3) >> 2;
    if(data == NULL) {
      // Complete erase requested
      write_nops(fdt, offset, 2 + prev_words);
    } else if(len <= curlen) {
      // New item fits in old place, overwrite and possible insert FDT_NOP
      memcpy_and_zeropad(curptr, data, len);
      u32p[-2] = be32(len);
      write_nops(fdt, offset + 2 + new_words, prev_words - new_words);
    } else {
      // Item does not fit, need to expand FDT
      offset += 2;
      if(fdt_insert(fdt, offset, new_words - prev_words, 0))
        return -1;
      u32p = fdt->buffer + (offset << 2);
      u32p[-2] = be32(len);
      memcpy_and_zeropad(u32p, data, len);
    }
  } else if(data == NULL) {
    // Want to erase and property does not exist. Easy enough; Do nothing
  } else {
    // Property does not exist
    fdtkey_t key = fdt_key_decode(keyx);
    if(skip_begin_node(fdt, &key))
      return -1;

    // See if we can find its name in the nametable ...
    const size_t namelen = strlen(name);
    int nameindex = fdt_find_string(fdt, name, namelen);

    const uint32_t new_words = (len + 3) >> 2;

    if(fdt_insert(fdt, key.offset, new_words + 3,
                  nameindex < 0 ? namelen + 1: 0))
      return -1;

    if(nameindex < 0) {
      // There's been made place for the string at the end
      const struct fdt_header *hdr = fdt->buffer;
      uint32_t pos = be32(hdr->totalsize) - namelen - 1;
      memcpy(fdt->buffer + pos, name, namelen + 1);
      nameindex = pos - be32(hdr->off_dt_strings);
    }

    u32p = fdt->buffer + (key.offset << 2);
    u32p[0] = be32(FDT_PROP);
    u32p[1] = be32(len);
    u32p[2] = be32(nameindex);
    memcpy_and_zeropad(u32p + 3, data, len);
  }
  return 0;
}

