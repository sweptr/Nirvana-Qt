
#ifndef REGEX_H_
#define REGEX_H_

#include <string>
#include <climits>
#include <cstdint>
#include <cstddef>
#include <bitset>
#include <QString>
#include "Types.h"
#include "RegexMatch.h"
#include "RegexException.h"


class len_range;

/* Structure to contain the compiled form of a regular expression plus
   pointers to matched text.  'program' is the actual compiled regex code. */

// Flags for CompileRE default settings (Markus Schwarzenberg)
enum RE_DEFAULT_FLAG {
	REDFLT_STANDARD = 0,
	REDFLT_CASE_INSENSITIVE = 1
	/* REDFLT_MATCH_NEWLINE = 2    Currently not used. */
};

// Flags for function shortcut_escape()
enum class EscapeFlags {
	CHECK_ESCAPE       = 0, // Check an escape sequence for validity only.
	CHECK_CLASS_ESCAPE = 1, // Check the validity of an escape within a character class
	EMIT_CLASS_BYTES   = 2, // Emit equivalent character class bytes, e.g \d=0123456789
	EMIT_NODE          = 3, // Emit the appropriate node.
};

class Regex {
	friend class RegexMatch;
public:
	/**
	 * @brief Compiles a regular expression into the internal format used by 'ExecRE'.
	 * @param exp - String containing the regex specification.
	 * @param defaultFlags - Flags for default RE-operation
	 * @return
	 */
	Regex(const char *exp, int defaultFlags);
	
private:
	Regex(const Regex &) = delete;
	Regex &operator=(const Regex &) = delete;

public:
	/**
	 * @brief ExecRE - Match a 'regexp' structure against a string.
	 * @param string - Text to search within.
	 * @param end -  Pointer to the end of 'string'.  If NULL will scan from 'string' until '\0' is found.
	 * @param reverse - Backward search.
	 * @param prev_char - Character immediately prior to 'string'.  Set to '\n' or '\0' if true beginning of text.
	 * @param succ_char - Character immediately after 'end'.  Set to '\n' or '\0' if true beginning of text.
	 * @param delimiters - Word delimiters to use (NULL for default)
	 * @param look_behind_to - Boundary for look-behind; defaults to "string" if NULL
	 * @param match_till - Boundary to where match can extend. \0 is assumed to be the boundary if not set. Lookahead can cross the boundary.
	 * @return
	 */
	RegexMatch* ExecRE(const char *string, const char *end, Direction direction, char prev_char, char succ_char,
	           const char *delimiters, const char *look_behind_to, const char *match_till);

private:
	// for CompileRE
	prog_type *alternative(int *flag_param, len_range *range_param);
	prog_type *atom(int *flag_param, len_range *range_param);
	prog_type *back_ref(const char *c, int *flag_param, EscapeFlags emitType);
	prog_type *chunk(int paren, int *flag_param, len_range *range_param);
	prog_type *emit_node(prog_type op_code);
	prog_type *emit_special(prog_type op_code, unsigned long test_val, int index);
	prog_type *piece(int *flag_param, len_range *range_param);
	prog_type *shortcut_escape(char c, int *flag_param, EscapeFlags emitType);
	prog_type *insert(prog_type op, prog_type *opnd, long min, long max, int index);
	void emit_byte(prog_type c);
	void emit_class_byte(prog_type c);
	bool isQuantifier(prog_type c) const;

public:
	/* Builds a default delimiter table that persists across 'ExecRE' calls that
	   is identical to 'delimiters'.  Pass NULL for "default default" set of
	   delimiters. */
	static void SetDefaultWordDelimiters(const char *delimiters);

private:
	prog_type       match_start_;     // Internal use only.
	char            anchor_;          // Internal use only.
	prog_type *     program_;
	size_t          Total_Paren; // Parentheses, (),  counter.
	size_t          Num_Braces;  // Number of general {m,n} constructs. {m,n} quantifiers of SIMPLE atoms are not included in this
	                             // count.

	QString         regex_;
	const char *    Reg_Parse;       // Input scan ptr (scans user's regex)
	std::bitset<32> Closed_Parens;   // Bit flags indicating () closure.
	std::bitset<32> Paren_Has_Width; // Bit flags indicating ()'s that are known to not match the empty string
	
	prog_type *     Code_Emit_Ptr; // When Code_Emit_Ptr is set to &Compute_Size no code is emitted. Instead, the size of
							       // code that WOULD have been generated is accumulated in Reg_Size.  Otherwise,
							       // Code_Emit_Ptr points to where compiled regex code is to be written.

	size_t          Reg_Size;      // Size of compiled regex code.
	bool            Is_Case_Insensitive;
	bool            Match_Newline;
	char            Brace_Char;
	const char *    Meta_Char;	
};

#endif
