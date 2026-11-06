/* Aurora window manager + compositor (Phase 7).
 *
 * Windows are kernel objects with a shared 32-bpp buffer (4 MiB slot each,
 * physically contiguous, mapped into the owning process at
 * WIN_BUF_VA_BASE + wid*WIN_BUF_VA_STRIDE). The process draws into its
 * buffer directly and calls SYS_win_commit; the compositor renders the
 * desktop (background, menu bar, window frames with rounded corners, shadow,
 * traffic lights, title text) into the fb back buffer. */
#ifndef MIRIX_GUI_WM_H
#define MIRIX_GUI_WM_H

#include <stdint.h>

#define WM_MAX_WINS      16
#define WM_WIN_MAX_DIM   1024
#define WM_TITLE_H       28
#define WM_RADIUS        10
#define WM_MENU_H        26
#define WIN_BUF_VA_BASE  0x40000000ULL
#define WIN_BUF_VA_STRIDE 0x400000ULL     /* 4 MiB per window */
#define WIN_BUF_PAGES    1024

#define WM_FRAMELESS     0x1              /* no title bar (dock) */

typedef struct {
    int      used;
    int      x, y, w, h;
    int      flags;
    int      visible;
    int      focused;
    char     title[64];
    uint64_t buf_phys;                    /* WIN_BUF_PAGES contiguous pages */
    uint8_t *buf;                         /* HHDM view for the compositor */
} wm_win_t;

void wm_init(void);
int  wm_create(int x, int y, int w, int h, const char *title, int flags); /* -> wid or -1 */
void wm_close(int wid);
void wm_move(int wid, int x, int y);
void wm_resize(int wid, int w, int h);
void wm_focus(int wid);                   /* raise to front + focus */
void wm_commit(int wid);                  /* mark compositor dirty */
int  wm_dirty(void);
void wm_render(void);                     /* composite desktop -> back buffer */
const wm_win_t *wm_wins(void);
int  wm_count(void);
int  wm_top_at(int x, int y);             /* wid of topmost window at point, -1 */

#endif
