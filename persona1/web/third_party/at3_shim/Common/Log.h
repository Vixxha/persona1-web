#pragma once
// Stand-in for PPSSPP's logging: decoder messages are dropped.
namespace Log {
constexpr int ME = 0;
}
#define DEBUG_LOG(category, ...) ((void)(category))
#define INFO_LOG(category, ...) ((void)(category))
#define WARN_LOG(category, ...) ((void)(category))
#define ERROR_LOG(category, ...) ((void)(category))
