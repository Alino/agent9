#ifndef _CC9_CXXABI_H
#define _CC9_CXXABI_H
/* cc9: minimal <cxxabi.h>. The staged libc++ headers don't install the
 * libcxxabi header, but libcc9cxx.a carries the real Itanium ABI runtime
 * (including the demangler). Declare the pieces ports actually use. */
#ifdef __cplusplus
#include <stddef.h>

namespace __cxxabiv1 {
extern "C" {
/* Itanium ABI demangler (real implementation in libcc9cxx.a). */
char* __cxa_demangle(char const* mangled_name, char* output_buffer, size_t* length, int* status);
}
}
namespace abi = __cxxabiv1;
#endif
#endif
