#include "rf_cp1251.h"

/*
 * Match Spark's runeToCP1251() mapping used by font6x8cp1251.
 */

uint8_t rf_cp1251_from_rune(uint32_t r)
{
	if (r >= 0x20 && r <= 0x7e)
		return (uint8_t)r;

	switch (r) {
	case 0x00a0: /* NBSP */
		return 0xa0;
	case 0x00b0: /* ° */
		return 0xb0;
	case 0x00b1: /* ± */
		return 0xb1;
	case 0x00b7: /* · */
		return 0xb7;
	case 0x00a9: /* © */
		return 0xa9;
	case 0x00ae: /* ® */
		return 0xae;
	case 0x2116: /* № */
		return 0xb9;
	case 0x00ab: /* « */
		return 0xab;
	case 0x00bb: /* » */
		return 0xbb;
	case 0x2026: /* … */
		return 0x85;
	case 0x2013: /* – */
		return 0x96;
	case 0x2014: /* — */
		return 0x97;
	case 0x0401: /* Ё */
		return 0xa8;
	case 0x0451: /* ё */
		return 0xb8;
	default:
		break;
	}

	if (r >= 0x0410 && r <= 0x042f) /* А-Я */
		return (uint8_t)(0xc0 + (r - 0x0410));
	if (r >= 0x0430 && r <= 0x044f) /* а-я */
		return (uint8_t)(0xe0 + (r - 0x0430));

	return (uint8_t)'?';
}

