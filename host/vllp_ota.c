#include "vllp_ota.h"

#include "mios_image.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <syslog.h>

#include "vllp.h"

static const char *
vllp_do_ota(vllp_t *v, const char *elfpath, vllp_channel_t *vc)
{
  int result;

  void *buf;
  size_t hdrsize;

  result = vllp_channel_read(vc, &buf, &hdrsize, 250000);
  if(result)
    return vllp_strerror(result);
  if(buf == NULL)
    return "Connection closed prematurely";

  uint8_t *hdr = alloca(hdrsize + 1);
  memcpy(hdr, buf, hdrsize);
  hdr[hdrsize] = 0;
  free(buf);

  if(hdrsize < 4 + 20 + 1)
    return "Initial running-info message is too short";

  if(hdr[0] != 0 || hdr[1] != 'r') {
    return "Unsupported OTA protocol/version";
  }

  // Null-terminated by alloca dance above
  const char *running_app = (const char *)hdr + 24;

  vllp_logf(v, LOG_INFO,
            "OTA: Connected. Running app: \"%s\" [buildid: %02x%02x%02x%02x...]",
            running_app,
            hdr[4],
            hdr[5],
            hdr[6],
            hdr[7]);

  int blocksize = hdr[2];
  int xferskip = hdr[3] * 1024;

  const char *errmsg;
  mios_image_t *mi = mios_image_from_elf_file(elfpath, xferskip, blocksize,
                                              &errmsg);
  if(mi == NULL)
    return errmsg;

  vllp_logf(v, LOG_INFO,
            "OTA: Loaded app: \"%s\" [buildid: %02x%02x%02x%02x...]",
            mi->appname,
            mi->buildid[0],
            mi->buildid[1],
            mi->buildid[2],
            mi->buildid[3]);

  if(strcmp(mi->appname, mi->appname)) {
    vllp_logf(v, LOG_ERR,
              "OTA: Mismatching appname (running:\"%s\", loaded:\"%s\")",
              running_app, mi->appname);
    free(mi);
    return "Mismatching appname";
  }

  if(!memcmp(hdr + 4, mi->buildid, sizeof(mi->buildid))) {
    vllp_logf(v, LOG_DEBUG, "OTA: build-id match. Nothing to do");
    free(mi);
    return NULL;
  }

  int num_blocks = mi->image_size / blocksize;
  uint32_t imghdr[2];
  imghdr[0] = num_blocks;
  imghdr[1] = ~vllp_crc32(0, mi->image, num_blocks * blocksize);

  vllp_logf(v, LOG_DEBUG, "OTA: Transfer start, %d bytes", mi->image_size);

  vllp_channel_send(vc, imghdr, sizeof(imghdr));

  for(int i = 0; i < num_blocks; i++) {
    vllp_channel_send(vc, mi->image + i * blocksize, blocksize);
  }
  free(mi);
  mi = NULL;

  vllp_logf(v, LOG_DEBUG, "OTA: Waiting for response");

  size_t finsize;
  result = vllp_channel_read(vc, &buf, &finsize, 10*1000*1000);
  if(result)
    return vllp_strerror(result);
  if(buf == NULL)
    return "Connection closed prematurely";
  if(finsize != 1) {
    free(buf);
    return "Final status has invalid size";
  }
  uint8_t fin;
  memcpy(&fin, buf, 1);
  free(buf);

  vllp_logf(v, LOG_DEBUG, "OTA: Got response, code:%d", fin);

  if(fin == 0)
    return NULL;

  return vllp_strerror(-(int)fin);

}


const char *
vllp_ota(struct vllp *v, const char *elfpath)
{
  vllp_channel_t *vc = vllp_channel_create(v, "ota", 0, NULL, NULL, NULL);

  const char *errstr = vllp_do_ota(v, elfpath, vc);

  vllp_channel_close(vc, 0, 1);
  return errstr;
}
