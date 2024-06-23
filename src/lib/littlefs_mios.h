#define LFS_NO_ASSERT

#define LFS_THREADSAFE

#include <mios/eventlog.h>

#define LFS_NO_MALLOC

#define LFS_NO_DEBUG

#define LFS_WARN(fmt...) evlog0(LOG_WARNING, NULL, fmt)

#define LFS_ERROR(fmt...) evlog0(LOG_ERR, NULL, fmt)
