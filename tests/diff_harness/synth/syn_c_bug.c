/* synlib in C with three deliberate bugs (see syn_c.c): a wrong check order, an
 * off-by-one range limit and a 32-bit product where the spec says 64-bit. The
 * differential harness has to flag exactly the cases that reach them. */
#define SYN_BUG_ORDER 1
#define SYN_BUG_LIMIT 1
#define SYN_BUG_WIDE 1
#include "syn_c.c"
