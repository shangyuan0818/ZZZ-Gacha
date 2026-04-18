// ============================================================
// ZZZ Gacha Exporter - 面向数据 / PMR 栈分配 / UIGF v4.2
// ============================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <ctime>
#include <windows.h>
#include <winhttp.h>
#include <string_view>
#include <charconv>
#include <ranges>
#include <memory_resource>
#include <array>
#include <numeric>
#include <unordered_set>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "User32.lib")

// ---------------------------------------------------------
// [极简 JSON 解析模块 - 修复边界]
// ---------------------------------------------------------
inline size_t FindJsonKey(std::string_view source, std::string_view key, size_t startPos = 0) {
    while (true) {
        size_t pos = source.find(key, startPos);
        if (pos == std::string_view::npos) return std::string_view::npos;
        if (pos > 0 && source[pos - 1] == '"' &&
            (pos + key.length() < source.length()) &&
            source[pos + key.length()] == '"') {
            return pos - 1;
        }
        startPos = pos + key.length();
    }
}

inline std::string_view ExtractJsonValue(std::string_view source, std::string_view key, bool isString) {
    size_t pos = FindJsonKey(source, key);
    if (pos == std::string_view::npos) return {};
    pos = source.find(':', pos + key.length() + 2);
    if (pos == std::string_view::npos) return {};
    ++pos;
    while (pos < source.length() &&
           (source[pos] == ' ' || source[pos] == '\t' ||
            source[pos] == '\n' || source[pos] == '\r')) ++pos;

    if (isString) {
        if (pos >= source.length() || source[pos] != '"') return {};
        ++pos;
        size_t endPos = pos;
        while (endPos < source.length() && source[endPos] != '"') {
            if (source[endPos] == '\\' && endPos + 1 < source.length()) {
                endPos += 2;
            } else {
                ++endPos;
            }
        }
        return (endPos < source.length()) ? source.substr(pos, endPos - pos) : std::string_view{};
    } else {
        size_t endPos = pos;
        while (endPos < source.length() &&
               source[endPos] != ',' && source[endPos] != '}' &&
               source[endPos] != ']' && source[endPos] != ' ' &&
               source[endPos] != '\n' && source[endPos] != '\r') ++endPos;
        return source.substr(pos, endPos - pos);
    }
}

// O(N) 逐字符扫描
template<typename Callback>
void ForEachJsonObject(std::string_view source, std::string_view arrayKey, Callback&& cb) {
    size_t pos = FindJsonKey(source, arrayKey);
    if (pos == std::string_view::npos) return;
    pos = source.find(':', pos + arrayKey.length() + 2);
    if (pos == std::string_view::npos) return;
    pos = source.find('[', pos);
    if (pos == std::string_view::npos) return;

    int depth = 0;
    size_t objStart = 0;
    const size_t len = source.length();
    for (size_t i = pos; i < len; ++i) {
        char c = source[i];
        if (c == '"') {
            for (++i; i < len; ++i) {
                if (source[i] == '\\' && i + 1 < len) { ++i; continue; }
                if (source[i] == '"') break;
            }
            continue;
        }
        if (c == '{') {
            if (depth == 0) objStart = i;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) cb(source.substr(objStart, i - objStart + 1));
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
}

inline std::wstring Utf8ToWstring(std::string_view str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
    return result;
}

inline char* I64ToStr(long long val, char* buf) {
    auto [ptr, ec] = std::to_chars(buf, buf + 20, val);
    *ptr = '\0';
    return buf;
}

inline std::string_view ExtractUrlParam(std::string_view url, std::string_view key) {
    size_t pos = url.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.length();
    size_t end = url.find('&', pos);
    return (end == std::string_view::npos) ? url.substr(pos) : url.substr(pos, end - pos);
}

inline std::string_view ExtractHost(std::string_view url) {
    auto pos = url.find("://");
    if (pos == std::string_view::npos) return {};
    pos += 3;
    auto end = url.find('/', pos);
    return (end == std::string_view::npos) ? url.substr(pos) : url.substr(pos, end - pos);
}

// ---------------------------------------------------------
// [单次扫描 - 一次性提取一条 item 的全部 9 个字段]
// ---------------------------------------------------------
struct ParsedItem {
    std::string_view id;
    std::string_view gacha_id;
    std::string_view gacha_type;
    std::string_view item_id;
    std::string_view name;
    std::string_view item_type;
    std::string_view rank_type;
    std::string_view time;
    std::string_view uid;
};

inline void ParseItem(std::string_view item, ParsedItem& out) {
    out = {};
    const char* const base = item.data();
    const size_t total = item.length();
    size_t i = 0;

    while (i < total) {
        const char* p = (const char*)std::memchr(base + i, '"', total - i);
        if (!p) break;
        i = p - base + 1;
        size_t keyStart = i;
        while (i < total && base[i] != '"') {
            if (base[i] == '\\' && i + 1 < total) i += 2;
            else ++i;
        }
        if (i >= total) break;
        std::string_view key(base + keyStart, i - keyStart);
        ++i;

        while (i < total && base[i] != ':') ++i;
        if (i >= total) break;
        ++i;
        while (i < total && (base[i] == ' ' || base[i] == '\t' ||
                             base[i] == '\n' || base[i] == '\r')) ++i;
        if (i >= total) break;

        std::string_view value;
        if (base[i] == '"') {
            ++i;
            size_t vs = i;
            while (i < total && base[i] != '"') {
                if (base[i] == '\\' && i + 1 < total) i += 2;
                else ++i;
            }
            if (i >= total) break;
            value = std::string_view(base + vs, i - vs);
            ++i;
        } else {
            size_t vs = i;
            while (i < total && base[i] != ',' && base[i] != '}' &&
                   base[i] != ']' && base[i] != ' ' && base[i] != '\n' &&
                   base[i] != '\r') ++i;
            value = std::string_view(base + vs, i - vs);
        }

        if      (key == "id")         out.id = value;
        else if (key == "gacha_id")   out.gacha_id = value;
        else if (key == "gacha_type") out.gacha_type = value;
        else if (key == "item_id")    out.item_id = value;
        else if (key == "name")       out.name = value;
        else if (key == "item_type")  out.item_type = value;
        else if (key == "rank_type")  out.rank_type = value;
        else if (key == "time")       out.time = value;
        else if (key == "uid")        out.uid = value;
    }
}

// ---------------------------------------------------------
// [AoS 记录]
// ---------------------------------------------------------
struct ExportRecord {
    long long parsed_id;
    std::string_view raw_id;
    std::string_view gacha_id;
    std::string_view gacha_type;
    std::string_view item_id;
    std::string_view name;
    std::string_view item_type;
    std::string_view rank_type;
    std::string_view time;
};

// ---------------------------------------------------------
// [RAII 句柄]
// ---------------------------------------------------------
struct FileHandle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    operator HANDLE() const { return h; }
};
struct MappingHandle {
    HANDLE h = NULL;
    ~MappingHandle() { if (h) CloseHandle(h); }
    operator HANDLE() const { return h; }
};
struct MapView {
    const void* p = nullptr;
    ~MapView() { if (p) UnmapViewOfFile(p); }
};
struct WinHttpHandle {
    HINTERNET h = NULL;
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    operator HINTERNET() const { return h; }
};

// ---------------------------------------------------------
// [网络抓取 - 修复 WinHttp 死循环]
// ---------------------------------------------------------
std::string FetchPath(HINTERNET hConnect, const std::wstring& path) {
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL,
                                            WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    std::string response;
    if (!hRequest) return response;

    bool ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(hRequest, NULL);

    if (ok) {
        char stackBuf[8192];
        DWORD dwSize = 0, dwDownloaded = 0;
        while (true) {
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
            if (dwSize == 0) break;

            if (dwSize <= sizeof(stackBuf)) {
                if (!WinHttpReadData(hRequest, stackBuf, dwSize, &dwDownloaded)) break;
                if (dwDownloaded == 0) break;
                response.append(stackBuf, dwDownloaded);
            } else {
                std::vector<char> heapBuf(dwSize);
                if (!WinHttpReadData(hRequest, heapBuf.data(), dwSize, &dwDownloaded)) break;
                if (dwDownloaded == 0) break;
                response.append(heapBuf.data(), dwDownloaded);
            }
        }
    }
    WinHttpCloseHandle(hRequest);
    return response;
}

struct PoolConfig { std::string gachaType, displayName; };

// ---------------------------------------------------------
// [BufferedWriter - RAII 析构自动 Flush]
// ---------------------------------------------------------
struct BufferedWriter {
    HANDLE hFile;
    char buf[65536];
    DWORD pos = 0;

    explicit BufferedWriter(HANDLE h) : hFile(h) {}
    ~BufferedWriter() { Flush(); }

    BufferedWriter(const BufferedWriter&) = delete;
    BufferedWriter& operator=(const BufferedWriter&) = delete;

    void Flush() {
        if (pos > 0 && hFile != INVALID_HANDLE_VALUE) {
            DWORD w;
            WriteFile(hFile, buf, pos, &w, NULL);
            pos = 0;
        }
    }
    void Write(const char* data, DWORD len) {
        while (len > 0) {
            DWORD space = sizeof(buf) - pos;
            DWORD chunk = (len < space) ? len : space;
            std::memcpy(buf + pos, data, chunk);
            pos += chunk; data += chunk; len -= chunk;
            if (pos == sizeof(buf)) Flush();
        }
    }
    void Write(std::string_view sv) { Write(sv.data(), (DWORD)sv.size()); }

    template<size_t N>
    void WriteLit(const char (&s)[N]) {
        constexpr DWORD len = N - 1;
        if (pos + len > sizeof(buf)) Flush();
        std::memcpy(buf + pos, s, len);
        pos += len;
    }

    void WriteEscaped(std::string_view s) {
        const char* p = s.data();
        const char* end = p + s.size();
        while (p < end) {
            const char* c = p;
            while (p < end && *p != '"' && *p != '\\') ++p;
            if (p > c) Write(c, (DWORD)(p - c));
            if (p < end) {
                if (*p == '"') WriteLit("\\\"");
                else           WriteLit("\\\\");
                ++p;
            }
        }
    }
    void WriteKV(std::string_view key, std::string_view val) {
        WriteLit("            \"");
        Write(key);
        WriteLit("\": \"");
        WriteEscaped(val);
        WriteLit("\"");
    }
};

// ---------------------------------------------------------
// 主流程
// ---------------------------------------------------------
int main() {
    SetConsoleOutputCP(CP_UTF8);

    char urlBuffer[4096];
    printf("请输入您的绝区零抽卡记录链接 (getGachaLog URL):\n> ");
    if (!fgets(urlBuffer, sizeof(urlBuffer), stdin)) return 1;

    std::string_view inputUrl(urlBuffer);
    while (!inputUrl.empty() &&
           (inputUrl.back() == ' ' || inputUrl.back() == '\n' ||
            inputUrl.back() == '\r' || inputUrl.back() == '\t')) {
        inputUrl.remove_suffix(1);
    }

    auto authkey = ExtractUrlParam(inputUrl, "authkey=");
    if (authkey.empty()) { printf("错误: 无法提取 authkey。\n"); system("pause"); return 1; }

    auto gameBiz = ExtractUrlParam(inputUrl, "game_biz=");
    auto region  = ExtractUrlParam(inputUrl, "region=");
    auto lang    = ExtractUrlParam(inputUrl, "lang=");
    if (gameBiz.empty()) gameBiz = "nap_global";
    if (region.empty())  region  = "prod_gf_jp";
    if (lang.empty())    lang    = "zh-cn";

    auto host = ExtractHost(inputUrl);
    if (host.empty()) host = "public-operation-nap-sg.hoyoverse.com";

    printf("\n已识别: host=%.*s  region=%.*s  game_biz=%.*s\n",
        (int)host.size(), host.data(),
        (int)region.size(), region.data(),
        (int)gameBiz.size(), gameBiz.data());

    std::vector<PoolConfig> pools = {
        {"2", "独家频段 (限定角色)"},
        {"3", "音擎频段 (限定音擎)"},
        {"1", "常驻频段"},
        {"5", "邦布频段"},
    };

    // PMR:栈上 2MB 内存池
    std::array<std::byte, 2 * 1024 * 1024> stackBuffer;
    std::pmr::monotonic_buffer_resource pool(stackBuffer.data(), stackBuffer.size());
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);

    std::pmr::vector<ExportRecord> records(alloc);
    records.reserve(10000);

    // 数据载体:网络 payload 和本地文件内容都进这里
    // deque 保证 push_back 后指针不失效,records 里的 string_view 安全引用
    std::deque<std::string> networkPayloads;

    std::pmr::unordered_set<long long> local_safe_ids(alloc);
    local_safe_ids.reserve(10000);

    std::string uigfFilename = "uigf_zzz.json";
    std::string savedUid;
    int savedTimezone = 0;

    // ---- 读取本地老记录(读完立即释放句柄,避免锁住目标文件)----
    {
        FileHandle hFile;
        hFile.h = CreateFileA(uigfFilename.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile.h != INVALID_HANDLE_VALUE) {
            DWORD fileSize = GetFileSize(hFile, NULL);
            if (fileSize != INVALID_FILE_SIZE && fileSize > 0) {
                MappingHandle hMap;
                hMap.h = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
                if (hMap.h) {
                    MapView view;
                    view.p = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
                    if (view.p) {
                        // 关键修复:把 mmap 数据复制到 networkPayloads,让 string_view
                        // 指向 deque 里的 string(deque push_back 不失效指针)。
                        // 这样就可以立即关闭 mmap/file 句柄,不会锁住目标文件导致
                        // 后续 MoveFileExA 失败。
                        networkPayloads.emplace_back(
                            std::string((const char*)view.p, fileSize));
                        std::string_view bufferView = networkPayloads.back();

                        if (bufferView.size() >= 3 &&
                            (unsigned char)bufferView[0] == 0xEF &&
                            (unsigned char)bufferView[1] == 0xBB &&
                            (unsigned char)bufferView[2] == 0xBF) {
                            bufferView.remove_prefix(3);
                        }

                        {
                            auto uidVal = ExtractJsonValue(bufferView, "uid", false);
                            if (!uidVal.empty()) {
                                if (uidVal.front() == '"' && uidVal.back() == '"' && uidVal.size() >= 2) {
                                    savedUid.assign(uidVal.data() + 1, uidVal.size() - 2);
                                } else {
                                    savedUid.assign(uidVal);
                                }
                            }
                            auto tzStr = ExtractJsonValue(bufferView, "timezone", false);
                            if (!tzStr.empty()) {
                                std::from_chars(tzStr.data(), tzStr.data() + tzStr.size(), savedTimezone);
                            }
                        }

                        ParsedItem parsed;
                        ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
                            ParseItem(itemStr, parsed);

                            long long parsed_id = 0;
                            if (!parsed.id.empty()) {
                                std::from_chars(parsed.id.data(),
                                               parsed.id.data() + parsed.id.size(),
                                               parsed_id);
                            }

                            records.push_back(ExportRecord{
                                parsed_id,
                                parsed.id,
                                parsed.gacha_id,
                                parsed.gacha_type,
                                parsed.item_id,
                                parsed.name,
                                parsed.item_type,
                                parsed.rank_type,
                                parsed.time
                            });
                            local_safe_ids.insert(parsed_id);
                        });

                        printf("成功加载本地存储的 %zu 条抽卡记录。\n", records.size());
                    }
                }
            }
        } else {
            printf("未发现本地记录,将创建新文件。\n");
        }
    }  // <- Guard 全部析构,文件完全释放

    printf("\n========================================\n");
    printf("       开始向服务器拉取调频数据\n");
    printf("========================================\n");

    WinHttpHandle hSession;
    hSession.h = WinHttpOpen(L"ZZZ Gacha Tool", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    WinHttpHandle hConnect;
    if (hSession.h) {
        hConnect.h = WinHttpConnect(hSession,
                                    Utf8ToWstring(std::string(host)).c_str(),
                                    INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    if (!hConnect.h) { printf("网络初始化失败!\n"); system("pause"); return 1; }

    std::pmr::unordered_set<long long> sessionIds(alloc);
    sessionIds.reserve(2000);

    std::string authkeyStr(authkey), gameBizStr(gameBiz),
                regionStr(region), langStr(lang);
    std::string commonParams = "authkey_ver=1&sign_type=2&authkey=" + authkeyStr
        + "&game_biz=" + gameBizStr + "&region=" + regionStr
        + "&lang=" + langStr + "&size=20";

    for (const auto& poolCfg : pools) {
        printf("\n>>> 正在抓取 [%s] ...\n", poolCfg.displayName.c_str());
        bool reachedExisting = false;
        std::string endId;
        int poolFetchedCount = 0;

        while (!reachedExisting) {
            std::string path = "/common/gacha_record/api/getGachaLog?" + commonParams
                + "&real_gacha_type=" + poolCfg.gachaType
                + "&end_id=" + endId;

            networkPayloads.emplace_back(FetchPath(hConnect, Utf8ToWstring(path)));
            std::string_view resView = networkPayloads.back();

            if (resView.empty()) {
                printf("  [错误] 网络请求失败。\n");
                break;
            }

            auto retcode = ExtractJsonValue(resView, "retcode", false);
            if (retcode != "0") {
                auto msg = ExtractJsonValue(resView, "message", true);
                printf("  [提示] %.*s\n", (int)msg.size(), msg.data());
                break;
            }

            bool gotItems = false;
            std::string lastId;
            ParsedItem parsed;

            ForEachJsonObject(resView, "list", [&](std::string_view itemStr) {
                if (reachedExisting) return;
                gotItems = true;

                ParseItem(itemStr, parsed);

                long long id = 0;
                if (!parsed.id.empty()) {
                    std::from_chars(parsed.id.data(),
                                   parsed.id.data() + parsed.id.size(), id);
                }
                lastId.assign(parsed.id);

                if (local_safe_ids.contains(id)) {
                    reachedExisting = true;
                    printf("  * 触达本地老记录 (ID: %lld),停止追溯。\n", id);
                    return;
                }
                if (sessionIds.contains(id)) {
                    printf("  [警告] 遇到重复数据 (ID: %lld),中止。\n", id);
                    reachedExisting = true;
                    return;
                }

                if (savedUid.empty() && !parsed.uid.empty()) {
                    savedUid.assign(parsed.uid);
                    if (!savedUid.empty() && savedUid.front() == '"') {
                        savedUid.erase(0, 1);
                    }
                    if (!savedUid.empty() && savedUid.back() == '"') {
                        savedUid.pop_back();
                    }
                }

                sessionIds.insert(id);

                records.push_back(ExportRecord{
                    id,
                    parsed.id,
                    parsed.gacha_id,
                    parsed.gacha_type,
                    parsed.item_id,
                    parsed.name,
                    parsed.item_type,
                    parsed.rank_type,
                    parsed.time
                });

                poolFetchedCount++;
                printf("  获取到: %.*s (%.*s 星) [%.*s] - %.*s\n",
                    (int)parsed.name.size(),      parsed.name.data(),
                    (int)parsed.rank_type.size(), parsed.rank_type.data(),
                    (int)parsed.item_type.size(), parsed.item_type.data(),
                    (int)parsed.time.size(),      parsed.time.data());
            });

            if (reachedExisting || !gotItems) break;
            endId = lastId;
            Sleep(300);
        }
        printf(">>> [%s] 抓取完成,本次新增: %d 条。\n",
               poolCfg.displayName.c_str(), poolFetchedCount);
        Sleep(500);
    }

    printf("\n========================================\n");
    printf("已完成!总计新增 %zu 条记录。\n", sessionIds.size());

    std::ranges::sort(records, [](const ExportRecord& a, const ExportRecord& b) {
        return a.parsed_id < b.parsed_id;
    });

    if (savedTimezone == 0) {
        if      (regionStr.find("cn") != std::string::npos) savedTimezone = 8;
        else if (regionStr.find("jp") != std::string::npos) savedTimezone = 9;
        else if (regionStr.find("eu") != std::string::npos) savedTimezone = 1;
        else if (regionStr.find("us") != std::string::npos) savedTimezone = -5;
        else savedTimezone = 0;
    }

    time_t rawtime; time(&rawtime);
    long long export_ts = (long long)rawtime;

    // 安全写入:tmp → 替换
    std::string tempFilename = uigfFilename + ".tmp";
    HANDLE hOut = CreateFileA(tempFilename.c_str(), GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hOut != INVALID_HANDLE_VALUE) {
        {
            BufferedWriter w(hOut);
            char numBuf[24];

            w.WriteLit("{\n  \"info\": {\n");
            w.WriteLit("    \"export_timestamp\": ");
            w.Write(I64ToStr(export_ts, numBuf));
            w.WriteLit(",\n");
            w.WriteLit("    \"export_app\": \"ZZZ Gacha Exporter\",\n");
            w.WriteLit("    \"export_app_version\": \"v1.0.0\",\n");
            w.WriteLit("    \"version\": \"v4.2\"\n  },\n");

            w.WriteLit("  \"nap\": [\n    {\n");
            w.WriteLit("      \"uid\": ");
            w.Write(savedUid.empty() ? "0" : savedUid);
            w.WriteLit(",\n");
            w.WriteLit("      \"timezone\": ");
            w.Write(I64ToStr(savedTimezone, numBuf));
            w.WriteLit(",\n");
            w.WriteLit("      \"lang\": \"");
            w.Write(langStr);
            w.WriteLit("\",\n");
            w.WriteLit("      \"list\": [\n");

            const size_t n = records.size();
            for (size_t i = 0; i < n; ++i) {
                const auto& r = records[i];
                w.WriteLit("        {\n");
                w.WriteKV("gacha_id",   r.gacha_id);   w.WriteLit(",\n");
                w.WriteKV("gacha_type", r.gacha_type); w.WriteLit(",\n");
                w.WriteKV("item_id",    r.item_id);    w.WriteLit(",\n");
                w.WriteLit("            \"count\": \"1\",\n");
                w.WriteKV("time",       r.time);       w.WriteLit(",\n");
                w.WriteKV("name",       r.name);       w.WriteLit(",\n");
                w.WriteKV("item_type",  r.item_type);  w.WriteLit(",\n");
                w.WriteKV("rank_type",  r.rank_type);  w.WriteLit(",\n");
                w.WriteKV("id",         r.raw_id);     w.WriteLit("\n");
                w.WriteLit("        }");
                if (i < n - 1) w.WriteLit(",");
                w.WriteLit("\n");
            }

            w.WriteLit("      ]\n    }\n  ]\n}\n");
        }
        CloseHandle(hOut);

        if (MoveFileExA(tempFilename.c_str(), uigfFilename.c_str(),
                        MOVEFILE_REPLACE_EXISTING)) {
            printf("已成功更新记录并保存至: %s (UIGF v4.2)\n", uigfFilename.c_str());
        } else {
            DWORD err = GetLastError();
            printf("文件覆盖失败(错误码 %lu)!请手动将 %s 重命名为 %s\n",
                   err, tempFilename.c_str(), uigfFilename.c_str());
        }
    } else {
        printf("临时文件创建失败!请检查目录权限。\n");
    }

    system("pause");
    return 0;
}
