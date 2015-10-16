
#include "RegexMatch.h"
#include "RegexOpcodes.h"
#include "RegexCommon.h"
#include "Regex.h"
#include <QtDebug>
#include <cassert>

#define MATCH_RETURN(X)           \
	{                             \
		--recursion_count_;        \
		return (X);               \
	}
	
#define CHECK_RECURSION_LIMIT     \
	if (Recursion_Limit_Exceeded) \
		MATCH_RETURN(false);

/* The next_ptr () function can consume up to 30% of the time during matching
   because it is called an immense number of times (an average of 25
   next_ptr() calls per match() call was witnessed for Perl syntax
   highlighting). Therefore it is well worth removing some of the function
   call overhead by selectively inlining the next_ptr() calls. Moreover,
   the inlined code can be simplified for matching because one of the tests,
   only necessary during compilation, can be left out.
   The net result of using this inlined version at two critical places is
   a 25% speedup (again, witnesses on Perl syntax highlighting). */
#define NEXT_PTR(in_ptr, out_ptr)                         \
	do {                                                  \
		const size_t next_ptr_offset = getOffset(in_ptr); \
		if (next_ptr_offset == 0) {                       \
			out_ptr = nullptr;                            \
		} else {                                          \
			if (getOpcode(in_ptr) == BACK) {              \
				out_ptr = in_ptr - next_ptr_offset;       \
			} else {                                      \
				out_ptr = in_ptr + next_ptr_offset;       \
			}                                             \
		}                                                 \
	} while(0)


namespace {


/*
 * Measured recursion limits:
 *    Linux:      +/-  40 000 (up to 110 000)
 *    Solaris:    +/-  85 000
 *    HP-UX 11:   +/- 325 000
 *
 * So 10 000 ought to be safe.
 */
const int RegexRecursionLimit = 10000;

//------------------------------------------------------------------------------
// Name: get_lower
//------------------------------------------------------------------------------
int get_lower(prog_type *p) {
	return ((p[Regex::NodeSize] & 0xff) << 8) + (p[Regex::NodeSize + 1] & 0xff);
}

//------------------------------------------------------------------------------
// Name: get_upper
//------------------------------------------------------------------------------
int get_upper(prog_type *p) {
	return ((p[Regex::NodeSize + 2] & 0xff) << 8) + (p[Regex::NodeSize + 3] & 0xff);
}

//------------------------------------------------------------------------------
// Name: string_length
//------------------------------------------------------------------------------
size_t string_length(const prog_type *s) {
	const prog_type *const s_ptr = s;

	assert(s);

	while(*s != '\0') {
		++s;
	}

	return s - s_ptr;
}

//------------------------------------------------------------------------------
// Name: string_compare
//------------------------------------------------------------------------------
int string_compare(const prog_type *s1, const char *s2, size_t n) {
	int ret = 0;

	assert(s1);
	assert(s2);

	while(!ret && (*s1 || *s2) && n--) {
		ret = (*s1++ - *s2++);
	}

	return ret;
}

//------------------------------------------------------------------------------
// Name: find_character
//------------------------------------------------------------------------------
const prog_type *find_character(const prog_type *s, char c) {
	const prog_type cmp = c;

	assert(s);

	while(*s != '\0') {
		if(*s == cmp) {
			return s;
		}
		++s;
	}
	return nullptr;
}

//------------------------------------------------------------------------------
// Name: makeDelimiterTable
// Desc: Translate a null-terminated string of delimiters into a 256 byte
//       lookup table for determining whether a character is a delimiter or
//       not.
//       
//       Table must be allocated by the caller.
//       
//       Return value is a pointer to the table.
//------------------------------------------------------------------------------
bool *makeDelimiterTable(const char *delimiters, bool *table) {

	std::fill_n(table, 256, false);

	for (const char *c = delimiters; *c != '\0'; c++) {
		table[static_cast<int>(*c)] = true;
	}

	table[static_cast<int>('\0')] = true; // These
	table[static_cast<int>('\t')] = true; // characters
	table[static_cast<int>('\n')] = true; // are always
	table[static_cast<int>(' ')]  = true; // delimiters.

	return table;
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
char literal_escape(char c) {

	static const char valid_escape[] = {
		'a', 'b', 'e', 'f', 'n',  'r', 't', 'v', '(', ')', '-', '[',  ']', '<',
		'>', '{', '}', '.', '\\', '|', '^', '$', '*', '+', '?', '&', '\0'
	};

	static const char value[] = {'\a', '\b',
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

//------------------------------------------------------------------------------
// Name: numeric_escape
// Desc: Implements hex and octal numeric escape sequence syntax.
//       
//       Hexadecimal Escape: \x##	Max of two digits  Must have leading 'x'.
//       Octal Escape:		\0###	Max of three digits and not greater
//       							than 377 octal.  Must have leading zero.
//       
//       Returns the actual character value or NULL if not a valid hex or
//       octal escape.  REG_FAIL is called if \x0, \x00, \0, \00, \000, or
//       \0000 is specified.
//------------------------------------------------------------------------------
uint8_t numeric_escape(char c, const char **parse) {

	static const char digits[] = "fedcbaFEDCBA9876543210";

	static unsigned int digit_val[] = {15, 14, 13, 12, 11, 10,              // Lower case Hex digits
	                                   15, 14, 13, 12, 11, 10,              // Upper case Hex digits
	                                   9,  8,  7,  6,  5,  4,  3, 2, 1, 0}; // Decimal Digits

	const char *digit_str;
	unsigned int value = 0;
	unsigned int radix = 8;
	int width = 3; // Can not be bigger than \0xff
	int pos_delta = 14;

	switch (c) {
	case '0':
		digit_str = digits + pos_delta; // Only use Octal digits, i.e. 0-7.
		break;

	case 'x':
	case 'X':
		width     = 2;      // Can not be bigger than \0xff
		radix     = 16;
		pos_delta = 0;
		digit_str = digits; // Use all of the digit characters.

		break;

	default:
		return '\0'; // Not a numeric escape
	}

	const char *scan = *parse;
	scan++; // Only change *parse on success.

	const char *pos_ptr = strchr(digit_str, *scan);

	for (int i = 0; pos_ptr != nullptr && (i < width); i++) {
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
		pos_ptr = strchr(digit_str, *scan);
	}

	// Handle the case of "\0" i.e. trying to specify a NULL character.

	if (value == 0) {
		if (c == '0') {
			throw RegexException("\\00 is an invalid octal escape");
		} else {
			throw RegexException("\\%c0 is an invalid hexadecimal escape", c);
		}
	} else {
		// Point to the last character of the number on success.

		scan--;
		*parse = scan;
	}

	return static_cast<uint8_t>(value);
}

//------------------------------------------------------------------------------
// Name: adjust_case
//------------------------------------------------------------------------------
void adjust_case(char *str, size_t len, char chgcase) {

	char *string = str;

	/* The tokens \u and \l only modify the first character while the tokens
	  \U and \L modify the entire string. */

	if (islower(chgcase) && len > 0) {
		len = 1;
	}

	switch (chgcase) {
	case 'u':
	case 'U':
		for (size_t i = 0; i < len; i++) {
			string[i] = toupper(string[i]);
		}

		break;

	case 'l':
	case 'L':
		for (size_t i = 0; i < len; i++) {
			string[i] = tolower(string[i]);
		}

		break;
	}
}

}

//------------------------------------------------------------------------------
// Name: RegexMatch
//------------------------------------------------------------------------------
RegexMatch::RegexMatch(Regex *regex) : regex_(regex), recursion_count_(0), extentpBW_(nullptr), extentpFW_(nullptr), top_branch_(0), Recursion_Limit_Exceeded(false), Current_Delimiters(nullptr), Total_Paren(0), Num_Braces(0) {
	std::fill_n(startp_, NSUBEXP, nullptr);
	std::fill_n(endp_,   NSUBEXP, nullptr);
	
	// Check validity of program.
	if (regex_->program_[0] != Regex::MAGIC) {
		throw RegexException("corrupted program");
	}
	
	Total_Paren = regex_->program_[1];
	Num_Braces  = regex_->program_[2];

	// Allocate memory for {m,n} construct counting variables if need be.
	if (Num_Braces > 0) {
		brace_counts_ = new uint32_t[Num_Braces];
	} else {
		brace_counts_ = nullptr;
	}	
}

//------------------------------------------------------------------------------
// Name: ~RegexMatch
//------------------------------------------------------------------------------
RegexMatch::~RegexMatch() {
	delete [] brace_counts_;
}

//------------------------------------------------------------------------------
// Name:     ExecRE
// Synopsis: match a 'Regex' structure against a string
// Desc:     If 'end' is non-NULL, matches may not BEGIN past end, but may extend past
//           it.  If reverse is true, 'end' must be specified, and searching begins at
//           'end'.  "isbol" should be set to true if the beginning of the string is the
//           actual beginning of a line (since 'ExecRE' can't look backwards from the
//           beginning to find whether there was a newline before).  Likewise, "isbow"
//           asks whether the string is preceded by a word delimiter.  End of string is
//           always treated as a word and line boundary (there may be cases where it
//           shouldn't be, in which case, this should be changed).  "delimit" (if
//           non-null) specifies a null-terminated string of characters to be considered
//           word delimiters matching "<" and ">".  if "delimit" is NULL, the default
//           delimiters (as set in SetREDefaultWordDelimiters) are used.
//           Look_behind_to indicates the position till where it is safe to
//           perform look-behind matches. If set, it should be smaller than or equal
//           to the start position of the search (pointed at by string). If it is NULL,
//           it defaults to the start position.
//           Finally, match_to indicates the logical end of the string, till where
//           matches are allowed to extend. Note that look-ahead patterns may look
//           past that boundary. If match_to is set to NULL, the terminating \0 is
//           assumed to correspond to the logical boundary. Match_to, if set, must be
//           larger than or equal to end, if set.
//------------------------------------------------------------------------------
bool RegexMatch::ExecRE(const char *string, const char *end, Direction direction, char prev_char, char succ_char, const char *delimiters, const char *look_behind_to, const char *match_to) {

	bool ret_val = false;

	try {
		// Check for valid parameters.
		if (!string) {
			throw RegexException("NULL parameter to 'ExecRE'");
		}

		const char *str;		
		const char **s_ptr = startp_;
		const char **e_ptr = endp_;

		// If caller has supplied delimiters, make a delimiter table
		bool tempDelimitTable[256];
		if (!delimiters) {
			Current_Delimiters = Regex::DefaultDelimiters;
		} else {
			Current_Delimiters = makeDelimiterTable(delimiters, tempDelimitTable);
		}

		// Remember the logical end of the string.		
		endOfString = match_to;

		if (end == nullptr && direction == Direction::Backward) {
			for (end = string; !atEndOfString(end); end++) {
			}
			succ_char = '\n';
		} else if (end == nullptr) {
			succ_char = '\n';
		}

		// Remember the beginning of the string for matching BOL
		startOfString = string;
		lookBehindTo  = look_behind_to ? look_behind_to : string;
		prevIsBOL     = ((prev_char == '\n') || (prev_char == '\0'));
		succIsEOL     = ((succ_char == '\n') || (succ_char == '\0'));
		prevIsDelim   = Current_Delimiters[static_cast<int>(prev_char)];
		succIsDelim   = Current_Delimiters[static_cast<int>(succ_char)];


		/* Initialize the first nine (9) capturing parentheses start and end
		  pointers to point to the start of the search string.  This is to prevent
		  crashes when later trying to reference captured parens that do not exist
		  in the compiled regex.  We only need to do the first nine since users
		  can only specify \1, \2, ... \9. */
		for (int i = 9; i > 0; i--) {
			*s_ptr++ = string;
			*e_ptr++ = string;
		}
		
		switch(direction) {
		case Direction::Forward:
			if (regex_->anchor_) {
				// Search is anchored at BOL

				if (attempt(string)) {
					ret_val = true;
					goto SINGLE_RETURN;
				}

				for (str = string; !atEndOfString(str) && str != end && !Recursion_Limit_Exceeded; str++) {

					if (*str == '\n') {
						if (attempt(str + 1)) {
							ret_val = true;
							break;
						}
					}
				}

				goto SINGLE_RETURN;

			} else if (regex_->match_start_ != '\0') {
				// We know what char match must start with.

				for (str = string; !atEndOfString(str) && str != end && !Recursion_Limit_Exceeded; str++) {

					if (*str == regex_->match_start_) {
						if (attempt(str)) {
							ret_val = true;
							break;
						}
					}
				}

				goto SINGLE_RETURN;
			} else {
				// General case

				for (str = string; !atEndOfString(str) && str != end && !Recursion_Limit_Exceeded; str++) {

					if (attempt(str)) {
						ret_val = true;
						break;
					}
				}

				// Beware of a single $ matching \0
				if (!Recursion_Limit_Exceeded && !ret_val && atEndOfString(str) && str != end) {
					if (attempt(str)) {
						ret_val = true;
					}
				}

				goto SINGLE_RETURN;
			}
			break;		
		case Direction::Backward:
			// Make sure that we don't start matching beyond the logical end
			if (endOfString != nullptr && end > endOfString) {
				end = endOfString;
			}

			if (regex_->anchor_) {
				// Search is anchored at BOL

				for (str = (end - 1); str >= string && !Recursion_Limit_Exceeded; str--) {

					if (*str == '\n') {
						if (attempt(str + 1)) {
							ret_val = true;
							goto SINGLE_RETURN;
						}
					}
				}

				if (!Recursion_Limit_Exceeded && attempt(string)) {
					ret_val = true;
					goto SINGLE_RETURN;
				}

				goto SINGLE_RETURN;
			} else if (regex_->match_start_ != '\0') {
				// We know what char match must start with.

				for (str = end; str >= string && !Recursion_Limit_Exceeded; str--) {

					if (*str == regex_->match_start_) {
						if (attempt(str)) {
							ret_val = true;
							break;
						}
					}
				}

				goto SINGLE_RETURN;
			} else {
				// General case

				for (str = end; str >= string && !Recursion_Limit_Exceeded; str--) {

					if (attempt(str)) {
						ret_val = true;
						break;
					}
				}
			}		
			break;
		}

	SINGLE_RETURN:
		if (Recursion_Limit_Exceeded) {
			return false;
		}

	} catch(const RegexException &e) {
		qDebug("%s", e.what());
		return false;
	}

	return ret_val;
}

/*
**  SubstituteRE - Perform substitutions after a 'Regex' match.
**
**  This function cleanly shortens results of more than max length to max.
**  To give the caller a chance to react to this the function returns False
**  on any error. The substitution will still be executed.
*/
bool RegexMatch::SubstituteRE(const char *source, char *dest, const int max) {

	const char *src_alias;
	char c;
	char test;
	bool anyWarnings = false;

	assert(source);
	assert(dest);

	if (regex_->program_[0] != Regex::MAGIC) {
		qDebug("damaged Regex passed to 'SubstituteRE'");
		return false;
	}

	const char *src = source;
	char *dst = dest;

	while ((c = *src++) != '\0') {
		char chgcase = '\0';
		int paren_no = -1;

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
			/* Can not pass register variable '&src' to function 'numeric_escape'
			so make a non-register copy that we can take the address of. */

			src_alias = src;

			if ('1' <= *src && *src <= '9') {
				paren_no = static_cast<int>(*src++ - '0');

			} else if ((test = literal_escape(*src)) != '\0') {
				c = test;
				src++;

			} else if ((test = numeric_escape(*src, &src_alias)) != '\0') {
				c = test;
				src = src_alias;
				src++;

				/* NOTE: if an octal escape for zero is attempted (e.g. \000), it will be treated as a literal string. */
			} else if (*src == '\0') {
				/* If '\' is the last character of the replacement string, it is interpreted as a literal backslash. */

				c = '\\';
			} else {
				c = *src++; // Allow any escape sequence (This is
			}               // INCONSISTENT with the 'CompileRE'
		}                   // mind set of issuing an error!

		if (paren_no < 0) { // Ordinary character.
			if ((dst - dest) >= (max - 1)) {
				qDebug("replacing expression in 'SubstituteRE'' too long; truncating");
				anyWarnings = true;
				break;
			} else {
				*dst++ = c;
			}
		} else if (startp_[paren_no] != nullptr && endp_[paren_no] != nullptr) {

			size_t len = endp_[paren_no] - startp_[paren_no];

			if ((dst + len - dest) >= max - 1) {
				qDebug("replacing expression in 'SubstituteRE' too long; truncating");
				anyWarnings = true;
				len = max - (dst - dest) - 1;
			}

			strncpy(dst, startp_[paren_no], len);

			if (chgcase != '\0')
				adjust_case(dst, len, chgcase);

			dst += len;

			if (len != 0 && *(dst - 1) == '\0') { // strncpy hit NUL.
				qDebug("damaged match string in 'SubstituteRE'");
				anyWarnings = true;
			}
		}
	}

	*dst = '\0';

	return !anyWarnings;
}

//------------------------------------------------------------------------------
// Name: attempt
// Desc: try match at specific point, returns: false failure, true success
//------------------------------------------------------------------------------
bool RegexMatch::attempt(const char *string) {

	int branch_index = 0; // Must be set to zero !

	input               = string;
	Start_Ptr_Ptr       = startp_;
	End_Ptr_Ptr         = endp_;
	const char **s_ptr  = startp_;
	const char **e_ptr  = endp_;

	// Reset the recursion counter.
	recursion_count_ = 0;

	// Overhead due to capturing parentheses.
	Extent_Ptr_BW = string;
	Extent_Ptr_FW = nullptr;

	for (int i = Total_Paren + 1; i > 0; i--) {
		*s_ptr++ = nullptr;
		*e_ptr++ = nullptr;
	}

	if (match(regex_->program_ + Regex::RegexStartOffset, &branch_index)) {
		startp_[0]  = string;
		endp_[0]    = input;         // <-- One char AFTER
		extentpBW_  = Extent_Ptr_BW; //     matched string!
		extentpFW_  = Extent_Ptr_FW;
		top_branch_ = branch_index;

		return true;
	} else {
		return false;
	}
}

//------------------------------------------------------------------------------
// Name: match
// Desc: Conceptually the strategy is simple: check to see whether the
//       current node matches, call self recursively to see whether the rest
//       matches, and then act accordingly.  In practice we make some effort
//       to avoid recursion, in particular by going through "ordinary" nodes
//       (that don't need to know whether the rest of the match failed) by a
//       loop instead of by recursion.  Returns 0 failure, 1 success.
//------------------------------------------------------------------------------
int RegexMatch::match(prog_type *prog, int *branch_index_param) {

	prog_type *next;       // Next node.

	if (++recursion_count_ > RegexRecursionLimit) {
		if (!Recursion_Limit_Exceeded) { // Prevent duplicate errors
			qDebug("recursion limit exceeded, please respecify expression");
		}
		Recursion_Limit_Exceeded = true;
		MATCH_RETURN(0);
	}

	// Current node.
	prog_type *scan = prog;

	while (scan != nullptr) {
		NEXT_PTR(scan, next);

		switch (getOpcode(scan)) {
		case BRANCH: {	

			if (getOpcode(next) != BRANCH) { // No choice.
				next = getOperand(scan);          // Avoid recursion.
			} else {
			
				int branch_index_local = 0;
				
				do {
					const char *save = input;

					if (match(getOperand(scan), nullptr)) {
						if (branch_index_param) {
							*branch_index_param = branch_index_local;
						}
						MATCH_RETURN(1);
					}

					CHECK_RECURSION_LIMIT

					++branch_index_local;

					input = save; // Backtrack.
					NEXT_PTR(scan, scan);
				} while (scan != nullptr && getOpcode(scan) == BRANCH);

				MATCH_RETURN(0); // NOT REACHED
			}
		}

		break;

		case EXACTLY: {
			prog_type *opnd = getOperand(scan);

			// Inline the first character, for speed.

			if (*opnd != *input)
				MATCH_RETURN(0);

			size_t len = string_length(opnd);

			if (endOfString != nullptr && input + len > endOfString) {
				MATCH_RETURN(0);
			}

			if (len > 1 && string_compare(opnd, input, len) != 0) {

				MATCH_RETURN(0);
			}

			input += len;
		}

		break;

		case SIMILAR: {
			prog_type test;

			prog_type *opnd = getOperand(scan);

			/* Note: the SIMILAR operand was converted to lower case during
			      regex compile. */

			while ((test = *opnd++) != '\0') {
				if (atEndOfString(input) || tolower(*input++) != test) {

					MATCH_RETURN(0);
				}
			}
		}

		break;

		case BOL: // '^' (beginning of line anchor)
			if (input == startOfString) {
				if (prevIsBOL)
					break;
			} else if (*(input - 1) == '\n') {
				break;
			}

			MATCH_RETURN(0);

		case EOL: // '$' anchor matches end of line and end of string
			if (*input == '\n' || (atEndOfString(input) && succIsEOL)) {
				break;
			}

			MATCH_RETURN(0);

		case BOWORD: // '<' (beginning of word anchor)
			         /* Check to see if the current character is not a delimiter
			            and the preceding character is. */
			{
				int prev_is_delim;
				if (input == startOfString) {
					prev_is_delim = prevIsDelim;
				} else {
					prev_is_delim = Current_Delimiters[static_cast<int>(*(input - 1))];
				}
				if (prev_is_delim) {
					int current_is_delim;
					if (atEndOfString(input)) {
						current_is_delim = succIsDelim;
					} else {
						current_is_delim = Current_Delimiters[static_cast<int>(*input)];
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
				if (input == startOfString) {
					prev_is_delim = prevIsDelim;
				} else {
					prev_is_delim = Current_Delimiters[static_cast<int>(*(input - 1))];
				}
				if (!prev_is_delim) {
					int current_is_delim;
					if (atEndOfString(input)) {
						current_is_delim = succIsDelim;
					} else {
						current_is_delim = Current_Delimiters[static_cast<int>(*input)];
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
			
			if (input == startOfString) {
				prev_is_delim = prevIsDelim;
			} else {
				prev_is_delim = Current_Delimiters[*input - 1];
			}
			if (atEndOfString(input)) {
				current_is_delim = succIsDelim;
			} else {
				current_is_delim = Current_Delimiters[static_cast<int>(*input)];
			}
			
			if (!(prev_is_delim ^ current_is_delim))
				break;
		}

			MATCH_RETURN(0);

		case IS_DELIM: // \y (A word delimiter character.)
			if (Current_Delimiters[static_cast<int>(*input)] && !atEndOfString(input)) {
				input++;
				break;
			}

			MATCH_RETURN(0);

		case NOT_DELIM: // \Y (NOT a word delimiter character.)
			if (!Current_Delimiters[static_cast<int>(*input)] && !atEndOfString(input)) {
				input++;
				break;
			}

			MATCH_RETURN(0);

		case WORD_CHAR: // \w (word character; alpha-numeric or underscore)
			if ((isalnum(*input) || *input == '_') && !atEndOfString(input)) {
				input++;
				break;
			}

			MATCH_RETURN(0);

		case NOT_WORD_CHAR: // \W (NOT a word character)
			if (isalnum(*input) || *input == '_' || *input == '\n' ||
			   atEndOfString(input))
				MATCH_RETURN(0);

			input++;
			break;

		case ANY: // '.' (matches any character EXCEPT newline)
			if (atEndOfString(input) || *input == '\n')
				MATCH_RETURN(0);

			input++;
			break;

		case EVERY: // '.' (matches any character INCLUDING newline)
			if (atEndOfString(input))
				MATCH_RETURN(0);

			input++;
			break;

		case DIGIT: // \d, same as [0123456789]
			if (!isdigit(*input) || atEndOfString(input))
				MATCH_RETURN(0);

			input++;
			break;

		case NOT_DIGIT: // \D, same as [^0123456789]
			if (isdigit(*input) || *input == '\n' ||atEndOfString(input))
				MATCH_RETURN(0);

			input++;
			break;

		case LETTER: // \l, same as [a-zA-Z]
			if (!isalpha(*input) ||atEndOfString(input))
				MATCH_RETURN(0);

			input++;
			break;

		case NOT_LETTER: // \L, same as [^0123456789]
			if (isalpha(*input) || *input == '\n' ||atEndOfString(input))
				MATCH_RETURN(0);

			input++;
			break;

		case SPACE: // \s, same as [ \t\r\f\v]
			if (!isspace(*input) || *input == '\n' ||atEndOfString(input))
				MATCH_RETURN(0);

			input++;
			break;

		case SPACE_NL: // \s, same as [\n \t\r\f\v]
			if (!isspace(*input) ||atEndOfString(input))
				MATCH_RETURN(0);

			input++;
			break;

		case NOT_SPACE: // \S, same as [^\n \t\r\f\v]
			if (isspace(*input) ||atEndOfString(input))
				MATCH_RETURN(0);

			input++;
			break;

		case NOT_SPACE_NL: // \S, same as [^ \t\r\f\v]
			if ((isspace(*input) && *input != '\n') ||atEndOfString(input))
				MATCH_RETURN(0);

			input++;
			break;

		case ANY_OF: // [...] character class.
			if (atEndOfString(input))
				MATCH_RETURN(0); /* Needed because strchr ()
				                   considers \0 as a member
				                   of the character set. */

			if (find_character(getOperand(scan), *input) == nullptr) {
				MATCH_RETURN(0);
			}

			input++;
			break;

		case ANY_BUT: /* [^...] Negated character class-- does NOT normally
		               match newline (\n added usually to operand at compile
		               time.) */

			if (atEndOfString(input))
				MATCH_RETURN(0); // See comment for ANY_OF.

			if (find_character(getOperand(scan), *input) != nullptr) {
				MATCH_RETURN(0);
			}

			input++;
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
			unsigned long min = ULONG_MAX;
			unsigned long max = REG_ZERO;
			prog_type next_char;
			bool lazy = false;

			/* Lookahead (when possible) to avoid useless match attempts
			      when we know what character comes next. */

			if (getOpcode(next) == EXACTLY) {
				next_char = *getOperand(next);
			} else {
				next_char = '\0'; // i.e. Don't know what next character is.
			}

			prog_type *next_op = getOperand(scan);

			switch (getOpcode(scan)) {
			case LAZY_STAR:
				lazy = true;
			case STAR:
				min = REG_ZERO;
				max = ULONG_MAX;
				break;

			case LAZY_PLUS:
				lazy = true;
			case PLUS:
				min = REG_ONE;
				max = ULONG_MAX;
				break;

			case LAZY_QUESTION:
				lazy = true;
			case QUESTION:
				min = REG_ZERO;
				max = REG_ONE;
				break;

			case LAZY_BRACE:
				lazy = true;
			case BRACE:
				min = getOffset(scan + Regex::NextPtrSize);

				max = getOffset(scan + (2 * Regex::NextPtrSize));

				if (max <= REG_INFINITY)
					max = ULONG_MAX;

				next_op = getOperand(scan + (2 * Regex::NextPtrSize));
			}

			const char *save = input;

			if (lazy) {
				if (min > REG_ZERO)
					num_matched = greedy(next_op, min);
			} else {
				num_matched = greedy(next_op, max);
			}

			while (min <= num_matched && num_matched <= max) {
				if (next_char == '\0' || next_char == *input) {
					if (match(next, nullptr))
						MATCH_RETURN(1);

					CHECK_RECURSION_LIMIT
				}

				// Couldn't or didn't match.

				if (lazy) {
					if (!greedy(next_op, 1)) {
						MATCH_RETURN(0);
					}

					num_matched++; // Inch forward.
				} else if (num_matched > REG_ZERO) {
					num_matched--; // Back up.
				} else if (min == REG_ZERO && num_matched == REG_ZERO) {
					break;
				}

				input = save + num_matched;
			}

			MATCH_RETURN(0);
		}

		break;

		case END:
			if (Extent_Ptr_FW == nullptr || (input - Extent_Ptr_FW) > 0) {
				Extent_Ptr_FW = input;
			}

			MATCH_RETURN(1); // Success!

			break;

		case INIT_COUNT:
			brace_counts_[*getOperand(scan)] = REG_ZERO;

			break;

		case INC_COUNT:
			brace_counts_[*getOperand(scan)]++;

			break;

		case TEST_COUNT:
			if (brace_counts_[*getOperand(scan)] < getOffset(scan + Regex::NextPtrSize + Regex::IndexSize)) {
				next = scan + Regex::NodeSize + Regex::IndexSize + Regex::NextPtrSize;
			}

			break;

		case BACK_REF:
		case BACK_REF_CI:
			// case X_REGEX_BR:
			// case X_REGEX_BR_CI: *** IMPLEMENT LATER
			{
				prog_type paren_no = *getOperand(scan);

				/* if (GET_OP_CODE (scan) == X_REGEX_BR ||
				       GET_OP_CODE (scan) == X_REGEX_BR_CI) {

				      if (Cross_Regex_Backref == nullptr) MATCH_RETURN (0);

				      captured =
				         (prog_type *) Cross_Regex_Backref->startp [paren_no];

				      finish =
				         (prog_type *) Cross_Regex_Backref->endp   [paren_no];
				   } else { */
				   
				   
				assert(paren_no < MaxBackRefs);
				const char *captured = Back_Ref_Start[paren_no];
				const char *finish   = Back_Ref_End[paren_no];
				// }

				if ((captured != nullptr) && (finish != nullptr)) {
					if (captured > finish)
						MATCH_RETURN(0);

                    if (getOpcode(scan) == BACK_REF_CI /* ||
                                  GET_OP_CODE (scan) == X_REGEX_BR_CI*/) {

						while (captured < finish) {
							if (atEndOfString(input) ||
								tolower(*captured++) != tolower(*input++)) {
								MATCH_RETURN(0);
							}
						}
					} else {
						while (captured < finish) {
							if (atEndOfString(input) || *captured++ != *input++)
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

			const char *save = input;

			/* Temporarily ignore the logical end of the string, to allow
			      lookahead past the end. */
			const char *saved_end = endOfString;
			endOfString = nullptr;

			int answer = match(next, nullptr); // Does the look-ahead regex match?

			CHECK_RECURSION_LIMIT

			if ((getOpcode(scan) == POS_AHEAD_OPEN) ? answer : !answer) {
				/* Remember the last (most to the right) character position
				     that we consume in the input for a successful match.  This
				     is info that may be needed should an attempt be made to
				     match the exact same text at the exact same place.  Since
				     look-aheads backtrack, a regex with a trailing look-ahead
				     may need more text than it matches to accomplish a
				     re-match. */

				if (Extent_Ptr_FW == nullptr || (input - Extent_Ptr_FW) > 0) {
					Extent_Ptr_FW = input;
				}

				input = save;          // Backtrack to look-ahead start.
				endOfString = saved_end; // Restore logical end.

				/* Jump to the node just after the (?=...) or (?!...)
				     Construct. */

				next = next_ptr(getOperand(scan)); // Skip 1st branch
				// Skip the chain of branches inside the look-ahead
				while (getOpcode(next) == BRANCH)
					next = next_ptr(next);
				next = next_ptr(next); // Skip the LOOK_AHEAD_CLOSE
			} else {
				input = save;          // Backtrack to look-ahead start.
				endOfString = saved_end; // Restore logical end.

				MATCH_RETURN(0);
			}
		}

		break;

		case POS_BEHIND_OPEN:
		case NEG_BEHIND_OPEN: {
			int found = 0;

			const char *save = input;
			const char *saved_end = endOfString;

			/* Prevent overshoot (greedy matching could end past the
			      current position) by tightening the matching boundary.
			      Lookahead inside lookbehind can still cross that boundary. */
			endOfString = input;

			int lower = get_lower(scan);
			int upper = get_upper(scan);

			/* Start with the shortest match first. This is the most
			      efficient direction in general.
			      Note! Negative look behind is _very_ tricky when the length
			      is not constant: we have to make sure the expression doesn't
			      match for _any_ of the starting positions. */
			for (int offset = lower; offset <= upper; ++offset) {
				input = save - offset;

				if (input < lookBehindTo) {
					// No need to look any further
					break;
				}

				int answer = match(next, nullptr); // Does the look-behind regex match?

				CHECK_RECURSION_LIMIT

				/* The match must have ended at the current position;
				             otherwise it is invalid */
				if (answer && input == save) {
					// It matched, exactly far enough
					found = 1;

					/* Remember the last (most to the left) character position
					    that we consume in the input for a successful match.
					    This is info that may be needed should an attempt be
					    made to match the exact same text at the exact same
					    place. Since look-behind backtracks, a regex with a
					    leading look-behind may need more text than it matches
					    to accomplish a re-match. */

					if (Extent_Ptr_BW == nullptr || (Extent_Ptr_BW - (save - offset)) > 0) {
						Extent_Ptr_BW = save - offset;
					}

					break;
				}
			}

			// Always restore the position and the logical string end.
			input = save;
			endOfString = saved_end;

			if ((getOpcode(scan) == POS_BEHIND_OPEN) ? found : !found) {
				/* The look-behind matches, so we must jump to the next
				     node. The look-behind node is followed by a chain of
				     branches (contents of the look-behind expression), and
				     terminated by a look-behind-close node. */
				next = next_ptr(getOperand(scan) + Regex::LengthSize); // 1st branch
				// Skip the chained branches inside the look-ahead
				while (getOpcode(next) == BRANCH)
					next = next_ptr(next);
				next = next_ptr(next); // Skip LOOK_BEHIND_CLOSE
			} else {
				// Not a match
				MATCH_RETURN(0);
			}
		} break;

		case LOOK_AHEAD_CLOSE:
		case LOOK_BEHIND_CLOSE:
			MATCH_RETURN(1); /* We have reached the end of the look-ahead or look-behind which implies that we matched it, so return TRUE. */
		default:
			if ((getOpcode(scan) > OPEN) && (getOpcode(scan) < OPEN + NSUBEXP)) {

				int no = getOpcode(scan) - OPEN;
				const char *save = input;

				if (no < 10) {
					Back_Ref_Start[no] = save;
					Back_Ref_End[no] = nullptr;
				}

				if (match(next, nullptr)) {
					/* Do not set 'Start_Ptr_Ptr' if some later invocation (think
					 recursion) of the same parentheses already has. */

					if (Start_Ptr_Ptr[no] == nullptr)
						Start_Ptr_Ptr[no] = save;

					MATCH_RETURN(1);
				} else {
					MATCH_RETURN(0);
				}
			} else if ((getOpcode(scan) > CLOSE) && (getOpcode(scan) < CLOSE + NSUBEXP)) {

				int no = getOpcode(scan) - CLOSE;
				const char *save = input;

				if (no < 10) {
					Back_Ref_End[no] = save;
				}

				if (match(next, nullptr)) {
					/* Do not set 'End_Ptr_Ptr' if some later invocation of the
					 same parentheses already has. */

					if (End_Ptr_Ptr[no] == nullptr)
						End_Ptr_Ptr[no] = save;

					MATCH_RETURN(1);
				} else {
					MATCH_RETURN(0);
				}
			} else {
				qDebug("memory corruption, 'match'");

				MATCH_RETURN(0);
			}

			break;
		}

		scan = next;
	}

	/* We get here only if there's trouble -- normally "case END" is
	  the terminating point. */

	qDebug("corrupted pointers, 'match'");

	MATCH_RETURN(0);
}

//------------------------------------------------------------------------------
// Name: greedy
// Desc: Repeatedly match something simple up to "max" times. If max <= 0
//       then match as much as possible (max = infinity).  Uses unsigned long
//       variables to maximize the amount of text matchable for unbounded
//       qualifiers like '*' and '+'.  This will allow at least 4,294,967,295
//       matches (4 Gig!) for an ANSI C compliant compiler.  If you are
//       applying a regex to something bigger than that, you shouldn't be
//       using NEdit!
// Returns: the actual number of matches.
//------------------------------------------------------------------------------
unsigned long RegexMatch::greedy(prog_type *p, long max) {


	unsigned long count = REG_ZERO;

	const char *input_str = input;
	prog_type *operand      = getOperand(p); // Literal char or start of class characters.
	unsigned long max_cmp = (max > 0) ? static_cast<unsigned long>(max) : ULONG_MAX;

	switch (getOpcode(p)) {
	case ANY:
		/* Race to the end of the line or string. Dot DOESN'T match
		    newline. */

		while (count < max_cmp && *input_str != '\n' && !atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case EVERY:
		// Race to the end of the line or string. Dot DOES match newline.

		while (count < max_cmp && !atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case EXACTLY: // Count occurrences of single character operand.
		while (count < max_cmp && *operand == *input_str && !atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case SIMILAR: // Case insensitive version of EXACTLY
		while (count < max_cmp && *operand == tolower(*input_str) && !atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case ANY_OF: // [...] character class.
		while (count < max_cmp && find_character(operand, *input_str) != nullptr &&
			   !atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case ANY_BUT: /* [^...] Negated character class- does NOT normally
	                   match newline (\n added usually to operand at compile
	                   time.) */

		while (count < max_cmp && find_character(operand, *input_str) == nullptr &&
			   !atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case IS_DELIM: /* \y (not a word delimiter char)
	                     NOTE: '\n' and '\0' are always word delimiters. */

		while (count < max_cmp && Current_Delimiters[static_cast<int>(*input_str)] && !atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case NOT_DELIM: /* \Y (not a word delimiter char)
	                     NOTE: '\n' and '\0' are always word delimiters. */

		while (count < max_cmp && !Current_Delimiters[static_cast<int>(*input_str)] && !atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case WORD_CHAR: // \w (word character, alpha-numeric or underscore)
		while (count < max_cmp && (isalnum(*input_str) || *input_str == '_') &&
			   !atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case NOT_WORD_CHAR: // \W (NOT a word character)
		while (count < max_cmp && !isalnum(*input_str) && *input_str != '_' &&
			   *input_str != '\n' && !atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case DIGIT: // same as [0123456789]
		while (count < max_cmp && isdigit(*input_str) && !atEndOfString(input_str)) {
			count++;
			input_str++;
		}

		break;

	case NOT_DIGIT: // same as [^0123456789]
		while (count < max_cmp && !isdigit(*input_str) && *input_str != '\n' &&
			   !atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case SPACE: // same as [ \t\r\f\v]-- doesn't match newline.
		while (count < max_cmp && isspace(*input_str) && *input_str != '\n' &&
			   !atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case SPACE_NL: // same as [\n \t\r\f\v]-- matches newline.
		while (count < max_cmp && isspace(*input_str) && !atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case NOT_SPACE: // same as [^\n \t\r\f\v]-- doesn't match newline.
		while (count < max_cmp && !isspace(*input_str) && !atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case NOT_SPACE_NL: // same as [^ \t\r\f\v]-- matches newline.
		while (count < max_cmp && (!isspace(*input_str) || *input_str == '\n') && !atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case LETTER: // same as [a-zA-Z]
		while (count < max_cmp && isalpha(*input_str) && !atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	case NOT_LETTER: // same as [^a-zA-Z]
		while (count < max_cmp && !isalpha(*input_str) && *input_str != '\n' && !atEndOfString(input_str)) {

			count++;
			input_str++;
		}

		break;

	default:
		/* Called inappropriately.  Only atoms that are SIMPLE should
		    generate a call to greedy.  The above cases should cover
		    all the atoms that are SIMPLE. */

		qDebug("internal error #10 'greedy'");
		count = 0U; // Best we can do.
	}

	// Point to character just after last matched character.

	input = input_str;

	return count;
}

//------------------------------------------------------------------------------
// Name: atEndOfString
//------------------------------------------------------------------------------
bool RegexMatch::atEndOfString(const char *p) const {
	return (*p == '\0' || (endOfString != nullptr && p >= endOfString));
}
