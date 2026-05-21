// ============================================================
// ZZZ Gacha Exporter - UIGF v4.2 / 面向数据 / PMR 栈分配
// ============================================================
// 绝区零 (Zenless Zone Zero) 抽卡记录导出工具
//
// 与终末地版本的关键差异:
//   1. API 来自米哈游 HoYoverse,不再是鹰角 Gryphline
//      端点: /common/gacha_record/api/getGachaLog
//      翻页: 用 end_id 游标 (不是 seq_id), size 上限 20
//      鉴权: authkey URL-encoded 在原 URL 里, 整段透传, 不解析
//   2. 卡池类型 (real_gacha_type/gacha_type):
//        "1" = 常驻频段(热门卡司)
//        "2" = 独家频段(代理人 UP)
//        "3" = 音擎频段(武器 UP)
//        "5" = 邦布频段
//      UIGF v4.2 nap.gacha_type enum 与之一致
//   3. 没有"30 抽赠送十连"机制, 不需要 is_free 字段
//   4. 物品类型字段值: "代理人"/"音擎"/"邦布" (中文 lang) 或英文
//   5. 输出键: 顶层 "nap" (UIGF v4.2 标准, 而非自定义 "endfield")
//   6. UIGF v4.2 nap 必填字段: gacha_type / item_id / time / id
//      还需 count (api 返回, 一般为 "1")
//
// 编译: cl /std:c++20 /EHsc /O2 main.cpp
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
// [枚举 / 无堆分配的大小写不敏感包含比较]
// ---------------------------------------------------------
// 绝区零有三种物品类型: 代理人(角色) / 音擎(武器) / 邦布(宠物)
// API 返回的 item_type 在 zh-cn 下就是中文名, 直接精确匹配
enum class ItemType : uint8_t { Unknown = 0, Agent, WEngine, Bangboo };

// 无堆分配的大小写不敏感 find —— 原版每次都 std::string 拷贝, 这是 hot-path bug
inline bool ContainsCI(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || haystack.size() < needle.size()) return false;
    const size_t H = haystack.size();
    const size_t N = needle.size();
    for (size_t i = 0; i + N <= H; ++i) {
        bool ok = true;
        for (size_t j = 0; j < N; ++j) {
            char a = haystack[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) { ok = false; break; }
        }
        if (ok) return true;
    }
    return false;
}

inline ItemType ParseItemType(std::string_view sv) {
    // zh-cn 下 API 返回中文 item_type 字段; 也兼容英文(en-us 等)
    // 中文短串直接 byte 级精确比较, 不做 lower (中文无大小写概念)
    if (sv == "代理人") return ItemType::Agent;
    if (sv == "音擎")   return ItemType::WEngine;
    if (sv == "邦布")   return ItemType::Bangboo;
    // 防御性英文兼容 (Agents / W-Engines / Bangboos)
    if (ContainsCI(sv, "agent"))   return ItemType::Agent;
    if (ContainsCI(sv, "engine"))  return ItemType::WEngine;
    if (ContainsCI(sv, "bangboo")) return ItemType::Bangboo;
    return ItemType::Unknown;
}

inline std::string_view ItemTypeToStr(ItemType type) {
    // 输出 UIGF 文件时统一用中文, 与游戏内 / API 一致, 也与 zh-cn lang 自洽
    if (type == ItemType::Agent)   return "代理人";
    if (type == ItemType::WEngine) return "音擎";
    if (type == ItemType::Bangboo) return "邦布";
    return "Unknown";
}

// ---------------------------------------------------------
// [极简 JSON 解析 - 修复转义边界]
// ---------------------------------------------------------
inline size_t FindJsonKey(std::string_view source, std::string_view key, size_t startPos = 0) {
    while (true) {
        size_t pos = source.find(key, startPos);
        if (pos == std::string_view::npos) return std::string_view::npos;
        if (pos > 0 && source[pos - 1] == '"' &&
            (pos + key.length() < source.length()) &&
            source[pos + key.length()] == '"') return pos - 1;
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
            // 修复: \\ 处理必须是"跳 2 字节", 原版 source[endPos]='\\' 后只 endPos++ 一次, 边界上会越界
            if (source[endPos] == '\\' && endPos + 1 < source.length()) endPos += 2;
            else ++endPos;
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

// 从原始 URL 里抠出某个 query 参数的 raw value (含 URL 编码部分)
// 与终末地版本完全一致, 米哈游 authkey 是大段 percent-encoded 串, 必须原样透传
inline std::string_view ExtractUrlParam(std::string_view url, std::string_view key) {
    size_t pos = url.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.length();
    size_t end = url.find('&', pos);
    return (end == std::string_view::npos) ? url.substr(pos) : url.substr(pos, end - pos);
}

// ---------------------------------------------------------
// [AoS 记录]
// 绝区零 id 字段是 19 位数字字符串, 全局递增, 没有"角色/武器交叉" id 冲突,
// 所以不需要终末地版本里"武器取负"的 trick. 直接用 long long 唯一去重即可.
// ---------------------------------------------------------
struct ExportRecord {
    long long id;             // 19 位数字 ID, long long 完全装得下 (max ~9.2e18)
    long long timestamp;      // gacha_ts: 抽卡时间秒级时间戳 (fallback)
    std::string_view gachaType;  // "1"/"2"/"3"/"5"
    std::string_view itemId;
    std::string_view name;
    ItemType itemType;
    std::string_view rankType;   // "2"/"3"/"4" (B/A/S)
    std::string_view count;      // 一般 "1"
    std::string_view gachaId;    // API 返回的 gacha_id, 池子的具体期次 ID
    std::string_view timeStr;    // API 返回的 time 字符串 "YYYY-MM-DD HH:MM:SS" (服务器时区, 优先用这个)
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
// [FetchPath - 修复 WinHttpQueryDataAvailable 失败死循环]
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

struct PoolConfig {
    std::string realGachaType;     // API 参数 real_gacha_type
    std::string initLogBaseType;   // API 参数 init_log_gacha_base_type (一般等于 real_gacha_type)
    std::string uigfGachaType;     // 写到 UIGF 文件里的 gacha_type ("1"/"2"/"3"/"5")
    std::string displayName;       // 控制台展示用
};

// ---------------------------------------------------------
// [BufferedWriter - 析构 RAII Flush]
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
            DWORD written;
            WriteFile(hFile, buf, pos, &written, NULL);
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
            const char* clean = p;
            while (p < end && *p != '"' && *p != '\\') ++p;
            if (p > clean) Write(clean, (DWORD)(p - clean));
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

    // UIGF v4.2 要求 time 字段是 "YYYY-MM-DD HH:MM:SS" 格式的本地时间.
    // 米哈游 API 返回的 time 字段本身就是这种字符串(已经是服务器时区下的本地时间),
    // 所以这里其实只在我们 fallback 用 gacha_ts 时才用; 主路径直接写 API 原 time.
    void WriteTimeKV(std::string_view key, long long sec_ts) {
        time_t t = sec_ts;
        struct tm tm_info;
        localtime_s(&tm_info, &t);
        char tbuf[64];
        int len = wsprintfA(tbuf, "%04d-%02d-%02d %02d:%02d:%02d",
                            tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                            tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        WriteLit("            \"");
        Write(key);
        WriteLit("\": \"");
        Write(tbuf, len);
        WriteLit("\"");
    }

    void WriteI64KV(std::string_view key, long long val, bool quotes) {
        char nbuf[32];
        auto [ptr, ec] = std::to_chars(nbuf, nbuf + 32, val);
        WriteLit("            \"");
        Write(key);
        WriteLit("\": ");
        if (quotes) WriteLit("\"");
        Write(nbuf, (DWORD)(ptr - nbuf));
        if (quotes) WriteLit("\"");
    }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // URL 缓冲区: 米哈游 authkey 是大段 percent-encoded 串, 单 authkey 就 ~1.5KB,
    // 完整 URL 加其它参数约 1.7KB. 给 16KB 留足富余 (栈数组, 不会爆栈).
    static char urlBuffer[16384];
    printf("请输入您的绝区零抽卡记录完整链接 (含 authkey 的 URL):\n> ");
    if (!fgets(urlBuffer, sizeof(urlBuffer), stdin)) return 1;

    // 截断检测: 如果读了 sizeof-1 字节但末尾不是 '\n', 说明 URL 比缓冲区还长
    size_t urlLen = strlen(urlBuffer);
    if (urlLen == sizeof(urlBuffer) - 1 && urlBuffer[urlLen - 1] != '\n') {
        printf("警告: 输入的 URL 超过 16KB，可能被截断。\n");
    }

    std::string_view inputUrl(urlBuffer);
    while (!inputUrl.empty() &&
           (inputUrl.back() == ' ' || inputUrl.back() == '\n' ||
            inputUrl.back() == '\r' || inputUrl.back() == '\t')) {
        inputUrl.remove_suffix(1);
    }

    // 必须的鉴权三件套: authkey + authkey_ver + sign_type
    auto authkey     = ExtractUrlParam(inputUrl, "authkey=");
    auto authkeyVer  = ExtractUrlParam(inputUrl, "authkey_ver=");
    auto signType    = ExtractUrlParam(inputUrl, "sign_type=");
    if (authkey.empty()) {
        printf("错误: 无法提取 authkey。\n");
        system("pause"); return 1;
    }
    if (authkeyVer.empty()) authkeyVer = "1";
    if (signType.empty())   signType   = "2";

    // game_biz / region 标识区服, 也需要透传; 不同区服走不同 host
    auto gameBiz = ExtractUrlParam(inputUrl, "game_biz=");
    auto region  = ExtractUrlParam(inputUrl, "region=");
    if (gameBiz.empty()) gameBiz = "nap_global";

    // 选择 host:
    //   nap_cn         -> public-operation-nap.mihoyo.com (国服, 米游社)
    //   nap_global     -> public-operation-nap-sg.hoyoverse.com (国际服)
    //   其它(测试服等) -> 默认走国际服 host, 通常也能解析
    std::wstring hostName;
    if (gameBiz == "nap_cn") {
        hostName = L"public-operation-nap.mihoyo.com";
        printf("已识别区服: 国服 (mihoyo)\n");
    } else {
        hostName = L"public-operation-nap-sg.hoyoverse.com";
        printf("已识别区服: 国际服 (hoyoverse)\n");
    }

    // 4 个池子. 顺序: 限定代理人池 / 限定音擎池 / 邦布池 / 常驻池
    // 这个顺序是为了让"用户最关心的池子先抓"; 实际去重靠 id, 顺序不影响正确性.
    std::vector<PoolConfig> pools = {
        {"2", "2", "2", "独家频段 - 代理人 UP"},
        {"3", "3", "3", "音擎频段 - 武器 UP"},
        {"5", "5", "5", "邦布频段"},
        {"1", "1", "1", "常驻频段"},
    };

    // PMR: 栈上 2MB 池 (与终末地 main.cpp 同款).
    // 注意: MSVC main 线程默认栈只 1MB, 直接放 2MB 会爆栈崩溃。
    // 必须在链接器加 /STACK:4194304 (4MB) 才能用栈池。
    // 栈池 vs 堆池的性能差异:
    //   - 分配/释放开销 0 (栈指针偏移 vs malloc 一次 2MB + free)
    //   - 与栈上的局部变量物理相邻, L1/L2 命中, TLB 不会 miss
    //   - monotonic_buffer 整个工作集都从此池分配, 锁在热区
    std::array<std::byte, 2 * 1024 * 1024> stackBuffer;
    std::pmr::monotonic_buffer_resource pool(stackBuffer.data(), stackBuffer.size());
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);

    std::pmr::vector<ExportRecord> records(alloc);
    records.reserve(20000);

    std::deque<std::string> networkPayloads;

    // 去重: unordered_set O(1) 查询, 比 vector linear scan O(n) 快得多
    std::pmr::unordered_set<long long> local_ids(alloc);
    local_ids.reserve(20000);

    std::string uigfFilename = "uigf_zzz.json";

    // ---- 读取本地老记录 (读完立即释放句柄, 避免锁住目标文件) ----
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
                        // 关键: 把 mmap 数据复制到 networkPayloads, 让 string_view
                        // 指向 deque 里的 string (deque push_back 不失效指针)。
                        // 这样就可以立即关闭 mmap/file 句柄, 不会锁住目标文件导致
                        // 后续 MoveFileExA 失败。
                        networkPayloads.emplace_back(
                            std::string((const char*)view.p, fileSize));
                        std::string_view bufferView = networkPayloads.back();

                        // RAII Guard 会在退出本作用域时自动 unmap / close

                        if (bufferView.size() >= 3 &&
                            (unsigned char)bufferView[0] == 0xEF &&
                            (unsigned char)bufferView[1] == 0xBB &&
                            (unsigned char)bufferView[2] == 0xBF) {
                            bufferView.remove_prefix(3);
                        }

                        // ForEachJsonObject 找 "list" key. v4.2 nap 文件里只有一处
                        // "list" (在 nap[0] 下), 所以直接命中. (info 块里没有 list)
                        ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
                            std::string_view raw_id = ExtractJsonValue(itemStr, "id", true);
                            long long parsed_id = 0, parsed_ts = 0;
                            if (!raw_id.empty()) {
                                std::from_chars(raw_id.data(), raw_id.data() + raw_id.size(), parsed_id);
                            }
                            std::string_view tsStr = ExtractJsonValue(itemStr, "gacha_ts", true);
                            if (!tsStr.empty()) {
                                std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), parsed_ts);
                            }

                            ItemType it = ParseItemType(ExtractJsonValue(itemStr, "item_type", true));

                            ExportRecord rec;
                            rec.id        = parsed_id;
                            rec.timestamp = parsed_ts;
                            rec.gachaType = ExtractJsonValue(itemStr, "gacha_type", true);
                            rec.itemId    = ExtractJsonValue(itemStr, "item_id",    true);
                            rec.name      = ExtractJsonValue(itemStr, "name",       true);
                            rec.itemType  = it;
                            rec.rankType  = ExtractJsonValue(itemStr, "rank_type",  true);
                            rec.count     = ExtractJsonValue(itemStr, "count",      true);
                            rec.gachaId   = ExtractJsonValue(itemStr, "gacha_id",   true);
                            rec.timeStr   = ExtractJsonValue(itemStr, "time",       true);
                            records.push_back(std::move(rec));
                            local_ids.insert(parsed_id);
                        });
                    }
                }
            }
            printf("成功加载本地存储的 %zu 条抽卡记录。\n", records.size());
        } else {
            printf("未发现本地记录，将创建新文件。\n");
        }
    }  // <- Guard 全部析构, 文件完全释放

    printf("\n========================================\n");
    printf("        开始向服务器拉取抽卡数据\n");
    printf("========================================\n");

    WinHttpHandle hSession;
    hSession.h = WinHttpOpen(L"ZZZ Gacha Tool", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    WinHttpHandle hConnect;
    if (hSession.h) {
        hConnect.h = WinHttpConnect(hSession, hostName.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    if (!hConnect.h) {
        printf("网络初始化失败!\n");
        system("pause"); return 1;
    }

    // sessionIds 用 unordered_set (O(1) 去重)
    std::pmr::unordered_set<long long> sessionIds(alloc);
    sessionIds.reserve(5000);

    std::string authkeyStr(authkey), authkeyVerStr(authkeyVer), signTypeStr(signType);
    std::string gameBizStr(gameBiz), regionStr(region);

    for (const auto& poolCfg : pools) {
        printf("\n>>> 正在抓取 [%s] ...\n", poolCfg.displayName.c_str());
        bool reachedExisting = false;
        std::string endIdCursor = "0";  // 米哈游 API: end_id="0" 表示从最新开始
        int page = 1, poolFetchedCount = 0;

        while (!reachedExisting) {
            // 拼路径: 完整透传鉴权三件套, 不解析也不重组 authkey
            // size=20 是米哈游 API 上限; 不要写 size=50 之类的, 服务器会返回报错或截断
            std::string currentPath =
                "/common/gacha_record/api/getGachaLog?"
                "authkey_ver=" + authkeyVerStr +
                "&sign_type=" + signTypeStr +
                "&authkey=" + authkeyStr +
                "&lang=zh-cn"
                "&game_biz=" + gameBizStr +
                (regionStr.empty() ? "" : "&region=" + regionStr) +
                "&real_gacha_type=" + poolCfg.realGachaType +
                "&init_log_gacha_base_type=" + poolCfg.initLogBaseType +
                "&size=20"
                "&end_id=" + endIdCursor;

            networkPayloads.emplace_back(FetchPath(hConnect, Utf8ToWstring(currentPath)));
            std::string_view resView = networkPayloads.back();

            if (resView.empty()) {
                printf("  [错误] 网络请求失败或 authkey 已失效。\n");
                break;
            }

            // 米哈游统一返回 {"retcode": 0, "message": "OK", "data": {...}}
            std::string_view codeStr = ExtractJsonValue(resView, "retcode", false);
            if (codeStr.empty()) {
                printf("  [错误] 接口返回非 JSON 数据或格式异常。\n");
                break;
            }
            if (codeStr != "0") {
                auto msgStr = ExtractJsonValue(resView, "message", true);
                printf("  [提示] 接口返回信息: %.*s\n",
                       (int)msgStr.size(), msgStr.data());
                break;
            }

            std::string lastIdInPage;
            int itemsThisPage = 0;
            ForEachJsonObject(resView, "list", [&](std::string_view itemStr) {
                if (reachedExisting) return;
                ++itemsThisPage;

                std::string_view rawIdStr = ExtractJsonValue(itemStr, "id", true);
                if (rawIdStr.empty()) return;
                lastIdInPage.assign(rawIdStr);  // 用于下一页游标

                long long parsed_id = 0;
                std::from_chars(rawIdStr.data(), rawIdStr.data() + rawIdStr.size(), parsed_id);

                if (local_ids.contains(parsed_id)) {
                    reachedExisting = true;
                    printf("  * 触达本地老记录 (ID: %lld)，停止追溯。\n",
                           parsed_id);
                    return;
                }
                if (sessionIds.contains(parsed_id)) {
                    printf("\n  [警告] 遇到重复数据 (ID: %lld)，防死循环中止。\n",
                           parsed_id);
                    reachedExisting = true;
                    return;
                }
                sessionIds.insert(parsed_id);

                // gacha_ts 在 API 里是字符串(秒级时间戳)
                long long parsed_ts = 0;
                std::string_view tsStr = ExtractJsonValue(itemStr, "time_stamp", true);
                if (tsStr.empty()) tsStr = ExtractJsonValue(itemStr, "gacha_ts", true);
                if (!tsStr.empty()) {
                    std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), parsed_ts);
                }
                // 如果 API 没给时间戳, 用 time 字符串解析 (米哈游 API 一般会返回 time 字符串)
                // 简化处理: 没解到就用 0; UIGF 写出时优先用 API 的 time 字符串

                ExportRecord rec;
                rec.id        = parsed_id;
                rec.timestamp = parsed_ts;
                rec.gachaType = poolCfg.uigfGachaType;  // 用我们配置的 UIGF gacha_type
                rec.itemId    = ExtractJsonValue(itemStr, "item_id",   true);
                rec.name      = ExtractJsonValue(itemStr, "name",      true);
                rec.itemType  = ParseItemType(ExtractJsonValue(itemStr, "item_type", true));
                rec.rankType  = ExtractJsonValue(itemStr, "rank_type", true);
                rec.count     = ExtractJsonValue(itemStr, "count",     true);
                rec.gachaId   = ExtractJsonValue(itemStr, "gacha_id",  true);
                rec.timeStr   = ExtractJsonValue(itemStr, "time",      true);  // 服务器时区本地时间字符串
                if (rec.count.empty()) rec.count = "1";

                records.push_back(std::move(rec));
                poolFetchedCount++;
                printf("  获取到: %.*s (rank_type=%.*s)\n",
                       (int)records.back().name.size(),     records.back().name.data(),
                       (int)records.back().rankType.size(), records.back().rankType.data());
            });

            if (reachedExisting) break;
            if (itemsThisPage == 0 || lastIdInPage.empty()) {
                // 没数据了 (API 返回空 list 表示池子翻完)
                break;
            }

            endIdCursor = lastIdInPage;
            page++;
            // 米哈游接口对单 IP 限流较严; 终末地原版 300ms, 这里 500ms 更稳妥
            Sleep(500);
        }
        printf(">>> [%s] 抓取完成，本次新增拉取: %d 条。\n",
               poolCfg.displayName.c_str(), poolFetchedCount);
        Sleep(800);  // 切池子也间隔一下
    }

    printf("\n========================================\n");
    printf("已完成全部抓取！总计记录数: %zu 条。\n", records.size());

    // 排序: 绝区零的 id 是 19 位全局递增 ID, 直接按 id 升序就是按时间升序.
    // 不需要终末地版本里"角色武器分区"的 trick.
    std::ranges::sort(records, [](const ExportRecord& a, const ExportRecord& b) {
        return a.id < b.id;
    });

    time_t rawtime; time(&rawtime);
    long long export_ts = (long long)rawtime;

    std::string tempFilename = uigfFilename + ".tmp";
    HANDLE hOut = CreateFileA(tempFilename.c_str(), GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hOut != INVALID_HANDLE_VALUE) {
        {
            BufferedWriter w(hOut);
            char numBuf[32];

            // ==========================================================
            // UIGF v4.2 输出 - 绝区零(nap)版
            // ----------------------------------------------------------
            // 文档: https://uigf.org/standards/UIGF.html
            //
            // 与终末地版本(自定义 endfield key)的差异:
            //   1. 顶层用 UIGF v4.2 标准支持的 "nap" 数组
            //   2. nap.list 元素必填字段:
            //        gacha_type / item_id / time / id
            //      推荐字段(为了可读性 + 跨工具兼容):
            //        gacha_id / count / time / name / item_type / rank_type
            //   3. 时区: 国际服 nap_global 服务器返回的 time 是 UTC+8 (米哈游
            //      统一服务器时区), timezone 字段写 8.
            //      若是其他区服, 用本机时区作 fallback.
            //   4. lang 必须是 schema 里 enum 之一; 中文写 "zh-cn"
            // ==========================================================

            time_t t = export_ts;
            struct tm tm_info;
            localtime_s(&tm_info, &t);
            char tbuf[64];
            int tlen = wsprintfA(tbuf, "%04d-%02d-%02d %02d:%02d:%02d",
                                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

            // ---- info 块 ----
            w.WriteLit("{\n    \"info\": {\n");
            w.WriteLit("        \"export_timestamp\": ");
            auto [ptr, ec] = std::to_chars(numBuf, numBuf + 32, export_ts);
            w.Write(numBuf, (DWORD)(ptr - numBuf));
            w.WriteLit(",\n");
            w.WriteLit("        \"export_app\": \"ZZZ Exporter\",\n"
                       "        \"export_app_version\": \"v1.0.0\",\n"
                       "        \"version\": \"v4.2\",\n");
            w.WriteLit("        \"export_time\": \""); w.Write(tbuf, tlen); w.WriteLit("\"\n    },\n");

            // ---- nap 数组 ----
            // 米哈游服务器统一 UTC+8, 所以国际服也是 timezone=8
            // (区别于游戏内 timezone 显示, API time 字段就是 UTC+8 字符串)
            int tzHours = 8;
            if (gameBizStr != "nap_cn" && gameBizStr != "nap_global") {
                // 未知区服, 用本机时区
                TIME_ZONE_INFORMATION tzi;
                DWORD tzKind = GetTimeZoneInformation(&tzi);
                LONG biasMinutes = tzi.Bias;
                if (tzKind == TIME_ZONE_ID_DAYLIGHT) biasMinutes += tzi.DaylightBias;
                else if (tzKind == TIME_ZONE_ID_STANDARD) biasMinutes += tzi.StandardBias;
                tzHours = (int)(-biasMinutes / 60);
            }

            w.WriteLit("    \"nap\": [\n        {\n");
            w.WriteLit("            \"uid\": \"0\",\n");
            w.WriteLit("            \"timezone\": ");
            auto [tzPtr, tzEc] = std::to_chars(numBuf, numBuf + 32, tzHours);
            w.Write(numBuf, (DWORD)(tzPtr - numBuf));
            w.WriteLit(",\n");
            w.WriteLit("            \"lang\": \"zh-cn\",\n");
            w.WriteLit("            \"list\": [\n");

            const size_t n = records.size();
            for (size_t i = 0; i < n; ++i) {
                const auto& r = records[i];
                w.WriteLit("        {\n");

                // 必填: gacha_type
                w.WriteKV("gacha_type", r.gachaType);       w.WriteLit(",\n");
                // 推荐: gacha_id
                if (!r.gachaId.empty()) {
                    w.WriteKV("gacha_id", r.gachaId);       w.WriteLit(",\n");
                }
                // 必填: id
                w.WriteI64KV("id", r.id, true);             w.WriteLit(",\n");
                // 必填: item_id
                w.WriteKV("item_id", r.itemId);             w.WriteLit(",\n");
                // 推荐: count
                w.WriteKV("count", r.count.empty() ? std::string_view{"1"} : r.count);
                w.WriteLit(",\n");
                // 必填: time
                // 优先用 API 返回的 time 字符串 (服务器时区 UTC+8 下的本地时间, 已经是 UIGF
                // 要求的 "YYYY-MM-DD HH:MM:SS" 格式). 这样写出的时间与 timezone=8 自洽.
                // 只有当读取本地老文件且老文件没存 time 字段时才 fallback 到 timestamp 重建.
                if (!r.timeStr.empty()) {
                    w.WriteKV("time", r.timeStr);
                } else {
                    w.WriteTimeKV("time", r.timestamp);
                }
                w.WriteLit(",\n");
                // 推荐: name / item_type / rank_type
                w.WriteKV("name", r.name);                  w.WriteLit(",\n");
                w.WriteKV("item_type", ItemTypeToStr(r.itemType));
                w.WriteLit(",\n");
                w.WriteKV("rank_type", r.rankType);

                w.WriteLit("\n        }");
                if (i < n - 1) w.WriteLit(",");
                w.WriteLit("\n");
            }

            w.WriteLit("            ]\n        }\n    ]\n}\n");
        }
        CloseHandle(hOut);

        if (MoveFileExA(tempFilename.c_str(), uigfFilename.c_str(),
                        MOVEFILE_REPLACE_EXISTING)) {
            printf("已成功更新记录并保存至: %s\n", uigfFilename.c_str());
        } else {
            printf("文件覆盖失败！请手动将 %s 重命名为 %s\n",
                   tempFilename.c_str(), uigfFilename.c_str());
        }
    } else {
        printf("临时文件创建失败！请检查目录权限。\n");
    }

    system("pause");
    return 0;
}
