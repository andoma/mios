#pragma once

#include <mios/error.h>

typedef struct fs_file fs_file_t;

error_t fs_mkdir(const char *path);

error_t fs_remove(const char *path);

error_t fs_rename(const char *from, const char *to);

error_t fs_open(const char *path, int flags, fs_file_t **f);

#define FS_RDONLY 0x1
#define FS_WRONLY 0x2
#define FS_RDWR   0x3
#define FS_CREAT  0x0100
#define FS_EXCL   0x0200
#define FS_TRUNC  0x0400
#define FS_APPEND 0x0800

error_t fs_close(fs_file_t *f);

ssize_t fs_read(fs_file_t *f, void *buffer, size_t len);

ssize_t fs_write(fs_file_t *f, const void *buffer, size_t len);

error_t fs_fsync(fs_file_t *f);

ssize_t fs_size(fs_file_t *f);

error_t fs_load(const char *path, void *buffer, size_t len,
                size_t *actual_size);

error_t fs_save(const char *path, const void *buffer, size_t len);

struct block_iface;

void fs_init(struct block_iface *bi);

