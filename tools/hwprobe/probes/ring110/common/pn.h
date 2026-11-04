/* pn.h - deterministic pseudo-noise marker codes for the #110 Layer 2 probe.
 *
 * Plain C99, no SDK dependency, so the host test (test_pn.py) can compile it
 * with `cc` and compare against pn.py. Code `id` is 128 signs (+1/-1):
 *
 *   s = (id * 2654435761u) ^ 0x9E3779B9u;  if (s == 0) s = 1;   (uint32 arithmetic)
 *   8 warm-up xorshift32 steps, then for each of 128 samples:
 *       s ^= s << 13;  s ^= s >> 17;  s ^= s << 5;
 *       sign = ((s >> 16) & 1) ? +1 : -1;
 *
 * id 0 is the fixed reference code. Test trials use id = trial number + 1.
 */
#ifndef R110_PN_H
#define R110_PN_H

#include <stdint.h>

#define R110_PN_LEN 128

static inline uint32_t r110_xorshift32(uint32_t s)
{
	s ^= s << 13;
	s ^= s >> 17;
	s ^= s << 5;
	return s;
}

static inline void r110_pn_signs(uint32_t id, int8_t out[R110_PN_LEN])
{
	uint32_t s = (id * 2654435761u) ^ 0x9E3779B9u;
	int i;
	if (s == 0)
		s = 1;
	for (i = 0; i < 8; i++)
		s = r110_xorshift32(s);
	for (i = 0; i < R110_PN_LEN; i++) {
		s = r110_xorshift32(s);
		out[i] = ((s >> 16) & 1) ? 1 : -1;
	}
}

#endif
