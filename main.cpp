// ============================================================
// ZZZ Gacha Exporter - UIGF v4.2 (nap) / 面向数据 / PMR / AoS
// ------------------------------------------------------------
// 自 Endfield Gacha Exporter (v0.1.3.3 加固版) 迁移到绝区零, 现对齐到 Endfield-Gacha v0.1.4。
//
// 绝区零数据契约 (米哈游 HoYoverse API):
//   1. 端点: /common/gacha_record/api/getGachaLog
//      翻页: end_id 游标 (上一页最后一条的 id; "0" 表示从最新开始), size 上限 20
//      鉴权: authkey + authkey_ver + sign_type; authkey 是大段 percent-encoded 串
//      (约 1~2KB), 整段透传, 不解析不重组 → 输入缓冲必须够大 (16KB)
//   2. 卡池类型 (real_gacha_type / 同时也是 UIGF 的 gacha_type 值):
//        "1" = 常驻频段(热门卡司)  "2" = 独家频段(代理人 UP)
//        "3" = 音擎频段(武器 UP)   "5" = 邦布频段
//        "102" = 独家重映频段(代理人复刻)  "103" = 音擎回响频段(音擎复刻)
//        (102 / 103 是 2.5 起新增的复刻频段, 保底各自独立)
//   3. 物品类型字段值: "代理人"/"音擎"/"邦布" (zh-cn lang)
//   4. 响应包装: {"retcode": 0, "message": "OK", "data": {...}}
//      (与鹰角的 {"code": 0, "msg": ...} 字段名不同); 信封字段只读对象本层
//   5. ID 是 19 位全局递增整数; 去重直接用 id (全局唯一, 不需要武器取负 trick);
//      排序按 id 升序即时间升序 (单关键字)
//   6. time 字段是区服服务器本地时间字符串, 直接原样写 UIGF; timezone 按区服写
//      (国服 / 亚服 / 港澳台服 +8, 欧服 +1, 美服 -5), 不取导出设备的本地时区
//   7. 终止条件: API 返回空 list = 该池翻完 (没有 hasMore 字段)
//   8. uid: 每条记录都带, 取首个非空写入 nap[0].uid (UIGF v4.2 必填)
//
// 解析健壮性: 基底文件可能是含多游戏段的 UIGF v4.2 文件 (hk4e/hkrpg/nap 并存,
// 每段内层都有 "list"), 按结构路径 根.nap[0].list 读取, 不受别的段的同名键干扰;
// 存档必须是结构完整的单账号文件, 否则中止。
//
// 继承自 Endfield 的全部加固语义 (v0.1.3.3 起, v0.1.4 补全):
//   - FetchPath netOk 出参: 读流中途失败 ≠ 自然结束, 截断响应不再被当完整数据
//   - fetchAborted: 任一频段出错 (含首页失败 / 重复 id / 游标不递减) → 整次中止不写盘
//   - 基底验收 (A2): 文件存在但不是结构完整的单账号 nap[0].list → 中止, 防覆盖原历史
//   - BufferedWriter ok 跟踪 + writeOk: 写失败删 tmp 不替换原文件
//   - 落盘: FlushFileBuffers + CloseHandle 检查, MoveFileEx 带 WRITE_THROUGH
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
#include "JsonScan.h"

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
// [共享 JSON 扫描器与结构化字段读取]
// ---------------------------------------------------------
using JsonValueKind = zzzjson::ValueKind;
using JsonValueRef = zzzjson::ValueRef;
using JsonArrayScan = zzzjson::ArrayScan;
using LocateResult = zzzjson::LocateResult;

inline JsonValueRef FindTopLevelValue(std::string_view obj, std::string_view key) {
    return zzzjson::FindMember(obj, key);
}
inline bool ParseFullInt64(std::string_view text, long long& out) {
    return zzzjson::ParseFullInt64(text, out);
}
template<typename Callback>
[[nodiscard]] JsonArrayScan ForEachObjectInArray(std::string_view text, Callback&& cb) {
    return zzzjson::ForEachObjectIn(text, std::forward<Callback>(cb));
}

// 字符串和数字都可承载整数; 有字段但解析失败与字段缺失必须分开。
inline bool ReadIntegerField(std::string_view obj, std::string_view key,
                             long long& out, bool& present) {
    const JsonValueRef v = FindTopLevelValue(obj, key);
    present = v.malformed || (v.kind != JsonValueKind::None && v.kind != JsonValueKind::Null);
    if (v.kind != JsonValueKind::String && v.kind != JsonValueKind::Number) return false;
    return ParseFullInt64(v.text, out);
}

// 保留字符串原有转义; 第三方存档使用数字或布尔字段时也不会被静默清空。
inline std::string_view ReadTextField(std::string_view obj, std::string_view key) {
    const JsonValueRef v = FindTopLevelValue(obj, key);
    if (v.kind == JsonValueKind::String || v.kind == JsonValueKind::Number ||
        v.kind == JsonValueKind::Bool) return v.text;
    return {};
}

struct UigfListLocation {
    JsonArrayScan napScan = JsonArrayScan::NotFound;
    size_t napEntries = 0;
    bool usable = false;
    std::string_view account;   // nap[0] 账号对象原文 (uid / timezone 在这一层)
    LocateResult listStatus = LocateResult::NotFound;
    std::string_view listText;
};
// 完整遍历外层账号数组, 避免读取第一个账号后覆盖掉其余账号。
inline UigfListLocation LocateUigfPullList(std::string_view doc) {
    UigfListLocation out;
    const JsonValueRef game = FindTopLevelValue(doc, "nap");
    if (game.kind != JsonValueKind::Array) {
        out.napScan = JsonArrayScan::Malformed;
        return out;
    }
    out.napScan = ForEachObjectInArray(game.text,
        [&out](std::string_view) { ++out.napEntries; });
    out.usable = out.napScan == JsonArrayScan::Ok && out.napEntries == 1;
    if (!out.usable) return out;
    const JsonValueRef entry = zzzjson::FirstElement(game.text);
    out.account = entry.text;
    const JsonValueRef list = FindTopLevelValue(entry.text, "list");
    if (list.kind == JsonValueKind::Array) {
        out.listStatus = LocateResult::Located;
        out.listText = list.text;
    } else if (list.malformed || (list.kind != JsonValueKind::None && list.kind != JsonValueKind::Null)) {
        out.listStatus = LocateResult::Malformed;
    }
    return out;
}

struct PageEnvelope {
    bool complete = false;
    bool codeFound = false;
    std::string_view code, msg;      // retcode / message 原文
    LocateResult listStatus = LocateResult::NotFound;
    std::string_view listText;
    std::string_view region;         // data.region (例如 "prod_gf_cn"), 用来确定 timezone
};
// 信封字段只读取对象本层; 记录内的同名 list/retcode 不可代替分页信封。
inline PageEnvelope InspectPageEnvelope(std::string_view doc) {
    PageEnvelope out;
    // 只查结构完整 (括号 / 引号配对闭合, 后面没有别的内容): 这一条就能挡住被截断的正文。
    out.complete = zzzjson::IsCompleteObjectDocument(doc);
    if (!out.complete) return out;
    const JsonValueRef code = FindTopLevelValue(doc, "retcode");
    out.codeFound = code.kind == JsonValueKind::String || code.kind == JsonValueKind::Number;
    if (out.codeFound) out.code = code.text;
    out.msg = ReadTextField(doc, "message");

    std::string_view host = doc;
    const JsonValueRef data = FindTopLevelValue(doc, "data");
    if (data.kind == JsonValueKind::Object) {
        host = data.text;
        out.region = ReadTextField(host, "region");
    } else if (data.kind != JsonValueKind::None && data.kind != JsonValueKind::Null) {
        out.listStatus = LocateResult::Malformed;
        return out;
    }
    JsonValueRef list = FindTopLevelValue(host, "list");
    // data 本层没有 list 时兼容根对象本层 list, 不把记录内部的同名键当作分页信封。
    if (host.data() != doc.data() && (list.kind == JsonValueKind::None || list.kind == JsonValueKind::Null)) {
        const JsonValueRef rootList = FindTopLevelValue(doc, "list");
        if (rootList.kind != JsonValueKind::None && rootList.kind != JsonValueKind::Null) list = rootList;
    }
    if (list.kind == JsonValueKind::Array) {
        out.listStatus = LocateResult::Located;
        out.listText = list.text;
    } else if (list.malformed || (list.kind != JsonValueKind::None && list.kind != JsonValueKind::Null)) {
        out.listStatus = LocateResult::Malformed;
    }
    // 未识别的嵌套记录数组不能当作空池, 也不能读取记录内部的同名键作为信封。
    if (out.listStatus == LocateResult::NotFound) {
        const auto nestedList = zzzjson::LocateArrayFullText(doc, "list");
        if (nestedList.first != LocateResult::NotFound) out.listStatus = LocateResult::Malformed;
    }
    return out;
}

// 区服 → 服务器时区 (UTC 偏移小时)。各区服的抽卡记录 time 都是服务器本地时间,
// 全年固定偏移, 不随夏令时变化。
inline bool RegionUtcOffset(std::string_view region, int& hours) {
    if (region == "prod_gf_cn" || region == "prod_gf_jp" || region == "prod_gf_sg") { hours = 8; return true; }
    if (region == "prod_gf_eu") { hours = 1;  return true; }
    if (region == "prod_gf_us") { hours = -5; return true; }
    return false;
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
    std::string_view poolId;     // UIGF gacha_type: "1"/"2"/"3"/"5"/"102"/"103"
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

// 6 个频段配置 (real_gacha_type 同时也是 UIGF gacha_type)
struct PoolConfig {
    std::string realGachaType;    // API 参数 real_gacha_type
    std::string initLogBaseType;  // API 参数 init_log_gacha_base_type (一般同上; 102/103 沿用链接原值)
    std::string uigfGachaType;    // 写入 UIGF 的 gacha_type
    std::string displayName;
};

// ---------------------------------------------------------
// [BufferedWriter - 析构 RAII Flush + 短写/失败检查]
// ---------------------------------------------------------
// 把 Unix 秒数格式化成 UTC+tzHours 的 "YYYY-MM-DD HH:MM:SS", 返回写出的长度 (无法表示时为 0)。
// export_time 与 time 的回退值都按区服服务器时区 (即 timezone 字段的偏移) 写出, 不取导出
// 设备的本地时区: 同一条记录在任何设备、任何季节写出的字符串都相同, 不受夏令时影响,
// 也始终与 timezone 字段对得上。
inline int FormatServerTime(long long seconds, int tzHours, char* buf, size_t capacity) {
    const time_t t = static_cast<time_t>(seconds + tzHours * 3600LL);
    struct tm value{};
    if (gmtime_s(&value, &t) != 0) return 0;
    const int len = std::snprintf(buf, capacity, "%04d-%02d-%02d %02d:%02d:%02d",
                                  value.tm_year + 1900, value.tm_mon + 1, value.tm_mday,
                                  value.tm_hour, value.tm_min, value.tm_sec);
    return len < 0 ? 0 : len;
}

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
    void Write(std::string_view sv) {
        // 原文视图可能超过 DWORD 上限, 分块写入以免长度窄化后静默截断。
        while (!sv.empty() && ok) {
            const DWORD len = sv.size() > MAXDWORD ? MAXDWORD : static_cast<DWORD>(sv.size());
            Write(sv.data(), len);
            sv.remove_prefix(len);
        }
    }

    template<size_t N>
    void WriteLit(const char (&s)[N]) {
        if (!ok) return;
        constexpr DWORD len = N - 1;
        if (pos + len > sizeof(buf) && !Flush()) return;
        std::memcpy(buf + pos, s, len);
        pos += len;
    }

    // WriteKV 的 val 全部来自 ReadTextField 的【原始转义形态】视图 (扫描器不解码
    // 转义, 返回引号之间的原文), 本就是合法的 JSON 字符串内容, 必须【原样写出】。
    // 再转义一遍会把 `\` 翻倍, 破坏往返幂等。
    void WriteKV(std::string_view key, std::string_view val) {
        WriteLit("            \"");
        Write(key);
        WriteLit("\": \"");
        Write(val);          // 原始转义形态, 原样写出
        WriteLit("\"");
    }

    void WriteTimeKV(std::string_view key, long long ms_ts, int tzHours) {
        char tbuf[64];
        const int len = FormatServerTime(ms_ts / 1000, tzHours, tbuf, sizeof(tbuf));
        WriteLit("            \"");
        Write(key);
        WriteLit("\": \"");
        Write(tbuf, len);
        WriteLit("\"");
    }

    void WriteI64KV(std::string_view key, long long val, bool quotes) {
        char nbuf[32];
        auto [ptr, ec] = std::to_chars(nbuf, nbuf + 32, val);
        if (ec != std::errc{}) { ok = false; return; }
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

    // 6 个频段. 顺序: 独家 / 音擎 / 独家重映 / 音擎回响 / 邦布 / 常驻 (用户最关心的先抓;
    // 去重靠全局唯一 id, 顺序不影响正确性)
    // 102 / 103 (2.5 起) 只替换 real_gacha_type, init_log_gacha_base_type 沿用链接里的原值
    // (Starward / HoYo.Gacha 请求这两个频段时都是这样); 链接里没有才填频段编号。
    const std::string linkInitBaseType(ExtractUrlParam(inputUrl, "init_log_gacha_base_type="));
    auto rerunInitType = [&](const char* own) {
        return linkInitBaseType.empty() ? std::string(own) : linkInitBaseType;
    };
    std::vector<PoolConfig> pools = {
        {"2",   "2",                  "2",   "独家频段 - 代理人 UP"},
        {"3",   "3",                  "3",   "音擎频段 - 武器 UP"},
        {"102", rerunInitType("102"), "102", "独家重映频段 - 代理人复刻"},
        {"103", rerunInitType("103"), "103", "音擎回响频段 - 音擎复刻"},
        {"5",   "5",                  "5",   "邦布频段"},
        {"1",   "1",                  "1",   "常驻频段"},
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
    bool baseLoadOk     = false;   // 打开、结构完整、单账号结构与字段解析全部通过
    long long baseTimezone = 0;    // 存档 nap[0].timezone: 认不出区服时沿用
    bool baseTimezoneKnown = false;
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

                        // 存档必须是一个结构完整的 JSON 对象且只有一个账号; 未读懂的任何内容都不回写。
                        if (zzzjson::IsCompleteObjectDocument(bufferView)) {
                            const UigfListLocation loc = LocateUigfPullList(bufferView);
                            JsonArrayScan pullScan = JsonArrayScan::Malformed;
                            size_t recordsMalformed = 0;
                            if (loc.usable && loc.listStatus == LocateResult::Located) {
                                // nap[0].uid / timezone 在账号对象本层。
                                // UIGF 允许 uid 为 string 或 integer, ReadTextField 两种都接受。
                                uid.assign(ReadTextField(loc.account, "uid"));
                                bool tzPresent = false;
                                baseTimezoneKnown = ReadIntegerField(loc.account, "timezone", baseTimezone, tzPresent);
                                pullScan = ForEachObjectInArray(loc.listText, [&](std::string_view item) {
                                    long long parsed_id = 0, parsed_ts = 0;
                                    bool idPresent = false, tsPresent = false;
                                    // gacha_ts 是自定义 fallback 字段, 不一定有 (秒级);
                                    // 主路径是 time 字符串
                                    if (!ReadIntegerField(item, "id", parsed_id, idPresent) ||
                                        (!ReadIntegerField(item, "gacha_ts", parsed_ts, tsPresent) && tsPresent)) {
                                        ++recordsMalformed;
                                        return;
                                    }
                                    records.push_back(ExportRecord{
                                        parsed_id,
                                        parsed_ts,
                                        ReadTextField(item, "gacha_type"),
                                        ReadTextField(item, "item_id"),
                                        ReadTextField(item, "name"),
                                        ParseItemType(ReadTextField(item, "item_type")),
                                        ReadTextField(item, "rank_type"),
                                        ReadTextField(item, "count"),
                                        ReadTextField(item, "gacha_id"),
                                        ReadTextField(item, "time")
                                    });
                                    local_safe_ids.insert(parsed_id);
                                });
                            }
                            baseLoadOk = loc.usable && pullScan == JsonArrayScan::Ok && recordsMalformed == 0;
                        }
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
        printf("[错误] 本地记录文件 %s 存在, 但无法读取, 或结构不是 UIGF v4.2 的\n", uigfFilename.c_str());
        printf("       单账号 nap[0].list 数组 (结构不完整、多账号或字段无法完整解析)。\n");
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
    std::string apiRegion;   // 响应 data.region, 用来确定 timezone

    // 任一频段的任何错误都使本次更新作废, 原存档直到完整成功前都不替换。
    bool fetchAborted = false;
    size_t newPulls = 0;
    for (const auto& poolCfg : pools) {
        printf("\n>>> 正在抓取 [%s] ...\n", poolCfg.displayName.c_str());
        bool reachedExisting = false;
        // 米哈游翻页游标: 上一页最后一条的 id【字符串】。直接字符串透传,
        // 不经 long long 中转 (19 位 id 接近有符号 64 位上限, 字符串最稳)。
        std::string endIdCursor = "0";
        long long cursorId = 0;   // endIdCursor 的数值, 只用来确认游标严格递减
        int page = 1;
        size_t poolPulls = 0;
        auto fail = [&](const char* reason) {
            printf("  [错误] %s\n", reason);
            fetchAborted = true;
        };

        while (!reachedExisting) {
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
            if (!netOk || resView.empty()) {
                fail("网络请求失败、响应不完整或 authkey 已失效。");
                break;
            }
            if (resView.size() >= 3 && (uint8_t)resView[0] == 0xEF &&
                (uint8_t)resView[1] == 0xBB && (uint8_t)resView[2] == 0xBF)
                resView.remove_prefix(3);

            // 米哈游统一返回 {"retcode": 0, "message": "OK", "data": {...}}
            const PageEnvelope env = InspectPageEnvelope(resView);
            if (!env.complete) {
                fail("接口正文不是完整的 JSON 对象 (多半是传输被截断)。");
                break;
            }
            if (!env.codeFound || env.code != "0") {
                // 常见: retcode=-101 authkey timeout (有效期约 24 小时)
                printf("  [接口信息] (retcode=%.*s) %.*s\n",
                       (int)env.code.size(), env.code.data(),
                       (int)env.msg.size(), env.msg.data());
                fail("响应缺少业务码或接口报告业务错误。");
                break;
            }
            if (env.listStatus == LocateResult::Malformed) {
                fail("接口记录数组类型异常。");
                break;
            }
            if (apiRegion.empty() && !env.region.empty()) apiRegion.assign(env.region);

            std::string lastIdInPage;
            long long lastIdParsed = 0;
            size_t itemsSeen = 0;
            JsonArrayScan pageScan = JsonArrayScan::NotFound;
            if (env.listStatus == LocateResult::Located) {
                pageScan = ForEachObjectInArray(env.listText, [&](std::string_view item) {
                    if (fetchAborted) return;
                    ++itemsSeen;

                    // ZZZ 用 "id": 19 位全局递增数字字符串, 全局唯一, 直接做去重键
                    long long parsed_id = 0, parsed_ts = 0;
                    bool idPresent = false, tsPresent = false;
                    if (!ReadIntegerField(item, "id", parsed_id, idPresent) || parsed_id <= 0) {
                        fail("记录的 id 缺失或不是完整的正整数。");
                        return;
                    }
                    // 时间戳 fallback (部分实现返回 time_stamp / gacha_ts; 主路径用 time 字符串)
                    bool tsOk = ReadIntegerField(item, "time_stamp", parsed_ts, tsPresent);
                    if (!tsPresent) tsOk = ReadIntegerField(item, "gacha_ts", parsed_ts, tsPresent);
                    if (!tsOk && tsPresent) {
                        fail("记录的时间戳不是完整的整数。");
                        return;
                    }
                    lastIdInPage.assign(ReadTextField(item, "id"));   // 下一页 end_id 游标 (字符串透传)
                    lastIdParsed = parsed_id;
                    // 找到本地边界后仍校验本页余下记录的 id / 时间戳, 任意错误都不能按成功收尾。
                    if (reachedExisting) return;
                    if (local_safe_ids.contains(parsed_id)) {
                        reachedExisting = true;
                        printf("  * 触达本地老记录 (ID: %lld),停止追溯。\n", parsed_id);
                        return;
                    }
                    if (sessionIds.contains(parsed_id)) {
                        fail("同一会话遇到重复 id, 分页可能未前进。");
                        return;
                    }
                    sessionIds.insert(parsed_id);

                    // 取首个非空 uid (每条记录都带; 用于 nap[0].uid)
                    if (uid.empty()) uid.assign(ReadTextField(item, "uid"));

                    ExportRecord rec{};
                    rec.safe_id   = parsed_id;
                    rec.timestamp = parsed_ts;
                    // 用我们配置的 UIGF gacha_type (与 real_gacha_type 一致), 不从响应读 —
                    // 部分实现响应里的 gacha_type 可能是派生值, 我们要 UIGF 标准池子分类
                    rec.poolId    = poolCfg.uigfGachaType;
                    rec.item_id   = ReadTextField(item, "item_id");
                    rec.name      = ReadTextField(item, "name");
                    rec.item_type = ParseItemType(ReadTextField(item, "item_type"));
                    rec.rank_type = ReadTextField(item, "rank_type");
                    rec.count     = ReadTextField(item, "count");
                    rec.gachaId   = ReadTextField(item, "gacha_id");
                    rec.timeStr   = ReadTextField(item, "time");  // 区服服务器本地时间

                    records.push_back(rec);
                    ++poolPulls; ++newPulls;
                    // rank_type: 2/3/4 = B/A/S, 直接打印 API 原字符串
                    printf("  获取到: %.*s (rank_type=%.*s)\n",
                           (int)rec.name.size(), rec.name.data(),
                           (int)rec.rank_type.size(), rec.rank_type.data());
                });
            }
            if (fetchAborted) break;
            if (pageScan == JsonArrayScan::Malformed) {
                fail("接口记录数组结构异常 (含非对象元素或分隔符异常)。");
                break;
            }
            if (env.listStatus == LocateResult::NotFound &&
                (page > 1 || poolPulls > 0)) {
                fail("翻页中途返回的这一页缺少记录数组。");
                break;
            }
            // 米哈游 API 没有 hasMore 字段: 空 list (itemsSeen==0, 含 "list":[]) → 该池翻完。
            // 合法空池/空数组和零新增都是正常完成。
            if (itemsSeen == 0) break;
            if (lastIdParsed <= 0 ||
                (page > 1 && lastIdParsed >= cursorId)) {
                fail("分页游标没有严格递减。");
                break;
            }
            if (reachedExisting) break;

            endIdCursor = lastIdInPage;
            cursorId = lastIdParsed;
            ++page;
            Sleep(500);   // 同池翻页间隔 (米哈游接口对单 IP 限流较严)
        }
        if (fetchAborted) {
            printf(">>> [%s] 抓取失败。\n", poolCfg.displayName.c_str());
            break;
        }
        printf(">>> [%s] 抓取完成,本次新增拉取: %zu 条。\n",
               poolCfg.displayName.c_str(), poolPulls);
        Sleep(800);       // 换池间隔
    }

    if (fetchAborted) {
        printf("\n========================================\n");
        printf("本次拉取发生错误: 已放弃整次更新, 本次【不写盘】,原记录文件保持原样。\n");
        printf("请处理错误或稍后重试, 以完整拉取。\n");
        system("pause");
        return 1;
    }
    printf("\n========================================\n");
    printf("已完成全部抓取!新增 %zu 条抽卡记录; 共计 %zu 条抽卡记录。\n", newPulls, records.size());

    // timezone 必须与 time 字符串对应: time 是区服服务器本地时间, 所以按区服定。
    // 响应里有 region 就用它, 没有才用链接参数; 认不出才沿用存档原值, 再不行按 +8。
    int tzHours = 8;
    const std::string_view tzRegion = apiRegion.empty() ? std::string_view(regionStr) : std::string_view(apiRegion);
    if (!RegionUtcOffset(tzRegion, tzHours) && baseTimezoneKnown) tzHours = (int)baseTimezone;

    // 排序: ZZZ 的 id 是 19 位全局递增, 单关键字升序 = 时间升序。
    // (不需要终末地的"角色/武器分区 + 时间 + |id|"三级排序 —— ZZZ id 全局唯一且单调。)
    std::ranges::sort(records, [](const ExportRecord& a, const ExportRecord& b) {
        return a.safe_id < b.safe_id;
    });

    time_t rawtime = time(nullptr);
    char exportTime[64];
    const int exportTimeLen = FormatServerTime(static_cast<long long>(rawtime), tzHours,
                                               exportTime, sizeof(exportTime));
    const long long export_ts = static_cast<long long>(rawtime);
    bool exportOk = false;

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
            // gacha_type 写 real_gacha_type 原值。UIGF v4.2 的 nap 枚举目前只列了 1/2/3/5;
            // 2.5 新增的独家重映 / 音擎回响按 HoYo.Gacha 的做法原样写 "102" / "103" ——
            // 规范收录之前, 不认识这两个值的工具可能跳过这部分记录。
            //
            // 时区: time 是区服服务器本地时间, timezone 写区服偏移 (见上方 tzHours),
            // export_time 与 time 的回退值也按同一偏移格式化, 三者自洽。
            // ==========================================================

            // ---- info 块 ----
            w.WriteLit("{\n    \"info\": {\n");
            w.WriteLit("        \"export_timestamp\": ");
            auto [ptr, ec] = std::to_chars(numBuf, numBuf + 32, export_ts);
            w.Write(numBuf, (DWORD)(ptr - numBuf));
            w.WriteLit(",\n");
            w.WriteLit("        \"export_app\": \"ZZZ Gacha Exporter\",\n"
                       "        \"export_app_version\": \"v1.1.0\",\n"
                       "        \"version\": \"v4.2\",\n");
            // export_time 不在 v4.2 必需字段里, 保留作人类可读辅助信息
            w.WriteLit("        \"export_time\": \""); w.Write(exportTime, (DWORD)exportTimeLen); w.WriteLit("\"\n    },\n");

            // ---- nap 数组 (单账号 → 单元素) ----
            // timezone 按区服写 (见上方 tzHours): 记录的 time 就是该区服的服务器本地时间, 两者一致。
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

                // 必填: gacha_type ("1"/"2"/"3"/"5"/"102"/"103")
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
                // 老记录缺 time 才 fallback 秒级时间戳重建 (WriteTimeKV 收毫秒, 按 tzHours 格式化)
                if (!r.timeStr.empty()) {
                    w.WriteKV("time", r.timeStr);
                } else {
                    w.WriteTimeKV("time", r.timestamp * 1000, tzHours);
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
        // 缓冲区写完后确认系统落盘与关闭成功, 才可替换原文件。
        if (writeOk && !FlushFileBuffers(hOut)) writeOk = false;
        if (!CloseHandle(hOut)) writeOk = false;

        if (!writeOk) {
            // 写入中途失败 (磁盘满 / IO 错误): 绝不能用半截 tmp 覆盖好的原文件
            DeleteFileA(tempFilename.c_str());
            printf("写入失败 (磁盘空间不足或 IO 错误)!已保留原记录文件,未做替换。\n");
        } else if (MoveFileExA(tempFilename.c_str(), uigfFilename.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            exportOk = true;
            printf("已成功更新记录并保存至: %s (UID: %s)\n",
                   uigfFilename.c_str(), uid.empty() ? "未知" : uid.c_str());
        } else {
            DeleteFileA(tempFilename.c_str());
            printf("文件替换失败!本次更新已放弃, 原记录文件保持原样。\n");
        }
    } else {
        printf("临时文件创建失败!请检查目录权限。\n");
    }

    system("pause");
    return exportOk ? 0 : 1;
}
