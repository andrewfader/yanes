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

// std::system() hands the string to `cmd /c` on Windows, and cmd strips the leading quote along
// with the last quote on the line whenever the command starts with one and carries more than a
// single quoted token (see `cmd /?`). That mangles every "<exe>" "<arg>" invocation below. Wrapping
// the whole command in one more pair of quotes gives cmd a pair to eat and leaves the real quoting
// intact. Elsewhere the shell needs no such help.
inline std::string shell_command(const std::string& command) {
#ifdef _WIN32
  return "\"" + command + "\"";
#else
  return command;
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
