// Unicast DNS client (RFC 1035), A records only. Servers come from DHCP,
// those of the interface with the default route are tried first.
//
// Queries go out from a fixed local port (UDP input handlers are bound
// to static ports), so a spoofed answer only has to guess the 16 bit id.

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
#include <unistd.h>

#include <mios/cli.h>
#include <mios/task.h>
#include <mios/bytestream.h>

#include "net/netif.h"
#include "net/net.h"
#include "net/pbuf.h"

#include "ipv4.h"
#include "udp.h"
#include "dns.h"

#define DNS_PORT        53
#define DNS_LOCAL_PORT  53053
#define DNS_ATTEMPTS    3
#define DNS_MAX_MSG     512   // Without EDNS answers over UDP fit in this
#define DNS_CACHE_SIZE  8
#define DNS_MAX_TTL     3600  // Seconds, cap what servers tell us
#define DNS_MAX_SERVERS 4

#define DNS_FLAG_QR 0x8000
#define DNS_FLAG_RD 0x0100

#define DNS_RCODE_NXDOMAIN 3

typedef struct dns_req {
  LIST_ENTRY(dns_req) dr_link;
  const char *dr_name;
  cond_t dr_cond;
  uint16_t dr_id;
  uint8_t dr_attempt;
  uint8_t dr_send;    // For the net task to send
  uint8_t dr_done;
  error_t dr_result;
  uint32_t dr_addr;
  uint32_t dr_ttl;
} dns_req_t;

typedef struct dns_cache_ent {
  char *name;
  uint32_t addr;
  int64_t expire;
} dns_cache_ent_t;

// Protects all of the below
static mutex_t dns_mutex = MUTEX_INITIALIZER("dns");
static LIST_HEAD(, dns_req) dns_reqs;
static dns_cache_ent_t dns_cache[DNS_CACHE_SIZE];


static int
dns_servers_add(const netif_t *ni, uint32_t *out, int n)
{
  if(!(ni->ni_flags & NETIF_F_UP))
    return n;
  for(int i = 0; i < 2 && n < DNS_MAX_SERVERS; i++) {
    if(ni->ni_ipv4_dns[i])
      out[n++] = ni->ni_ipv4_dns[i];
  }
  return n;
}


// Collect servers, those behind the default route first. Net thread
static int
dns_servers(uint32_t *out)
{
  netif_t *def = ipv4_default_route();
  int n = def ? dns_servers_add(def, out, 0) : 0;

  netif_t *ni;
  SLIST_FOREACH(ni, &netifs, ni_global_link) {
    if(ni != def)
      n = dns_servers_add(ni, out, n);
  }
  return n;
}


static pbuf_t *
dns_make_query(const dns_req_t *dr)
{
  const size_t namelen = strlen(dr->dr_name);
  pbuf_t *pb = pbuf_make(16 + 20 + 8, 0);
  if(pb == NULL)
    return NULL;

  uint8_t *p = pbuf_append(pb, 12 + namelen + 2 + 4);
  wr16_be(p + 0, dr->dr_id);
  wr16_be(p + 2, DNS_FLAG_RD);
  wr16_be(p + 4, 1); // Questions
  wr16_be(p + 6, 0);
  wr16_be(p + 8, 0);
  wr16_be(p + 10, 0);

  // www.example.com -> 3www7example3com0
  uint8_t *q = p + 12;
  const char *s = dr->dr_name;
  while(*s) {
    const char *dot = strchr(s, '.');
    const size_t len = dot ? (size_t)(dot - s) : strlen(s);
    *q++ = len;
    memcpy(q, s, len);
    q += len;
    s += len;
    if(*s == '.')
      s++;
  }
  *q++ = 0;
  wr16_be(q, 1);     // Type A
  wr16_be(q + 2, 1); // Class IN
  return pb;
}


static void
dns_task_cb(net_task_t *nt, uint32_t signals)
{
  uint32_t servers[DNS_MAX_SERVERS];
  const int num_servers = dns_servers(servers);

  mutex_lock(&dns_mutex);
  dns_req_t *dr;
  LIST_FOREACH(dr, &dns_reqs, dr_link) {
    if(!dr->dr_send)
      continue;
    dr->dr_send = 0;
    if(num_servers == 0) {
      dr->dr_result = ERR_NOT_READY;
      dr->dr_done = 1;
      cond_signal(&dr->dr_cond);
      continue;
    }
    pbuf_t *pb = dns_make_query(dr);
    if(pb != NULL)
      udp_send(NULL, pb, servers[dr->dr_attempt % num_servers], NULL,
               DNS_LOCAL_PORT, DNS_PORT);
  }
  mutex_unlock(&dns_mutex);
}

static net_task_t dns_task = { dns_task_cb };


// Offset after the (possibly compressed) name at off, -1 if malformed
static int
dns_skip_name(const uint8_t *m, int len, int off)
{
  while(off < len) {
    const uint8_t c = m[off];
    if(c == 0)
      return off + 1;
    if((c & 0xc0) == 0xc0)
      return off + 2 <= len ? off + 2 : -1;
    if(c & 0xc0)
      return -1;
    off += 1 + c;
  }
  return -1;
}


// Parse an answer. Returns 0 with the first A record, or an error
static error_t
dns_parse(const uint8_t *m, int len, uint32_t *addr, uint32_t *ttl)
{
  const uint16_t flags = rd16_be(m + 2);
  if((flags & 0xf) == DNS_RCODE_NXDOMAIN)
    return ERR_NOT_FOUND;
  if(flags & 0xf)
    return ERR_OPERATION_FAILED;

  int off = 12;
  for(int i = rd16_be(m + 4); i > 0; i--) {
    off = dns_skip_name(m, len, off);
    if(off < 0 || off + 4 > len)
      return ERR_MALFORMED;
    off += 4;
  }

  // Follow no CNAMEs: servers put the records they lead to in the same
  // answer, so the first A record is the one we want
  for(int i = rd16_be(m + 6); i > 0; i--) {
    off = dns_skip_name(m, len, off);
    if(off < 0 || off + 10 > len)
      return ERR_MALFORMED;
    const uint16_t type = rd16_be(m + off);
    const uint16_t class = rd16_be(m + off + 2);
    const uint32_t t = rd32_be(m + off + 4);
    const uint16_t rdlen = rd16_be(m + off + 8);
    off += 10;
    if(off + rdlen > len)
      return ERR_MALFORMED;
    if(type == 1 && class == 1 && rdlen == 4) {
      memcpy(addr, m + off, 4);
      *ttl = t;
      return 0;
    }
    off += rdlen;
  }
  return ERR_NOT_FOUND;
}


static pbuf_t *
dns_input(netif_t *ni, pbuf_t *pb, size_t udp_offset)
{
  const int len = pb->pb_pktlen - udp_offset - 8;
  if(len < 12 || len > DNS_MAX_MSG)
    return pb;

  uint8_t *m = xalloc(len, 0, MEM_MAY_FAIL);
  if(m == NULL)
    return pb;
  if(!pbuf_read_at(pb, m, udp_offset + 8, len) &&
     rd16_be(m + 2) & DNS_FLAG_QR) {
    const uint16_t id = rd16_be(m);
    mutex_lock(&dns_mutex);
    dns_req_t *dr;
    LIST_FOREACH(dr, &dns_reqs, dr_link) {
      if(dr->dr_id == id && !dr->dr_done) {
        dr->dr_result = dns_parse(m, len, &dr->dr_addr, &dr->dr_ttl);
        dr->dr_done = 1;
        cond_signal(&dr->dr_cond);
        break;
      }
    }
    mutex_unlock(&dns_mutex);
  }
  free(m);
  return pb;
}

UDP_INPUT(dns_input, DNS_LOCAL_PORT);


// A dotted quad such as 10.0.0.1
static int
is_ipv4_literal(const char *s)
{
  int dots = 0, digits = 0;
  for(; *s; s++) {
    if(*s == '.') {
      if(digits == 0)
        return 0;
      dots++;
      digits = 0;
    } else if(*s >= '0' && *s <= '9') {
      if(++digits > 3)
        return 0;
    } else {
      return 0;
    }
  }
  return dots == 3 && digits > 0;
}


static int
is_valid_name(const char *s)
{
  const size_t len = strlen(s);
  if(len == 0 || len > 253)
    return 0;
  size_t label = 0;
  for(; *s; s++) {
    if(*s == '.') {
      if(label == 0)
        return 0;
      label = 0;
    } else if(++label > 63) {
      return 0;
    }
  }
  return label > 0;
}


// Called with dns_mutex held
static dns_cache_ent_t *
dns_cache_find(const char *name, int64_t now)
{
  for(int i = 0; i < DNS_CACHE_SIZE; i++) {
    dns_cache_ent_t *dce = &dns_cache[i];
    if(dce->name && dce->expire > now && !strcasecmp(dce->name, name))
      return dce;
  }
  return NULL;
}


// Called with dns_mutex held. Replaces the entry that expires first
static void
dns_cache_insert(const char *name, uint32_t addr, uint32_t ttl, int64_t now)
{
  dns_cache_ent_t *victim = &dns_cache[0];
  for(int i = 0; i < DNS_CACHE_SIZE; i++) {
    dns_cache_ent_t *dce = &dns_cache[i];
    if(dce->name && !strcasecmp(dce->name, name)) {
      victim = dce;
      break;
    }
    if(dce->expire < victim->expire)
      victim = dce;
  }
  free(victim->name);
  victim->name = strdup(name);
  victim->addr = addr;
  if(ttl > DNS_MAX_TTL)
    ttl = DNS_MAX_TTL;
  victim->expire = now + ttl * 1000000ll;
}


error_t
dns_resolve(const char *name, uint32_t *addr)
{
  if(is_ipv4_literal(name)) {
    *addr = inet_addr(name);
    return 0;
  }
  if(!is_valid_name(name))
    return ERR_INVALID_ARGS;

  mutex_lock(&dns_mutex);

  const dns_cache_ent_t *dce = dns_cache_find(name, clock_get());
  if(dce != NULL) {
    *addr = dce->addr;
    mutex_unlock(&dns_mutex);
    return 0;
  }

  dns_req_t dr = {
    .dr_name = name,
    .dr_id = rand(),
  };
  cond_init(&dr.dr_cond, "dns");
  LIST_INSERT_HEAD(&dns_reqs, &dr, dr_link);

  // 1 + 2 + 2 seconds. The id stays the same, so a late answer to an
  // earlier attempt still counts
  for(int i = 0; i < DNS_ATTEMPTS && !dr.dr_done; i++) {
    dr.dr_attempt = i;
    dr.dr_send = 1;
    net_task_raise(&dns_task, 1);
    const int64_t deadline = clock_get() + (i ? 2000000 : 1000000);
    while(!dr.dr_done) {
      if(cond_wait_timeout(&dr.dr_cond, &dns_mutex, deadline))
        break;
    }
  }
  LIST_REMOVE(&dr, dr_link);

  error_t err = dr.dr_done ? dr.dr_result : ERR_TIMEOUT;
  if(!err) {
    dns_cache_insert(name, dr.dr_addr, dr.dr_ttl, clock_get());
    *addr = dr.dr_addr;
  }
  mutex_unlock(&dns_mutex);
  return err;
}


static error_t
cmd_host(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;

  uint32_t addr;
  error_t err = dns_resolve(argv[1], &addr);
  if(err == ERR_NOT_READY) {
    cli_printf(cli, "No DNS server\n");
  } else if(err == ERR_NOT_FOUND) {
    cli_printf(cli, "%s not found\n", argv[1]);
  } else if(!err) {
    cli_printf(cli, "%s has address %Id\n", argv[1], addr);
  }
  return err;
}

CLI_CMD_DEF_EXT("host", cmd_host, "<name>", "Look up a host name in DNS");


static error_t
cmd_show_dns(cli_t *cli, int argc, char **argv)
{
  netif_t *ni = NULL;
  while((ni = netif_get_net(ni)) != NULL) {
    for(int i = 0; i < 2; i++) {
      if(ni->ni_ipv4_dns[i])
        cli_printf(cli, "Server %Id dev %s\n", ni->ni_ipv4_dns[i],
                   ni->ni_dev.d_name);
    }
  }

  mutex_lock(&dns_mutex);
  const int64_t now = clock_get();
  for(int i = 0; i < DNS_CACHE_SIZE; i++) {
    const dns_cache_ent_t *dce = &dns_cache[i];
    if(dce->name && dce->expire > now)
      cli_printf(cli, "%s %Id %ds\n", dce->name, dce->addr,
                 (int)((dce->expire - now) / 1000000));
  }
  mutex_unlock(&dns_mutex);
  return 0;
}

CLI_CMD_DEF_EXT("show_dns", cmd_show_dns, NULL,
                "Show DNS servers and cached names");
