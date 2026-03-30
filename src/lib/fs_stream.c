#include <unistd.h>
#include <malloc.h>
#include <stdlib.h>

#include <mios/fs.h>
#include <mios/stream.h>

typedef struct {
  stream_t stream;
  fs_file_t *file;
} fs_stream_t;


static ssize_t
fs_stream_write(stream_t *s, const void *buf, size_t size, int flags)
{
  fs_stream_t *fs = (fs_stream_t *)s;
  if(buf == NULL)
    return 0; // Flush
  return fs_write(fs->file, buf, size);
}


static ssize_t
fs_stream_read(stream_t *s, void *buf, size_t size, size_t required)
{
  fs_stream_t *fs = (fs_stream_t *)s;
  return fs_read(fs->file, buf, size);
}


static void
fs_stream_close(stream_t *s)
{
  fs_stream_t *fs = (fs_stream_t *)s;
  fs_close(fs->file);
  free(fs);
}


static const stream_vtable_t fs_stream_vtable = {
  .read = fs_stream_read,
  .write = fs_stream_write,
  .close = fs_stream_close,
};


stream_t *
fs_stream_open(const char *path, int flags)
{
  fs_file_t *fp;
  error_t err = fs_open(path, flags, &fp);
  if(err)
    return NULL;

  fs_stream_t *fs = xalloc(sizeof(fs_stream_t), 0, MEM_MAY_FAIL);
  if(fs == NULL) {
    fs_close(fp);
    return NULL;
  }

  fs->stream.vtable = &fs_stream_vtable;
  fs->file = fp;
  return &fs->stream;
}
