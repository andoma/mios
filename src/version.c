#include <mios/version.h>
#include <mios/eventlog.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>

const char _appname[] __attribute__((section("appname"))) = APPNAME;
uint8_t _miosversion[21] __attribute__((section("miosversion")));
uint8_t _appversion[21] __attribute__((section("appversion")));

const char *
mios_get_app_name(void)
{
  return _appname;
}

const uint8_t *
mios_get_app_version(void)
{
  return _appversion;
}

static void
stprintversion(stream_t *s, const uint8_t *ver)
{
  uint8_t or = 0;
  for(size_t i = 0; i < 21; i++) {
    or |= ver[i];
  }
  if(or == 0) {
    stprintf(s, "no-version-info\n");
    return;
  }
  sthexstr(s, ver, 20);
  stprintf(s, "%s\n", ver[20] ? "-dirty" : "");
}


void
log_sysinfo(void)
{
  evlog(LOG_NOTICE, "System booted");

  if(APPNAME[0]) {
    evlog(LOG_NOTICE, "%s version: %.*s%s", APPNAME,
          -20, _appversion, _appversion[20] ? "-dirty" : "");
  }

  evlog(LOG_NOTICE, "Mios version: %.*s%s",
        -20, _miosversion, _miosversion[20] ? "-dirty" : "");

  evlog(LOG_NOTICE, "Build: %.*s", -20, mios_build_id());
}


void
mios_print_version(stream_t *s)
{
  if(APPNAME[0]) {
    stprintf(s, "%s version:", APPNAME);
    stprintversion(s, _appversion);
  }

  stprintf(s, "Mios version:");
  stprintversion(s, _miosversion);

  stprintf(s, "BuildID: ");
  sthexstr(s, mios_build_id(), 20);
  stprintf(s, "\n");
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
