// ============================================================
// ZZZ Gacha Visualizer - Win32 + GDI+ + PMR / 预分桶 / AoS
// ============================================================
// 绝区零(Zenless Zone Zero)抽卡记录可视化分析工具
//
// 与终末地版本的关键差异:
//   1. UIGF 顶层 key: "nap" (官方支持) 而非自定义 "endfield"
//   2. UIGF 字段名: "name" 而非 "item_name"; 多了 "count"/"gacha_id"
//   3. gacha_type 是数字字符串 ("1"/"2"/"3"/"5"), 不是枚举字符串
//   4. 物品稀有度: rank_type 字符串, "2"=B级, "3"=A级, "4"=S级
//      (注: ZZZ rank_type 是 2/3/4, 不是 3/4/5/6)
//   5. 概率模型完全不同 (米哈游模型, 见 InitCDFTables)
//   6. 没有 30 抽赠送十连, 没有 60 抽返利, 不需要 is_free 字段
//   7. 池子分类: 代理人 UP 池("2") + 音擎 UP 池("3") 是分析对象
//      常驻池("1") 和邦布池("5") 可以读但不分析
//   8. 50/50 vs 75/25: 代理人池歪率 50%, 音擎池歪率 25%
//
// 编译: cl /std:c++20 /EHsc /O2 gui.cpp /link /STACK:4194304
// ============================================================
#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
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
// MSVC 不会自动链接 Win32 GUI 基础库 (在 /MT 或某些工具链版本下), 必须显式声明.
// 如果忘加, 会出现一大堆 LNK2019 (BitBlt/GetMessageW/DragQueryFileW 等无法解析).
#pragma comment(lib, "gdi32.lib")     // BitBlt / GDI 绘图
#pragma comment(lib, "user32.lib")    // 窗口 / 消息 / 控件 / GetMessage 等
#pragma comment(lib, "shell32.lib")   // DragQueryFile / DragAcceptFiles
#pragma comment(lib, "ole32.lib")     // GdiplusStartup 间接依赖 (COM)
#pragma comment(lib, "Msimg32.lib")   // (保险) AlphaBlend 等

// ---------------------------------------------------------
// [枚举降维]
// ---------------------------------------------------------
// 绝区零物品类型: 代理人/音擎/邦布 (角色/武器/宠物)
enum class ItemType  : uint8_t { Unknown = 0, Agent, WEngine, Bangboo };

// 稀有度: ZZZ 用 2/3/4 表示 B/A/S (不是终末地的 3/4/5/6)
enum class RankType  : uint8_t { Unknown = 0, RankB = 2, RankA = 3, RankS = 4 };

// gacha_type 数字字符串映射: "1"/"2"/"3"/"5"
enum class GachaType : uint8_t {
    Unknown = 0,
    Standard = 1,    // "1" 常驻频段(热门卡司)
    AgentUP = 2,     // "2" 独家频段(代理人 UP)
    WEngineUP = 3,   // "3" 音擎频段(武器 UP)
    Bangboo = 5      // "5" 邦布频段
};

// 无堆分配的大小写不敏感包含比较
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
    // zh-cn: API 直接返回中文 "代理人"/"音擎"/"邦布"
    if (sv == "代理人") return ItemType::Agent;     // 代理人
    if (sv == "音擎")             return ItemType::WEngine;   // 音擎
    if (sv == "邦布")             return ItemType::Bangboo;   // 邦布
    if (ContainsCI(sv, "agent"))   return ItemType::Agent;
    if (ContainsCI(sv, "engine"))  return ItemType::WEngine;
    if (ContainsCI(sv, "bangboo")) return ItemType::Bangboo;
    return ItemType::Unknown;
}

inline RankType ParseRankType(std::string_view sv) {
    // ZZZ: 4=S, 3=A, 2=B (注意比终末地少一档)
    if (sv == "4") return RankType::RankS;
    if (sv == "3") return RankType::RankA;
    if (sv == "2") return RankType::RankB;
    return RankType::Unknown;
}

inline GachaType ParseGachaType(std::string_view sv) {
    // 数字字符串直接对应; 严格相等避免误匹配 "12" 这种(虽然 API 不返回)
    if (sv == "1") return GachaType::Standard;
    if (sv == "2") return GachaType::AgentUP;
    if (sv == "3") return GachaType::WEngineUP;
    if (sv == "5") return GachaType::Bangboo;
    return GachaType::Unknown;
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

// 注意: UP 列表用 ASCII ',' 作分隔符; 不识别全角逗号(避免和池名内的全角逗号冲突)
// 终末地版本里有同样的注释; 绝区零暂未见全角逗号池名, 但保持习惯一致.
std::unordered_set<std::string, StringHash, std::equal_to<>> ParseCommaSeparatedUtf8(const std::wstring& text) {
    std::unordered_set<std::string, StringHash, std::equal_to<>> result;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',') {
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

inline std::string TrimUtf8(std::string_view sv) {
    size_t b = sv.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    size_t e = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(b, e - b + 1));
}

std::unordered_set<std::string, StringHash, std::equal_to<>> ParseCommaSeparatedUtf8FromUtf8(std::string_view text) {
    std::unordered_set<std::string, StringHash, std::equal_to<>> result;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ',') {
            std::string trimmed = TrimUtf8(cur);
            if (!trimmed.empty()) result.insert(std::move(trimmed));
            cur.clear();
        } else {
            cur += text[i];
        }
    }
    std::string trimmed = TrimUtf8(cur);
    if (!trimmed.empty()) result.insert(std::move(trimmed));
    return result;
}

// ---------------------------------------------------------
// [SoA 分桶]
// 绝区零简化: 没有 is_free, 不需要 poolNames 映射 (因为绝区零不会"歪到任意非UP"
// —— 歪了一定是常驻S, 而常驻S列表是固定的, 直接用 standard_names 集合判定就够了.
// 第三个参数 pool_map 在终末地用来"按池子名找对应UP"; 绝区零的 UP 角色由用户在
// GUI 输入框里指定, 用 up_names 集合统一判定即可.)
// ---------------------------------------------------------
struct PullBucket {
    std::pmr::vector<RankType>         rank_types;
    std::pmr::vector<std::string_view> names;

    explicit PullBucket(std::pmr::polymorphic_allocator<std::byte> alloc)
        : rank_types(alloc), names(alloc) {}

    void reserve(size_t cap) {
        rank_types.reserve(cap); names.reserve(cap);
    }
    void push_back(RankType rt, std::string_view name) {
        rank_types.push_back(rt); names.push_back(name);
    }
    size_t size() const { return rank_types.size(); }
};

struct alignas(128) StatsAccumulator {
    std::array<int, 200> freq_all{};   // ZZZ UP 上限可达 180 (大保底), 给 200 留富余
    std::array<int, 200> freq_up{};
    long long sum_all = 0, sum_sq_all = 0, sum_up = 0, sum_sq_up = 0, sum_win = 0;
    int count_all = 0, count_up = 0, count_win = 0;
    int max_pity_all = 0, max_pity_up = 0;
    int win_5050 = 0, lose_5050 = 0;
    int censored_pity_all = 0;
    int censored_pity_up  = 0;
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
    double ks_d_all = 0.0, ks_d_up = 0.0;
    bool ks_is_normal = true, ks_is_normal_up = true;
    int censored_pity_all = 0;
    int censored_pity_up  = 0;
};

// 全局统计结果: 代理人池 / 音擎池
StatsResult statsAgent, statsWEngine;
HWND hOutEdit, hAgentEdit, hWEngineEdit;
static HBITMAP g_hChartBmp = NULL;
int g_dpi = 96;
int   DPIScale (int value)   { return MulDiv(value, g_dpi, 96); }
float DPIScaleF(float value) { return value * (g_dpi / 96.0f); }

// -------------------------------------------------------
// CDF 表 & KS 检验
// -------------------------------------------------------
// 绝区零代理人池(独家频段):
//   基础概率 0.6%, 90 抽硬保底, 综合概率 1.6%
//   软保底拐点: 第74抽起, 每抽 +6%
//     k=1..73:  hazard = 0.006
//     k=74..89: hazard = 0.006 + (k-73) * 0.06 = 0.066, 0.126, ..., 0.966
//     k=90:     hazard = 1.0 (硬保底)
//   验证: 综合期望 ≈ 62.30 抽, 综合概率 = 1/62.30 ≈ 1.605% ✓
//
// 绝区零音擎池(音擎频段):
//   基础概率 1.0%, 80 抽硬保底, 综合概率 2.0%
//   软保底拐点: 第65抽起, 每抽 +7%
//     k=1..64:  hazard = 0.01
//     k=65..79: hazard = 0.01 + (k-64) * 0.07 = 0.08, 0.15, ..., 0.99
//     k=80:     hazard = 1.0 (硬保底)
//   验证: 综合期望 ≈ 49.71 抽, 综合概率 = 1/49.71 ≈ 2.012% ✓
//
// UP CDF: 双状态前向迭代 (与终末地相同结构, 但参数不同):
//   代理人: 50% 不歪, 90 硬保底, UP 大保底上限 180
//   音擎:   75% 不歪, 80 硬保底, UP 大保底上限 160
//
// 绝区零无"30 抽赠送十连", 无"60 抽返利", 模型比终末地干净.
static double g_cdf_agent[92]      = {};   // x=0..90,   代理人池综合
static double g_cdf_wengine[82]    = {};   // x=0..80,   音擎池综合
static double g_cdf_agent_up[182]  = {};   // x=0..180,  代理人池 UP
static double g_cdf_wengine_up[162]= {};   // x=0..160,  音擎池 UP
static bool   g_cdf_init           = false;

void InitCDFTables() {
    if (g_cdf_init) return;

    auto h_agent = [](int k) -> double {
        if (k <= 73)      return 0.006;
        else if (k <= 89) return (std::min)(1.0, 0.006 + (k - 73) * 0.06);
        else              return 1.0;
    };
    auto h_wengine = [](int k) -> double {
        if (k <= 64)      return 0.01;
        else if (k <= 79) return (std::min)(1.0, 0.01 + (k - 64) * 0.07);
        else              return 1.0;
    };

    // ---- 代理人池综合 CDF ----
    {
        double surv = 1.0;
        for (int i = 1; i <= 90; ++i) {
            double p = h_agent(i);
            g_cdf_agent[i] = g_cdf_agent[i - 1] + surv * p;
            surv *= (1.0 - p);
        }
        g_cdf_agent[91] = 1.0;
    }

    // ---- 音擎池综合 CDF ----
    {
        double surv = 1.0;
        for (int i = 1; i <= 80; ++i) {
            double p = h_wengine(i);
            g_cdf_wengine[i] = g_cdf_wengine[i - 1] + surv * p;
            surv *= (1.0 - p);
        }
        g_cdf_wengine[81] = 1.0;
    }

    // ---- 代理人池 UP CDF (双状态前向迭代, 50/50, 上限 180) ----
    // 状态空间: D_no_lost[s] (没歪过, 水位 s) + D_lost[s] (已歪, 水位 s)
    // 转移: 没歪状态出货 50% 毕业, 50% 转入 D_lost[0]
    //       已歪状态出货 100% 毕业 (大保底)
    {
        constexpr int hard_cap = 90;
        constexpr int full_cap = 180;
        std::array<double, hard_cap + 1> Dn{}; Dn[0] = 1.0;
        std::array<double, hard_cap + 1> Dl{};
        double cum = 0.0;
        for (int n = 1; n <= full_cap; ++n) {
            std::array<double, hard_cap + 1> newDn{}, newDl{};
            for (int s = 0; s < hard_cap; ++s) {
                double h = h_agent(s + 1);
                if (Dn[s] > 0) {
                    double hit = Dn[s] * h;
                    cum += hit * 0.5;                        // 50% 毕业
                    newDl[0] += hit * 0.5;                   // 50% 歪 -> 已歪状态, 水位 0
                    newDn[s + 1] += Dn[s] * (1.0 - h);       // 不命中, 水位 +1
                }
                if (Dl[s] > 0) {
                    double hit = Dl[s] * h;
                    cum += hit;                              // 100% 毕业
                    newDl[s + 1] += Dl[s] * (1.0 - h);       // 不命中
                }
            }
            g_cdf_agent_up[n] = (std::min)(1.0, cum);
            Dn = newDn; Dl = newDl;
        }
        g_cdf_agent_up[full_cap + 1] = 1.0;
    }

    // ---- 音擎池 UP CDF (75/25, 上限 160) ----
    {
        constexpr int hard_cap = 80;
        constexpr int full_cap = 160;
        std::array<double, hard_cap + 1> Dn{}; Dn[0] = 1.0;
        std::array<double, hard_cap + 1> Dl{};
        double cum = 0.0;
        for (int n = 1; n <= full_cap; ++n) {
            std::array<double, hard_cap + 1> newDn{}, newDl{};
            for (int s = 0; s < hard_cap; ++s) {
                double h = h_wengine(s + 1);
                if (Dn[s] > 0) {
                    double hit = Dn[s] * h;
                    cum += hit * 0.75;                       // 75% 毕业
                    newDl[0] += hit * 0.25;                  // 25% 歪
                    newDn[s + 1] += Dn[s] * (1.0 - h);
                }
                if (Dl[s] > 0) {
                    double hit = Dl[s] * h;
                    cum += hit;
                    newDl[s + 1] += Dl[s] * (1.0 - h);
                }
            }
            g_cdf_wengine_up[n] = (std::min)(1.0, cum);
            Dn = newDn; Dl = newDl;
        }
        g_cdf_wengine_up[full_cap + 1] = 1.0;
    }

    g_cdf_init = true;
}

// 离散阶梯 CDF 的 K-S 统计量 (与终末地相同实现)
double ComputeKS(const std::array<int, 200>& freq, int max_pity, int n,
                 const double* cdf_table, int cdf_len) {
    if (n == 0) return 0.0;
    if (max_pity > 199) max_pity = 199;
    double max_d = 0.0;
    int cum_count = 0;
    for (int x = 1; x <= max_pity; ++x) {
        double fn_before    = (double)cum_count / n;
        double cdf_before_x = (x - 1 < cdf_len) ? cdf_table[x - 1] : 1.0;

        cum_count += freq[x];

        double fn_after    = (double)cum_count / n;
        double cdf_after_x = (x < cdf_len) ? cdf_table[x] : 1.0;

        double d1 = std::abs(fn_before - cdf_before_x);
        double d2 = std::abs(fn_after  - cdf_after_x);
        if (d1 > max_d) max_d = d1;
        if (d2 > max_d) max_d = d2;
    }
    return max_d;
}

// t 分布 95% 双侧临界值 (与终末地相同实现)
inline double TCritical95(int df) {
    if (df <= 0) return 1.959963984540054;
    static constexpr double kTable[] = {
        0.0, 12.706205, 4.302653, 3.182446, 2.776445,
    };
    if (df <= 4) return kTable[df];

    constexpr double z = 1.959963984540054;
    constexpr double z2 = z * z;
    constexpr double z3 = z2 * z;
    constexpr double z5 = z3 * z2;
    constexpr double z7 = z5 * z2;
    constexpr double z9 = z7 * z2;
    constexpr double g1 = (z3 + z) / 4.0;
    constexpr double g2 = (5.0*z5 + 16.0*z3 + 3.0*z) / 96.0;
    constexpr double g3 = (3.0*z7 + 19.0*z5 + 17.0*z3 - 15.0*z) / 384.0;
    constexpr double g4 = (79.0*z9 + 776.0*z7 + 1482.0*z5 - 1920.0*z3 - 945.0*z) / 92160.0;
    double d = (double)df;
    double inv_d = 1.0 / d;
    return z + g1 * inv_d
             + g2 * inv_d * inv_d
             + g3 * inv_d * inv_d * inv_d
             + g4 * inv_d * inv_d * inv_d * inv_d;
}

inline double SampleVariance(long long sum, long long sum_sq, int n) {
    if (n <= 1) return 0.0;
    double numerator = (double)sum_sq - (double)sum * sum / (double)n;
    if (numerator < 0.0) numerator = 0.0;
    return numerator / (double)(n - 1);
}

// ---------------------------------------------------------
// 统计核心
//
// 绝区零与终末地的 UP 判定语义差异:
//   终末地: 武器池每个 6 星独立 25% 判定 UP, 无大保底
//   绝区零: 代理人池 50% / 音擎池 75%, 都有大保底("歪了下次必中")
//   所以代理人池和音擎池在结构上一致 (都是双状态), 只是参数不同.
//
// 这里 isWEngine 仅用于:
//   1. 选择对应的理论 CDF 表
//   2. 输出文本里的措辞
//   不再像终末地那样切换"独立 25%"和"50/50 大保底"两种语义
//
// UP 判定: 简单规则 — 出的 S 不在 standard_names 常驻名单里, 就是 UP.
// 这个逻辑成立的前提:
//   - 绝区零代理人/音擎 UP 池里的 S 级只有"当期 UP + 常驻 S"两种来源
//   - 常驻名单是相对稳定的 (绝区零开服至今6个常驻代理人 + 6个常驻音擎)
//   - 用户只需维护这一个常驻列表, 不用每开新池就更新"当期 UP"
// 这与终末地原版逻辑一致 (终末地也是通过 pool_map 间接做"非UP=常驻"判断,
// 但绝区零没有歪到"任意非UP角色"的情况, 所以可以直接二元化为"是否常驻").
// ---------------------------------------------------------
StatsResult Calculate(const PullBucket& bucket, bool isWEngine,
                     const std::unordered_set<std::string, StringHash, std::equal_to<>>& standard_names) {
    StatsAccumulator acc;
    int current_pity = 0, pity_since_last_up = 0;
    bool had_non_up = false;  // 上一次 S 是否歪了 (用于追踪小/大保底状态)

    const size_t total = bucket.size();
    for (size_t i = 0; i < total; ++i) {
        ++current_pity;
        ++pity_since_last_up;

        if (bucket.rank_types[i] != RankType::RankS) [[likely]] {
            continue;
        }

        // 出 S 级
        const int slot_all = current_pity;
        if (slot_all < 200) acc.freq_all[slot_all]++;
        if (slot_all > acc.max_pity_all) acc.max_pity_all = slot_all;
        acc.count_all++;
        acc.sum_all    += slot_all;
        acc.sum_sq_all += (long long)slot_all * slot_all;

        // 判定是否 UP: 不在常驻名单里就是 UP
        bool isUP = !standard_names.contains(bucket.names[i]);

        if (isUP) {
            const int slot_up = pity_since_last_up;
            if (slot_up < 200) acc.freq_up[slot_up]++;
            if (slot_up > acc.max_pity_up) acc.max_pity_up = slot_up;
            acc.count_up++;
            acc.sum_up    += slot_up;
            acc.sum_sq_up += (long long)slot_up * slot_up;

            // win_5050: 不歪率统计
            // had_non_up=false (50/50 状态) 时出 UP 才计入 "win"
            // had_non_up=true (大保底) 时出 UP 是必然事件, 不算"赢了 50/50"
            // count_win/sum_win: 仅追踪"50/50 阶段成功毕业"的样本期望
            if (!had_non_up) {
                acc.win_5050++;
                acc.count_win++;
                acc.sum_win += slot_all;
            }
            had_non_up = false;
            pity_since_last_up = 0;
        } else {
            // 出非 UP S
            // 仅当上一个 S 是 UP (即此次是新一轮的 50/50 失败) 才计入 lose
            // had_non_up=true 时不重复计数 (违反规则的异常数据 / 跨期继承)
            if (!had_non_up) {
                acc.lose_5050++;
            }
            had_non_up = true;
        }
        current_pity = 0;
    }

    acc.censored_pity_all = current_pity;
    acc.censored_pity_up  = pity_since_last_up;

    if (acc.max_pity_all > 199) acc.max_pity_all = 199;
    if (acc.max_pity_up  > 199) acc.max_pity_up  = 199;
    if (acc.censored_pity_all > 199) acc.censored_pity_all = 199;
    if (acc.censored_pity_up  > 199) acc.censored_pity_up  = 199;

    StatsResult s;
    s.freq_all  = acc.freq_all;
    s.freq_up   = acc.freq_up;
    s.count_all = acc.count_all;
    s.count_up  = acc.count_up;
    s.win_5050  = acc.win_5050;
    s.lose_5050 = acc.lose_5050;
    s.censored_pity_all = acc.censored_pity_all;
    s.censored_pity_up  = acc.censored_pity_up;

    if (acc.count_all > 0) {
        s.avg_all = (double)acc.sum_all / acc.count_all;
        double var = SampleVariance(acc.sum_all, acc.sum_sq_all, acc.count_all);
        double std_all = std::sqrt(var);
        s.cv_all = (s.avg_all > 0) ? std_all / s.avg_all : 0;
        double t_crit = TCritical95(acc.count_all - 1);
        s.ci_all_err = t_crit * std_all / std::sqrt((double)acc.count_all);

        const double* cdf_tbl = isWEngine ? g_cdf_wengine : g_cdf_agent;
        int cdf_len = isWEngine ? 82 : 92;
        s.ks_d_all = ComputeKS(acc.freq_all, acc.max_pity_all, acc.count_all,
                               cdf_tbl, cdf_len);
        s.ks_is_normal = (s.ks_d_all <= (1.36 / std::sqrt((double)acc.count_all)));
    }

    if (acc.count_all > 0 || acc.censored_pity_all > 0) {
        int survivors = acc.count_all + (acc.censored_pity_all > 0 ? 1 : 0);
        int max_reach_all = (std::max)(acc.max_pity_all, acc.censored_pity_all);
        if (max_reach_all > 199) max_reach_all = 199;
        for (int x = 1; x <= max_reach_all; ++x) {
            if (survivors > 0) {
                s.hazard_all[x] = (double)acc.freq_all[x] / survivors;
                survivors -= acc.freq_all[x];
                if (x == acc.censored_pity_all) survivors -= 1;
            }
        }
    }

    if (acc.count_up > 0) {
        s.avg_up = (double)acc.sum_up / acc.count_up;
        double var = SampleVariance(acc.sum_up, acc.sum_sq_up, acc.count_up);
        double std_up = std::sqrt(var);
        double t_crit = TCritical95(acc.count_up - 1);
        s.ci_up_err = t_crit * std_up / std::sqrt((double)acc.count_up);

        const double* cdf_up_tbl = isWEngine ? g_cdf_wengine_up : g_cdf_agent_up;
        int cdf_up_len = isWEngine ? 162 : 182;
        s.ks_d_up = ComputeKS(acc.freq_up, acc.max_pity_up, acc.count_up,
                              cdf_up_tbl, cdf_up_len);
        s.ks_is_normal_up = (s.ks_d_up <= (1.36 / std::sqrt((double)acc.count_up)));
    }

    if (acc.count_up > 0 || acc.censored_pity_up > 0) {
        int survivors = acc.count_up + (acc.censored_pity_up > 0 ? 1 : 0);
        int max_reach_up = (std::max)(acc.max_pity_up, acc.censored_pity_up);
        if (max_reach_up > 199) max_reach_up = 199;
        for (int x = 1; x <= max_reach_up; ++x) {
            if (survivors > 0) {
                s.hazard_up[x] = (double)acc.freq_up[x] / survivors;
                survivors -= acc.freq_up[x];
                if (x == acc.censored_pity_up) survivors -= 1;
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

inline std::wstring GetDynamicWindowText(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    std::wstring buf((size_t)len, L'\0');
    GetWindowTextW(hwnd, buf.data(), len + 1);
    return buf;
}

// ---------------------------------------------------------
// [文件处理 - 工作线程化]
// ---------------------------------------------------------
#define WM_APP_PROCESS_DONE  (WM_APP + 1)

void RebuildChartCache(HWND hwnd);

struct ProcessOutput {
    HWND        hwnd_main = NULL;

    HANDLE      hFile = INVALID_HANDLE_VALUE;
    HANDLE      hMap  = NULL;
    const void* viewPtr = nullptr;
    DWORD       fileSize = 0;

    // 输入文本: 仅 2 个输入框
    //   utf8_agentStd:   常驻 S 级代理人
    //   utf8_wengineStd: 常驻 S 级音擎
    // UP 判定逻辑: S 级名字不在常驻列表里, 就是 UP. 简单且稳定 — 用户只需维护
    // 这一份相对静态的"常驻 S 级"列表 (绝区零开服至今 6 个常驻代理人 + 6 个常驻音擎).
    // 不需要每开新池就更新 "当期 UP" 列表.
    std::string utf8_agentStd;
    std::string utf8_wengineStd;

    bool        ok = false;
    StatsResult statsAgent;
    StatsResult statsWEngine;
    std::wstring outMsg;
    std::wstring errMsg;

    ~ProcessOutput() {
        if (viewPtr) UnmapViewOfFile(viewPtr);
        if (hMap)    CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    }
};

static volatile LONG g_processing = 0;

DWORD WINAPI ProcessFile_Worker(LPVOID arg) {
    auto* out = (ProcessOutput*)arg;

    auto agentStd   = ParseCommaSeparatedUtf8FromUtf8(out->utf8_agentStd);
    auto wengineStd = ParseCommaSeparatedUtf8FromUtf8(out->utf8_wengineStd);

    std::string_view bufferView((const char*)out->viewPtr, out->fileSize);
    if (bufferView.size() >= 3 &&
        (unsigned char)bufferView[0] == 0xEF &&
        (unsigned char)bufferView[1] == 0xBB &&
        (unsigned char)bufferView[2] == 0xBF) {
        bufferView.remove_prefix(3);
    }

    std::array<std::byte, 2 * 1024 * 1024> stackBuffer;
    std::pmr::monotonic_buffer_resource pool(stackBuffer.data(), stackBuffer.size());
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);

    struct Temp {
        long long id;
        ItemType  it;
        GachaType gt;
        RankType  rt;
        std::string_view name;
    };
    std::pmr::vector<Temp> temps(alloc);
    temps.reserve(10000);

    // ForEachJsonObject 找的是 "list" 这个 key.
    // UIGF v4.2 的 nap 顶层结构:
    //   { "info": {...},
    //     "nap": [ { "uid", "timezone", "lang", "list": [ {...}, {...}, ... ] } ] }
    // 顶层 info 没有 list, nap[0] 内层有, 所以直接命中正确数组.
    // (这个 ForEachJsonObject 实现只匹配第一处 "list", 多账号文件会被截断,
    //  但 UIGF v4.2 nap 数组通常只有一个 uid 元素, 实际不影响.)
    ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
        ItemType  it = ParseItemType (ExtractJsonValue(itemStr, "item_type",  true));
        RankType  rt = ParseRankType (ExtractJsonValue(itemStr, "rank_type",  true));
        GachaType gt = ParseGachaType(ExtractJsonValue(itemStr, "gacha_type", true));

        // 只关心代理人 UP 池("2") 和音擎 UP 池("3")
        // 常驻("1")/邦布("5") 不进入分析 (没有 UP 概念, 分析意义不大)
        // 此外: 代理人 UP 池里也会出"音擎/邦布 A 级", 这些不是 S 级出货, 不影响分析
        if (gt != GachaType::AgentUP && gt != GachaType::WEngineUP) return;

        // ZZZ name 字段名是 "name" (不是终末地的 "item_name")
        std::string_view name = ExtractJsonValue(itemStr, "name", true);

        std::string_view idStr = ExtractJsonValue(itemStr, "id", true);
        if (idStr.empty()) idStr = ExtractJsonValue(itemStr, "id", false);
        long long parsed_id = 0;
        if (!idStr.empty()) {
            std::from_chars(idStr.data(), idStr.data() + idStr.size(), parsed_id);
        }

        temps.push_back(Temp{parsed_id, it, gt, rt, name});
    });

    if (temps.empty()) {
        out->ok = false;
        out->errMsg = L"JSON 解析失败或无数据。";
        PostMessageW(out->hwnd_main, WM_APP_PROCESS_DONE, 0, (LPARAM)out);
        return 0;
    }

    // 排序: 绝区零的 id 是 19 位全局递增 ID, 直接按 id 升序就是按时间升序.
    // 不需要终末地版本里"角色武器分区 + 时间戳排序"的复杂规则.
    auto less = [](const Temp& a, const Temp& b) { return a.id < b.id; };
    bool sorted = true;
    for (size_t i = 1; i < temps.size(); ++i) {
        if (less(temps[i], temps[i - 1])) { sorted = false; break; }
    }
    if (!sorted) std::ranges::sort(temps, less);

    PullBucket bucketAgent  (alloc); bucketAgent.reserve(6000);
    PullBucket bucketWEngine(alloc); bucketWEngine.reserve(4000);
    for (const auto& t : temps) {
        if (t.gt == GachaType::AgentUP) {
            bucketAgent.push_back(t.rt, t.name);
        } else if (t.gt == GachaType::WEngineUP) {
            bucketWEngine.push_back(t.rt, t.name);
        }
    }

    out->statsAgent   = Calculate(bucketAgent,   /*isWEngine=*/false, agentStd);
    out->statsWEngine = Calculate(bucketWEngine, /*isWEngine=*/true,  wengineStd);

    wchar_t winAgentStr[64] = L"[无数据]";  // [无数据]
    if (out->statsAgent.avg_win >= 0)
        swprintf(winAgentStr, 64, L"%.2f 抽", out->statsAgent.avg_win);  // X 抽

    wchar_t winWEngineStr[64] = L"[无数据]";
    if (out->statsWEngine.avg_win >= 0)
        swprintf(winWEngineStr, 64, L"%.2f 抽", out->statsWEngine.avg_win);

    wchar_t pendAgentStr[128] = L"";
    if (out->statsAgent.censored_pity_all > 0 || out->statsAgent.censored_pity_up > 0) {
        // [当前垫刀: 距上次S级 N 抽 / 距上次UP M 抽]
        swprintf(pendAgentStr, 128,
            L"  [当前垫刀: 距上次S级 %d 抽 / 距上次UP %d 抽]",
            out->statsAgent.censored_pity_all, out->statsAgent.censored_pity_up);
    }
    wchar_t pendWEngineStr[128] = L"";
    if (out->statsWEngine.censored_pity_all > 0 || out->statsWEngine.censored_pity_up > 0) {
        swprintf(pendWEngineStr, 128,
            L"  [当前垫刀: 距上次S级 %d 抽 / 距上次UP %d 抽]",
            out->statsWEngine.censored_pity_all, out->statsWEngine.censored_pity_up);
    }

    auto ksLabel = [](const StatsResult& r) -> const wchar_t* {
        if (r.count_all == 0) return L"-";
        return r.ks_is_normal
            ? L"符合理论模型"   // 符合理论模型
            : L"偏离过大";                          // 偏离过大
    };
    auto ksUpLabel = [](const StatsResult& r) -> const wchar_t* {
        if (r.count_up == 0) return L"-";
        return r.ks_is_normal_up
            ? L"符合理论模型"
            : L"偏离过大";
    };

    // 文本里的所有中文都用 wchar_t 字面量更清晰; 但为了对齐排版用 \xXX 序列也可
    // 这里直接写 L"中文"
    wchar_t outMsg[3200];
    // 理论值参考:
    //   代理人池综合 6 星: 62.30 抽
    //   代理人池 UP:        93.45 抽
    //   音擎池综合 6 星:    49.71 抽
    //   音擎池 UP:          62.13 抽
    swprintf(outMsg, 3200,
        L"【独家频段 (代理人 UP 池)】 总计 S 级: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合 S 级 (含歪) 出货平均期望:     %5.2f 抽 (理论 ≈ 62.30)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽 (理论 ≈ 93.45)   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (理论 50%%) (%d 胜 %d 负)\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls\r\n\r\n"
        L"【音擎频段 (武器 UP 池)】 总计 S 级: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合 S 级 (含歪) 出货平均期望:     %5.2f 抽 (理论 ≈ 49.71)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽 (理论 ≈ 62.13)   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (理论 75%%) (%d 胜 %d 负)\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls",
        out->statsAgent.count_all, out->statsAgent.count_up, pendAgentStr,
        out->statsAgent.avg_all, (std::max)(1.0, out->statsAgent.avg_all - out->statsAgent.ci_all_err),
        out->statsAgent.avg_all + out->statsAgent.ci_all_err, out->statsAgent.cv_all * 100.0,
        out->statsAgent.ks_d_all, ksLabel(out->statsAgent),
        out->statsAgent.avg_up, (std::max)(1.0, out->statsAgent.avg_up - out->statsAgent.ci_up_err),
        out->statsAgent.avg_up + out->statsAgent.ci_up_err,
        out->statsAgent.win_rate_5050 >= 0 ? out->statsAgent.win_rate_5050 * 100.0 : 0.0,
        out->statsAgent.win_5050, out->statsAgent.lose_5050,
        out->statsAgent.ks_d_up, ksUpLabel(out->statsAgent),
        winAgentStr,
        out->statsWEngine.count_all, out->statsWEngine.count_up, pendWEngineStr,
        out->statsWEngine.avg_all, (std::max)(1.0, out->statsWEngine.avg_all - out->statsWEngine.ci_all_err),
        out->statsWEngine.avg_all + out->statsWEngine.ci_all_err, out->statsWEngine.cv_all * 100.0,
        out->statsWEngine.ks_d_all, ksLabel(out->statsWEngine),
        out->statsWEngine.avg_up, (std::max)(1.0, out->statsWEngine.avg_up - out->statsWEngine.ci_up_err),
        out->statsWEngine.avg_up + out->statsWEngine.ci_up_err,
        out->statsWEngine.win_rate_5050 >= 0 ? out->statsWEngine.win_rate_5050 * 100.0 : 0.0,
        out->statsWEngine.win_5050, out->statsWEngine.lose_5050,
        out->statsWEngine.ks_d_up, ksUpLabel(out->statsWEngine),
        winWEngineStr
    );
    out->outMsg = outMsg;
    out->ok = true;

    PostMessageW(out->hwnd_main, WM_APP_PROCESS_DONE, 0, (LPARAM)out);
    return 0;
}

bool ProcessFile_Submit(HWND hwnd, const std::wstring& path) {
    if (InterlockedCompareExchange(&g_processing, 1, 0) != 0) {
        return false;
    }

    auto out = std::make_unique<ProcessOutput>();
    out->hwnd_main = hwnd;

    out->utf8_agentStd   = WideToUtf8(GetDynamicWindowText(hAgentEdit));
    out->utf8_wengineStd = WideToUtf8(GetDynamicWindowText(hWEngineEdit));

    out->hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out->hFile == INVALID_HANDLE_VALUE) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    out->fileSize = GetFileSize(out->hFile, NULL);
    if (out->fileSize == 0 || out->fileSize == INVALID_FILE_SIZE) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    out->hMap = CreateFileMappingW(out->hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!out->hMap) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    out->viewPtr = MapViewOfFile(out->hMap, FILE_MAP_READ, 0, 0, 0);
    if (!out->viewPtr) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }

    HANDLE hThread = CreateThread(NULL, 4 * 1024 * 1024,
                                  ProcessFile_Worker, out.get(), 0, NULL);
    if (!hThread) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    CloseHandle(hThread);
    out.release();
    return true;
}

void ProcessFile_Consume(HWND hwnd, ProcessOutput* out) {
    if (out->ok) {
        statsAgent   = out->statsAgent;
        statsWEngine = out->statsWEngine;
        SetWindowTextW(hOutEdit, out->outMsg.c_str());
        RebuildChartCache(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
    } else {
        SetWindowTextW(hOutEdit,
            out->errMsg.empty()
              ? L"处理失败，请检查文件格式"
              : out->errMsg.c_str());
    }
    delete out;
    InterlockedExchange(&g_processing, 0);
}
// -------------------------------------------------------
// 图形渲染
// -------------------------------------------------------

// ---------------------------------------------------------
// [ECDF (经验累积分布函数) 图]
//
// 设计同终末地版本: 离散阶梯线 + 理论 CDF 虚线 + KS 标记
// 与终末地的差异:
//   - 数组容量改为 200 (代理人 UP 池 CDF 长度可达 180+1)
//   - 不再有 ecdf_up_step_size 参数: 绝区零没有"10 抽一组"的离散判定 (终末地
//     武器池有, 因为它是十连绑定), 所有点都是单抽粒度, stepSize 恒为 1
// ---------------------------------------------------------
void DrawECDF(Gdiplus::Graphics& g, Gdiplus::Rect rect,
              const std::array<int, 200>& freq_all, const std::array<int, 200>& freq_up,
              int count_all, int count_up,
              [[maybe_unused]] int censored_all, [[maybe_unused]] int censored_up,
              const double* theory_cdf_all, int theory_cdf_all_len,
              const double* theory_cdf_up,  int theory_cdf_up_len,
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
    bool hasData = (count_all > 0) || (count_up > 0);
    for (int i = 1; i < 200; i++) {
        if (freq_all[i] > 0 || freq_up[i] > 0) {
            if (i > max_x) max_x = i;
        }
    }
    if (!hasData) {
        Gdiplus::Font emptyFont(&fontFamily, DPIScaleF(14.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush emptyBrush(Gdiplus::Color(255, 150, 150, 150));
        g.DrawString(L"暂无出金数据", -1, &emptyFont,  // 暂无出金数据
                     Gdiplus::PointF((float)rect.X + (float)rect.Width / 2.0f - DPIScaleF(50.0f),
                                     (float)rect.Y + (float)rect.Height / 2.0f),
                     &emptyBrush);
        return;
    }
    max_x = ((max_x / 10) + 1) * 10;

    Gdiplus::Pen gridPen(Gdiplus::Color(255, 230, 230, 230), DPIScaleF(1.0f));
    Gdiplus::Pen axisPen(Gdiplus::Color(255, 80, 80, 80),  DPIScaleF(1.0f));
    float plotX = (float)rect.X + DPIScaleF(50.0f);
    float plotY = (float)rect.Y + DPIScaleF(40.0f);
    float plotW = (float)rect.Width  - DPIScaleF(70.0f);
    float plotH = (float)rect.Height - DPIScaleF(60.0f);
    if (plotW <= 0 || plotH <= 0) return;

    g.DrawLine(&axisPen, plotX, plotY,         plotX, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);

    auto getPt = [&](int x, double y) -> Gdiplus::PointF {
        if (y < 0) y = 0; if (y > 1) y = 1;
        return Gdiplus::PointF(plotX + (float)x / (float)max_x * plotW,
                               plotY + plotH - (float)y * plotH);
    };

    Gdiplus::Font tickFont(&fontFamily, DPIScaleF(11.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255, 120, 120, 120));

    for (int i = 0; i <= 4; ++i) {
        double y_val = (double)i / 4.0;
        float py = plotY + plotH - (float)y_val * plotH;
        if (i > 0) g.DrawLine(&gridPen, plotX, py, plotX + plotW, py);
        g.DrawLine(&axisPen, plotX - DPIScaleF(5.0f), py, plotX, py);
        wchar_t y_label[16]; swprintf(y_label, 16, L"%d%%", i * 25);
        float labelW = (float)wcslen(y_label) * DPIScaleF(5.5f) + DPIScaleF(8.0f);
        g.DrawString(y_label, -1, &tickFont, Gdiplus::PointF(plotX - labelW, py - DPIScaleF(6.0f)), &tickBrush);
    }
    int step = (max_x > 140) ? 20 : 10;
    for (int x = 0; x <= max_x; x += step) {
        float px = plotX + (float)x / (float)max_x * plotW;
        g.DrawLine(&axisPen, px, plotY + plotH, px, plotY + plotH + DPIScaleF(5.0f));
        wchar_t x_label[16]; swprintf(x_label, 16, L"%d", x);
        float xoff = (x < 10 ? 4.0f : x < 100 ? 8.0f : 12.0f) * DPIScaleF(1.0f);
        g.DrawString(x_label, -1, &tickFont,
                     Gdiplus::PointF(px - xoff, plotY + plotH + DPIScaleF(8.0f)), &tickBrush);
    }

    // 画理论 CDF (虚线, 折线模式 + 自动跳跃检测)
    // 与终末地版本完全一致, 跳跃检测也完美适用于绝区零的"软保底响应"段
    auto drawTheoryCDF = [&](const double* cdf, int cdf_len, Gdiplus::Color color) {
        if (!cdf || cdf_len < 2) return;
        Gdiplus::Pen pen(color, DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(4.0f), DPIScaleF(3.0f) };
        pen.SetDashPattern(dash, 2);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        int upper = (cdf_len - 1 < max_x) ? cdf_len - 1 : max_x;
        if (upper < 1) return;
        Gdiplus::GraphicsPath path;
        auto p0 = getPt(0, cdf[0]);
        Gdiplus::PointF prev = p0;
        constexpr double JUMP_THRESHOLD = 5.0;
        constexpr double MIN_PREV_DELTA = 1e-6;
        bool inStepMode = false;
        for (int k = 1; k <= upper; ++k) {
            double curDelta  = cdf[k] - cdf[k-1];
            double prevDelta = (k >= 2) ? cdf[k-1] - cdf[k-2] : 0.0;
            bool drawAsStep;
            if (inStepMode) {
                if (curDelta > prevDelta && prevDelta > MIN_PREV_DELTA) {
                    drawAsStep = true;
                } else {
                    inStepMode = false;
                    drawAsStep = false;
                }
            } else {
                if (prevDelta > MIN_PREV_DELTA
                    && curDelta / prevDelta > JUMP_THRESHOLD) {
                    inStepMode = true;
                    drawAsStep = true;
                } else {
                    drawAsStep = false;
                }
            }
            if (drawAsStep) {
                auto pH = getPt(k, cdf[k - 1]);
                auto pV = getPt(k, cdf[k]);
                path.AddLine(prev, pH);
                path.AddLine(pH, pV);
                prev = pV;
            } else {
                auto p = getPt(k, cdf[k]);
                path.AddLine(prev, p);
                prev = p;
            }
        }
        g.DrawPath(&pen, &path);
    };

    auto drawEmpiricalECDF = [&](const std::array<int, 200>& freq, int total,
                                  Gdiplus::Color color) {
        if (total == 0) return;
        Gdiplus::Pen pen(color, DPIScaleF(2.5f));
        double cum = 0.0;
        auto prev_pt = getPt(0, 0);
        for (int k = 1; k <= max_x; ++k) {
            if (freq[k] == 0) continue;
            auto h_end = getPt(k, cum);
            g.DrawLine(&pen, prev_pt.X, prev_pt.Y, h_end.X, h_end.Y);
            cum += (double)freq[k] / (double)total;
            auto v_end = getPt(k, cum);
            g.DrawLine(&pen, h_end.X, h_end.Y, v_end.X, v_end.Y);
            prev_pt = v_end;
        }
        auto end_pt = getPt(max_x, cum);
        g.DrawLine(&pen, prev_pt.X, prev_pt.Y, end_pt.X, end_pt.Y);
    };

    drawTheoryCDF(theory_cdf_all, theory_cdf_all_len, Gdiplus::Color(180, 65, 140, 240));
    drawTheoryCDF(theory_cdf_up,  theory_cdf_up_len,  Gdiplus::Color(180, 240, 80, 80));
    drawEmpiricalECDF(freq_all, count_all, Gdiplus::Color(255, 65, 140, 240));
    drawEmpiricalECDF(freq_up,  count_up,  Gdiplus::Color(255, 240, 80, 80));

    // KS 标记
    enum class KSLabelAnchor { LeftTop, RightBottom };
    auto drawKSMarker = [&](const std::array<int, 200>& freq, int total,
                            const double* cdf, int cdf_len,
                            BYTE r, BYTE gC, BYTE b,
                            KSLabelAnchor anchor) {
        if (total == 0 || !cdf || cdf_len < 2) return;
        double max_d = 0; int max_d_x = 0;
        double cum = 0;
        for (int k = 1; k <= max_x && k < cdf_len; ++k) {
            cum += (double)freq[k] / (double)total;
            double d = std::fabs(cum - cdf[k]);
            if (d > max_d) { max_d = d; max_d_x = k; }
        }
        if (max_d <= 0.01 || max_d_x <= 0) return;
        double emp_y = 0;
        for (int k = 1; k <= max_d_x; ++k) emp_y += (double)freq[k] / (double)total;
        double th_y = cdf[max_d_x];
        auto p_emp = getPt(max_d_x, emp_y);
        auto p_th  = getPt(max_d_x, th_y);
        Gdiplus::Pen ksPen(Gdiplus::Color(255, r, gC, b), DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(2.0f), DPIScaleF(2.0f) };
        ksPen.SetDashPattern(dash, 2);
        g.DrawLine(&ksPen, p_emp.X, p_emp.Y, p_th.X, p_th.Y);

        wchar_t lbl[32];
        swprintf(lbl, 32, L"KS D=%.3f", max_d);
        float midY = (p_emp.Y + p_th.Y) * 0.5f;

        Gdiplus::RectF box;
        g.MeasureString(lbl, -1, &tickFont, Gdiplus::PointF(0, 0), &box);

        float tx, ty;
        if (anchor == KSLabelAnchor::LeftTop) {
            tx = p_emp.X - DPIScaleF(4.0f) - box.Width;
            ty = midY - DPIScaleF(2.0f) - box.Height;
        } else {
            tx = p_emp.X + DPIScaleF(4.0f);
            ty = midY + DPIScaleF(2.0f);
        }

        Gdiplus::SolidBrush whiteBr(Gdiplus::Color(255, 252, 253, 255));
        for (int dx = -1; dx <= 1; dx += 2) {
            for (int dy = -1; dy <= 1; dy += 2) {
                g.DrawString(lbl, -1, &tickFont,
                             Gdiplus::PointF(tx + (float)dx, ty + (float)dy),
                             &whiteBr);
            }
        }
        Gdiplus::SolidBrush mainBr(Gdiplus::Color(255, r, gC, b));
        g.DrawString(lbl, -1, &tickFont, Gdiplus::PointF(tx, ty), &mainBr);
    };
    drawKSMarker(freq_all, count_all, theory_cdf_all, theory_cdf_all_len,
                 65, 140, 240, KSLabelAnchor::LeftTop);
    drawKSMarker(freq_up, count_up, theory_cdf_up, theory_cdf_up_len,
                 240, 80, 80, KSLabelAnchor::RightBottom);

    // 图例
    Gdiplus::Font legendFont(&fontFamily, DPIScaleF(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush blueBr(Gdiplus::Color(255, 65, 140, 240));
    Gdiplus::SolidBrush redBr (Gdiplus::Color(255, 240, 80, 80));

    const wchar_t* legAll  = L"综合 S 级 ECDF";       // 综合 S 级 ECDF
    const wchar_t* legUp   = L"当期限定 UP ECDF";  // 当期限定 UP ECDF
    const wchar_t* legThy  = L"理论 CDF (综合)";  // 理论 CDF (综合)

    auto measureW = [&](const wchar_t* s) -> float {
        Gdiplus::RectF box;
        g.MeasureString(s, -1, &legendFont, Gdiplus::PointF(0, 0), &box);
        return box.Width;
    };
    const float swatchW    = DPIScaleF(14.0f);
    const float swatchGap  = DPIScaleF(6.0f);
    const float entryGap   = DPIScaleF(16.0f);
    const float legendY    = (float)rect.Y + DPIScaleF(12.0f);
    const float swatchYOff = DPIScaleF(8.0f);

    float wAll  = measureW(legAll);
    float wUp   = measureW(legUp);
    float wThy  = measureW(legThy);
    float xRight = (float)rect.X + (float)rect.Width - DPIScaleF(12.0f);

    float xThyText = xRight - wThy;
    float xThySw   = xThyText - swatchGap - swatchW;
    {
        Gdiplus::Pen dashPen(Gdiplus::Color(255, 130, 130, 130), DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(2.5f), DPIScaleF(2.0f) };
        dashPen.SetDashPattern(dash, 2);
        g.DrawLine(&dashPen, xThySw, legendY + swatchYOff,
                   xThySw + swatchW, legendY + swatchYOff);
    }
    g.DrawString(legThy, -1, &legendFont,
                 Gdiplus::PointF(xThyText, legendY), &textBrush);

    float xUpText = xThySw - entryGap - wUp;
    float xUpSw   = xUpText - swatchGap - swatchW;
    g.FillRectangle(&redBr, xUpSw, legendY + swatchYOff - DPIScaleF(1.5f),
                    swatchW, DPIScaleF(3.0f));
    g.DrawString(legUp, -1, &legendFont,
                 Gdiplus::PointF(xUpText, legendY), &textBrush);

    float xAllText = xUpSw - entryGap - wAll;
    float xAllSw   = xAllText - swatchGap - swatchW;
    g.FillRectangle(&blueBr, xAllSw, legendY + swatchYOff - DPIScaleF(1.5f),
                    swatchW, DPIScaleF(3.0f));
    g.DrawString(legAll, -1, &legendFont,
                 Gdiplus::PointF(xAllText, legendY), &textBrush);
}

// ---------------------------------------------------------
// [MRL (Mean Residual Life) 图]
// ---------------------------------------------------------
void DrawMRL(Gdiplus::Graphics& g, Gdiplus::Rect rect,
             const std::array<int, 200>& freq_all,
             const std::array<int, 200>& freq_up,
             int count_all, int count_up,
             int censored_all, int censored_up,
             const double* theory_cdf_all, int theory_cdf_all_len,
             const double* theory_cdf_up,  int theory_cdf_up_len,
             const std::wstring& title, int limit_base,
             int theory_all_cap = 0, int theory_up_cap = 0) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255));
    g.FillRectangle(&bgBrush, rect);
    Gdiplus::FontFamily fontFamily(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&fontFamily, DPIScaleF(15.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 40, 40, 40));
    g.DrawString(title.c_str(), -1, &titleFont,
                 Gdiplus::PointF((float)rect.X + DPIScaleF(15.0f), (float)rect.Y + DPIScaleF(12.0f)),
                 &textBrush);

    int max_x = limit_base;
    bool hasData = (count_all > 0) || (count_up > 0);
    for (int i = 1; i < 200; i++) {
        if (freq_all[i] > 0 || freq_up[i] > 0) if (i > max_x) max_x = i;
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

    auto computeEmpiricalMRL = [&](const std::array<int, 200>& freq, int total)
        -> std::pair<std::array<double, 200>, std::array<int, 200>> {
        std::array<double, 200> mrl{}; mrl.fill(-1.0);
        std::array<int, 200> surv{}; surv.fill(0);
        if (total == 0) return {mrl, surv};
        long long suf_count = 0, suf_weighted = 0;
        for (int t = max_x; t >= 0; --t) {
            surv[t] = (int)suf_count;
            if (suf_count >= 1) {
                mrl[t] = (double)(suf_weighted - (long long)t * suf_count) / (double)suf_count;
            }
            if (t >= 1) {
                suf_count    += freq[t];
                suf_weighted += (long long)t * freq[t];
            }
        }
        return {mrl, surv};
    };

    auto mrl_all = computeEmpiricalMRL(freq_all, count_all);
    auto mrl_up  = computeEmpiricalMRL(freq_up,  count_up);

    auto computeTheoryMRL = [&](const double* cdf, int cdf_len) {
        std::array<double, 200> tmrl{}; tmrl.fill(-1.0);
        if (!cdf || cdf_len < 2) return tmrl;
        int upper = cdf_len - 1;
        for (int t = 0; t <= upper - 1 && t <= max_x; ++t) {
            double surv_t = 1.0 - cdf[t];
            if (surv_t < 1e-9) break;
            double num = 0.0;
            for (int k = t + 1; k <= upper; ++k) {
                double pdf_k = cdf[k] - cdf[k-1];
                num += (double)(k - t) * pdf_k;
            }
            tmrl[t] = num / surv_t;
        }
        return tmrl;
    };
    auto theory_mrl_all = computeTheoryMRL(theory_cdf_all, theory_cdf_all_len);
    auto theory_mrl_up  = computeTheoryMRL(theory_cdf_up,  theory_cdf_up_len);

    double max_y = 1.0;
    for (int t = 0; t <= max_x; ++t) {
        if (mrl_all.first[t] > max_y) max_y = mrl_all.first[t];
        if (mrl_up.first[t]  > max_y) max_y = mrl_up.first[t];
        if (theory_mrl_all[t] > max_y) max_y = theory_mrl_all[t];
        if (theory_mrl_up[t]  > max_y) max_y = theory_mrl_up[t];
    }
    max_y = std::ceil(max_y * 1.1 / 10.0) * 10.0;
    if (max_y < 10) max_y = 10;

    Gdiplus::Pen gridPen(Gdiplus::Color(255, 230, 230, 230), DPIScaleF(1.0f));
    Gdiplus::Pen axisPen(Gdiplus::Color(255, 80, 80, 80),  DPIScaleF(1.0f));
    float plotX = (float)rect.X + DPIScaleF(50.0f);
    float plotY = (float)rect.Y + DPIScaleF(40.0f);
    float plotW = (float)rect.Width  - DPIScaleF(70.0f);
    float plotH = (float)rect.Height - DPIScaleF(60.0f);
    if (plotW <= 0 || plotH <= 0) return;

    g.DrawLine(&axisPen, plotX, plotY,         plotX, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);

    auto getPt = [&](int x, double y) -> Gdiplus::PointF {
        if (y < 0) y = 0; if (y > max_y) y = max_y;
        return Gdiplus::PointF(plotX + (float)x / (float)max_x * plotW,
                               plotY + plotH - (float)(y / max_y) * plotH);
    };

    Gdiplus::Font tickFont(&fontFamily, DPIScaleF(11.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255, 120, 120, 120));

    for (int i = 0; i <= 4; ++i) {
        double y_val = max_y * (double)i / 4.0;
        float py = plotY + plotH - (float)i / 4.0f * plotH;
        if (i > 0) g.DrawLine(&gridPen, plotX, py, plotX + plotW, py);
        g.DrawLine(&axisPen, plotX - DPIScaleF(5.0f), py, plotX, py);
        wchar_t y_label[16]; swprintf(y_label, 16, L"%.0f", y_val);
        float labelW = (float)wcslen(y_label) * DPIScaleF(5.5f) + DPIScaleF(8.0f);
        g.DrawString(y_label, -1, &tickFont, Gdiplus::PointF(plotX - labelW, py - DPIScaleF(6.0f)), &tickBrush);
    }
    int step = (max_x > 140) ? 20 : 10;
    for (int x = 0; x <= max_x; x += step) {
        float px = plotX + (float)x / (float)max_x * plotW;
        g.DrawLine(&axisPen, px, plotY + plotH, px, plotY + plotH + DPIScaleF(5.0f));
        wchar_t x_label[16]; swprintf(x_label, 16, L"%d", x);
        float xoff = (x < 10 ? 4.0f : x < 100 ? 8.0f : 12.0f) * DPIScaleF(1.0f);
        g.DrawString(x_label, -1, &tickFont,
                     Gdiplus::PointF(px - xoff, plotY + plotH + DPIScaleF(8.0f)), &tickBrush);
    }

    auto drawTheoryMRL = [&](const std::array<double, 200>& tmrl, int cap, Gdiplus::Color color) {
        Gdiplus::Pen pen(color, DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(4.0f), DPIScaleF(3.0f) };
        pen.SetDashPattern(dash, 2);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        int upper = (cap > 0 && cap <= max_x) ? cap : max_x;
        Gdiplus::GraphicsPath path;
        Gdiplus::PointF prev;
        bool has_prev = false;
        for (int t = 0; t <= upper; ++t) {
            if (tmrl[t] < 0) continue;
            auto p = getPt(t, tmrl[t]);
            if (has_prev) path.AddLine(prev, p);
            prev = p; has_prev = true;
        }
        if (has_prev) g.DrawPath(&pen, &path);
    };
    drawTheoryMRL(theory_mrl_all, theory_all_cap, Gdiplus::Color(180, 65, 140, 240));
    drawTheoryMRL(theory_mrl_up,  theory_up_cap,  Gdiplus::Color(180, 240, 80, 80));

    auto drawEmpiricalMRL = [&](const std::pair<std::array<double, 200>, std::array<int, 200>>& mrl_data,
                                 BYTE r, BYTE gC, BYTE b) {
        const auto& mrl = mrl_data.first;
        const auto& surv = mrl_data.second;
        Gdiplus::Pen thickPen(Gdiplus::Color(255, r, gC, b), DPIScaleF(2.5f));
        thickPen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::Pen thinPen (Gdiplus::Color(115, r, gC, b), DPIScaleF(1.8f));
        thinPen.SetLineJoin(Gdiplus::LineJoinRound);

        Gdiplus::GraphicsPath thickPath, thinPath;
        Gdiplus::PointF prev; bool has_prev = false; bool prev_thick = true;
        for (int t = 0; t <= max_x; ++t) {
            if (mrl[t] < 0 || surv[t] == 0) {
                if (has_prev) {
                    thickPath.StartFigure();
                    thinPath.StartFigure();
                }
                has_prev = false; continue;
            }
            auto p = getPt(t, mrl[t]);
            bool thick = (surv[t] >= 2);
            if (has_prev) {
                if (thick && prev_thick) thickPath.AddLine(prev, p);
                else                     thinPath.AddLine(prev, p);
            }
            prev = p; has_prev = true; prev_thick = thick;
        }
        g.DrawPath(&thickPen, &thickPath);
        g.DrawPath(&thinPen,  &thinPath);
    };
    drawEmpiricalMRL(mrl_all, 65, 140, 240);
    drawEmpiricalMRL(mrl_up,  240, 80, 80);

    // "你在这里" 竖线
    struct CensoredLabel {
        std::wstring text;
        Gdiplus::Color color;
    };
    std::vector<CensoredLabel> censoredLabels;
    censoredLabels.reserve(2);

    auto resolveAndDrawLine = [&](int censored,
                                   const std::pair<std::array<double, 200>, std::array<int, 200>>& mrl_data,
                                   const std::array<double, 200>& tmrl,
                                   int theory_cap,
                                   BYTE r, BYTE gC, BYTE b) {
        if (censored <= 0 || censored > max_x) return;
        double y_value = -1.0;
        if (censored < (int)tmrl.size() && tmrl[censored] > 0
            && (theory_cap == 0 || censored <= theory_cap)) {
            y_value = tmrl[censored];
        }
        if (y_value <= 0 && mrl_data.first[censored] > 0) {
            y_value = mrl_data.first[censored];
        }
        if (y_value <= 0) return;
        Gdiplus::Color color(255, r, gC, b);
        Gdiplus::Pen markPen(color, DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(4.0f), DPIScaleF(3.0f) };
        markPen.SetDashPattern(dash, 2);
        auto top = getPt(censored, y_value);
        g.DrawLine(&markPen, top.X, top.Y, top.X, plotY + plotH);

        wchar_t lbl[64];
        // 已垫 X 抽 · 预期还需 Y.Y
        swprintf(lbl, 64, L"已垫 %d 抽 · 预期还需 %.1f",
                 censored, y_value);
        censoredLabels.push_back({ std::wstring(lbl), color });
    };
    resolveAndDrawLine(censored_all, mrl_all, theory_mrl_all, theory_all_cap, 65, 140, 240);
    resolveAndDrawLine(censored_up,  mrl_up,  theory_mrl_up,  theory_up_cap,  240, 80, 80);

    if (!censoredLabels.empty()) {
        Gdiplus::StringFormat fmtRight;
        fmtRight.SetAlignment(Gdiplus::StringAlignmentFar);
        fmtRight.SetLineAlignment(Gdiplus::StringAlignmentNear);

        Gdiplus::SolidBrush whiteBr(Gdiplus::Color(255, 252, 253, 255));

        const float anchorX = plotX + plotW - DPIScaleF(6.0f);
        const float anchorY = plotY + DPIScaleF(6.0f);
        const float lineHeight = DPIScaleF(16.0f);

        for (size_t i = 0; i < censoredLabels.size(); ++i) {
            const auto& entry = censoredLabels[i];
            float ly = anchorY + (float)i * lineHeight;
            Gdiplus::PointF pt(anchorX, ly);
            for (int dx = -1; dx <= 1; dx += 2) {
                for (int dy = -1; dy <= 1; dy += 2) {
                    g.DrawString(entry.text.c_str(), -1, &tickFont,
                                 Gdiplus::PointF(pt.X + (float)dx, pt.Y + (float)dy),
                                 &fmtRight, &whiteBr);
                }
            }
            Gdiplus::SolidBrush lblBrush(entry.color);
            g.DrawString(entry.text.c_str(), -1, &tickFont, pt, &fmtRight, &lblBrush);
        }
    }

    // 图例 (与 ECDF 共用样式)
    Gdiplus::Font legendFont(&fontFamily, DPIScaleF(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush blueBr(Gdiplus::Color(255, 65, 140, 240));
    Gdiplus::SolidBrush redBr (Gdiplus::Color(255, 240, 80, 80));

    const wchar_t* legAll  = L"综合 S 级 剩余期望";  // 综合 S 级 剩余期望
    const wchar_t* legUp   = L"当期限定 UP 剩余期望";
    const wchar_t* legThy  = L"理论值 (综合)";

    auto measureW = [&](const wchar_t* s) -> float {
        Gdiplus::RectF box;
        g.MeasureString(s, -1, &legendFont, Gdiplus::PointF(0, 0), &box);
        return box.Width;
    };
    const float swatchW    = DPIScaleF(14.0f);
    const float swatchGap  = DPIScaleF(6.0f);
    const float entryGap   = DPIScaleF(16.0f);
    const float legendY    = (float)rect.Y + DPIScaleF(12.0f);
    const float swatchYOff = DPIScaleF(8.0f);

    float wAll  = measureW(legAll);
    float wUp   = measureW(legUp);
    float wThy  = measureW(legThy);
    float xRight = (float)rect.X + (float)rect.Width - DPIScaleF(12.0f);

    float xThyText = xRight - wThy;
    float xThySw   = xThyText - swatchGap - swatchW;
    {
        Gdiplus::Pen dashPen(Gdiplus::Color(255, 130, 130, 130), DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(2.5f), DPIScaleF(2.0f) };
        dashPen.SetDashPattern(dash, 2);
        g.DrawLine(&dashPen, xThySw, legendY + swatchYOff,
                   xThySw + swatchW, legendY + swatchYOff);
    }
    g.DrawString(legThy, -1, &legendFont,
                 Gdiplus::PointF(xThyText, legendY), &textBrush);

    float xUpText = xThySw - entryGap - wUp;
    float xUpSw   = xUpText - swatchGap - swatchW;
    g.FillRectangle(&redBr, xUpSw, legendY + swatchYOff - DPIScaleF(1.5f),
                    swatchW, DPIScaleF(3.0f));
    g.DrawString(legUp, -1, &legendFont,
                 Gdiplus::PointF(xUpText, legendY), &textBrush);

    float xAllText = xUpSw - entryGap - wAll;
    float xAllSw   = xAllText - swatchGap - swatchW;
    g.FillRectangle(&blueBr, xAllSw, legendY + swatchYOff - DPIScaleF(1.5f),
                    swatchW, DPIScaleF(3.0f));
    g.DrawString(legAll, -1, &legendFont,
                 Gdiplus::PointF(xAllText, legendY), &textBrush);
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
        // 代理人池 ECDF: X 轴 180 (UP 大保底)
        DrawECDF (g, Gdiplus::Rect(DPIScale(20),  DPIScale(320), DPIScale(600), DPIScale(250)),
                  statsAgent.freq_all, statsAgent.freq_up,
                  statsAgent.count_all, statsAgent.count_up,
                  statsAgent.censored_pity_all, statsAgent.censored_pity_up,
                  g_cdf_agent, 92, g_cdf_agent_up, 182,
                  L"代理人池累积分布 (ECDF)",  // 代理人池累积分布 (ECDF)
                  180);
        // 代理人池 MRL
        DrawMRL  (g, Gdiplus::Rect(DPIScale(640), DPIScale(320), DPIScale(600), DPIScale(250)),
                  statsAgent.freq_all, statsAgent.freq_up,
                  statsAgent.count_all, statsAgent.count_up,
                  statsAgent.censored_pity_all, statsAgent.censored_pity_up,
                  g_cdf_agent, 92, g_cdf_agent_up, 182,
                  L"代理人池剩余抽数期望 (MRL)",  // 代理人池剩余抽数期望 (MRL)
                  180,
                  /*theory_all_cap=*/90, /*theory_up_cap=*/180);
        // 音擎池 ECDF: X 轴 160 (UP 大保底)
        DrawECDF (g, Gdiplus::Rect(DPIScale(20),  DPIScale(575), DPIScale(600), DPIScale(250)),
                  statsWEngine.freq_all, statsWEngine.freq_up,
                  statsWEngine.count_all, statsWEngine.count_up,
                  statsWEngine.censored_pity_all, statsWEngine.censored_pity_up,
                  g_cdf_wengine, 82, g_cdf_wengine_up, 162,
                  L"音擎池累积分布 (ECDF)",  // 音擎池累积分布 (ECDF)
                  160);
        // 音擎池 MRL
        DrawMRL  (g, Gdiplus::Rect(DPIScale(640), DPIScale(575), DPIScale(600), DPIScale(250)),
                  statsWEngine.freq_all, statsWEngine.freq_up,
                  statsWEngine.count_all, statsWEngine.count_up,
                  statsWEngine.censored_pity_all, statsWEngine.censored_pity_up,
                  g_cdf_wengine, 82, g_cdf_wengine_up, 162,
                  L"音擎池剩余抽数期望 (MRL)",  // 音擎池剩余抽数期望 (MRL)
                  160,
                  /*theory_all_cap=*/80, /*theory_up_cap=*/160);
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

        // 提示行
        HWND hL1 = CreateWindowW(L"STATIC",
            L"请确认上方常驻 S 级名单（开服至今较少变动）；不在常驻名单内的 S 级即视为当期 UP；拖入 uigf_zzz.json 开始分析。",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(15), DPIScale(1200), DPIScale(20), hwnd, NULL, NULL, NULL);

        // 第 1 行: 常驻 S 级代理人
        HWND hL_AgentStd = CreateWindowW(L"STATIC",
            L"常驻 S 级代理人:",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(45), DPIScale(125), DPIScale(20), hwnd, NULL, NULL, NULL);
        hAgentEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"猫又,「11号」,珂蕾妲,莱卡恩,格莉丝,丽娜",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            DPIScale(150), DPIScale(40), DPIScale(1090), DPIScale(26), hwnd, NULL, NULL, NULL);

        // 第 2 行: 常驻 S 级音擎
        HWND hL_WepStd = CreateWindowW(L"STATIC",
            L"常驻 S 级音擎:",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(75), DPIScale(125), DPIScale(20), hwnd, NULL, NULL, NULL);
        hWEngineEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"钢铁肉垫,硫磺石,燃狱齿轮,拘缚者,嵌合编译器,啜泣摇篮",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            DPIScale(150), DPIScale(70), DPIScale(1090), DPIScale(26), hwnd, NULL, NULL, NULL);

        // 输出区
        hOutEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"等待拖入文件...",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            DPIScale(20), DPIScale(105), DPIScale(1220), DPIScale(205), hwnd, NULL, NULL, NULL);

        DWORD tabStops[] = {50};
        SendMessage(hOutEdit, EM_SETTABSTOPS, 1, (LPARAM)tabStops);
        SendMessage(hOutEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)GetSysColor(COLOR_3DFACE));

        for (HWND h : {hL1, hL_AgentStd, hAgentEdit, hL_WepStd, hWEngineEdit, hOutEdit})
            SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);
        RebuildChartCache(hwnd);
        break;
    }
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        wchar_t filePath[MAX_PATH];
        DragQueryFileW(hDrop, 0, filePath, MAX_PATH);
        DragFinish(hDrop);
        ProcessFile_Submit(hwnd, filePath);
        break;
    }
    case WM_APP_PROCESS_DONE: {
        auto* out = (ProcessOutput*)lParam;
        ProcessFile_Consume(hwnd, out);
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
    HDC hdcScreen = GetDC(NULL);
    g_dpi = GetDeviceCaps(hdcScreen, LOGPIXELSX);
    ReleaseDC(NULL, hdcScreen);

    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);
    InitCDFTables();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"ZZZStatsClass";
    RegisterClassW(&wc);

    DWORD dwStyle = (WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX) | WS_CLIPCHILDREN;
    // 窗口高度 860: 提示行(15) + 2 个输入框(30*2=60) + 输出区(205) + 图表区(2*250=500) + 间距/边距 ≈ 860
    RECT rect = {0, 0, DPIScale(1280), DPIScale(860)};
    AdjustWindowRectEx(&rect, dwStyle, FALSE, 0);

    HWND hwnd = CreateWindowW(wc.lpszClassName,
        L"绝区零抽卡记录分析与可视化",  // 绝区零抽卡记录分析与可视化
        dwStyle, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
