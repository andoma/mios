#pragma once

/*
 * Simulation threads for virtual time test suites.
 *
 * A simulation thread is a real Linux thread that plays a peer or a
 * piece of hardware: a DHCP server, the far end of a CAN bus, a sensor.
 * It is written like an ordinary blocking program:
 *
 *   while(1) {
 *     int n = vnet_peer_recv(v, buf, sizeof(buf), deadline);   // blocks
 *     if(n < 0) { deadline reached, do timer work }
 *     else      { handle frame, maybe vnet_peer_send(...) }
 *   }
 *
 * Underneath, everything runs in lockstep so results are deterministic:
 *
 *  - Exactly one participant executes at any time: either the Mios CPU
 *    thread or one simulation thread. Handover is explicit, through
 *    sim_wait() on the peer side and the idle loop on the Mios side.
 *
 *  - The virtual clock only advances when every participant is waiting.
 *    It then jumps to the earliest deadline (a Mios timer or a
 *    sim_wait() deadline) and runs whoever owns it. Peers due at the
 *    same instant run in creation order, then Mios takes its interrupt.
 *
 *  - A peer talks to Mios the way hardware does: writes into a ring and
 *    pends an IRQ line (host_irq_pend). It must never call into the Mios
 *    kernel (no malloc, mutexes, task or net calls). clock_get() and
 *    printf() are fine.
 *
 * Only available in virtual time mode (test suites).
 */

#include <stdint.h>
#include <stddef.h>

#define SIM_NEVER UINT64_MAX

typedef struct sim_thread sim_thread_t;

// Create a peer. It starts running when Mios next goes idle.
sim_thread_t *sim_thread_create(const char *name, void (*fn)(void *arg),
                                void *arg, size_t stack_size);

// The calling simulation thread, NULL on the Mios CPU thread
sim_thread_t *sim_current(void);

// Block until virtual time reaches deadline or someone sim_post():s us.
// Returns 1 if posted, 0 if the deadline was reached. Peer threads only.
int sim_wait(uint64_t deadline);

// Make t runnable now. Callable from Mios or from a peer.
void sim_post(sim_thread_t *t);

// Coordinator, called by the idle loop in virtual time mode
void sim_idle(void);
