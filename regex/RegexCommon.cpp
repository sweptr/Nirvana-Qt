
#include "RegexCommon.h"
#include "RegexOpcodes.h"

/* Address of this used as flag. */
prog_type Compute_Size;

/* Default table for determining whether a character is a word delimiter. */
bool DefaultDelimiters[UCHAR_MAX + 1] = { 0 };

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
