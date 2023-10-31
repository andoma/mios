#pragma once

typedef struct atomic {
  int v;
} atomic_t;

static inline void
atomic_inc(atomic_t *a)
{
  __sync_add_and_fetch(&a->v, 1);
}

static inline void
atomic_add(atomic_t *a, int v)
{
  __sync_add_and_fetch(&a->v, v);
}

static inline int __attribute__((warn_unused_result))
atomic_add_and_fetch(atomic_t *a, int v)
{
  return __sync_add_and_fetch(&a->v, v);
}

static inline int
atomic_dec(atomic_t *a)
{
  return __sync_add_and_fetch(&a->v, -1);
}

static inline int
atomic_get(const atomic_t *a)
{
  return __atomic_load_n(&a->v, __ATOMIC_SEQ_CST);
}

static inline void
atomic_set(atomic_t *a, int v)
{
  __atomic_store_n(&a->v, v, __ATOMIC_SEQ_CST);
}
