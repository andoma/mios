#include "littlefs/lfs.h"

#include <malloc.h>

#include <mios/block.h>
#include <mios/task.h>
#include <mios/cli.h>
#include <mios/mios.h>

typedef struct fs {
  struct lfs_config cfg; // Must be first
  lfs_t lfs;
  mutex_t lock;
} fs_t;

static fs_t *g_fs;

static int
fs_blk_read(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, void *buffer, lfs_size_t size)
{
  block_iface_t *bi = c->context;
  error_t err = bi->read(bi, block, off, buffer, size);
  return err ? LFS_ERR_IO : 0;
}

static int
fs_blk_prog(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, const void *buffer, lfs_size_t size)
{
  block_iface_t *bi = c->context;
  error_t err = bi->write(bi, block, off, buffer, size);
  return err ? LFS_ERR_IO : 0;
}

static int
fs_blk_erase(const struct lfs_config *c, lfs_block_t block)
{
  block_iface_t *bi = c->context;
  error_t err = bi->erase(bi, block);
  return err ? LFS_ERR_IO : 0;
}

static int
fs_blk_sync(const struct lfs_config *c)
{
  block_iface_t *bi = c->context;
  error_t err = bi->sync(bi);
  return err ? LFS_ERR_IO : 0;
}

static int
fs_blk_lock(const struct lfs_config *c)
{
  fs_t *fs = (fs_t *)c;
  mutex_lock(&fs->lock);
  return 0;
}

static int
fs_blk_unlock(const struct lfs_config *c)
{
  fs_t *fs = (fs_t *)c;
  mutex_unlock(&fs->lock);
  return 0;
}


void
fs_init(block_iface_t *bi)
{
  fs_t *fs = calloc(1, sizeof(fs_t));

  fs->cfg.context = bi;
  fs->cfg.read = fs_blk_read;
  fs->cfg.prog = fs_blk_prog;
  fs->cfg.erase = fs_blk_erase;
  fs->cfg.sync = fs_blk_sync;
  fs->cfg.lock = fs_blk_lock;
  fs->cfg.unlock = fs_blk_unlock;

  fs->cfg.read_size = 32;
  fs->cfg.prog_size = 32;
  fs->cfg.block_size = bi->block_size;
  fs->cfg.block_count = bi->num_blocks;
  fs->cfg.cache_size = 32;
  fs->cfg.lookahead_size = 32;
  fs->cfg.block_cycles = 500;
  mutex_init(&fs->lock, "fs");

  int err = lfs_mount(&fs->lfs, &fs->cfg);
  if(err) {
    printf("Failed to mount fs: %d\n", err);
    err = lfs_format(&fs->lfs, &fs->cfg);
    if(err) {
      printf("Failed to format: %d\n", err);
    } else {
      err = lfs_mount(&fs->lfs, &fs->cfg);
      if(err) {
        printf("Failed to mount after format: %d\n", err);
      }
    }
  }

  if(err) {
    free(fs);
    free(bi);
  } else {
    printf("FS mounted ok\n");
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

static error_t
maperr(int err)
{
  if(err == 0)
    return 0;

  for(size_t i = 0; i < ARRAYSIZE(lfs_errmap); i+=2) {
    if(err == lfs_errmap[i])
      return lfs_errmap[i + 1];
  }
  return ERR_FS;
}



static error_t
cmd_mkdir(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;
  if(g_fs == NULL)
    return ERR_NO_DEVICE;
  return maperr(lfs_mkdir(&g_fs->lfs, argv[1]));
}

CLI_CMD_DEF("mkdir", cmd_mkdir);

static error_t
cmd_rm(cli_t *cli, int argc, char **argv)
{
  if(argc != 2)
    return ERR_INVALID_ARGS;
  if(g_fs == NULL)
    return ERR_NO_DEVICE;
  return maperr(lfs_remove(&g_fs->lfs, argv[1]));
}

CLI_CMD_DEF("rm", cmd_rm);

static error_t
cmd_mv(cli_t *cli, int argc, char **argv)
{
  if(argc != 3)
    return ERR_INVALID_ARGS;
  if(g_fs == NULL)
    return ERR_NO_DEVICE;
  return maperr(lfs_rename(&g_fs->lfs, argv[1], argv[2]));
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
      cli_printf(cli, "<dir>\t%s\n", ls->info.name);
    } else {
      cli_printf(cli, "%d\t%s\n",
                 ls->info.size, ls->info.name);
    }
  }
  cli_printf(cli, "\n");
  lfs_dir_close(&g_fs->lfs, &ls->dir);
  free(ls);
  return err;
}

CLI_CMD_DEF("ls", cmd_ls);

#if 0
static error_t
cmd_stress(cli_t *cli, int argc, char **argv)
{
  int cnt = 0;
  while(1) {
    lfs_mkdir(&g_fs->lfs, "tjena");
    lfs_remove(&g_fs->lfs, "tjena");

    cnt++;
    printf("%d\n", cnt);
  }
  return 0;
}

CLI_CMD_DEF("stress", cmd_stress);

#endif
