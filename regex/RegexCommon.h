
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

/* The first byte of the Regex internal 'program' is a magic number to help
   gaurd against corrupted data; the compiled regex code really begins in the
   second byte. */
enum : prog_type {
	MAGIC = 0234
};


/* A node is one char of opcode followed by two chars of NEXT pointer plus
 * any operands.  NEXT pointers are stored as two 8-bit pieces, high order
 * first.  The value is a positive offset from the opcode of the node
 * containing it.  An operand, if any, simply follows the node.  (Note that
 * much of the code generation knows about this implicit relationship.)
 *
 * Using two bytes for NEXT_PTR_SIZE is vast overkill for most things,
 * but allows patterns to get big without disasters. */
const int LengthSize  = 4;
const int IndexSize   = 1;
const int OpcodeSize  = 1;
const int NextPtrSize = 2;
const int NodeSize    = (NextPtrSize + OpcodeSize);


/* Address of this used as flag. */
extern prog_type Compute_Size;

/* Default table for determining whether a character is a word delimiter. */
extern bool DefaultDelimiters[UCHAR_MAX + 1];


inline prog_type getOpcode(const prog_type *p) {
	return *p;
}

inline prog_type *getOperand(prog_type *p) {
	return p + NodeSize;
}

inline size_t getOffset(prog_type *p) {
	return ((p[1] & 0xff) << 8) + (p[2] & 0xff);
}

inline prog_type putOffsetL(ptrdiff_t v) {
	return static_cast<prog_type>((v >> 8) & 0xff);
}

inline prog_type putOffsetR(ptrdiff_t v) {
	return static_cast<prog_type>(v & 0xff);
}

prog_type *next_ptr(prog_type *ptr);

#endif
