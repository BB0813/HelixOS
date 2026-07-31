#pragma once

#include "helix/types.h"
#include "helix/vfs.h"

/* M11 pipe(2) — circular buffer shared between a read and a write vfs_file.
 * Blocking semantics are cooperative: a task that reads an empty pipe or
 * writes a full pipe yields until the other end makes progress. */

#define PIPE_BUF_SIZE 4096
#define PIPE_NUM_READERS 1

struct helix_pipe {
    u8   buf[PIPE_BUF_SIZE];
    int  head;   /* write index */
    int  tail;   /* read index */
    int  count;  /* bytes buffered */
    struct vfs_file *read_end;  /* read-end wrapper (liveness via refcount) */
    struct vfs_file *write_end; /* write-end wrapper */
};

struct helix_pipe *helix_pipe_create(void);
/* Wrap one end as a vfs_file: is_write=0 → read end, 1 → write end. */
struct vfs_file *helix_pipe_wrap(struct helix_pipe *p, int is_write);
void              helix_pipe_free(struct helix_pipe *p);
