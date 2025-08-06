#pragma once

typedef struct vlink vlink_t;

vlink_t *vlink_server_create(uint32_t local_id, uint32_t remote_id);
