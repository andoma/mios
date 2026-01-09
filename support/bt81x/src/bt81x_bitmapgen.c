#include <sys/param.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define EVE_FORMAT_L4 2

static int g_fd;
static z_stream zs;

static void (*output)(const void *data, size_t len);

static void
output_bytes_raw(const void *data, size_t len)
{
  if(write(g_fd, data, len) != len) {
    fprintf(stderr, "write failed\n");
    exit(1);
  }
}


static void
output_bytes_bin2c(const void *data, size_t len)
{
  const uint8_t *u8 = data;
  static int pos;

  for(size_t i = 0; i < len; i++) {
    dprintf(g_fd, "0x%02x,%s", u8[i], ((pos & 7) == 7) ? "\n  " : "");
    pos++;
  }
}

typedef struct bitmap_header {
  uint16_t format;
  uint16_t width;
  uint16_t height;
} bitmap_header_t;

static void
write_header(uint16_t format, uint16_t width, uint16_t height)
{
  bitmap_header_t bh = {format, width, height};
  output(&bh, sizeof(bh));
}

static int
do_deflate(int how)
{
  uint8_t outbuf[64];

  zs.next_out = outbuf;
  zs.avail_out = sizeof(outbuf);
  int ret = deflate(&zs, how);
  if(ret < 0) {
    fprintf(stderr, "deflate failed: %s\n", zError(ret));
    exit(1);
  }
  const size_t outbytes = sizeof(outbuf) - zs.avail_out;

  output(outbuf, outbytes);
  return ret;
}



static void
write_payload(const void *data, size_t len)
{
  zs.next_in = (void *)data;
  zs.avail_in = len;
  do_deflate(Z_NO_FLUSH);
}





static void
output_to_l4(const char *output_filename,
             int width, int height,
             const uint8_t *rgba)
{
  if(width & 1) {
    fprintf(stderr, "Width is not a multple of two\n");
    exit(1);
  }

  write_header(EVE_FORMAT_L4, width, height);

  size_t outsize = width / 2 * height;
  uint8_t l4[outsize];
  const uint8_t *s = rgba;
  uint8_t *d = l4;

  for(int y = 0; y < height; y++) {
    for(int x = 0; x < width; x += 2) {
      int a = s[3] >> 4;
      int b = s[7] >> 4;
      *d = (a << 4) | b;
      s += 8;
      d++;
    }
  }

  assert(d == l4 + outsize);

  write_payload(l4, outsize);
}




int
main(int argc, char **argv)
{
  int width, height, channels;
  int format;

  if(argc < 4) {
    fprintf(stderr, "Usage %s <INFILE> <FMT> <OUTFILE>\n", argv[0]);
    exit(1);
  }

  const char *fmt = argv[2];

  if(!strcmp(fmt, "l4")) {
    format = EVE_FORMAT_L4;
  } else {
    fprintf(stderr, "Unknown format %s\n", fmt);
  }

  uint8_t* img = stbi_load(argv[1], &width, &height, &channels, 4);
  if(img == NULL) {
    fprintf(stderr, "Failed to load %s\n", argv[1]);
    exit(1);
  }

  if(!strcmp(argv[3], "-")) {
    g_fd = 1;
  } else {
    g_fd = open(argv[3], O_RDWR | O_CREAT | O_TRUNC, 0666);
    if(g_fd == -1) {
      fprintf(stderr, "Open %s -- %m\n", argv[3]);
      exit(1);
    }
  }

  if(argc == 5) {
    dprintf(g_fd, "static const unsigned char %s[] = {\n  ",
            argv[4]);
    output = output_bytes_bin2c;
  } else {
    output = output_bytes_raw;
  }

  deflateInit(&zs, 9);


  if(format == EVE_FORMAT_L4) {
    output_to_l4(argv[3], width, height, img);
  } else {
    abort();
  }
  while(do_deflate(Z_FINISH) != Z_STREAM_END) {};

  if(argc == 5) {
    dprintf(g_fd, "\n};\n");
  }

  return 0;
}
