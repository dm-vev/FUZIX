#include "rf_types.h"

const char *rf_data_rate_str(enum rf_data_rate r)
{
	switch (r) {
	case RF_RATE_250K:
		return "250K";
	case RF_RATE_1M:
		return "1M";
	case RF_RATE_2M:
		return "2M";
	default:
		return "?";
	}
}

const char *rf_crc_mode_str(enum rf_crc_mode c)
{
	switch (c) {
	case RF_CRC_OFF:
		return "OFF";
	case RF_CRC_1B:
		return "1B";
	case RF_CRC_2B:
		return "2B";
	default:
		return "?";
	}
}

const char *rf_power_level_str(enum rf_power_level p)
{
	switch (p) {
	case RF_PWR_MIN:
		return "MIN";
	case RF_PWR_LOW:
		return "LOW";
	case RF_PWR_HIGH:
		return "HIGH";
	case RF_PWR_MAX:
		return "MAX";
	default:
		return "?";
	}
}

const char *rf_wf_palette_str(enum rf_wf_palette p)
{
	switch (p) {
	case RF_WF_PAL_CYAN:
		return "CYAN";
	case RF_WF_PAL_FIRE:
		return "FIRE";
	case RF_WF_PAL_GRAY:
		return "GRAY";
	case RF_WF_PAL_CUBIC:
		return "CUBIC";
	default:
		return "?";
	}
}

const char *rf_protocol_mode_str(enum rf_protocol_mode m)
{
	switch (m) {
	case RF_PROTO_DECODED:
		return "DECODED";
	case RF_PROTO_RAW:
		return "RAW";
	default:
		return "?";
	}
}

const char *rf_filter_crc_str(enum rf_filter_crc f)
{
	switch (f) {
	case RF_FILTER_CRC_ANY:
		return "ANY";
	case RF_FILTER_CRC_OK:
		return "OK";
	case RF_FILTER_CRC_BAD:
		return "BAD";
	default:
		return "?";
	}
}

const char *rf_filter_channel_str(enum rf_filter_channel f)
{
	switch (f) {
	case RF_FILTER_CH_ALL:
		return "ALL";
	case RF_FILTER_CH_SELECTED:
		return "SEL";
	case RF_FILTER_CH_RANGE:
		return "RNG";
	default:
		return "?";
	}
}

const char *rf_analysis_view_str(enum rf_analysis_view v)
{
	switch (v) {
	case RF_ANALYSIS_CHANNELS:
		return "CHAN";
	case RF_ANALYSIS_DEVICES:
		return "DEV";
	case RF_ANALYSIS_TIMING:
		return "TIME";
	case RF_ANALYSIS_COLLISIONS:
		return "COL";
	case RF_ANALYSIS_CORRELATION:
		return "CORR";
	case RF_ANALYSIS_COMPARISON:
		return "COMP";
	case RF_ANALYSIS_MONITORING:
		return "MON";
	case RF_ANALYSIS_ANNOTATIONS:
		return "NOTE";
	case RF_ANALYSIS_DIAGNOSTICS:
		return "DIAG";
	case RF_ANALYSIS_STRESS:
		return "STRS";
	default:
		return "?";
	}
}

int rf_wrap_enum(int v, int n)
{
	if (n <= 0)
		return 0;
	while (v < 0)
		v += n;
	while (v >= n)
		v -= n;
	return v;
}

