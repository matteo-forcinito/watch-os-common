#ifndef LITE_CONTENT_H
#define LITE_CONTENT_H
#include "navigation.h"
typedef struct {
    char heading[32], title[80], detail[3][80], button[32];
} lite_content_t;
void lite_content_load(const lite_navigation_t *nav, lite_content_t *out);
void lite_content_activate(lite_page_t page);
#endif
