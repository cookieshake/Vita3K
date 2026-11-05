#pragma once
#include <string>
#define LOG_WARN(...) ((void)0)
#define LOG_WARN_ONCE(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
template <class T> std::string log_hex(T value) { return std::to_string(value); }
