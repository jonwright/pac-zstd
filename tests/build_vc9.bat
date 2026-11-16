@echo off
rem Visual C++ 2008 (the Python 2.7 compiler, "Visual C++ for Python 9.0"), 32 bit.
rem Run from the repository root.  Output in build-win\ ; ends by running the tests.
rem   - src/pac.c is plain C and needs nothing special (it has its own _MSC_VER < 1600 paths)
rem   - kcb's bitshuffle.c is C99 (declarations in for loops, restrict): VC9 compiles it as
rem     C++ (/TP) with restrict -> __restrict (tests\vc9\immintrin.h).  The submodule itself stays untouched.
rem   - tests\vc9 has the stdint.h and immintrin.h that VC9 lacks.
call "%LOCALAPPDATA%\Programs\Common\Microsoft\Visual C++ for Python\9.0\vcvarsall.bat" x86 || exit /b 1
if not exist build-win mkdir build-win
cl /nologo /O2 /W3 /arch:SSE2 /c /Fobuild-win\ /DPAC_WITH_KCB /Ithird_party\zstd\lib /Ithird_party\lz4\lib src\pac.c || exit /b 1
cl /nologo /O2 /c /Fobuild-win\ third_party\lz4\lib\lz4.c || exit /b 1
cl /nologo /O2 /c /Fobuild-win\ /DZSTD_DISABLE_ASM=1 /DZSTD_LEGACY_SUPPORT=0 third_party\zstd\lib\common\*.c third_party\zstd\lib\compress\*.c third_party\zstd\lib\decompress\*.c || exit /b 1
cl /nologo /O2 /TP /Itests\vc9 /Ithird_party\kcb\src /c /Fobuild-win\ third_party\kcb\src\bitshuffle.c || exit /b 1
cl /nologo /O2 /W3 /Itests\vc9 /Isrc /Ithird_party\zstd\lib /Ithird_party\lz4\lib tests\test_pac.c build-win\*.obj /Febuild-win\test_pac.exe || exit /b 1
build-win\test_pac.exe 1 20 quick
