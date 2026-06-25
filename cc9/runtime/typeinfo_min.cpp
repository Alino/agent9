// The out-of-line std::type_info destructor. Being the key function, defining it
// emits type_info's vtable AND its typeinfo (_ZTISt9type_info) — exactly the RTTI
// base symbols dynamic_cast/typeid need. We do NOT pull libcxxabi's
// stdlib_typeinfo.cpp because it would duplicate bad_cast/bad_typeid (already in
// libcxx's exception.cpp). private_typeinfo.cpp supplies __dynamic_cast.
#include <typeinfo>
std::type_info::~type_info() {}
