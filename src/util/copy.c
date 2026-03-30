#include <mios/copy.h>
#include <mios/cli.h>

#include <string.h>

extern unsigned long _copyhandler_array_begin;
extern unsigned long _copyhandler_array_end;

#define HANDLER_BEGIN ((const copy_handler_t *)&_copyhandler_array_begin)
#define HANDLER_END   ((const copy_handler_t *)&_copyhandler_array_end)


static const copy_handler_t *
find_handler(const char *url, int need_write)
{
  for(const copy_handler_t *h = HANDLER_BEGIN; h != HANDLER_END; h++) {
    if(h->prefix == NULL)
      return h; // Fallback handler (filesystem)
    if(!strncmp(url, h->prefix, strlen(h->prefix))) {
      if(need_write && h->open_write == NULL)
        continue;
      if(!need_write && h->read_to == NULL)
        continue;
      return h;
    }
  }
  return NULL;
}


static error_t
cmd_copy(cli_t *cli, int argc, char **argv)
{
  if(argc != 3)
    return ERR_INVALID_ARGS;

  const char *src = argv[1];
  const char *dst = argv[2];

  // Find destination handler and open writable stream
  const copy_handler_t *dst_handler = find_handler(dst, 1);
  if(dst_handler == NULL) {
    cli_printf(cli, "No handler for destination: %s\n", dst);
    return ERR_OPERATION_FAILED;
  }

  stream_t *sink = dst_handler->open_write(dst);
  if(sink == NULL) {
    cli_printf(cli, "Failed to open destination: %s\n", dst);
    return ERR_OPERATION_FAILED;
  }

  // Find source handler and push data to sink
  const copy_handler_t *src_handler = find_handler(src, 0);
  if(src_handler == NULL) {
    cli_printf(cli, "No handler for source: %s\n", src);
    stream_close(sink);
    return ERR_OPERATION_FAILED;
  }

  error_t err = src_handler->read_to(src, sink);

  if(err)
    cli_printf(cli, "Source '%s' failed: %s (%d)\n", src,
               error_to_string(err), err);

  stream_close(sink);

  return err;
}

CLI_CMD_DEF("copy", cmd_copy);
