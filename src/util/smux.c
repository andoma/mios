#include <mios/smux.h>

#include <mios/stream.h>
#include <mios/poll.h>
#include <mios/task.h>

#include <stdlib.h>

// Serial multiplexer

typedef struct smux {

  stream_t *muxed;

  mutex_t tx_mutex;

  pollset_t *ps;

  uint32_t write_flags;

  int16_t current;

  uint8_t delimiter;
  uint8_t num_channels;
  uint8_t reset_token;

  uint8_t channelids[0];
} smux_t;


static void
smux_tx(smux_t *smux, const void *buf, size_t len, uint8_t channel)
{
  uint8_t cmd[2] = {smux->delimiter, channel};
  mutex_lock(&smux->tx_mutex);
  if(channel != smux->current) {
    smux->current = channel;
    stream_write(smux->muxed, cmd, 2, 0);
  }

  const uint8_t *u8 = buf;
  size_t i;

  for(i = 0; i < len; i++) {
    if(u8[i] != smux->delimiter)
      continue;

    if(i)
      stream_write(smux->muxed, u8, i, 0);

    cmd[1] = smux->delimiter;
    stream_write(smux->muxed, cmd, 2, 0);

    len -= i;
    u8 += i;
    i = 0;
  }

  if(i)
    stream_write(smux->muxed, u8, i, 0);
  mutex_unlock(&smux->tx_mutex);
}


__attribute__((noreturn))
static void *
smux_tx_thread(void *arg)
{
  smux_t *smux = arg;
  pollset_t *ps = smux->ps;
  uint8_t buf[16];

  while(1) {
    int which = poll(ps, smux->num_channels, NULL, INT64_MAX);
    int r = stream_read(ps[which].obj, buf, sizeof(buf), 0);
    if(r)
      smux_tx(smux, buf, r, smux->channelids[which]);
  }
}


static void
smux_tx_reset(smux_t *smux)
{
  uint8_t buf[2] = {smux->delimiter, smux->reset_token};

  mutex_lock(&smux->tx_mutex);
  stream_write(smux->muxed, buf, 2, 0);
  mutex_unlock(&smux->tx_mutex);
}


static stream_t *
smux_find_channel(smux_t *smux, uint8_t id)
{
  for(size_t i = 0; i < smux->num_channels; i++) {
    if(smux->channelids[i] == id)
      return smux->ps[i].obj;
  }
  return NULL;
}


__attribute__((noreturn))
static void *
smux_rx_thread(void *arg)
{
  smux_t *smux = arg;

  stream_t *channel = smux->ps[0].obj;
  int esc = 0;
  uint8_t buf[32];

  smux_tx_reset(smux);

  while(1) {

    int r = stream_read(smux->muxed, buf, sizeof(buf), 1);
    size_t j = 0;

    for(size_t i = 0; i < r; i++) {

      if(esc) {
        esc = 0;
        if(buf[i] != smux->delimiter) {

          if(channel) {
            stream_write(channel, buf, j, smux->write_flags);
            j = 0;
          }

          if(buf[i] == smux->reset_token) {
            // Received reset
            smux->current = -1;
            continue;
          }

          channel = smux_find_channel(smux, buf[i]);
          if(channel == NULL) {
            smux_tx_reset(smux);
          }
          continue;
        }
        // two delimiters back-to-back is the delimiter byte verbatim
        // so just fall thru

      } else if(buf[i] == smux->delimiter) {
        esc = 1;
        continue;
      }

      if(channel == NULL) {
        continue;
      }
      buf[j++] = buf[i];
    }

    if(channel) {
      stream_write(channel, buf, j, smux->write_flags);
    }
  }
}

void
smux_create(stream_t *muxed, uint8_t delimiter, uint8_t reset_token,
            size_t count, const uint8_t *idvec,
            stream_t **streamvec, int write_flags)
{
  smux_t *smux = calloc(1, sizeof(smux_t) + count);
  smux->muxed = muxed;
  mutex_init(&smux->tx_mutex, "smux");
  smux->delimiter = delimiter;
  smux->reset_token = reset_token;
  smux->current = -1;
  smux->num_channels = count;
  smux->write_flags = write_flags;

  smux->ps = calloc(count, sizeof(pollset_t));
  pollset_t *ps = smux->ps;

  for(size_t i = 0; i < count; i++) {
    smux->channelids[i] = idvec[i];
    ps[i].obj = streamvec[i];

    // Only poll if channel have a read() interface
    if(streamvec[i]->vtable->read != NULL) {
      ps[i].type = POLL_STREAM_READ;
    } else {
      _Static_assert(POLL_NONE == 0);
      // ps[i].type = POLL_NONE;
    }
  }

  thread_create(smux_rx_thread, smux, 1024, "smux_rx", TASK_DETACHED, 4);
  thread_create(smux_tx_thread, smux, 1024, "smux_tx", TASK_DETACHED, 4);
}

