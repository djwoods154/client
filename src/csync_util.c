/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2006 by Andreas Schneider <mail@cynapses.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * vim: ts=2 sw=2 et cindent
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>

#include "csync_util.h"
#include "vio/csync_vio.h"

#define CSYNC_LOG_CATEGORY_NAME "csync.util"
#include "csync_log.h"

typedef struct {
  const char *instr_str;
  enum csync_instructions_e instr_code;
} _instr_code_struct;

static const _instr_code_struct _instr[] =
{
  { "CSYNC_INSTRUCTION_NONE", CSYNC_INSTRUCTION_NONE },
  { "CSYNC_INSTRUCTION_EVAL", CSYNC_INSTRUCTION_EVAL },
  { "CSYNC_INSTRUCTION_REMOVE", CSYNC_INSTRUCTION_REMOVE },
  { "CSYNC_INSTRUCTION_RENAME", CSYNC_INSTRUCTION_RENAME },
  { "CSYNC_INSTRUCTION_NEW", CSYNC_INSTRUCTION_NEW },
  { "CSYNC_INSTRUCTION_CONFLICT", CSYNC_INSTRUCTION_CONFLICT },
  { "CSYNC_INSTRUCTION_IGNORE", CSYNC_INSTRUCTION_IGNORE },
  { "CSYNC_INSTRUCTION_SYNC", CSYNC_INSTRUCTION_SYNC },
  { "CSYNC_INSTRUCTION_STAT_ERROR", CSYNC_INSTRUCTION_STAT_ERROR },
  { "CSYNC_INSTRUCTION_ERROR", CSYNC_INSTRUCTION_ERROR },
  { "CSYNC_INSTRUCTION_DELETED", CSYNC_INSTRUCTION_DELETED },
  { "CSYNC_INSTRUCTION_UPDATED", CSYNC_INSTRUCTION_UPDATED },
  { NULL, CSYNC_INSTRUCTION_ERROR }
};

struct csync_memstat_s {
  int size;
  int resident;
  int shared;
  int trs;
  int drs;
  int lrs;
  int dt;
};

const char *csync_instruction_str(enum csync_instructions_e instr)
{
  int idx = 0;

  while (_instr[idx].instr_str != NULL) {
    if (_instr[idx].instr_code == instr) {
      return _instr[idx].instr_str;
    }
    idx++;
  }

  return "ERROR!";
}


void csync_memstat_check(void) {
  int s = 0;
  struct csync_memstat_s m;
  FILE* fp;

  /* get process memory stats */
  fp = fopen("/proc/self/statm","r");
  if (fp == NULL) {
    return;
  }
  s = fscanf(fp, "%d%d%d%d%d%d%d", &m.size, &m.resident, &m.shared, &m.trs,
      &m.drs, &m.lrs, &m.dt);
  fclose(fp);
  if (s == EOF) {
    return;
  }

  CSYNC_LOG(CSYNC_LOG_PRIORITY_INFO, "Memory: %dK total size, %dK resident, %dK shared",
                 m.size * 4, m.resident * 4, m.shared * 4);
}

static int _merge_file_trees_visitor(void *obj, void *data) {
  csync_file_stat_t *fs = NULL;
  csync_vio_file_stat_t *vst = NULL;

  CSYNC *ctx = NULL;
  c_rbtree_t *tree = NULL;
  c_rbnode_t *node = NULL;

  char *uri = NULL;
  int rc = -1;

  fs = (csync_file_stat_t *) obj;
  ctx = (CSYNC *) data;

  /* search for UPDATED file */
  if (fs->instruction != CSYNC_INSTRUCTION_UPDATED) {
    rc = 0;
    goto out;
  }

  switch (ctx->current) {
    case LOCAL_REPLICA:
      tree = ctx->local.tree;
      break;
    case REMOTE_REPLCIA:
      tree = ctx->remote.tree;
      break;
    default:
      break;
  }

  /* check if the file is new or has been synced */
  node = c_rbtree_find(tree, &fs->phash);
  if (node == NULL) {
    csync_file_stat_t *new = NULL;

    new = c_malloc(sizeof(csync_file_stat_t) + fs->pathlen + 1);
    if (new == NULL) {
      rc = -1;
      goto out;
    }
    new = memcpy(new, fs, sizeof(csync_file_stat_t) + fs->pathlen + 1);

    if (c_rbtree_insert(tree, new) < 0) {
      SAFE_FREE(new);
      rc = -1;
      goto out;
    }

    node = c_rbtree_find(tree, &fs->phash);
    if (node == NULL) {
      rc = -1;
      goto out;
    }
  }
  fs = c_rbtree_node_data(node);

  switch (ctx->current) {
    case LOCAL_REPLICA:
      if (asprintf(&uri, "%s/%s", ctx->local.uri, fs->path) < 0) {
        rc =-1;
        goto out;
      }
      break;
    case REMOTE_REPLCIA:
      if (asprintf(&uri, "%s/%s", ctx->remote.uri, fs->path) < 0) {
        rc = -1;
        goto out;
      }
      break;
    default:
      break;
  }

  /* get file stat of the file on the replica */
  vst = csync_vio_file_stat_new();
  if (csync_vio_stat(ctx, uri, vst) < 0) {
    rc = -1;
    goto out;
  }

  /* update file stat */
  fs->inode = vst->inode;
  fs->modtime = vst->mtime;

  CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG, "file: %s, instruction: UPDATED", uri);

  fs->instruction = CSYNC_INSTRUCTION_NONE;

  rc = 0;
out:
  csync_vio_file_stat_destroy(vst);
  SAFE_FREE(uri);

  if (rc != 0) {
    fs->instruction = CSYNC_INSTRUCTION_ERROR;
  }

  return rc;
}

/*
 * merge the local tree with the new files from remote and update the
 * inode numbers
 */
int csync_merge_file_trees(CSYNC *ctx) {
  int rc = -1;

  /* walk over remote tree, stat on local system */
  ctx->current = LOCAL_REPLICA;
  ctx->replica = ctx->local.type;

  rc = c_rbtree_walk(ctx->remote.tree, ctx, _merge_file_trees_visitor);
  if (rc < 0) {
    goto out;
  }

#if 0
  /* We don't have to merge the remote tree atm. */

  /* walk over local tree, stat on remote system */
  ctx->current = REMOTE_REPLICA;
  ctx->replica = ctx->remote.type;

  rc = c_rbtree_walk(ctx->local.tree, ctx, _merge_file_trees_visitor);
  if (rc < 0) {
    goto out;
  }
#endif

out:
  return rc;
}

