#include "http_util.h"
#include <assert.h>

/*



  if(hc->hc_num_header_callbacks)
    return 0;

  if(hc->hc_header_match_len == 0) {
    hc->hc_header_match_mask = (1 << hc->hc_num_header_callbacks) - 1;
  }

  if(hc->hc_header_match_len == 255)
    return 0;

  for(size_t i = 0; i < length; i++) {
    char c = at[i];
    if(c == 0)
      return 1;
    if(c >= 'A' && c <= 'Z')
      c += 32;
    for(size_t j = 0; j < hc->hc_num_header_callbacks; j++) {
      match_header(hc, c, j);
    }
    hc->hc_header_match_len++;
  }
  return 0;
*/

static void
match_header(http_header_matcher_t *hhm, char c, size_t j,
             const http_header_callback_t *callbacks)
{
  uint32_t mask = (1 << j);
  if(!(hhm->hhm_mask & mask))
    return;

  const http_header_callback_t *hhc = callbacks + j;
  if(hhc->name[hhm->hhm_len] != c)
    hhm->hhm_mask &= ~mask;
}



int
http_match_header_field(http_header_matcher_t *hhm,
                        const char *str, size_t len,
                        const http_header_callback_t *callbacks,
                        size_t num_callbacks)
{
  if(num_callbacks == 0)
    return 0;

  if(hhm->hhm_len == 0) {
    hhm->hhm_mask = (1 << num_callbacks) - 1;
  }

  if(hhm->hhm_len == 255)
    return 0;

  for(size_t i = 0; i < len; i++) {
    char c = str[i];
    if(c == 0) // TBD: Can this happen from http_parser.c ?
      return 1;
    if(c >= 'A' && c <= 'Z')
      c += 32;
    for(size_t j = 0; j < num_callbacks; j++) {
      match_header(hhm, c, j, callbacks);
    }
    hhm->hhm_len++;
  }
  return 0;
}

int
http_match_header_value(http_header_matcher_t *hhm,
                        const char *str, size_t len,
                        const http_header_callback_t *callbacks,
                        size_t num_callbacks, void *opaque)
{
  hhm->hhm_len = 0;
  if (hhm->hhm_mask == 0) {
    return 0;
  }
  unsigned int which = __builtin_clz(hhm->hhm_mask);
  if(which == 32) 
    return 0;
  which = 31 - which;
  assert(which < num_callbacks);
  return callbacks[which].cb(opaque, str, len);
}
