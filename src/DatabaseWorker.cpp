#include "ScoreTransfer.hpp"

#include <sqlite3.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace ScoreTransfer {
namespace {
constexpr uintptr_t kPNetObjectAddress = 0x008986A0;
constexpr uintptr_t kSceneIdAddress = 0x008A0044;
constexpr uintptr_t kPreferredImageBase = 0x00400000;
constexpr uintptr_t kGameDevicePointerRva = 0x008A0E30 - kPreferredImageBase;
constexpr uintptr_t kLeftProfileOffset = 0x04;
constexpr uintptr_t kRightProfileOffset = 0x24;
constexpr size_t kProfileSize = 0x20;
constexpr long long kActiveRunMaxAgeSeconds = 30LL * 60LL;

struct MatchRecord {
    std::string timestampText;
    std::string p1Name;
    std::string p2Name;
    int p1Rounds = 0;
    int p2Rounds = 0;
    bool timestampValid = false;
    bool fileTimeTimestamp = false;
    long long epochSeconds = 0;
    long long freshnessEpochSeconds = 0;
};

struct LatestRun {
    bool exists = false;
    bool participantsConsistent = true;
    bool endTimeValid = false;
    std::string p1Name;
    std::string p2Name;
    int p1Wins = 0;
    int p2Wins = 0;
    long long endEpochSeconds = 0;
};

struct FileFingerprint {
    bool valid = false;
    uint64_t size = 0;
    FILETIME writeTime{};
    bool operator==(const FileFingerprint& other) const {
        return valid == other.valid && (!valid ||
            (size == other.size && writeTime.dwLowDateTime == other.writeTime.dwLowDateTime &&
             writeTime.dwHighDateTime == other.writeTime.dwHighDateTime));
    }
};

bool SafeReadMemory(uintptr_t address, void* destination, size_t size)
{
    if (address < 0x10000 || !destination || size == 0)
        return false;
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address),
        destination, size, &bytesRead) != FALSE && bytesRead == size;
}

bool BuildLocalEpoch(int year, int month, int day, int hour, int minute, int second, long long& epoch)
{
    if (year < 1970 || year > 2200 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
        return false;
    std::tm local{};
    local.tm_year = year - 1900;
    local.tm_mon = month - 1;
    local.tm_mday = day;
    local.tm_hour = hour;
    local.tm_min = minute;
    local.tm_sec = second;
    local.tm_isdst = -1;
    const std::time_t value = std::mktime(&local);
    if (value == static_cast<std::time_t>(-1))
        return false;
    std::tm verify{};
    if (localtime_s(&verify, &value) != 0 || verify.tm_year != year - 1900 ||
        verify.tm_mon != month - 1 || verify.tm_mday != day || verify.tm_hour != hour ||
        verify.tm_min != minute || verify.tm_sec != second)
        return false;
    epoch = static_cast<long long>(value);
    return true;
}

bool ParseTimestampText(const std::string& text, long long& epoch)
{
    int year=0, month=0, day=0, hour=0, minute=0, second=0;
    if (sscanf_s(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day,
                 &hour, &minute, &second) == 6 ||
        sscanf_s(text.c_str(), "%d/%d/%d %d:%d:%d", &year, &month, &day,
                 &hour, &minute, &second) == 6) {
        return BuildLocalEpoch(year, month, day, hour, minute, second, epoch);
    }

    // Numeric timestamps may be stored as INTEGER or TEXT in Default.db.
    if (!text.empty()) {
        bool digitsOnly = true;
        for (unsigned char ch : text) {
            if (ch < '0' || ch > '9') { digitsOnly = false; break; }
        }
        if (digitsOnly) {
            try {
                const long long value = std::stoll(text);
                if (value >= 1000000000LL && value <= 9999999999LL) {
                    epoch = value;
                    return true;
                }
                if (value >= 1000000000000LL && value <= 9999999999999LL) {
                    epoch = value / 1000LL;
                    return true;
                }
                if (value >= 116444736000000000LL && value <= 200000000000000000LL) {
                    epoch = value / 10000000LL - 11644473600LL;
                    return true;
                }
                if (value >= 19700101000000LL && value <= 22000101000000LL) {
                    long long v = value;
                    second = static_cast<int>(v % 100); v /= 100;
                    minute = static_cast<int>(v % 100); v /= 100;
                    hour = static_cast<int>(v % 100); v /= 100;
                    day = static_cast<int>(v % 100); v /= 100;
                    month = static_cast<int>(v % 100); v /= 100;
                    year = static_cast<int>(v);
                    return BuildLocalEpoch(year, month, day, hour, minute, second, epoch);
                }
            } catch (...) {
                return false;
            }
        }
    }
    return false;
}

std::string BlobString(sqlite3_stmt* statement, int column)
{
    const auto* bytes = static_cast<const unsigned char*>(sqlite3_column_blob(statement, column));
    int length = sqlite3_column_bytes(statement, column);
    if (!bytes || length <= 0)
        return {};
    int actual = length;
    for (int i = 0; i < length; ++i) {
        if (bytes[i] == 0) { actual = i; break; }
    }
    while (actual > 0 && (bytes[actual-1] == ' ' || bytes[actual-1] == '\t' ||
           bytes[actual-1] == '\r' || bytes[actual-1] == '\n'))
        --actual;
    return std::string(reinterpret_cast<const char*>(bytes), static_cast<size_t>(actual));
}

std::string NormalizeProfile(std::string value)
{
    size_t first = 0, last = value.size();
    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (first < last && isSpace(static_cast<unsigned char>(value[first]))) ++first;
    while (last > first && isSpace(static_cast<unsigned char>(value[last - 1]))) --last;
    return value.substr(first, last - first);
}

bool ReadMatch(sqlite3_stmt* statement, MatchRecord& record)
{
    const int originalType = sqlite3_column_type(statement, 0);
    const long long integerValue = (originalType == SQLITE_INTEGER)
        ? sqlite3_column_int64(statement, 0) : 0LL;
    const unsigned char* timestamp = sqlite3_column_text(statement, 0);
    if (timestamp)
        record.timestampText = reinterpret_cast<const char*>(timestamp);
    else if (originalType == SQLITE_INTEGER)
        record.timestampText = std::to_string(integerValue);

    record.timestampValid = ParseTimestampText(record.timestampText, record.epochSeconds);
    if (!record.timestampValid && originalType == SQLITE_INTEGER) {
        record.timestampValid = ParseTimestampText(std::to_string(integerValue), record.epochSeconds);
    }
    if (record.timestampValid) {
        record.freshnessEpochSeconds = record.epochSeconds;
        // Match OIOverlay/TskDatabase's existing OIV freshness normalization for FILETIME rows.
        if (integerValue >= 116444736000000000LL && integerValue <= 200000000000000000LL) {
            record.fileTimeTimestamp = true;
            record.freshnessEpochSeconds -= 9LL * 60LL * 60LL;
        } else if (!record.timestampText.empty()) {
            try {
                const long long value = std::stoll(record.timestampText);
                if (value >= 116444736000000000LL && value <= 200000000000000000LL) {
                    record.fileTimeTimestamp = true;
                    record.freshnessEpochSeconds -= 9LL * 60LL * 60LL;
                }
            } catch (...) {}
        }
    }

    record.p1Name = NormalizeProfile(BlobString(statement, 1));
    record.p1Rounds = sqlite3_column_int(statement, 2);
    record.p2Name = NormalizeProfile(BlobString(statement, 3));
    record.p2Rounds = sqlite3_column_int(statement, 4);
    return true;
}

bool EarlierRecord(const MatchRecord& a, const MatchRecord& b)
{
    if (a.timestampValid && b.timestampValid)
        return a.epochSeconds < b.epochSeconds;
    if (a.timestampValid != b.timestampValid)
        return a.timestampValid; // Unparseable records sort last, forcing a safe 0/0 result if newest.
    return a.timestampText < b.timestampText;
}

std::vector<LatestRun> BuildRuns(std::vector<MatchRecord>& records)
{
    std::vector<LatestRun> runs;
    if (records.empty())
        return runs;
    std::stable_sort(records.begin(), records.end(), EarlierRecord);

    LatestRun current{};
    std::string currentP2;
    bool hasCurrent = false;
    for (const auto& match : records) {
        const std::string p1 = NormalizeProfile(match.p1Name);
        const std::string p2 = NormalizeProfile(match.p2Name);
        if (!hasCurrent || p2 != currentP2) {
            if (hasCurrent)
                runs.push_back(current);
            current = {};
            current.exists = true;
            current.participantsConsistent = true;
            current.p1Name = p1;
            current.p2Name = p2;
            currentP2 = p2;
            hasCurrent = true;
        }
        if (p1.empty() || p2.empty())
            current.participantsConsistent = false;
        if (current.p1Name != p1 || current.p2Name != p2)
            current.participantsConsistent = false;
        current.endTimeValid = match.timestampValid;
        current.endEpochSeconds = match.freshnessEpochSeconds;
        if (match.p1Rounds >= 2)
            ++current.p1Wins;
        else if (match.p2Rounds >= 2)
            ++current.p2Wins;
    }
    if (hasCurrent)
        runs.push_back(current);
    return runs; // oldest first, like the chronological OIV source table
}

std::wstring GameDirectory()
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    std::wstring result(path, length);
    const size_t slash = result.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return result.substr(0, slash);
}

bool IsFile(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring FindDefaultDatabase()
{
    const std::wstring root = GameDirectory();
    if (root.empty()) return {};
    const std::wstring nested = root + L"\\th4_5888\\Default.db";
    if (IsFile(nested)) return nested;
    const std::wstring rootDb = root + L"\\Default.db";
    if (IsFile(rootDb)) return rootDb;
    return {};
}

FileFingerprint GetFingerprint(const std::wstring& path)
{
    FileFingerprint result{};
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        ULARGE_INTEGER size{};
        size.LowPart = data.nFileSizeLow;
        size.HighPart = data.nFileSizeHigh;
        result.valid = true;
        result.size = size.QuadPart;
        result.writeTime = data.ftLastWriteTime;
    }
    return result;
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        result.data(), count, nullptr, nullptr) != count) return {};
    return result;
}

bool LoadRunsFromDb(const std::wstring& path, std::vector<LatestRun>& runs)
{
    runs.clear();
    const std::string utf8Path = WideToUtf8(path);
    if (utf8Path.empty()) return false;
    sqlite3* db = nullptr;
    const int openResult = sqlite3_open_v2(utf8Path.c_str(), &db,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (openResult != SQLITE_OK || !db) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_busy_timeout(db, 200);
    constexpr const char* sql =
        "SELECT timestamp, CAST(p1name AS BLOB), p1win, CAST(p2name AS BLOB), p2win "
        "FROM trackrecord123 ORDER BY timestamp ASC";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) != SQLITE_OK || !statement) {
        if (statement) sqlite3_finalize(statement);
        sqlite3_close(db);
        return false;
    }
    std::vector<MatchRecord> records;
    int step = SQLITE_OK;
    while ((step = sqlite3_step(statement)) == SQLITE_ROW) {
        MatchRecord record{};
        if (!ReadMatch(statement, record)) {
            sqlite3_finalize(statement); sqlite3_close(db); return false;
        }
        records.emplace_back(std::move(record));
    }
    sqlite3_finalize(statement);
    sqlite3_close(db);
    if (step != SQLITE_DONE)
        return false;
    runs = BuildRuns(records);
    return true; // An empty but successfully read table is valid; its visible value is 0/0.
}

bool TryReadScene(int& scene)
{
    int value = -1;
    if (!SafeReadMemory(kSceneIdAddress, &value, sizeof(value)) || value < 0 || value > 31)
        return false;
    scene = value;
    return true;
}

std::string ReadProfile(uintptr_t offset)
{
    uint32_t netObject = 0;
    if (!SafeReadMemory(kPNetObjectAddress, &netObject, sizeof(netObject)) ||
        netObject < 0x10000 || netObject > 0x7FFF0000u)
        return {};
    char buffer[kProfileSize + 1]{};
    if (!SafeReadMemory(static_cast<uintptr_t>(netObject) + offset, buffer, kProfileSize))
        return {};
    buffer[kProfileSize] = '\0';
    return NormalizeProfile(std::string(buffer));
}

bool GetGameDevice(IDirect3DDevice9** output)
{
    if (!output) return false;
    *output = nullptr;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t pointerAddress = base ? base + kGameDevicePointerRva : 0;
    return SafeReadMemory(pointerAddress, output, sizeof(*output)) && *output != nullptr;
}

enum class ConnectionRole { Unknown, Host, Client };

void PublishForProfiles(const std::vector<LatestRun>& runs, int scene,
    const std::string& left, const std::string& right, ConnectionRole role)
{
    int leftWins = 0;
    int rightWins = 0;
    if (!IsDisplayScene(scene) || left.empty() || right.empty() || left == right) {
        g_leftWins.store(0, std::memory_order_release);
        g_rightWins.store(0, std::memory_order_release);
        return;
    }

    // Match OIOverlay's connection-role rule: scene 9 means client; the other
    // character-select scenes default to host when no client role was observed.
    const std::string& opponentProfile = (role == ConnectionRole::Client) ? left : right;
    const LatestRun* selected = nullptr;
    for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
        if (it->exists && it->p2Name == opponentProfile) {
            selected = &*it;
            break; // newest run for this opponent; never fall back to older data
        }
    }

    if (selected && selected->participantsConsistent && selected->endTimeValid &&
        !selected->p1Name.empty() && !selected->p2Name.empty() &&
        selected->p1Name != selected->p2Name) {
        const long long age = static_cast<long long>(std::time(nullptr)) - selected->endEpochSeconds;
        if (age >= 0 && age <= kActiveRunMaxAgeSeconds) {
            if (left == selected->p1Name && right == selected->p2Name) {
                leftWins = selected->p1Wins;
                rightWins = selected->p2Wins;
            } else if (left == selected->p2Name && right == selected->p1Name) {
                leftWins = selected->p2Wins;
                rightWins = selected->p1Wins;
            }
        }
    }
    g_leftWins.store(leftWins, std::memory_order_release);
    g_rightWins.store(rightWins, std::memory_order_release);
}

} // namespace

DWORD WINAPI WorkerThreadEntry(void*)
{
    OutputDebugStringW(L"[ScoreTransfer] Worker thread started.\n");
    std::vector<LatestRun> cachedRuns;
    bool hasLoadedDb = false;
    std::wstring loadedPath;
    FileFingerprint loadedFingerprint{};
    ULONGLONG nextDbCheck = 0;
    ULONGLONG nextHookAttempt = 0;
    IDirect3DDevice9* device = nullptr;
    int previousScene = -1;
    ConnectionRole connectionRole = ConnectionRole::Unknown;

    while (!g_stopRequested.load(std::memory_order_acquire)) {
        const ULONGLONG nowTick = GetTickCount64();
        int scene = -1;
        if (TryReadScene(scene)) {
            g_sceneId.store(scene, std::memory_order_release);
            if (scene != previousScene) {
                if (scene <= 7 || scene == 12 || scene == 15)
                    connectionRole = ConnectionRole::Unknown;
                else if (scene == 9)
                    connectionRole = ConnectionRole::Client;
                else if ((scene == 8 || scene == 10 || scene == 11) && connectionRole == ConnectionRole::Unknown)
                    connectionRole = ConnectionRole::Host;
                else if ((scene == 13 || scene == 14) && connectionRole == ConnectionRole::Unknown)
                    connectionRole = ConnectionRole::Host;
                previousScene = scene;
            }
        } else {
            g_sceneId.store(-1, std::memory_order_release);
        }

        const std::string left = ReadProfile(kLeftProfileOffset);
        const std::string right = ReadProfile(kRightProfileOffset);
        if (nowTick >= nextDbCheck) {
            nextDbCheck = nowTick + 1000;
            const std::wstring path = FindDefaultDatabase();
            const FileFingerprint currentFingerprint = GetFingerprint(path);
            const bool dbChanged = path != loadedPath || !(currentFingerprint == loadedFingerprint);
            if (path.empty()) {
                cachedRuns.clear();
                hasLoadedDb = false;
                loadedPath.clear();
                loadedFingerprint = {};
            } else if (dbChanged || !hasLoadedDb) {
                std::vector<LatestRun> freshRuns;
                if (currentFingerprint.valid && LoadRunsFromDb(path, freshRuns)) {
                    cachedRuns = std::move(freshRuns);
                    hasLoadedDb = true;
                    loadedPath = path;
                    loadedFingerprint = currentFingerprint;
                    OutputDebugStringW(L"[ScoreTransfer] Default.db loaded (read-only).\n");
                } else {
                    cachedRuns.clear();
                    hasLoadedDb = false; // Retry on the next poll; never reuse stale or other-profile counts.
                    loadedPath = path;
                    loadedFingerprint = currentFingerprint;
                }
            }
        }

        PublishForProfiles(hasLoadedDb ? cachedRuns : std::vector<LatestRun>{},
            g_sceneId.load(std::memory_order_acquire), left, right, connectionRole);

        // Poll the game-owned device pointer but don't race Soku/OIOverlay's setup hook.
        IDirect3DDevice9* observedDevice = nullptr;
        if (GetGameDevice(&observedDevice) && observedDevice) {
            if (observedDevice != device) {
                device = observedDevice;
                nextHookAttempt = nowTick + 1250;
            } else if (nowTick >= nextHookAttempt) {
                if (TryInstallD3D9Hooks(device)) {
                    nextHookAttempt = UINT64_MAX;
                } else {
                    nextHookAttempt = nowTick + 1500;
                }
            }
        }
        Sleep(50);
    }
    return 0;
}

} // namespace ScoreTransfer
