// cc9: std::exception_ptr / current_exception / rethrow_exception / nested_exception
// for the freestanding 9front runtime. These delegate to the libcxxabi __cxa_*
// primary-exception API (already in the archive). Body adapted from libc++'s
// libcxx/src/support/runtime/exception_pointer_cxxabi.ipp (Apache-2.0 WITH
// LLVM-exception); we ship only this slice because the full libc++ exception.cpp
// also redefines terminate/unexpected/bad_cast/bad_typeid and would clash with
// the libcxxabi objects cc9 links. Needed by std::promise/future/call_once and
// std::current_exception/rethrow_exception.
#include <cxxabi.h>
#include <exception>

namespace std {

exception_ptr::~exception_ptr() noexcept { abi::__cxa_decrement_exception_refcount(__ptr_); }

exception_ptr::exception_ptr(const exception_ptr& other) noexcept : __ptr_(other.__ptr_) {
  abi::__cxa_increment_exception_refcount(__ptr_);
}

exception_ptr& exception_ptr::operator=(const exception_ptr& other) noexcept {
  if (__ptr_ != other.__ptr_) {
    abi::__cxa_increment_exception_refcount(other.__ptr_);
    abi::__cxa_decrement_exception_refcount(__ptr_);
    __ptr_ = other.__ptr_;
  }
  return *this;
}

exception_ptr exception_ptr::__from_native_exception_pointer(void* __e) noexcept {
  exception_ptr ptr;
  ptr.__ptr_ = __e;
  abi::__cxa_increment_exception_refcount(ptr.__ptr_);
  return ptr;
}

nested_exception::nested_exception() noexcept : __ptr_(current_exception()) {}

nested_exception::~nested_exception() noexcept {}

void nested_exception::rethrow_nested() const {
  if (__ptr_ == nullptr)
    terminate();
  rethrow_exception(__ptr_);
}

exception_ptr current_exception() noexcept {
  exception_ptr ptr;
  ptr.__ptr_ = abi::__cxa_current_primary_exception();
  return ptr;
}

void rethrow_exception(exception_ptr p) {
  abi::__cxa_rethrow_primary_exception(p.__ptr_);
  terminate();   // returns here only if p was null
}

} // namespace std
