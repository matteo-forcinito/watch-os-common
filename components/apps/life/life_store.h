#ifndef LIFE_STORE_H
#define LIFE_STORE_H
#include "esp_err.h"
#include "life_model.h"
#include <time.h>
esp_err_t life_open(life_kind_t kind);
void life_close(life_kind_t kind);
cJSON *life_snapshot(life_kind_t kind);
bool life_command(life_kind_t kind, const cJSON *command, char *error, size_t size);
uint32_t life_revision(life_kind_t kind);
int life_today(void);
void life_services_init(void); /* restore only explicitly scheduled reminders */
bool life_menu_hint(char *out, size_t size);
#endif
