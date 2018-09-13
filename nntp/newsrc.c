/**
 * @file
 * Read/parse/write an NNTP config file of subscribed newsgroups
 *
 * @authors
 * Copyright (C) 1998 Brandon Long <blong@fiction.net>
 * Copyright (C) 1999 Andrej Gritsenko <andrej@lucky.net>
 * Copyright (C) 2000-2017 Vsevolod Volkov <vvv@mutt.org.ua>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page nntp_newsrc Read/parse/write an NNTP config file of subscribed newsgroups
 *
 * Read/parse/write an NNTP config file of subscribed newsgroups
 */

#include "config.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "nntp_private.h"
#include "mutt/mutt.h"
#include "email/lib.h"
#include "conn/conn.h"
#include "mutt.h"
#include "bcache.h"
#include "context.h"
#include "format_flags.h"
#include "globals.h"
#include "mailbox.h"
#include "mutt_account.h"
#include "mutt_logging.h"
#include "mutt_socket.h"
#include "mutt_window.h"
#include "muttlib.h"
#include "mx.h"
#include "nntp.h"
#include "protos.h"
#include "sort.h"
#ifdef USE_HCACHE
#include "hcache/hcache.h"
#endif

/* These Config Variables are only used in nntp/newsrc.c */
char *NewsCacheDir; ///< Config: (nntp) Directory for cached news articles
char *Newsrc; ///< Config: (nntp) File containing list of subscribed newsgroups

struct BodyCache;

/**
 * mdata_find - Find NntpMboxData for given newsgroup or add it
 * @param nserv NNTP server
 * @param group Newsgroup
 * @retval ptr  NNTP data
 * @retval NULL Error
 */
static struct NntpMboxData *mdata_find(struct NntpServer *nserv, const char *group)
{
  struct NntpMboxData *mdata = mutt_hash_find(nserv->groups_hash, group);
  if (mdata)
    return mdata;

  size_t len = strlen(group) + 1;
  /* create NntpMboxData structure and add it to hash */
  mdata = mutt_mem_calloc(1, sizeof(struct NntpMboxData) + len);
  mdata->group = (char *) mdata + sizeof(struct NntpMboxData);
  mutt_str_strfcpy(mdata->group, group, len);
  mdata->nserv = nserv;
  mdata->deleted = true;
  mutt_hash_insert(nserv->groups_hash, mdata->group, mdata);

  /* add NntpMboxData to list */
  if (nserv->groups_num >= nserv->groups_max)
  {
    nserv->groups_max *= 2;
    mutt_mem_realloc(&nserv->groups_list, nserv->groups_max * sizeof(mdata));
  }
  nserv->groups_list[nserv->groups_num++] = mdata;

  return mdata;
}

/**
 * nntp_acache_free - Remove all temporarily cache files
 * @param mdata NNTP Mailbox data
 */
void nntp_acache_free(struct NntpMboxData *mdata)
{
  for (int i = 0; i < NNTP_ACACHE_LEN; i++)
  {
    if (mdata->acache[i].path)
    {
      unlink(mdata->acache[i].path);
      FREE(&mdata->acache[i].path);
    }
  }
}

/**
 * nntp_data_free - Free NntpMboxData, used to destroy hash elements
 * @param data NNTP data
 */
void nntp_data_free(void *data)
{
  struct NntpMboxData *mdata = data;

  if (!mdata)
    return;
  nntp_acache_free(mdata);
  mutt_bcache_close(&mdata->bcache);
  FREE(&mdata->newsrc_ent);
  FREE(&mdata->desc);
  FREE(&data);
}

/**
 * nntp_hash_destructor_t - Free our hash table data - Implements ::hash_destructor_t
 */
void nntp_hash_destructor_t(int type, void *obj, intptr_t data)
{
  nntp_data_free(obj);
}

/**
 * nntp_newsrc_close - Unlock and close .newsrc file
 * @param nserv NNTP server
 */
void nntp_newsrc_close(struct NntpServer *nserv)
{
  if (!nserv->newsrc_fp)
    return;

  mutt_debug(1, "Unlocking %s\n", nserv->newsrc_file);
  mutt_file_unlock(fileno(nserv->newsrc_fp));
  mutt_file_fclose(&nserv->newsrc_fp);
}

/**
 * nntp_group_unread_stat - Count number of unread articles using .newsrc data
 * @param mdata NNTP Mailbox data
 */
void nntp_group_unread_stat(struct NntpMboxData *mdata)
{
  mdata->unread = 0;
  if (mdata->last_message == 0 || mdata->first_message > mdata->last_message)
    return;

  mdata->unread = mdata->last_message - mdata->first_message + 1;
  for (unsigned int i = 0; i < mdata->newsrc_len; i++)
  {
    anum_t first = mdata->newsrc_ent[i].first;
    if (first < mdata->first_message)
      first = mdata->first_message;
    anum_t last = mdata->newsrc_ent[i].last;
    if (last > mdata->last_message)
      last = mdata->last_message;
    if (first <= last)
      mdata->unread -= last - first + 1;
  }
}

/**
 * nntp_newsrc_parse - Parse .newsrc file
 * @param nserv NNTP server
 * @retval  0 Not changed
 * @retval  1 Parsed
 * @retval -1 Error
 */
int nntp_newsrc_parse(struct NntpServer *nserv)
{
  char *line = NULL;
  struct stat sb;

  if (nserv->newsrc_fp)
  {
    /* if we already have a handle, close it and reopen */
    mutt_file_fclose(&nserv->newsrc_fp);
  }
  else
  {
    /* if file doesn't exist, create it */
    nserv->newsrc_fp = mutt_file_fopen(nserv->newsrc_file, "a");
    mutt_file_fclose(&nserv->newsrc_fp);
  }

  /* open .newsrc */
  nserv->newsrc_fp = mutt_file_fopen(nserv->newsrc_file, "r");
  if (!nserv->newsrc_fp)
  {
    mutt_perror(nserv->newsrc_file);
    return -1;
  }

  /* lock it */
  mutt_debug(1, "Locking %s\n", nserv->newsrc_file);
  if (mutt_file_lock(fileno(nserv->newsrc_fp), false, true))
  {
    mutt_file_fclose(&nserv->newsrc_fp);
    return -1;
  }

  if (stat(nserv->newsrc_file, &sb))
  {
    mutt_perror(nserv->newsrc_file);
    nntp_newsrc_close(nserv);
    return -1;
  }

  if (nserv->size == sb.st_size && nserv->mtime == sb.st_mtime)
    return 0;

  nserv->size = sb.st_size;
  nserv->mtime = sb.st_mtime;
  nserv->newsrc_modified = true;
  mutt_debug(1, "Parsing %s\n", nserv->newsrc_file);

  /* .newsrc has been externally modified or hasn't been loaded yet */
  for (unsigned int i = 0; i < nserv->groups_num; i++)
  {
    struct NntpMboxData *mdata = nserv->groups_list[i];

    if (!mdata)
      continue;

    mdata->subscribed = false;
    mdata->newsrc_len = 0;
    FREE(&mdata->newsrc_ent);
  }

  line = mutt_mem_malloc(sb.st_size + 1);
  while (sb.st_size && fgets(line, sb.st_size + 1, nserv->newsrc_fp))
  {
    char *b = NULL, *h = NULL;
    unsigned int j = 1;
    bool subs = false;

    /* find end of newsgroup name */
    char *p = strpbrk(line, ":!");
    if (!p)
      continue;

    /* ":" - subscribed, "!" - unsubscribed */
    if (*p == ':')
      subs = true;
    *p++ = '\0';

    /* get newsgroup data */
    struct NntpMboxData *mdata = mdata_find(nserv, line);
    FREE(&mdata->newsrc_ent);

    /* count number of entries */
    b = p;
    while (*b)
      if (*b++ == ',')
        j++;
    mdata->newsrc_ent = mutt_mem_calloc(j, sizeof(struct NewsrcEntry));
    mdata->subscribed = subs;

    /* parse entries */
    j = 0;
    while (p)
    {
      b = p;

      /* find end of entry */
      p = strchr(p, ',');
      if (p)
        *p++ = '\0';

      /* first-last or single number */
      h = strchr(b, '-');
      if (h)
        *h++ = '\0';
      else
        h = b;

      if (sscanf(b, ANUM, &mdata->newsrc_ent[j].first) == 1 &&
          sscanf(h, ANUM, &mdata->newsrc_ent[j].last) == 1)
      {
        j++;
      }
    }
    if (j == 0)
    {
      mdata->newsrc_ent[j].first = 1;
      mdata->newsrc_ent[j].last = 0;
      j++;
    }
    if (mdata->last_message == 0)
      mdata->last_message = mdata->newsrc_ent[j - 1].last;
    mdata->newsrc_len = j;
    mutt_mem_realloc(&mdata->newsrc_ent, j * sizeof(struct NewsrcEntry));
    nntp_group_unread_stat(mdata);
    mutt_debug(2, "%s\n", mdata->group);
  }
  FREE(&line);
  return 1;
}

/**
 * nntp_newsrc_gen_entries - Generate array of .newsrc entries
 * @param ctx Mailbox
 */
void nntp_newsrc_gen_entries(struct Context *ctx)
{
  struct NntpMboxData *mdata = ctx->mailbox->data;
  anum_t last = 0, first = 1;
  bool series;
  int save_sort = SORT_ORDER;
  unsigned int entries;

  if (Sort != SORT_ORDER)
  {
    save_sort = Sort;
    Sort = SORT_ORDER;
    mutt_sort_headers(ctx, false);
  }

  entries = mdata->newsrc_len;
  if (!entries)
  {
    entries = 5;
    mdata->newsrc_ent = mutt_mem_calloc(entries, sizeof(struct NewsrcEntry));
  }

  /* Set up to fake initial sequence from 1 to the article before the
   * first article in our list */
  mdata->newsrc_len = 0;
  series = true;
  for (int i = 0; i < ctx->mailbox->msg_count; i++)
  {
    /* search for first unread */
    if (series)
    {
      /* We don't actually check sequential order, since we mark
       * "missing" entries as read/deleted */
      last = NNTP_EDATA(ctx->mailbox->hdrs[i])->article_num;
      if (last >= mdata->first_message && !ctx->mailbox->hdrs[i]->deleted &&
          !ctx->mailbox->hdrs[i]->read)
      {
        if (mdata->newsrc_len >= entries)
        {
          entries *= 2;
          mutt_mem_realloc(&mdata->newsrc_ent, entries * sizeof(struct NewsrcEntry));
        }
        mdata->newsrc_ent[mdata->newsrc_len].first = first;
        mdata->newsrc_ent[mdata->newsrc_len].last = last - 1;
        mdata->newsrc_len++;
        series = false;
      }
    }

    /* search for first read */
    else
    {
      if (ctx->mailbox->hdrs[i]->deleted || ctx->mailbox->hdrs[i]->read)
      {
        first = last + 1;
        series = true;
      }
      last = NNTP_EDATA(ctx->mailbox->hdrs[i])->article_num;
    }
  }

  if (series && first <= mdata->last_loaded)
  {
    if (mdata->newsrc_len >= entries)
    {
      entries++;
      mutt_mem_realloc(&mdata->newsrc_ent, entries * sizeof(struct NewsrcEntry));
    }
    mdata->newsrc_ent[mdata->newsrc_len].first = first;
    mdata->newsrc_ent[mdata->newsrc_len].last = mdata->last_loaded;
    mdata->newsrc_len++;
  }
  mutt_mem_realloc(&mdata->newsrc_ent, mdata->newsrc_len * sizeof(struct NewsrcEntry));

  if (save_sort != Sort)
  {
    Sort = save_sort;
    mutt_sort_headers(ctx, false);
  }
}

/**
 * update_file - Update file with new contents
 * @param filename File to update
 * @param buf      New context
 * @retval  0 Success
 * @retval -1 Failure
 */
static int update_file(char *filename, char *buf)
{
  FILE *fp = NULL;
  char tmpfile[PATH_MAX];
  int rc = -1;

  while (true)
  {
    snprintf(tmpfile, sizeof(tmpfile), "%s.tmp", filename);
    fp = mutt_file_fopen(tmpfile, "w");
    if (!fp)
    {
      mutt_perror(tmpfile);
      *tmpfile = '\0';
      break;
    }
    if (fputs(buf, fp) == EOF)
    {
      mutt_perror(tmpfile);
      break;
    }
    if (mutt_file_fclose(&fp) == EOF)
    {
      mutt_perror(tmpfile);
      fp = NULL;
      break;
    }
    fp = NULL;
    if (rename(tmpfile, filename) < 0)
    {
      mutt_perror(filename);
      break;
    }
    *tmpfile = '\0';
    rc = 0;
    break;
  }
  if (fp)
    mutt_file_fclose(&fp);
  if (*tmpfile)
    unlink(tmpfile);
  return rc;
}

/**
 * nntp_newsrc_update - Update .newsrc file
 * @param nserv NNTP server
 * @retval  0 Success
 * @retval -1 Failure
 */
int nntp_newsrc_update(struct NntpServer *nserv)
{
  char *buf = NULL;
  size_t buflen, off;
  int rc = -1;

  if (!nserv)
    return -1;

  buflen = 10 * LONG_STRING;
  buf = mutt_mem_calloc(1, buflen);
  off = 0;

  /* we will generate full newsrc here */
  for (unsigned int i = 0; i < nserv->groups_num; i++)
  {
    struct NntpMboxData *mdata = nserv->groups_list[i];

    if (!mdata || !mdata->newsrc_ent)
      continue;

    /* write newsgroup name */
    if (off + strlen(mdata->group) + 3 > buflen)
    {
      buflen *= 2;
      mutt_mem_realloc(&buf, buflen);
    }
    snprintf(buf + off, buflen - off, "%s%c ", mdata->group,
             mdata->subscribed ? ':' : '!');
    off += strlen(buf + off);

    /* write entries */
    for (unsigned int j = 0; j < mdata->newsrc_len; j++)
    {
      if (off + LONG_STRING > buflen)
      {
        buflen *= 2;
        mutt_mem_realloc(&buf, buflen);
      }
      if (j)
        buf[off++] = ',';
      if (mdata->newsrc_ent[j].first == mdata->newsrc_ent[j].last)
        snprintf(buf + off, buflen - off, "%u", mdata->newsrc_ent[j].first);
      else if (mdata->newsrc_ent[j].first < mdata->newsrc_ent[j].last)
      {
        snprintf(buf + off, buflen - off, "%u-%u",
                 mdata->newsrc_ent[j].first, mdata->newsrc_ent[j].last);
      }
      off += strlen(buf + off);
    }
    buf[off++] = '\n';
  }
  buf[off] = '\0';

  /* newrc being fully rewritten */
  mutt_debug(1, "Updating %s\n", nserv->newsrc_file);
  if (nserv->newsrc_file && update_file(nserv->newsrc_file, buf) == 0)
  {
    struct stat sb;

    rc = stat(nserv->newsrc_file, &sb);
    if (rc == 0)
    {
      nserv->size = sb.st_size;
      nserv->mtime = sb.st_mtime;
    }
    else
    {
      mutt_perror(nserv->newsrc_file);
    }
  }
  FREE(&buf);
  return rc;
}

/**
 * cache_expand - Make fully qualified cache file name
 * @param dst    Buffer for filename
 * @param dstlen Length of buffer
 * @param acct   Account
 * @param src    Path to add to the URL
 */
static void cache_expand(char *dst, size_t dstlen, struct ConnAccount *acct, const char *src)
{
  char *c = NULL;
  char file[PATH_MAX];

  /* server subdirectory */
  if (acct)
  {
    struct Url url;

    mutt_account_tourl(acct, &url);
    url.path = mutt_str_strdup(src);
    url_tostring(&url, file, sizeof(file), U_PATH);
  }
  else
    mutt_str_strfcpy(file, src ? src : "", sizeof(file));

  snprintf(dst, dstlen, "%s/%s", NewsCacheDir, file);

  /* remove trailing slash */
  c = dst + strlen(dst) - 1;
  if (*c == '/')
    *c = '\0';
  mutt_expand_path(dst, dstlen);
  mutt_encode_path(dst, dstlen, dst);
}

/**
 * nntp_expand_path - Make fully qualified url from newsgroup name
 * @param buf    Buffer for the result
 * @param buflen Length of buffer
 * @param acct Account to serialise
 */
void nntp_expand_path(char *buf, size_t buflen, struct ConnAccount *acct)
{
  struct Url url;

  mutt_account_tourl(acct, &url);
  url.path = mutt_str_strdup(buf);
  url_tostring(&url, buf, buflen, 0);
  FREE(&url.path);
}

/**
 * nntp_add_group - Parse newsgroup
 * @param line String to parse
 * @param data NNTP data
 * @retval 0 Always
 */
int nntp_add_group(char *line, void *data)
{
  struct NntpServer *nserv = data;
  struct NntpMboxData *mdata = NULL;
  char group[LONG_STRING] = "";
  char desc[HUGE_STRING] = "";
  char mod;
  anum_t first, last;

  if (!nserv || !line)
    return 0;

  /* These sscanf limits must match the sizes of the group and desc arrays */
  if (sscanf(line, "%1023s " ANUM " " ANUM " %c %8191[^\n]", group, &last,
             &first, &mod, desc) < 4)
  {
    mutt_debug(4, "Cannot parse server line: %s\n", line);
    return 0;
  }

  mdata = mdata_find(nserv, group);
  mdata->deleted = false;
  mdata->first_message = first;
  mdata->last_message = last;
  mdata->allowed = (mod == 'y') || (mod == 'm');
  mutt_str_replace(&mdata->desc, desc);
  if (mdata->newsrc_ent || mdata->last_cached)
    nntp_group_unread_stat(mdata);
  else if (mdata->last_message && mdata->first_message <= mdata->last_message)
    mdata->unread = mdata->last_message - mdata->first_message + 1;
  else
    mdata->unread = 0;
  return 0;
}

/**
 * active_get_cache - Load list of all newsgroups from cache
 * @param nserv NNTP server
 * @retval  0 Success
 * @retval -1 Failure
 */
static int active_get_cache(struct NntpServer *nserv)
{
  char buf[HUGE_STRING];
  char file[PATH_MAX];
  time_t t;

  cache_expand(file, sizeof(file), &nserv->conn->account, ".active");
  mutt_debug(1, "Parsing %s\n", file);
  FILE *fp = mutt_file_fopen(file, "r");
  if (!fp)
    return -1;

  if (!fgets(buf, sizeof(buf), fp) || (sscanf(buf, "%ld%s", &t, file) != 1) || (t == 0))
  {
    mutt_file_fclose(&fp);
    return -1;
  }
  nserv->newgroups_time = t;

  mutt_message(_("Loading list of groups from cache..."));
  while (fgets(buf, sizeof(buf), fp))
    nntp_add_group(buf, nserv);
  nntp_add_group(NULL, NULL);
  mutt_file_fclose(&fp);
  mutt_clear_error();
  return 0;
}

/**
 * nntp_active_save_cache - Save list of all newsgroups to cache
 * @param nserv NNTP server
 * @retval  0 Success
 * @retval -1 Failure
 */
int nntp_active_save_cache(struct NntpServer *nserv)
{
  char file[PATH_MAX];
  char *buf = NULL;
  size_t buflen, off;
  int rc;

  if (!nserv->cacheable)
    return 0;

  buflen = 10 * LONG_STRING;
  buf = mutt_mem_calloc(1, buflen);
  snprintf(buf, buflen, "%lu\n", (unsigned long) nserv->newgroups_time);
  off = strlen(buf);

  for (unsigned int i = 0; i < nserv->groups_num; i++)
  {
    struct NntpMboxData *mdata = nserv->groups_list[i];

    if (!mdata || mdata->deleted)
      continue;

    if (off + strlen(mdata->group) + (mdata->desc ? strlen(mdata->desc) : 0) + 50 > buflen)
    {
      buflen *= 2;
      mutt_mem_realloc(&buf, buflen);
    }
    snprintf(buf + off, buflen - off, "%s %u %u %c%s%s\n", mdata->group,
             mdata->last_message, mdata->first_message,
             mdata->allowed ? 'y' : 'n', mdata->desc ? " " : "",
             mdata->desc ? mdata->desc : "");
    off += strlen(buf + off);
  }

  cache_expand(file, sizeof(file), &nserv->conn->account, ".active");
  mutt_debug(1, "Updating %s\n", file);
  rc = update_file(file, buf);
  FREE(&buf);
  return rc;
}

#ifdef USE_HCACHE
/**
 * nntp_hcache_namer - Compose hcache file names - Implements ::hcache_namer_t
 */
static int nntp_hcache_namer(const char *path, char *dest, size_t destlen)
{
  int count = snprintf(dest, destlen, "%s.hcache", path);

  /* Strip out any directories in the path */
  char *first = strchr(dest, '/');
  char *last = strrchr(dest, '/');
  if (first && last && (last > first))
  {
    memmove(first, last, strlen(last) + 1);
    count -= (last - first);
  }

  return count;
}

/**
 * nntp_hcache_open - Open newsgroup hcache
 * @param mdata NNTP Mailbox data
 * @retval ptr  Header cache
 * @retval NULL Error
 */
header_cache_t *nntp_hcache_open(struct NntpMboxData *mdata)
{
  struct Url url;
  char file[PATH_MAX];

  if (!mdata->nserv || !mdata->nserv->cacheable ||
      !mdata->nserv->conn || !mdata->group ||
      !(mdata->newsrc_ent || mdata->subscribed || SaveUnsubscribed))
  {
    return NULL;
  }

  mutt_account_tourl(&mdata->nserv->conn->account, &url);
  url.path = mdata->group;
  url_tostring(&url, file, sizeof(file), U_PATH);
  return mutt_hcache_open(NewsCacheDir, file, nntp_hcache_namer);
}

/**
 * nntp_hcache_update - Remove stale cached headers
 * @param mdata NNTP Mailbox data
 * @param hc    Header cache
 */
void nntp_hcache_update(struct NntpMboxData *mdata, header_cache_t *hc)
{
  char buf[16];
  bool old = false;
  void *hdata = NULL;
  anum_t first = 0, last = 0;

  if (!hc)
    return;

  /* fetch previous values of first and last */
  hdata = mutt_hcache_fetch_raw(hc, "index", 5);
  if (hdata)
  {
    mutt_debug(2, "mutt_hcache_fetch index: %s\n", (char *) hdata);
    if (sscanf(hdata, ANUM " " ANUM, &first, &last) == 2)
    {
      old = true;
      mdata->last_cached = last;

      /* clean removed headers from cache */
      for (anum_t current = first; current <= last; current++)
      {
        if (current >= mdata->first_message && current <= mdata->last_message)
          continue;

        snprintf(buf, sizeof(buf), "%u", current);
        mutt_debug(2, "mutt_hcache_delete %s\n", buf);
        mutt_hcache_delete(hc, buf, strlen(buf));
      }
    }
    mutt_hcache_free(hc, &hdata);
  }

  /* store current values of first and last */
  if (!old || mdata->first_message != first || mdata->last_message != last)
  {
    snprintf(buf, sizeof(buf), "%u %u", mdata->first_message, mdata->last_message);
    mutt_debug(2, "mutt_hcache_store index: %s\n", buf);
    mutt_hcache_store_raw(hc, "index", 5, buf, strlen(buf));
  }
}
#endif

/**
 * nntp_bcache_delete - Remove bcache file - Implements ::bcache_list_t
 * @retval 0 Always
 */
static int nntp_bcache_delete(const char *id, struct BodyCache *bcache, void *data)
{
  struct NntpMboxData *mdata = data;
  anum_t anum;
  char c;

  if (!mdata || sscanf(id, ANUM "%c", &anum, &c) != 1 ||
      anum < mdata->first_message || anum > mdata->last_message)
  {
    if (mdata)
      mutt_debug(2, "mutt_bcache_del %s\n", id);
    mutt_bcache_del(bcache, id);
  }
  return 0;
}

/**
 * nntp_bcache_update - Remove stale cached messages
 * @param mdata NNTP Mailbox data
 */
void nntp_bcache_update(struct NntpMboxData *mdata)
{
  mutt_bcache_list(mdata->bcache, nntp_bcache_delete, mdata);
}

/**
 * nntp_delete_group_cache - Remove hcache and bcache of newsgroup
 * @param mdata NNTP Mailbox data
 */
void nntp_delete_group_cache(struct NntpMboxData *mdata)
{
  if (!mdata || !mdata->nserv || !mdata->nserv->cacheable)
    return;

#ifdef USE_HCACHE
  char file[PATH_MAX];
  nntp_hcache_namer(mdata->group, file, sizeof(file));
  cache_expand(file, sizeof(file), &mdata->nserv->conn->account, file);
  unlink(file);
  mdata->last_cached = 0;
  mutt_debug(2, "%s\n", file);
#endif

  if (!mdata->bcache)
  {
    mdata->bcache = mutt_bcache_open(&mdata->nserv->conn->account, mdata->group);
  }
  if (mdata->bcache)
  {
    mutt_debug(2, "%s/*\n", mdata->group);
    mutt_bcache_list(mdata->bcache, nntp_bcache_delete, NULL);
    mutt_bcache_close(&mdata->bcache);
  }
}

/**
 * nntp_clear_cache - Clear the NNTP cache
 * @param nserv NNTP server
 *
 * Remove hcache and bcache of all unexistent and unsubscribed newsgroups
 */
void nntp_clear_cache(struct NntpServer *nserv)
{
  char file[PATH_MAX];
  char *fp = NULL;
  struct dirent *entry = NULL;
  DIR *dp = NULL;

  if (!nserv || !nserv->cacheable)
    return;

  cache_expand(file, sizeof(file), &nserv->conn->account, NULL);
  dp = opendir(file);
  if (dp)
  {
    mutt_str_strncat(file, sizeof(file), "/", 1);
    fp = file + strlen(file);
    while ((entry = readdir(dp)))
    {
      char *group = entry->d_name;
      struct stat sb;
      struct NntpMboxData *mdata = NULL;
      struct NntpMboxData nntp_tmp;

      if ((mutt_str_strcmp(group, ".") == 0) || (mutt_str_strcmp(group, "..") == 0))
        continue;
      *fp = '\0';
      mutt_str_strncat(file, sizeof(file), group, strlen(group));
      if (stat(file, &sb))
        continue;

#ifdef USE_HCACHE
      if (S_ISREG(sb.st_mode))
      {
        char *ext = group + strlen(group) - 7;
        if (strlen(group) < 8 || (mutt_str_strcmp(ext, ".hcache") != 0))
          continue;
        *ext = '\0';
      }
      else
#endif
          if (!S_ISDIR(sb.st_mode))
        continue;

      mdata = mutt_hash_find(nserv->groups_hash, group);
      if (!mdata)
      {
        mdata = &nntp_tmp;
        mdata->nserv = nserv;
        mdata->group = group;
        mdata->bcache = NULL;
      }
      else if (mdata->newsrc_ent || mdata->subscribed || SaveUnsubscribed)
        continue;

      nntp_delete_group_cache(mdata);
      if (S_ISDIR(sb.st_mode))
      {
        rmdir(file);
        mutt_debug(2, "%s\n", file);
      }
    }
    closedir(dp);
  }
}

/**
 * nntp_format_str - Expand the newsrc filename - Implements ::format_t
 *
 * | Expando | Description
 * |:--------|:--------------------------------------------------------
 * | \%a     | Account url
 * | \%p     | Port
 * | \%P     | Port if specified
 * | \%s     | News server name
 * | \%S     | Url schema
 * | \%u     | Username
 */
const char *nntp_format_str(char *buf, size_t buflen, size_t col, int cols, char op,
                            const char *src, const char *prec, const char *if_str,
                            const char *else_str, unsigned long data, enum FormatFlag flags)
{
  struct NntpServer *nserv = (struct NntpServer *) data;
  struct ConnAccount *acct = &nserv->conn->account;
  struct Url url;
  char fn[SHORT_STRING], fmt[SHORT_STRING], *p = NULL;

  switch (op)
  {
    case 'a':
      mutt_account_tourl(acct, &url);
      url_tostring(&url, fn, sizeof(fn), U_PATH);
      p = strchr(fn, '/');
      if (p)
        *p = '\0';
      snprintf(fmt, sizeof(fmt), "%%%ss", prec);
      snprintf(buf, buflen, fmt, fn);
      break;
    case 'p':
      snprintf(fmt, sizeof(fmt), "%%%su", prec);
      snprintf(buf, buflen, fmt, acct->port);
      break;
    case 'P':
      *buf = '\0';
      if (acct->flags & MUTT_ACCT_PORT)
      {
        snprintf(fmt, sizeof(fmt), "%%%su", prec);
        snprintf(buf, buflen, fmt, acct->port);
      }
      break;
    case 's':
      strncpy(fn, acct->host, sizeof(fn) - 1);
      mutt_str_strlower(fn);
      snprintf(fmt, sizeof(fmt), "%%%ss", prec);
      snprintf(buf, buflen, fmt, fn);
      break;
    case 'S':
      mutt_account_tourl(acct, &url);
      url_tostring(&url, fn, sizeof(fn), U_PATH);
      p = strchr(fn, ':');
      if (p)
        *p = '\0';
      snprintf(fmt, sizeof(fmt), "%%%ss", prec);
      snprintf(buf, buflen, fmt, fn);
      break;
    case 'u':
      snprintf(fmt, sizeof(fmt), "%%%ss", prec);
      snprintf(buf, buflen, fmt, acct->user);
      break;
  }
  return src;
}

/**
 * nntp_select_server - Open a connection to an NNTP server
 * @param mailbox    Mailbox
 * @param server     Server URI
 * @param leave_lock Leave the server locked?
 * @retval ptr  NNTP server
 * @retval NULL Error
 *
 * Automatically loads a newsrc into memory, if necessary.  Checks the
 * size/mtime of a newsrc file, if it doesn't match, load again.  Hmm, if a
 * system has broken mtimes, this might mean the file is reloaded every time,
 * which we'd have to fix.
 */
struct NntpServer *nntp_select_server(struct Mailbox *mailbox, char *server, bool leave_lock)
{
  char file[PATH_MAX];
#ifdef USE_HCACHE
  char *p = NULL;
#endif
  int rc;
  struct ConnAccount acct;
  struct NntpServer *nserv = NULL;
  struct NntpMboxData *mdata = NULL;
  struct Connection *conn = NULL;
  struct Url url;

  if (!server || !*server)
  {
    mutt_error(_("No news server defined"));
    return NULL;
  }

  /* create account from news server url */
  acct.flags = 0;
  acct.port = NNTP_PORT;
  acct.type = MUTT_ACCT_TYPE_NNTP;
  snprintf(file, sizeof(file), "%s%s", strstr(server, "://") ? "" : "news://", server);
  if (url_parse(&url, file) < 0 || (url.path && *url.path) ||
      !(url.scheme == U_NNTP || url.scheme == U_NNTPS) || !url.host ||
      mutt_account_fromurl(&acct, &url) < 0)
  {
    url_free(&url);
    mutt_error(_("%s is an invalid news server specification"), server);
    return NULL;
  }
  if (url.scheme == U_NNTPS)
  {
    acct.flags |= MUTT_ACCT_SSL;
    acct.port = NNTP_SSL_PORT;
  }
  url_free(&url);

  /* find connection by account */
  conn = mutt_conn_find(NULL, &acct);
  if (!conn)
    return NULL;
  if (!(conn->account.flags & MUTT_ACCT_USER) && acct.flags & MUTT_ACCT_USER)
  {
    conn->account.flags |= MUTT_ACCT_USER;
    conn->account.user[0] = '\0';
  }

  /* news server already exists */
  nserv = conn->data;
  if (nserv)
  {
    if (nserv->status == NNTP_BYE)
      nserv->status = NNTP_NONE;
    if (nntp_open_connection(nserv) < 0)
      return NULL;

    rc = nntp_newsrc_parse(nserv);
    if (rc < 0)
      return NULL;

    /* check for new newsgroups */
    if (!leave_lock && nntp_check_new_groups(mailbox, nserv) < 0)
      rc = -1;

    /* .newsrc has been externally modified */
    if (rc > 0)
      nntp_clear_cache(nserv);
    if (rc < 0 || !leave_lock)
      nntp_newsrc_close(nserv);
    return (rc < 0) ? NULL : nserv;
  }

  /* new news server */
  nserv = mutt_mem_calloc(1, sizeof(struct NntpServer));
  nserv->conn = conn;
  nserv->groups_hash = mutt_hash_create(1009, 0);
  mutt_hash_set_destructor(nserv->groups_hash, nntp_hash_destructor_t, 0);
  nserv->groups_max = 16;
  nserv->groups_list = mutt_mem_malloc(nserv->groups_max * sizeof(mdata));

  rc = nntp_open_connection(nserv);

  /* try to create cache directory and enable caching */
  nserv->cacheable = false;
  if (rc >= 0 && NewsCacheDir && *NewsCacheDir)
  {
    cache_expand(file, sizeof(file), &conn->account, NULL);
    if (mutt_file_mkdir(file, S_IRWXU) < 0)
    {
      mutt_error(_("Can't create %s: %s"), file, strerror(errno));
    }
    nserv->cacheable = true;
  }

  /* load .newsrc */
  if (rc >= 0)
  {
    mutt_expando_format(file, sizeof(file), 0, MuttIndexWindow->cols,
                        NONULL(Newsrc), nntp_format_str, (unsigned long) nserv, 0);
    mutt_expand_path(file, sizeof(file));
    nserv->newsrc_file = mutt_str_strdup(file);
    rc = nntp_newsrc_parse(nserv);
  }
  if (rc >= 0)
  {
    /* try to load list of newsgroups from cache */
    if (nserv->cacheable && active_get_cache(nserv) == 0)
      rc = nntp_check_new_groups(mailbox, nserv);

    /* load list of newsgroups from server */
    else
      rc = nntp_active_fetch(nserv, false);
  }

  if (rc >= 0)
    nntp_clear_cache(nserv);

#ifdef USE_HCACHE
  /* check cache files */
  if (rc >= 0 && nserv->cacheable)
  {
    struct dirent *entry = NULL;
    DIR *dp = opendir(file);

    if (dp)
    {
      while ((entry = readdir(dp)))
      {
        header_cache_t *hc = NULL;
        void *hdata = NULL;
        char *group = entry->d_name;

        p = group + strlen(group) - 7;
        if (strlen(group) < 8 || (strcmp(p, ".hcache") != 0))
          continue;
        *p = '\0';
        mdata = mutt_hash_find(nserv->groups_hash, group);
        if (!mdata)
          continue;

        hc = nntp_hcache_open(mdata);
        if (!hc)
          continue;

        /* fetch previous values of first and last */
        hdata = mutt_hcache_fetch_raw(hc, "index", 5);
        if (hdata)
        {
          anum_t first, last;

          if (sscanf(hdata, ANUM " " ANUM, &first, &last) == 2)
          {
            if (mdata->deleted)
            {
              mdata->first_message = first;
              mdata->last_message = last;
            }
            if (last >= mdata->first_message && last <= mdata->last_message)
            {
              mdata->last_cached = last;
              mutt_debug(2, "%s last_cached=%u\n", mdata->group, last);
            }
          }
          mutt_hcache_free(hc, &hdata);
        }
        mutt_hcache_close(hc);
      }
      closedir(dp);
    }
  }
#endif

  if (rc < 0 || !leave_lock)
    nntp_newsrc_close(nserv);

  if (rc < 0)
  {
    mutt_hash_destroy(&nserv->groups_hash);
    FREE(&nserv->groups_list);
    FREE(&nserv->newsrc_file);
    FREE(&nserv->authenticators);
    FREE(&nserv);
    mutt_socket_close(conn);
    mutt_socket_free(conn);
    return NULL;
  }

  conn->data = nserv;
  return nserv;
}

/**
 * nntp_article_status - Get status of articles from .newsrc
 * @param mailbox Mailbox
 * @param e       Email
 * @param group   Newsgroup
 * @param anum    Article number
 *
 * Full status flags are not supported by nntp, but we can fake some of them:
 * Read = a read message number is in the .newsrc
 * New = not read and not cached
 * Old = not read but cached
 */
void nntp_article_status(struct Mailbox *mailbox, struct Email *e, char *group, anum_t anum)
{
  struct NntpMboxData *mdata = mailbox->data;

  if (group)
    mdata = mutt_hash_find(mdata->nserv->groups_hash, group);

  if (!mdata)
    return;

  for (unsigned int i = 0; i < mdata->newsrc_len; i++)
  {
    if ((anum >= mdata->newsrc_ent[i].first) &&
        (anum <= mdata->newsrc_ent[i].last))
    {
      /* can't use mutt_set_flag() because mx_update_context()
         didn't get called yet */
      e->read = true;
      return;
    }
  }

  /* article was not cached yet, it's new */
  if (anum > mdata->last_cached)
    return;

  /* article isn't read but cached, it's old */
  if (MarkOld)
    e->old = true;
}

/**
 * mutt_newsgroup_subscribe - Subscribe newsgroup
 * @param nserv NNTP server
 * @param group Newsgroup
 * @retval ptr  NNTP data
 * @retval NULL Error
 */
struct NntpMboxData *mutt_newsgroup_subscribe(struct NntpServer *nserv, char *group)
{
  struct NntpMboxData *mdata = NULL;

  if (!nserv || !nserv->groups_hash || !group || !*group)
    return NULL;

  mdata = mdata_find(nserv, group);
  mdata->subscribed = true;
  if (!mdata->newsrc_ent)
  {
    mdata->newsrc_ent = mutt_mem_calloc(1, sizeof(struct NewsrcEntry));
    mdata->newsrc_len = 1;
    mdata->newsrc_ent[0].first = 1;
    mdata->newsrc_ent[0].last = 0;
  }
  return mdata;
}

/**
 * mutt_newsgroup_unsubscribe - Unsubscribe newsgroup
 * @param nserv NNTP server
 * @param group Newsgroup
 * @retval ptr  NNTP data
 * @retval NULL Error
 */
struct NntpMboxData *mutt_newsgroup_unsubscribe(struct NntpServer *nserv, char *group)
{
  struct NntpMboxData *mdata = NULL;

  if (!nserv || !nserv->groups_hash || !group || !*group)
    return NULL;

  mdata = mutt_hash_find(nserv->groups_hash, group);
  if (!mdata)
    return NULL;

  mdata->subscribed = false;
  if (!SaveUnsubscribed)
  {
    mdata->newsrc_len = 0;
    FREE(&mdata->newsrc_ent);
  }
  return mdata;
}

/**
 * mutt_newsgroup_catchup - Catchup newsgroup
 * @param ctx   Mailbox
 * @param nserv NNTP server
 * @param group Newsgroup
 * @retval ptr  NNTP data
 * @retval NULL Error
 */
struct NntpMboxData *mutt_newsgroup_catchup(struct Context *ctx,
                                        struct NntpServer *nserv, char *group)
{
  struct NntpMboxData *mdata = NULL;

  if (!nserv || !nserv->groups_hash || !group || !*group)
    return NULL;

  mdata = mutt_hash_find(nserv->groups_hash, group);
  if (!mdata)
    return NULL;

  if (mdata->newsrc_ent)
  {
    mutt_mem_realloc(&mdata->newsrc_ent, sizeof(struct NewsrcEntry));
    mdata->newsrc_len = 1;
    mdata->newsrc_ent[0].first = 1;
    mdata->newsrc_ent[0].last = mdata->last_message;
  }
  mdata->unread = 0;
  if (ctx && ctx->mailbox->data == mdata)
  {
    for (unsigned int i = 0; i < ctx->mailbox->msg_count; i++)
      mutt_set_flag(ctx, ctx->mailbox->hdrs[i], MUTT_READ, 1);
  }
  return mdata;
}

/**
 * mutt_newsgroup_uncatchup - Uncatchup newsgroup
 * @param ctx   Mailbox
 * @param nserv NNTP server
 * @param group Newsgroup
 * @retval ptr  NNTP data
 * @retval NULL Error
 */
struct NntpMboxData *mutt_newsgroup_uncatchup(struct Context *ctx,
                                          struct NntpServer *nserv, char *group)
{
  struct NntpMboxData *mdata = NULL;

  if (!nserv || !nserv->groups_hash || !group || !*group)
    return NULL;

  mdata = mutt_hash_find(nserv->groups_hash, group);
  if (!mdata)
    return NULL;

  if (mdata->newsrc_ent)
  {
    mutt_mem_realloc(&mdata->newsrc_ent, sizeof(struct NewsrcEntry));
    mdata->newsrc_len = 1;
    mdata->newsrc_ent[0].first = 1;
    mdata->newsrc_ent[0].last = mdata->first_message - 1;
  }
  if (ctx && ctx->mailbox->data == mdata)
  {
    mdata->unread = ctx->mailbox->msg_count;
    for (unsigned int i = 0; i < ctx->mailbox->msg_count; i++)
      mutt_set_flag(ctx, ctx->mailbox->hdrs[i], MUTT_READ, 0);
  }
  else
  {
    mdata->unread = mdata->last_message;
    if (mdata->newsrc_ent)
      mdata->unread -= mdata->newsrc_ent[0].last;
  }
  return mdata;
}

/**
 * nntp_mailbox - Get first newsgroup with new messages
 * @param mailbox Mailbox
 * @param buf     Buffer for result
 * @param buflen  Length of buffer
 */
void nntp_mailbox(struct Mailbox *mailbox, char *buf, size_t buflen)
{
  if (!mailbox)
    return;

  for (unsigned int i = 0; i < CurrentNewsSrv->groups_num; i++)
  {
    struct NntpMboxData *mdata = CurrentNewsSrv->groups_list[i];

    if (!mdata || !mdata->subscribed || !mdata->unread)
      continue;

    if ((mailbox->magic == MUTT_NNTP) &&
        (mutt_str_strcmp(mdata->group, ((struct NntpMboxData *) mailbox->data)->group) == 0))
    {
      unsigned int unread = 0;

      for (unsigned int j = 0; j < mailbox->msg_count; j++)
        if (!mailbox->hdrs[j]->read && !mailbox->hdrs[j]->deleted)
          unread++;
      if (!unread)
        continue;
    }
    mutt_str_strfcpy(buf, mdata->group, buflen);
    break;
  }
}
