
#ifndef REGEX_EXCEPTION_H_
#define REGEX_EXCEPTION_H_

#include <stdexcept>

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

#endif
