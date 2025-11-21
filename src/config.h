/* config.c - Configuration system */
#include "config.h"
#include <string.h>

void config_default(Config *cfg) {
    cfg->tab_width = 4;
    cfg->show_line_numbers = 1;
    cfg->auto_indent = 1;
    cfg->syntax_highlighting = 1;
    strncpy(cfg->color_scheme, "default", sizeof(cfg->color_scheme) - 1);
    cfg->show_status_bar = 1;
    cfg->show_welcome = 1;
    cfg->create_backup = 0;
    cfg->auto_save_interval = 0;
}


// comment to test git push // 

