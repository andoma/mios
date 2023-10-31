#pragma once

typedef struct websocket_parser {

  uint8_t wp_header[14];
  uint8_t wp_header_len;

  size_t wp_fragment_used;

} websocket_parser_t;



#define WS_OPCODE_TEXT   1
#define WS_OPCODE_BINARY 2
#define WS_OPCODE_CLOSE  8
#define WS_OPCODE_PING   9
#define WS_OPCODE_PONG   10

#define WS_STATUS_NORMAL_CLOSE      1000
#define WS_STATUS_GOING_AWAY        1001
#define WS_STATUS_PROTOCOL_ERROR    1002
#define WS_STATUS_CANNOT_ACCEPT     1003

#define WS_STATUS_NO_STATUS         1005
#define WS_STATUS_ABNORMALLY_CLOSED 1006

#define WS_STATUS_TOO_BIG           1009
