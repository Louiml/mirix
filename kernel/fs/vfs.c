#include "vfs.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/uart.h"
#include "lib/spinlock.h"
#include "arch/x86_64/cpu.h"

/* SMP (8c): the vnode table + pty rings are shared by all CPUs. Lock order:
 * sched -> vfs -> heap -> pmm. */
static spinlock_t g_vfs_lock = SPINLOCK_INIT;

/* Phase 7b: pty — two 4 KiB rings between the master and slave endpoints. */
#define PTY_RING 4096
typedef struct {
    uint8_t  buf[PTY_RING];
    uint32_t head, tail;         /* head = write pos, tail = read pos */
} pty_ring_t;
typedef struct {
    pty_ring_t m2s;              /* written on master, read on slave */
    pty_ring_t s2m;              /* written on slave,  read on master */
} pty_t;

typedef struct {
    uint8_t  type;               /* VN_* */
    char     name[VFS_NAME_MAX];
    uint32_t size;
    uint32_t cap;
    uint8_t *data;
    pty_t   *pty;                /* VN_PTY only */
    int      pty_side;           /* 0 = master, 1 = slave */
} vnode_t;

#define VN_TOTAL (2 + VFS_MAX_FILES)
static vnode_t g_vnodes[VN_TOTAL];

#define VN_CONSOLE_IDX 0
#define VN_ROOT_IDX    1

void vfs_init(void)
{
    memset(g_vnodes, 0, sizeof(g_vnodes));
    g_vnodes[VN_CONSOLE_IDX].type = VN_CONSOLE;
    strcpy(g_vnodes[VN_CONSOLE_IDX].name, "console");
    g_vnodes[VN_ROOT_IDX].type = VN_DIR;
    strcpy(g_vnodes[VN_ROOT_IDX].name, "/");
    uart_printf("vfs: ramdisk ready (%d file slots, %d KiB max/file, flat /)\n",
                VFS_MAX_FILES, VFS_FILE_MAX / 1024);
}

int vfs_vnode_type(int vnode)
{
    if (vnode < 0 || vnode >= VN_TOTAL)
        return -1;
    return g_vnodes[vnode].type;
}

static int vfs_open_locked(const char *path, int flags)
{
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0))
        return VN_ROOT_IDX;                       /* root dir */
    if (path[0] == '/')
        path++;                                   /* flat fs: strip leading / */

    /* existing file or pty endpoint? */
    for (int i = 2; i < VN_TOTAL; i++) {
        if ((g_vnodes[i].type == VN_FILE || g_vnodes[i].type == VN_PTY) &&
            strncmp(g_vnodes[i].name, path, VFS_NAME_MAX) == 0) {
            if (flags & O_TRUNC)
                g_vnodes[i].size = 0;
            return i;
        }
    }
    if (!(flags & O_CREAT))
        return -2;                                /* ENOENT */

    /* create */
    for (int i = 2; i < VN_TOTAL; i++) {
        if (g_vnodes[i].type == 0) {
            g_vnodes[i].type = VN_FILE;
            memset(g_vnodes[i].name, 0, VFS_NAME_MAX);
            strncpy(g_vnodes[i].name, path, VFS_NAME_MAX - 1);
            g_vnodes[i].size = 0;
            g_vnodes[i].cap = 4096;
            g_vnodes[i].data = kmalloc(4096);
            if (!g_vnodes[i].data)
                return -12;                       /* ENOMEM */
            memset(g_vnodes[i].data, 0, 4096);
            uart_printf("vfs: created '/%s' (vnode %d)\n", g_vnodes[i].name, i);
            return i;
        }
    }
    return -28;                                   /* ENOSPC */
}

/* Phase 7b: pty endpoints are non-blocking byte rings. */
static int vfs_pty_create_locked(void)
{
    int mi = -1, si = -1;
    for (int i = 2; i < VN_TOTAL && (mi < 0 || si < 0); i++)
        if (g_vnodes[i].type == 0) {
            if (mi < 0) mi = i;
            else si = i;
        }
    if (mi < 0 || si < 0)
        return -28;                               /* ENOSPC */
    pty_t *p = kmalloc(sizeof(pty_t));
    if (!p)
        return -12;                               /* ENOMEM */
    memset(p, 0, sizeof(pty_t));
    g_vnodes[mi].type = VN_PTY;
    g_vnodes[mi].pty = p;
    g_vnodes[mi].pty_side = 0;
    strcpy(g_vnodes[mi].name, "ptym0");
    g_vnodes[si].type = VN_PTY;
    g_vnodes[si].pty = p;
    g_vnodes[si].pty_side = 1;
    strcpy(g_vnodes[si].name, "ptys0");
    /* caller holds g_vfs_lock with IF=0: the print is already atomic */
    uart_printf("vfs: pty created /ptym0 (vnode %d) + /ptys0 (vnode %d)\n", mi, si);
    return si;                                    /* slave */
}

static long vfs_read_locked(int vnode, uint32_t *offset, void *buf, long len)
{
    if (vnode < 0 || vnode >= VN_TOTAL)
        return -9;                                /* EBADF */
    vnode_t *pv = &g_vnodes[vnode];
    if (pv->type == VN_PTY) {
        pty_ring_t *r = (pv->pty_side == 0) ? &pv->pty->s2m : &pv->pty->m2s;
        long n = 0;
        while (n < len && r->tail != r->head) {
            ((uint8_t *)buf)[n++] = r->buf[r->tail % PTY_RING];
            r->tail++;
        }
        return n;                                 /* 0 = empty (non-blocking) */
    }
    if (pv->type != VN_FILE)
        return -9;                                /* EBADF */
    vnode_t *v = &g_vnodes[vnode];
    if (*offset >= v->size)
        return 0;                                 /* EOF */
    long n = len;
    if ((uint32_t)n > v->size - *offset)
        n = (long)(v->size - *offset);
    memcpy(buf, v->data + *offset, (size_t)n);
    *offset += (uint32_t)n;
    return n;
}

static long vfs_write_locked(int vnode, uint32_t *offset, const void *buf, long len)
{
    if (vnode < 0 || vnode >= VN_TOTAL)
        return -9;
    vnode_t *pv = &g_vnodes[vnode];
    if (pv->type == VN_PTY) {
        pty_ring_t *r = (pv->pty_side == 0) ? &pv->pty->m2s : &pv->pty->s2m;
        long n = 0;
        while (n < len) {
            uint32_t next = (r->head + 1) % PTY_RING;
            if (next == r->tail)
                break;                            /* full: drop the rest */
            r->buf[r->head % PTY_RING] = ((const uint8_t *)buf)[n++];
            r->head = next;
        }
        return n;
    }
    if (pv->type != VN_FILE)
        return -9;
    vnode_t *v = &g_vnodes[vnode];
    uint64_t need = (uint64_t)*offset + (uint64_t)len;
    if (need > VFS_FILE_MAX)
        return -27;                               /* EFBIG */
    while (need > v->cap) {
        uint32_t ncap = v->cap * 2;
        if (ncap > VFS_FILE_MAX)
            ncap = VFS_FILE_MAX;
        uint8_t *nd = kmalloc(ncap);
        if (!nd)
            return -12;
        memcpy(nd, v->data, v->size);
        kfree(v->data);
        v->data = nd;
        v->cap = ncap;
    }
    memcpy(v->data + *offset, buf, (size_t)len);
    *offset += (uint32_t)len;
    if (*offset > v->size)
        v->size = *offset;
    return len;
}

static long vfs_getdents_locked(int vnode, void *buf, long len)
{
    if (vnode != VN_ROOT_IDX)
        return -9;
    long max_entries = len / (long)sizeof(mirix_dirent_t);
    long n = 0;
    mirix_dirent_t *out = (mirix_dirent_t *)buf;
    for (int i = 2; i < VN_TOTAL && n < max_entries; i++) {
        if (g_vnodes[i].type != VN_FILE)
            continue;
        memset(&out[n], 0, sizeof(mirix_dirent_t));
        strncpy(out[n].name, g_vnodes[i].name, VFS_NAME_MAX - 1);
        out[n].size = g_vnodes[i].size;
        out[n].type = VN_FILE;
        n++;
    }
    return n * (long)sizeof(mirix_dirent_t);
}

/* Public entry points take the vfs lock (callers run with IF=0 in syscalls
 * or IF=1 in the main loop — the irqsave variant covers both). */
int vfs_open(const char *path, int flags)
{
    uint64_t fl = spin_lock_irqsave(&g_vfs_lock);
    int r = vfs_open_locked(path, flags);
    spin_unlock_irqrestore(&g_vfs_lock, fl);
    return r;
}

int vfs_pty_create(void)
{
    uint64_t fl = spin_lock_irqsave(&g_vfs_lock);
    int r = vfs_pty_create_locked();
    spin_unlock_irqrestore(&g_vfs_lock, fl);
    return r;
}

long vfs_read(int vnode, uint32_t *offset, void *buf, long len)
{
    uint64_t fl = spin_lock_irqsave(&g_vfs_lock);
    long r = vfs_read_locked(vnode, offset, buf, len);
    spin_unlock_irqrestore(&g_vfs_lock, fl);
    return r;
}

long vfs_write(int vnode, uint32_t *offset, const void *buf, long len)
{
    uint64_t fl = spin_lock_irqsave(&g_vfs_lock);
    long r = vfs_write_locked(vnode, offset, buf, len);
    spin_unlock_irqrestore(&g_vfs_lock, fl);
    return r;
}

long vfs_getdents(int vnode, void *buf, long len)
{
    uint64_t fl = spin_lock_irqsave(&g_vfs_lock);
    long r = vfs_getdents_locked(vnode, buf, len);
    spin_unlock_irqrestore(&g_vfs_lock, fl);
    return r;
}
