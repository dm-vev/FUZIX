#ifndef RF_MENU_H
#define RF_MENU_H

#include "rf_types.h"

struct rf_key;
struct rf_task;

void rf_menu_open(struct rf_task *t);
void rf_menu_close(struct rf_task *t);
void rf_menu_handle_key(struct rf_task *t, const struct rf_key *k);

/* Render helpers */
const char *rf_menu_category_label(enum rf_menu_category cat);

struct rf_menu_item {
	enum rf_menu_item_id id;
	const char *label;
};

const struct rf_menu_item *rf_menu_items(enum rf_menu_category cat, int *count);
void rf_menu_item_line(const struct rf_task *t, struct rf_menu_item it, char *out, unsigned outsz);

#endif

