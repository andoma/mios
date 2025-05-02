#include "mios_image.h"

#include <sys/param.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

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


static const char *
get_shstrtab(const void *elf)
{
  // TODO: Check all the boundaries, this is so unsafe

  const elf32hdr_t *elfhdr = elf;

  const elf32shdr_t *shdr = elf + elfhdr->section_header_table_offset +
    elfhdr->string_table_section_entry * elfhdr->section_header_entry_size;

  return elf + shdr->offset;
}

mios_image_t *
mios_image_from_elf_mem(const void *elf, size_t elfsize)
{
  const elf32hdr_t *elfhdr = elf;

  const char *shstrtab = get_shstrtab(elf);

  uint32_t image_begin = UINT32_MAX;
  uint32_t image_end = 0;

  for(size_t i = 0; i < elfhdr->program_header_entries; i++) {
    const elf32phdr_t *phdr = elf + elfhdr->program_header_table_offset +
      i * elfhdr->program_header_entry_size;

    image_begin = MIN(phdr->paddr, image_begin);
    image_end = MAX(phdr->paddr + phdr->filesz, image_end);
  }

  const size_t image_size = image_end - image_begin;

  mios_image_t *mi = calloc(1, sizeof(mios_image_t) + image_size);
  mi->load_addr = image_begin;
  mi->image_size = image_size;

  for(size_t i = 0; i < elfhdr->program_header_entries; i++) {
    const elf32phdr_t *phdr = elf + elfhdr->program_header_table_offset +
      i * elfhdr->program_header_entry_size;

    uint32_t image_offset = phdr->paddr - image_begin;
    memcpy(mi->image + image_offset, elf + phdr->offset, phdr->filesz);
  }

  for(size_t i = 0; i < elfhdr->section_header_entries; i++) {
    const elf32shdr_t *shdr = elf + elfhdr->section_header_table_offset +
      i * elfhdr->section_header_entry_size;

    const char *nam = shstrtab + shdr->name;
    if(!strcmp(nam, ".build_id")) {
      memcpy(mi->buildid, elf + shdr->offset + 16, 20);
      mi->buildid_paddr = shdr->addr + 16;
    }

    if(!strcmp(nam, ".miosversion")) {
      memcpy(mi->mios_version, elf + shdr->offset, 21);
    }

    if(!strcmp(nam, ".appversion")) {
      memcpy(mi->app_version, elf + shdr->offset, 21);
    }
  }
  return mi;
}


mios_image_t *
mios_image_from_elf_file(const char *path)
{
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd == -1)
    return NULL;

  struct stat st;
  if(fstat(fd, &st) == -1) {
    int err = errno;
    close(fd);
    errno = err;
    return NULL;
  }

  void *mem = malloc(st.st_size);
  int rlen = read(fd, mem, st.st_size);
  close(fd);
  if(rlen != st.st_size) {
    free(mem);
    errno = ENODATA;
    return NULL;
  }

  mios_image_t *mi = mios_image_from_elf_mem(mem, st.st_size);
  free(mem);
  return mi;
}
