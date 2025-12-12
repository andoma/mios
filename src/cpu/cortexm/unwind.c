#include <mios/unwind.h>

#include <mios/task.h>

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* These three prevents gcc from linking with a bunch of stack-unwinding code
 * from libgcc which is not useful for us
 */

void __aeabi_unwind_cpp_pr0(void) {}
void __aeabi_unwind_cpp_pr1(void) {}
void __aeabi_unwind_cpp_pr2(void) {}


struct exidx_entry {
    uint32_t offset;
    uint32_t word1;
};


static uint32_t
prel31_to_addr(const uint32_t *ptr)
{
  uint32_t location = *ptr & 0x7fffffff;
  if(location & 0x40000000)
    location |= ~0x7fffffffU;
  return (uint32_t)location + (uint32_t)ptr;
}

static const struct exidx_entry *
find_exidx(uint32_t pc)
{
  extern const uint32_t __exidx_start[];
  extern const uint32_t __exidx_end[];
  extern const uint32_t _text_end;

  if(pc >= (intptr_t)&_text_end) {
    return NULL;
  }

  const uint32_t *start = __exidx_start;
  const uint32_t *end   = __exidx_end;
  size_t entries = (end - start) / 2;

  const struct exidx_entry *table =
    (const struct exidx_entry *)start;

  if(entries == 0)
    return NULL;

  pc &= ~1u;

  size_t lo = 0;
  size_t hi = entries;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    const struct exidx_entry *e = &table[mid];

    uint32_t fn_addr = prel31_to_addr(&e->offset);

    if(pc < fn_addr) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }

  if(lo == 0)
    return NULL; // PC before first function

  return &table[lo - 1];
}


typedef union {
  struct {
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r4;
    uint32_t r5;
    uint32_t r6;
    uint32_t r7;
    uint32_t r8;
    uint32_t r9;
    uint32_t r10;
    uint32_t r11;
    uint32_t r12;
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
  };

  uint32_t r[16];
} armv7_regs_t;


typedef struct {
  uint32_t word;
  uint32_t *ptr;
  uint8_t valid_loaded;
  uint8_t remaining_words;
} opcode_stream_t;

static int
get_opcode(opcode_stream_t *ops)
{
  if(ops->valid_loaded) {
    uint8_t r = ops->word;
    ops->word = ops->word >> 8;
    ops->valid_loaded--;
    return r;
  }

  if(ops->remaining_words == 0)
    return -1;

  uint32_t x = __builtin_bswap32(*ops->ptr);
  ops->word = x >> 8;
  ops->valid_loaded = 3;
  ops->ptr++;
  return x & 0xff;
}

static uint32_t
pop_gprs(uintptr_t sp, armv7_regs_t *regs, uint16_t gpr_mask)
{
  uint32_t *p = (uint32_t *)sp;

  for (unsigned reg = 0; reg < 16; ++reg) {
    if(gpr_mask & (1u << reg)) {
      regs->r[reg] = *p++;
    }
  }
  return (uint32_t)p;
}

static const char *
execute_opcodes(opcode_stream_t *ops, armv7_regs_t *regs)
{
  uintptr_t vsp = regs->r[13];      // virtual stack pointer
  int op, op2;

  regs->pc = 0;
  while(1) {

    if((op = get_opcode(ops)) == -1)
      break;

    if((op & 0xC0) == 0x00) {
      // 00xxxxxx : vsp = vsp + ((xxxxxx << 2) + 4)
      vsp += ((op & 0x3F) << 2) + 4;
    } else if((op & 0xC0) == 0x40) {
      // 01xxxxxx : vsp = vsp - ((xxxxxx << 2) + 4)
      vsp -= ((op & 0x3F) << 2) + 4;
    } else if(op == 0x80) {
      // 10000000 00000000 : refuse to unwind
      return "cant-unwind";
    } else if((op & 0xF0) == 0x80) {
      // 1000iiii iiiiiiii : pop GPRs under mask
      if((op2 = get_opcode(ops)) == -1)
        return "bad-opcode";
      uint16_t mask = ((op & 0x0f) << 12) | (op2 << 4);
      if(mask == 0)
        return "cant-unwind";
      vsp = pop_gprs(vsp, regs, mask);
    } else if((op & 0xF0) == 0x90) {
      // 1001nnnn : vsp = r[n]
      unsigned n = op & 0x0F;
      if(n == 13 || n == 15)
        return "bad-opcode";
      vsp = regs->r[n];
    } else if((op & 0xF8) == 0xA0) {
      // 10100nnn : pop r4..r[4+nnn]
      unsigned n = op & 0x07;
      uint16_t mask = ((1u << (n + 1)) - 1u) << 4; // bits for r4+
      vsp = pop_gprs(vsp, regs, mask);
    }
    else if((op & 0xF8) == 0xA8) {
      // 10101nnn : pop r4..r[4+nnn], lr
      unsigned n = op & 0x07;
      uint16_t mask = ((1u << (n + 1)) - 1u) << 4;
      mask |= (1u << 14); // lr
      vsp = pop_gprs(vsp, regs, mask);
    } else if(op == 0xB0) {
      // 10110000 : finish
      goto done;
    } else if(op == 0xB1) {
      // 10110001 0000iiii : pop {r0..r3}
      if((op2 = get_opcode(ops)) == -1)
        return "bad-opcode";
      uint8_t mask_low = op2 & 0x0F;
      if(mask_low == 0)
        return "bad-opcode";
      vsp = pop_gprs(vsp, regs, mask_low);
    }
    else if(op == 0xB2) {
      // 10110010 uleb128 : vsp = vsp + 0x204 + (uleb128 << 2)
      // (for large positive adjustments)
      uint64_t value = 0;
      unsigned shift = 0;
      int ok = 0;
      int b;
      while(1) {
        if((b = get_opcode(ops)) == -1)
          return "bad-opcode";
        value |= (uint64_t)(b & 0x7F) << shift;
        if(!(b & 0x80)) {
          ok = 1;
          break;
        }
        shift += 7;
        if(shift >= 32)
          break; // sanity
      }
      if(!ok)
        return "bad-opcode";

      vsp += 0x204 + ((uintptr_t)value << 2);
    } else {
      // Uncommon / not supported
      return "bad-opcode";
    }
  }

 done:
  // Commit new SP and PC:
  regs->r[13] = (uint32_t)vsp;

  // If r15 was popped, use that as new PC; otherwise use LR.
  uint32_t new_pc = regs->r[15] ?: regs->r[14];
  if(new_pc == 0) {
    return "no-pc";
  }
  regs->r[15] = new_pc;
  // In Thumb mode, ensure bit 0 is set if code is Thumb; often it already is.
  return NULL;
}




#define EXIDX_CANTUNWIND 1

extern void cpu_idle(void);


static const char errmsg[] = {
  "Thread mode\0"
  "Exc1\0"
  "NMI\0"
  "Hard fault\0"
  "Memory management fault\0"
  "Bus fault\0"
  "Usage fault\0"
  "Exc7\0"
  "Exc8\0"
  "Exc9\0"
  "Exc10\0"
  "SVC\0"
  "Debug\0"
  "Exc13\0"
  "PendSV\0"
  "Systick\0"
  "\0"
};

static void
print_ipsr(struct stream *st, uint32_t ipsr)
{
  if(ipsr >= 16) {
    stprintf(st, "IRQ #%d", ipsr - 16);
    return;
  }
  if(ipsr == 0) {
    thread_t *t = thread_current();
    stprintf(st, "Thread \"%s\"", t ? t->t_name : "???");
    return;
  }
  stprintf(st, "%s", strtbl(errmsg, ipsr));
}


void
backtrace_regs(struct stream *st, armv7_regs_t *r, uint32_t ipsr)
{
  while(1) {
    uint32_t pc = r->pc;
    if(pc == 0xffffffff) {
      break;
    }
    if((pc & 0xffffffe0) == 0xffffffe0) {
      uint32_t *sp = (uint32_t *)r->sp;
      if(pc & (1 << 2)) {
        // Switch to PSP
        asm volatile ("mrs %0, psp\n" : "=r" (sp));
      }
      r->r0 = sp[0];
      r->r1 = sp[1];
      r->r2 = sp[2];
      r->r3 = sp[3];
      r->r12 = sp[4];
      r->lr = sp[5];
      r->pc = sp[6];

      r->sp = (uint32_t)(sp + 8);
      if(ipsr != -1) {
        stprintf(st, "! Exception: ");
        print_ipsr(st, ipsr);
        stprintf(st, "\n> Backtrace continues at %cSP:0x%08x in %s mode\n",
                 pc & (1 << 2) ? 'P' : 'M',
                 r->sp,
                 pc & (1 << 3) ? "Thread" : "Handler");
      }
      ipsr = sp[7] & 0x1ff;
      continue;
    }
    stprintf(st, "  0x%08x ", pc & ~1u);
    const struct exidx_entry *e = find_exidx(pc);
    if(e == NULL) {
      stprintf(st, "<not-in-text-segment>\n");
      break;
    }
    if(pc == (intptr_t)&thread_exit) {
      stprintf(st, "<thread_exit>\n");
      break;
    }
    if(pc == (intptr_t)&cpu_idle) {
      stprintf(st, "<cpu_idle>\n");
      break;
    }

    uint32_t word1 = e->word1;
    if(word1 == EXIDX_CANTUNWIND) {
      stprintf(st, "<no-unwind-info>\n");
      break;
    }

    if(word1 & 0x80000000) {
      uint32_t top_nibble = (word1 >> 28) & 0xF;
      if(top_nibble != 0x8) {
        stprintf(st, "<exidx corrupt>\n");
        break;
      }

      opcode_stream_t ops = {};
      ops.word = __builtin_bswap32(word1) >> 8;
      ops.valid_loaded = 3;
      const char *err = execute_opcodes(&ops, r);
      if(err != NULL) {
        stprintf(st, "<%s>\n", err);
        break;
      }
    } else {
      uint32_t *extab = (uint32_t *)prel31_to_addr(&e->word1);
      uint32_t header = *extab;
      if((header & 0xff000000) == 0x81000000) {
        opcode_stream_t ops = {};
        ops.remaining_words = (header >> 16) & 0xff;
        ops.word = __builtin_bswap32(header) >> 16;
        ops.valid_loaded = 2;
        ops.ptr = extab + 1;
        const char *err = execute_opcodes(&ops, r);
        if(err != NULL) {
          stprintf(st, "<%s>\n", err);
          break;
        }
      } else {
        stprintf(st, "Unsupported extab hdr 0x%0x\n", header);
        break;
      }
    }
    stprintf(st, "\n");
  }
  if(ipsr != -1) {
    stprintf(st, "* Top of stack: ");
    print_ipsr(st, ipsr);
    stprintf(st, "\n");
  }
}


void
backtrace_print_thread(struct stream *st, struct thread *t)
{
  if(t->t_task.t_state == TASK_STATE_RUNNING)
    return;

  uint32_t *sp = t->t_sp;
  armv7_regs_t r;
  r.r0 = sp[8];
  r.r1 = sp[9];
  r.r2 = sp[10];
  r.r3 = sp[11];
  r.r4 = sp[0];
  r.r5 = sp[1];
  r.r6 = sp[2];
  r.r7 = sp[3];
  r.r8 = sp[4];
  r.r9 = sp[5];
  r.r10 = sp[6];
  r.r11 = sp[7];
  r.r12 = sp[12];
  r.sp = (intptr_t)(sp + 16);
  r.lr = sp[13];
  r.pc = sp[14];
  backtrace_regs(st, &r, -1);
}


void
backtrace_print_frame(struct stream *st, void *frame)
{
  if(frame == NULL) {
    backtrace_print(st);
  } else {
    backtrace_regs(st, frame, -1);
  }
}
