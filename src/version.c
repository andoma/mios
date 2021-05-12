#include <mios/version.h>

#include <stdio.h>

#include "version_git.h"

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
  return APPNAME;
}

void
mios_print_version(stream_t *s)
{
  if(APPNAME[0]) {
    stprintf(s, "%s (%s) on ", APPNAME, VERSION_APP_GIT);
  }
  stprintf(s, "Mios (%s)\n", VERSION_MIOS_GIT);
}
