#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace test_platform {

inline int process_id() {
#ifdef _WIN32
  return _getpid();
#else
  return getpid();
#endif
}

inline bool set_environment(const char* name, const std::string& value) {
#ifdef _WIN32
  return _putenv_s(name, value.c_str()) == 0;
#else
  return setenv(name, value.c_str(), 1) == 0;
#endif
}

inline void unset_environment(const char* name) {
#ifdef _WIN32
  (void)_putenv_s(name, "");
#else
  (void)unsetenv(name);
#endif
}

inline const char* null_device() {
#ifdef _WIN32
  return "NUL";
#else
  return "/dev/null";
#endif
}

inline char path_list_separator() {
#ifdef _WIN32
  return ';';
#else
  return ':';
#endif
}

}  // namespace test_platform
