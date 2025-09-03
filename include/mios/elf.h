#pragma once

#include <stdint.h>

struct stream;

struct stream *elf_to_bin(struct stream *output, uint32_t paddr_start);
