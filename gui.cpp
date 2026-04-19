// ============================================================
// ZZZ Gacha Visualizer - Win32 + GDI+ + PMR / SoA 热路径
// ============================================================
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <charconv>
#include <ranges>
#include <memory_resource>
#include <array>
#include <cstdint>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------
// [枚举降维]
// ---------------------------------------------------------
enum class GachaType : uint8_t { Unknown = 0, Standard = 1, Exclusive = 2, WEngine = 3, Bangboo = 5 };
enum class RankType  : uint8_t { Unknown = 0, RankA = 3, RankS = 4 };

inline GachaType ParseGachaType(std::string_view sv) {
    if (sv == "2") return GachaType::Exclusive;
    if (sv == "3") return GachaType::WEngine;
    if (sv == "1") return GachaType::Standard;
    if (sv == "5") return GachaType::Bangboo;
    return GachaType::Unknown;
}

inline RankType ParseRankType(std::string_view sv) {
    if (sv == "4") return RankType::RankS;
    if (sv == "3") return RankType::RankA;
    return RankType::Unknown;
}

// ---------------------------------------------------------
// [极简 JSON 模块 - 修复转义边界]
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

template<typename Callback>
void ForEachJsonObject(std::string_view source, std::string_view arrayKey, Callback&& cb) {
    size_t pos = FindJsonKey(source, arrayKey);
    if (pos == std::string_view::npos) return;
    pos = source.find(':', pos + arrayKey.length() + 2);
    if (pos == std::string_view::npos) return;
    pos = source.find('[', pos);
    if (pos == std::string_view::npos) return;

    // 逐字符扫描:单轮 O(N) 线性复杂度,紧凑循环对 cache/branch predictor 极友好
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

// 小 item 字符串(~300B)下,多次定向扫描优于单次全字段解析 —— 原因:
// 1. 定向扫描只处理我们关心的 4 个字段,key 比较分支预测命中率高
// 2. working set 小,L1 cache 友好
// 3. 编译器容易向量化简单的 find 循环

struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const { return std::hash<std::string_view>{}(sv); }
};

inline std::string WideToUtf8(std::wstring_view wstr) {
    if (wstr.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), result.data(), size, nullptr, nullptr);
    return result;
}

std::unordered_set<std::string, StringHash, std::equal_to<>> ParseCommaSeparatedUtf8(const std::wstring& text) {
    std::unordered_set<std::string, StringHash, std::equal_to<>> result;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L'\uFF0C') {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n"));
            if (!cur.empty()) cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
            if (!cur.empty()) result.insert(WideToUtf8(cur));
            cur.clear();
        } else cur += c;
    }
    cur.erase(0, cur.find_first_not_of(L" \t\r\n"));
    if (!cur.empty()) cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
    if (!cur.empty()) result.insert(WideToUtf8(cur));
    return result;
}

// ---------------------------------------------------------
// [SoA - 4 字段热路径,统计只访问其中 3 列]
// 预分桶版本:按 GachaType 分 4 个独立的桶,避免每次统计时线性过滤
// ---------------------------------------------------------
struct PullDataSoA {
    std::pmr::vector<long long>        ids;
    std::pmr::vector<RankType>         rank_types;
    std::pmr::vector<std::string_view> names;

    explicit PullDataSoA(std::pmr::polymorphic_allocator<std::byte> alloc)
        : ids(alloc), rank_types(alloc), names(alloc) {}

    void reserve(size_t cap) {
        ids.reserve(cap); rank_types.reserve(cap); names.reserve(cap);
    }
    void push_back(long long id, RankType rt, std::string_view name) {
        ids.push_back(id); rank_types.push_back(rt); names.push_back(name);
    }
    size_t size() const { return ids.size(); }
    bool empty() const { return ids.empty(); }
};

struct alignas(64) StatsAccumulator {
    std::array<int, 200> freq_all{};
    std::array<int, 200> freq_up{};
    long long sum_all = 0, sum_sq_all = 0, sum_up = 0, sum_sq_up = 0, sum_win = 0;
    int count_all = 0, count_up = 0, count_win = 0;
    int max_pity_all = 0, max_pity_up = 0;
    int win_5050 = 0, lose_5050 = 0;
};

struct StatsResult {
    std::array<int, 200> freq_all{};
    std::array<int, 200> freq_up{};
    int count_all = 0, count_up = 0;
    double avg_all = 0.0, avg_up = 0.0, avg_win = -1.0;
    double cv_all = 0.0, ci_all_err = 0.0, ci_up_err = 0.0;
    int win_5050 = 0, lose_5050 = 0;
    double win_rate_5050 = -1.0;
    std::array<double, 200> hazard_all{}, hazard_up{};
    double ks_d_all = 0.0;
    bool ks_is_normal = true;
};

StatsResult statsAgent, statsWEngine;
HWND hOutEdit, hCharEdit, hWepEdit;
static HBITMAP g_hChartBmp = NULL;
int g_dpi = 96;
int    DPIScale (int value)   { return MulDiv(value, g_dpi, 96); }
float  DPIScaleF(float value) { return value * (g_dpi / 96.0f); }

// -------------------------------------------------------
// CDF 表
// -------------------------------------------------------
static double g_cdf_agent[91] = {};
static double g_cdf_wengine[81] = {};

void InitCDFTables() {
    double surv = 1.0;
    for (int i = 1; i <= 90; ++i) {
        double p = (i <= 72) ? 0.006 :
                   (i <= 89) ? 0.006 + (i - 72) * 0.06 : 1.0;
        if (p > 1.0) p = 1.0;
        g_cdf_agent[i] = g_cdf_agent[i - 1] + surv * p;
        surv *= (1.0 - p);
    }
    surv = 1.0;
    for (int i = 1; i <= 80; ++i) {
        double p = (i <= 62) ? 0.008 :
                   (i <= 79) ? 0.008 + (i - 62) * 0.07 : 1.0;
        if (p > 1.0) p = 1.0;
        g_cdf_wengine[i] = g_cdf_wengine[i - 1] + surv * p;
        surv *= (1.0 - p);
    }
}

double ComputeKS(const std::array<int, 200>& freq, int max_pity, int n, const double* cdf_table, int cdf_len) {
    if (n == 0) return 0.0;
    double max_d = 0.0;
    int cum_count = 0;
    for (int x = 1; x <= max_pity; ++x) {
        double f_val = (x < cdf_len) ? cdf_table[x] : 1.0;
        double fn_before = (double)cum_count / n;
        cum_count += freq[x];
        double fn_after = (double)cum_count / n;
        double d1 = std::abs(fn_before - f_val);
        double d2 = std::abs(fn_after  - f_val);
        if (d1 > max_d) max_d = d1;
        if (d2 > max_d) max_d = d2;
    }
    return max_d;
}

// -------------------------------------------------------
// 统计核心:分桶后的 SoA 已按 id 升序,直接顺序遍历即可
// -------------------------------------------------------
StatsResult Calculate(const PullDataSoA& bucket, bool isWEngine,
                      const std::unordered_set<std::string, StringHash, std::equal_to<>>& standard_names) {
    StatsAccumulator acc;
    int current_pity = 0, pity_since_last_up = 0;
    bool had_non_up = false;

    const size_t total = bucket.size();
    for (size_t i = 0; i < total; ++i) {
        ++current_pity; ++pity_since_last_up;

        // 非 S 级大多数:标记为 likely 分支
        if (bucket.rank_types[i] != RankType::RankS) [[likely]] {
            continue;
        }

        // 到这里:触发 S 级
        if (current_pity < 200) acc.freq_all[current_pity]++;
        if (current_pity > acc.max_pity_all) acc.max_pity_all = current_pity;
        acc.count_all++;
        acc.sum_all    += current_pity;
        acc.sum_sq_all += (long long)current_pity * current_pity;

        bool isUP = !standard_names.contains(bucket.names[i]);

        if (isUP) {
            if (pity_since_last_up < 200) acc.freq_up[pity_since_last_up]++;
            if (pity_since_last_up > acc.max_pity_up) acc.max_pity_up = pity_since_last_up;
            acc.count_up++;
            acc.sum_up    += pity_since_last_up;
            acc.sum_sq_up += (long long)pity_since_last_up * pity_since_last_up;

            if (!had_non_up) { acc.count_win++; acc.sum_win += current_pity; acc.win_5050++; }
            had_non_up = false;
            pity_since_last_up = 0;
        } else {
            if (!had_non_up) acc.lose_5050++;
            had_non_up = true;
        }
        current_pity = 0;
    }

    StatsResult s;
    s.freq_all  = acc.freq_all;
    s.freq_up   = acc.freq_up;
    s.count_all = acc.count_all;
    s.count_up  = acc.count_up;
    s.win_5050  = acc.win_5050;
    s.lose_5050 = acc.lose_5050;

    if (acc.count_all > 0) {
        s.avg_all = (double)acc.sum_all / acc.count_all;
        double var = (double)acc.sum_sq_all / acc.count_all - s.avg_all * s.avg_all;
        double std_all = std::sqrt(var > 0 ? var : 0);
        s.cv_all = (s.avg_all > 0) ? std_all / s.avg_all : 0;
        s.ci_all_err = 1.96 * std_all / std::sqrt((double)acc.count_all);

        int survivors = acc.count_all;
        for (int x = 1; x <= acc.max_pity_all; ++x) {
            if (survivors > 0) {
                s.hazard_all[x] = (double)acc.freq_all[x] / survivors;
                survivors -= acc.freq_all[x];
            }
        }
        const double* cdf_tbl = isWEngine ? g_cdf_wengine : g_cdf_agent;
        int cdf_len = isWEngine ? 81 : 91;
        s.ks_d_all = ComputeKS(acc.freq_all, acc.max_pity_all, acc.count_all, cdf_tbl, cdf_len);
        s.ks_is_normal = (s.ks_d_all <= (1.36 / std::sqrt((double)acc.count_all)));
    }

    if (acc.count_up > 0) {
        s.avg_up = (double)acc.sum_up / acc.count_up;
        double var = (double)acc.sum_sq_up / acc.count_up - s.avg_up * s.avg_up;
        double std_up = std::sqrt(var > 0 ? var : 0);
        s.ci_up_err = 1.96 * std_up / std::sqrt((double)acc.count_up);

        int survivors = acc.count_up;
        for (int x = 1; x <= acc.max_pity_up; ++x) {
            if (survivors > 0) {
                s.hazard_up[x] = (double)acc.freq_up[x] / survivors;
                survivors -= acc.freq_up[x];
            }
        }
    }

    if (acc.count_win > 0) s.avg_win = (double)acc.sum_win / acc.count_win;
    if (acc.win_5050 + acc.lose_5050 > 0)
        s.win_rate_5050 = (double)acc.win_5050 / (acc.win_5050 + acc.lose_5050);

    return s;
}

// ---------------------------------------------------------
// [RAII 句柄]
// ---------------------------------------------------------
struct FileGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~FileGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    operator HANDLE() const { return h; }
};
struct MapGuard {
    HANDLE h = NULL;
    ~MapGuard() { if (h) CloseHandle(h); }
    operator HANDLE() const { return h; }
};
struct ViewGuard {
    const void* p = nullptr;
    ~ViewGuard() { if (p) UnmapViewOfFile(p); }
};

// ---------------------------------------------------------
// 文件处理
// ---------------------------------------------------------
void ProcessFile(const std::wstring& path) {
    wchar_t charBuf[1024]; GetWindowTextW(hCharEdit, charBuf, 1024);
    auto stdAgents = ParseCommaSeparatedUtf8(charBuf);

    wchar_t wepBuf[4096]; GetWindowTextW(hWepEdit, wepBuf, 4096);
    auto stdWEngines = ParseCommaSeparatedUtf8(wepBuf);

    FileGuard hFile;
    hFile.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                          NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile.h == INVALID_HANDLE_VALUE) return;

    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == 0 || fileSize == INVALID_FILE_SIZE) return;

    MapGuard hMap;
    hMap.h = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap.h) return;

    ViewGuard view;
    view.p = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!view.p) return;

    std::string_view bufferView((const char*)view.p, fileSize);
    if (bufferView.size() >= 3 &&
        (unsigned char)bufferView[0] == 0xEF &&
        (unsigned char)bufferView[1] == 0xBB &&
        (unsigned char)bufferView[2] == 0xBF) {
        bufferView.remove_prefix(3);
    }

    // PMR:栈上 2MB 内存池
    std::array<std::byte, 2 * 1024 * 1024> stackBuffer;
    std::pmr::monotonic_buffer_resource pool(stackBuffer.data(), stackBuffer.size());
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);

    // 预分桶:GachaType → PullDataSoA
    // 我们只关心 Exclusive 和 WEngine 两类,其他忽略
    PullDataSoA bucketAgent(alloc);   bucketAgent.reserve(3000);
    PullDataSoA bucketWEngine(alloc); bucketWEngine.reserve(3000);

    // 第一遍:收集 (id, gacha_type, rank_type, name),还要排序
    // 为了排序用 indices,但排序完成后按 bucket 分发 —— 一次过
    struct Temp {
        long long id;
        GachaType gt;
        RankType  rt;
        std::string_view name;
    };
    std::pmr::vector<Temp> temps(alloc);
    temps.reserve(6000);

    ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
        RankType  rt = ParseRankType(ExtractJsonValue(itemStr, "rank_type", true));
        GachaType gt = ParseGachaType(ExtractJsonValue(itemStr, "gacha_type", true));

        // 只对关心的桶解析 id 和 name,节省掉无关记录的字段提取
        if (gt != GachaType::Exclusive && gt != GachaType::WEngine) return;

        std::string_view idStr = ExtractJsonValue(itemStr, "id", true);
        if (idStr.empty()) idStr = ExtractJsonValue(itemStr, "id", false);
        long long pid = 0;
        if (!idStr.empty()) {
            std::from_chars(idStr.data(), idStr.data() + idStr.size(), pid);
        }
        std::string_view name = ExtractJsonValue(itemStr, "name", true);

        temps.push_back(Temp{pid, gt, rt, name});
    });

    if (temps.empty()) {
        SetWindowTextW(hOutEdit, L"JSON 解析失败或无数据。");
        return;
    }

    // 校验是否已有序 —— UIGF 正常写出的文件天然升序;无序时才 sort
    bool sorted = true;
    for (size_t i = 1; i < temps.size(); ++i) {
        if (temps[i - 1].id > temps[i].id) { sorted = false; break; }
    }
    if (!sorted) {
        std::ranges::sort(temps, [](const Temp& a, const Temp& b) { return a.id < b.id; });
    }

    // 一次分发到桶
    for (const auto& t : temps) {
        if (t.gt == GachaType::Exclusive) {
            bucketAgent.push_back(t.id, t.rt, t.name);
        } else if (t.gt == GachaType::WEngine) {
            bucketWEngine.push_back(t.id, t.rt, t.name);
        }
    }

    statsAgent   = Calculate(bucketAgent,   /*isWEngine=*/false, stdAgents);
    statsWEngine = Calculate(bucketWEngine, /*isWEngine=*/true,  stdWEngines);

    wchar_t winAStr[64] = L"[无数据]";
    if (statsAgent.avg_win >= 0) swprintf(winAStr, 64, L"%.2f 抽", statsAgent.avg_win);
    wchar_t winWStr[64] = L"[无数据]";
    if (statsWEngine.avg_win >= 0) swprintf(winWStr, 64, L"%.2f 抽", statsWEngine.avg_win);

    wchar_t outMsg[2048];
    swprintf(outMsg, 2048,
        L"【独家频段 (限定代理人)】 总计 S级: %d | 出当期 UP: %d\r\n"
        L" ▶ 综合 S级 (含歪) 出货平均期望:     %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S D: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期 UP 的综合平均期望:       %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (%d胜%d负)\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls\r\n\r\n"
        L"【音擎频段 (限定音擎)】 总计 S级: %d | 出当期 UP: %d\r\n"
        L" ▶ 综合 S级 (含歪) 出货平均期望:     %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S D: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期 UP 的综合平均期望:       %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (%d胜%d负)\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls",
        statsAgent.count_all, statsAgent.count_up,
        statsAgent.avg_all, (std::max)(1.0, statsAgent.avg_all - statsAgent.ci_all_err),
        statsAgent.avg_all + statsAgent.ci_all_err, statsAgent.cv_all * 100.0, statsAgent.ks_d_all,
        (statsAgent.count_all == 0 ? L"-" : (statsAgent.ks_is_normal ? L"符合理论模型" : L"偏离过大")),
        statsAgent.avg_up, (std::max)(1.0, statsAgent.avg_up - statsAgent.ci_up_err),
        statsAgent.avg_up + statsAgent.ci_up_err,
        statsAgent.win_rate_5050 >= 0 ? statsAgent.win_rate_5050 * 100.0 : 0.0,
        statsAgent.win_5050, statsAgent.lose_5050, winAStr,
        statsWEngine.count_all, statsWEngine.count_up,
        statsWEngine.avg_all, (std::max)(1.0, statsWEngine.avg_all - statsWEngine.ci_all_err),
        statsWEngine.avg_all + statsWEngine.ci_all_err, statsWEngine.cv_all * 100.0, statsWEngine.ks_d_all,
        (statsWEngine.count_all == 0 ? L"-" : (statsWEngine.ks_is_normal ? L"符合理论模型" : L"偏离过大")),
        statsWEngine.avg_up, (std::max)(1.0, statsWEngine.avg_up - statsWEngine.ci_up_err),
        statsWEngine.avg_up + statsWEngine.ci_up_err,
        statsWEngine.win_rate_5050 >= 0 ? statsWEngine.win_rate_5050 * 100.0 : 0.0,
        statsWEngine.win_5050, statsWEngine.lose_5050, winWStr
    );
    SetWindowTextW(hOutEdit, outMsg);
}

// -------------------------------------------------------
// 图形渲染 —— drawCurve 使用栈数组,无堆分配
// -------------------------------------------------------
void DrawKDE(Gdiplus::Graphics& g, Gdiplus::Rect rect,
             const std::array<int, 200>& freq_all, const std::array<int, 200>& freq_up,
             const std::wstring& title, int limit_base) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255));
    g.FillRectangle(&bgBrush, rect);
    Gdiplus::FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&fontFamily, DPIScaleF(15.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 40, 40, 40));
    g.DrawString(title.c_str(), -1, &titleFont,
                 Gdiplus::PointF((float)rect.X + DPIScaleF(15.0f), (float)rect.Y + DPIScaleF(12.0f)),
                 &textBrush);

    int max_x = limit_base;
    bool hasData = false;
    for (int i = 1; i < 200; i++) {
        if (freq_all[i] > 0 || freq_up[i] > 0) {
            hasData = true;
            if (i > max_x) max_x = i;
        }
    }
    if (!hasData) {
        Gdiplus::Font emptyFont(&fontFamily, DPIScaleF(14.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush emptyBrush(Gdiplus::Color(255, 150, 150, 150));
        g.DrawString(L"暂无出金数据", -1, &emptyFont,
                     Gdiplus::PointF((float)rect.X + (float)rect.Width / 2.0f - DPIScaleF(50.0f),
                                     (float)rect.Y + (float)rect.Height / 2.0f),
                     &emptyBrush);
        return;
    }
    max_x = ((max_x / 10) + 1) * 10;

    auto calcKDE = [&](const std::array<int, 200>& freqs, std::array<double, 200>& out_curve) {
        out_curve.fill(0.0);
        int total = 0;
        for (int i = 1; i <= max_x; i++) total += freqs[i];
        if (total == 0) return;
        double bandwidth = 4.0;
        int spread = (int)(4.0 * bandwidth) + 1;
        for (int v = 1; v <= max_x; ++v) {
            if (freqs[v] == 0) continue;
            int lo = (std::max)(1, v - spread), hi = (std::min)(max_x, v + spread);
            for (int x = lo; x <= hi; ++x) {
                double u = (x - v) / bandwidth;
                out_curve[x] += freqs[v] * std::exp(-0.5 * u * u);
            }
        }
        double inv_total = 1.0 / total;
        for (int x = 1; x <= max_x; ++x) out_curve[x] *= inv_total;
    };

    std::array<double, 200> kde_all{}, kde_up{};
    calcKDE(freq_all, kde_all);
    calcKDE(freq_up,  kde_up);

    double max_y = 0.0001;
    for (int i = 1; i <= max_x; i++) {
        if (kde_all[i] > max_y) max_y = kde_all[i];
        if (kde_up[i]  > max_y) max_y = kde_up[i];
    }
    max_y *= 1.25;

    float plotX = (float)rect.X + DPIScaleF(55.0f), plotY = (float)rect.Y + DPIScaleF(45.0f);
    float plotW = (float)rect.Width - DPIScaleF(85.0f), plotH = (float)rect.Height - DPIScaleF(75.0f);
    Gdiplus::Pen axisPen(Gdiplus::Color(255, 150, 150, 150), DPIScaleF(1.5f));
    Gdiplus::Pen gridPen(Gdiplus::Color(255, 235, 235, 235), DPIScaleF(1.0f));
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY, plotX, plotY + plotH);

    auto getPt = [&](int x, double y) {
        return Gdiplus::PointF(plotX + (float)x / (float)max_x * plotW,
                               plotY + plotH - (float)(y / max_y) * plotH);
    };
    Gdiplus::Font tickFont(&fontFamily, DPIScaleF(11.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255, 120, 120, 120));

    for (int i = 0; i <= 4; ++i) {
        float py = plotY + plotH - (float)i / 4.0f * plotH;
        if (i > 0) g.DrawLine(&gridPen, plotX, py, plotX + plotW, py);
        g.DrawLine(&axisPen, plotX - DPIScaleF(5.0f), py, plotX, py);
        wchar_t y_label[32]; swprintf(y_label, 32, L"%.1f%%", (max_y / 4.0) * i * 100.0);
        float labelW = (float)wcslen(y_label) * DPIScaleF(5.5f) + DPIScaleF(8.0f);
        g.DrawString(y_label, -1, &tickFont, Gdiplus::PointF(plotX - labelW, py - DPIScaleF(6.0f)), &tickBrush);
    }
    int step = (max_x > 140) ? 20 : 10;
    for (int x = 0; x <= max_x; x += step) {
        float px = plotX + (float)x / (float)max_x * plotW;
        g.DrawLine(&axisPen, px, plotY + plotH, px, plotY + plotH + DPIScaleF(5.0f));
        wchar_t x_label[16]; swprintf(x_label, 16, L"%d", x);
        float xoff = (x < 10 ? 4.0f : x < 100 ? 8.0f : 12.0f) * DPIScaleF(1.0f);
        g.DrawString(x_label, -1, &tickFont, Gdiplus::PointF(px - xoff, plotY + plotH + DPIScaleF(8.0f)), &tickBrush);
    }

    // 栈数组,避免堆分配 —— max_x 最大 <200,201 足够
    auto drawCurve = [&](const std::array<double, 200>& kde, Gdiplus::Color color) {
        std::array<Gdiplus::PointF, 201> pts;
        pts[0] = getPt(0, 0);
        int ptsCount = 1;
        for (int x = 1; x <= max_x && ptsCount < (int)pts.size(); ++x) {
            pts[ptsCount++] = getPt(x, kde[x]);
        }
        Gdiplus::Pen pen(color, DPIScaleF(2.5f));
        g.DrawCurve(&pen, pts.data(), ptsCount, 0.3f);
    };

    drawCurve(kde_all, Gdiplus::Color(255, 65, 140, 240));
    drawCurve(kde_up,  Gdiplus::Color(255, 240, 80, 80));

    Gdiplus::SolidBrush blueBrush(Gdiplus::Color(255, 65, 140, 240)),
                        redBrush(Gdiplus::Color(255, 240, 80, 80));
    float legendX = (float)rect.X + (float)rect.Width - DPIScaleF(190.0f);
    g.DrawString(L"━━ 综合S级分布 (含歪)", -1, &tickFont,
                 Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(15.0f)), &blueBrush);
    g.DrawString(L"━━ 当期限定 UP 分布", -1, &tickFont,
                 Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(35.0f)), &redBrush);
}

void DrawHazard(Gdiplus::Graphics& g, Gdiplus::Rect rect,
                const std::array<double, 200>& hazard_all,
                const std::array<double, 200>& hazard_up,
                const std::wstring& title, int limit_base) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255));
    g.FillRectangle(&bgBrush, rect);
    Gdiplus::FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&fontFamily, DPIScaleF(15.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 40, 40, 40));
    g.DrawString(title.c_str(), -1, &titleFont,
                 Gdiplus::PointF((float)rect.X + DPIScaleF(15.0f), (float)rect.Y + DPIScaleF(12.0f)),
                 &textBrush);

    int max_x = limit_base;
    double max_y = 0.1;
    bool hasData = false;
    for (int i = 1; i < 200; i++) {
        if (hazard_all[i] > 0 || hazard_up[i] > 0) {
            hasData = true;
            if (i > max_x) max_x = i;
            if (hazard_all[i] > max_y) max_y = hazard_all[i];
            if (hazard_up[i]  > max_y) max_y = hazard_up[i];
        }
    }
    if (!hasData) return;
    max_x = ((max_x / 10) + 1) * 10;
    if (max_y > 0.8) max_y = 1.05;
    else max_y = (std::ceil(max_y * 10)) / 10.0 + 0.1;

    float plotX = (float)rect.X + DPIScaleF(55.0f), plotY = (float)rect.Y + DPIScaleF(45.0f);
    float plotW = (float)rect.Width - DPIScaleF(85.0f), plotH = (float)rect.Height - DPIScaleF(75.0f);
    Gdiplus::Pen axisPen(Gdiplus::Color(255, 150, 150, 150), DPIScaleF(1.5f));
    Gdiplus::Pen gridPen(Gdiplus::Color(255, 235, 235, 235), DPIScaleF(1.0f));
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY, plotX, plotY + plotH);

    auto getPt = [&](int x, double y) {
        return Gdiplus::PointF(plotX + (float)x / (float)max_x * plotW,
                               plotY + plotH - (float)(y / max_y) * plotH);
    };
    Gdiplus::Font tickFont(&fontFamily, DPIScaleF(11.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255, 120, 120, 120));

    for (int i = 0; i <= 4; ++i) {
        float py = plotY + plotH - (float)i / 4.0f * plotH;
        if (i > 0) g.DrawLine(&gridPen, plotX, py, plotX + plotW, py);
        g.DrawLine(&axisPen, plotX - DPIScaleF(5.0f), py, plotX, py);
        wchar_t y_label[32]; swprintf(y_label, 32, L"%.0f%%", (max_y / 4.0) * i * 100.0);
        float labelW = (float)wcslen(y_label) * DPIScaleF(5.5f) + DPIScaleF(8.0f);
        g.DrawString(y_label, -1, &tickFont, Gdiplus::PointF(plotX - labelW, py - DPIScaleF(6.0f)), &tickBrush);
    }
    int step = (max_x > 140) ? 20 : 10;
    for (int x = 0; x <= max_x; x += step) {
        float px = plotX + (float)x / (float)max_x * plotW;
        g.DrawLine(&axisPen, px, plotY + plotH, px, plotY + plotH + DPIScaleF(5.0f));
        wchar_t x_label[16]; swprintf(x_label, 16, L"%d", x);
        float xoff = (x < 10 ? 4.0f : x < 100 ? 8.0f : 12.0f) * DPIScaleF(1.0f);
        g.DrawString(x_label, -1, &tickFont, Gdiplus::PointF(px - xoff, plotY + plotH + DPIScaleF(8.0f)), &tickBrush);
    }

    float barW = (std::max)(1.5f, plotW / max_x * 0.4f);
    Gdiplus::SolidBrush brushAll(Gdiplus::Color(180, 65, 140, 240));
    for (int x = 1; x <= max_x; x++) {
        if (hazard_all[x] > 0) {
            Gdiplus::PointF p = getPt(x, hazard_all[x]);
            g.FillRectangle(&brushAll, p.X - barW, p.Y, barW, plotY + plotH - p.Y);
        }
    }
    Gdiplus::SolidBrush brushUp(Gdiplus::Color(180, 240, 80, 80));
    for (int x = 1; x <= max_x; x++) {
        if (hazard_up[x] > 0) {
            Gdiplus::PointF p = getPt(x, hazard_up[x]);
            g.FillRectangle(&brushUp, p.X, p.Y, barW, plotY + plotH - p.Y);
        }
    }

    Gdiplus::SolidBrush blueBrush(Gdiplus::Color(255, 65, 140, 240)),
                        redBrush(Gdiplus::Color(255, 240, 80, 80));
    float legendX = (float)rect.X + (float)rect.Width - DPIScaleF(160.0f);
    g.DrawString(L"■ 综合 S级条件概率", -1, &tickFont,
                 Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(15.0f)), &blueBrush);
    g.DrawString(L"■ 限定 UP 条件概率", -1, &tickFont,
                 Gdiplus::PointF(legendX, (float)rect.Y + DPIScaleF(35.0f)), &redBrush);
}

void RebuildChartCache(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    HDC hdcWnd = GetDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdcWnd);
    if (g_hChartBmp) DeleteObject(g_hChartBmp);
    g_hChartBmp = CreateCompatibleBitmap(hdcWnd, w, h);
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hChartBmp);
    FillRect(hdcMem, &rc, (HBRUSH)(COLOR_WINDOW + 1));
    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        DrawKDE   (g, Gdiplus::Rect(DPIScale(20),  DPIScale(330), DPIScale(600), DPIScale(250)),
                   statsAgent.freq_all,   statsAgent.freq_up,
                   L"独家频段 S级核密度", 85);
        DrawHazard(g, Gdiplus::Rect(DPIScale(640), DPIScale(330), DPIScale(600), DPIScale(250)),
                   statsAgent.hazard_all, statsAgent.hazard_up,
                   L"独家频段 经验风险函数", 85);
        DrawKDE   (g, Gdiplus::Rect(DPIScale(20),  DPIScale(585), DPIScale(600), DPIScale(250)),
                   statsWEngine.freq_all, statsWEngine.freq_up,
                   L"音擎频段 S级核密度", 80);
        DrawHazard(g, Gdiplus::Rect(DPIScale(640), DPIScale(585), DPIScale(600), DPIScale(250)),
                   statsWEngine.hazard_all, statsWEngine.hazard_up,
                   L"音擎频段 经验风险函数", 80);
    }
    SelectObject(hdcMem, hOld);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdcWnd);
}

static HFONT hFont = NULL;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        DragAcceptFiles(hwnd, TRUE);
        hFont = CreateFontW(-DPIScale(13), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");

        HWND hL1 = CreateWindowW(L"STATIC",
                                 L"支持通过常驻S级名单自动判定UP/歪。请保持名单最新。",
                                 WS_CHILD | WS_VISIBLE,
                                 DPIScale(20), DPIScale(15), DPIScale(1000), DPIScale(20),
                                 hwnd, NULL, NULL, NULL);
        HWND hLC = CreateWindowW(L"STATIC", L"常驻S级代理人:",
                                 WS_CHILD | WS_VISIBLE,
                                 DPIScale(20), DPIScale(45), DPIScale(110), DPIScale(20),
                                 hwnd, NULL, NULL, NULL);
        hCharEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
                                    L"格莉丝,丽娜,珂蕾妲,猫又,「11号」,莱卡恩",
                                    WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    DPIScale(135), DPIScale(40), DPIScale(1100), DPIScale(26),
                                    hwnd, NULL, NULL, NULL);
        HWND hLW = CreateWindowW(L"STATIC", L"常驻S级音擎:",
                                 WS_CHILD | WS_VISIBLE,
                                 DPIScale(20), DPIScale(75), DPIScale(110), DPIScale(20),
                                 hwnd, NULL, NULL, NULL);
        hWepEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
                                   L"嵌合编译器,钢铁肉垫,啜泣摇篮,拘束者,硫磺石,燃狱齿轮",
                                   WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                   DPIScale(135), DPIScale(70), DPIScale(1100), DPIScale(26),
                                   hwnd, NULL, NULL, NULL);

        hOutEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
                                   L"等待拖入 UIGF JSON 文件...",
                                   WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                                   DPIScale(20), DPIScale(105), DPIScale(1220), DPIScale(215),
                                   hwnd, NULL, NULL, NULL);
        DWORD tabStops[] = {200};
        SendMessage(hOutEdit, EM_SETTABSTOPS, 1, (LPARAM)tabStops);
        SendMessage(hOutEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)GetSysColor(COLOR_3DFACE));

        for (HWND h : {hL1, hLC, hCharEdit, hLW, hWepEdit, hOutEdit})
            SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
        RebuildChartCache(hwnd);
        break;
    }
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        wchar_t fp[MAX_PATH];
        DragQueryFileW(hDrop, 0, fp, MAX_PATH);
        DragFinish(hDrop);
        ProcessFile(fp);
        RebuildChartCache(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (g_hChartBmp) {
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hChartBmp);
            BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top,
                   hdcMem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
        }
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: {
        if (g_hChartBmp) { DeleteObject(g_hChartBmp); g_hChartBmp = NULL; }
        if (hFont) DeleteObject(hFont);
        PostQuitMessage(0);
        break;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    LoadLibrary(L"Msftedit.dll");
    SetProcessDPIAware();
    HDC hdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(NULL, hdc);
    ULONG_PTR gdipToken;
    Gdiplus::GdiplusStartupInput si;
    Gdiplus::GdiplusStartup(&gdipToken, &si, NULL);
    InitCDFTables();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ZZZStatsClass";
    RegisterClassW(&wc);

    DWORD dwStyle = (WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX) | WS_CLIPCHILDREN;
    RECT r = {0, 0, DPIScale(1280), DPIScale(870)};
    AdjustWindowRectEx(&r, dwStyle, FALSE, 0);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"绝区零调频记录分析与可视化",
                              dwStyle, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top,
                              NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    Gdiplus::GdiplusShutdown(gdipToken);
    return 0;
}
