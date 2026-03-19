/*
    V003: Collect cpu metrics,
                  memery usage,
                  load averge,
                  nvme metrics.
*/
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <sys/statvfs.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common.hpp"

namespace fs = std::filesystem;

struct CpuTimes {
    unsigned long long user = 0;
    unsigned long long nice = 0;
    unsigned long long system = 0;
    unsigned long long idle = 0;
    unsigned long long iowait = 0;
    unsigned long long irq = 0;
    unsigned long long softirq = 0;
    unsigned long long steal = 0;
};

struct NvmeSmartInfo {
    bool ok = false;
    double temperatureC = -1.0;
    int percentageUsed = -1;
    int availableSpare = -1;
    long long mediaErrors = -1;
    int criticalWarning = -1;
    std::time_t lastUpdate = 0;
};

struct ThresholdConfig {
    double cpuWarnPercent = 20.0;
    double memWarnPercent = 50.0;
    double tempWarnC = 70.0;
    double diskWarnPercent = 80.0;

    double nvmeTempWarnC = 60.0;
    int nvmePercentageUsedWarn = 80;
    int nvmeAvailableSpareWarn = 10;
    long long nvmeMediaErrorsWarn = 1;
};

static const std::string DATA_DIR = "data";
static const std::string LOG_DIR = "logs";
static const std::string CSV_PATH = "data/collector.csv";
static const std::string LOG_PATH = "logs/collector.log";
static const std::string EVENT_LOG_PATH = "logs/events.log";
static const std::string CONFIG_PATH = "config/threshold.conf";

static const std::string NVME_DEVICE = "/dev/nvme0";

static const int NVME_REFRESH_SECONDS = 600;

static const bool DEBUG = false;

void ensureCsvHeader(const std::string& csvPath) {
    if (fileExists(csvPath)) return;

    std::ofstream ofs(csvPath, std::ios::out);
    ofs << "timestamp"
        << ",cpu_percent"
        << ",memory_percent"
        << ",memory_used_mb"
        << ",memory_available_mb"
        << ",load_1"
        << ",load_5"
        << ",load_15"
        << ",cpu_temp_c"
        << ",disk_root_percent"
        << ",disk_root_used_gb"
        << ",disk_root_avail_gb"
        << ",nvme_temp_c"
        << ",nvme_percentage_used"
        << ",nvme_available_spare"
        << ",nvme_media_errors"
        << ",nvme_critical_warning"
        << "\n";
}

void writeEvent(const std::string& logPath,
                const std::string& title,
                const std::vector<std::string>& details = {}) {
    std::ofstream ofs(logPath, std::ios::app);
    ofs << "[" << nowString() << "] " << title << "\n";
    for (const auto& line : details) {
        ofs << "  " << line << "\n";
    }
}

std::string execCommand(const std::string& cmd) {
    std::string result;
    char buffer[512];

    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return result;

    while (fgets(buffer, sizeof(buffer), fp)) {
        result += buffer;
    }

    pclose(fp);
    return result;
}

bool readCpuTimes(CpuTimes& times) {
    if (DEBUG) std::cout << "Reading CPU times from /proc/stat.\n";
    std::ifstream file("/proc/stat");
    if (!file.is_open()) return false;

    std::string cpuLabel;
    file >> cpuLabel;
    if (cpuLabel != "cpu") return false;

    file >> times.user
         >> times.nice
         >> times.system
         >> times.idle
         >> times.iowait
         >> times.irq
         >> times.softirq
         >> times.steal;
    if (DEBUG) {
        std::cout << times.user << ", "
                  << times.nice << ", "
                  << times.system << ", "
                  << times.idle << ", "
                  << times.iowait << ", "
                  << times.irq << ", "
                  << times.softirq << ", "
                  << times.steal << std::endl;
        std::cout << "Finished reading CPU times\n";
    }
    return true;
}

double calculateCpuPercent(const CpuTimes& prev, const CpuTimes& curr) {
    if (DEBUG) std::cout << "Calculating CPU percent.\n";
    unsigned long long prevIdle = prev.idle + prev.iowait;
    unsigned long long currIdle = curr.idle + curr.iowait;

    unsigned long long prevNonIdle =
        prev.user + prev.nice + prev.system + prev.irq + prev.softirq + prev.steal;
    unsigned long long currNonIdle =
        curr.user + curr.nice + curr.system + curr.irq + curr.softirq + curr.steal;

    unsigned long long prevTotal = prevIdle + prevNonIdle;
    unsigned long long currTotal = currIdle + currNonIdle;

    unsigned long long totald = currTotal - prevTotal;
    unsigned long long idled = currIdle - prevIdle;

    if (totald == 0) return 0.0;

    return 100.0 * static_cast<double>(totald - idled) / static_cast<double>(totald);
}

bool readMemory(double& memPercent, double& usedMB, double& availMB) {
    if (DEBUG) std::cout << "Reading memory info from /proc/meminfo.\n";
    std::ifstream file("/proc/meminfo");
    if (!file.is_open()) return false;

    std::string key;
    unsigned long long value = 0;
    std::string unit;

    unsigned long long memTotalKB = 0;
    unsigned long long memAvailableKB = 0;

    while (file >> key >> value >> unit) {
        if (key == "MemTotal:") {
            memTotalKB = value;
        } else if (key == "MemAvailable:") {
            memAvailableKB = value;
        }
    }

    if (memTotalKB == 0) return false;

    unsigned long long memUsedKB = memTotalKB - memAvailableKB;

    memPercent = 100.0 * static_cast<double>(memUsedKB) / static_cast<double>(memTotalKB);
    usedMB = static_cast<double>(memUsedKB) / 1024.0;
    availMB = static_cast<double>(memAvailableKB) / 1024.0;
    return true;
}

bool readLoadAvg(double& l1, double& l5, double& l15) {
    if (DEBUG) std::cout << "Reading load average from /proc/loadavg.\n";
    std::ifstream file("/proc/loadavg");
    if (!file.is_open()) return false;

    file >> l1 >> l5 >> l15;
    return true;
}

bool readCpuTemperatureC(double& temp) {
    if (DEBUG) std::cout << "Reading CPU temperature from /sys/class/thermal/thermal_zone0/temp.\n";
    std::ifstream file("/sys/class/thermal/thermal_zone0/temp");
    if (!file.is_open()) return false;

    long tempMilli = 0;
    file >> tempMilli;
    temp = static_cast<double>(tempMilli) / 1000.0;
    return true;
}

bool readDiskUsageRoot(double& usedPercent, double& usedGB, double& availGB) {
    if (DEBUG) std::cout << "Reading disk usage for root from statvfs.\n";
    struct statvfs fsinfo {};
    if (statvfs("/", &fsinfo) != 0) return false;

    unsigned long long total =
        static_cast<unsigned long long>(fsinfo.f_blocks) * fsinfo.f_frsize;
    unsigned long long avail =
        static_cast<unsigned long long>(fsinfo.f_bavail) * fsinfo.f_frsize;
    unsigned long long used = total - avail;

    if (total == 0) return false;

    usedPercent = 100.0 * static_cast<double>(used) / static_cast<double>(total);
    usedGB = static_cast<double>(used) / (1024.0 * 1024.0 * 1024.0);
    availGB = static_cast<double>(avail) / (1024.0 * 1024.0 * 1024.0);

    return true;
}

std::vector<std::string> getTopProcessesByCpu(int topN = 3) {
    std::vector<std::string> result;
    FILE* fp = popen("ps -eo pid,comm,%cpu,%mem --sort=-%cpu | head -n 4", "r");
    if (!fp) return result;

    char buffer[512];
    bool skipHeader = true;

    while (fgets(buffer, sizeof(buffer), fp)) {
        std::string line(buffer);
        if (skipHeader) {
            skipHeader = false;
            continue;
        }

        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }

        if (!line.empty()) {
            result.push_back(line);
        }

        if (static_cast<int>(result.size()) >= topN) break;
    }

    pclose(fp);
    return result;
}

std::vector<std::string> getTopProcessesByMem(int topN = 3) {
    std::vector<std::string> result;
    FILE* fp = popen("ps -eo pid,comm,%cpu,%mem --sort=-%mem | head -n 4", "r");
    if (!fp) return result;

    char buffer[512];
    bool skipHeader = true;

    while (fgets(buffer, sizeof(buffer), fp)) {
        std::string line(buffer);
        if (skipHeader) {
            skipHeader = false;
            continue;
        }

        if (!line.empty() && line.back() == '\n') {
            line.pop_back();
        }

        if (!line.empty()) {
            result.push_back(line);
        }

        if (static_cast<int>(result.size()) >= topN) break;
    }

    pclose(fp);
    return result;
}

static std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }

    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }

    return s.substr(start, end - start);
}

static bool splitKeyValueLine(const std::string& line, std::string& key, std::string& value) {
    size_t pos = line.find(':');
    if (pos == std::string::npos) {
        return false;
    }
    key = trim(line.substr(0, pos));
    value = trim(line.substr(pos + 1));
    return true;
}

bool extractNvmeInfo(const std::string& text, 
                     double& temperatureC, 
                     int& percentageUsed, 
                     int& availableSpare, 
                     long long& mediaErrors, 
                     int& criticalWarning) {
    std::istringstream iss(text);
    std::string line;
    bool tempFound = false, percentageUsedFound = false, availableSpareFound = false, mediaErrorsFound = false, criticalWarningFound = false;
    int bufPTU = 0, bufAS =0, bufCW = 0;
    double bufTemp = 0.0;
    long long bufME = 0;
    while (std::getline(iss, line)) {
        std::string key, val;
        if (!splitKeyValueLine(line, key, val)) {
            continue;
        }
        
        if (key == "temperature") {
            bufTemp = static_cast<double>(std::stoi(val));
            tempFound = true;
            if (DEBUG) {
                std::cout << "find temperature key: " << key
                          << ", temperature value: " << bufTemp << "\n";
            }
        } else if (key == "percentage_used") {
            bufPTU = std::stoi(val);
            percentageUsedFound = true;
            if (DEBUG) {
                std::cout << "find percentage_used key: " << key
                          << ", percentage_used value: " << bufPTU << "\n";
            }
        } else if (key == "available_spare") {
            bufAS = std::stoi(val);
            availableSpareFound = true;
            if (DEBUG) {
                std::cout << "find available_spare key: " << key
                          << ", available_spare value: " << bufAS << "\n";
            }
        } else if (key == "media_errors") {
            bufME = static_cast<long long>(std::stoll(val));
            mediaErrorsFound = true;
            if (DEBUG) {
                std::cout << "find media_errors key: " << key
                          << ", media_errors value: " << bufME << "\n";
            }
        } else if (key == "critical_warning") {
            bufCW = std::stoi(val);
            criticalWarningFound = true;
            if (DEBUG) {
                std::cout << "find critical_warning key: " << key
                          << ", critical_warning value: " << bufCW << "\n";
            }
        } else {
            if (DEBUG) {
                std::cout << "No match Key: " << key << ", Value: " << val << "\n";
            }
        }
    }
    if (!tempFound && percentageUsedFound && availableSpareFound && mediaErrorsFound && criticalWarningFound) {
        if (DEBUG) std::cout << "Failed to find all required NVMe smart info keys.\n";
        return false;
    }
    temperatureC = bufTemp;
    percentageUsed = bufPTU;
    availableSpare = bufAS;
    mediaErrors = bufME;
    criticalWarning = bufCW;
    return true;
}

void readNvmeSmartInfoDirect(NvmeSmartInfo& nvmeInfo, const std::string& device) {
    if (DEBUG) std::cout << "Read NVMe smartInfo with nvme smart-log " << device << "\n";
    std::string cmd = "nvme smart-log " + device + " 2>/dev/null";
    std::string output = execCommand(cmd);
    if (DEBUG) std::cout << "NVMe smart-log output:\n" << output << "\n";

    if (output.empty()) {
        if (DEBUG) std::cout << "Failed to get NVMe smart-log output.\n";
        return;
    }
    nvmeInfo.ok = extractNvmeInfo(output, nvmeInfo.temperatureC, nvmeInfo.percentageUsed, nvmeInfo.availableSpare, nvmeInfo.mediaErrors, nvmeInfo.criticalWarning);
    if (DEBUG) std::cout << "Failed to extract NVMe smart info.\n";
}

void readNvmeSmartInfoCached(NvmeSmartInfo& nvmeInfo, const std::string& device) {
    std::time_t now = std::time(nullptr);
    if (DEBUG) std::cout << "Whether read NVMe smartInfo with ten minutes.\n";
    if (nvmeInfo.lastUpdate != 0 && (now - nvmeInfo.lastUpdate) < NVME_REFRESH_SECONDS) {
        if (DEBUG) std::cout << "Not yet time to update NVMe smartInfo.\n";
        return;
    }
    readNvmeSmartInfoDirect(nvmeInfo, device);
    nvmeInfo.lastUpdate = now;
}

SystemMetrics collectMetrics(const CpuTimes& prevCpu,
                             const CpuTimes& currCpu,
                             NvmeSmartInfo& nvmeInfo) {
    SystemMetrics m;
    m.timestamp = nowString();
    if (DEBUG) std::cout << "Calculate CPU usage\n";
    m.cpuPercent = calculateCpuPercent(prevCpu, currCpu);
    
    if (!readMemory(m.memoryPercent, m.memoryUsedMB, m.memoryAvailableMB)) {
        if (DEBUG) std::cout << "Failed to read /proc/meminfo \n";
        m.memoryPercent = 0.0;
        m.memoryUsedMB = 0.0;
        m.memoryAvailableMB = 0.0;
    }

    if (!readLoadAvg(m.load1, m.load5, m.load15)) {
        if (DEBUG) std::cout << "Failed to read /proc/loadavg \n";
        m.load1 = 0.0;
        m.load5 = 0.0;
        m.load15 = 0.0;
    }

    if (!readCpuTemperatureC(m.cpuTempC)) {
        if (DEBUG) std::cout << "Failed to read /sys/class/thermal/thermal_zone0/temp \n";
        m.cpuTempC = -1.0;
    }

    if (!readDiskUsageRoot(m.diskRootPercent, m.diskRootUsedGB, m.diskRootAvailGB)) {
        if (DEBUG) std::cout << "Failed to read /proc/diskstats \n";
        m.diskRootPercent = 0.0;
        m.diskRootUsedGB = 0.0;
        m.diskRootAvailGB = 0.0;
    }

    readNvmeSmartInfoCached(nvmeInfo, NVME_DEVICE);
    if (DEBUG) std::cout << "Update NVMe metrics.\n";
    m.nvmeTempC = nvmeInfo.temperatureC;
    m.nvmePercentageUsed = nvmeInfo.percentageUsed;
    m.nvmeAvailableSpare = nvmeInfo.availableSpare;
    m.nvmeMediaErrors = nvmeInfo.mediaErrors;
    m.nvmeCriticalWarning = nvmeInfo.criticalWarning;
    return m;
}

void appendCsvRow(const std::string& csvPath, const SystemMetrics& m) {
    if (DEBUG) std::cout << "Appending metrics to CSV.\n";
    std::ofstream ofs(csvPath, std::ios::app);
    ofs << m.timestamp << ","
        << std::fixed << std::setprecision(2)
        << m.cpuPercent << ","
        << m.memoryPercent << ","
        << m.memoryUsedMB << ","
        << m.memoryAvailableMB << ","
        << m.load1 << ","
        << m.load5 << ","
        << m.load15 << ","
        << m.cpuTempC << ","
        << m.diskRootPercent << ","
        << m.diskRootUsedGB << ","
        << m.diskRootAvailGB << ","
        << m.nvmeTempC << ","
        << m.nvmePercentageUsed << ","
        << m.nvmeAvailableSpare << ","
        << m.nvmeMediaErrors << ","
        << m.nvmeCriticalWarning
        << "\n";
}

void checkThresholdsAndLog(const SystemMetrics& m,
                           const ThresholdConfig& cfg,
                           const std::string& eventLogPath) {
    if (m.cpuPercent > cfg.cpuWarnPercent) {
        auto topCpu = getTopProcessesByCpu(3);
        std::vector<std::string> details;
        details.push_back("cpu=" + std::to_string(m.cpuPercent) + "%");
        for (size_t i = 0; i < topCpu.size(); ++i) {
            details.push_back("CPU_TOP" + std::to_string(i + 1) + " " + topCpu[i]);
        }
        writeEvent(eventLogPath, "CPU threshold exceeded", details);
    }

    if (m.memoryPercent > cfg.memWarnPercent) {
        auto topMem = getTopProcessesByMem(3);
        std::vector<std::string> details;
        details.push_back("mem=" + std::to_string(m.memoryPercent) + "%");
        for (size_t i = 0; i < topMem.size(); ++i) {
            details.push_back("MEM_TOP" + std::to_string(i + 1) + " " + topMem[i]);
        }
        writeEvent(eventLogPath, "Memory threshold exceeded", details);
    }

    if (m.cpuTempC >= 0.0 && m.cpuTempC > cfg.tempWarnC) {
        std::vector<std::string> details;
        details.push_back("cpu_temp_c=" + std::to_string(m.cpuTempC));
        writeEvent(eventLogPath, "CPU temperature threshold exceeded", details);
    }

    if (m.diskRootPercent > cfg.diskWarnPercent) {
        std::vector<std::string> details;
        details.push_back("disk_root_percent=" + std::to_string(m.diskRootPercent));
        details.push_back("disk_root_used_gb=" + std::to_string(m.diskRootUsedGB));
        details.push_back("disk_root_avail_gb=" + std::to_string(m.diskRootAvailGB));
        writeEvent(eventLogPath, "Disk threshold exceeded", details);
    }

    if (m.nvmeTempC >= 0.0 && m.nvmeTempC > cfg.nvmeTempWarnC) {
        std::vector<std::string> details;
        details.push_back("nvme_temp_c=" + std::to_string(m.nvmeTempC));
        writeEvent(eventLogPath, "NVMe temperature threshold exceeded", details);
    }

    if (m.nvmePercentageUsed >= 0 && m.nvmePercentageUsed > cfg.nvmePercentageUsedWarn) {
        std::vector<std::string> details;
        details.push_back("nvme_percentage_used=" + std::to_string(m.nvmePercentageUsed));
        writeEvent(eventLogPath, "NVMe lifetime usage threshold exceeded", details);
    }

    if (m.nvmeAvailableSpare >= 0 && m.nvmeAvailableSpare < cfg.nvmeAvailableSpareWarn) {
        std::vector<std::string> details;
        details.push_back("nvme_available_spare=" + std::to_string(m.nvmeAvailableSpare));
        writeEvent(eventLogPath, "NVMe available spare below threshold", details);
    }

    if (m.nvmeMediaErrors >= cfg.nvmeMediaErrorsWarn && m.nvmeMediaErrors >= 0) {
        std::vector<std::string> details;
        details.push_back("nvme_media_errors=" + std::to_string(m.nvmeMediaErrors));
        writeEvent(eventLogPath, "NVMe media errors detected", details);
    }

    if (m.nvmeCriticalWarning > 0) {
        std::vector<std::string> details;
        details.push_back("nvme_critical_warning=" + std::to_string(m.nvmeCriticalWarning));
        writeEvent(eventLogPath, "NVMe critical warning detected", details);
    }
}

bool loadThresholdConfig(const std::string& path, ThresholdConfig& cfg) {
    if (DEBUG) std::cout << "Loading threshold config from " << path << "\n";
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Config file not found, using default values.\n";
        return false;
    }

    std::string line;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        line = trim(line);
        size_t pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = trim(line.substr(0, pos));
        std::string val = trim(line.substr(pos + 1));

        try {
            if (key == "cpuWarnPercent") {
                if (DEBUG) std::cout << "Setting cpuWarnPercent to " << val << "\n";
                cfg.cpuWarnPercent = std::stod(val);
            } else if (key == "memWarnPercent") {
                if (DEBUG) std::cout << "Setting memWarnPercent to " << val << "\n";
                cfg.memWarnPercent = std::stod(val);
            } else if (key == "tempWarnC") {
                if (DEBUG) std::cout << "Setting tempWarnC to " << val << "\n";
                cfg.tempWarnC = std::stod(val);
            } else if (key == "diskWarnPercent") {
                if (DEBUG) std::cout << "Setting diskWarnPercent to " << val << "\n";
                cfg.diskWarnPercent = std::stod(val);
            } else if (key == "nvmeTempWarnC") {
                if (DEBUG) std::cout << "Setting nvmeTempWarnC to " << val << "\n";
                cfg.nvmeTempWarnC = std::stod(val);
            } else if (key == "nvmePercentageUsedWarn") {
                if (DEBUG) std::cout << "Setting nvmePercentageUsedWarn to " << val << "\n";
                cfg.nvmePercentageUsedWarn = std::stoi(val);
            } else if (key == "nvmeAvailableSpareWarn") {
                if (DEBUG) std::cout << "Setting nvmeAvailableSpareWarn to " << val << "\n";
                cfg.nvmeAvailableSpareWarn = std::stoi(val);
            } else if (key == "nvmeMediaErrorsWarn") {
                if (DEBUG) std::cout << "Setting nvmeMediaErrorsWarn to " << val << "\n";
                cfg.nvmeMediaErrorsWarn = std::stoll(val);
            } else {
                std::cerr << "Unknown config key: " << key << "\n";
            }
        } catch (...) {
            std::cerr << "Invalid value for key: " << key << "\n";
        }
    }

    return true;
}

int main() {
    if (!ensureDirectory(DATA_DIR)) {
        std::cerr << "Failed to create data directory: " << DATA_DIR << "\n";
        return 1;
    }

    if (!ensureDirectory(LOG_DIR)) {
        std::cerr << "Failed to create log directory: " << LOG_DIR << "\n";
        return 1;
    }

    ensureCsvHeader(CSV_PATH);

    ThresholdConfig cfg;
    NvmeSmartInfo nvmeInfo;

    CpuTimes prevCpu {};
    CpuTimes currCpu {};

    loadThresholdConfig(CONFIG_PATH, cfg);

    if (!readCpuTimes(prevCpu)) {
        if (DEBUG) std::cout << "Failed to read /proc/stat at startup\n";
        appendLog(LOG_PATH, "Failed to read /proc/stat at startup");
        return 1;
    }

    appendLog(LOG_PATH, "Collector started");
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        if (!readCpuTimes(currCpu)) {
            if (DEBUG) std::cout << "Failed to read /proc/stat\n";
            appendLog(LOG_PATH, "Failed to read /proc/stat");
            std::this_thread::sleep_for(std::chrono::seconds(59));
            continue;
        }

        SystemMetrics metrics = collectMetrics(prevCpu, currCpu, nvmeInfo);

        appendCsvRow(CSV_PATH, metrics);
        checkThresholdsAndLog(metrics, cfg, EVENT_LOG_PATH);

        prevCpu = currCpu;

        std::this_thread::sleep_for(std::chrono::seconds(59));
    }

    return 0;
}
