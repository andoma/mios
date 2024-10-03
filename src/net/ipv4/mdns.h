#pragma once 
#include <stdint.h>

error_t mdns_request(const char *dnsname, uint32_t *inaddr);
