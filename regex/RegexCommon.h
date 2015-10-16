
#ifndef REGEX_COMMON_H_
#define REGEX_COMMON_H_

#include <cstdint>
#include <cstddef>
#include <climits>

/* Number of text capturing parentheses allowed. */
#define NSUBEXP 50

#define REG_INFINITY 0UL
#define REG_ZERO     0UL
#define REG_ONE      1UL

typedef uint16_t prog_type;

/* Address of this used as flag. */
extern prog_type Compute_Size;

prog_type *getOperand(prog_type *p);
prog_type *next_ptr(prog_type *ptr);
prog_type  getOpcode(const prog_type *p);
prog_type  putOffsetL(ptrdiff_t v);
prog_type  putOffsetR(ptrdiff_t v);
size_t     getOffset(prog_type *p);


#endif
