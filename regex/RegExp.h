
#ifndef REGULAR_EXP_H_
#define REGULAR_EXP_H_

#include <stdexcept>
#include <string>
#include <climits>
#include <cstdint>
#include <cstddef>

/* Number of text capturing parentheses allowed. */

#define NSUBEXP 50

/* Array sizes for arrays used by function init_ansi_classes. */

#define WHITE_SPACE_SIZE 16
#define ALNUM_CHAR_SIZE 256

/* Structure to contain the compiled form of a regular expression plus
   pointers to matched text.  'program' is the actual compiled regex code. */

class RegexException : public std::runtime_error {
public:
	RegexException(const std::string &str) : std::runtime_error(str) {
	}
};

struct len_range {
	long lower;
	long upper;
};

// Flags for CompileRE default settings (Markus Schwarzenberg)
enum RE_DEFAULT_FLAG {
	REDFLT_STANDARD = 0,
	REDFLT_CASE_INSENSITIVE = 1
	/* REDFLT_MATCH_NEWLINE = 2    Currently not used. */
};

struct CompileState;
struct ExecState;

class RegExp {
public:
	/**
	 * @brief Compiles a regular expression into the internal format used by
	 * 'ExecRE'.
	 * @param exp - String containing the regex specification.
	 * @param defaultFlags - Flags for default RE-operation
	 * @return
	 */
    RegExp(const char *exp, int defaultFlags);
	RegExp(const RegExp &) = delete;
	RegExp &operator=(const RegExp &) = delete;

public:
	/**
	 * @brief ExecRE - Match a 'regexp' structure against a string.
	 * @param string - Text to search within.
	 * @param end -  Pointer to the end of 'string'.  If NULL will scan from
	 * 'string' until '\0' is found.
	 * @param reverse - Backward search.
	 * @param prev_char - Character immediately prior to 'string'.  Set to '\n' or
	 * '\0' if true beginning of text.
	 * @param succ_char - Character immediately after 'end'.  Set to '\n' or '\0'
	 * if true beginning of text.
	 * @param delimiters - Word delimiters to use (NULL for default)
	 * @param look_behind_to - Boundary for look-behind; defaults to "string" if
	 * NULL
	 * @param match_till - Boundary to where match can extend. \0 is assumed to be
	 * the boundary if not set. Lookahead
	 * can
	 * cross the boundary.
	 * @returne
	 */
	int ExecRE(const char *string, const char *end, bool reverse, char prev_char, char succ_char,
	           const char *delimiters, const char *look_behind_to, const char *match_till);

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
	int match(uint8_t *prog, int *branch_index_param, ExecState &state);
	bool attempt(const char *string, ExecState &state);
	unsigned long greedy(uint8_t *p, long max, ExecState &state);

private:
	// for CompileRE
	uint8_t *alternative(int *flag_param, len_range *range_param, CompileState &cState);
	uint8_t *atom(int *flag_param, len_range *range_param, CompileState &cState);
	uint8_t *back_ref(const uint8_t *c, int *flag_param, int emitType, CompileState &cState);
	uint8_t *chunk(int paren, int *flag_param, len_range *range_param, CompileState &cState);
	uint8_t *emit_node(uint8_t op_code, CompileState &cState);
	uint8_t *emit_special(uint8_t op_code, unsigned long test_val, int index, CompileState &cState);
	uint8_t *piece(int *flag_param, len_range *range_param, CompileState &cState);
	uint8_t *shortcut_escape(uint8_t c, int *flag_param, int emitType, CompileState &cState);
	void emit_byte(uint8_t c, CompileState &cState);
	void emit_class_byte(uint8_t c, CompileState &cState);
	uint8_t *insert(uint8_t op, uint8_t *opnd, long min, long max, int index, CompileState &cState);

private:
	bool init_ansi_classes();
	uint8_t *next_ptr(uint8_t *ptr);
	uint8_t literal_escape(uint8_t c);
	uint8_t numeric_escape(uint8_t c, const uint8_t **parse);
	void adjustcase(uint8_t *str, size_t len, uint8_t chgcase);
	void branch_tail(uint8_t *ptr, int offset, uint8_t *val);
	void offset_tail(uint8_t *ptr, int offset, uint8_t *val);
	void tail(uint8_t *search_from, uint8_t *point_t);

private:
	static uint8_t *makeDelimiterTable(const uint8_t *delimiters, uint8_t *table);

public:
	/* Builds a default delimiter table that persists across 'ExecRE' calls that
	   is identical to 'delimiters'.  Pass NULL for "default default" set of
	   delimiters. */
	static void SetREDefaultWordDelimiters(const char *delimiters);

private:
	char White_Space[WHITE_SPACE_SIZE]; /* Arrays used by       */
	char Word_Char[ALNUM_CHAR_SIZE];    /* functions            */
	char Letter_Char[ALNUM_CHAR_SIZE];  /* init_ansi_classes () and shortcut_escape ().  */
	uint8_t Compute_Size;                  /* Address of this used as flag. */

private:
	int Recursion_Count;           /* Recursion counter */
	bool Recursion_Limit_Exceeded; /* Recursion limit exceeded flag */

private:
	/* Default table for determining whether a character is a word delimiter. */
	static uint8_t Default_Delimiters[UCHAR_MAX + 1];
	uint8_t *Current_Delimiters; /* Current delimiter table */

public:
	int top_branch() const {
		return top_branch_;
	}

	const char *endp(int index) const {
		return endp_[index];
	}

	const char *startp(int index) const {
		return startp_[index];
	}

private:
	const char *startp_[NSUBEXP]; // Captured text starting locations.
	const char *endp_[NSUBEXP];   // Captured text ending locations.
	const char *extentpBW_;  // Points to the maximum extent of text scanned by ExecRE in front of the string to achieve a
	                   // match (needed because of positive look-behind.)
	const char *extentpFW_;  // Points to the maximum extent of text scanned by ExecRE to achieve a match (needed because of
	                   // positive look-ahead.)
	int top_branch_;   // Zero-based index of the top branch that matches. Used by syntax highlighting only.
	char match_start_; // Internal use only.
	char anchor_;      // Internal use only.
	uint8_t *program_;
	int Total_Paren; // Parentheses, (),  counter.
	int Num_Braces;  // Number of general {m,n} constructs. {m,n} quantifiers of SIMPLE atoms are not included in this
	                 // count.
};


#endif
