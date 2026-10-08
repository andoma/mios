// A synthetic, valid basic exception frame for VPOP {d8-d11}, ICI=11.
// ARM DDI0403E.e B1.5.10: SP for interrupted increment-after is the
// original SP; d8-d10 have completed and d11 remains to be transferred.
// QEMU restarts the entire transfer, so this checks fault/ownership behavior,
// not the hardware's partial-transfer memory access sequence.
static uint32_t stack[128] __attribute__((aligned(8)));
static uint32_t outgoing[32] __attribute__((aligned(8)));
uint32_t *outgoing_sp;
static int32_t incoming_fp[33], old_fp[33], old_live[33];
static thread_t incoming, old_owner;
extern void resume_vpop(void);

static void print(const char *p)
{
  register uint32_t r0 asm("r0") = 4;
  register const char *r1 asm("r1") = p;
  asm volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
}

static void done(int fail)
{
  uint32_t status[2] = {0x20026, fail};
  register uint32_t r0 asm("r0") = 0x20;
  register void *r1 asm("r1") = status;
  asm volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
  for(;;) {}
}

static void fail(void)
{
  print("FAIL: continuation test\n");
  done(1);
}

void exc_usage_fault(void *unused)
{
  const uint32_t *frame;
  asm volatile("mrs %0, psp" : "=r"(frame));
  if(TEST_OLD_POLICY && *UFSR == 2 && frame[7] == 0x0100b000 &&
     frame[6] == ((uint32_t)resume_vpop & ~1u) &&
     *(volatile uint32_t *)0xe000ed88 == 0) {
    print("PASS: old policy reproduces INVSTATE at VPOP, ICI=11, CPACR=0\n");
    done(0);
  }
  fail();
}
void exc_nmi(void *p) { fail(); }
void exc_hard_fault(void *p) { fail(); }
void exc_bus_fault(void *p) { fail(); }
void exc_mm_fault(void *p) { fail(); }

void completed(uint32_t *sp)
{
  int32_t regs[33];
  asm volatile("vstm %0, {s0-s31}" :: "r"(regs) : "memory");
  asm volatile("vmrs %0, fpscr" : "=r"(regs[32]));
  if(TEST_OLD_POLICY || sp != &stack[80] || *UFSR ||
     cpu0.sched.current_fpu != &incoming || cpu_fpu_ici_restores != 1 ||
     fpu_nocp_restores != 0)
    fail();
  for(unsigned i = 0; i < 33; i++) {
    if(regs[i] != incoming_fp[i] || old_fp[i] != old_live[i])
      fail();
  }
  print("PASS: PendSV resumes ICI VPOP with correct FP owner, registers and SP\n");
  done(0);
}

void *task_switch(void *saved_sp)
{
  uint32_t msp;
  asm volatile("mrs %0, msp" : "=r"(msp));
  if(msp & 7)
    fail();
  current = &incoming;
#if TEST_OLD_POLICY
  cpu_fpu_enable(cpu0.sched.current_fpu == current);
#else
  cpu_fpu_resume(current);
#endif
  return current->t_sp;
}

void reset(void)
{
  *(volatile uint32_t *)0xe000ef34 = 0;
  *(volatile uint32_t *)0xe000ed24 = 1u << 18;
  cpu_fpu_enable(1);
  for(unsigned i = 0; i < 32; i++) {
    incoming_fp[i] = 0x41000000 + i;
    old_live[i] = 0x42000000 + i;
    old_fp[i] = 0;
  }
  incoming_fp[32] = 0x01000000;
  old_live[32] = 0x00400000;
  asm volatile("vldm %0, {s0-s31}" :: "r"(old_live) : "memory");
  asm volatile("vmsr fpscr, %0" :: "r"(old_live[32]));
  incoming.t_fpuctx = incoming_fp;
  incoming.t_sp = &stack[56];
  old_owner.t_fpuctx = old_fp;
  cpu0.sched.current_fpu = &old_owner;
  // Eight software-saved registers followed by the hardware frame.
  stack[70] = (uint32_t)resume_vpop & ~1u;
  stack[71] = 0x0100b000;
  for(unsigned i = 0; i < 8; i++)
    stack[72 + i] = incoming_fp[16 + i];
  outgoing_sp = &outgoing[16];
  asm volatile("svc 0");
  fail();
}
