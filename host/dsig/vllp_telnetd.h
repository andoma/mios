#pragma once

struct vllp;
typedef struct vllp_telnetd vllp_telnetd_t;

vllp_telnetd_t *vllp_telnetd_create(struct vllp *v, const char *service, int port);

void vllp_telnetd_destroy(vllp_telnetd_t *vtd);
