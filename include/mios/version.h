#pragma once

struct stream;

const char *mios_get_app_name(void);

const unsigned char *mios_get_app_version(void);

void mios_print_version(struct stream *s);

const unsigned char *mios_build_id(void);

void log_sysinfo(void);
