#include "littlefs/lfs.h"

#include <malloc.h>
#include <stdlib.h>
#include <mios/block.h>
#include <mios/task.h>
#include <mios/cli.h>
#include <mios/mios.h>
#include <mios/fs.h>
#include <mios/type_macros.h>

#if CACHE_LINE_SIZE

#define LFS_READ_SIZE CACHE_LINE_SIZE
#define LFS_LOOKAHEAD_SIZE CACHE_LINE_SIZE

#else

#define LFS_READ_SIZE 32
#define LFS_LOOKAHEAD_SIZE 32

#endif


#define LFS_CACHE_SIZE (LFS_READ_SIZE * 1)


typedef struct fs {
  struct lfs_config cfg; // Must be first
  lfs_t lfs;
  mutex_t lock;
  uint8_t lookahead_buffer[LFS_LOOKAHEAD_SIZE]
    __attribute__ ((aligned (CACHE_LINE_SIZE)));
  uint8_t read_buffer[LFS_CACHE_SIZE];
  uint8_t prog_buffer[LFS_CACHE_SIZE];

} fs_t;

static fs_t *g_fs;

static int
fs_blk_read(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, void *buffer, lfs_size_t size)
{
  block_iface_t *bi = c->context;
  error_t err = block_read(bi, block, off, buffer, size);
  return err ? LFS_ERR_IO : 0;
}

static int
fs_blk_prog(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, const void *buffer, lfs_size_t size)
{
  block_iface_t *bi = c->context;
  error_t err = block_write(bi, block, off, buffer, size);
  return err ? LFS_ERR_IO : 0;
}

static int
fs_blk_erase(const struct lfs_config *c, lfs_block_t block)
{
  block_iface_t *bi = c->context;
  error_t err = block_erase(bi, block, 1);
  return err ? LFS_ERR_IO : 0;
}

static int
fs_blk_sync(const struct lfs_config *c)
{
  block_iface_t *bi = c->context;
  error_t err = block_ctrl(bi, BLOCK_SYNC);
  return err ? LFS_ERR_IO : 0;
}

static int
fs_blk_lock(const struct lfs_config *c)
{
  block_iface_t *bi = c->context;
  error_t err = block_ctrl(bi, BLOCK_LOCK);
  return err ? LFS_ERR_IO : 0;
}

static int
fs_blk_unlock(const struct lfs_config *c)
{
  block_iface_t *bi = c->context;
  error_t err = block_ctrl(bi, BLOCK_SUSPEND);
  block_ctrl(bi, BLOCK_UNLOCK);
  return err ? LFS_ERR_IO : 0;
}


void
fs_init(block_iface_t *bi)
{
  fs_t *fs = xalloc(sizeof(fs_t), CACHE_LINE_SIZE, MEM_MAY_FAIL | MEM_TYPE_DMA);
  if(fs == NULL) {
    evlog(LOG_ERR, "fs_init: No memory");
    return;
  }
  memset(fs, 0, sizeof(fs_t));

  fs->cfg.context = bi;
  fs->cfg.read = fs_blk_read;
  fs->cfg.prog = fs_blk_prog;
  fs->cfg.erase = fs_blk_erase;
  fs->cfg.sync = fs_blk_sync;
  fs->cfg.lock = fs_blk_lock;
  fs->cfg.unlock = fs_blk_unlock;

  fs->cfg.read_size = LFS_READ_SIZE;
  fs->cfg.prog_size = LFS_READ_SIZE;
  fs->cfg.block_size = bi->block_size;
  fs->cfg.block_count = bi->num_blocks;
  fs->cfg.cache_size = LFS_CACHE_SIZE;
  fs->cfg.lookahead_size = LFS_LOOKAHEAD_SIZE;
  fs->cfg.read_buffer = fs->read_buffer;
  fs->cfg.prog_buffer = fs->prog_buffer;
  fs->cfg.lookahead_buffer = fs->lookahead_buffer;

  fs->cfg.block_cycles = 500;
  mutex_init(&fs->lock, "fs");

  int err = lfs_mount(&fs->lfs, &fs->cfg);

  if(err) {
    evlog(LOG_ERR, "Failed to mount fs: %d. Formatting", err);
    err = lfs_format(&fs->lfs, &fs->cfg);
    if(err) {
      evlog(LOG_ERR, "Failed to format: %d", err);
    } else {
      err = lfs_mount(&fs->lfs, &fs->cfg);
      if(err) {
        evlog(LOG_ERR, "Failed to mount after format: %d", err);
      }
    }
  }

  if(err) {
    free(fs);
    free(bi);
  } else {
    evlog(LOG_INFO,"FS mounted ok (%d kbyte)",
                   bi->block_size * bi->num_blocks / 1024);
    g_fs = fs;
  }
}



static const int8_t lfs_errmap[] = {
  LFS_ERR_IO,          ERR_IO,
  LFS_ERR_CORRUPT,     ERR_CORRUPT,
  LFS_ERR_NOENT,       ERR_NOT_FOUND,
  LFS_ERR_EXIST,       ERR_EXIST,
  LFS_ERR_NOTDIR,      ERR_NOTDIR,
  LFS_ERR_ISDIR,       ERR_ISDIR,
  LFS_ERR_NOTEMPTY,    ERR_NOTEMPTY,
  LFS_ERR_BADF,        ERR_BADF,
  LFS_ERR_FBIG,        ERR_FBIG,
  LFS_ERR_INVAL,       ERR_INVALID_PARAMETER,
  LFS_ERR_NOSPC,       ERR_NOSPC,
  LFS_ERR_NOMEM,       ERR_NO_MEMORY,
  LFS_ERR_NOATTR,      ERR_NOATTR,
  LFS_ERR_NAMETOOLONG, ERR_TOOLONG
};

static int
maperr(int err)
{
  if(err >= 0)
    return err;

  for(size_t i = 0; i < ARRAYSIZE(lfs_errmap); i+=2) {
    if(err == lfs_errmap[i])
      return lfs_errmap[i + 1];
  }
  return ERR_FS;
}


error_t
fs_mkdir(const char *path)
{
  if(g_fs == NULL)
    return ERR_NO_DEVICE;
  return maperr(lfs_mkdir(&g_fs->lfs, path));
}

error_t
fs_rename(const char *from, const char *to)
{
  if(g_fs == NULL)
    return ERR_NO_DEVICE;
  return maperr(lfs_rename(&g_fs->lfs, from, to));
}

error_t
fs_remove(const char *path)
{
  if(g_fs == NULL)
    return ERR_NO_DEVICE;
  return maperr(lfs_remove(&g_fs->lfs, path));
}


_Static_assert(FS_RDONLY == LFS_O_RDONLY);
_Static_assert(FS_WRONLY == LFS_O_WRONLY);
_Static_assert(FS_RDWR   == LFS_O_RDWR);

_Static_assert(FS_CREAT  == LFS_O_CREAT);
_Static_assert(FS_EXCL   == LFS_O_EXCL);
_Static_assert(FS_TRUNC  == LFS_O_TRUNC);
_Static_assert(FS_APPEND == LFS_O_APPEND);

struct fs_file {
  lfs_file_t file;
  struct lfs_file_config config;
  uint8_t buffer[LFS_CACHE_SIZE]
    __attribute__ ((aligned (CACHE_LINE_SIZE)));
};

error_t
fs_open(const char *path, int flags, fs_file_t **fp)
{
  if(g_fs == NULL)
    return ERR_NO_DEVICE;

  fs_file_t *f = xalloc(sizeof(fs_file_t), CACHE_LINE_SIZE, MEM_MAY_FAIL | MEM_TYPE_DMA);
  if(f == NULL)
    return ERR_NO_MEMORY;
  memset(f, 0, sizeof(fs_file_t));
  f->config.buffer = &f->buffer;

  int r = lfs_file_opencfg(&g_fs->lfs, &f->file, path, flags, &f->config);
  if(r) {
    free(f);
    return maperr(r);
  }
  *fp = f;
  return 0;
}

error_t
fs_close(fs_file_t *f)
{
  int r = lfs_file_close(&g_fs->lfs, &f->file);
  free(f);
  return maperr(r);
}

ssize_t
fs_read(fs_file_t *f, void *buffer, size_t len)
{
  return maperr(lfs_file_read(&g_fs->lfs, &f->file, buffer, len));
}

ssize_t
fs_write(fs_file_t *f, const void *buffer, size_t len)
{
  return maperr(lfs_file_write(&g_fs->lfs, &f->file, buffer, len));
}

error_t
fs_fsync(fs_file_t *f)
{
  return maperr(lfs_file_sync(&g_fs->lfs, &f->file));
}

ssize_t
fs_size(fs_file_t *f)
{
  return maperr(lfs_file_size(&g_fs->lfs, &f->file));
}

error_t
fs_load(const char *path, void *buffer, size_t len, size_t *actual)
{
  fs_file_t *fp;
  error_t err = fs_open(path, FS_RDONLY, &fp);
  if(err)
    return err;

  ssize_t result = fs_read(fp, buffer, len);
  fs_close(fp);
  if(result < 0)
    return result;
  if(actual != NULL) {
    *actual = result;
  } else if(result != len) {
    memset(buffer, 0, len);
    return ERR_MALFORMED;
  }
  return 0;
}

error_t
fs_save(const char *path, const void *buffer, size_t len)
{
  fs_file_t *fp;
  error_t err = fs_open(path, FS_CREAT | FS_WRONLY | FS_TRUNC, &fp);
  if(err)
    return err;

  ssize_t result = fs_write(fp, buffer, len);
  fs_close(fp);
  if(result == len)
    return 0;

  if(result >= 0)
    err = ERR_MALFORMED;
  fs_remove(path);
  return err;
}


//--------------------------------------------------------------------

static error_t
cmd_mkdir(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;
  return fs_mkdir(argv[1]);
}

CLI_CMD_DEF("mkdir", cmd_mkdir);

static error_t
cmd_rm(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;
  return fs_remove(argv[1]);
}

CLI_CMD_DEF("rm", cmd_rm);

static error_t
cmd_mv(cli_t *cli, int argc, char **argv)
{
  if(argc != 3)
    return ERR_INVALID_ARGS;
  return fs_rename(argv[1], argv[2]);
}

CLI_CMD_DEF("mv", cmd_mv);


typedef struct ls_state {
  lfs_dir_t dir;
  struct lfs_info info;
} ls_state_t;


static error_t
cmd_ls(cli_t *cli, int argc, char **argv)
{
  if(g_fs == NULL)
    return ERR_NO_DEVICE;

  ls_state_t *ls = xalloc(sizeof(ls_state_t), 0, MEM_MAY_FAIL);
  if(ls == NULL)
    return ERR_NO_MEMORY;
  int r = lfs_dir_open(&g_fs->lfs, &ls->dir, argc > 1 ? argv[1] : "/");
  if(r < 0) {
    free(ls);
    return maperr(r);
  }
  cli_printf(cli, "\n");
  error_t err = 0;
  while(1) {
    r = lfs_dir_read(&g_fs->lfs, &ls->dir, &ls->info);
    if(r == 0)
      break;
    if(r < 0) {
      err = maperr(r);
      break;
    }

    if(ls->info.type == LFS_TYPE_DIR) {
      cli_printf(cli, "  <dir>  %s\n", ls->info.name);
    } else {
      cli_printf(cli, "%8d %s\n",
                 ls->info.size, ls->info.name);
    }
  }
  cli_printf(cli, "\n");
  lfs_dir_close(&g_fs->lfs, &ls->dir);
  free(ls);
  return err;
}

CLI_CMD_DEF("ls", cmd_ls);


static error_t
display_file(stream_t *st, const char *path, int hex)
{
  char buf[16];

  fs_file_t *fp;
  error_t err = fs_open(path, FS_RDONLY, &fp);
  if(err) {
    return err;
  }
  size_t offset = 0;
  while(1) {
    ssize_t r = fs_read(fp, buf, sizeof(buf));
    if(r < 0) {
      err = r;
      break;
    }
    if(r == 0)
      break;

    if(hex) {
      sthexdump(st, NULL, buf, r, offset);
      offset += r;
    } else {
      stream_write(st, buf, r, 0);
    }
  }
  fs_close(fp);
  return err;
}


static error_t
cmd_cat(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;

  return display_file(cli->cl_stream, argv[1], 0);
}

CLI_CMD_DEF("cat", cmd_cat);


static error_t
cmd_xxd(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;

  return display_file(cli->cl_stream, argv[1], 1);
}

CLI_CMD_DEF("xxd", cmd_xxd);


#if 0
static error_t
cmd_stress(cli_t *cli, int argc, char **argv)
{
  int cnt = 0;

  error_t err;
  fs_remove("foo");
  while(cli_getc(cli, 0) == ERR_NOT_READY) {
    err = fs_mkdir("foo");
    if(err) {
      cli_printf(cli, "mkdir failed %s\n", error_to_string(err));
      break;
    }
    err = fs_rename("foo", "bar");
    if(err) {
      cli_printf(cli, "rename failed %s\n", error_to_string(err));
      break;
    }
    err = fs_remove("bar");
    if(err) {
      cli_printf(cli, "remove failed %s\n", error_to_string(err));
      break;
    }
    cnt++;
    cli_printf(cli, "%d\n", cnt);
  }
  return 0;
}

CLI_CMD_DEF("fs_stress", cmd_stress);

#endif
