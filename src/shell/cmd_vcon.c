#include <mios/cli.h>
#include <mios/vcon.h>
#include <mios/stream.h>

// Virtual console shell commands.
//
//   consoles          list registered virtual consoles
//   attach <name>     bind this terminal to a console; replays scrollback and
//                     pumps keystrokes until the detach sequence.
//
// Detach sequence (screen-style), handled here on cli->cl_stream:
//   ^A d   detach and return to the shell
//   ^A ^A  send a literal ^A to the console
// (^A rather than tmux's ^B, which many host terminal emulators bind to detach.)
//
// Nesting: attach to a unit, then attach to something else from that
// unit's shell, and two of these loops are in series on one byte stream.
// The outer one is the first filter a byte meets, so it always wins --
// ^A d detaches the outermost session from any depth, which is the
// property you want from the key you reach for when something is wrong.
// Addressing an inner level means doubling the prefix for every level
// above it, so it takes 2^(N-1) of them to reach level N:
//
//   ^A d              detach the outermost
//   ^A ^A d           detach one level in
//   ^A ^A ^A ^A d     two levels in
//
// Note that the count is a power of two, not a running total: an odd
// number above one just arms a prefix on some inner level and then
// detaches an outer one. Deliberately left as-is rather than made to
// prefer the innermost, which cannot be done without the outer loop
// knowing whether an inner one exists -- and the only channel it could
// learn that on is the console output itself.
//
// Detaching an outer session does not tear down the inner ones; they stay
// attached, and re-attaching puts you back inside them.

#define VCON_PREFIX 0x01 // Ctrl-A


static void
vcon_list(cli_t *cli)
{
  for(vcon_t *vc = vcon_first(); vc != NULL; vc = vcon_next(vc)) {
    cli_printf(cli, "  %-10s  %6u bytes scrollback  %d client(s)\n",
               vcon_name(vc),
               (unsigned)vcon_scrollback_used(vc),
               vcon_client_count(vc));
  }
}


static error_t
cmd_consoles(cli_t *cli, int argc, char **argv)
{
  vcon_list(cli);
  return 0;
}

CLI_CMD_DEF_EXT("consoles", cmd_consoles, NULL,
                "List virtual consoles");


static error_t
cmd_attach(cli_t *cli, int argc, char **argv)
{
  if(argc != 2) {
    cli_printf(cli, "Available consoles:\n");
    vcon_list(cli);
    return 0;
  }

  vcon_t *vc = vcon_find(argv[1]);
  if(vc == NULL) {
    cli_printf(cli, "No such console: %s\n", argv[1]);
    vcon_list(cli);
    return ERR_NOT_FOUND;
  }

  stream_t *term = cli->cl_stream;

  cli_printf(cli, "[attached to %s -- '^A d' to detach]\n", argv[1]);

  vcon_client_t *vcc = vcon_attach(vc, term);
  if(vcc == NULL)
    return ERR_NO_MEMORY;

  uint8_t buf[64];
  int prefix = 0;
  int detach = 0;

  while(!detach) {
    int progress = 0;

    // Service input first, so the detach key stays responsive even while the
    // console is flooding output. Non-blocking; buf is reused for output below
    // only after these bytes are consumed.
    ssize_t r = stream_read(term, buf, sizeof(buf), 0);
    if(r < 0)
      break;  // The terminal went away (a dropped link, say). Without
              // this the loop makes no progress, sleeps on a stream that
              // is permanently ready, and spins forever.
    for(ssize_t i = 0; i < r && !detach; i++) {
      uint8_t c = buf[i];

      if(prefix) {
        prefix = 0;
        if(c == 'd' || c == 'D') {
          detach = 1;
          break;
        }
        // ^A ^A -> literal ^A; anything else -> deliver the byte as-is
        vcon_input(vc, &c, 1);
        continue;
      }

      if(c == VCON_PREFIX) {
        prefix = 1;
        continue;
      }

      vcon_input(vc, &c, 1);
    }
    if(r > 0)
      progress = 1;
    if(detach)
      break;

    // Drain one chunk of pending console output (blocking write, so the full
    // scrollback and live stream get through; not truncated like NO_WAIT).
    size_t n = vcon_client_output(vcc, buf, sizeof(buf));
    if(n) {
      stream_write(term, buf, n, 0);
      progress = 1;
    }

    // Idle: flush, then sleep until there is output or the user types
    // something. The flush matters when the terminal buffers -- a pushpull
    // stream (any shell reached over VLLP, BLE or MBUS) only hands data to
    // the network once a fragment fills or someone flushes, so without
    // this a nested session's output sits in the buffer indefinitely and
    // the console looks dead. Doing it here rather than after every write
    // means a burst still coalesces into full fragments.
    if(!progress) {
      stream_flush(term);
      vcon_client_wait(vcc);
    }
  }

  vcon_detach(vcc);
  cli_printf(cli, "\n[detached from %s]\n", argv[1]);
  return 0;
}

CLI_CMD_DEF_EXT("attach", cmd_attach, "<console>",
                "Attach terminal to a virtual console");
