#pragma once

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

enum class LogLevel { Info, Warn, Error, Ok };

struct LogLine {
    LogLevel    level;
    std::string text;
};

class Log {
public:
    void Add(LogLevel lvl, const char* fmt, ...) {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.push_back({lvl, buf});
        if (m_lines.size() > 2000) m_lines.erase(m_lines.begin(), m_lines.begin() + 1000);
    }

    void Info(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt); AddV(LogLevel::Info, fmt, ap); va_end(ap);
    }
    void Warn(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt); AddV(LogLevel::Warn, fmt, ap); va_end(ap);
    }
    void Error(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt); AddV(LogLevel::Error, fmt, ap); va_end(ap);
    }
    void Ok(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt); AddV(LogLevel::Ok, fmt, ap); va_end(ap);
    }

    std::vector<LogLine> Snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lines;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.clear();
    }

private:
    void AddV(LogLevel lvl, const char* fmt, va_list ap) {
        char buf[2048];
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lines.push_back({lvl, buf});
        if (m_lines.size() > 2000) m_lines.erase(m_lines.begin(), m_lines.begin() + 1000);
    }

    mutable std::mutex   m_mutex;
    std::vector<LogLine> m_lines;
};
