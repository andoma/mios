#pragma once

#include "error.h"
#include <stddef.h>

error_t efi_exec(const void *bin, size_t size, const char *cmdline);
