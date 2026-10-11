/*
 * camview: minimal low latency viewer for H.264 over RTP (RFC 6184)
 *
 * Listens on a UDP port, reassembles access units (single NAL unit,
 * STAP-A and FU-A packets), decodes each one as soon as its last packet
 * arrives and shows it right away. There is no jitter buffer and no
 * clock: frames are displayed in arrival order. A new SSRC, or a large
 * sequence number jump, resets the receiver so a restarted stream is
 * picked up immediately.
 *
 * The overlay marks intra coded macroblocks, using the motion vectors
 * exported by libavcodec (every inter macroblock, including skipped ones,
 * has at least one motion vector, intra macroblocks have none).
 *
 * Packet loss can be simulated with keys 1-5 (drop that many percent of
 * the received packets at random), 0 turns it off.
 *
 * Keys: o overlay, v motion vectors, g macroblock grid, f fullscreen,
 *       0-5 simulated packet loss (%), q / Esc quit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <libavcodec/avcodec.h>
#include <libavutil/motion_vector.h>
#include <SDL.h>

#define MAX_AU_SIZE (4 * 1024 * 1024)

// Sequence number jumps larger than this are treated as a new stream
#define MAX_SEQ_GAP 1000

typedef struct {
  // RTP state
  int have_ssrc;
  uint32_t ssrc;
  uint16_t next_seq;
  uint32_t au_ts;
  int au_lost;           // Packet loss within the current access unit
  int au_slices;         // Slice NAL units in the current access unit
  int fu_active;

  uint8_t *au;
  size_t au_len;

  // Decoder
  AVCodecContext *dec;
  AVPacket *pkt;
  AVFrame *frame;

  // Display
  SDL_Window *window;
  SDL_Renderer *renderer;
  SDL_Texture *texture;
  int width;
  int height;
  int overlay;
  int vectors;
  int grid;
  int fullscreen;
  int drop_pct;          // Simulated packet loss

  uint8_t *mb_intra;     // Per macroblock: 1 if intra in the current frame
  int mb_w;
  int mb_h;

  // Statistics, per second
  uint64_t stat_start;
  unsigned packets;
  unsigned lost;
  unsigned dropped;      // Simulated loss
  unsigned bytes;
  unsigned frames;
  unsigned slices;
  unsigned iframes;
  unsigned pframes;
  unsigned intra_mbs;
  unsigned total_mbs;
  int64_t delay_min;     // Arrival time minus RTP time, microseconds
  int64_t delay_max;
  int64_t delay_base;    // Lowest offset seen in this stream
  unsigned streams;
} camview_t;


static uint64_t
now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}


static void
au_append(camview_t *cv, const uint8_t *data, size_t len)
{
  if(cv->au_len + len > MAX_AU_SIZE) {
    cv->au_lost = 1;
    return;
  }
  memcpy(cv->au + cv->au_len, data, len);
  cv->au_len += len;
}


static void
au_start_nal(camview_t *cv, uint8_t nal_header)
{
  static const uint8_t sc[4] = { 0, 0, 0, 1 };
  const int type = nal_header & 0x1f;
  if(type == 1 || type == 5)
    cv->au_slices++;
  au_append(cv, sc, sizeof(sc));
}


static void
au_reset(camview_t *cv)
{
  cv->au_len = 0;
  cv->au_lost = 0;
  cv->au_slices = 0;
  cv->fu_active = 0;
}


static void
stream_reset(camview_t *cv, uint32_t ssrc, uint16_t seq)
{
  if(cv->have_ssrc)
    printf("New stream (SSRC 0x%08x, seq %u)\n", ssrc, seq);
  else
    printf("Stream started (SSRC 0x%08x, seq %u)\n", ssrc, seq);
  cv->have_ssrc = 1;
  cv->ssrc = ssrc;
  cv->next_seq = seq;
  cv->delay_base = INT64_MAX;
  cv->streams++;
  au_reset(cv);
  avcodec_flush_buffers(cv->dec);
}


static void
mark_mbs(camview_t *cv, const AVFrame *f)
{
  const int mb_w = (f->width + 15) / 16;
  const int mb_h = (f->height + 15) / 16;
  if(mb_w != cv->mb_w || mb_h != cv->mb_h) {
    free(cv->mb_intra);
    cv->mb_intra = calloc(mb_w * mb_h, 1);
    cv->mb_w = mb_w;
    cv->mb_h = mb_h;
  }

  // Everything is intra unless an inter partition covers it
  memset(cv->mb_intra, 1, mb_w * mb_h);

  const AVFrameSideData *sd =
    av_frame_get_side_data(f, AV_FRAME_DATA_MOTION_VECTORS);
  if(sd != NULL) {
    const AVMotionVector *mvs = (const AVMotionVector *)sd->data;
    const int n = sd->size / sizeof(AVMotionVector);
    for(int i = 0; i < n; i++) {
      const AVMotionVector *mv = &mvs[i];
      const int x0 = (mv->dst_x - mv->w / 2) / 16;
      const int y0 = (mv->dst_y - mv->h / 2) / 16;
      const int x1 = (mv->dst_x + mv->w / 2 - 1) / 16;
      const int y1 = (mv->dst_y + mv->h / 2 - 1) / 16;
      for(int y = y0; y <= y1; y++) {
        for(int x = x0; x <= x1; x++) {
          if(x >= 0 && y >= 0 && x < mb_w && y < mb_h)
            cv->mb_intra[y * mb_w + x] = 0;
        }
      }
    }
  }

  for(int i = 0; i < mb_w * mb_h; i++)
    cv->intra_mbs += cv->mb_intra[i];
  cv->total_mbs += mb_w * mb_h;
}


static void
draw_overlay(camview_t *cv, const AVFrame *f)
{
  SDL_Renderer *r = cv->renderer;
  SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

  if(cv->overlay && cv->mb_intra != NULL) {
    SDL_SetRenderDrawColor(r, 255, 40, 40, 90);
    for(int y = 0; y < cv->mb_h; y++) {
      for(int x = 0; x < cv->mb_w; x++) {
        if(cv->mb_intra[y * cv->mb_w + x]) {
          SDL_Rect rc = { x * 16, y * 16, 16, 16 };
          SDL_RenderFillRect(r, &rc);
        }
      }
    }
  }

  if(cv->grid) {
    SDL_SetRenderDrawColor(r, 255, 255, 255, 40);
    for(int x = 16; x < cv->width; x += 16)
      SDL_RenderDrawLine(r, x, 0, x, cv->height);
    for(int y = 16; y < cv->height; y += 16)
      SDL_RenderDrawLine(r, 0, y, cv->width, y);
  }

  if(cv->vectors) {
    const AVFrameSideData *sd =
      av_frame_get_side_data(f, AV_FRAME_DATA_MOTION_VECTORS);
    if(sd != NULL) {
      const AVMotionVector *mvs = (const AVMotionVector *)sd->data;
      const int n = sd->size / sizeof(AVMotionVector);
      SDL_SetRenderDrawColor(r, 40, 255, 40, 200);
      for(int i = 0; i < n; i++) {
        const AVMotionVector *mv = &mvs[i];
        if(mv->src_x == mv->dst_x && mv->src_y == mv->dst_y)
          continue;
        SDL_RenderDrawLine(r, mv->src_x, mv->src_y, mv->dst_x, mv->dst_y);
        SDL_RenderDrawPoint(r, mv->dst_x, mv->dst_y);
      }
    }
  }
}


static void
display(camview_t *cv, const AVFrame *f)
{
  if(f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
    fprintf(stderr, "Unsupported pixel format %d\n", f->format);
    return;
  }

  if(cv->texture == NULL || f->width != cv->width || f->height != cv->height) {
    if(cv->texture)
      SDL_DestroyTexture(cv->texture);
    cv->texture = SDL_CreateTexture(cv->renderer, SDL_PIXELFORMAT_IYUV,
                                    SDL_TEXTUREACCESS_STREAMING,
                                    f->width, f->height);
    cv->width = f->width;
    cv->height = f->height;
    // Draw everything (including the overlay) in frame coordinates
    SDL_RenderSetLogicalSize(cv->renderer, f->width, f->height);
    if(!cv->fullscreen)
      SDL_SetWindowSize(cv->window, f->width, f->height);
    printf("Video %dx%d\n", f->width, f->height);
  }

  SDL_UpdateYUVTexture(cv->texture, NULL,
                       f->data[0], f->linesize[0],
                       f->data[1], f->linesize[1],
                       f->data[2], f->linesize[2]);
  SDL_RenderClear(cv->renderer);
  SDL_RenderCopy(cv->renderer, cv->texture, NULL, NULL);
  draw_overlay(cv, f);
  SDL_RenderPresent(cv->renderer);
}


static void
decode_au(camview_t *cv)
{
  if(cv->au_len == 0)
    return;

  cv->slices += cv->au_slices;
  cv->pkt->data = cv->au;
  cv->pkt->size = cv->au_len;
  int err = avcodec_send_packet(cv->dec, cv->pkt);
  au_reset(cv);
  if(err < 0 && err != AVERROR(EAGAIN))
    return;

  while(avcodec_receive_frame(cv->dec, cv->frame) == 0) {
    cv->frames++;
    if(cv->frame->pict_type == AV_PICTURE_TYPE_I)
      cv->iframes++;
    else
      cv->pframes++;
    mark_mbs(cv, cv->frame);
    display(cv, cv->frame);
    av_frame_unref(cv->frame);
  }
}


static void
handle_rtp(camview_t *cv, const uint8_t *p, size_t len)
{
  if(len < 12 || (p[0] >> 6) != 2)
    return;

  const int padding = p[0] & 0x20;
  const int ext = p[0] & 0x10;
  const int cc = p[0] & 0x0f;
  const int marker = p[1] & 0x80;
  const uint16_t seq = (p[2] << 8) | p[3];
  const uint32_t ts = (p[4] << 24) | (p[5] << 16) | (p[6] << 8) | p[7];
  const uint32_t ssrc = (p[8] << 24) | (p[9] << 16) | (p[10] << 8) | p[11];

  size_t off = 12 + cc * 4;
  if(ext) {
    if(off + 4 > len)
      return;
    off += 4 + ((p[off + 2] << 8) | p[off + 3]) * 4;
  }
  if(padding && len > 0)
    len -= p[len - 1];
  if(off >= len)
    return;

  cv->packets++;
  cv->bytes += len;

  if(!cv->have_ssrc || ssrc != cv->ssrc) {
    stream_reset(cv, ssrc, seq);
  } else {
    const uint16_t gap = seq - cv->next_seq;
    if(gap >= 0x8000) {
      // Behind the expected sequence number: a reordered or duplicated
      // packet, unless it's way off, then the sender restarted
      if((uint16_t)(cv->next_seq - seq) < MAX_SEQ_GAP)
        return;
      stream_reset(cv, ssrc, seq);
    } else if(gap > MAX_SEQ_GAP) {
      stream_reset(cv, ssrc, seq);
    } else if(gap) {
      cv->lost += gap;
      cv->au_lost = 1;
      cv->fu_active = 0;
    }
  }
  cv->next_seq = seq + 1;

  // A new timestamp means a new access unit, even if we never saw the
  // marker bit of the previous one
  if(cv->au_len && ts != cv->au_ts)
    decode_au(cv);
  cv->au_ts = ts;

  const uint8_t *pl = p + off;
  const size_t plen = len - off;
  const int type = pl[0] & 0x1f;

  if(type >= 1 && type <= 23) {
    au_start_nal(cv, pl[0]);
    au_append(cv, pl, plen);
  } else if(type == 24) {
    // STAP-A: 16 bit size + NAL unit, repeated
    size_t i = 1;
    while(i + 2 <= plen) {
      const size_t n = (pl[i] << 8) | pl[i + 1];
      i += 2;
      if(i + n > plen)
        break;
      au_start_nal(cv, pl[i]);
      au_append(cv, pl + i, n);
      i += n;
    }
  } else if(type == 28 && plen >= 2) {
    // FU-A
    const int start = pl[1] & 0x80;
    if(start) {
      const uint8_t hdr = (pl[0] & 0xe0) | (pl[1] & 0x1f);
      au_start_nal(cv, hdr);
      au_append(cv, &hdr, 1);
      cv->fu_active = 1;
    }
    if(cv->fu_active)
      au_append(cv, pl + 2, plen - 2);
    if(pl[1] & 0x40)
      cv->fu_active = 0;
  }

  if(marker) {
    // Arrival time relative to the RTP clock. The lowest value seen is
    // the fastest the network delivered a frame, anything above it is
    // queueing/jitter.
    const int64_t offset = (int64_t)now_us() - (int64_t)ts * 100 / 9;
    if(offset < cv->delay_base)
      cv->delay_base = offset;
    const int64_t d = offset - cv->delay_base;
    if(d < cv->delay_min)
      cv->delay_min = d;
    if(d > cv->delay_max)
      cv->delay_max = d;
    decode_au(cv);
  }
}


static void
update_stats(camview_t *cv)
{
  const uint64_t now = now_us();
  const uint64_t elapsed = now - cv->stat_start;
  if(elapsed < 1000000)
    return;

  char drop[64] = "";
  if(cv->drop_pct)
    snprintf(drop, sizeof(drop), "  [dropping %d%%: %u packets]",
             cv->drop_pct, cv->dropped);

  char title[320];
  if(cv->frames) {
    snprintf(title, sizeof(title),
             "camview %dx%d  %.1f fps  %u kbit/s  I:%u P:%u  slices:%.1f  "
             "intra MBs:%.1f%%  loss:%u  jitter:%lld-%lld ms%s",
             cv->width, cv->height, cv->frames * 1e6 / elapsed,
             (unsigned)((uint64_t)cv->bytes * 8000 / elapsed),
             cv->iframes, cv->pframes, (double)cv->slices / cv->frames,
             cv->total_mbs ? 100.0 * cv->intra_mbs / cv->total_mbs : 0.0,
             cv->lost,
             (long long)(cv->delay_min / 1000),
             (long long)(cv->delay_max / 1000), drop);
  } else {
    snprintf(title, sizeof(title), "camview  waiting for stream");
  }
  SDL_SetWindowTitle(cv->window, title);
  if(cv->frames)
    printf("%s\n", title + strlen("camview "));

  cv->stat_start = now;
  cv->packets = cv->lost = cv->dropped = cv->bytes = 0;
  cv->frames = cv->iframes = cv->pframes = cv->slices = 0;
  cv->intra_mbs = cv->total_mbs = 0;
  cv->delay_min = INT64_MAX;
  cv->delay_max = 0;
}


static int
handle_events(camview_t *cv)
{
  SDL_Event ev;
  int redraw = 0;
  while(SDL_PollEvent(&ev)) {
    if(ev.type == SDL_QUIT)
      return -1;
    if(ev.type == SDL_KEYDOWN) {
      switch(ev.key.keysym.sym) {
      case SDLK_q:
      case SDLK_ESCAPE:
        return -1;
      case SDLK_o:
        cv->overlay = !cv->overlay;
        redraw = 1;
        break;
      case SDLK_v:
        cv->vectors = !cv->vectors;
        redraw = 1;
        break;
      case SDLK_g:
        cv->grid = !cv->grid;
        redraw = 1;
        break;
      case SDLK_0 ... SDLK_5:
        cv->drop_pct = ev.key.keysym.sym - SDLK_0;
        printf("Simulated packet loss: %d%%\n", cv->drop_pct);
        break;
      case SDLK_f:
        cv->fullscreen = !cv->fullscreen;
        SDL_SetWindowFullscreen(cv->window, cv->fullscreen ?
                                SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        break;
      }
    }
  }
  (void)redraw; // Applied with the next frame
  return 0;
}


static void
usage(const char *argv0)
{
  fprintf(stderr,
          "Usage: %s [-p port] [-o] [-d pct]\n"
          "  -p port   UDP port to listen on (default 5000)\n"
          "  -o        Start with the intra macroblock overlay on\n"
          "  -d pct    Simulate this much packet loss (percent)\n"
          "Keys: o overlay, v motion vectors, g grid, f fullscreen,\n"
          "      0-5 simulated packet loss (%%), q quit\n",
          argv0);
}


int
main(int argc, char **argv)
{
  int port = 5000;
  camview_t cv = {};
  int opt;

  setvbuf(stdout, NULL, _IOLBF, 0);
  srand(time(NULL));

  while((opt = getopt(argc, argv, "p:od:h")) != -1) {
    switch(opt) {
    case 'p':
      port = atoi(optarg);
      break;
    case 'o':
      cv.overlay = 1;
      break;
    case 'd':
      cv.drop_pct = atoi(optarg);
      break;
    default:
      usage(argv[0]);
      return 1;
    }
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if(fd < 0) {
    perror("socket");
    return 1;
  }
  int rcvbuf = 8 * 1024 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in sin = {
    .sin_family = AF_INET,
    .sin_port = htons(port),
    .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if(bind(fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
    perror("bind");
    return 1;
  }

  const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
  if(codec == NULL) {
    fprintf(stderr, "No H.264 decoder\n");
    return 1;
  }
  cv.dec = avcodec_alloc_context3(codec);
  // Output every frame as soon as it's decoded
  cv.dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
  cv.dec->thread_count = 1;
  cv.dec->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;
  if(avcodec_open2(cv.dec, codec, NULL) < 0) {
    fprintf(stderr, "Unable to open decoder\n");
    return 1;
  }
  cv.pkt = av_packet_alloc();
  cv.frame = av_frame_alloc();
  cv.au = malloc(MAX_AU_SIZE + AV_INPUT_BUFFER_PADDING_SIZE);
  memset(cv.au + MAX_AU_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

  if(SDL_Init(SDL_INIT_VIDEO) < 0) {
    fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  cv.window = SDL_CreateWindow("camview  waiting for stream",
                               SDL_WINDOWPOS_UNDEFINED,
                               SDL_WINDOWPOS_UNDEFINED, 640, 480,
                               SDL_WINDOW_RESIZABLE);
  // No vsync, present as soon as a frame is decoded
  cv.renderer = SDL_CreateRenderer(cv.window, -1, 0);
  if(cv.window == NULL || cv.renderer == NULL) {
    fprintf(stderr, "SDL: %s\n", SDL_GetError());
    return 1;
  }

  printf("Listening for RTP/H.264 on UDP port %d\n", port);

  cv.stat_start = now_us();
  cv.delay_min = INT64_MAX;
  cv.delay_base = INT64_MAX;

  static uint8_t buf[65536];
  while(1) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    poll(&pfd, 1, 10);

    while(1) {
      ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
      if(n <= 0)
        break;
      if(cv.drop_pct && rand() % 100 < cv.drop_pct) {
        cv.dropped++;
        continue;
      }
      handle_rtp(&cv, buf, n);
    }

    if(handle_events(&cv))
      break;
    update_stats(&cv);
  }

  SDL_Quit();
  avcodec_free_context(&cv.dec);
  return 0;
}
