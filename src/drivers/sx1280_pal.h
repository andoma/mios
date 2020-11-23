#pragma once

#include <mios/error.h>
#include <mios/io.h>
#include <mios/task.h>
#include <malloc.h>

typedef mutex_t sx1280_mutex_t;
typedef cond_t sx1280_cond_t;
typedef task_t *sx1280_thread_t;

#define sx1280_malloc(s) xalloc(s, 0, MEM_TYPE_DMA)
#define sx1280_free(p) free(p)

#define sx1280_mutex_lock(m)   mutex_lock(m)
#define sx1280_mutex_unlock(m) mutex_unlock(m)
#define sx1280_cond_wait(c, m) cond_wait(c, m)
#define sx1280_cond_signal(c)  cond_signal(c)
#define sx1280_clock_get() clock_get()
#define sx1280_thread_create(t, p, a, pri) *(t) = task_create(p, a, 1024, "sx1280", TASK_DMA_STACK, pri)



typedef error_t sx1280_err_t;

#define SX1280_ERR_INVALID ERR_INVALID_ID
#define SX1280_ERR_MTU     ERR_MTU_EXCEEDED
#define SX1280_ERR_NO_BUFS ERR_NO_BUFFER

sx1280_t *sx1280_create(spi_t *bus, gpio_t nss, gpio_t busy,
                        gpio_t irq, gpio_t reset,
                        gpio_t dbg1, gpio_t dbg2);

