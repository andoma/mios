#include "../../host/dfu.c"

int
main(int argc, char **argv)
{
  if(argc != 2) {
    printf("usage: %s <elf-file>\n", argv[0]);
    exit(1);
  }

  libusb_context *ctx;
  if(libusb_init(&ctx)) {
    printf("libusb_init failed\n");
    exit(1);
  }

  const char *err = dfu_flash_elf(ctx, argv[1], 1);
  libusb_exit(ctx);
  if(err) {
    printf("%s\n", err);
    exit(1);
  }
  return 0;
}
