#include <mios/alert.h>
#include <mios/task.h>
#include <mios/ghook.h>
#include <mios/cli.h>
#include <mios/atomic.h>

#include <stdlib.h>
#include <string.h>
#include <malloc.h>

SLIST_HEAD(alert_source_slist, alert_source);
static struct alert_source_slist alerts;
static mutex_t alert_mutex;

static void
alert_source_refcount(alert_source_t *as, int value)
{
  if(as->as_class->ac_refcount != NULL)
    as->as_class->ac_refcount(as, value);
}

static int
alert_cmp(const alert_source_t *a, const alert_source_t *b)
{
  return strcmp(a->as_key, b->as_key);
}

void
alert_register(alert_source_t *as, const alert_class_t *ac, const char *key)
{
  as->as_key = key;
  as->as_class = ac;
  as->as_code = 0;
  alert_source_refcount(as, 1);
  mutex_lock(&alert_mutex);
  SLIST_INSERT_SORTED(&alerts, as, as_link, alert_cmp);
  mutex_unlock(&alert_mutex);
}

void
alert_unregister(alert_source_t *as)
{
  mutex_lock(&alert_mutex);
  SLIST_REMOVE(&alerts, as, alert_source, as_link);
  as->as_link.sle_next = NULL;
  mutex_unlock(&alert_mutex);
  alert_source_refcount(as, -1);
}

int
alert_set(alert_source_t *as, int code)
{
  if(as->as_code == code)
    return 0;

  as->as_code = code;

  if(!code) {
    evlog(LOG_INFO, "Alert cleared [%s]", as->as_key);
  } else {
    stream_t *st = evlog_stream_begin();
    stprintf(st, "Alert raised [%s] -- ", as->as_key);
    const alert_class_t *ac = as->as_class;
    ac->ac_message(as, st);
    evlog_stream_end(ac->ac_level(as));
  }

  ghook_invoke(GHOOK_ALERT_UPDATED);
  return 1;
}

alert_source_t *
alert_get_next(alert_source_t *cur)
{
  alert_source_t *as;
  mutex_lock(&alert_mutex);

  if(cur == NULL) {
    as = SLIST_FIRST(&alerts);
  } else {
    as = SLIST_NEXT(cur, as_link);
  }

  if(as)
    alert_source_refcount(as, 1);
  mutex_unlock(&alert_mutex);
  if(cur)
    alert_source_refcount(cur, -1);
  return as;
}

#define LEVELTBL "EMERG\0ALERT\0CRIT\0ERROR\0WARNING\0NOTICE\0INFO\0DEBUG\0\0"

const char *
alert_level_to_string(event_level_t level)
{
  return strtbl(LEVELTBL, level & 7);
}


typedef struct {
  alert_source_t as;
  atomic_t refcount;
  char key[0];
} fake_alert_t;


static void
fake_alert_message(const struct alert_source *as, struct stream *output)
{
  stprintf(output, "This is a test");
}


static event_level_t
fake_alert_level(const struct alert_source *as)
{
  return LOG_WARNING;
}

static void
fake_alert_refcount(struct alert_source *as, int value)
{
  fake_alert_t *fa = (fake_alert_t *)as;
  if(atomic_add_and_fetch(&fa->refcount, value) == 0)
    free(fa);
}


static const alert_class_t fake_alert_class = {
  .ac_message = fake_alert_message,
  .ac_level = fake_alert_level,
  .ac_refcount = fake_alert_refcount,
};


static error_t
raise_fake_alert(cli_t *cli, const char *key)
{
  size_t keylen = strlen(key);
  fake_alert_t *fa = xalloc(sizeof(fake_alert_t) + keylen + 1, 0,
                            MEM_CLEAR | MEM_MAY_FAIL);
  if(fa == NULL)
    return ERR_NO_MEMORY;

  memcpy(fa->key, key, keylen + 1);
  alert_register(&fa->as, &fake_alert_class, fa->key);

  alert_set(&fa->as, 1);
  cli_printf(cli, "Alert raised, press a key to clear ... ");
  cli_flush(cli);
  cli_getc(cli, 1);
  cli_printf(cli, "Done\n");
  alert_set(&fa->as, 0);
  alert_unregister(&fa->as);
  return 0;
}


static error_t
cmd_alert(cli_t *cli, int argc, char **argv)
{
  alert_source_t *as = NULL;

  if(argc == 3 && !strcmp(argv[1], "raise")) {
    return raise_fake_alert(cli, argv[2]);
  }

  while((as = alert_get_next(as)) != NULL) {
    cli_printf(cli, "%-20s ", as->as_key);

    if(!as->as_code) {
      cli_printf(cli, "No alert\n");
    } else {
      cli_printf(cli, "%-7s ",
                 alert_level_to_string(as->as_class->ac_level(as)));
      as->as_class->ac_message(as, cli->cl_stream);
      cli_printf(cli, "\n");
    }
  }
  return 0;
}

CLI_CMD_DEF("alert", cmd_alert);
