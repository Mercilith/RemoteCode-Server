#include "ActivityLog.h"

#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::wstring TodayStamp() {
    time_t t = time(nullptr);
    tm utc{};
    gmtime_s(&utc, &t);
    wchar_t buffer[16];
    wcsftime(buffer, 16, L"%Y-%m-%d", &utc);
    return buffer;
}

// source_ is always an ASCII identifier ("orchestrator", "mcp-<agent-id>",
// and agent ids are already Slugify()'d) — a byte-for-byte widen is exact.
std::wstring WidenAscii(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

} // namespace

ActivityLog::ActivityLog(std::wstring logDir, std::string source)
    : logDir_(std::move(logDir)), source_(std::move(source)) {
    std::error_code ec;
    fs::create_directories(logDir_, ec);
}

void ActivityLog::Log(
    const std::string& chatId, const std::string& agentId, const std::string& eventType, const json& detail) {
    json entry;
    entry["ts"] = static_cast<int64_t>(time(nullptr));
    entry["source"] = source_;
    entry["chatId"] = chatId;
    entry["agentId"] = agentId;
    entry["eventType"] = eventType;
    if (!detail.is_null()) {
        entry["detail"] = detail;
    }

    const std::wstring path = logDir_ + L"\\activity-" + TodayStamp() + L"." + WidenAscii(source_) + L".jsonl";

    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream file(path.c_str(), std::ios::app);
    if (file.is_open()) {
        file << entry.dump() << "\n";
    }
}
