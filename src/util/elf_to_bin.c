#include <mios/elf.h>

#include <mios/stream.h>
#include <mios/error.h>
#include <mios/eventlog.h>

#include <sys/param.h>

#include <stdint.h>
#include <stddef.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>

typedef struct elf32hdr {
  uint32_t magic;
  uint8_t bits;
  uint8_t endian;
  uint8_t header_version;
  uint8_t abi;
  uint8_t pad[8];
  uint16_t type;
  uint16_t instruction_set;
  uint32_t elf_version;
  uint32_t program_entry_offset;
  uint32_t program_header_table_offset;
  uint32_t section_header_table_offset;
  uint32_t flags;
  uint16_t elf_header_size;
  uint16_t program_header_entry_size;
  uint16_t program_header_entries;
  uint16_t section_header_entry_size;
  uint16_t section_header_entries;
  uint16_t string_table_section_entry;
} elf32hdr_t;


typedef struct elf32phdr {
  uint32_t type;
  uint32_t offset;
  uint32_t vaddr;
  uint32_t paddr;
  uint32_t filesz;
  uint32_t memsz;
  uint32_t flags;
  uint32_t alignment;
} elf32phdr_t;

typedef struct elf32shdr {
  uint32_t name;
  uint32_t type;
  uint32_t flags;
  uint32_t addr;
  uint32_t offset;
  uint32_t size;
  uint32_t link;
  uint32_t info;
  uint32_t align;
  uint32_t entsize;
} elf32shdr_t;


typedef struct elf_to_bin {
  struct stream input;
  struct stream *output;
  void *pht;
  uint32_t ipos;   // Position in input stream
  uint32_t paddr;  // Current output paddr
  uint32_t segment_offset; // Offset in current segment
  int segment; // Current segment
  elf32hdr_t hdr;
} elf_to_bin_t;

static const elf32phdr_t *
current_segment(const elf_to_bin_t *etb)
{
  return etb->pht + etb->segment * etb->hdr.program_header_entry_size;
}

static int
skip_segments(elf_to_bin_t *etb)
{
  while(1) {

    if(etb->segment == etb->hdr.program_header_entries) {
      return 0; // End of file
    }

    const elf32phdr_t *phdr = current_segment(etb);
    if(phdr->type != 1 || !phdr->filesz) {
      // Skip anything that's not PT_LOAD or is zero-sized
      etb->segment++;
      continue;
    }

    if(phdr->offset < etb->ipos) {
      // We can't seek backward, this is game over
      // One option might be to sort the phdrs on init
      return ERR_MALFORMED;
    }
    return 1; // Keep going
  }
}

static const uint8_t pad_ff[16] = {
  0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,
};

static ssize_t
etb_output_pad(elf_to_bin_t *etb, size_t pad)
{
  while(pad) {
    size_t chunk = MIN(pad, sizeof(pad_ff));
    ssize_t r = stream_write(etb->output, pad_ff, chunk, 0);
    if(r < 1)
      return r;
    pad -= chunk;
  }
  return 1;
}

static ssize_t
etb_output(elf_to_bin_t *etb, uint32_t paddr, const void *data, size_t len)
{
  if(paddr > etb->paddr) {
    // Insert PAD
    size_t pad = paddr - etb->paddr;
    etb->paddr += pad;
    ssize_t r = etb_output_pad(etb, pad);
    if(r < 1)
      return r;
  }

  if(paddr + len <= etb->paddr)
    return 1; // Skip everything

  size_t skip = etb->paddr - paddr;
  data += skip;
  len -= skip;

  etb->paddr += len;

  ssize_t r = stream_write(etb->output, data, len, 0);
  return r < 1 ? r : 1;
}


static int
etb_consume_partial(elf_to_bin_t *etb, const void *data, size_t len)
{
  if(etb->ipos < sizeof(elf32hdr_t)) {
    const size_t chunk = MIN(len, sizeof(elf32hdr_t) - etb->ipos);
    memcpy(((void *)&etb->hdr) + etb->ipos, data, chunk);
    if(etb->ipos + chunk == sizeof(elf32hdr_t)) {

      if(etb->hdr.magic != 0x464c457f || etb->hdr.type != 2 ||
         etb->hdr.instruction_set != 40) {
        evlog(LOG_ERR, "Not an ARM ELF executable");
        return ERR_MALFORMED;
      }
    }
    return chunk;
  }

  const uint32_t pht_start = etb->hdr.program_header_table_offset;

  if(etb->ipos < pht_start) {
    return MIN(len, pht_start - etb->ipos);
  }

  const uint32_t pht_size =
    etb->hdr.program_header_entry_size * etb->hdr.program_header_entries;
  const uint32_t pht_end = pht_start + pht_size;

  if(etb->ipos >= pht_start && etb->ipos < pht_end) {
    const size_t chunk = MIN(len, pht_end - etb->ipos);

    if(etb->pht == NULL) {
      etb->pht = xalloc(pht_size, 0, MEM_MAY_FAIL);
      if(etb->pht == NULL) {
        return ERR_NO_MEMORY;
      }
    }

    memcpy(etb->pht + etb->ipos - pht_start, data, chunk);

    if(etb->ipos + chunk == pht_end) {
      int r = skip_segments(etb);
      if(r < 1)
        return r;
    }
    return chunk;
  }

  const elf32phdr_t *phdr = current_segment(etb);

  if(etb->ipos < phdr->offset) {
    return MIN(len, phdr->offset - etb->ipos);
  }

  if(etb->ipos == phdr->offset) {
    // Start of segment
    etb->segment_offset = 0;
  }

  const uint32_t segment_end = phdr->offset + phdr->filesz;
  const size_t chunk = MIN(len, segment_end - etb->ipos);

  int r = etb_output(etb, phdr->paddr + etb->segment_offset, data, chunk);
  if(r < 1)
    return r;

  if(etb->ipos + chunk == segment_end) {
    etb->segment++;
    int r = skip_segments(etb);
    if(r < 1)
      return r;
  }
  etb->segment_offset += chunk;
  return chunk;
}


static int
etb_push(elf_to_bin_t *etb, const void *data, size_t len)
{
  while(len) {
    int r = etb_consume_partial(etb, data, len);
    if(r < 1)
      return r;
    data += r;
    len -= r;
    etb->ipos += r;
  }
  return 1;
}


static ssize_t
etb_write(struct stream *st, const void *data, size_t len, int flags)
{
  elf_to_bin_t *etb = (elf_to_bin_t *)st;
  if(data == NULL) // Flush
    return 0;

  int n = etb_push(etb, data, len);
  if(n == 0) {
    // Local EOF, flush
    return stream_write(etb->output, NULL, 0, 0);
  }
  return n == 1 ? len : n;
}

static void
etb_close(struct stream *st)
{
  elf_to_bin_t *etb = (elf_to_bin_t *)st;

  stream_close(etb->output);
  free(etb->pht);
  free(etb);
}


static const stream_vtable_t etb_vtable = {
  .write = etb_write,
  .close = etb_close,
};

struct stream *
elf_to_bin(struct stream *output, uint32_t paddr_start)
{
  if(output == NULL)
    return NULL;

  elf_to_bin_t *etb = xalloc(sizeof(elf_to_bin_t), 0,
                             MEM_MAY_FAIL | MEM_CLEAR);
  if(etb == NULL)
    return NULL;

  etb->input.vtable = &etb_vtable;
  etb->paddr = paddr_start;
  etb->output = output;
  return &etb->input;
}
