#include "rf_view.h"

#include "rf_analytics.h"
#include "rf_prompt.h"
#include "rf_task.h"

#include <string.h>

void rf_view_reset(struct rf_task *t)
{
	if (!t)
		return;

	t->focus = RF_FOCUS_SPECTRUM;
	t->selected_channel = 37;
	t->selected_setting = 0;
	t->show_menu = 0;
	t->show_help = 0;
	t->show_filters = 0;
	rf_prompt_close(t);

	t->scan_chan = t->channel_range_lo;
	t->scan_next_tick = 0;
	t->sweep_count = 0;
	t->last_sweep_tick = 0;

	memset(t->energy_cur, 0, sizeof(t->energy_cur));
	memset(t->energy_avg, 0, sizeof(t->energy_avg));
	memset(t->energy_peak, 0, sizeof(t->energy_peak));
	if (t->wf_buf && t->wf_cap)
		memset(t->wf_buf, 0, t->wf_cap);
	t->wf_head = 0;

	rf_analytics_reset(t);
	rf_task_invalidate(t, RF_DIRTY_ALL);
}
