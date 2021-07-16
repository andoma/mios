#pragma once

struct stream;

const char *mios_get_version(void);

const char *mios_get_app_version(void);

const char *mios_get_app_name(void);

void mios_print_version(struct stream *s);

const unsigned char *mios_build_id(void);
