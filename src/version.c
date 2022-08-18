#include <mios/version.h>

#include <stdio.h>
#include <stdint.h>

#include "version_git.h"

char _appname[] __attribute__((section("appname"))) = APPNAME;

const char *
mios_get_version(void)
{
  return VERSION_MIOS_GIT;
}

const char *
mios_get_app_version(void)
{
  return VERSION_APP_GIT;
}

const char *
mios_get_app_name(void)
{
  return _appname;
}

void
mios_print_version(stream_t *s)
{
  if(APPNAME[0]) {
    stprintf(s, "%s (%s) on ", APPNAME, VERSION_APP_GIT);
  }
  const unsigned char *bid = mios_build_id();

  stprintf(s, "Mios (%s) Build: [%02x%02x%02x%02x]\n",
           VERSION_MIOS_GIT,
           bid[0], bid[1], bid[2], bid[3]);
}

typedef struct {
  uint32_t namesz;
  uint32_t descsz;
  uint32_t type;
  uint8_t data[];
} ElfNoteSection_t;

extern ElfNoteSection_t _build_id;

const unsigned char *
mios_build_id(void)
{
  return &_build_id.data[_build_id.namesz];
}
