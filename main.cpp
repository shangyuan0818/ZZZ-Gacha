// ============================================================
// ZZZ Gacha Exporter - UIGF v4.2 (nap) / 面向数据 / PMR / AoS
// ------------------------------------------------------------
// 自 Endfield Gacha Exporter (v0.1.3.3 加固版) 迁移到绝区零。
//
// 绝区零数据契约 (米哈游 HoYoverse API):
//   1. 端点: /common/gacha_record/api/getGachaLog
//      翻页: end_id 游标 (上一页最后一条的 id; "0" 表示从最新开始), size 上限 20
//      鉴权: authkey + authkey_ver + sign_type; authkey 是大段 percent-encoded 串
//      (约 1~2KB), 整段透传, 不解析不重组 → 输入缓冲必须够大 (16KB)
//   2. 卡池类型 (real_gacha_type / 同时也是 UIGF 的 gacha_type 值):
//        "1" = 常驻频段(热门卡司)  "2" = 独家频段(代理人 UP)
//        "3" = 音擎频段(武器 UP)   "5" = 邦布频段
//   3. 物品类型字段值: "代理人"/"音擎"/"邦布" (zh-cn lang)
//   4. 响应包装: {"retcode": 0, "message": "OK", "data": {...}}
//      (与鹰角的 {"code": 0, "msg": ...} 字段名不同)
//   5. ID 是 19 位全局递增整数; 去重直接用 id (全局唯一, 不需要武器取负 trick);
//      排序按 id 升序即时间升序 (单关键字)
//   6. time 字段是服务器时区 (UTC+8) 本地时间字符串, 直接原样写 UIGF,
//      timezone=8 自洽; 未知区服 fallback 本机时区
//   7. 终止条件: API 返回空 list = 该池翻完 (没有 hasMore 字段)
//   8. uid: 每条记录都带, 取首个非空写入 nap[0].uid (UIGF v4.2 必填)
//
// 解析健壮性: 基底文件可能是含多游戏段的 UIGF v4.2 文件 (hk4e/hkrpg/nap 并存,
// 每段内层都有 "list"), 先定位 "nap" 再找其内层 "list"; 找不到 "nap" 才回退
// 全文件搜索 (兼容只含 ZZZ 数据的宽松第三方文件)。
//
// 继承自 Endfield v0.1.3.3 的全部加固语义:
//   - FetchPath netOk 出参: 读流中途失败 ≠ 自然结束, 截断响应不再被当完整数据
//   - fetchAborted: 翻页中途失败/重复 id (游标异常) → 整次中止不写盘, 防记录缺口
//   - 基底验收 (A2): 文件存在但读不出 "list" 结构 → 中止, 防覆盖原历史
//   - BufferedWriter ok 跟踪 + writeOk: 写失败删 tmp 不替换原文件
//   - GetFileSizeEx (64 位) + SIZE_MAX 校验; 堆上 2MB PMR arena (不清零)
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
#include <memory>           // std::make_unique_for_overwrite (C++20): 2MB PMR arena 在堆上不清零分配
#include <cstdint>          // uint8_t / SIZE_MAX
#include <array>
#include <numeric>
#include <unordered_set>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "User32.lib")

// ---------------------------------------------------------
// [枚举]
// ---------------------------------------------------------
enum class ItemType : uint8_t { Unknown = 0, Agent, WEngine, Bangboo };

inline ItemType ParseItemType(std::string_view sv) {
    // API zh-cn 直接返回中文短串, byte 级精确比较 (UTF-8 源文件)
    if (sv == "代理人") return ItemType::Agent;
    if (sv == "音擎")   return ItemType::WEngine;
    if (sv == "邦布")   return ItemType::Bangboo;
    // 防御性英文兼容 (en 语言导出的第三方基底文件)
    if (sv == "Agents"    || sv == "Agent")    return ItemType::Agent;
    if (sv == "W-Engines" || sv == "W-Engine") return ItemType::WEngine;
    if (sv == "Bangboos"  || sv == "Bangboo")  return ItemType::Bangboo;
    return ItemType::Unknown;
}

inline std::string_view ItemTypeToStr(ItemType type) {
    // 输出 UIGF 时统一用中文, 与 API zh-cn 返回值一致
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
            // \\ 处理必须是"跳 2 字节", 边界上不会越界
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

// O(N) 逐字符扫描。
// 返回值 bool —— 是否定位到了 "arrayKey": [ ... ] 数组结构 (数组为空也算定位成功)。
// 基底加载用它区分"结构正确的空数据"(正常, 0 条) 与"无结构的损坏/异类文件"
// (中止, 防止覆盖原历史)。
template<typename Callback>
bool ForEachJsonObject(std::string_view source, std::string_view arrayKey, Callback&& cb) {
    size_t pos = FindJsonKey(source, arrayKey);
    if (pos == std::string_view::npos) return false;
    pos = source.find(':', pos + arrayKey.length() + 2);
    if (pos == std::string_view::npos) return false;
    pos = source.find('[', pos);
    if (pos == std::string_view::npos) return false;

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
    return true;   // 已定位数组 (即便其中 0 个对象)
}

// nap 段定位: 找到 "nap" key 则从那里起取子串 (在其内找到的第一个 "list" 即
// nap[0].list; info 块没有 list, 账号对象的 uid 也在 list 之前), 否则原样返回。
inline std::string_view ScopeToNap(std::string_view source) {
    size_t pos = FindJsonKey(source, "nap");
    return pos == std::string_view::npos ? source : source.substr(pos);
}

inline std::wstring Utf8ToWstring(std::string_view str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
    return result;
}

inline std::string_view ExtractUrlParam(std::string_view url, std::string_view key) {
    size_t pos = url.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.length();
    size_t end = url.find('&', pos);
    return (end == std::string_view::npos) ? url.substr(pos) : url.substr(pos, end - pos);
}

// ---------------------------------------------------------
// [AoS 记录 - 导出场景多字段一起访问, AoS 空间局部性更好]
// 全部 string_view 指向 networkPayloads (deque<string>) 内字节;
// deque emplace_back 不失效已有指针。
// ---------------------------------------------------------
struct ExportRecord {
    long long safe_id;           // 19 位全局递增 ID (全局唯一, 去重键 + 排序键)
    long long timestamp;         // 秒级时间戳 fallback (主路径用 timeStr)
    std::string_view poolId;     // UIGF gacha_type: "1"/"2"/"3"/"5"
    std::string_view item_id;
    std::string_view name;
    ItemType item_type;
    std::string_view rank_type;  // "2"/"3"/"4" (B/A/S)
    std::string_view count;      // 一般 "1"
    std::string_view gachaId;    // API gacha_id, 池子期次 ID
    std::string_view timeStr;    // API time 字符串 "YYYY-MM-DD HH:MM:SS" (服务器时区)
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
// [FetchPath - netOk 出参: 读流中途失败 ≠ 自然结束]
// ---------------------------------------------------------
// 仅 HTTP 200 且响应体完整读毕才置 netOk=true; 任一环节 (打开/发送/接收/状态码/
// 可用量查询/读取) 失败都置 false, 由调用方按失败处理。否则截断恰好落在 list 中段
// 时, 解析端能读出 retcode:0 与若干完整记录, 部分数据会被当完整数据提交。
std::string FetchPath(HINTERNET hConnect, const std::wstring& path, bool& netOk) {
    netOk = false;
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
        DWORD status = 0, statusSize = sizeof(status);
        ok = WinHttpQueryHeaders(hRequest,
                                 WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX) &&
             status == 200;
    }

    if (ok) {
        // 固定 16KB 复用缓冲分块读完
        std::array<char, 16384> readBuf;
        bool readFailed = false;
        bool reading = true;
        while (reading) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &available)) { readFailed = true; break; }
            if (available == 0) break;   // 自然结束 (与查询失败区分开)
            while (available > 0) {
                DWORD bufSz = (DWORD)readBuf.size();
                DWORD chunk  = (available < bufSz) ? available : bufSz;
                DWORD downloaded = 0;
                if (!WinHttpReadData(hRequest, readBuf.data(), chunk, &downloaded) ||
                    downloaded == 0) {
                    readFailed = true;   // 读失败 / 流提前终止: 不可当自然结束
                    reading = false;
                    break;
                }
                response.append(readBuf.data(), downloaded);
                available -= downloaded;
            }
        }
        netOk = !readFailed;
    }
    WinHttpCloseHandle(hRequest);
    return response;
}

// 4 个频段配置 (real_gacha_type 同时也是 UIGF gacha_type)
struct PoolConfig {
    std::string realGachaType;    // API 参数 real_gacha_type
    std::string initLogBaseType;  // API 参数 init_log_gacha_base_type (一般同上)
    std::string uigfGachaType;    // 写入 UIGF 的 gacha_type
    std::string displayName;
};

// ---------------------------------------------------------
// [BufferedWriter - 析构 RAII Flush + 短写/失败检查]
// ---------------------------------------------------------
struct BufferedWriter {
    HANDLE hFile;
    char buf[65536];
    DWORD pos = 0;
    bool ok = true;   // 一旦写失败置 false: 后续 Flush/Write 短路, 调用方据此决定是否提交结果

    explicit BufferedWriter(HANDLE h) : hFile(h) {}
    ~BufferedWriter() { Flush(); }

    BufferedWriter(const BufferedWriter&) = delete;
    BufferedWriter& operator=(const BufferedWriter&) = delete;

    bool Flush() {
        if (!ok) return false;
        if (hFile == INVALID_HANDLE_VALUE) { ok = false; return false; }
        DWORD offset = 0;
        while (offset < pos) {
            DWORD written = 0;
            if (!WriteFile(hFile, buf + offset, pos - offset, &written, nullptr) ||
                written == 0) {
                ok = false;
                return false;
            }
            offset += written;
        }
        pos = 0;
        return true;
    }
    void Write(const char* data, DWORD len) {
        if (!ok) return;
        while (len > 0) {
            DWORD space = sizeof(buf) - pos;
            DWORD chunk = (len < space) ? len : space;
            std::memcpy(buf + pos, data, chunk);
            pos += chunk; data += chunk; len -= chunk;
            if (pos == sizeof(buf) && !Flush()) return;
        }
    }
    void Write(std::string_view sv) { Write(sv.data(), (DWORD)sv.size()); }

    template<size_t N>
    void WriteLit(const char (&s)[N]) {
        if (!ok) return;
        constexpr DWORD len = N - 1;
        if (pos + len > sizeof(buf) && !Flush()) return;
        std::memcpy(buf + pos, s, len);
        pos += len;
    }

    // WriteKV 的 val 全部来自 ExtractJsonValue 的【原始转义形态】视图 (扫描器不解码
    // 转义, 返回引号之间的原文), 本就是合法的 JSON 字符串内容, 必须【原样写出】。
    // 再转义一遍会把 `\` 翻倍, 破坏往返幂等。
    void WriteKV(std::string_view key, std::string_view val) {
        WriteLit("            \"");
        Write(key);
        WriteLit("\": \"");
        Write(val);          // 原始转义形态, 原样写出
        WriteLit("\"");
    }

    void WriteTimeKV(std::string_view key, long long ms_ts) {
        time_t t = ms_ts / 1000;
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

    // 16KB 输入缓冲: authkey 是约 1~2KB 的 percent-encoded 串, 加上完整 URL 其余
    // 参数, 1KB 缓冲必然截断 → 鉴权必败。16KB 留足余量, 并显式检测截断。
    static char urlBuffer[16384];
    printf("请输入您的绝区零抽卡记录完整链接 (含 authkey 参数, 来自游戏内抽卡记录页面):\n> ");
    if (!fgets(urlBuffer, sizeof(urlBuffer), stdin)) return 1;
    if (!strchr(urlBuffer, '\n') && strlen(urlBuffer) == sizeof(urlBuffer) - 1) {
        // 缓冲读满且没有换行 = 输入被截断, authkey 不完整, 继续必然鉴权失败
        printf("错误: 链接过长被截断 (超过 %zu 字节)。请确认复制的是单条 getGachaLog 链接。\n",
               sizeof(urlBuffer) - 1);
        system("pause");
        return 1;
    }

    std::string_view inputUrl(urlBuffer);
    while (!inputUrl.empty() &&
           (inputUrl.back() == ' ' || inputUrl.back() == '\n' ||
            inputUrl.back() == '\r' || inputUrl.back() == '\t')) {
        inputUrl.remove_suffix(1);
    }

    // ZZZ 鉴权三件套: authkey + authkey_ver + sign_type, 整段透传
    auto authkey = ExtractUrlParam(inputUrl, "authkey=");
    if (authkey.empty()) {
        printf("错误: 无法提取 authkey。请确认链接含 authkey= 参数。\n");
        system("pause");
        return 1;
    }
    auto authkeyVer = ExtractUrlParam(inputUrl, "authkey_ver=");
    auto signType   = ExtractUrlParam(inputUrl, "sign_type=");
    std::string authkeyVerStr = authkeyVer.empty() ? std::string("1") : std::string(authkeyVer);
    std::string signTypeStr   = signType.empty()   ? std::string("2") : std::string(signType);

    // game_biz / region 标识区服, 透传; 不同区服走不同 host
    auto gameBiz = ExtractUrlParam(inputUrl, "game_biz=");
    auto region  = ExtractUrlParam(inputUrl, "region=");
    std::string gameBizStr = gameBiz.empty() ? std::string("nap_global") : std::string(gameBiz);
    std::string regionStr(region);

    // 选择 host:
    //   nap_cn -> public-operation-nap.mihoyo.com (国服)
    //   其它   -> public-operation-nap-sg.hoyoverse.com (国际服)
    std::wstring hostName;
    if (gameBizStr == "nap_cn") {
        hostName = L"public-operation-nap.mihoyo.com";
        printf("\n已自动识别区服: 国服 (mihoyo)\n");
    } else {
        hostName = L"public-operation-nap-sg.hoyoverse.com";
        printf("\n已自动识别区服: 国际服 (hoyoverse)\n");
    }

    // 4 个频段. 顺序: 独家 / 音擎 / 邦布 / 常驻 (用户最关心的先抓;
    // 去重靠全局唯一 id, 顺序不影响正确性)
    std::vector<PoolConfig> pools = {
        {"2", "2", "2", "独家频段 - 代理人 UP"},
        {"3", "3", "3", "音擎频段 - 武器 UP"},
        {"5", "5", "5", "邦布频段"},
        {"1", "1", "1", "常驻频段"},
    };

    // PMR: 2MB 单调缓冲池在【堆】上 (make_unique_for_overwrite 不清零)。
    //   - 没显式指定 upstream → 默认 get_default_resource(): 记录数远超
    //     reserve(10000) 把 2MB 用尽时 fallback 到堆而非崩溃 (有意为之)。
    //   - 生命周期: arena → pool → alloc 顺序声明, 析构逆序, pool 引用的
    //     arena 内存在 pool 存活期间始终有效。
    constexpr size_t kArenaSize = 2 * 1024 * 1024;
    auto arena = std::make_unique_for_overwrite<std::byte[]>(kArenaSize);
    std::pmr::monotonic_buffer_resource pool(arena.get(), kArenaSize);
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);

    // AoS 记录
    std::pmr::vector<ExportRecord> records(alloc);
    records.reserve(10000);

    std::deque<std::string> networkPayloads;

    // 去重: unordered_set O(1)
    std::pmr::unordered_set<long long> local_safe_ids(alloc);
    local_safe_ids.reserve(10000);

    std::string uigfFilename = "uigf_zzz.json";
    std::string uid;   // 真实 uid: 基底文件 nap[0].uid 或 API 记录里的 uid, 先到先得

    // ---- 读取本地老记录 (读完立即释放句柄, 避免锁住目标文件) ----
    // 基底验收口径 (A2): 文件【不存在】= 全新拉取 (正常); 文件存在但打不开 /
    // 0 字节 / 映射失败 / 找不到 "list" 数组结构 = 按损坏处理, 中止且不写盘,
    // 防止运行结束时 MoveFileEx 覆盖原历史; "list" 数组存在但为空 = 结构正确的
    // 空数据, 0 条正常继续。
    bool baseFileExists = false;   // 文件存在 (无论能否读)
    bool baseLoadOk     = false;   // 打开 + 映射 + 结构 ("list" 数组) 三关全过
    {
        FileHandle hFile;
        hFile.h = CreateFileA(uigfFilename.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile.h != INVALID_HANDLE_VALUE) {
            baseFileExists = true;
            // GetFileSizeEx (64 位) + size_t 上界校验
            LARGE_INTEGER fileSize64{};
            if (GetFileSizeEx(hFile, &fileSize64) &&
                fileSize64.QuadPart > 0 &&
                static_cast<unsigned long long>(fileSize64.QuadPart) <=
                    static_cast<unsigned long long>(SIZE_MAX)) {
                size_t fileSize = static_cast<size_t>(fileSize64.QuadPart);
                MappingHandle hMap;
                hMap.h = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
                if (hMap.h) {
                    MapView view;
                    view.p = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
                    if (view.p) {
                        // 把 mmap 数据复制到 networkPayloads, 让 string_view 指向
                        // deque 里的 string (deque push_back 不失效指针), 然后立即
                        // 关闭 mmap/file 句柄, 不锁目标文件 (后续 MoveFileExA 需要)。
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

                        // UIGF v4.2: 先定位 "nap" 段再找其内层 "list"
                        // (多游戏 UIGF 文件防串台); 找不到 "nap" 回退全文件搜索。
                        std::string_view napScope = ScopeToNap(bufferView);

                        // nap[0].uid 在账号对象头部 (list 之前)。
                        // UIGF 允许 uid 为 string 或 integer, 两种都试。
                        {
                            std::string_view u = ExtractJsonValue(napScope, "uid", true);
                            if (u.empty()) u = ExtractJsonValue(napScope, "uid", false);
                            if (!u.empty()) uid.assign(u);
                        }

                        baseLoadOk = ForEachJsonObject(napScope, "list", [&](std::string_view itemStr) {
                            std::string_view raw_id = ExtractJsonValue(itemStr, "id", true);
                            long long parsed_id = 0, parsed_ts = 0;
                            if (!raw_id.empty()) {
                                std::from_chars(raw_id.data(), raw_id.data() + raw_id.size(), parsed_id);
                            }
                            // gacha_ts 是自定义 fallback 字段, 不一定有 (秒级);
                            // 主路径是 time 字符串
                            std::string_view tsStr = ExtractJsonValue(itemStr, "gacha_ts", true);
                            if (!tsStr.empty()) {
                                std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), parsed_ts);
                            }

                            ItemType it = ParseItemType(ExtractJsonValue(itemStr, "item_type", true));

                            records.push_back(ExportRecord{
                                parsed_id,
                                parsed_ts,
                                ExtractJsonValue(itemStr, "gacha_type", true),
                                ExtractJsonValue(itemStr, "item_id",   true),
                                ExtractJsonValue(itemStr, "name",      true),
                                it,
                                ExtractJsonValue(itemStr, "rank_type", true),
                                ExtractJsonValue(itemStr, "count",     true),
                                ExtractJsonValue(itemStr, "gacha_id",  true),
                                ExtractJsonValue(itemStr, "time",      true)
                            });
                            local_safe_ids.insert(parsed_id);
                        });
                    }
                }
            }
            if (baseLoadOk) {
                printf("成功加载本地存储的 %zu 条抽卡记录。\n", records.size());
            }
        } else {
            DWORD openErr = GetLastError();
            if (openErr == ERROR_FILE_NOT_FOUND || openErr == ERROR_PATH_NOT_FOUND) {
                printf("未发现本地记录,将创建新文件。\n");
            } else {
                baseFileExists = true;   // 存在但打不开 (占用/权限): 按"存在但不可用"走下方中止
            }
        }
    }  // <- Guard 全部析构, 文件完全释放

    if (baseFileExists && !baseLoadOk) {
        printf("[错误] 本地记录文件 %s 存在, 但无法读取或不含 \"list\" 数组结构\n", uigfFilename.c_str());
        printf("       (0 字节、被占用、已损坏或非本工具格式)。\n");
        printf("       为防止本次运行结束时覆盖原有历史, 已中止。请检查或移走该文件后重试。\n");
        system("pause");
        return 1;
    }

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
    if (!hConnect.h) { printf("网络初始化失败!\n"); system("pause"); return 1; }

    // sessionIds: O(1) 去重
    std::pmr::unordered_set<long long> sessionIds(alloc);
    sessionIds.reserve(2000);

    std::string authkeyStr(authkey);

    // 翻页中途异常停止保护。本池已吃进部分新记录后再异常停止时, 若照常写盘,
    // 部分新记录一旦落地, 下次增量拉取在最新记录处即触达老记录而停 —— 中间缺失
    // 的更早页【永远不会回补】。置位后整次更新中止、不写盘。
    bool fetchAborted = false;

    for (const auto& poolCfg : pools) {
        printf("\n>>> 正在抓取 [%s] ...\n", poolCfg.displayName.c_str());
        bool hasMore = true, reachedExisting = false;
        // 米哈游翻页游标: 上一页最后一条的 id【字符串】。直接字符串透传,
        // 不经 long long 中转 (19 位 id 接近有符号 64 位上限, 字符串最稳)。
        std::string endIdCursor = "0";
        int poolFetchedCount = 0;

        while (hasMore && !reachedExisting) {
            // size=20 是米哈游 API 上限
            std::string currentPath =
                "/common/gacha_record/api/getGachaLog?"
                "authkey_ver=" + authkeyVerStr +
                "&sign_type=" + signTypeStr +
                "&authkey=" + authkeyStr +
                "&lang=zh-cn"
                "&game_biz=" + gameBizStr +
                (regionStr.empty() ? std::string() : "&region=" + regionStr) +
                "&real_gacha_type=" + poolCfg.realGachaType +
                "&init_log_gacha_base_type=" + poolCfg.initLogBaseType +
                "&size=20"
                "&end_id=" + endIdCursor;

            bool netOk = false;
            networkPayloads.emplace_back(FetchPath(hConnect, Utf8ToWstring(currentPath), netOk));
            std::string_view resView = networkPayloads.back();

            // 三个异常分支: 页 1 失败 (poolFetchedCount == 0, 本池无部分状态, 无缺口
            // 风险) 维持宽松跳池; 翻页中途失败升级为整次中止 (见 fetchAborted 注释)。
            if (!netOk || resView.empty()) {
                printf("  [错误] 网络请求失败、响应不完整或 authkey 已失效。\n");
                if (poolFetchedCount > 0) fetchAborted = true;
                break;
            }

            // 米哈游统一返回 {"retcode": 0, "message": "OK", "data": {...}}
            std::string_view codeStr = ExtractJsonValue(resView, "retcode", false);
            if (codeStr.empty()) {
                printf("  [错误] 接口返回了非 JSON 数据或格式异常。\n");
                if (poolFetchedCount > 0) fetchAborted = true;
                break;
            }
            if (codeStr != "0") {
                // 常见: retcode=-101 authkey timeout (有效期约 24 小时)
                auto msgStr = ExtractJsonValue(resView, "message", true);
                printf("  [提示] 接口返回信息 (retcode=%.*s): %.*s\n",
                       (int)codeStr.size(), codeStr.data(),
                       (int)msgStr.size(), msgStr.data());
                if (poolFetchedCount > 0) fetchAborted = true;
                break;
            }

            std::string lastIdInPage;
            int itemsSeen = 0;
            ForEachJsonObject(resView, "list", [&](std::string_view itemStr) {
                if (reachedExisting || !hasMore) return;
                ++itemsSeen;

                // ZZZ 用 "id": 19 位全局递增数字字符串, 全局唯一, 直接做去重键
                std::string_view rawIdStr = ExtractJsonValue(itemStr, "id", true);
                if (rawIdStr.empty()) return;
                lastIdInPage.assign(rawIdStr);   // 下一页 end_id 游标 (字符串透传)

                long long parsed_id = 0;
                std::from_chars(rawIdStr.data(), rawIdStr.data() + rawIdStr.size(), parsed_id);

                if (local_safe_ids.contains(parsed_id)) {
                    reachedExisting = true;
                    printf("  * 触达本地老记录 (ID: %lld),停止追溯。\n", parsed_id);
                    return;
                }
                if (sessionIds.contains(parsed_id)) {
                    printf("\n  [警告] 遇到重复数据 (ID: %lld),防死循环中止。\n", parsed_id);
                    hasMore = false;
                    // 重复数据 = 分页游标异常 (服务器返回未推进)。已吃进部分新记录时
                    // 与下方未拉取的历史之间存在缺口, 升级为整次中止。
                    if (poolFetchedCount > 0) fetchAborted = true;
                    return;
                }
                sessionIds.insert(parsed_id);

                // 取首个非空 uid (每条记录都带; 用于 nap[0].uid)
                if (uid.empty()) {
                    std::string_view u = ExtractJsonValue(itemStr, "uid", true);
                    if (u.empty()) u = ExtractJsonValue(itemStr, "uid", false);
                    if (!u.empty()) uid.assign(u);
                }

                // 时间戳 fallback (部分实现返回 time_stamp / gacha_ts; 主路径用 time 字符串)
                long long parsed_ts = 0;
                std::string_view tsStr = ExtractJsonValue(itemStr, "time_stamp", true);
                if (tsStr.empty()) tsStr = ExtractJsonValue(itemStr, "gacha_ts", true);
                if (!tsStr.empty()) {
                    std::from_chars(tsStr.data(), tsStr.data() + tsStr.size(), parsed_ts);
                }

                ExportRecord rec;
                rec.safe_id   = parsed_id;
                rec.timestamp = parsed_ts;
                // 用我们配置的 UIGF gacha_type (与 real_gacha_type 一致), 不从响应读 —
                // 部分实现响应里的 gacha_type 可能是派生值, 我们要 UIGF 标准池子分类
                rec.poolId    = poolCfg.uigfGachaType;
                rec.item_id   = ExtractJsonValue(itemStr, "item_id",   true);
                rec.name      = ExtractJsonValue(itemStr, "name",      true);
                rec.item_type = ParseItemType(ExtractJsonValue(itemStr, "item_type", true));
                rec.rank_type = ExtractJsonValue(itemStr, "rank_type", true);
                rec.count     = ExtractJsonValue(itemStr, "count",     true);
                rec.gachaId   = ExtractJsonValue(itemStr, "gacha_id",  true);
                rec.timeStr   = ExtractJsonValue(itemStr, "time",      true);  // 服务器时区 (UTC+8)

                records.push_back(std::move(rec));
                poolFetchedCount++;
                // rank_type: 2/3/4 = B/A/S, 直接打印 API 原字符串
                printf("  获取到: %.*s (rank_type=%.*s)\n",
                    (int)records.back().name.size(),      records.back().name.data(),
                    (int)records.back().rank_type.size(), records.back().rank_type.data());
            });

            if (reachedExisting || !hasMore) break;

            // 米哈游 API 没有 hasMore 字段: 空 list (itemsSeen==0, 含 "list":[])
            // 或本页没有可用 id → 该池翻完
            if (itemsSeen == 0 || lastIdInPage.empty()) break;

            endIdCursor = lastIdInPage;
            Sleep(500);   // 同池翻页间隔 (米哈游接口对单 IP 限流较严)
        }
        if (fetchAborted) {
            printf(">>> [%s] 拉取在翻页中途异常停止。\n", poolCfg.displayName.c_str());
            break;   // 后续池无需再拉, 本次整体不写盘
        }
        printf(">>> [%s] 抓取完成,本次新增拉取: %d 条。\n",
               poolCfg.displayName.c_str(), poolFetchedCount);
        Sleep(800);       // 换池间隔
    }

    if (fetchAborted) {
        printf("\n========================================\n");
        printf("本次拉取在翻页中途异常停止: 为避免写入带缺口的记录历史, 本次【不写盘】,\n");
        printf("原记录文件保持原样。请稍后重新运行以完整拉取。\n");
        system("pause");
        return 1;
    }

    printf("\n========================================\n");
    printf("已完成全部抓取!总计新增拉取了 %zu 条记录。\n", sessionIds.size());

    // 排序: ZZZ 的 id 是 19 位全局递增, 单关键字升序 = 时间升序。
    // (不需要终末地的"角色/武器分区 + 时间 + |id|"三级排序 —— ZZZ id 全局唯一且单调。)
    std::ranges::sort(records, [](const ExportRecord& a, const ExportRecord& b) {
        return a.safe_id < b.safe_id;
    });

    time_t rawtime; time(&rawtime);
    long long export_ts = (long long)rawtime;

    // 安全写入: tmp → 替换
    std::string tempFilename = uigfFilename + ".tmp";
    HANDLE hOut = CreateFileA(tempFilename.c_str(), GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hOut != INVALID_HANDLE_VALUE) {
        bool writeOk = false;   // 写出是否全部成功; 失败则不替换原文件
        {
            BufferedWriter w(hOut);
            char numBuf[32];

            // ==========================================================
            // UIGF v4.2 输出 - 绝区零 (nap)
            // ----------------------------------------------------------
            // 文档地址: https://uigf.org/standards/UIGF.html
            //
            // ZZZ 是 UIGF v4.2 官方支持的米哈游游戏 (hk4e/hkrpg/nap/hk4e_ugc),
            // 顶层 key 用标准 "nap"。
            //
            // 顶层结构:
            //   { "info": { ... v4.2 公共字段 ... },
            //     "nap": [ { "uid", "timezone", "lang", "list": [ ... ] } ] }
            //
            // nap 账号级必填: uid / timezone / list
            // list 元素必填: gacha_type / item_id / time / id
            // 推荐字段: gacha_id / count / name / item_type / rank_type
            //
            // 时区: 米哈游服务器时区 UTC+8, API time 字符串即服务器本地时间 →
            // nap_cn / nap_global 固定写 8 与 time 自洽; 未知区服 fallback 本机偏移。
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
            w.WriteLit("        \"export_app\": \"ZZZ Gacha Exporter\",\n"
                       "        \"export_app_version\": \"v1.0.0\",\n"
                       "        \"version\": \"v4.2\",\n");
            // export_time 不在 v4.2 必需字段里, 保留作人类可读辅助信息
            w.WriteLit("        \"export_time\": \""); w.Write(tbuf, tlen); w.WriteLit("\"\n    },\n");

            // ---- nap 数组 (单账号 → 单元素) ----
            // timezone: nap_cn / nap_global 固定 8 (米哈游服务器时区, 与 time
            // 字符串自洽); 其它未知区服 fallback 本机偏移 (Windows 上没有 tm_gmtoff,
            // 用 GetTimeZoneInformation; Bias 符号约定 "UTC = local + Bias",
            // 东 8 区返回 -480, 取负再除 60)。
            int tzHours = 8;
            if (gameBizStr != "nap_cn" && gameBizStr != "nap_global") {
                TIME_ZONE_INFORMATION tzi;
                DWORD tzKind = GetTimeZoneInformation(&tzi);
                LONG biasMinutes = tzi.Bias;
                if (tzKind == TIME_ZONE_ID_DAYLIGHT) biasMinutes += tzi.DaylightBias;
                else if (tzKind == TIME_ZONE_ID_STANDARD) biasMinutes += tzi.StandardBias;
                tzHours = (int)(-biasMinutes / 60);
            }

            w.WriteLit("    \"nap\": [\n        {\n");
            // uid: 真实值 (API 记录 / 基底文件提取); 为空 fallback "0"
            w.WriteKV("uid", uid.empty() ? std::string_view{"0"} : std::string_view{uid});
            w.WriteLit(",\n");
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

                // 必填: gacha_type ("1"/"2"/"3"/"5")
                w.WriteKV("gacha_type", r.poolId);          w.WriteLit(",\n");
                // 推荐: gacha_id (池子期次 ID; 基底老文件可能没有, 缺省不写)
                if (!r.gachaId.empty()) {
                    w.WriteKV("gacha_id", r.gachaId);       w.WriteLit(",\n");
                }
                // 必填: id (19 位全局唯一, 引号字符串形态)
                w.WriteI64KV("id", r.safe_id, true);        w.WriteLit(",\n");
                // 必填: item_id
                w.WriteKV("item_id", r.item_id);            w.WriteLit(",\n");
                // 推荐: count (缺省补 "1")
                w.WriteKV("count", r.count.empty() ? std::string_view{"1"} : r.count);
                w.WriteLit(",\n");
                // 必填: time —— 优先 API 原 time 字符串 (服务器时区, 已是 UIGF
                // 要求的 "YYYY-MM-DD HH:MM:SS" 格式), 与 timezone 自洽;
                // 老记录缺 time 才 fallback 秒级时间戳重建 (WriteTimeKV 收毫秒)
                if (!r.timeStr.empty()) {
                    w.WriteKV("time", r.timeStr);
                } else {
                    w.WriteTimeKV("time", r.timestamp * 1000);
                }
                w.WriteLit(",\n");
                // 推荐: name / item_type / rank_type
                w.WriteKV("name", r.name);                  w.WriteLit(",\n");
                w.WriteKV("item_type", ItemTypeToStr(r.item_type));
                w.WriteLit(",\n");
                w.WriteKV("rank_type", r.rank_type);
                w.WriteLit("\n");
                w.WriteLit("        }");
                if (i < n - 1) w.WriteLit(",");
                w.WriteLit("\n");
            }

            w.WriteLit("            ]\n        }\n    ]\n}\n");
            w.Flush();              // 显式收尾 flush 并捕获结果 (析构里那次因 pos==0 成 no-op)
            writeOk = w.ok;
        }
        CloseHandle(hOut);

        if (!writeOk) {
            // 写入中途失败 (磁盘满 / IO 错误): 绝不能用半截 tmp 覆盖好的原文件
            DeleteFileA(tempFilename.c_str());
            printf("写入失败 (磁盘空间不足或 IO 错误)!已保留原记录文件,未做替换。\n");
        } else if (MoveFileExA(tempFilename.c_str(), uigfFilename.c_str(),
                               MOVEFILE_REPLACE_EXISTING)) {
            printf("已成功更新记录并保存至: %s (UID: %s)\n",
                   uigfFilename.c_str(), uid.empty() ? "未知" : uid.c_str());
        } else {
            printf("文件覆盖失败!请手动将 %s 重命名为 %s\n",
                   tempFilename.c_str(), uigfFilename.c_str());
        }
    } else {
        printf("临时文件创建失败!请检查目录权限。\n");
    }

    system("pause");
    return 0;
}
