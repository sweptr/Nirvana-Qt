
#ifndef TYPES_H_
#define TYPES_H_

#include <string>

//#define USE_WCHAR

#ifdef USE_WCHAR

typedef wchar_t                     char_type;
typedef std::char_traits<char_type> traits_type;
#define _T(x)     L ## x
#define _snprintf swprintf
#define _sscanf   swscanf
#define _strncpy  wcsncpy
#define _strcpy   wcscpy

#else

typedef char                        char_type;
typedef std::char_traits<char_type> traits_type;
#define _T(x)     x
#define _snprintf snprintf
#define _sscanf   sscanf
#define _strncpy  strncpy
#define _strcpy   strcpy
#endif

#endif
