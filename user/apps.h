/* apps.h — interface between the Aurora desktop shell (aurora.c, C) and the
 * C++ app modules (editor.cpp, settings.cpp, built on the mixtk toolkit).
 * Phase 7d. C/C++ dual header. */
#ifndef MIRIX_USER_APPS_H
#define MIRIX_USER_APPS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Editor app (user/editor.cpp) */
void editor_attach(int wid, uint32_t *buf, int x, int y, int w, int h);
void editor_detach(void);
int  editor_key(char c);            /* 1 if the window needs a repaint */
int  editor_click(int lx, int ly);  /* content coords (title bar removed) */
int  editor_dirty(void);
void editor_paint(void);            /* repaint + SYS_win_commit */

/* Settings app (user/settings.cpp) */
void settings_attach(int wid, uint32_t *buf, int x, int y, int w, int h);
void settings_detach(void);
int  settings_click(int lx, int ly);
int  settings_dirty(void);
void settings_paint(void);

/* Hooks implemented by aurora.c, called by the Settings app when a toggle
 * changes. */
void aurora_set_clock_seconds(int on);
void aurora_set_term_dark(int on);
void aurora_set_dock_magnify(int on);

#ifdef __cplusplus
}
#endif

#endif
