#include "logger.h"

#include <ctime>
#include <iomanip>
#include <sstream>

namespace dbs {

namespace {
const char* levelName(LogLevel level) {
    switch (level) {
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "INFO";
}
}  // namespace

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::openFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    file_ = std::fopen(path.c_str(), "w");
}

std::uint64_t Logger::nextSequence() {
    return sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::string formatLogLine(std::uint64_t seq, LogLevel level, std::string_view component,
                          std::string_view message,
                          std::chrono::system_clock::time_point ts) {
    const auto now = std::chrono::time_point_cast<std::chrono::milliseconds>(ts);
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - secs).count();
    const std::time_t t = std::chrono::system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);

    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S")
        << '.' << std::setw(3) << std::setfill('0') << millis
        << " #" << seq
        << " [" << std::setw(5) << std::setfill(' ') << levelName(level) << "]"
        << "[" << component << "] " << message;
    return out.str();
}

void Logger::log(LogLevel level, std::string_view component, std::string_view message) {
    const std::uint64_t seq = nextSequence();
    const std::string line = formatLogLine(seq, level, component, message);
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) {
        std::fwrite(line.data(), 1, line.size(), file_);
        std::fputc('\n', file_);
        std::fflush(file_);
    }
    if (consoleEnabled_) {
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fputc('\n', stderr);
    }
}

}  // namespace dbs
