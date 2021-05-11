#include <mios/version.h>

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
