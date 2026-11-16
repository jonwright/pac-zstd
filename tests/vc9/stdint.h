/* Stand-in for Visual C++ 2008 (VC9, the Python 2.7 compiler), which has no <stdint.h>.
 * Used only by tests/build_vc9.bat, through /Itests\vc9. */
#ifndef PAC_VC9_STDINT_H
#define PAC_VC9_STDINT_H
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef short int16_t;
typedef unsigned short uint16_t;
typedef int int32_t;
typedef unsigned int uint32_t;
typedef __int64 int64_t;
typedef unsigned __int64 uint64_t;
typedef int intptr_t;
typedef unsigned int uintptr_t;
#endif
