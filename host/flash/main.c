#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "flash.h"

static void
usage(const char *argv0)
{
  printf("Usage: %s [OPTIONS] [ELF-FILE]\n"
         "\n"
         "  Load mios firmware onto a target device\n"
         "\n"
         "   -m METHOD    jlink, dfu, openocd, nrfdfu or auto [default: auto]\n"
         "   -c CMDLINE   Boot cmdline deposited in RAM (dfu)\n"
         "   -s SERIAL    Select probe by USB serial number (jlink)\n"
         "   -S KHZ       SWD clock [default: 4000] (jlink)\n"
         "   -H HOST      OpenOCD TCL host [default: 127.0.0.1]\n"
         "   -P PORT      OpenOCD TCL port [default: 6666]\n"
         "   -n           Don't reset & run after programming\n"
         "   -N           Skip verification\n"
         "   -f           Flash even if build ID matches (dfu)\n"
         "   -e           Recover/unlock chip first (jlink; wipes chip)\n"
         "   -r           Just reset the target and run\n",
         argv0);
}

int
main(int argc, char **argv)
{
  flash_params_t p = {};
  int opt;

  while((opt = getopt(argc, argv, "m:c:s:S:H:P:nNferh")) != -1) {
    switch(opt) {
    case 'm': p.method = optarg; break;
    case 'c': p.cmdline = optarg; break;
    case 's': p.serial = optarg; break;
    case 'S': p.swd_khz = atoi(optarg); break;
    case 'H': p.openocd_host = optarg; break;
    case 'P': p.openocd_port = atoi(optarg); break;
    case 'n': p.flags |= FLASH_NO_RUN; break;
    case 'N': p.flags |= FLASH_NO_VERIFY; break;
    case 'f': p.flags |= FLASH_FORCE; break;
    case 'e': p.flags |= FLASH_RECOVER; break;
    case 'r': p.flags |= FLASH_RESET_ONLY; break;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 1;
    }
  }
  argc -= optind;
  argv += optind;

  if(!(p.flags & FLASH_RESET_ONLY)) {
    if(argc != 1) {
      fprintf(stderr, "No ELF file given\n");
      return 1;
    }
    p.elf_path = argv[0];
  }

  flash_log_t log;
  flash_log_init(&log, stdout);
  const int r = flash_run(&p, &log);
  flash_log_free(&log);
  return r ? 1 : 0;
}
