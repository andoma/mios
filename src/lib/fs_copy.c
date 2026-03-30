#include <mios/copy.h>
#include <mios/fs.h>

#include <malloc.h>

stream_t *fs_stream_open(const char *path, int flags);


static stream_t *
fs_copy_open_write(const char *url)
{
  return fs_stream_open(url, FS_WRONLY | FS_CREAT | FS_TRUNC);
}


static error_t
fs_copy_read_to(const char *url, stream_t *output)
{
  fs_file_t *fp;
  error_t err = fs_open(url, FS_RDONLY, &fp);
  if(err)
    return err;

  uint8_t buf[128];
  while(1) {
    ssize_t r = fs_read(fp, buf, sizeof(buf));
    if(r <= 0)
      break;
    ssize_t w = stream_write(output, buf, r, 0);
    if(w < 0) {
      err = w;
      break;
    }
  }

  stream_write(output, NULL, 0, 0); // Flush
  fs_close(fp);
  return err;
}


// Priority 9 = last resort (filesystem fallback, no prefix match)
COPY_HANDLER_DEF(filesystem, 9,
  .prefix = NULL,
  .open_write = fs_copy_open_write,
  .read_to = fs_copy_read_to,
);
