#include "vfs.h"
#include "mm/kmalloc.h"
#include "lib/string.h"
#include "lib/uart.h"

typedef struct {
    uint8_t  type;               /* VN_* */
    char     name[VFS_NAME_MAX];
    uint32_t size;
    uint32_t cap;
    uint8_t *data;
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

int vfs_open(const char *path, int flags)
{
    if (path[0] == 0 || (path[0] == '/' && path[1] == 0))
        return VN_ROOT_IDX;                       /* root dir */
    if (path[0] == '/')
        path++;                                   /* flat fs: strip leading / */

    /* existing file? */
    for (int i = 2; i < VN_TOTAL; i++) {
        if (g_vnodes[i].type == VN_FILE && strncmp(g_vnodes[i].name, path, VFS_NAME_MAX) == 0) {
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

long vfs_read(int vnode, uint32_t *offset, void *buf, long len)
{
    if (vnode < 0 || vnode >= VN_TOTAL || g_vnodes[vnode].type != VN_FILE)
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

long vfs_write(int vnode, uint32_t *offset, const void *buf, long len)
{
    if (vnode < 0 || vnode >= VN_TOTAL || g_vnodes[vnode].type != VN_FILE)
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

long vfs_getdents(int vnode, void *buf, long len)
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
