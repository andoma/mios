#pragma once

#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (void *)__mptr - offsetof(type,member) );})
