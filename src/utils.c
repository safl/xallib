/**
 * Misc. utilities goes here
 */
#include <inttypes.h>
#include <stdio.h>

void
dump_bytes(void *buf, int nbytes)
{
	for (int i = 0; i < nbytes; ++i) {
		uint8_t val = ((uint8_t *)buf)[i];

		if ((val > 31) && (val < 127)) {
			printf("%03d: '%c'\n", i, val);
		} else {
			printf("%03d: %u\n", i, val);
		}
	}
}
