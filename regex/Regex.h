
#ifndef REGEX_H_
#define REGEX_H_

#include <stdexcept>
#include <string>
#include <climits>
#include <cstdint>
#include <cstddef>
#include <QString>
#include "Types.h"

enum class Direction {
	Backward, Forward
};

typedef uint16_t prog_type;

class RegexException : public std::exception {
public:
	explicit RegexException(const char *format, ...) {
		va_list ap;
		va_start(ap, format);
		vsnprintf(error_, sizeof(error_), format, ap);
		va_end(ap);
	}

	const char *what() const noexcept {
		return error_;
	}

private:
	char error_[255];
};

struct len_range {
	long lower;
	long upper;
};

/* Number of text capturing parentheses allowed. */
#define NSUBEXP 50

/* Structure to contain the compiled form of a regular expression plus
   pointers to matched text.  'program' is the actual compiled regex code. */

// Flags for CompileRE default settings (Markus Schwarzenberg)
enum RE_DEFAULT_FLAG {
	REDFLT_STANDARD = 0,
	REDFLT_CASE_INSENSITIVE = 1
	/* REDFLT_MATCH_NEWLINE = 2    Currently not used. */
};

struct CompileState;
struct ExecState;

struct Capture {
	const char *start;
	const char *end;
};

class Regex {
public:
	/**
	 * @brief Compiles a regular expression into the internal format used by 'ExecRE'.
	 * @param exp - String containing the regex specification.
	 * @param defaultFlags - Flags for default RE-operation
	 * @return
	 */
	Regex(const char *exp, int defaultFlags);
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
	int ExecRE(const char *string, const char *end, Direction direction, char prev_char, char succ_char,
	           const char *delimiters, const char *look_behind_to, const char *match_till);

	/**
	 * @brief ExecRE - Match a 'regexp' structure against a string.
	 * @param string - Text to search within.
	 * @param end -  Pointer to the end of 'string'.  If NULL will scan from 'string' until '\0' is found.
	 * @param reverse - Backward search.
	 * @param delimiters - Word delimiters to use (NULL for default)
	 * @param look_behind_to - Boundary for look-behind; defaults to "string" if NULL
	 * @param match_till - Boundary to where match can extend. \0 is assumed to be the boundary if not set. Lookahead can cross the boundary.
	 * @return
	 */
	int ExecRE(const char *string, const char *end, Direction direction, const char *delimiters, const char *look_behind_to, const char *match_till);

	/**
	 * @brief SubstituteRE - Perform substitutions after a 'regexp' match.
	 * @param prog
	 * @param source
	 * @param dest
	 * @param max
	 * @return
	 */
	bool SubstituteRE(const char *source, char *dest, const int max);

private:
	// for ExecRE
	int match(prog_type *prog, int *branch_index_param, ExecState &state);
	bool attempt(const char *string, ExecState &state);
	unsigned long greedy(prog_type *p, long max, ExecState &state) const;

private:
	// for CompileRE
	prog_type *alternative(int *flag_param, len_range *range_param, CompileState &cState);
	prog_type *atom(int *flag_param, len_range *range_param, CompileState &cState);
	prog_type *back_ref(const char *c, int *flag_param, int emitType, CompileState &cState);
	prog_type *chunk(int paren, int *flag_param, len_range *range_param, CompileState &cState);
	prog_type *emit_node(prog_type op_code, CompileState &cState);
	prog_type *emit_special(prog_type op_code, unsigned long test_val, int index, CompileState &cState);
	prog_type *piece(int *flag_param, len_range *range_param, CompileState &cState);
	prog_type *shortcut_escape(char c, int *flag_param, int emitType, CompileState &cState);
	prog_type *insert(prog_type op, prog_type *opnd, long min, long max, int index, CompileState &cState);

public:
	/* Builds a default delimiter table that persists across 'ExecRE' calls that
	   is identical to 'delimiters'.  Pass NULL for "default default" set of
	   delimiters. */
	static void SetDefaultWordDelimiters(const char *delimiters);

public:
	int top_branch() const {
		return top_branch_;
	}

	Capture capture(int index) const {
		Capture cap;
		cap.start = startp_[index];
		cap.end   = endp_[index];
		return cap;
	}



private:
	int Recursion_Count;           /* Recursion counter */
	bool Recursion_Limit_Exceeded; /* Recursion limit exceeded flag */
	bool *Current_Delimiters; /* Current delimiter table */

	const char *startp_[NSUBEXP]; // Captured text starting locations.
	const char *endp_[NSUBEXP];   // Captured text ending locations.
	const char *extentpBW_;  // Points to the maximum extent of text scanned by ExecRE in front of the string to achieve a
	                   // match (needed because of positive look-behind.)
	const char *extentpFW_;  // Points to the maximum extent of text scanned by ExecRE to achieve a match (needed because of
	                   // positive look-ahead.)
	int top_branch_;   // Zero-based index of the top branch that matches. Used by syntax highlighting only.
	prog_type match_start_; // Internal use only.
	char anchor_;      // Internal use only.
	prog_type *program_;
	size_t Total_Paren; // Parentheses, (),  counter.
	size_t Num_Braces;  // Number of general {m,n} constructs. {m,n} quantifiers of SIMPLE atoms are not included in this
	                 // count.

	QString regex_;
};


#endif
