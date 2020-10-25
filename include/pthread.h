#pragma once

// Wrapper for pthread defintions. Far from complete

#include "task.h"

#define PTHREAD_MUTEX_INITIALIZER MUTEX_INITIALIZER(NULL)

typedef mutex_t pthread_mutex_t;

#define pthread_mutex_lock(m)   mutex_lock(m)
#define pthread_mutex_trylock(m) mutex_trylock(m)
#define pthread_mutex_unlock(m) mutex_unlock(m)

#define PTHREAD_COND_INITIALIZER COND_INITIALIZER(NULL)

typedef cond_t pthread_cond_t;

#define pthread_cond_init(c, a) cond_init(c, NULL)
#define pthread_cond_signal(c)  cond_signal(c)
#define pthread_cond_wait(c, m) cond_wait(c, m)
