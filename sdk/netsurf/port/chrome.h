/* SPDX-License-Identifier: MIT */
#include <stdbool.h>
struct gui_window;
struct hlcache_handle;
struct form_control;
struct nsurl;
struct ciuki_browser_surface;
void ciuki_chrome_status(const char *);
void ciuki_chrome_url(struct nsurl *);
void ciuki_chrome_loading(bool);
bool ciuki_chrome_busy(void);
void ciuki_chrome_complete(struct gui_window *);
void ciuki_chrome_title(struct gui_window *,const char *);
void ciuki_chrome_file(struct gui_window *,struct hlcache_handle *,struct form_control *);
bool ciuki_chrome_modal(void);
int ciuki_chrome_start(void);
void ciuki_chrome_finish(void);
int ciuki_chrome_arguments(int *,char **);
void ciuki_chrome_defaults(void);
extern const char *ciuki_resource_path;
extern const char *ciuki_capture_path;
const char *ciuki_chrome_choices(void);
void ciuki_chrome_capture(void *);
extern struct gui_download_table ciuki_download_table;
