#include "rf_selection.h"

#include "rf_session.h"
#include "rf_sniffer.h"
#include "rf_task.h"

#include <string.h>

int rf_selection_selected_packet_addr(const struct rf_task *t, uint8_t *addr_len_out, uint8_t addr_out[5])
{
	if (addr_len_out)
		*addr_len_out = 0;
	if (addr_out)
		memset(addr_out, 0, 5);
	if (!t || !addr_len_out || !addr_out)
		return 0;

	if (t->replay_active) {
		struct rf_session_packet_meta meta;
		if (!rf_sniffer_filtered_replay_packet_meta_by_index(t, t->sniffer_sel, &meta) || meta.addr_len == 0)
			return 0;
		*addr_len_out = meta.addr_len;
		memcpy(addr_out, meta.addr, 5);
		return 1;
	}

	const struct rf_packet *p = rf_sniffer_filtered_live_packet_by_index(t, t->sniffer_sel);
	if (!p || p->addr_len == 0)
		return 0;
	*addr_len_out = p->addr_len;
	memcpy(addr_out, p->addr, 5);
	return 1;
}

int rf_selection_selected_packet_tick(const struct rf_task *t, uint64_t *tick_out)
{
	if (tick_out)
		*tick_out = 0;
	if (!t || !tick_out)
		return 0;

	if (t->replay_active) {
		struct rf_session_packet_meta meta;
		if (!rf_sniffer_filtered_replay_packet_meta_by_index(t, t->sniffer_sel, &meta) || meta.tick == 0)
			return 0;
		*tick_out = meta.tick;
		return 1;
	}

	const struct rf_packet *p = rf_sniffer_filtered_live_packet_by_index(t, t->sniffer_sel);
	if (!p || p->tick == 0)
		return 0;
	*tick_out = p->tick;
	return 1;
}

