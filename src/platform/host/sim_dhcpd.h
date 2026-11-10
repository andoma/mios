#pragma once

/*
 * DHCP server for a vnet, running as a simulation thread (see
 * cpu/host/sim.h): a blocking receive loop with a deadline, like a real
 * server program. Also answers ARP for its own address so unicast
 * renewals can be delivered, and optionally DNS queries for a small
 * table of names. Behaviour knobs let a suite drive the client's state
 * machine through its corners; the suite sets them from the Mios main
 * thread, which is safe because only one participant runs at a time.
 */

#include <stdint.h>
#include <stddef.h>

#include "vnet.h"

typedef struct sim_dns_record {
  const char *name;
  uint32_t addr;               // Network byte order
  uint32_t ttl;
  const char *cname;           // Answer with a CNAME to this name first
  int padding;                 // Unrelated records to put before the A
} sim_dns_record_t;

typedef struct sim_dhcpd {
  vnet_t *vnet;                // Set by sim_dhcpd_attach()

  // Configuration (network byte order addresses)
  uint8_t  mac[6];
  uint32_t server_ip;
  uint32_t netmask;
  uint32_t gateway;
  uint32_t pool_ip;            // The one address we hand out
  uint32_t lease_time;         // seconds
  uint32_t ntp_server;         // 0: don't include
  uint32_t dns_server;         // 0: don't include
  uint16_t mtu;                // Interface MTU option, 0: don't include

  // DNS server on server_ip, names not in the table get NXDOMAIN
  const sim_dns_record_t *dns_records;
  int dns_num_records;
  int dns_drop;                // Ignore this many queries, then answer

  // Behaviour
  int silent;                  // Ignore all DHCP
  int ignore_discovers;        // Ignore this many DISCOVERs, then answer
  int nak_requests;            // NAK this many REQUESTs, then ACK
  uint32_t reply_delay;        // us, 0 = reply synchronously

  // Observations
  int rx_discover;
  int rx_request;
  int rx_request_unicast;      // REQUESTs addressed to our MAC (renewals)
  int rx_arp_request;
  int tx_offer;
  int tx_ack;
  int tx_nak;
  int rx_dns_query;
  uint32_t last_xid;
  uint32_t last_discover_xid;
  uint32_t last_request_xid;
  int discover_xid_changes;    // DISCOVERs whose xid differed from the previous one
  int request_xid_changes;     // same for REQUESTs
  uint32_t last_ciaddr;
  uint32_t last_requested_ip;
  uint32_t last_server_id;

  // Internals
  struct {                     // Delayed replies, in order of arrival
    uint64_t at;
    uint16_t len;
    uint8_t frame[1100];
  } pending[8];
  int pending_rd;
  int pending_wr;
  uint16_t ip_id;
  uint8_t rxframe[2048];
  uint8_t dnsframe[1100];      // DNS reply being built
} sim_dhcpd_t;

// Configure for 10.<net>.0.0/24: server .1, gateway .1, pool .50
void sim_dhcpd_defaults(sim_dhcpd_t *d, int net);

// Create the vnet and start the server thread on it
vnet_t *sim_dhcpd_attach(sim_dhcpd_t *d, const char *ifname,
                         const uint8_t client_mac[6]);
