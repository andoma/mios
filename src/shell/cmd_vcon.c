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

    // Idle: sleep until there is output or the user types something.
    if(!progress)
      vcon_client_wait(vcc);
  }

  vcon_detach(vcc);
  cli_printf(cli, "\n[detached from %s]\n", argv[1]);
  return 0;
}

CLI_CMD_DEF_EXT("attach", cmd_attach, "<console>",
                "Attach terminal to a virtual console");
