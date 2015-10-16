
#ifndef REGEX_MATCH_H_
#define REGEX_MATCH_H_

#include "Types.h"
#include "RegexCommon.h"

enum class Direction {
	Backward, Forward
};

struct Capture {
	const char *start;
	const char *end;
};

class Regex;

class RegexMatch {
	friend class Regex;
public:
	static const int MaxBackRefs = 10;

public:
	explicit RegexMatch(Regex *regex);
	~RegexMatch();
	
private:
	RegexMatch(const RegexMatch &) = delete;
	RegexMatch& operator=(const RegexMatch &) = delete;

private:
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
	bool ExecRE(const char *string, const char *end, Direction direction, char prev_char, char succ_char,
	           const char *delimiters, const char *look_behind_to, const char *match_till);

public:	   
	/**
	 * @brief SubstituteRE - Perform substitutions after a 'regexp' match.
	 * @param source
	 * @param dest
	 * @param max
	 * @return
	 */
	bool SubstituteRE(const char *source, char *dest, const int max);			   

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
	int match(prog_type *prog, int *branch_index_param);
	bool attempt(const char *string);
	unsigned long greedy(prog_type *p, long max);
	bool atEndOfString(const char *p) const;

private:
	const Regex *const regex_;

private:
	const char *input;                       // String-input pointer.
	const char *startOfString;               // Beginning of input, for ^ and < checks.
	const char *endOfString;                 // Logical end of input (if supplied, till \0 otherwise)
	const char *lookBehindTo;                // Position till were look behind can safely check back
	const char **Start_Ptr_Ptr;              // Pointer to 'startp' array.
	const char **End_Ptr_Ptr;                // Ditto for 'endp'.
	const char *Extent_Ptr_FW;               // Forward extent pointer
	const char *Extent_Ptr_BW;               // Backward extent pointer
	const char *Back_Ref_Start[MaxBackRefs]; // Back_Ref_Start [0] and
	const char *Back_Ref_End[MaxBackRefs];   // Back_Ref_End [0] are not used. This simplifies indexing.

	bool prevIsBOL;
	bool succIsEOL;
	bool prevIsDelim;
	bool succIsDelim;

	uint32_t *brace_counts_;
	int       recursion_count_;        // Recursion counter

	const char *    startp_[NSUBEXP]; // Captured text starting locations.
	const char *    endp_[NSUBEXP];   // Captured text ending locations.

	const char *    extentpBW_;       // Points to the maximum extent of text scanned by ExecRE in front of the string to achieve a
	                                  // match (needed because of positive look-behind.)
	const char *    extentpFW_;       // Points to the maximum extent of text scanned by ExecRE to achieve a match (needed because of
	                                  // positive look-ahead.)

	int             top_branch_;      // Zero-based index of the top branch that matches. Used by syntax highlighting only.

	bool            Recursion_Limit_Exceeded; // Recursion limit exceeded flag
	bool *          Current_Delimiters;       // Current delimiter table
	
	size_t          Total_Paren; // Parentheses, (),  counter.
	size_t          Num_Braces;  // Number of general {m,n} constructs. {m,n} quantifiers of SIMPLE atoms are not included in this
	                             // count.	
};

#endif
