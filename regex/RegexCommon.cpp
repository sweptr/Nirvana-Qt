
#include "RegexCommon.h"
#include "RegexOpcodes.h"
#include "Regex.h"

/* Address of this used as flag. */
prog_type Compute_Size;

/*----------------------------------------------------------------------*
 * next_ptr - compute the address of a node's "NEXT" pointer.
 * Note: a simplified inline version is available via the NEXT_PTR() macro,
 *       but that one is only to be used at time-critical places (see the
 *       description of the macro).
 *----------------------------------------------------------------------*/
prog_type *next_ptr(prog_type *ptr) {

	if (ptr == &Compute_Size) {
		return nullptr;
	}

	const size_t offset = getOffset(ptr);

	if (offset == 0) {
		return nullptr;
	}

	if (getOpcode(ptr) == BACK) {
		return (ptr - offset);
	} else {
		return (ptr + offset);
	}
}

//------------------------------------------------------------------------------
// Name: 
//------------------------------------------------------------------------------
prog_type getOpcode(const prog_type *p) {
	return *p;
}

//------------------------------------------------------------------------------
// Name: 
//------------------------------------------------------------------------------
prog_type *getOperand(prog_type *p) {
	return p + Regex::NodeSize;
}

//------------------------------------------------------------------------------
// Name: 
//------------------------------------------------------------------------------
size_t getOffset(prog_type *p) {
	return ((p[1] & 0xff) << 8) + (p[2] & 0xff);
}

//------------------------------------------------------------------------------
// Name: 
//------------------------------------------------------------------------------
prog_type putOffsetL(ptrdiff_t v) {
	return static_cast<prog_type>((v >> 8) & 0xff);
}

//------------------------------------------------------------------------------
// Name: 
//------------------------------------------------------------------------------
prog_type putOffsetR(ptrdiff_t v) {
	return static_cast<prog_type>(v & 0xff);
}
