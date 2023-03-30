#pragma once

#include <stdint.h>
#include <stddef.h>

struct pbuf;

struct pbuf *mbus_dsig_input(struct pbuf *pb, uint16_t group_addr);

void mbus_dsig_emit(uint16_t signal, const void *data, size_t len);
