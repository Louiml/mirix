/* Syscall interface: syscall/sysret setup + the C dispatcher.
 * Numbers: 0=write(fd,buf,len) 1=exit(code) 2=gettid(). */
#ifndef MIRIX_PROC_SYSCALL_H
#define MIRIX_PROC_SYSCALL_H

#include <stdint.h>

#define SYS_write    0
#define SYS_exit     1
#define SYS_gettid   2
#define SYS_yield    3
#define SYS_open     4
#define SYS_read     5
#define SYS_close    6
#define SYS_getdents 7
#define SYS_ps       8
#define SYS_diskread 9   /* (lba, nsec, buf) -> bytes read from AHCI disk */
#define SYS_flip     10  /* present the back buffer to the screen */
#define SYS_fbinfo   11  /* (w, h, bpp) out via 3 ints in buf */
#define SYS_getevent 12  /* pop one input_event_t (kbd or mouse) into buf */
#define SYS_mousepos 13  /* write (x, y, buttons) as 3 ints into buf */
/* Phase 7: window manager. Window content buffer is mapped into the caller
 * at WIN_BUF_VA_BASE + wid*WIN_BUF_VA_STRIDE (4 MiB slots, 32-bpp). */
#define SYS_win_create 14  /* (x, y, w, h, title, flags) -> wid */
#define SYS_win_commit 15  /* (wid) — redraw compositor */
#define SYS_win_move   16  /* (wid, x, y) */
#define SYS_win_resize 17  /* (wid, w, h) */
#define SYS_win_close  18  /* (wid) */
#define SYS_win_focus  19  /* (wid) — raise + focus */
#define SYS_win_top    20  /* (x, y) -> wid of topmost window at point, -1 */
#define SYS_uptime     21  /* () -> milliseconds since timer start */

void syscall_init(void);

/* Called from syscall.asm. Args arrive in SysV order. Returns rax value. */
uint64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6,
                          uint64_t user_rip, uint64_t user_rflags, uint64_t user_rsp);

/* asm helpers */
void enter_usermode(uint64_t entry, uint64_t user_sp) __attribute__((noreturn));

#endif
