/* Minimal VFS over an in-memory ramdisk (flat root directory).
 *
 * Vnodes: 0 = console (write -> serial, read -> keyboard ring),
 *         1 = root dir "/", 2.. = files.
 * All operations run in syscall context with IF=0 (single CPU), so they are
 * atomic without locks. File data lives in kmalloc'd buffers that grow by
 * alloc+copy (no krealloc yet).
 */
#ifndef MIRIX_FS_VFS_H
#define MIRIX_FS_VFS_H

#include <stdint.h>
#include <stddef.h>

#define VFS_MAX_FILES   32
#define VFS_NAME_MAX    32
#define VFS_FILE_MAX    (256 * 1024)

#define VN_CONSOLE 1
#define VN_DIR     2
#define VN_FILE    3
#define VN_PTY     4   /* Phase 7b: master/slave pair sharing a pty_t */

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x80

/* dirent layout shared with userland — must stay 40 bytes */
typedef struct {
    char     name[VFS_NAME_MAX];
    uint32_t size;
    uint8_t  type;
    uint8_t  _pad[3];
} mirix_dirent_t;

void vfs_init(void);
int  vfs_open(const char *path, int flags);          /* vnode idx or -errno */
long vfs_read(int vnode, uint32_t *offset, void *buf, long len);
long vfs_write(int vnode, uint32_t *offset, const void *buf, long len);
long vfs_getdents(int vnode, void *buf, long len);    /* dir only, bytes out */
int  vfs_vnode_type(int vnode);

/* Phase 7b: create a pty pair registered as /ptymN (master) and /ptysN
 * (slave). Non-blocking: read returns 0 when the ring is empty.
 * Returns the SLAVE vnode index, or -errno. */
int  vfs_pty_create(void);

#endif
