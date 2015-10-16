
#ifndef REGEX_OPCODES_H_
#define REGEX_OPCODES_H_

#include "RegexCommon.h"

enum RegexOpcodes : uint8_t {
	/* STRUCTURE FOR A REGULAR EXPRESSION (regex) 'PROGRAM'.
     *
     * This is essentially a linear encoding of a nondeterministic finite-state
     * machine or NFA (aka syntax charts or 'railroad normal form' in parsing
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

#endif
