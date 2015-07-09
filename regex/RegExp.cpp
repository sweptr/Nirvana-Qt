
#include "RegExp.h"
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cassert>

#define ENABLE_COUNTING_QUANTIFIER

namespace {
/* The first byte of the regexp internal `program' is a magic number to help
   gaurd against corrupted data; the compiled regex code really begins in the
   second byte. */
const uint8_t MAGIC = 0234;

enum Opcodes : uint8_t {
	/* STRUCTURE FOR A REGULAR EXPRESSION (regex) `PROGRAM'.
     *
     * This is essentially a linear encoding of a nondeterministic finite-state
     * machine or NFA (aka syntax charts or `railroad normal form' in parsing
     * technology).  Each node is an opcode plus a NEXT pointer, possibly
     * followed by operands.  NEXT pointers of all nodes except BRANCH implement
     * concatenation; a NEXT pointer with a BRANCH on both ends of it is
     * connecting two alternatives.  (Here we have one of the subtle syntax
     * dependencies:  an individual BRANCH (as opposed to a collection of them) is
     * never concatenated with anything because of operator precedence.)  The
     * operand of some types of nodes is a literal string; for others, it is a
     * node
     * leading into a sub-FSM.  In particular, the operand of a BRANCH node is the
     * first node of the branch. (NB this is _NOT_ a tree structure:  the tail of
     * the branch connects to the thing following the set of BRANCHes.)
     *
     * The opcodes are:
     */

	END = 1, // End of program.

	// Zero width positional assertions.
	BOL = 2,          // Match position at beginning of line.
	EOL = 3,          // Match position at end of line.
	BOWORD = 4,       // Match "" representing word delimiter or BOL
	EOWORD = 5,       // Match "" representing word delimiter or EOL
	NOT_BOUNDARY = 6, // Not word boundary (\B, opposite of < and >)

	// Op codes with null terminated string operands.

	EXACTLY = 7,  // Match this string.
	SIMILAR = 8,  // Match this case insensitive string
	ANY_OF = 9,   // Match any character in the set.
	ANY_BUT = 10, // Match any character not in the set.

	// Op codes to match any character.
	ANY = 11,   // Match any one character (implements '.')
	EVERY = 12, // Same as ANY but matches newline.

	// Shortcut escapes, \d, \D, \l, \L, \s, \S, \w, \W, \y, \Y.
	DIGIT = 13,         // Match any digit, i.e. [0123456789]
	NOT_DIGIT = 14,     // Match any non-digit, i.e. [^0123456789]
	LETTER = 15,        // Match any letter character [a-zA-Z]
	NOT_LETTER = 16,    // Match any non-letter character [^a-zA-Z]
	SPACE = 17,         // Match any whitespace character EXCEPT \n
	SPACE_NL = 18,      // Match any whitespace character INCLUDING \n
	NOT_SPACE = 19,     // Match any non-whitespace character
	NOT_SPACE_NL = 20,  // Same as NOT_SPACE but matches newline.
	WORD_CHAR = 21,     // Match any word character [a-zA-Z0-9_]
	NOT_WORD_CHAR = 22, // Match any non-word character [^a-zA-Z0-9_]
	IS_DELIM = 23,      // Match any character that's a word delimiter
	NOT_DELIM = 24,     // Match any character NOT a word delimiter

	// Quantifier nodes. (Only applied to SIMPLE nodes.  Quantifiers applied to non SIMPLE nodes or larger atoms are
	// implemented using complex constructs.)
	STAR = 25,          // Match this (simple) thing 0 or more times.
	LAZY_STAR = 26,     // Minimal matching STAR
	QUESTION = 27,      // Match this (simple) thing 0 or 1 times.
	LAZY_QUESTION = 28, // Minimal matching QUESTION
	PLUS = 29,          // Match this (simple) thing 1 or more times.
	LAZY_PLUS = 30,     // Minimal matching PLUS
	BRACE = 31,         // Match this (simple) thing m to n times.
	LAZY_BRACE = 32,    // Minimal matching BRACE

	// Nodes used to build complex constructs.
	NOTHING = 33,    // Match empty string (always matches)
	BRANCH = 34,     // Match this alternative, or the next...
	BACK = 35,       // Always matches, NEXT ptr points backward.
	INIT_COUNT = 36, // Initialize {m,n} counter to zero
	INC_COUNT = 37,  // Increment {m,n} counter by one
	TEST_COUNT = 38, // Test {m,n} counter against operand

	// Back Reference nodes.
	BACK_REF = 39,      // Match latest matched parenthesized text
	BACK_REF_CI = 40,   // Case insensitive version of BACK_REF
	X_REGEX_BR = 41,    // Cross-Regex Back-Ref for syntax highlighting
	X_REGEX_BR_CI = 42, // Case insensitive version of X_REGEX_BR_CI

	// Various nodes used to implement parenthetical constructs.
	POS_AHEAD_OPEN = 43,   // Begin positive look ahead
	NEG_AHEAD_OPEN = 44,   // Begin negative look ahead
	LOOK_AHEAD_CLOSE = 45, // End positive or negative look ahead

	POS_BEHIND_OPEN = 46,   // Begin positive look behind
	NEG_BEHIND_OPEN = 47,   // Begin negative look behind
	LOOK_BEHIND_CLOSE = 48, // Close look behind

	OPEN = 49, // Open for capturing parentheses.

	//  OPEN+1 is number 1, etc.
	CLOSE = (OPEN + NSUBEXP), // Close for capturing parentheses.

	LAST_PAREN = (CLOSE + NSUBEXP),
};

const char Default_Meta_Char[] = "{.*+?[(|)^<>$";
const char ASCII_Digits[]      = "0123456789"; // Same for all locales.
}

// Global work variables for 'ExecRE'.
struct ExecState {
public:
	bool atEndOfString(const char *p) const {
		return (*p == '\0' || (End_Of_String != nullptr && p >= End_Of_String));
	}
public:
	const char *Reg_Input;          // String-input pointer.
	const char *Start_Of_String;    // Beginning of input, for ^ and < checks.
	const char *End_Of_String;      // Logical end of input (if supplied, till \0 otherwise)	
	const char *Look_Behind_To;     // Position till were look behind can safely check back
	const char **Start_Ptr_Ptr;     // Pointer to 'startp' array.
	const char **End_Ptr_Ptr;       // Ditto for 'endp'.
	const char *Extent_Ptr_FW;      // Forward extent pointer
	const char *Extent_Ptr_BW;      // Backward extent pointer
	const char *Back_Ref_Start[10]; // Back_Ref_Start [0] and
	const char *Back_Ref_End[10];   // Back_Ref_End [0] are not used. This simplifies indexing.
	
	bool Prev_Is_BOL;
	bool Succ_Is_EOL;
	bool Prev_Is_Delim;
	bool Succ_Is_Delim;
};

// Global work variables for 'CompileRE'.
struct CompileState {
public:
	bool isQuantifier(uint8_t c) const {
		return (c == '*' || c == '+' || c == '?' || c == Brace_Char);
	}
public:
	const uint8_t *Reg_Parse;  // Input scan ptr (scans user's regex)
	int Closed_Parens;   // Bit flags indicating () closure.
	int Paren_Has_Width; // Bit flags indicating ()'s that are known to not match the empty string

	uint8_t *Code_Emit_Ptr; // When Code_Emit_Ptr is set to &Compute_Size no code is emitted. Instead, the size of
							// code that WOULD have been generated is accumulated in Reg_Size.  Otherwise,
							// Code_Emit_Ptr points to where compiled regex code is to be written.

	unsigned long Reg_Size; // Size of compiled regex code.
	bool Is_Case_Insensitive;
	bool Match_Newline;
	char Brace_Char;
	const char *Meta_Char;
};

uint8_t RegExp::Default_Delimiters[UCHAR_MAX + 1] = {0};

/* The "internal use only" fields in `regexp.h' are present to pass info from
 * `CompileRE' to `ExecRE' which permits the execute phase to run lots faster on
 * simple cases.  They are:
 *
 *   match_start     Character that must begin a match; '\0' if none obvious.
 *   anchor          Is the match anchored (at beginning-of-line only)?
 *
 * `match_start' and `anchor' permit very fast decisions on suitable starting
 * points for a match, considerably reducing the work done by ExecRE.
 */

/* The next_ptr () function can consume up to 30% of the time during matching
   because it is called an immense number of times (an average of 25
   next_ptr() calls per match() call was witnessed for Perl syntax
   highlighting). Therefore it is well worth removing some of the function
   call overhead by selectively inlining the next_ptr() calls. Moreover,
   the inlined code can be simplified for matching because one of the tests,
   only necessary during compilation, can be left out.
   The net result of using this inlined version at two critical places is
   a 25% speedup (again, witnesses on Perl syntax highlighting). */

#define NEXT_PTR(in_ptr, out_ptr)                                                                                      \
	next_ptr_offset = GET_OFFSET(in_ptr);                                                                              \
	if (next_ptr_offset == 0) {                                                                                        \
		out_ptr = nullptr;                                                                                             \
	} else {                                                                                                           \
		if (GET_OP_CODE(in_ptr) == BACK) {                                                                             \
			out_ptr = in_ptr - next_ptr_offset;                                                                        \
		} else {                                                                                                       \
			out_ptr = in_ptr + next_ptr_offset;                                                                        \
		}                                                                                                              \
	}

/* OPCODE NOTES:
   ------------

   All nodes consist of an 8 bit op code followed by 2 bytes that make up a 16
   bit NEXT pointer.  Some nodes have a null terminated character string operand
   following the NEXT pointer.  Other nodes may have an 8 bit index operand.
   The TEST_COUNT node has an index operand followed by a 16 bit test value.
   The BRACE and LAZY_BRACE nodes have two 16 bit values for min and max but no
   index value.

   SIMILAR
      Operand(s): null terminated string

      Implements a case insensitive match of a string.  Mostly intended for use
      in syntax highlighting patterns for keywords of languages like FORTRAN
      and Ada that are case insensitive.  The regex text in this node is
      converted to lower case during regex compile.

   DIGIT, NOT_DIGIT, LETTER, NOT_LETTER, SPACE, NOT_SPACE, WORD_CHAR,
   NOT_WORD_CHAR
      Operand(s): None

      Implements shortcut escapes \d, \D, \l, \L, \s, \S, \w, \W.  The locale
      aware ANSI functions isdigit(), isalpha(), isalnun(), and isspace() are
      used to implement these in the hopes of increasing portability.

   NOT_BOUNDARY
      Operand(s): None

      Implements \B as a zero width assertion that the current character is
      NOT on a word boundary.  Word boundaries are defined to be the position
      between two characters where one of those characters is one of the
      dynamically defined word delimiters, and the other character is not.

   IS_DELIM
      Operand(s): None

      Implements \y as any character that is one of the dynamically
      specified word delimiters.

   NOT_DELIM
      Operand(s): None

      Implements \Y as any character that is NOT one of the dynamically
      specified word delimiters.

   STAR, PLUS, QUESTION, and complex '*', '+', and '?'
      Operand(s): None (Note: NEXT pointer is usually zero.  The code that
                        processes this node skips over it.)

      Complex (parenthesized) versions implemented as circular BRANCH
      structures using BACK.  SIMPLE versions (one character per match) are
      implemented separately for speed and to minimize recursion.

   BRACE, LAZY_BRACE
      Operand(s): minimum value (2 bytes), maximum value (2 bytes)

      Implements the {m,n} construct for atoms that are SIMPLE.

   BRANCH
      Operand(s): None

      The set of branches constituting a single choice are hooked together
      with their NEXT pointers, since precedence prevents anything being
      concatenated to any individual branch.  The NEXT pointer of the last
      BRANCH in a choice points to the thing following the whole choice.  This
      is also where the final NEXT pointer of each individual branch points;
      each branch starts with the operand node of a BRANCH node.

   BACK
      Operand(s): None

      Normal NEXT pointers all implicitly point forward.  Back implicitly
      points backward.  BACK exists to make loop structures possible.

   INIT_COUNT
      Operand(s): index (1 byte)

      Initializes the count array element referenced by the index operand.
      This node is used to build general (i.e. parenthesized) {m,n} constructs.

   INC_COUNT
      Operand(s): index (1 byte)

      Increments the count array element referenced by the index operand.
      This node is used to build general (i.e. parenthesized) {m,n} constructs.

   TEST_COUNT
      Operand(s): index (1 byte), test value (2 bytes)

      Tests the current value of the count array element specified by the
      index operand against the test value.  If the current value is less than
      the test value, control passes to the node after that TEST_COUNT node.
      Otherwise control passes to the node referenced by the NEXT pointer for
      the TEST_COUNT node.  This node is used to build general (i.e.
      parenthesized) {m,n} constructs.

   BACK_REF, BACK_REF_CI
      Operand(s): index (1 byte, value 1-9)

      Implements back references.  This node will attempt to match whatever text
      was most recently captured by the index'th set of parentheses.
      BACK_REF_CI is case insensitive version.

   X_REGEX_BR, X_REGEX_BR_CI
      (NOT IMPLEMENTED YET)

      Operand(s): index (1 byte, value 1-9)

      Implements back references into a previously matched but separate regular
      expression.  This is used by syntax highlighting patterns. This node will
      attempt to match whatever text was most captured by the index'th set of
      parentheses of the separate regex passed to ExecRE. X_REGEX_BR_CI is case
      insensitive version.

   POS_AHEAD_OPEN, NEG_AHEAD_OPEN, LOOK_AHEAD_CLOSE

      Operand(s): None

      Implements positive and negative look ahead.  Look ahead is an assertion
      that something is either there or not there.   Once this is determined the
      regex engine backtracks to where it was just before the look ahead was
      encountered, i.e. look ahead is a zero width assertion.

   POS_BEHIND_OPEN, NEG_BEHIND_OPEN, LOOK_BEHIND_CLOSE

      Operand(s): 2x2 bytes for OPEN (match boundaries), None for CLOSE

      Implements positive and negative look behind.  Look behind is an assertion
      that something is either there or not there in front of the current
      position.  Look behind is a zero width assertion, with the additional
      constraint that it must have a bounded length (for complexity and
      efficiency reasons; note that most other implementation even impose
      fixed length).

   OPEN, CLOSE

      Operand(s): None

      OPEN  + n = Start of parenthesis 'n', CLOSE + n = Close of parenthesis
      'n', and are numbered at compile time.
 */

/* A node is one char of opcode followed by two chars of NEXT pointer plus
 * any operands.  NEXT pointers are stored as two 8-bit pieces, high order
 * first.  The value is a positive offset from the opcode of the node
 * containing it.  An operand, if any, simply follows the node.  (Note that
 * much of the code generation knows about this implicit relationship.)
 *
 * Using two bytes for NEXT_PTR_SIZE is vast overkill for most things,
 * but allows patterns to get big without disasters. */

#define OP_CODE_SIZE 1
#define NEXT_PTR_SIZE 2
#define INDEX_SIZE 1
#define LENGTH_SIZE 4
#define NODE_SIZE (NEXT_PTR_SIZE + OP_CODE_SIZE)

inline uint8_t GET_OP_CODE(const uint8_t *p) {
	return *p;
}

inline uint8_t *OPERAND(uint8_t *p) {
	return p + NODE_SIZE;
}

inline int GET_OFFSET(uint8_t *p) {
	return (((*(p + 1) & 0xff) << 8) + ((*(p + 2)) & 0xff));
}

inline uint8_t PUT_OFFSET_L(ptrdiff_t v) {
	return static_cast<uint8_t>((v >> 8) & 0xff);
}

inline uint8_t PUT_OFFSET_R(ptrdiff_t v) {
	return  static_cast<uint8_t>(v & 0xff);
}

inline int GET_LOWER(uint8_t *p) {
	return (((*(p + NODE_SIZE) & 0xff) << 8) + ((*(p + NODE_SIZE + 1)) & 0xff));
}

inline int GET_UPPER(uint8_t *p) {
	return (((*(p + NODE_SIZE + 2) & 0xff) << 8) + ((*(p + NODE_SIZE + 3)) & 0xff));
}

// Utility definitions.
inline void SET_BIT(int &i, int n) {
	i |= (1 << (n - 1));
}

inline int TEST_BIT(int i, int n) {
	return i & (1 << (n - 1));
}

inline unsigned int U_CHAR_AT(const uint8_t *p) {
	return static_cast<unsigned int>(*p);
}



// Flags to be passed up and down via function parameters during compile.
#define WORST 0     // Worst case. No assumptions can be made.
#define HAS_WIDTH 1 // Known never to match null string.
#define SIMPLE 2    // Simple enough to be STAR/PLUS operand.

#define NO_PAREN 0    // Only set by initial call to "chunk".
#define PAREN 1       // Used for normal capturing parentheses.
#define NO_CAPTURE 2  // Non-capturing parentheses (grouping only).
#define INSENSITIVE 3 // Case insensitive parenthetical construct
#define SENSITIVE 4   // Case sensitive parenthetical construct
#define NEWLINE 5     // Construct to match newlines in most cases
#define NO_NEWLINE 6  // Construct to match newlines normally

#define REG_INFINITY 0UL
#define REG_ZERO 0UL
#define REG_ONE 1UL

// Flags for function shortcut_escape()

#define CHECK_ESCAPE 0       // Check an escape sequence for validity only.
#define CHECK_CLASS_ESCAPE 1 // Check the validity of an escape within a character class
#define EMIT_CLASS_BYTES 2   // Emit equivalent character class bytes, e.g \d=0123456789
#define EMIT_NODE 3          // Emit the appropriate node.

/* Number of bytes to offset from the beginning of the regex program to the
   start
   of the actual compiled regex code, i.e. skipping over the MAGIC number and
   the two counters at the front.  */

#define REGEX_START_OFFSET 3

#define MAX_COMPILED_SIZE 32767UL // Largest size a compiled regex can be. Probably could be 65535UL.

/*----------------------------------------------------------------------*
 * CompileRE
 *
 * Compiles a regular expression into the internal format used by
 * 'ExecRE'.
 *
 * The default behaviour wrt. case sensitivity and newline matching can
 * be controlled through the defaultFlags argument (Markus Schwarzenberg).
 * Future extensions are possible by using other flag bits.
 * Note that currently only the case sensitivity flag is effectively used.
 *
 * Beware that the optimization and preparation code in here knows about
 * some of the structure of the compiled regexp.
 *----------------------------------------------------------------------*/
RegExp::RegExp(const char *exp, int defaultFlags) {

	uint8_t *scan;
	int flags_local;
	len_range range_local;
	CompileState compileState;

#ifdef ENABLE_COUNTING_QUANTIFIER
	compileState.Brace_Char = '{';
	compileState.Meta_Char = &Default_Meta_Char[0];
#else
	compileState.Brace_Char = '*';                  // Bypass the '{' in
	compileState.Meta_Char = &Default_Meta_Char[1]; // Default_Meta_Char
#endif

    if (!exp) {
		throw RegexException("NULL argument, 'CompileRE'");
	}

	// Initialize arrays used by function 'shortcut_escape'.

	if (!init_ansi_classes()) {
		throw RegexException("internal error #1, 'CompileRE'");
	}

	compileState.Code_Emit_Ptr = &Compute_Size;
	compileState.Reg_Size = 0UL;

	/* We can't allocate space until we know how big the compiled form will be,
	  but we can't compile it (and thus know how big it is) until we've got a
	  place to put the code.  So we cheat: we compile it twice, once with code
	  generation turned off and size counting turned on, and once "for real".
	  This also means that we don't allocate space until we are sure that the
	  thing really will compile successfully, and we never have to move the
	  code and thus invalidate pointers into it.  (Note that it has to be in
	  one piece because free() must be able to free it all.) */

	for (int pass = 1; pass <= 2; pass++) {
		/*-------------------------------------------*
		* FIRST  PASS: Determine size and legality. *
		* SECOND PASS: Emit code.                   *
		*-------------------------------------------*/

		/*  Schwarzenberg:
		* If defaultFlags = 0 use standard defaults:
		*   compileState.Is_Case_Insensitive: Case sensitive is the default
		*   compileState.Match_Newline:       Newlines are NOT matched by default
		*                        in character classes
		*/
		compileState.Is_Case_Insensitive = ((defaultFlags & REDFLT_CASE_INSENSITIVE) ? true : false);
		compileState.Match_Newline = false; // ((defaultFlags & REDFLT_MATCH_NEWLINE)   ? true : false); Currently not used. Uncomment if needed.

        compileState.Reg_Parse = reinterpret_cast<const uint8_t *>(exp);
		Total_Paren = 1;
		Num_Braces = 0;
		compileState.Closed_Parens = 0;
		compileState.Paren_Has_Width = 0;

		emit_byte(MAGIC, compileState);
		emit_byte('%', compileState); // Placeholder for num of capturing parentheses.
		emit_byte('%', compileState); // Placeholder for num of general {m,n} constructs.

		if (chunk(NO_PAREN, &flags_local, &range_local, compileState) == nullptr) {
			throw RegexException("Internal Error"); // Something went wrong
		}

		if (pass == 1) {
			if (compileState.Reg_Size >= MAX_COMPILED_SIZE) {
				/* Too big for NEXT pointers NEXT_PTR_SIZE bytes long to span.
		   This is a real issue since the first BRANCH node usually points
		   to the end of the compiled regex code. */
				char Error_Text[128];
				sprintf(Error_Text, "regexp > %lu bytes", MAX_COMPILED_SIZE);
				throw RegexException(Error_Text);
			}

			// Allocate memory.
			program_ = new uint8_t[compileState.Reg_Size + 1];

			compileState.Code_Emit_Ptr = program_;
		}
	}

	program_[1] = (uint8_t)Total_Paren - 1;
	program_[2] = (uint8_t)Num_Braces;

	/*----------------------------------------*
	* Dig out information for optimizations. *
	*----------------------------------------*/

	match_start_ = '\0'; // Worst-case defaults.
	anchor_ = 0;

	// First BRANCH.

	scan = program_ + REGEX_START_OFFSET;

	if (GET_OP_CODE(next_ptr(scan)) == END) { // Only one top-level choice.
		scan = OPERAND(scan);

		// Starting-point info.

		if (GET_OP_CODE(scan) == EXACTLY) {
			match_start_ = *OPERAND(scan);

		} else if (PLUS <= GET_OP_CODE(scan) && GET_OP_CODE(scan) <= LAZY_PLUS) {

			// Allow x+ or x+? at the start of the regex to be optimized.

			if (GET_OP_CODE(scan + NODE_SIZE) == EXACTLY) {
				match_start_ = *OPERAND(scan + NODE_SIZE);
			}
		} else if (GET_OP_CODE(scan) == BOL) {
			anchor_++;
		}
	}
}

/*----------------------------------------------------------------------*
 * chunk                                                                *
 *                                                                      *
 * Process main body of regex or process a parenthesized "thing".       *
 *                                                                      *
 * Caller must absorb opening parenthesis.                              *
 *                                                                      *
 * Combining parenthesis handling with the base level of regular        *
 * expression is a trifle forced, but the need to tie the tails of the  *
 * branches to what follows makes it hard to avoid.                     *
 *----------------------------------------------------------------------*/

uint8_t *RegExp::chunk(int paren, int *flag_param, len_range *range_param, CompileState &cState) {

	uint8_t *ret_val = nullptr;
	uint8_t *this_branch;
	uint8_t *ender = nullptr;
	int this_paren = 0;
	int flags_local, first = 1, zero_width, i;
	bool old_sensitive = cState.Is_Case_Insensitive;
	bool old_newline = cState.Match_Newline;
	len_range range_local;
	int look_only = 0;
	uint8_t *emit_look_behind_bounds = nullptr;

	*flag_param = HAS_WIDTH; // Tentatively.
	range_param->lower = 0;  // Idem
	range_param->upper = 0;

	// Make an OPEN node, if parenthesized.

	if (paren == PAREN) {
		if (Total_Paren >= NSUBEXP) {
			char Error_Text[128];
			sprintf(Error_Text, "number of ()'s > %d", NSUBEXP);
			throw RegexException(Error_Text);
		}

		this_paren = Total_Paren;
		Total_Paren++;
		ret_val = emit_node(OPEN + this_paren, cState);
	} else if (paren == POS_AHEAD_OPEN || paren == NEG_AHEAD_OPEN) {
		*flag_param = WORST; // Look ahead is zero width.
		look_only = 1;
		ret_val = emit_node(paren, cState);
	} else if (paren == POS_BEHIND_OPEN || paren == NEG_BEHIND_OPEN) {
		*flag_param = WORST; // Look behind is zero width.
		look_only = 1;
		// We'll overwrite the zero length later on, so we save the ptr
		ret_val = emit_special(paren, 0, 0, cState);
		emit_look_behind_bounds = ret_val + NODE_SIZE;
	} else if (paren == INSENSITIVE) {
		cState.Is_Case_Insensitive = true;
	} else if (paren == SENSITIVE) {
		cState.Is_Case_Insensitive = false;
	} else if (paren == NEWLINE) {
		cState.Match_Newline = true;
	} else if (paren == NO_NEWLINE) {
		cState.Match_Newline = false;
	}

	// Pick up the branches, linking them together.

	do {
		this_branch = alternative(&flags_local, &range_local, cState);

		if (this_branch == nullptr)
			return nullptr;

		if (first) {
			first = 0;
			*range_param = range_local;
			if (ret_val == nullptr)
				ret_val = this_branch;
		} else if (range_param->lower >= 0) {
			if (range_local.lower >= 0) {
				if (range_local.lower < range_param->lower)
					range_param->lower = range_local.lower;
				if (range_local.upper > range_param->upper)
					range_param->upper = range_local.upper;
			} else {
				range_param->lower = -1; // Branches have different lengths
				range_param->upper = -1;
			}
		}

		tail(ret_val, this_branch); // Connect BRANCH -> BRANCH.

		/* If any alternative could be zero width, consider the whole
		 parenthisized thing to be zero width. */

		if (!(flags_local & HAS_WIDTH))
			*flag_param &= ~HAS_WIDTH;

		// Are there more alternatives to process?

        if (*cState.Reg_Parse != '|')
			break;

		cState.Reg_Parse++;
	} while (1);

	// Make a closing node, and hook it on the end.

	if (paren == PAREN) {
		ender = emit_node(CLOSE + this_paren, cState);

	} else if (paren == NO_PAREN) {
		ender = emit_node(END, cState);

	} else if (paren == POS_AHEAD_OPEN || paren == NEG_AHEAD_OPEN) {
		ender = emit_node(LOOK_AHEAD_CLOSE, cState);

	} else if (paren == POS_BEHIND_OPEN || paren == NEG_BEHIND_OPEN) {
		ender = emit_node(LOOK_BEHIND_CLOSE, cState);

	} else {
		ender = emit_node(NOTHING, cState);
	}

	tail(ret_val, ender);

	// Hook the tails of the branch alternatives to the closing node.

	for (this_branch = ret_val; this_branch != nullptr;) {
		branch_tail(this_branch, NODE_SIZE, ender);
		this_branch = next_ptr(this_branch);
	}

	// Check for proper termination.

    if (paren != NO_PAREN && *cState.Reg_Parse++ != ')') {
		throw RegexException("missing right parenthesis ')'");
    } else if (paren == NO_PAREN && *cState.Reg_Parse != '\0') {
        if (*cState.Reg_Parse == ')') {
			throw RegexException("missing left parenthesis '('");
		} else {
			throw RegexException("junk on end"); // "Can't happen" - NOTREACHED
		}
	}

	// Check whether look behind has a fixed size

	if (emit_look_behind_bounds) {
		if (range_param->lower < 0) {
			throw RegexException("look-behind does not have a bounded size");
		}
		if (range_param->upper > 65535L) {
			throw RegexException("max. look-behind size is too large (>65535)");
		}
		if (cState.Code_Emit_Ptr != &Compute_Size) {
			*emit_look_behind_bounds++ = PUT_OFFSET_L(range_param->lower);
			*emit_look_behind_bounds++ = PUT_OFFSET_R(range_param->lower);
			*emit_look_behind_bounds++ = PUT_OFFSET_L(range_param->upper);
			*emit_look_behind_bounds = PUT_OFFSET_R(range_param->upper);
		}
	}

	// For look ahead/behind, the length must be set to zero again
	if (look_only) {
		range_param->lower = 0;
		range_param->upper = 0;
	}

	zero_width = 0;

	/* Set a bit in cState.Closed_Parens to let future calls to function `back_ref'
	  know that we have closed this set of parentheses. */

	if (paren == PAREN && this_paren <= (int)sizeof(cState.Closed_Parens) * CHAR_BIT) {
		SET_BIT(cState.Closed_Parens, this_paren);

		/* Determine if a parenthesized expression is modified by a quantifier
		 that can have zero width. */

		if (*(cState.Reg_Parse) == '?' || *(cState.Reg_Parse) == '*') {
			zero_width++;
		} else if (*(cState.Reg_Parse) == '{' && cState.Brace_Char == '{') {
			if (*(cState.Reg_Parse + 1) == ',' || *(cState.Reg_Parse + 1) == '}') {
				zero_width++;
			} else if (*(cState.Reg_Parse + 1) == '0') {
				i = 2;

				while (*(cState.Reg_Parse + i) == '0')
					i++;

				if (*(cState.Reg_Parse + i) == ',')
					zero_width++;
			}
		}
	}

	/* If this set of parentheses is known to never match the empty string, set
	  a bit in cState.Paren_Has_Width to let future calls to function back_ref know
	  that this set of parentheses has non-zero width.  This will allow star
	  (*) or question (?) quantifiers to be aplied to a back-reference that
	  refers to this set of parentheses. */

	if ((*flag_param & HAS_WIDTH) && paren == PAREN && !zero_width &&
	    this_paren <= (int)(sizeof(cState.Paren_Has_Width) * CHAR_BIT)) {

		SET_BIT(cState.Paren_Has_Width, this_paren);
	}

	cState.Is_Case_Insensitive = old_sensitive;
	cState.Match_Newline = old_newline;

	return ret_val;
}

/*----------------------------------------------------------------------*
 * alternative
 *
 * Processes one alternative of an '|' operator.  Connects the NEXT
 * pointers of each regex atom together sequentialy.
 *----------------------------------------------------------------------*/
uint8_t *RegExp::alternative(int *flag_param, len_range *range_param, CompileState &cState) {

	uint8_t *ret_val;
	uint8_t *chain;
	uint8_t *latest;
	int flags_local;
	len_range range_local;

	*flag_param = WORST;    // Tentatively.
	range_param->lower = 0; // Idem
	range_param->upper = 0;

	ret_val = emit_node(BRANCH, cState);
	chain = nullptr;

	/* Loop until we hit the start of the next alternative, the end of this set
	  of alternatives (end of parentheses), or the end of the regex. */

    while (*cState.Reg_Parse != '|' && *cState.Reg_Parse != ')' && *cState.Reg_Parse != '\0') {
		latest = piece(&flags_local, &range_local, cState);

		if (latest == nullptr)
			return nullptr; // Something went wrong.

		*flag_param |= flags_local & HAS_WIDTH;
		if (range_local.lower < 0) {
			// Not a fixed length
			range_param->lower = -1;
			range_param->upper = -1;
		} else if (range_param->lower >= 0) {
			range_param->lower += range_local.lower;
			range_param->upper += range_local.upper;
		}

		if (chain) { // Connect the regex atoms together sequentialy.
			tail(chain, latest);
		}

		chain = latest;
	}

	if (chain == nullptr) { // Loop ran zero times.
		emit_node(NOTHING, cState);
	}

	return (ret_val);
}

/*----------------------------------------------------------------------*
 * piece - something followed by possible '*', '+', '?', or "{m,n}"
 *
 * Note that the branching code sequences used for the general cases of
 * *, +. ?, and {m,n} are somewhat optimized:  they use the same
 * NOTHING node as both the endmarker for their branch list and the
 * body of the last branch. It might seem that this node could be
 * dispensed with entirely, but the endmarker role is not redundant.
 *----------------------------------------------------------------------*/

uint8_t *RegExp::piece(int *flag_param, len_range *range_param, CompileState &cState) {

	uint8_t *ret_val;
	uint8_t *next;
	uint8_t op_code;
	unsigned long min_max[2] = {REG_ZERO, REG_INFINITY};
	int flags_local, i, brace_present = 0;
	int lazy = 0, comma_present = 0;
	int digit_present[2] = {0, 0};
	len_range range_local;

	ret_val = atom(&flags_local, &range_local, cState);

	if (ret_val == nullptr)
		return nullptr; // Something went wrong.

    op_code = *cState.Reg_Parse;

	if (!cState.isQuantifier(op_code)) {
		*flag_param = flags_local;
		*range_param = range_local;
		return (ret_val);
	} else if (op_code == '{') { // {n,m} quantifier present
		brace_present++;
		cState.Reg_Parse++;

		/* This code will allow specifying a counting range in any of the
		 following forms:

		 {m,n}  between m and n.
		 {,n}   same as {0,n} or between 0 and infinity.
		 {m,}   same as {m,0} or between m and infinity.
		 {m}    same as {m,m} or exactly m.
		 {,}    same as {0,0} or between 0 and infinity or just '*'.
		 {}     same as {0,0} or between 0 and infinity or just '*'.

		 Note that specifying a max of zero, {m,0} is not allowed in the regex
		 itself, but it is implemented internally that way to support '*', '+',
		 and {min,} constructs and signals an unlimited number. */

		for (i = 0; i < 2; i++) {
			/* Look for digits of number and convert as we go.  The numeric maximum
			value for max and min of 65,535 is due to using 2 bytes to store
			each value in the compiled regex code. */

            while (isdigit(*cState.Reg_Parse)) {
				// (6553 * 10 + 6) > 65535 (16 bit max)

                if ((min_max[i] == 6553UL && (*cState.Reg_Parse - '0') <= 5) || (min_max[i] <= 6552UL)) {

                    min_max[i] = (min_max[i] * 10UL) + (unsigned long)(*cState.Reg_Parse - '0');
					cState.Reg_Parse++;

					digit_present[i]++;
				} else {

					char Error_Text[128];

					if (i == 0) {
                        sprintf(Error_Text, "min operand of {%lu%c,???} > 65535", min_max[0], *cState.Reg_Parse);
					} else {
						sprintf(Error_Text, "max operand of {%lu,%lu%c} > 65535", min_max[0], min_max[1],
                                *cState.Reg_Parse);
					}

					throw RegexException(Error_Text);
				}
			}

            if (!comma_present && *cState.Reg_Parse == ',') {
				comma_present++;
				cState.Reg_Parse++;
			}
		}

		/* A max of zero can not be specified directly in the regex since it would
		 signal a max of infinity.  This code specifically disallows '{0,0}',
		 '{,0}', and '{0}' which really means nothing to humans but would be
		 interpreted as '{0,infinity}' or '*' if we didn't make this check. */

		if (digit_present[0] && (min_max[0] == REG_ZERO) && !comma_present) {

			throw RegexException("{0} is an invalid range");
		} else if (digit_present[0] && (min_max[0] == REG_ZERO) && digit_present[1] && (min_max[1] == REG_ZERO)) {

			throw RegexException("{0,0} is an invalid range");
		} else if (digit_present[1] && (min_max[1] == REG_ZERO)) {
			if (digit_present[0]) {
				char Error_Text[128];
				sprintf(Error_Text, "{%lu,0} is an invalid range", min_max[0]);
				throw RegexException(Error_Text);
			} else {
				throw RegexException("{,0} is an invalid range");
			}
		}

		if (!comma_present)
			min_max[1] = min_max[0]; // {x} means {x,x}

        if (*cState.Reg_Parse != '}') {
			throw RegexException("{m,n} specification missing right '}'");

		} else if (min_max[1] != REG_INFINITY && min_max[0] > min_max[1]) {
			// Disallow a backward range.
			char Error_Text[128];
			sprintf(Error_Text, "{%lu,%lu} is an invalid range", min_max[0], min_max[1]);
			throw RegexException(Error_Text);
		}
	}

	cState.Reg_Parse++;

	// Check for a minimal matching (non-greedy or "lazy") specification.

    if (*cState.Reg_Parse == '?') {
		lazy = 1;
		cState.Reg_Parse++;
	}

	// Avoid overhead of counting if possible

	if (op_code == '{') {
		if (min_max[0] == REG_ZERO && min_max[1] == REG_INFINITY) {
			op_code = '*';
		} else if (min_max[0] == REG_ONE && min_max[1] == REG_INFINITY) {
			op_code = '+';
		} else if (min_max[0] == REG_ZERO && min_max[1] == REG_ONE) {
			op_code = '?';
		} else if (min_max[0] == REG_ONE && min_max[1] == REG_ONE) {
			/* "x{1,1}" is the same as "x".  No need to pollute the compiled
			 regex with such nonsense. */

			*flag_param = flags_local;
			*range_param = range_local;
			return (ret_val);
		} else if (Num_Braces > (int)UCHAR_MAX) {
			char Error_Text[128];
			sprintf(Error_Text, "number of {m,n} constructs > %d", UCHAR_MAX);
			throw RegexException(Error_Text);
		}
	}

	if (op_code == '+')
		min_max[0] = REG_ONE;
	if (op_code == '?')
		min_max[1] = REG_ONE;

	/* It is dangerous to apply certain quantifiers to a possibly zero width
	  item. */

	if (!(flags_local & HAS_WIDTH)) {
		char Error_Text[128];
		if (brace_present) {
			sprintf(Error_Text, "{%lu,%lu} operand could be empty", min_max[0], min_max[1]);
		} else {
			sprintf(Error_Text, "%c operand could be empty", op_code);
		}

		throw RegexException(Error_Text);
	}

	*flag_param = (min_max[0] > REG_ZERO) ? (WORST | HAS_WIDTH) : WORST;
	if (range_local.lower >= 0) {
		if (min_max[1] != REG_INFINITY) {
			range_param->lower = range_local.lower * min_max[0];
			range_param->upper = range_local.upper * min_max[1];
		} else {
			range_param->lower = -1; // Not a fixed-size length
			range_param->upper = -1;
		}
	} else {
		range_param->lower = -1; // Not a fixed-size length
		range_param->upper = -1;
	}

	/*---------------------------------------------------------------------*
	*          Symbol  Legend  For  Node  Structure  Diagrams
	*---------------------------------------------------------------------*
	* (...) = general grouped thing
	* B     = (B)ranch,  K = bac(K),  N = (N)othing
	* I     = (I)nitialize count,     C = Increment (C)ount
	* T~m   = (T)est against mini(m)um- go to NEXT pointer if >= operand
	* T~x   = (T)est against ma(x)imum- go to NEXT pointer if >= operand
	* '~'   = NEXT pointer, \___| = forward pointer, |___/ = Backward pointer
	*---------------------------------------------------------------------*/

	if (op_code == '*' && (flags_local & SIMPLE)) {
		insert((lazy ? LAZY_STAR : STAR), ret_val, 0UL, 0UL, 0, cState);

	} else if (op_code == '+' && (flags_local & SIMPLE)) {
		insert(lazy ? LAZY_PLUS : PLUS, ret_val, 0UL, 0UL, 0, cState);

	} else if (op_code == '?' && (flags_local & SIMPLE)) {
		insert(lazy ? LAZY_QUESTION : QUESTION, ret_val, 0UL, 0UL, 0, cState);

	} else if (op_code == '{' && (flags_local & SIMPLE)) {
		insert(lazy ? LAZY_BRACE : BRACE, ret_val, min_max[0], min_max[1], 0, cState);

	} else if ((op_code == '*' || op_code == '+') && lazy) {
		/*  Node structure for (x)*?    Node structure for (x)+? construct.
	*  construct.                  (Same as (x)*? except for initial
	*                              forward jump into parenthesis.)
	*
	*                                  ___6____
	*   _______5_______               /________|______
	*  | _4__        1_\             /| ____   |     _\
	*  |/    |       / |\           / |/    |  |    / |\
	*  B~ N~ B~ (...)~ K~ N~       N~ B~ N~ B~ (...)~ K~ N~
	*      \  \___2_______|               \  \___________|
	*       \_____3_______|                \_____________|
	*
	*/

		tail(ret_val, emit_node(BACK, cState));              // 1
		(void)insert(BRANCH, ret_val, 0UL, 0UL, 0, cState);  // 2,4
		(void)insert(NOTHING, ret_val, 0UL, 0UL, 0, cState); // 3

		next = emit_node(NOTHING, cState); // 2,3

		offset_tail(ret_val, NODE_SIZE, next);        // 2
		tail(ret_val, next);                          // 3
		insert(BRANCH, ret_val, 0UL, 0UL, 0, cState); // 4,5
		tail(ret_val, ret_val + (2 * NODE_SIZE));     // 4
		offset_tail(ret_val, 3 * NODE_SIZE, ret_val); // 5

		if (op_code == '+') {
			insert(NOTHING, ret_val, 0UL, 0UL, 0, cState); // 6
			tail(ret_val, ret_val + (4 * NODE_SIZE));      // 6
		}
	} else if (op_code == '*') {
		/* Node structure for (x)* construct.
	*     ____1_____
	*    |          \
	*    B~ (...)~ K~ B~ N~
	*     \      \_|2 |\_|
	*      \__3_______|  4
	*/

		insert(BRANCH, ret_val, 0UL, 0UL, 0, cState);             // 1,3
		offset_tail(ret_val, NODE_SIZE, emit_node(BACK, cState)); // 2
		offset_tail(ret_val, NODE_SIZE, ret_val);                 // 1
		tail(ret_val, emit_node(BRANCH, cState));                 // 3
		tail(ret_val, emit_node(NOTHING, cState));                // 4
	} else if (op_code == '+') {
		/* Node structure for (x)+ construct.
	*
	*      ____2_____
	*     |          \
	*     (...)~ B~ K~ B~ N~
	*          \_|\____|\_|
	*          1     3    4
	*/

		next = emit_node(BRANCH, cState); // 1

		tail(ret_val, next);                       // 1
		tail(emit_node(BACK, cState), ret_val);    // 2
		tail(next, emit_node(BRANCH, cState));     // 3
		tail(ret_val, emit_node(NOTHING, cState)); // 4
	} else if (op_code == '?' && lazy) {
		/* Node structure for (x)?? construct.
	*      _4__        1_
	*     /    |       / |
	*    B~ N~ B~ (...)~ N~
	*        \  \___2____|
	*         \_____3____|
	*/

		(void)insert(BRANCH, ret_val, 0UL, 0UL, 0, cState);  // 2,4
		(void)insert(NOTHING, ret_val, 0UL, 0UL, 0, cState); // 3

		next = emit_node(NOTHING, cState); // 1,2,3

		offset_tail(ret_val, 2 * NODE_SIZE, next);    // 1
		offset_tail(ret_val, NODE_SIZE, next);        // 2
		tail(ret_val, next);                          // 3
		insert(BRANCH, ret_val, 0UL, 0UL, 0, cState); // 4
		tail(ret_val, (ret_val + (2 * NODE_SIZE)));   // 4

	} else if (op_code == '?') {
		/* Node structure for (x)? construct.
	*      ___1____  _2
	*     /        |/ |
	*    B~ (...)~ B~ N~
	*            \__3_|
	*/

		insert(BRANCH, ret_val, 0UL, 0UL, 0, cState); // 1
		tail(ret_val, emit_node(BRANCH, cState));     // 1

		next = emit_node(NOTHING, cState); // 2,3

		tail(ret_val, next);                   // 2
		offset_tail(ret_val, NODE_SIZE, next); // 3
	} else if (op_code == '{' && min_max[0] == min_max[1]) {
		/* Node structure for (x){m}, (x){m}?, (x){m,m}, or (x){m,m}? constructs.
	*Note that minimal and maximal matching mean the same thing when we
	*specify the minimum and maximum to be the same value.
	*      _______3_____
	*     |    1_  _2   \
	*     |    / |/ |    \
	*  I~ (...)~ C~ T~m K~ N~
	*   \_|          \_____|
	*    5              4
	*/

		tail(ret_val, emit_special(INC_COUNT, 0UL, Num_Braces, cState));         // 1
		tail(ret_val, emit_special(TEST_COUNT, min_max[0], Num_Braces, cState)); // 2
		tail(emit_node(BACK, cState), ret_val);                                  // 3
		tail(ret_val, emit_node(NOTHING, cState));                               // 4

		next = insert(INIT_COUNT, ret_val, 0UL, 0UL, Num_Braces, cState); // 5

		tail(ret_val, next); // 5

		Num_Braces++;
	} else if (op_code == '{' && lazy) {
		if (min_max[0] == REG_ZERO && min_max[1] != REG_INFINITY) {
			/* Node structure for (x){0,n}? or {,n}? construct.
	  *       _________3____________
	  *    8_| _4__        1_  _2   \
	  *    / |/    |       / |/ |    \
	  *   I~ B~ N~ B~ (...)~ C~ T~x K~ N~
	  *          \  \            \__7__|
	  *           \  \_________6_______|
	  *            \______5____________|
	  */

			tail(ret_val, emit_special(INC_COUNT, 0UL, Num_Braces, cState)); // 1

			next = emit_special(TEST_COUNT, min_max[0], Num_Braces, cState); // 2,7

			tail(ret_val, next);                                          // 2
			(void)insert(BRANCH, ret_val, 0UL, 0UL, Num_Braces, cState);  // 4,6
			(void)insert(NOTHING, ret_val, 0UL, 0UL, Num_Braces, cState); // 5
			(void)insert(BRANCH, ret_val, 0UL, 0UL, Num_Braces, cState);  // 3,4,8
			tail(emit_node(BACK, cState), ret_val);                       // 3
			tail(ret_val, ret_val + (2 * NODE_SIZE));                     // 4

			next = emit_node(NOTHING, cState); // 5,6,7

			offset_tail(ret_val, NODE_SIZE, next);     // 5
			offset_tail(ret_val, 2 * NODE_SIZE, next); // 6
			offset_tail(ret_val, 3 * NODE_SIZE, next); // 7

			next = insert(INIT_COUNT, ret_val, 0UL, 0UL, Num_Braces, cState); // 8

			tail(ret_val, next); // 8

		} else if (min_max[0] > REG_ZERO && min_max[1] == REG_INFINITY) {
			/* Node structure for (x){m,}? construct.
	  *       ______8_________________
	  *      |         _______3_____  \
	  *      | _7__   |    1_  _2   \  \
	  *      |/    |  |    / |/ |    \  \
	  *   I~ B~ N~ B~ (...)~ C~ T~m K~ K~ N~
	  *    \_____\__\_|          \_4___|  |
	  *       9   \  \_________5__________|
	  *            \_______6______________|
	  */

			tail(ret_val, emit_special(INC_COUNT, 0UL, Num_Braces, cState)); // 1

			next = emit_special(TEST_COUNT, min_max[0], Num_Braces, cState); // 2,4

			tail(ret_val, next);                                 // 2
			tail(emit_node(BACK, cState), ret_val);              // 3
			tail(ret_val, emit_node(BACK, cState));              // 4
			(void)insert(BRANCH, ret_val, 0UL, 0UL, 0, cState);  // 5,7
			(void)insert(NOTHING, ret_val, 0UL, 0UL, 0, cState); // 6

			next = emit_node(NOTHING, cState); // 5,6

			offset_tail(ret_val, NODE_SIZE, next);                           // 5
			tail(ret_val, next);                                             // 6
			(void)insert(BRANCH, ret_val, 0UL, 0UL, 0, cState);              // 7,8
			tail(ret_val, ret_val + (2 * NODE_SIZE));                        // 7
			offset_tail(ret_val, 3 * NODE_SIZE, ret_val);                    // 8
			(void)insert(INIT_COUNT, ret_val, 0UL, 0UL, Num_Braces, cState); // 9
			tail(ret_val, ret_val + INDEX_SIZE + (4 * NODE_SIZE));           // 9

		} else {
			/* Node structure for (x){m,n}? construct.
	  *       ______9_____________________
	  *      |         _____________3___  \
	  *      | __8_   |    1_  _2       \  \
	  *      |/    |  |    / |/ |        \  \
	  *   I~ B~ N~ B~ (...)~ C~ T~x T~m K~ K~ N~
	  *    \_____\__\_|          \   \__4__|  |
	  *      10   \  \            \_7_________|
	  *            \  \_________6_____________|
	  *             \_______5_________________|
	  */

			tail(ret_val, emit_special(INC_COUNT, 0UL, Num_Braces, cState)); // 1

			next = emit_special(TEST_COUNT, min_max[1], Num_Braces, cState); // 2,7

			tail(ret_val, next); // 2

			next = emit_special(TEST_COUNT, min_max[0], Num_Braces, cState); // 4

			tail(emit_node(BACK, cState), ret_val);              // 3
			tail(next, emit_node(BACK, cState));                 // 4
			(void)insert(BRANCH, ret_val, 0UL, 0UL, 0, cState);  // 6,8
			(void)insert(NOTHING, ret_val, 0UL, 0UL, 0, cState); // 5
			(void)insert(BRANCH, ret_val, 0UL, 0UL, 0, cState);  // 8,9

			next = emit_node(NOTHING, cState); // 5,6,7

			offset_tail(ret_val, NODE_SIZE, next);                     // 5
			offset_tail(ret_val, 2 * NODE_SIZE, next);                 // 6
			offset_tail(ret_val, 3 * NODE_SIZE, next);                 // 7
			tail(ret_val, ret_val + (2 * NODE_SIZE));                  // 8
			offset_tail(next, -NODE_SIZE, ret_val);                    // 9
			insert(INIT_COUNT, ret_val, 0UL, 0UL, Num_Braces, cState); // 10
			tail(ret_val, ret_val + INDEX_SIZE + (4 * NODE_SIZE));     // 10
		}

		Num_Braces++;
	} else if (op_code == '{') {
		if (min_max[0] == REG_ZERO && min_max[1] != REG_INFINITY) {
			/* Node structure for (x){0,n} or (x){,n} construct.
	  *
	  *       ___3____________
	  *      |       1_  _2   \   5_
	  *      |       / |/ |    \  / |
	  *   I~ B~ (...)~ C~ T~x K~ B~ N~
	  *    \_|\            \_6___|__|
	  *    7   \________4________|
	  */

			tail(ret_val, emit_special(INC_COUNT, 0UL, Num_Braces, cState)); // 1

			next = emit_special(TEST_COUNT, min_max[1], Num_Braces, cState); // 2,6

			tail(ret_val, next);                                // 2
			(void)insert(BRANCH, ret_val, 0UL, 0UL, 0, cState); // 3,4,7
			tail(emit_node(BACK, cState), ret_val);             // 3

			next = emit_node(BRANCH, cState); // 4,5

			tail(ret_val, next);                    // 4
			tail(next, emit_node(NOTHING, cState)); // 5,6
			offset_tail(ret_val, NODE_SIZE, next);  // 6

			next = insert(INIT_COUNT, ret_val, 0UL, 0UL, Num_Braces, cState); // 7

			tail(ret_val, next); // 7

		} else if (min_max[0] > REG_ZERO && min_max[1] == REG_INFINITY) {
			/* Node structure for (x){m,} construct.
	  *       __________4________
	  *      |    __3__________  \
	  *     _|___|    1_  _2   \  \    _7
	  *    / | 8 |    / |/ |    \  \  / |
	  *   I~ B~  (...)~ C~ T~m K~ K~ B~ N~
	  *       \             \_5___|  |
	  *        \__________6__________|
	  */

			tail(ret_val, emit_special(INC_COUNT, 0UL, Num_Braces, cState)); // 1

			next = emit_special(TEST_COUNT, min_max[0], Num_Braces, cState); // 2

			tail(ret_val, next);                                // 2
			tail(emit_node(BACK, cState), ret_val);             // 3
			(void)insert(BRANCH, ret_val, 0UL, 0UL, 0, cState); // 4,6

			next = emit_node(BACK, cState); // 4

			tail(next, ret_val);                       // 4
			offset_tail(ret_val, NODE_SIZE, next);     // 5
			tail(ret_val, emit_node(BRANCH, cState));  // 6
			tail(ret_val, emit_node(NOTHING, cState)); // 7

			insert(INIT_COUNT, ret_val, 0UL, 0UL, Num_Braces, cState); // 8

			tail(ret_val, ret_val + INDEX_SIZE + (2 * NODE_SIZE)); // 8

		} else {
			/* Node structure for (x){m,n} construct.
	  *       _____6________________
	  *      |   _____________3___  \
	  *    9_|__|    1_  _2       \  \    _8
	  *    / |  |    / |/ |        \  \  / |
	  *   I~ B~ (...)~ C~ T~x T~m K~ K~ B~ N~
	  *       \            \   \__4__|  |  |
	  *        \            \_7_________|__|
	  *         \_________5_____________|
	  */

			tail(ret_val, emit_special(INC_COUNT, 0UL, Num_Braces, cState)); // 1

			next = emit_special(TEST_COUNT, min_max[1], Num_Braces, cState); // 2,4

			tail(ret_val, next); // 2

			next = emit_special(TEST_COUNT, min_max[0], Num_Braces, cState); // 4

			tail(emit_node(BACK, cState), ret_val);             // 3
			tail(next, emit_node(BACK, cState));                // 4
			(void)insert(BRANCH, ret_val, 0UL, 0UL, 0, cState); // 5,6

			next = emit_node(BRANCH, cState); // 5,8

			tail(ret_val, next);                    // 5
			offset_tail(next, -NODE_SIZE, ret_val); // 6

			next = emit_node(NOTHING, cState); // 7,8

			offset_tail(ret_val, NODE_SIZE, next); // 7

			offset_tail(next, -NODE_SIZE, next);                             // 8
			(void)insert(INIT_COUNT, ret_val, 0UL, 0UL, Num_Braces, cState); // 9
			tail(ret_val, ret_val + INDEX_SIZE + (2 * NODE_SIZE));           // 9
		}

		Num_Braces++;
	} else {
		/* We get here if the IS_QUANTIFIER macro is not coordinated properly
		 with this function. */

		throw RegexException("internal error #2, `piece\'");
	}

	if (cState.isQuantifier(*cState.Reg_Parse)) {
		char Error_Text[128];
		if (op_code == '{') {
            sprintf(Error_Text, "nested quantifiers, {m,n}%c", *cState.Reg_Parse);
		} else {
            sprintf(Error_Text, "nested quantifiers, %c%c", op_code, *cState.Reg_Parse);
		}

		throw RegexException(Error_Text);
	}

	return (ret_val);
}

/*----------------------------------------------------------------------*
 * atom
 *
 * Process one regex item at the lowest level
 *
 * OPTIMIZATION:  Lumps a continuous sequence of ordinary characters
 * together so that it can turn them into a single EXACTLY node, which
 * is smaller to store and faster to run.
 *----------------------------------------------------------------------*/

uint8_t *RegExp::atom(int *flag_param, len_range *range_param, CompileState &cState) {

	uint8_t *ret_val;
	uint8_t test;
	int flags_local;
	len_range range_local;

	*flag_param = WORST;    // Tentatively.
	range_param->lower = 0; // Idem
	range_param->upper = 0;

	/* Process any regex comments, e.g. '(?# match next token->)'.  The
	  terminating right parenthesis can not be escaped.  The comment stops at
	  the first right parenthesis encountered (or the end of the regex
	  string)... period.  Handles multiple sequential comments,
	  e.g. '(?# one)(?# two)...'  */

    while (*cState.Reg_Parse == '(' && *(cState.Reg_Parse + 1) == '?' && *(cState.Reg_Parse + 2) == '#') {

		cState.Reg_Parse += 3;

        while (*cState.Reg_Parse != ')' && *cState.Reg_Parse != '\0') {
			cState.Reg_Parse++;
		}

        if (*cState.Reg_Parse == ')') {
			cState.Reg_Parse++;
		}

        if (*cState.Reg_Parse == ')' || *cState.Reg_Parse == '|' || *cState.Reg_Parse == '\0') {
			/* Hit end of regex string or end of parenthesized regex; have to
	  return "something" (i.e. a NOTHING node) to avoid generating an
	  error. */

			ret_val = emit_node(NOTHING, cState);

			return (ret_val);
		}
	}

    switch (*cState.Reg_Parse++) {
	case '^':
		ret_val = emit_node(BOL, cState);
		break;

	case '$':
		ret_val = emit_node(EOL, cState);
		break;

	case '<':
		ret_val = emit_node(BOWORD, cState);
		break;

	case '>':
		ret_val = emit_node(EOWORD, cState);
		break;

	case '.':
		if (cState.Match_Newline) {
			ret_val = emit_node(EVERY, cState);
		} else {
			ret_val = emit_node(ANY, cState);
		}

		*flag_param |= (HAS_WIDTH | SIMPLE);
		range_param->lower = 1;
		range_param->upper = 1;
		break;

	case '(':
        if (*cState.Reg_Parse == '?') { // Special parenthetical expression
			cState.Reg_Parse++;
			range_local.lower = 0; // Make sure it is always used
			range_local.upper = 0;

            if (*cState.Reg_Parse == ':') {
				cState.Reg_Parse++;
				ret_val = chunk(NO_CAPTURE, &flags_local, &range_local, cState);
            } else if (*cState.Reg_Parse == '=') {
				cState.Reg_Parse++;
				ret_val = chunk(POS_AHEAD_OPEN, &flags_local, &range_local, cState);
            } else if (*cState.Reg_Parse == '!') {
				cState.Reg_Parse++;
				ret_val = chunk(NEG_AHEAD_OPEN, &flags_local, &range_local, cState);
            } else if (*cState.Reg_Parse == 'i') {
				cState.Reg_Parse++;
				ret_val = chunk(INSENSITIVE, &flags_local, &range_local, cState);
            } else if (*cState.Reg_Parse == 'I') {
				cState.Reg_Parse++;
				ret_val = chunk(SENSITIVE, &flags_local, &range_local, cState);
            } else if (*cState.Reg_Parse == 'n') {
				cState.Reg_Parse++;
				ret_val = chunk(NEWLINE, &flags_local, &range_local, cState);
            } else if (*cState.Reg_Parse == 'N') {
				cState.Reg_Parse++;
				ret_val = chunk(NO_NEWLINE, &flags_local, &range_local, cState);
            } else if (*cState.Reg_Parse == '<') {
				cState.Reg_Parse++;
                if (*cState.Reg_Parse == '=') {
					cState.Reg_Parse++;
					ret_val = chunk(POS_BEHIND_OPEN, &flags_local, &range_local, cState);
                } else if (*cState.Reg_Parse == '!') {
					cState.Reg_Parse++;
					ret_val = chunk(NEG_BEHIND_OPEN, &flags_local, &range_local, cState);
				} else {
					char Error_Text[128];
                    sprintf(Error_Text, "invalid look-behind syntax, \"(?<%c...)\"", *cState.Reg_Parse);

					throw RegexException(Error_Text);
				}
			} else {
				char Error_Text[128];
                sprintf(Error_Text, "invalid grouping syntax, \"(?%c...)\"", *cState.Reg_Parse);

				throw RegexException(Error_Text);
			}
		} else { // Normal capturing parentheses
			ret_val = chunk(PAREN, &flags_local, &range_local, cState);
		}

		if (ret_val == nullptr)
			return nullptr; // Something went wrong.

		// Add HAS_WIDTH flag if it was set by call to chunk.

		*flag_param |= flags_local & HAS_WIDTH;
		*range_param = range_local;

		break;

	case '\0':
	case '|':
	case ')':
		throw RegexException("internal error #3, `atom\'"); // Supposed to be
	                                                        // caught earlier.
	case '?':
	case '+':
	case '*': {
		char Error_Text[128];
		sprintf(Error_Text, "%c follows nothing", *(cState.Reg_Parse - 1));
		throw RegexException(Error_Text);
	}

	case '{':
#ifdef ENABLE_COUNTING_QUANTIFIER
		throw RegexException("{m,n} follows nothing");
#else
		ret_val = emit_node(EXACTLY, cState); // Treat braces as literals.
		emit_byte('{', cState);
		emit_byte('\0', cState);
		range_param->lower = 1;
		range_param->upper = 1;
#endif

		break;

	case '[': {
		unsigned int second_value;
		unsigned int last_value;
		uint8_t last_emit = 0;

		// Handle characters that can only occur at the start of a class.

        if (*cState.Reg_Parse == '^') { // Complement of range.
			ret_val = emit_node(ANY_BUT, cState);
			cState.Reg_Parse++;

			/* All negated classes include newline unless escaped with
			      a "(?n)" switch. */

			if (!cState.Match_Newline)
				emit_byte('\n', cState);
		} else {
			ret_val = emit_node(ANY_OF, cState);
		}

        if (*cState.Reg_Parse == ']' || *cState.Reg_Parse == '-') {
			/* If '-' or ']' is the first character in a class,
			      it is a literal character in the class. */

            last_emit = *cState.Reg_Parse;
            emit_byte(*cState.Reg_Parse, cState);
			cState.Reg_Parse++;
		}

		// Handle the rest of the class characters.

        while (*cState.Reg_Parse != '\0' && *cState.Reg_Parse != ']') {
            if (*cState.Reg_Parse == '-') { // Process a range, e.g [a-z].
				cState.Reg_Parse++;

                if (*cState.Reg_Parse == ']' || *cState.Reg_Parse == '\0') {
					/* If '-' is the last character in a class it is a literal
					    character.  If `cState.Reg_Parse' points to the end of the
					    regex string, an error will be generated later. */

					emit_byte('-', cState);
					last_emit = '-';
				} else {
					/* We must get the range starting character value from the
					    emitted code since it may have been an escaped
					    character.  `second_value' is set one larger than the
					    just emitted character value.  This is done since
					    `second_value' is used as the start value for the loop
					    that emits the values in the range.  Since we have
					    already emitted the first character of the class, we do
					    not want to emit it again. */

					second_value = ((unsigned int)last_emit) + 1;

                    if (*cState.Reg_Parse == '\\') {
						/* Handle escaped characters within a class range.
						   Specifically disallow shortcut escapes as the end of
						   a class range.  To allow this would be ambiguous
						   since shortcut escapes represent a set of characters,
						   and it would not be clear which character of the
						   class should be treated as the "last" character. */

						cState.Reg_Parse++;

                        if ((test = numeric_escape(*cState.Reg_Parse, &cState.Reg_Parse))) {
							last_value = (unsigned int)test;
                        } else if ((test = literal_escape(*cState.Reg_Parse))) {
							last_value = (unsigned int)test;
                        } else if (shortcut_escape(*cState.Reg_Parse, nullptr, CHECK_CLASS_ESCAPE, cState)) {
							char Error_Text[128];
                            sprintf(Error_Text, "\\%c is not allowed as range operand", *cState.Reg_Parse);

							throw RegexException(Error_Text);
						} else {
							char Error_Text[128];
                            sprintf(Error_Text, "\\%c is an invalid char class escape sequence", *cState.Reg_Parse);
							throw RegexException(Error_Text);
						}
					} else {
						last_value = U_CHAR_AT(cState.Reg_Parse);
					}

					if (cState.Is_Case_Insensitive) {
						second_value = (unsigned int)tolower((int)second_value);
						last_value = (unsigned int)tolower((int)last_value);
					}

					/* For case insensitive, something like [A-_] will
					    generate an error here since ranges are converted to
					    lower case. */

					if (second_value - 1 > last_value) {
						throw RegexException("invalid [] range");
					}

					/* If only one character in range (e.g [a-a]) then this
					    loop is not run since the first character of any range
					    was emitted by the previous iteration of while loop. */

					for (; second_value <= last_value; second_value++) {
						emit_class_byte(second_value, cState);
					}

					last_emit = (uint8_t)last_value;

					cState.Reg_Parse++;

				} // End class character range code.
            } else if (*cState.Reg_Parse == '\\') {
				cState.Reg_Parse++;

                if ((test = numeric_escape(*cState.Reg_Parse, &cState.Reg_Parse)) != '\0') {
					emit_class_byte(test, cState);

					last_emit = test;
                } else if ((test = literal_escape(*cState.Reg_Parse)) != '\0') {
					emit_byte(test, cState);
					last_emit = test;
                } else if (shortcut_escape(*cState.Reg_Parse, nullptr, CHECK_CLASS_ESCAPE, cState)) {

					if (*(cState.Reg_Parse + 1) == '-') {
						/* Specifically disallow shortcut escapes as the start
						   of a character class range (see comment above.) */

						char Error_Text[128];
                        sprintf(Error_Text, "\\%c not allowed as range operand", *cState.Reg_Parse);

						throw RegexException(Error_Text);
					} else {
						/* Emit the bytes that are part of the shortcut
						   escape sequence's range (e.g. \d = 0123456789) */

                        shortcut_escape(*cState.Reg_Parse, nullptr, EMIT_CLASS_BYTES, cState);
					}
				} else {
					char Error_Text[128];
                    sprintf(Error_Text, "\\%c is an invalid char class escape sequence", *cState.Reg_Parse);

					throw RegexException(Error_Text);
				}

				cState.Reg_Parse++;

				// End of class escaped sequence code
			} else {
                emit_class_byte(*cState.Reg_Parse, cState); // Ordinary class character.

                last_emit = *cState.Reg_Parse;
				cState.Reg_Parse++;
			}
        } // End of while (*cState.Reg_Parse != '\0' && *cState.Reg_Parse != ']')

        if (*cState.Reg_Parse != ']')
			throw RegexException("missing right ']'");

		emit_byte('\0', cState);

		/* NOTE: it is impossible to specify an empty class.  This is
		       because [] would be interpreted as "begin character class"
		       followed by a literal ']' character and no "end character class"
		       delimiter (']').  Because of this, it is always safe to assume
		       that a class HAS_WIDTH. */

		cState.Reg_Parse++;
		*flag_param |= HAS_WIDTH | SIMPLE;
		range_param->lower = 1;
		range_param->upper = 1;
	}

	break; // End of character class code.

	case '\\':
        if ((ret_val = shortcut_escape(*cState.Reg_Parse, flag_param, EMIT_NODE, cState))) {

			cState.Reg_Parse++;
			range_param->lower = 1;
			range_param->upper = 1;
			break;

		} else if ((ret_val = back_ref(cState.Reg_Parse, flag_param, EMIT_NODE, cState))) {
			/* Can't make any assumptions about a back-reference as to SIMPLE
			   or HAS_WIDTH.  For example (^|<) is neither simple nor has
			   width.  So we don't flip bits in flag_param here. */

			cState.Reg_Parse++;
			// Back-references always have an unknown length
			range_param->lower = -1;
			range_param->upper = -1;
			break;
		}

	/* At this point it is apparent that the escaped character is not a
	    shortcut escape or back-reference.  Back up one character to allow
	    the default code to include it as an ordinary character. */

	/* Fall through to Default case to handle literal escapes and numeric
	    escapes. */

	default:
		cState.Reg_Parse--; /* If we fell through from the above code, we are now
		                 pointing at the back slash (\) character. */
		{
			const uint8_t *parse_save;
			int len = 0;

			if (cState.Is_Case_Insensitive) {
				ret_val = emit_node(SIMILAR, cState);
			} else {
				ret_val = emit_node(EXACTLY, cState);
			}

			/* Loop until we find a meta character, shortcut escape, back
			       reference, or end of regex string. */

            for (; *cState.Reg_Parse != '\0' && !strchr((char *)cState.Meta_Char, (int)*cState.Reg_Parse); len++) {

				/* Save where we are in case we have to back
				      this character out. */

				parse_save = cState.Reg_Parse;

                if (*cState.Reg_Parse == '\\') {
					cState.Reg_Parse++; // Point to escaped character

                    if ((test = numeric_escape(*cState.Reg_Parse, &cState.Reg_Parse))) {
						if (cState.Is_Case_Insensitive) {
							emit_byte(tolower(test), cState);
						} else {
							emit_byte(test, cState);
						}
                    } else if ((test = literal_escape(*cState.Reg_Parse))) {
						emit_byte(test, cState);
					} else if (back_ref(cState.Reg_Parse, nullptr, CHECK_ESCAPE, cState)) {
						// Leave back reference for next `atom' call

						cState.Reg_Parse--;
						break;
                    } else if (shortcut_escape(*cState.Reg_Parse, nullptr, CHECK_ESCAPE, cState)) {
						// Leave shortcut escape for next `atom' call

						cState.Reg_Parse--;
						break;
					} else {

						char Error_Text[128];
                        sprintf(Error_Text, "\\%c is an invalid escape sequence", *cState.Reg_Parse);
						throw RegexException(Error_Text);
					}

					cState.Reg_Parse++;
				} else {
					// Ordinary character

					if (cState.Is_Case_Insensitive) {
                        emit_byte(tolower(*cState.Reg_Parse), cState);
					} else {
                        emit_byte(*cState.Reg_Parse, cState);
					}

					cState.Reg_Parse++;
				}

				/* If next regex token is a quantifier (?, +. *, or {m,n}) and
				      our EXACTLY node so far is more than one character, leave the
				      last character to be made into an EXACTLY node one character
				      wide for the multiplier to act on.  For example 'abcd* would
				      have an EXACTLY node with an 'abc' operand followed by a STAR
				      node followed by another EXACTLY node with a 'd' operand. */

				if (cState.isQuantifier(*cState.Reg_Parse) && len > 0) {
					cState.Reg_Parse = parse_save; // Point to previous regex token.

					if (cState.Code_Emit_Ptr == &Compute_Size) {
						cState.Reg_Size--;
					} else {
						cState.Code_Emit_Ptr--; // Write over previously emitted byte.
					}

					break;
				}
			}

			if (len <= 0)
				throw RegexException("internal error #4, `atom\'");

			*flag_param |= HAS_WIDTH;

			if (len == 1)
				*flag_param |= SIMPLE;

			range_param->lower = len;
			range_param->upper = len;

			emit_byte('\0', cState);
		}
    } // END switch (*cState.Reg_Parse++)

	return (ret_val);
}

/*----------------------------------------------------------------------*
 * emit_node
 *
 * Emit (if appropriate) the op code for a regex node atom.
 *
 * The NEXT pointer is initialized to nullptr.
 *
 * Returns a pointer to the START of the emitted node.
 *----------------------------------------------------------------------*/

uint8_t *RegExp::emit_node(uint8_t op_code, CompileState &cState) {

	uint8_t *ret_val;
	uint8_t *ptr;

	ret_val = cState.Code_Emit_Ptr; // Return address of start of node

	if (ret_val == &Compute_Size) {
		cState.Reg_Size += NODE_SIZE;
	} else {
		ptr = ret_val;
		*ptr++ = op_code;
		*ptr++ = '\0'; // Null "NEXT" pointer.
		*ptr++ = '\0';

		cState.Code_Emit_Ptr = ptr;
	}

	return ret_val;
}

/*----------------------------------------------------------------------*
 * emit_byte
 *
 * Emit (if appropriate) a byte of code (usually part of an operand.)
 *----------------------------------------------------------------------*/

void RegExp::emit_byte(uint8_t c, CompileState &cState) {

	if (cState.Code_Emit_Ptr == &Compute_Size) {
		cState.Reg_Size++;
	} else {
		*cState.Code_Emit_Ptr++ = c;
	}
}

/*----------------------------------------------------------------------*
 * emit_class_byte
 *
 * Emit (if appropriate) a byte of code (usually part of a character
 * class operand.)
 *----------------------------------------------------------------------*/

void RegExp::emit_class_byte(uint8_t c, CompileState &cState) {

	if (cState.Code_Emit_Ptr == &Compute_Size) {
		cState.Reg_Size++;

		if (cState.Is_Case_Insensitive && isalpha(c))
			cState.Reg_Size++;
	} else if (cState.Is_Case_Insensitive && isalpha(c)) {
		/* For case insensitive character classes, emit both upper and lower case
		 versions of alphabetical characters. */

		*cState.Code_Emit_Ptr++ = tolower(c);
		*cState.Code_Emit_Ptr++ = toupper(c);
	} else {
		*cState.Code_Emit_Ptr++ = c;
	}
}

/*----------------------------------------------------------------------*
 * emit_special
 *
 * Emit nodes that need special processing.
 *----------------------------------------------------------------------*/

uint8_t *RegExp::emit_special(uint8_t op_code, unsigned long test_val, int index, CompileState &cState) {

	uint8_t *ret_val = &Compute_Size;
	uint8_t *ptr;

	if (cState.Code_Emit_Ptr == &Compute_Size) {
		switch (op_code) {
		case POS_BEHIND_OPEN:
		case NEG_BEHIND_OPEN:
			cState.Reg_Size += LENGTH_SIZE; // Length of the look-behind match
			cState.Reg_Size += NODE_SIZE;   // Make room for the node
			break;

		case TEST_COUNT:
			cState.Reg_Size += NEXT_PTR_SIZE; // Make room for a test value.

		case INC_COUNT:
			cState.Reg_Size += INDEX_SIZE; // Make room for an index value.

		default:
			cState.Reg_Size += NODE_SIZE; // Make room for the node.
		}
	} else {
		ret_val = emit_node(op_code, cState); // Return the address for start of node.
		ptr = cState.Code_Emit_Ptr;

		if (op_code == INC_COUNT || op_code == TEST_COUNT) {
			*ptr++ = (uint8_t)index;

			if (op_code == TEST_COUNT) {
				*ptr++ = PUT_OFFSET_L(test_val);
				*ptr++ = PUT_OFFSET_R(test_val);
			}
		} else if (op_code == POS_BEHIND_OPEN || op_code == NEG_BEHIND_OPEN) {
			*ptr++ = PUT_OFFSET_L(test_val);
			*ptr++ = PUT_OFFSET_R(test_val);
			*ptr++ = PUT_OFFSET_L(test_val);
			*ptr++ = PUT_OFFSET_R(test_val);
		}

		cState.Code_Emit_Ptr = ptr;
	}

	return (ret_val);
}

/*----------------------------------------------------------------------*
 * insert
 *
 * Insert a node in front of already emitted node(s).  Means relocating
 * the operand.  cStateCode_Emit_Ptr points one byte past the just emitted
 * node and operand.  The parameter `insert_pos' points to the location
 * where the new node is to be inserted.
 *----------------------------------------------------------------------*/

uint8_t *RegExp::insert(uint8_t op, uint8_t *insert_pos, long min, long max, int index, CompileState &cState) {

	uint8_t *src;
	uint8_t *dst;
	uint8_t *place;
	int insert_size = NODE_SIZE;

	if (op == BRACE || op == LAZY_BRACE) {
		// Make room for the min and max values.

		insert_size += (2 * NEXT_PTR_SIZE);
	} else if (op == INIT_COUNT) {
		// Make room for an index value .

		insert_size += INDEX_SIZE;
	}

	if (cState.Code_Emit_Ptr == &Compute_Size) {
		cState.Reg_Size += insert_size;
		return &Compute_Size;
	}

	src = cState.Code_Emit_Ptr;
	cState.Code_Emit_Ptr += insert_size;
	dst = cState.Code_Emit_Ptr;

	// Relocate the existing emitted code to make room for the new node.

	while (src > insert_pos)
		*--dst = *--src;

	place = insert_pos; // Where operand used to be.
	*place++ = op;      // Inserted operand.
	*place++ = '\0';    // NEXT pointer for inserted operand.
	*place++ = '\0';

	if (op == BRACE || op == LAZY_BRACE) {
		*place++ = PUT_OFFSET_L(min);
		*place++ = PUT_OFFSET_R(min);

		*place++ = PUT_OFFSET_L(max);
		*place++ = PUT_OFFSET_R(max);
	} else if (op == INIT_COUNT) {
		*place++ = (uint8_t)index;
	}

	return place; // Return a pointer to the start of the code moved.
}

/*----------------------------------------------------------------------*
 * tail - Set the next-pointer at the end of a node chain.
 *----------------------------------------------------------------------*/

void RegExp::tail(uint8_t *search_from, uint8_t *point_to) {

	uint8_t *next;

	if (search_from == &Compute_Size) {
		return;
	}

	// Find the last node in the chain (node with a null NEXT pointer)

	uint8_t *scan = search_from;

	for (;;) {
		next = next_ptr(scan);

		if (!next)
			break;

		scan = next;
	}

	ptrdiff_t offset;
	if (GET_OP_CODE(scan) == BACK) {
		offset = scan - point_to;
	} else {
		offset = point_to - scan;
	}

	// Set NEXT pointer

	*(scan + 1) = PUT_OFFSET_L(offset);
	*(scan + 2) = PUT_OFFSET_R(offset);
}

/*--------------------------------------------------------------------*
 * offset_tail
 *
 * Perform a tail operation on (ptr + offset).
 *--------------------------------------------------------------------*/
void RegExp::offset_tail(uint8_t *ptr, int offset, uint8_t *val) {

	if (ptr == &Compute_Size || ptr == nullptr)
		return;

	tail(ptr + offset, val);
}

/*--------------------------------------------------------------------*
 * branch_tail
 *
 * Perform a tail operation on (ptr + offset) but only if `ptr' is a
 * BRANCH node.
 *--------------------------------------------------------------------*/
void RegExp::branch_tail(uint8_t *ptr, int offset, uint8_t *val) {

	if (ptr == &Compute_Size || ptr == nullptr || GET_OP_CODE(ptr) != BRANCH) {
		return;
	}

	tail(ptr + offset, val);
}

/*--------------------------------------------------------------------*
 * shortcut_escape
 *
 * Implements convenient escape sequences that represent entire
 * character classes or special location assertions (similar to escapes
 * supported by Perl)
 *                                                  _
 *    \d     Digits                  [0-9]           |
 *    \D     NOT a digit             [^0-9]          | (Examples
 *    \l     Letters                 [a-zA-Z]        |  at left
 *    \L     NOT a Letter            [^a-zA-Z]       |    are
 *    \s     Whitespace              [ \t\n\r\f\v]   |    for
 *    \S     NOT Whitespace          [^ \t\n\r\f\v]  |     C
 *    \w     "Word" character        [a-zA-Z0-9_]    |   Locale)
 *    \W     NOT a "Word" character  [^a-zA-Z0-9_]  _|
 *
 *    \B     Matches any character that is NOT a word-delimiter
 *
 *    Codes for the "emit" parameter:
 *
 *    EMIT_NODE
 *       Emit a shortcut node.  Shortcut nodes have an implied set of
 *       class characters.  This helps keep the compiled regex string
 *       small.
 *
 *    EMIT_CLASS_BYTES
 *       Emit just the equivalent characters of the class.  This makes
 *       the escape usable from within a class, e.g. [a-fA-F\d].  Only
 *       \d, \D, \s, \S, \w, and \W can be used within a class.
 *
 *    CHECK_ESCAPE
 *       Only verify that this is a valid shortcut escape.
 *
 *    CHECK_CLASS_ESCAPE
 *       Same as CHECK_ESCAPE but only allows characters valid within
 *       a class.
 *
 *--------------------------------------------------------------------*/

uint8_t *RegExp::shortcut_escape(uint8_t c, int *flag_param, int emitType, CompileState &cState) {

	const char *characterClass = nullptr;
	static const uint8_t *const codes = (uint8_t *)"ByYdDlLsSwW";
	uint8_t *ret_val = (uint8_t *)1; // Assume success.
	const uint8_t *valid_codes;

	if (emitType == EMIT_CLASS_BYTES || emitType == CHECK_CLASS_ESCAPE) {
		valid_codes = codes + 3; // \B, \y and \Y are not allowed in classes
	} else {
		valid_codes = codes;
	}

	if (!strchr((char *)valid_codes, (int)c)) {
		return nullptr; // Not a valid shortcut escape sequence
	} else if (emitType == CHECK_ESCAPE || emitType == CHECK_CLASS_ESCAPE) {
		return ret_val; // Just checking if this is a valid shortcut escape.
	}

	switch (c) {
	case 'd':
	case 'D':
		if (emitType == EMIT_CLASS_BYTES) {
			characterClass = ASCII_Digits;
		} else if (emitType == EMIT_NODE) {
			ret_val = (islower(c) ? emit_node(DIGIT, cState) : emit_node(NOT_DIGIT, cState));
		}

		break;

	case 'l':
	case 'L':
		if (emitType == EMIT_CLASS_BYTES) {
			characterClass = Letter_Char;
		} else if (emitType == EMIT_NODE) {
			ret_val = (islower(c) ? emit_node(LETTER, cState) : emit_node(NOT_LETTER, cState));
		}

		break;

	case 's':
	case 'S':
		if (emitType == EMIT_CLASS_BYTES) {
			if (cState.Match_Newline)
				emit_byte('\n', cState);

			characterClass = White_Space;
		} else if (emitType == EMIT_NODE) {
			if (cState.Match_Newline) {
				ret_val = (islower(c) ? emit_node(SPACE_NL, cState) : emit_node(NOT_SPACE_NL, cState));
			} else {
				ret_val = (islower(c) ? emit_node(SPACE, cState) : emit_node(NOT_SPACE, cState));
			}
		}

		break;

	case 'w':
	case 'W':
		if (emitType == EMIT_CLASS_BYTES) {
			characterClass = Word_Char;
		} else if (emitType == EMIT_NODE) {
			ret_val = (islower(c) ? emit_node(WORD_CHAR, cState) : emit_node(NOT_WORD_CHAR, cState));
		}

		break;

	/* Since the delimiter table is not available at regex compile time \B,
	 \Y and \Y can only generate a node.  At run time, the delimiter table
	 will be available for these nodes to use. */

	case 'y':

		if (emitType == EMIT_NODE) {
			ret_val = emit_node(IS_DELIM, cState);
		} else {
			throw RegexException("internal error #5 `shortcut_escape\'");
		}

		break;

	case 'Y':

		if (emitType == EMIT_NODE) {
			ret_val = emit_node(NOT_DELIM, cState);
		} else {
			throw RegexException("internal error #6 `shortcut_escape\'");
		}

		break;

	case 'B':

		if (emitType == EMIT_NODE) {
			ret_val = emit_node(NOT_BOUNDARY, cState);
		} else {
			throw RegexException("internal error #7 `shortcut_escape\'");
		}

		break;

	default:
		/* We get here if there isn't a case for every character in
		    the string "codes" */

		throw RegexException("internal error #8 `shortcut_escape\'");
	}

	if (emitType == EMIT_NODE && c != 'B') {
		*flag_param |= (HAS_WIDTH | SIMPLE);
	}

	if (characterClass) {
		// Emit bytes within a character class operand.

		while (*characterClass != '\0') {
			emit_byte(*characterClass++, cState);
		}
	}

	return ret_val;
}

/*--------------------------------------------------------------------*
 * numeric_escape
 *
 * Implements hex and octal numeric escape sequence syntax.
 *
 * Hexadecimal Escape: \x##    Max of two digits  Must have leading 'x'.
 * Octal Escape:       \0###   Max of three digits and not greater
 *                             than 377 octal.  Must have leading zero.
 *
 * Returns the actual character value or NULL if not a valid hex or
 * octal escape.  REG_FAIL is called if \x0, \x00, \0, \00, \000, or
 * \0000 is specified.
 *--------------------------------------------------------------------*/

uint8_t RegExp::numeric_escape(uint8_t c, const uint8_t **parse) {

	static uint8_t digits[] = "fedcbaFEDCBA9876543210";

	static unsigned int digit_val[] = {15, 14, 13, 12, 11, 10,              // Lower case Hex digits
	                                   15, 14, 13, 12, 11, 10,              // Upper case Hex digits
	                                   9,  8,  7,  6,  5,  4,  3, 2, 1, 0}; // Decimal Digits

	uint8_t *pos_ptr;
	uint8_t *digit_str;
	unsigned int value = 0;
	unsigned int radix = 8;
	int width = 3; // Can not be bigger than \0xff
	int pos_delta = 14;
	int i;

	switch (c) {
	case '0':
		digit_str = digits + pos_delta; // Only use Octal digits, i.e. 0-7.

		break;

	case 'x':
	case 'X':
		width = 2; // Can not be bigger than \0xff
		radix = 16;
		pos_delta = 0;
		digit_str = digits; // Use all of the digit characters.

		break;

	default:
		return '\0'; // Not a numeric escape
	}

	const uint8_t *scan = *parse;
	scan++; // Only change *parse on success.

	pos_ptr = (uint8_t *)strchr((char *)digit_str, (int)*scan);

	for (i = 0; pos_ptr != nullptr && (i < width); i++) {
		size_t pos = (pos_ptr - digit_str) + pos_delta;
		value = (value * radix) + digit_val[pos];

		/* If this digit makes the value over 255, treat this digit as a literal
		 character instead of part of the numeric escape.  For example, \0777
		 will be processed as \077 (an 'M') and a literal '7' character, NOT
		 511 decimal which is > 255. */

		if (value > 255) {
			// Back out calculations for last digit processed.

			value -= digit_val[pos];
			value /= radix;

			break; /* Note that scan will not be incremented and still points to
			       the digit that caused overflow.  It will be decremented by
			       the "else" below to point to the last character that is
			       considered to be part of the octal escape. */
		}

		scan++;
		pos_ptr = (uint8_t *)strchr((char *)digit_str, (int)*scan);
	}

	// Handle the case of "\0" i.e. trying to specify a NULL character.

	if (value == 0) {
		char Error_Text[128];
		if (c == '0') {
			sprintf(Error_Text, "\\00 is an invalid octal escape");
		} else {
			sprintf(Error_Text, "\\%c0 is an invalid hexadecimal escape", c);
		}
		throw RegexException(Error_Text);
	} else {
		// Point to the last character of the number on success.

		scan--;
		*parse = scan;
	}

	return (uint8_t)value;
}

/*--------------------------------------------------------------------*
 * literal_escape
 *
 * Recognize escaped literal characters (prefixed with backslash),
 * and translate them into the corresponding character.
 *
 * Returns the proper character value or NULL if not a valid literal
 * escape.
 *--------------------------------------------------------------------*/

uint8_t RegExp::literal_escape(uint8_t c) {

	static const uint8_t valid_escape[] = {
		'a', 'b', 'e', 'f', 'n',  'r', 't', 'v', '(', ')', '-', '[',  ']', '<',
		'>', '{', '}', '.', '\\', '|', '^', '$', '*', '+', '?', '&', '\0'
	};

	static const uint8_t value[] = {'\a', '\b',
#ifdef EBCDIC_CHARSET
	                          0x27, // Escape character in IBM's EBCDIC character set.
#else
	                          0x1B, // Escape character in ASCII character set.
#endif
	                          '\f', '\n', '\r', '\t', '\v', '(', ')', '-', '[', ']', '<', '>',
	                          '{',  '}',  '.',  '\\', '|',  '^', '$', '*', '+', '?', '&', '\0'};

	for (int i = 0; valid_escape[i] != '\0'; i++) {
		if (c == valid_escape[i]) {
			return value[i];
		}
	}

	return '\0';
}

/*--------------------------------------------------------------------*
 * back_ref
 *
 * Process a request to match a previous parenthesized thing.
 * Parenthetical entities are numbered beginning at 1 by counting
 * opening parentheses from left to to right.  \0 would represent
 * whole match, but would confuse numeric_escape as an octal escape,
 * so it is forbidden.
 *
 * Constructs of the form \~1, \~2, etc. are cross-regex back
 * references and are used in syntax highlighting patterns to match
 * text previously matched by another regex. *** IMPLEMENT LATER ***
 *--------------------------------------------------------------------*/

uint8_t *RegExp::back_ref(const uint8_t *c, int *flag_param, int emitType, CompileState &cState) {

	int paren_no;
	int c_offset = 0;
	int is_cross_regex = 0;

	uint8_t *ret_val;

	// Implement cross regex backreferences later.

	/* if (*c == (uint8_t) ('~')) {
	  c_offset++;
	  is_cross_regex++;
  } */

    paren_no = (c[c_offset] - (uint8_t)('0'));

	if (!isdigit(c[c_offset]) || // Only \1, \2, ... \9 are supported.
	    paren_no == 0) {             // Should be caught by numeric_escape.

		return nullptr;
	}

	// Make sure parentheses for requested back-reference are complete.

	if (!is_cross_regex && !TEST_BIT(cState.Closed_Parens, paren_no)) {
		char Error_Text[128];
		sprintf(Error_Text, "\\%d is an illegal back reference", paren_no);
		throw RegexException(Error_Text);
	}

	if (emitType == EMIT_NODE) {
		if (is_cross_regex) {
			cState.Reg_Parse++; /* Skip past the '~' in a cross regex back reference.
			             We only do this if we are emitting code. */

			if (cState.Is_Case_Insensitive) {
				ret_val = emit_node(X_REGEX_BR_CI, cState);
			} else {
				ret_val = emit_node(X_REGEX_BR, cState);
			}
		} else {
			if (cState.Is_Case_Insensitive) {
				ret_val = emit_node(BACK_REF_CI, cState);
			} else {
				ret_val = emit_node(BACK_REF, cState);
			}
		}

		emit_byte((uint8_t)paren_no, cState);

		if (is_cross_regex || TEST_BIT(cState.Paren_Has_Width, paren_no)) {
			*flag_param |= HAS_WIDTH;
		}
	} else if (emitType == CHECK_ESCAPE) {
		ret_val = (uint8_t *)1;
	} else {
		ret_val = nullptr;
	}

	return ret_val;
}

/*======================================================================*
 *  Regex execution related code
 *======================================================================*/

/*
 * Measured recursion limits:
 *    Linux:      +/-  40 000 (up to 110 000)
 *    Solaris:    +/-  85 000
 *    HP-UX 11:   +/- 325 000
 *
 * So 10 000 ought to be safe.
 */
#define REGEX_RECURSION_LIMIT 10000

// Define a pointer to an array to hold general (...){m,n} counts.

struct brace_counts {
	unsigned long count[1]; // More unwarranted chumminess with compiler.
};

static struct brace_counts *Brace;

/*
 * ExecRE - match a `regexp' structure against a string
 *
 * If `end' is non-NULL, matches may not BEGIN past end, but may extend past
 * it.  If reverse is true, `end' must be specified, and searching begins at
 * `end'.  "isbol" should be set to true if the beginning of the string is the
 * actual beginning of a line (since `ExecRE' can't look backwards from the
 * beginning to find whether there was a newline before).  Likewise, "isbow"
 * asks whether the string is preceded by a word delimiter.  End of string is
 * always treated as a word and line boundary (there may be cases where it
 * shouldn't be, in which case, this should be changed).  "delimit" (if
 * non-null) specifies a null-terminated string of characters to be considered
 * word delimiters matching "<" and ">".  if "delimit" is NULL, the default
 * delimiters (as set in SetREDefaultWordDelimiters) are used.
 * Look_behind_to indicates the position till where it is safe to
 * perform look-behind matches. If set, it should be smaller than or equal
 * to the start position of the search (pointed at by string). If it is NULL,
 * it defaults to the start position.
 * Finally, match_to indicates the logical end of the string, till where
 * matches are allowed to extend. Note that look-ahead patterns may look
 * past that boundary. If match_to is set to NULL, the terminating \0 is
 * assumed to correspond to the logical boundary. Match_to, if set, must be
 * larger than or equal to end, if set.
 */

int RegExp::ExecRE(const char *string, const char *end, bool reverse, char prev_char, char succ_char,
                   const char *delimiters, const char *look_behind_to, const char *match_to) {

	uint8_t tempDelimitTable[256];
	const char *str;
	const char **s_ptr;
	const char **e_ptr;
	int ret_val = 0;
	int i;
	ExecState state;

	// Check for valid parameters.

	if (!string) {
		fprintf(stderr, "NULL parameter to 'ExecRE'");
		goto SINGLE_RETURN;
	}

	// Check validity of program.

	if (U_CHAR_AT(program_) != MAGIC) {
		fprintf(stderr, "corrupted program");
		goto SINGLE_RETURN;
	}

	s_ptr = startp_;
	e_ptr = endp_;

	// If caller has supplied delimiters, make a delimiter table

	if (delimiters == nullptr) {
		Current_Delimiters = Default_Delimiters;
	} else {
		Current_Delimiters = makeDelimiterTable((uint8_t *)delimiters, (uint8_t *)tempDelimitTable);
	}

	// Remember the logical end of the string.

	state.End_Of_String = match_to;

	if (end == nullptr && reverse) {
		for (end = string; !state.atEndOfString(end); end++)
			;
		succ_char = '\n';
	} else if (end == nullptr) {
		succ_char = '\n';
	}

	// Initialize arrays used by shortcut_escape.

	if (!init_ansi_classes())
		goto SINGLE_RETURN;

	// Remember the beginning of the string for matching BOL

	state.Start_Of_String = string;
	state.Look_Behind_To  = look_behind_to ? look_behind_to : string;

	state.Prev_Is_BOL   = ((prev_char == '\n') || (prev_char == '\0') ? true : false);
	state.Succ_Is_EOL   = ((succ_char == '\n') || (succ_char == '\0') ? true : false);
	state.Prev_Is_Delim = (Current_Delimiters[(uint8_t)prev_char] ? true : false);
	state.Succ_Is_Delim = (Current_Delimiters[(uint8_t)succ_char] ? true : false);

	Total_Paren = (int)(program_[1]);
	Num_Braces  = (int)(program_[2]);

	// Reset the recursion detection flag
	Recursion_Limit_Exceeded = false;

	// Allocate memory for {m,n} construct counting variables if need be.
	if (Num_Braces > 0) {
		Brace = new brace_counts[Num_Braces];
	} else {
		Brace = nullptr;
	}

	/* Initialize the first nine (9) capturing parentheses start and end
	  pointers to point to the start of the search string.  This is to prevent
	  crashes when later trying to reference captured parens that do not exist
	  in the compiled regex.  We only need to do the first nine since users
	  can only specify \1, \2, ... \9. */

	for (i = 9; i > 0; i--) {
		*s_ptr++ = string;
		*e_ptr++ = string;
	}

	if (!reverse) { // Forward Search
		if (anchor_) {
			// Search is anchored at BOL

			if (attempt(string, state)) {
				ret_val = 1;
				goto SINGLE_RETURN;
			}

			for (str = string; !state.atEndOfString(str) && str != end && !Recursion_Limit_Exceeded; str++) {

				if (*str == '\n') {
					if (attempt(str + 1, state)) {
						ret_val = 1;
						break;
					}
				}
			}

			goto SINGLE_RETURN;

		} else if (match_start_ != '\0') {
			// We know what char match must start with.

			for (str = string; !state.atEndOfString(str) && str != end && !Recursion_Limit_Exceeded; str++) {

				if (*str == (uint8_t)match_start_) {
					if (attempt(str, state)) {
						ret_val = 1;
						break;
					}
				}
			}

			goto SINGLE_RETURN;
		} else {
			// General case

			for (str = string; !state.atEndOfString(str) && str != end && !Recursion_Limit_Exceeded; str++) {

				if (attempt(str, state)) {
					ret_val = 1;
					break;
				}
			}

			// Beware of a single $ matching \0
			if (!Recursion_Limit_Exceeded && !ret_val && state.atEndOfString(str) && str != end) {
				if (attempt(str, state)) {
					ret_val = 1;
				}
			}

			goto SINGLE_RETURN;
		}
	} else { // Search reverse, same as forward, but loops run backward

		// Make sure that we don't start matching beyond the logical end
		if (state.End_Of_String != nullptr && end > state.End_Of_String) {
			end = state.End_Of_String;
		}

		if (anchor_) {
			// Search is anchored at BOL

			for (str = (end - 1); str >= string && !Recursion_Limit_Exceeded; str--) {

				if (*str == '\n') {
					if (attempt(str + 1, state)) {
						ret_val = 1;
						goto SINGLE_RETURN;
					}
				}
			}

			if (!Recursion_Limit_Exceeded && attempt(string, state)) {
				ret_val = 1;
				goto SINGLE_RETURN;
			}

			goto SINGLE_RETURN;
		} else if (match_start_ != '\0') {
			// We know what char match must start with.

			for (str = end; str >= string && !Recursion_Limit_Exceeded; str--) {

				if (*str == (uint8_t)match_start_) {
					if (attempt(str, state)) {
						ret_val = 1;
						break;
					}
				}
			}

			goto SINGLE_RETURN;
		} else {
			// General case

			for (str = end; str >= string && !Recursion_Limit_Exceeded; str--) {

				if (attempt(str, state)) {
					ret_val = 1;
					break;
				}
			}
		}
	}

SINGLE_RETURN:
	delete [] Brace;

	if (Recursion_Limit_Exceeded)
		return 0;

	return ret_val;
}

/*--------------------------------------------------------------------*
 * init_ansi_classes
 *
 * Generate character class sets using locale aware ANSI C functions.
 *
 *--------------------------------------------------------------------*/
bool RegExp::init_ansi_classes() {

	static bool initialized = false;
	int word_count;
	int letter_count;
	int space_count;

	if (!initialized) {
		initialized = true; // Only need to generate character sets once.
		word_count   = 0;
		letter_count = 0;
		space_count  = 0;

		for (int i = 1; i < UCHAR_MAX; i++) {
		
			const char ch = i;
		
			if (isalnum(ch) || ch == '_') {
				Word_Char[word_count++] = ch;
			}

			if (isalpha(ch)) {
				Letter_Char[letter_count++] = ch;
			}

			/* Note: Whether or not newline is considered to be whitespace is
			handled by switches within the original regex and is thus omitted
			here. */

			if (isspace(ch) && (ch != '\n')) {
				White_Space[space_count++] = ch;
			}

			/* Make sure arrays are big enough.  ("- 2" because of zero array
			origin and we need to leave room for the NULL terminator.) */

			if (word_count > (ALNUM_CHAR_SIZE - 2) || space_count > (WHITE_SPACE_SIZE - 2) ||
			    letter_count > (ALNUM_CHAR_SIZE - 2)) {

				fprintf(stderr, "internal error #9 'init_ansi_classes'");
				return false;
			}
		}

		Word_Char[word_count]    = '\0';
		Letter_Char[word_count]  = '\0';
		White_Space[space_count] = '\0';
	}

	return true;
}

/*----------------------------------------------------------------------*
 * match - main matching routine
 *
 * Conceptually the strategy is simple: check to see whether the
 * current node matches, call self recursively to see whether the rest
 * matches, and then act accordingly.  In practice we make some effort
 * to avoid recursion, in particular by going through "ordinary" nodes
 * (that don't need to know whether the rest of the match failed) by a
 * loop instead of by recursion.  Returns 0 failure, 1 success.
 *----------------------------------------------------------------------*/
#define MATCH_RETURN(X)                                                                                                \
	{                                                                                                                  \
		--Recursion_Count;                                                                                             \
		return (X);                                                                                                    \
	}
#define CHECK_RECURSION_LIMIT                                                                                          \
	if (Recursion_Limit_Exceeded)                                                                                      \
		MATCH_RETURN(0);

int RegExp::match(uint8_t *prog, int *branch_index_param, ExecState &state) {

	uint8_t *scan;       // Current node.
	uint8_t *next;       // Next node.
	int next_ptr_offset; // Used by the NEXT_PTR () macro

	if (++Recursion_Count > REGEX_RECURSION_LIMIT) {
		if (!Recursion_Limit_Exceeded) { // Prevent duplicate errors
			fprintf(stderr, "recursion limit exceeded, please respecify expression");
		}
		Recursion_Limit_Exceeded = true;
		MATCH_RETURN(0);
	}

	scan = prog;

	while (scan != nullptr) {
		NEXT_PTR(scan, next);

		switch (GET_OP_CODE(scan)) {
		case BRANCH: {
			const char *save;
			int branch_index_local = 0;

			if (GET_OP_CODE(next) != BRANCH) { // No choice.
				next = OPERAND(scan);          // Avoid recursion.
			} else {
				do {
					save = state.Reg_Input;

					if (match(OPERAND(scan), nullptr, state)) {
						if (branch_index_param) {
							*branch_index_param = branch_index_local;
						}
						MATCH_RETURN(1);
					}

					CHECK_RECURSION_LIMIT

					++branch_index_local;

					state.Reg_Input = save; // Backtrack.
					NEXT_PTR(scan, scan);
				} while (scan != nullptr && GET_OP_CODE(scan) == BRANCH);

				MATCH_RETURN(0); // NOT REACHED
			}
		}

		break;

		case EXACTLY: {
			int len;
			uint8_t *opnd;

			opnd = OPERAND(scan);

			// Inline the first character, for speed.

			if (*opnd != *state.Reg_Input)
				MATCH_RETURN(0);

			len = static_cast<int>(strlen((char *)opnd));

			if (state.End_Of_String != nullptr && state.Reg_Input + len > state.End_Of_String) {
				MATCH_RETURN(0);
			}

			if (len > 1 && strncmp((char *)opnd, (char *)state.Reg_Input, len) != 0) {

				MATCH_RETURN(0);
			}

			state.Reg_Input += len;
		}

		break;

		case SIMILAR: {
			uint8_t *opnd;
			uint8_t test;

			opnd = OPERAND(scan);

			/* Note: the SIMILAR operand was converted to lower case during
			      regex compile. */

			while ((test = *opnd++) != '\0') {
				if (state.atEndOfString(state.Reg_Input) || tolower(*state.Reg_Input++) != test) {

					MATCH_RETURN(0);
				}
			}
		}

		break;

		case BOL: // '^' (beginning of line anchor)
			if (state.Reg_Input == state.Start_Of_String) {
				if (state.Prev_Is_BOL)
					break;
			} else if (*(state.Reg_Input - 1) == '\n') {
				break;
			}

			MATCH_RETURN(0);

		case EOL: // '$' anchor matches end of line and end of string
			if (*state.Reg_Input == '\n' || (state.atEndOfString(state.Reg_Input) && state.Succ_Is_EOL)) {
				break;
			}

			MATCH_RETURN(0);

		case BOWORD: // '<' (beginning of word anchor)
			         /* Check to see if the current character is not a delimiter
			            and the preceding character is. */
			{
				int prev_is_delim;
				if (state.Reg_Input == state.Start_Of_String) {
					prev_is_delim = state.Prev_Is_Delim;
				} else {
					prev_is_delim = Current_Delimiters[(int)*(state.Reg_Input - 1)];
				}
				if (prev_is_delim) {
					int current_is_delim;
					if (state.atEndOfString(state.Reg_Input)) {
						current_is_delim = state.Succ_Is_Delim;
					} else {
						current_is_delim = Current_Delimiters[(int)*state.Reg_Input];
					}
					if (!current_is_delim)
						break;
				}
			}

			MATCH_RETURN(0);

		case EOWORD: // '>' (end of word anchor)
			         /* Check to see if the current character is a delimiter
			        and the preceding character is not. */
			{
				int prev_is_delim;
				if (state.Reg_Input == state.Start_Of_String) {
					prev_is_delim = state.Prev_Is_Delim;
				} else {
					prev_is_delim = Current_Delimiters[(int)*(state.Reg_Input - 1)];
				}
				if (!prev_is_delim) {
					int current_is_delim;
					if (state.atEndOfString(state.Reg_Input)) {
						current_is_delim = state.Succ_Is_Delim;
					} else {
						current_is_delim = Current_Delimiters[(int)*state.Reg_Input];
					}
					if (current_is_delim)
						break;
				}
			}

			MATCH_RETURN(0);

		case NOT_BOUNDARY: // \B (NOT a word boundary)
		{
			int prev_is_delim;
			int current_is_delim;
			if (state.Reg_Input == state.Start_Of_String) {
				prev_is_delim = state.Prev_Is_Delim;
			} else {
				prev_is_delim = Current_Delimiters[(int)*(state.Reg_Input - 1)];
			}
			if (state.atEndOfString(state.Reg_Input)) {
				current_is_delim = state.Succ_Is_Delim;
			} else {
				current_is_delim = Current_Delimiters[(int)*state.Reg_Input];
			}
			if (!(prev_is_delim ^ current_is_delim))
				break;
		}

			MATCH_RETURN(0);

		case IS_DELIM: // \y (A word delimiter character.)
			if (Current_Delimiters[(int)*state.Reg_Input] && !state.atEndOfString(state.Reg_Input)) {
				state.Reg_Input++;
				break;
			}

			MATCH_RETURN(0);

		case NOT_DELIM: // \Y (NOT a word delimiter character.)
			if (!Current_Delimiters[(int)*state.Reg_Input] && !state.atEndOfString(state.Reg_Input)) {
				state.Reg_Input++;
				break;
			}

			MATCH_RETURN(0);

		case WORD_CHAR: // \w (word character; alpha-numeric or underscore)
			if ((isalnum(*state.Reg_Input) || *state.Reg_Input == '_') && !state.atEndOfString(state.Reg_Input)) {
				state.Reg_Input++;
				break;
			}

			MATCH_RETURN(0);

		case NOT_WORD_CHAR: // \W (NOT a word character)
			if (isalnum(*state.Reg_Input) || *state.Reg_Input == '_' || *state.Reg_Input == '\n' ||
			   state.atEndOfString(state.Reg_Input))
				MATCH_RETURN(0);

			state.Reg_Input++;
			break;

		case ANY: // '.' (matches any character EXCEPT newline)
			if (state.atEndOfString(state.Reg_Input) || *state.Reg_Input == '\n')
				MATCH_RETURN(0);

			state.Reg_Input++;
			break;

		case EVERY: // '.' (matches any character INCLUDING newline)
			if (state.atEndOfString(state.Reg_Input))
				MATCH_RETURN(0);

			state.Reg_Input++;
			break;

		case DIGIT: // \d, same as [0123456789]
			if (!isdigit((int)*state.Reg_Input) ||state.atEndOfString(state.Reg_Input))
				MATCH_RETURN(0);

			state.Reg_Input++;
			break;

		case NOT_DIGIT: // \D, same as [^0123456789]
			if (isdigit((int)*state.Reg_Input) || *state.Reg_Input == '\n' ||state.atEndOfString(state.Reg_Input))
				MATCH_RETURN(0);

			state.Reg_Input++;
			break;

		case LETTER: // \l, same as [a-zA-Z]
			if (!isalpha((int)*state.Reg_Input) ||state.atEndOfString(state.Reg_Input))
				MATCH_RETURN(0);

			state.Reg_Input++;
			break;

		case NOT_LETTER: // \L, same as [^0123456789]
			if (isalpha((int)*state.Reg_Input) || *state.Reg_Input == '\n' ||state.atEndOfString(state.Reg_Input))
				MATCH_RETURN(0);

			state.Reg_Input++;
			break;

		case SPACE: // \s, same as [ \t\r\f\v]
			if (!isspace((int)*state.Reg_Input) || *state.Reg_Input == '\n' ||state.atEndOfString(state.Reg_Input))
				MATCH_RETURN(0);

			state.Reg_Input++;
			break;

		case SPACE_NL: // \s, same as [\n \t\r\f\v]
			if (!isspace((int)*state.Reg_Input) ||state.atEndOfString(state.Reg_Input))
				MATCH_RETURN(0);

			state.Reg_Input++;
			break;

		case NOT_SPACE: // \S, same as [^\n \t\r\f\v]
			if (isspace((int)*state.Reg_Input) ||state.atEndOfString(state.Reg_Input))
				MATCH_RETURN(0);

			state.Reg_Input++;
			break;

		case NOT_SPACE_NL: // \S, same as [^ \t\r\f\v]
			if ((isspace((int)*state.Reg_Input) && *state.Reg_Input != '\n') ||state.atEndOfString(state.Reg_Input))
				MATCH_RETURN(0);

			state.Reg_Input++;
			break;

		case ANY_OF: // [...] character class.
			if (state.atEndOfString(state.Reg_Input))
				MATCH_RETURN(0); /* Needed because strchr ()
				                   considers \0 as a member
				                   of the character set. */

			if (strchr((char *)OPERAND(scan), (int)*state.Reg_Input) == nullptr) {
				MATCH_RETURN(0);
			}

			state.Reg_Input++;
			break;

		case ANY_BUT: /* [^...] Negated character class-- does NOT normally
		               match newline (\n added usually to operand at compile
		               time.) */

			if (state.atEndOfString(state.Reg_Input))
				MATCH_RETURN(0); // See comment for ANY_OF.

			if (strchr((char *)OPERAND(scan), (int)*state.Reg_Input) != nullptr) {
				MATCH_RETURN(0);
			}

			state.Reg_Input++;
			break;

		case NOTHING:
		case BACK:
			break;

		case STAR:
		case PLUS:
		case QUESTION:
		case BRACE:

		case LAZY_STAR:
		case LAZY_PLUS:
		case LAZY_QUESTION:
		case LAZY_BRACE: {
			unsigned long num_matched = REG_ZERO;
			unsigned long min = ULONG_MAX, max = REG_ZERO;
			const char *save;
			uint8_t next_char;
			uint8_t *next_op;
			int lazy = 0;

			/* Lookahead (when possible) to avoid useless match attempts
			      when we know what character comes next. */

			if (GET_OP_CODE(next) == EXACTLY) {
				next_char = *OPERAND(next);
			} else {
				next_char = '\0'; // i.e. Don't know what next character is.
			}

			next_op = OPERAND(scan);

			switch (GET_OP_CODE(scan)) {
			case LAZY_STAR:
				lazy = 1;
			case STAR:
				min = REG_ZERO;
				max = ULONG_MAX;
				break;

			case LAZY_PLUS:
				lazy = 1;
			case PLUS:
				min = REG_ONE;
				max = ULONG_MAX;
				break;

			case LAZY_QUESTION:
				lazy = 1;
			case QUESTION:
				min = REG_ZERO;
				max = REG_ONE;
				break;

			case LAZY_BRACE:
				lazy = 1;
			case BRACE:
				min = (unsigned long)GET_OFFSET(scan + NEXT_PTR_SIZE);

				max = (unsigned long)GET_OFFSET(scan + (2 * NEXT_PTR_SIZE));

				if (max <= REG_INFINITY)
					max = ULONG_MAX;

				next_op = OPERAND(scan + (2 * NEXT_PTR_SIZE));
			}

			save = state.Reg_Input;

			if (lazy) {
				if (min > REG_ZERO)
					num_matched = greedy(next_op, min, state);
			} else {
				num_matched = greedy(next_op, max, state);
			}

			while (min <= num_matched && num_matched <= max) {
				if (next_char == '\0' || next_char == *state.Reg_Input) {
					if (match(next, nullptr, state))
						MATCH_RETURN(1);

					CHECK_RECURSION_LIMIT
				}

				// Couldn't or didn't match.

				if (lazy) {
					if (!greedy(next_op, 1, state))
						MATCH_RETURN(0);

					num_matched++; // Inch forward.
				} else if (num_matched > REG_ZERO) {
					num_matched--; // Back up.
				} else if (min == REG_ZERO && num_matched == REG_ZERO) {
					break;
				}

				state.Reg_Input = save + num_matched;
			}

			MATCH_RETURN(0);
		}

		break;

		case END:
			if (state.Extent_Ptr_FW == nullptr || (state.Reg_Input - state.Extent_Ptr_FW) > 0) {
				state.Extent_Ptr_FW = state.Reg_Input;
			}

			MATCH_RETURN(1); // Success!

			break;

		case INIT_COUNT:
			Brace->count[*OPERAND(scan)] = REG_ZERO;

			break;

		case INC_COUNT:
			Brace->count[*OPERAND(scan)]++;

			break;

		case TEST_COUNT:
			if (Brace->count[*OPERAND(scan)] < (unsigned long)GET_OFFSET(scan + NEXT_PTR_SIZE + INDEX_SIZE)) {

				next = scan + NODE_SIZE + INDEX_SIZE + NEXT_PTR_SIZE;
			}

			break;

		case BACK_REF:
		case BACK_REF_CI:
			// case X_REGEX_BR:
			// case X_REGEX_BR_CI: *** IMPLEMENT LATER
			{
				int paren_no = (int)*OPERAND(scan);

				/* if (GET_OP_CODE (scan) == X_REGEX_BR ||
				       GET_OP_CODE (scan) == X_REGEX_BR_CI) {

				      if (Cross_Regex_Backref == nullptr) MATCH_RETURN (0);

				      captured =
				         (uint8_t *) Cross_Regex_Backref->startp [paren_no];

				      finish =
				         (uint8_t *) Cross_Regex_Backref->endp   [paren_no];
				   } else { */
				const char *captured = state.Back_Ref_Start[paren_no];
				const char *finish = state.Back_Ref_End[paren_no];
				// }

				if ((captured != nullptr) && (finish != nullptr)) {
					if (captured > finish)
						MATCH_RETURN(0);

					if (GET_OP_CODE(scan) == BACK_REF_CI /* ||
                                  GET_OP_CODE (scan) == X_REGEX_BR_CI*/) {

						while (captured < finish) {
							if (state.atEndOfString(state.Reg_Input) ||
							    tolower(*captured++) != tolower(*state.Reg_Input++)) {
								MATCH_RETURN(0);
							}
						}
					} else {
						while (captured < finish) {
							if (state.atEndOfString(state.Reg_Input) || *captured++ != *state.Reg_Input++)
								MATCH_RETURN(0);
						}
					}

					break;
				} else {
					MATCH_RETURN(0);
				}
			}

		case POS_AHEAD_OPEN:
		case NEG_AHEAD_OPEN: {

			const char *save = state.Reg_Input;

			/* Temporarily ignore the logical end of the string, to allow
			      lookahead past the end. */
			const char *saved_end = state.End_Of_String;
			state.End_Of_String = nullptr;

			int answer = match(next, nullptr, state); // Does the look-ahead regex match?

			CHECK_RECURSION_LIMIT

			if ((GET_OP_CODE(scan) == POS_AHEAD_OPEN) ? answer : !answer) {
				/* Remember the last (most to the right) character position
				     that we consume in the input for a successful match.  This
				     is info that may be needed should an attempt be made to
				     match the exact same text at the exact same place.  Since
				     look-aheads backtrack, a regex with a trailing look-ahead
				     may need more text than it matches to accomplish a
				     re-match. */

				if (state.Extent_Ptr_FW == nullptr || (state.Reg_Input - state.Extent_Ptr_FW) > 0) {
					state.Extent_Ptr_FW = state.Reg_Input;
				}

				state.Reg_Input = save;          // Backtrack to look-ahead start.
				state.End_Of_String = saved_end; // Restore logical end.

				/* Jump to the node just after the (?=...) or (?!...)
				     Construct. */

				next = next_ptr(OPERAND(scan)); // Skip 1st branch
				// Skip the chain of branches inside the look-ahead
				while (GET_OP_CODE(next) == BRANCH)
					next = next_ptr(next);
				next = next_ptr(next); // Skip the LOOK_AHEAD_CLOSE
			} else {
				state.Reg_Input = save;          // Backtrack to look-ahead start.
				state.End_Of_String = saved_end; // Restore logical end.

				MATCH_RETURN(0);
			}
		}

		break;

		case POS_BEHIND_OPEN:
		case NEG_BEHIND_OPEN: {
			int answer;
			int offset;
			int found = 0;

			const char *save = state.Reg_Input;
			const char *saved_end = state.End_Of_String;

			/* Prevent overshoot (greedy matching could end past the
			      current position) by tightening the matching boundary.
			      Lookahead inside lookbehind can still cross that boundary. */
			state.End_Of_String = state.Reg_Input;

			int lower = GET_LOWER(scan);
			int upper = GET_UPPER(scan);

			/* Start with the shortest match first. This is the most
			      efficient direction in general.
			      Note! Negative look behind is _very_ tricky when the length
			      is not constant: we have to make sure the expression doesn't
			      match for _any_ of the starting positions. */
			for (offset = lower; offset <= upper; ++offset) {
				state.Reg_Input = save - offset;

				if (state.Reg_Input < state.Look_Behind_To) {
					// No need to look any further
					break;
				}

				answer = match(next, nullptr, state); // Does the look-behind regex match?

				CHECK_RECURSION_LIMIT

				/* The match must have ended at the current position;
				             otherwise it is invalid */
				if (answer && state.Reg_Input == save) {
					// It matched, exactly far enough
					found = 1;

					/* Remember the last (most to the left) character position
					    that we consume in the input for a successful match.
					    This is info that may be needed should an attempt be
					    made to match the exact same text at the exact same
					    place. Since look-behind backtracks, a regex with a
					    leading look-behind may need more text than it matches
					    to accomplish a re-match. */

					if (state.Extent_Ptr_BW == nullptr || (state.Extent_Ptr_BW - (save - offset)) > 0) {
						state.Extent_Ptr_BW = save - offset;
					}

					break;
				}
			}

			// Always restore the position and the logical string end.
			state.Reg_Input = save;
			state.End_Of_String = saved_end;

			if ((GET_OP_CODE(scan) == POS_BEHIND_OPEN) ? found : !found) {
				/* The look-behind matches, so we must jump to the next
				     node. The look-behind node is followed by a chain of
				     branches (contents of the look-behind expression), and
				     terminated by a look-behind-close node. */
				next = next_ptr(OPERAND(scan) + LENGTH_SIZE); // 1st branch
				// Skip the chained branches inside the look-ahead
				while (GET_OP_CODE(next) == BRANCH)
					next = next_ptr(next);
				next = next_ptr(next); // Skip LOOK_BEHIND_CLOSE
			} else {
				// Not a match
				MATCH_RETURN(0);
			}
		} break;

		case LOOK_AHEAD_CLOSE:
		case LOOK_BEHIND_CLOSE:
			MATCH_RETURN(1); /* We have reached the end of the look-ahead or
			          look-behind which implies that we matched it,
			  so return TRUE. */
		default:
			if ((GET_OP_CODE(scan) > OPEN) && (GET_OP_CODE(scan) < OPEN + NSUBEXP)) {

				int no = GET_OP_CODE(scan) - OPEN;
				const char *save = state.Reg_Input;

				if (no < 10) {
					state.Back_Ref_Start[no] = save;
					state.Back_Ref_End[no] = nullptr;
				}

				if (match(next, nullptr, state)) {
					/* Do not set `state.Start_Ptr_Ptr' if some later invocation (think
					 recursion) of the same parentheses already has. */

					if (state.Start_Ptr_Ptr[no] == nullptr)
						state.Start_Ptr_Ptr[no] = save;

					MATCH_RETURN(1);
				} else {
					MATCH_RETURN(0);
				}
			} else if ((GET_OP_CODE(scan) > CLOSE) && (GET_OP_CODE(scan) < CLOSE + NSUBEXP)) {

				int no = GET_OP_CODE(scan) - CLOSE;
				const char *save = state.Reg_Input;

				if (no < 10)
					state.Back_Ref_End[no] = save;

				if (match(next, nullptr, state)) {
					/* Do not set `state.End_Ptr_Ptr' if some later invocation of the
					 same parentheses already has. */

					if (state.End_Ptr_Ptr[no] == nullptr)
						state.End_Ptr_Ptr[no] = save;

					MATCH_RETURN(1);
				} else {
					MATCH_RETURN(0);
				}
			} else {
				fprintf(stderr, "memory corruption, 'match'");

				MATCH_RETURN(0);
			}

			break;
		}

		scan = next;
	}

	/* We get here only if there's trouble -- normally "case END" is
	  the terminating point. */

	fprintf(stderr, "corrupted pointers, 'match'");

	MATCH_RETURN(0);
}

/*----------------------------------------------------------------------*
 * greedy
 *
 * Repeatedly match something simple up to "max" times. If max <= 0
 * then match as much as possible (max = infinity).  Uses unsigned long
 * variables to maximize the amount of text matchable for unbounded
 * qualifiers like '*' and '+'.  This will allow at least 4,294,967,295
 * matches (4 Gig!) for an ANSI C compliant compiler.  If you are
 * applying a regex to something bigger than that, you shouldn't be
 * using NEdit!
 *
 * Returns the actual number of matches.
 *----------------------------------------------------------------------*/

unsigned long RegExp::greedy(uint8_t *p, long max, ExecState &state) {


	unsigned long count = REG_ZERO;

	const char *input_str = state.Reg_Input;
	uint8_t *operand         = OPERAND(p); // Literal char or start of class characters.
	unsigned long max_cmp    = (max > 0) ? (unsigned long)max : ULONG_MAX;

	switch (GET_OP_CODE(p)) {
	case ANY:
		/* Race to the end of the line or string. Dot DOESN'T match
		    newline. */

		while (count < max_cmp && *input_str != '\n' && !state.atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case EVERY:
		// Race to the end of the line or string. Dot DOES match newline.

		while (count < max_cmp && !state.atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case EXACTLY: // Count occurrences of single character operand.
		while (count < max_cmp && *operand == *input_str && !state.atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case SIMILAR: // Case insensitive version of EXACTLY
		while (count < max_cmp && *operand == tolower(*input_str) && !state.atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case ANY_OF: // [...] character class.
		while (count < max_cmp && strchr((char *)operand, static_cast<int>(*input_str)) != nullptr &&
			   !state.atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case ANY_BUT: /* [^...] Negated character class- does NOT normally
	                   match newline (\n added usually to operand at compile
	                   time.) */

		while (count < max_cmp && strchr((char *)operand, static_cast<int>(*input_str)) == nullptr &&
			   !state.atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case IS_DELIM: /* \y (not a word delimiter char)
	                     NOTE: '\n' and '\0' are always word delimiters. */

		while (count < max_cmp && Current_Delimiters[(int)*input_str] && !state.atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case NOT_DELIM: /* \Y (not a word delimiter char)
	                     NOTE: '\n' and '\0' are always word delimiters. */

		while (count < max_cmp && !Current_Delimiters[(int)*input_str] && !state.atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case WORD_CHAR: // \w (word character, alpha-numeric or underscore)
		while (count < max_cmp && (isalnum(*input_str) || *input_str == '_') &&
			   !state.atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case NOT_WORD_CHAR: // \W (NOT a word character)
		while (count < max_cmp && !isalnum(static_cast<int>(*input_str)) && *input_str != '_' &&
			   *input_str != '\n' && !state.atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case DIGIT: // same as [0123456789]
		while (count < max_cmp && isdigit(static_cast<int>(*input_str)) && !state.atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case NOT_DIGIT: // same as [^0123456789]
		while (count < max_cmp && !isdigit(static_cast<int>(*input_str)) && *input_str != '\n' &&
			   !state.atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case SPACE: // same as [ \t\r\f\v]-- doesn't match newline.
		while (count < max_cmp && isspace(static_cast<int>(*input_str)) && *input_str != '\n' &&
			   !state.atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case SPACE_NL: // same as [\n \t\r\f\v]-- matches newline.
		while (count < max_cmp && isspace(static_cast<int>(*input_str)) && !state.atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case NOT_SPACE: // same as [^\n \t\r\f\v]-- doesn't match newline.
		while (count < max_cmp && !isspace(static_cast<int>(*input_str)) && !state.atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case NOT_SPACE_NL: // same as [^ \t\r\f\v]-- matches newline.
		while (count < max_cmp && (!isspace(static_cast<int>(*input_str)) || *input_str == '\n') &&
			   !state.atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case LETTER: // same as [a-zA-Z]
		while (count < max_cmp && isalpha(static_cast<int>(*input_str)) && !state.atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case NOT_LETTER: // same as [^a-zA-Z]
		while (count < max_cmp && !isalpha(static_cast<int>(*input_str)) && *input_str != '\n' &&
			   !state.atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	default:
		/* Called inappropriately.  Only atoms that are SIMPLE should
		    generate a call to greedy.  The above cases should cover
		    all the atoms that are SIMPLE. */

		fprintf(stderr, "internal error #10 'greedy'");
		count = 0U; // Best we can do.
	}

	// Point to character just after last matched character.

	state.Reg_Input = input_str;

	return count;
}

/*
**  SubstituteRE - Perform substitutions after a `regexp' match.
**
**  This function cleanly shortens results of more than max length to max.
**  To give the caller a chance to react to this the function returns False
**  on any error. The substitution will still be executed.
*/
bool RegExp::SubstituteRE(const char *source, char *dest, const int max) {

	const uint8_t *src;
	const uint8_t *src_alias;
	uint8_t *dst;
	uint8_t c;
	uint8_t test;
	int paren_no;
	uint8_t chgcase;
	bool anyWarnings = false;

	assert(source);
	assert(dest);

	if (U_CHAR_AT(program_) != MAGIC) {
		fprintf(stderr, "damaged regexp passed to 'SubstituteRE'");
		return false;
	}

	src = (uint8_t *)source;
	dst = (uint8_t *)dest;

	while ((c = *src++) != '\0') {
		chgcase = '\0';
		paren_no = -1;

		if (c == '\\') {
			// Process any case altering tokens, i.e \u, \U, \l, \L.

			if (*src == 'u' || *src == 'U' || *src == 'l' || *src == 'L') {
				chgcase = *src;
				src++;
				c = *src++;

				if (c == '\0')
					break;
			}
		}

		if (c == '&') {
			paren_no = 0;
		} else if (c == '\\') {
			/* Can not pass register variable '&src' to function `numeric_escape'
			so make a non-register copy that we can take the address of. */

			src_alias = src;

			if ('1' <= *src && *src <= '9') {
				paren_no = (int)*src++ - (int)'0';

			} else if ((test = literal_escape(*src)) != '\0') {
				c = test;
				src++;

			} else if ((test = numeric_escape(*src, &src_alias)) != '\0') {
				c = test;
				src = src_alias;
				src++;

				/* NOTE: if an octal escape for zero is attempted (e.g. \000), it
		   will be treated as a literal string. */
			} else if (*src == '\0') {
				/* If '\' is the last character of the replacement string, it is
		   interpreted as a literal backslash. */

				c = '\\';
			} else {
				c = *src++; // Allow any escape sequence (This is
			}               // INCONSISTENT with the `CompileRE'
		}                   // mind set of issuing an error!

		if (paren_no < 0) { // Ordinary character.
			if (((char *)dst - dest) >= (max - 1)) {
				fprintf(stderr, "replacing expression in 'SubstituteRE'' too long; truncating");
				anyWarnings = true;
				break;
			} else {
				*dst++ = c;
			}
		} else if (startp_[paren_no] != nullptr && endp_[paren_no] != nullptr) {

			size_t len = endp_[paren_no] - startp_[paren_no];

			if (((char *)dst + len - dest) >= max - 1) {
				fprintf(stderr, "replacing expression in 'SubstituteRE' too long; truncating");
				anyWarnings = true;
				len = max - ((char *)dst - dest) - 1;
			}

			strncpy((char *)dst, startp_[paren_no], len);

			if (chgcase != '\0')
				adjustcase(dst, len, chgcase);

			dst += len;

			if (len != 0 && *(dst - 1) == '\0') { // strncpy hit NUL.
				fprintf(stderr, "damaged match string in 'SubstituteRE'");
				anyWarnings = true;
			}
		}
	}

	*dst = '\0';

	return !anyWarnings;
}

void RegExp::adjustcase(uint8_t *str, size_t len, uint8_t chgcase) {

	uint8_t *string = str;

	/* The tokens \u and \l only modify the first character while the tokens
	  \U and \L modify the entire string. */

	if (islower(chgcase) && len > 0)
		len = 1;

	switch (chgcase) {
	case 'u':
	case 'U':
		for (size_t i = 0; i < len; i++) {
			string[i] = toupper((int)string[i]);
		}

		break;

	case 'l':
	case 'L':
		for (size_t i = 0; i < len; i++) {
			string[i] = tolower((int)string[i]);
		}

		break;
	}
}

/*----------------------------------------------------------------------*
 * makeDelimiterTable
 *
 * Translate a null-terminated string of delimiters into a 256 byte
 * lookup table for determining whether a character is a delimiter or
 * not.
 *
 * Table must be allocated by the caller.
 *
 * Return value is a pointer to the table.
 *----------------------------------------------------------------------*/
uint8_t *RegExp::makeDelimiterTable(const uint8_t *delimiters, uint8_t *table) {

	memset(table, 0, 256);

	for (const uint8_t *c = delimiters; *c != '\0'; c++) {
		table[*c] = 1;
	}

	table[(int)'\0'] = 1; // These
	table[(int)'\t'] = 1; // characters
	table[(int)'\n'] = 1; // are always
	table[(int)' '] = 1;  // delimiters.

	return table;
}

/*----------------------------------------------------------------------*
 * SetREDefaultWordDelimiters
 *
 * Builds a default delimiter table that persists across `ExecRE' calls.
 *----------------------------------------------------------------------*/
void RegExp::SetREDefaultWordDelimiters(const char *delimiters) {
	makeDelimiterTable((const uint8_t *)delimiters, Default_Delimiters);
}

/*----------------------------------------------------------------------*
 * next_ptr - compute the address of a node's "NEXT" pointer.
 * Note: a simplified inline version is available via the NEXT_PTR() macro,
 *       but that one is only to be used at time-critical places (see the
 *       description of the macro).
 *----------------------------------------------------------------------*/
uint8_t *RegExp::next_ptr(uint8_t *ptr) {

	if (ptr == &Compute_Size) {
		return nullptr;
	}

	int offset = GET_OFFSET(ptr);

	if (offset == 0) {
		return nullptr;
	}

	if (GET_OP_CODE(ptr) == BACK) {
		return (ptr - offset);
	} else {
		return (ptr + offset);
	}
}

/*----------------------------------------------------------------------*
 * attempt - try match at specific point, returns: false failure, true success
 *----------------------------------------------------------------------*/
bool RegExp::attempt(const char *string, ExecState &state) {

	int branch_index = 0; // Must be set to zero !

	state.Reg_Input     = string;
	state.Start_Ptr_Ptr = startp_;
	state.End_Ptr_Ptr   = endp_;
	const char **s_ptr  = startp_;
	const char **e_ptr  = endp_;

	// Reset the recursion counter.
	Recursion_Count = 0;

	// Overhead due to capturing parentheses.

	state.Extent_Ptr_BW = string;
	state.Extent_Ptr_FW = nullptr;

	for (int i = Total_Paren + 1; i > 0; i--) {
		*s_ptr++ = nullptr;
		*e_ptr++ = nullptr;
	}

	if (match(program_ + REGEX_START_OFFSET, &branch_index, state)) {
		startp_[0]  = string;
		endp_[0]    = state.Reg_Input;       // <-- One char AFTER
		extentpBW_  = state.Extent_Ptr_BW; //     matched string!
		extentpFW_  = state.Extent_Ptr_FW;
		top_branch_ = branch_index;

		return true;
	} else {
		return false;
	}
}
