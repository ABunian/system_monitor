#ifndef COMMON_HPP
#define COMMON_HPP

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <filesystem>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>


struct SystemMetrics {
    std::string timestamp;

    double cpuPercent = 0.0;
    double memoryPercent = 0.0;
    double memoryUsedMB = 0.0;
    double memoryAvailableMB = 0.0;

    double load1 = 0.0;
    double load5 = 0.0;
    double load15 = 0.0;

    double cpuTempC = -1.0;

    double diskRootPercent = 0.0;
    double diskRootUsedGB = 0.0;
    double diskRootAvailGB = 0.0;

    double nvmeTempC = -1.0;
    int nvmePercentageUsed = -1;
    int nvmeAvailableSpare = -1;
    long long nvmeMediaErrors = -1;
    int nvmeCriticalWarning = -1;
};

inline std::string nowString() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_r(&t, &local_tm);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

inline std::string dateString() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_r(&t, &local_tm);

    std::ostringstream oss;
    oss << std::put_time(&local_tm, "%Y-%m-%d");
    return oss.str();
}

inline void ensureDir(const std::string& path) {
    mkdir(path.c_str(), 0755);
}

inline bool fileExists(const std::string& path) {
    std::ifstream ifs(path);
    return ifs.good();
}

inline bool ensureDirectory(const std::string& path) {
    try {
        if (!std::filesystem::exists(path)) {
            return std::filesystem::create_directories(path);
        }
        return true;
    } catch (...) {
        return false;
    }
}

inline void appendLog(const std::string& logPath, const std::string& msg) {
    std::ofstream ofs(logPath, std::ios::app);
    ofs << "[" << nowString() << "] " << msg << "\n";
}

#endif
