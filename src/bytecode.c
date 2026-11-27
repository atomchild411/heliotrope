/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Compiled JavaScript kept on disk, in ~/.cache/heliotrope/bytecode. At start
 * the cache is trimmed to HELIOTROPE_BYTECODE_CACHE_MB (default 64), least
 * recently used first; HELIOTROPE_NO_BYTECODE_CACHE=1 turns it off.
 */

#include "heliotrope.h"
#include "bytecode.h"

typedef struct {
    char *path;
    goffset size;
    time_t used;
} CacheFile;

static int by_use(const void *a, const void *b)
{
    const CacheFile *x = a, *y = b;
    return x->used < y->used ? -1 : x->used > y->used;
}

/* WebKit (patch 0011) keeps scripts' bytecode in WPE_BYTECODE_CACHE_DIR, one
 * file per script version; trim it to HELIOTROPE_BYTECODE_CACHE_MB (default
 * 64), the files used longest ago first. HELIOTROPE_NO_BYTECODE_CACHE=1: none. */
void setup_bytecode_cache(void)
{
    const char *mb = getenv("HELIOTROPE_BYTECODE_CACHE_MB");
    goffset limit = (goffset)(mb && atoi(mb) > 0 ? atoi(mb) : 64) << 20, total = 0;
    char *dir;
    GDir *d;
    const char *name;
    GArray *files;
    guint i;

    if (env_on("HELIOTROPE_NO_BYTECODE_CACHE"))
        return;
    dir = g_build_filename(g_get_user_cache_dir(), "heliotrope", "bytecode", NULL);
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_free(dir);
        return;
    }
    g_setenv("WPE_BYTECODE_CACHE_DIR", dir, TRUE);
    files = g_array_new(FALSE, FALSE, sizeof(CacheFile));
    if ((d = g_dir_open(dir, 0, NULL))) {
        while ((name = g_dir_read_name(d))) {
            CacheFile f;
            struct stat st;
            f.path = g_build_filename(dir, name, NULL);
            if (stat(f.path, &st) != 0 || !S_ISREG(st.st_mode)) {
                g_free(f.path);
                continue;
            }
            f.size = st.st_size;
            f.used = st.st_atime > st.st_mtime ? st.st_atime : st.st_mtime;
            total += f.size;
            g_array_append_val(files, f);
        }
        g_dir_close(d);
    }
    g_array_sort(files, by_use);
    for (i = 0; i < files->len; i++) {
        CacheFile *f = &g_array_index(files, CacheFile, i);
        if (total > limit && g_unlink(f->path) == 0)
            total -= f->size;
        g_free(f->path);
    }
    g_array_free(files, TRUE);
    g_free(dir);
}
