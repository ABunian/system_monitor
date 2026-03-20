/*
    V001: read current.csv and plot, then report summary.
    V002: Add send plot and summary message that used by telegram.
          Add mode 1= trigger, can't backup and clean current_CSV.
              mode 0= daily report, will backup and clean current CSV.
	V003: Load telegram.conf to get bot_token and chat_id.
    V004: Change to read collector.csv
*/

#include "common.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <curl/curl.h>

struct TelegramConfig {
    std::string botToken;
    std::string chatId;
};

static const std::string DATA_DIR = "./data";
static const std::string BACKUP_DIR = "./data/backup";
static const std::string REPORT_DIR = "./data/reports";
static const std::string PLOT_DIR = "./data/plots";
static const std::string LOG_DIR = "./logs";

static const std::string CURRENT_CSV = "./data/collector.csv";
static const std::string LOG_PATH = "./logs/reporter.log";

bool DEBUG = false;

TelegramConfig loadTelegramConfig(const std::string& path) {
    TelegramConfig cfg;

    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open telegram config: " + path);
    }

    std::string line;

    while (std::getline(file, line)) {
        if (line.empty())
            continue;

        auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);

        if (key == "BOT_TOKEN")
            cfg.botToken = value;
        else if (key == "CHAT_ID")
            cfg.chatId = value;
    }

    if (cfg.botToken.empty() || cfg.chatId.empty())
        throw std::runtime_error("telegram.conf missing BOT_TOKEN or CHAT_ID");

    return cfg;
}

std::vector<SystemMetrics> readSamples() {
    std::vector<SystemMetrics> samples;
    std::ifstream ifs(CURRENT_CSV);
    if (!ifs.is_open()) return samples;

    std::string line;
    std::getline(ifs, line); // header

    while (std::getline(ifs, line)) {
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string token;
        SystemMetrics s;

        std::getline(ss, s.timestamp, ',');

        std::getline(ss, token, ',');
        s.cpuPercent = std::stod(token);
        if (DEBUG) std::cout << "Parsed CPU percent token: '" << s.cpuPercent << "'\n";

        std::getline(ss, token, ',');
        s.memoryPercent = std::stod(token);
        if (DEBUG) std::cout << "Parsed Memory percent token: '" << s.memoryPercent << "'\n";

        std::getline(ss, token, ',');
        s.memoryUsedMB = std::stod(token);
        if (DEBUG) std::cout << "Parsed Memory used MB token: '" << s.memoryUsedMB << "'\n";

        std::getline(ss, token, ',');
        s.memoryAvailableMB = std::stod(token);
        if (DEBUG) std::cout << "Parsed Memory available MB token: '" << s.memoryAvailableMB << "'\n";

        std::getline(ss, token, ',');
        s.load1 = std::stod(token);
        if (DEBUG) std::cout << "Parsed Load 1 token: '" << s.load1 << "'\n";

        std::getline(ss, token, ',');
        s.load5 = std::stod(token);
        if (DEBUG) std::cout << "Parsed Load 5 token: '" << s.load5 << "'\n";

        std::getline(ss, token, ',');
        s.load15 = std::stod(token);
        if (DEBUG) std::cout << "Parsed Load 15 token: '" << s.load15 << "'\n";

        std::getline(ss, token, ',');
        s.cpuTempC = std::stod(token);
        if (DEBUG) std::cout << "Parsed CPU Temp C token: '" << s.cpuTempC << "'\n";

        std::getline(ss, token, ',');
        s.diskRootPercent = std::stod(token);
        if (DEBUG) std::cout << "Parsed Disk Root Percent token: '" << s.diskRootPercent << "'\n";

        std::getline(ss, token, ',');
        s.diskRootUsedGB = std::stod(token);
        if (DEBUG) std::cout << "Parsed Disk Root Used GB token: '" << s.diskRootUsedGB << "'\n";

        std::getline(ss, token, ',');
        s.diskRootAvailGB = std::stod(token);
        if (DEBUG) std::cout << "Parsed Disk Root Available GB token: '" << s.diskRootAvailGB << "'\n";

        std::getline(ss, token, ',');
        s.nvmeTempC = std::stod(token);
        if (DEBUG) std::cout << "Parsed NVMe Temp C token: '" << s.nvmeTempC << "'\n";

        std::getline(ss, token, ',');
        s.nvmePercentageUsed = std::stoi(token);
        if (DEBUG) std::cout << "Parsed NVMe Percentage Used token: '" << s.nvmePercentageUsed << "'\n";

        std::getline(ss, token, ',');
        s.nvmeAvailableSpare = std::stoi(token);
        if (DEBUG) std::cout << "Parsed NVMe Available Spare token: '" << s.nvmeAvailableSpare << "'\n";

        std::getline(ss, token, ',');
        s.nvmeMediaErrors = std::stoll(token);
        if (DEBUG) std::cout << "Parsed NVMe Media Errors token: '" << s.nvmeMediaErrors << "'\n";

        std::getline(ss, token, ',');
        s.nvmeCriticalWarning = std::stoi(token);
        if (DEBUG) std::cout << "Parsed NVMe Critical Warning token: '" << s.nvmeCriticalWarning << "'\n";

        samples.push_back(s);
    }

    return samples;
}

double avgOf(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (double x : v) sum += x;
    return sum / v.size();
}

double minOf(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double m = v[0];
    for (double x : v) {
        if (x < m) m = x;
    }
    return m;
}

double maxOf(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double m = v[0];
    for (double x : v) {
        if (x > m) m = x;
    }
    return m;
}

std::string makeBackupPath() {
    return BACKUP_DIR + "/system_usage_" + dateString() + ".csv";
}

std::string makeSummaryPath() {
    return REPORT_DIR + "/summary_" + dateString() + ".csv";
}

std::string makePlotPath() {
    return PLOT_DIR + "/system_usage_" + dateString() + ".png";
}

std::string makeGnuplotScriptPath() {
    return PLOT_DIR + "/plot_" + dateString() + ".gnuplot";
}

bool backupCurrentCsv(const std::string& backupPath) {
    std::ifstream src(CURRENT_CSV, std::ios::binary);
    if (!src.is_open()) return false;

    std::ofstream dst(backupPath, std::ios::binary);
    if (!dst.is_open()) return false;

    dst << src.rdbuf();
    return true;
}

void writeSummary(const std::vector<SystemMetrics>& samples, const std::string& summaryPath) {
    std::vector<double> cpu;
    std::vector<double> mem;
    std::vector<double> disk;

    for (const auto& s : samples) {
        cpu.push_back(s.cpuPercent);
        mem.push_back(s.memoryPercent);
        disk.push_back(s.diskRootPercent);
    }

    std::ofstream ofs(summaryPath);
    ofs << "metric,value\n";
    ofs << "cpu_avg_percent," << avgOf(cpu) << "\n";
    ofs << "cpu_max_percent," << maxOf(cpu) << "\n";
    ofs << "cpu_min_percent," << minOf(cpu) << "\n";
    ofs << "memory_avg_percent," << avgOf(mem) << "\n";
    ofs << "memory_max_percent," << maxOf(mem) << "\n";
    ofs << "memory_min_percent," << minOf(mem) << "\n";
    ofs << "disk_avg_percent," << avgOf(disk) << "\n";
    ofs << "disk_max_percent," << maxOf(disk) << "\n";
    ofs << "disk_min_percent," << minOf(disk) << "\n";
    ofs << "samples," << samples.size() << "\n";
}

void generatePlot(const std::string& inputCsv, const std::string& outputPng) {
    std::string gpPath = makeGnuplotScriptPath();
    std::string dateStr = dateString();

    std::ofstream gp(gpPath);
    gp << "set terminal png size 1400,700\n";
    gp << "set output '" << outputPng << "'\n";
    gp << "set datafile separator ','\n";
    gp << "set xdata time\n";
    gp << "set timefmt '%Y-%m-%d %H:%M:%S'\n";
    gp << "set format x '%H:%M'\n";
    gp << "set title 'Daily CPU / Memory Usage - " << dateStr << "'\n";
    gp << "set xlabel 'Time'\n";
    gp << "set ylabel 'Usage (%)'\n";
    gp << "set grid\n";
    gp << "plot '" << inputCsv << "' using 1:2 with lines title 'CPU Usage (%)',\\\n";
    gp << "     '" << inputCsv << "' using 1:3 with lines title 'Memory Usage (%)'\n";
    gp.close();

    std::string cmd = "gnuplot " + gpPath;
    std::system(cmd.c_str());
}

void clearCurrentCsv() {
    std::ofstream ofs(CURRENT_CSV, std::ios::trunc);
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

bool sendTelegramMessage(const TelegramConfig& cfg, const std::string& text) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    appendLog(LOG_PATH, "----------------------------------");
	appendLog(LOG_PATH, "Send Message: " + text);

    std::string url = "https://api.telegram.org/bot" + cfg.botToken + "/sendMessage";
    std::string postFields = "chat_id=" + cfg.chatId + "&text=" + text;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.c_str());

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    appendLog(LOG_PATH, "----------------------------------\n");
    return (res == CURLE_OK);
}

bool sendTelegramFile(const TelegramConfig& cfg, 
                      const std::string& apiMethod,
                      const std::string& fieldName,
                      const std::string& filePath,
                      const std::string& caption) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    appendLog(LOG_PATH, "----------------------------------");
	appendLog(LOG_PATH, "Send " + fieldName + ": " + filePath);

    std::string url = "https://api.telegram.org/bot" + cfg.botToken + "/" + apiMethod;

    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = nullptr;

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "chat_id");
    curl_mime_data(part, cfg.chatId.c_str(), CURL_ZERO_TERMINATED);
	
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "caption");
    curl_mime_data(part, caption.c_str(), CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, fieldName.c_str());
    curl_mime_filedata(part, filePath.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);

    CURLcode res = curl_easy_perform(curl);

    curl_mime_free(mime);
    curl_easy_cleanup(curl);

	appendLog(LOG_PATH, "----------------------------------\n");
    return (res == CURLE_OK);
}

bool sendTelegramPhoto(const TelegramConfig& cfg, const std::string& filePath, const std::string& caption) {
    return sendTelegramFile(cfg, "sendPhoto", "photo", filePath, caption);
}

bool sendTelegramDocument(const TelegramConfig& cfg, const std::string& filePath, const std::string& caption) {
    return sendTelegramFile(cfg, "sendDocument", "document", filePath, caption);
}

int main(int argc, char* argv[]) {
    ensureDir(DATA_DIR);
    ensureDir(BACKUP_DIR);
    ensureDir(REPORT_DIR);
    ensureDir(PLOT_DIR);
    ensureDir(LOG_DIR);

    if (argc < 2) {
        appendLog(LOG_PATH, "missing mode argument. usage: ./reporter <0|1>");
        std::cerr << "Usage: " << argv[0] << " <0|1>\n";
        std::cerr << "  1 = trigger mode (no backup, no clear collector.csv)\n";
        std::cerr << "  0 = daily report mode (backup and clear collector.csv)\n";
        return 1;
    }
    std::string modeArg = argv[1];
    bool isTriggerMode = false;
    bool isDailyMode = false;

    if (modeArg == "1") {
        isTriggerMode = true;
    } else if (modeArg == "0") {
        isDailyMode = true;
    } else {
        appendLog(LOG_PATH, "invalid mode argument: " + modeArg);
        std::cerr << "Invalid mode: " << modeArg << "\n";
        std::cerr << "Use 1 for trigger mode, 0 for daily report mode.\n";
        return 1;
    }

    std::string modeName = isTriggerMode ? "trigger" : "daily";
    appendLog(LOG_PATH, "daily report started, mode=" + modeName);
    

    if (!fileExists(CURRENT_CSV)) {
        appendLog(LOG_PATH, "collector.csv not found, nothing to do");
        return 1;
    }

    auto samples = readSamples();
    if (samples.empty()) {
        appendLog(LOG_PATH, "no sample rows in collector.csv");
        if (isDailyMode) {
            clearCurrentCsv();
            appendLog(LOG_PATH, "collector.csv header reset in daily mode");
        }
        return 1;
    }

    std::string inputCsvForPlot;
    std::string backupPath;
    std::string summaryPath = makeSummaryPath();
    std::string plotPath = makePlotPath();

    if (isDailyMode) {
        backupPath = makeBackupPath();

        if (!backupCurrentCsv(backupPath)) {
            appendLog(LOG_PATH, "failed to backup collector.csv");
            return 1;
        }

        inputCsvForPlot = backupPath;
        appendLog(LOG_PATH, "backup created: " + backupPath);
    } else {
        inputCsvForPlot = CURRENT_CSV;
        appendLog(LOG_PATH, "trigger mode: skip backup and keep collector.csv");
    }   

    writeSummary(samples, summaryPath);
    generatePlot(inputCsvForPlot, plotPath);

    std::vector<double> cpu;
    std::vector<double> mem;
    for (const auto& s : samples) {
        cpu.push_back(s.cpuPercent);
        mem.push_back(s.memoryPercent);
    }

    std::ostringstream msg;
    msg << "系統資源日報\n"
        << "日期: " << dateString() << "\n"
        << "樣本數: " << samples.size() << "\n"
        << std::fixed << std::setprecision(2)
        << "CPU 平均: " << avgOf(cpu) << "%\n"
        << "CPU 最大: " << maxOf(cpu) << "%\n"
        << "RAM 平均: " << avgOf(mem) << "%\n"
        << "RAM 最大: " << maxOf(mem) << "%";

	TelegramConfig tg;
    try {
        tg = loadTelegramConfig("./config/telegram.conf");
    } catch(const std::exception& e) {
        appendLog(LOG_PATH, e.what());
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    bool okMsg = sendTelegramMessage(tg, msg.str());
    bool okPhoto = sendTelegramPhoto(tg, plotPath, "CPU / RAM 每日趨勢圖");
    // bool okSummary = sendTelegramDocument(tg, summaryPath, "每日統計 summary CSV");
    // bool okBackup = sendTelegramDocument(tg, backupPath, "每日原始備份 CSV");

    curl_global_cleanup();

    std::ostringstream tglog;
    tglog << "telegram send result: "
          << "message=" << (okMsg ? "ok" : "fail") << ", "
          << "photo=" << (okPhoto ? "ok" : "fail") << ", ";
    //       << "summary=" << (okSummary ? "ok" : "fail") << ", ";
    // if (isDailyMode) {
    //     tglog << "backup=" << (okBackup ? "ok" : "fail");
    // } else {
    //     tglog << "backup=skipped";
    // }
    appendLog(LOG_PATH, tglog.str());

    if (isDailyMode) {
        clearCurrentCsv();
        appendLog(LOG_PATH, "collector.csv cleared for daily mode");
    } else {
        appendLog(LOG_PATH, "trigger mode finished, collector.csv preserved");
    }

    std::ostringstream done;
    done << "daily report done. backup=" << backupPath
         << " summary=" << summaryPath
         << " plot=" << plotPath
         << " rows=" << samples.size();
    if (isDailyMode) {
        done << " backup=" << backupPath;
    }
    appendLog(LOG_PATH, done.str());

    return 0;
}
