#pragma once
#ifndef LIB_SCOPE_H_
#define LIB_SCOPE_H_

/**
 * Implementation of SCOPE_GUARD macro for adding a lightweight defer-alike to
 * get RAII like behaviour when dealing with C functions.
 */
namespace lib {
namespace scope {

template <typename Func>
class ScopeGuard {
 public:
  ScopeGuard(Func f) : f_(std::move(f)) {}
  ScopeGuard(ScopeGuard&& sg) : f_(std::move(sg.f_)) {}

  ScopeGuard(const ScopeGuard& f) = delete;
  ScopeGuard& operator=(const ScopeGuard& f) = delete;

  ~ScopeGuard() { f_(); }

 private:
  Func f_;
};

template <typename Func>
inline ScopeGuard<Func> MakeScopeGuard(Func f) {
  return ScopeGuard<Func>(f);
}

}  // namespace scope
}  // namespace lib

#define CONCATENATE_REAL(x, y) x ## y
#define CONCATENATE_EXPAND(x, y) CONCATENATE_REAL(x, y)
#define SCOPE_GUARD(c) \
  auto CONCATENATE_EXPAND(scope_guard_, __LINE__) = lib::scope::MakeScopeGuard([&](){c;});

#endif
