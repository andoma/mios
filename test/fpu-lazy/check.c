static void done_at(int ok, unsigned line) {
 const char *text=ok ? "PASS: 1000 lazy FP switches through UsageFault, s0-s31/FPSCR, NOCP cleared\n" : "FAIL\n";
 register uint32_t r0 asm("r0")=4;
 register const char *r1 asm("r1")=text;
 asm volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
 if(!ok) {
   char where[] = "line 0000\n";
   for(unsigned i=0;i<4;i++) {
     where[8-i] = '0' + line % 10;
     line /= 10;
   }
   r0=4; r1=where;
   asm volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
 }
 uint32_t status[2]={0x20026,ok?0:1};
 r0=0x20; r1=(const char *)status;
 asm volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
 for(;;){}
}
#define done(ok) done_at(ok, __LINE__)

static void verify(int32_t *ctx) {
 int32_t regs[32]; uint32_t fpscr;
 asm volatile("vstm %0, {s0-s31}" :: "r"(regs) : "memory");
 asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
 for(unsigned i=0;i<32;i++) if(regs[i]!=ctx[i]) done(0);
 if(fpscr!=(uint32_t)ctx[32]) done(0);
}
void fault(void) { done(0); }
void exc_usage_fault(void *p) { done(0); }
void exc_nmi(void *p) { done(0); }
void exc_hard_fault(void *p) { done(0); }
void exc_bus_fault(void *p) { done(0); }
void exc_mm_fault(void *p) { done(0); }

void reset(void) {
 *(volatile uint32_t *)0xe000ef34=0;
 *(volatile uint32_t *)0xe000ed24 |= 1u<<18;
 int32_t a[33], b[33], live[32];
 uint32_t saved_frame[16];
 // Nonzero upper IT bits must not be mistaken for ICI. Cycle through
 // normal state and IT masks occupying each part of IT[3:0].
 const uint32_t xpsrs[] = {
   0x01000000, 0x01008800, 0x01008400, 0x03008000, 0x05008000
 };
 thread_t ta={a, saved_frame}, tb={b, saved_frame}, idle={0};
 for(unsigned i=0;i<32;i++) { a[i]=0x3f800000+i; b[i]=0x40000000+i; }
 a[32]=0x01000000; b[32]=0x00400000;
 cpu0.sched.current_fpu=0;
 for(unsigned n=0;n<1000;n++) {
  saved_frame[15] = xpsrs[n % 5];
  current=&ta; cpu_fpu_resume(current); verify(a);
  // Mutate live registers, then check they survive ownership transfer.
  for(unsigned i=0;i<32;i++) live[i]=0x41000000 + n * 32 + i;
  asm volatile("vldm %0, {s0-s31}" :: "r"(live) : "memory");
  current=&tb; cpu_fpu_resume(current); verify(b);
  for(unsigned i=0;i<32;i++) if(a[i]!=live[i]) done(0);
  current=&idle; cpu_fpu_resume(current);
  if(*(volatile uint32_t *)0xe000ed88 & 0x00f00000) done(0);
  if(cpu0.sched.current_fpu!=&tb) done(0);
  current=&tb; cpu_fpu_resume(current); verify(b);
  current=&ta; cpu_fpu_resume(current); verify(a);
 }
 if(*UFSR || fpu_nocp_restores != 2001 || cpu_fpu_ici_restores) done(0);
 done(1);
}
