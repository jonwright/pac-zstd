/* VC9 has SSE2 (emmintrin.h) and nothing newer; kcb's bitshuffle.c only needs this one.
 * It also needs C99 restrict, which VC9 spells __restrict; defined here, after the system
 * headers (which use "restrict" in __declspec(restrict)), since this is the last include
 * bitshuffle.c makes before using the word. */
#include <emmintrin.h>
#include <malloc.h>
#include <string.h>
#define restrict __restrict
