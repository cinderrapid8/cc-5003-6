#pragma once

#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <chrono>
#include <atomic>

namespace dbs {

enum class LogLevel { Info, Warn, Error };

class Logger {
public:
    static Logger& instance();

    void openFile(const std::string& path);
    void setConsoleEnabled(bool enabled) { consoleEnabled_ = enabled; }

    void log(LogLevel level, std::string_view component, std::string_view message);

    std::uint64_t nextSequence();

private:
    Logger() = default;
    std::mutex mutex_;
    std::FILE* file_ = nullptr;
    bool consoleEnabled_ = true;
    std::atomic<std::uint64_t> sequence_{0};
};

#define DBS_LOG_INFO(component, msg) ::dbs::Logger::instance().log(::dbs::LogLevel::Info, component, msg)
#define DBS_LOG_WARN(component, msg) ::dbs::Logger::instance().log(::dbs::LogLevel::Warn, component, msg)
#define DBS_LOG_ERROR(component, msg) ::dbs::Logger::instance().log(::dbs::LogLevel::Error, component, msg)

std::string formatLogLine(std::uint64_t seq, LogLevel level, std::string_view component,
                          std::string_view message,
                          std::chrono::system_clock::time_point ts = std::chrono::system_clock::now());

}  // namespace dbs
