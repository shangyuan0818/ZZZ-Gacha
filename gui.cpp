// ============================================================
// ZZZ Gacha Visualizer - Win32 + GDI+ + PMR / 预分桶 / AoS
// ------------------------------------------------------------
// 自 Endfield Gacha Visualizer (v0.1.3.3 加固版) 迁移到绝区零, 现对齐到 Endfield-Gacha v0.1.4:
//   五个分析池: 独家频段("2") / 独家重映频段("102") / 音擎频段("3") /
//               音擎回响频段("103") / 常驻频段("1"); 邦布频段("5") 不分析。
//   保底模型: 代理人/常驻 0.6%/74软/90硬, 音擎 1.0%/65软/80硬 (重映/回响同口径);
//   UP 大保底真实存在 ("歪了下次必中"), 水位与大保底状态跨期【永续】
//   (区别于终末地的每期重置) → 无卡池边界探测, 无 pool_map, 无 is_free。
//   重映/回响 (2.5 起) 每期首个 S 必为 UP (福利保底), 记录里却没有期次标识,
//   故这两个频段的 UP 侧只做描述统计, 不判 50/50、不做 K-S (见 Calculate)。
//   数组容量 200: 代理人 UP 大保底间隔上限 180 (90 歪 + 90 保底) + 富余。
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
#include <span>           // v0.1.3.3: 理论 CDF 表改用 std::span 传参
#include <cstdint>
#include <memory>      // std::make_unique_for_overwrite (C++20) —— worker 的 2MB PMR arena 用它在堆上不清零分配
#include <process.h>   // _beginthreadex / _endthreadex(调用 CRT 的线程应走这个而非裸 CreateThread)
#include "JsonScan.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")    // CreateWindowExW / SendMessageW / MessageBoxW / GetMessageW ...
#pragma comment(lib, "gdi32.lib")     // BitBlt / CreateCompatibleDC / SelectObject / CreateFontW ...
#pragma comment(lib, "shell32.lib")   // DragAcceptFiles / DragQueryFileW / DragFinish

// ---------------------------------------------------------
// [枚举降维]
// 注: 分析核心不消费 item_type 字段 —— 分桶按 gacha_type, 稀有度按
// rank_type, UP 判定按 name + time (item_type 仅存在于拉取/导出链路 main_zzz.cpp)。
// ---------------------------------------------------------
// 稀有度: 2=B, 3=A, 4=S
enum class RankType  : uint8_t { Unknown = 0, RankB = 2, RankA = 3, RankS = 4 };
// gacha_type 数字字符串 = 导出时查询的 real_gacha_type。UIGF v4.2 的 nap 枚举只列了
// 1/2/3/5; 102/103 是 2.5 新增的频段, 本工具与 HoYo.Gacha 一样原样写入。
enum class GachaType : uint8_t {
    Unknown       = 0,
    Standard      = 1,     // "1"   常驻频段(热门卡司) — 分析对象 (仅综合, 无 UP)
    AgentUP       = 2,     // "2"   独家频段(代理人 UP) — 分析对象
    WEngineUP     = 3,     // "3"   音擎频段(武器 UP)   — 分析对象
    Bangboo       = 5,     // "5"   邦布频段 — 不分析
    Rescreening   = 102,   // "102" 独家重映频段(代理人复刻) — 分析对象
    Reverberation = 103    // "103" 音擎回响频段(音擎复刻)   — 分析对象
};

inline RankType ParseRankType(std::string_view sv) {
    if (sv == "4") return RankType::RankS;
    if (sv == "3") return RankType::RankA;
    if (sv == "2") return RankType::RankB;
    return RankType::Unknown;
}

inline GachaType ParseGachaType(std::string_view sv) {
    // ZZZ 的 gacha_type 是数字字符串, 严格相等匹配即可;
    // 不做 Contains 匹配以免 "12" 之类的值误配 (API 实际不返回这种值, 纯防御)
    if (sv == "1")   return GachaType::Standard;
    if (sv == "2")   return GachaType::AgentUP;
    if (sv == "3")   return GachaType::WEngineUP;
    if (sv == "5")   return GachaType::Bangboo;
    if (sv == "102") return GachaType::Rescreening;
    if (sv == "103") return GachaType::Reverberation;
    return GachaType::Unknown;
}

// ---------------------------------------------------------
// [共享 JSON 扫描器 - 按结构读取 UIGF 存档]
// ---------------------------------------------------------
inline std::string_view ExtractJsonValue(std::string_view source, std::string_view key, bool isString) {
    return zzzjson::ExtractValue(source, key, isString);
}

template<typename Callback>
[[nodiscard]] zzzjson::ArrayScan ReadUigfPullList(std::string_view document, Callback&& cb) {
    // 本层定位根.nap[0].list, 避免误读多游戏 UIGF 文件里 hk4e / hkrpg 段的 list。
    const auto game = zzzjson::FindMember(document, "nap");
    if (game.kind == zzzjson::ValueKind::Array) {
        const auto account = zzzjson::FirstElement(game.text);
        if (account.kind != zzzjson::ValueKind::Object) return zzzjson::ArrayScan::Malformed;
        const auto list = zzzjson::FindMember(account.text, "list");
        if (list.kind != zzzjson::ValueKind::Array) return zzzjson::ArrayScan::Malformed;
        return zzzjson::ForEachObjectIn(list.text, std::forward<Callback>(cb));
    }
    // 键存在但类型异常/结构损坏时中止, 不能再从别的游戏段里找一份替代数组。
    if (game.kind != zzzjson::ValueKind::None || game.malformed) return zzzjson::ArrayScan::Malformed;
    // 完全没有 nap 段的旧格式/第三方文件保留原来的 list 回退路径。
    return zzzjson::ForEachObjectByKey(document, "list", std::forward<Callback>(cb));
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

// 注意:名单文本中故意只识别 ASCII ',' 作为条目分隔符。
// 全角逗号 '，'(U+FF0C) 不视为分隔符 —— 因为合法的物品名本身可能含全角符号
// (ZZZ 里「11号」用了全角直角引号「」, 未来也可能出现含全角逗号的名字)。
// 把全角逗号当分隔符会导致这些条目被切碎, UP 识别失效。
//
// ZZZ 的 UP 判定是二元化的: "S 级不在非 UP 名单 = 当期 UP", 不需要终末地那种
// "池名→UP角色" 映射 (ParsePoolMapUtf8* 已随之移除); 宽字符版
// ParseCommaSeparatedUtf8 在 Endfield 版中已是死代码 (主线程只做
// GetWindowText→WideToUtf8, 解析统一在 worker 用 FromUtf8 版), 一并移除。
//
// 3.1 起独家 / 音擎频段的歪池加入「定制调频」: 凯撒 / 柳 / 朱鸢 与 奔袭獠牙 / 时流贤者 /
// 防暴者Ⅵ型 从此也会以非 UP 身份出现, 而它们此前都当过 UP —— 同一个名字在不同时段
// 身份不同, 名单因此要能写时间:
//   名字               全时段都不是 UP
//   名字@起            该时刻起不是 UP (之前抽到的仍算 UP)
//   名字@起~止         只在该时段内不是 UP; 起或止可省略 (以后复刻当 UP 时用来挖掉那一段)
// 同一名字可写多条, 命中任意一条即为非 UP。时间写 YYYY-MM-DD[ HH:MM[:SS]], 与记录里的
// time 一样是区服服务器时间; '@' / '~' 也接受全角的 '＠' / '～' (中文输入法下顺手)。
inline std::string TrimUtf8(std::string_view sv) {
    size_t b = sv.find_first_not_of(" \t\r\n");
    if (b == std::string_view::npos) return {};
    size_t e = sv.find_last_not_of(" \t\r\n");
    return std::string(sv.substr(b, e - b + 1));
}

struct NonUpWindow {
    std::string from, to;   // "YYYY-MM-DD HH:MM:SS"; 空 = 该端不设限
};

struct NonUpList {
    std::unordered_map<std::string, std::vector<NonUpWindow>, StringHash, std::equal_to<>> entries;

    // time 是记录的 "YYYY-MM-DD HH:MM:SS": 定长、零填充, 字典序即时间序。
    // 记录缺 time 时只认全时段条目 —— 无从判断它落在哪个时段。
    bool Contains(std::string_view name, std::string_view time) const {
        const auto it = entries.find(name);
        if (it == entries.end()) return false;
        for (const NonUpWindow& w : it->second) {
            if (!w.from.empty() && (time.empty() || time < std::string_view(w.from))) continue;
            if (!w.to.empty()   && (time.empty() || time > std::string_view(w.to)))   continue;
            return true;
        }
        return false;
    }
};

// 把时间写法补齐成 "YYYY-MM-DD HH:MM:SS"。作为【止】时补到当天 / 当分钟的最后一秒,
// 这样 "@~2026-12-01" 含 12 月 1 日全天。空串 = 不设限。
inline std::string NormalizeNonUpTime(std::string_view text, bool isEnd) {
    std::string t = TrimUtf8(text);
    if (t.size() == 10)      t += isEnd ? " 23:59:59" : " 00:00:00";
    else if (t.size() == 16) t += isEnd ? ":59" : ":00";
    return t;
}

inline NonUpList ParseNonUpListUtf8(std::string_view text) {
    constexpr std::string_view kFullAt = "＠", kFullTilde = "～";
    NonUpList result;
    auto addEntry = [&](std::string_view raw) {
        const std::string entry = TrimUtf8(raw);
        const std::string_view ev = entry;
        size_t at = ev.find('@'), atLen = 1;
        if (at == std::string_view::npos) { at = ev.find(kFullAt); atLen = kFullAt.size(); }
        std::string name = TrimUtf8(ev.substr(0, at));
        if (name.empty()) return;
        NonUpWindow w;
        if (at != std::string_view::npos) {
            const std::string_view spec = ev.substr(at + atLen);
            size_t tilde = spec.find('~'), tildeLen = 1;
            if (tilde == std::string_view::npos) { tilde = spec.find(kFullTilde); tildeLen = kFullTilde.size(); }
            w.from = NormalizeNonUpTime(spec.substr(0, tilde), false);
            if (tilde != std::string_view::npos) w.to = NormalizeNonUpTime(spec.substr(tilde + tildeLen), true);
        }
        result.entries[std::move(name)].push_back(std::move(w));
    };
    size_t start = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ',') {
            addEntry(text.substr(start, i - start));
            start = i + 1;
        }
    }
    return result;
}

// ---------------------------------------------------------
// [SoA 分桶 - 五个频段独立桶,Calculate 不再 filter 全量]
// 统计热路径 (非 S 级的 [[likely]] 分支) 只访问紧凑的 rank_types 标量数组;
// names / times 仅在少量 S 级记录做 UP 判定时才访问 (会触达 mmap 字节)。
// ZZZ 不存在终末地的 poolNames(期边界重置)/is_free(赠送十连)/starts_new_banner:
// 水位与大保底状态跨期永续, 全部记录按 id 升序连续推进。
// ---------------------------------------------------------
struct PullBucket {
    std::pmr::vector<RankType>         rank_types;
    std::pmr::vector<std::string_view> names;
    std::pmr::vector<std::string_view> times;   // 非 UP 名单的时间窗按它判断

    explicit PullBucket(std::pmr::polymorphic_allocator<std::byte> alloc)
        : rank_types(alloc), names(alloc), times(alloc) {}

    void reserve(size_t cap) {
        rank_types.reserve(cap); names.reserve(cap); times.reserve(cap);
    }
    void push_back(RankType rt, std::string_view name, std::string_view time) {
        rank_types.push_back(rt); names.push_back(name); times.push_back(time);
    }
    size_t size() const { return rank_types.size(); }
};

// StatsAccumulator: Calculate() 内的单线程累加器 (局部变量 acc), 不存在多核并发写,
// 不需要 cache-line 对齐 —— 故不加 alignas (旧版的 alignas(128) 在单线程下是无操作,
// 留着只会误导维护者以为这里有并发)。将来若改成多线程分片归约、每线程持有相邻
// accumulator, 再按实际 cache-line 布局补 padding 防 false sharing 即可。
struct StatsAccumulator {
    std::array<int, 200> freq_all{};   // 容量 200: 代理人 UP 间隔上限 180 + 富余
    std::array<int, 200> freq_up{};
    long long sum_all = 0, sum_sq_all = 0, sum_up = 0, sum_sq_up = 0, sum_win = 0;
    int count_all = 0, count_up = 0, count_win = 0;
    int max_pity_all = 0, max_pity_up = 0;
    int win_5050 = 0, lose_5050 = 0;
    // 右删失:循环结束时仍在累积、尚未结算的当前保底计数
    // 生存分析里这些样本应参与分母(risk set),但不参与分子(event)
    int censored_pity_all = 0;
    int censored_pity_up  = 0;
};

struct KSLocation {
    int x = 0;
    double empirical = 0.0, theory = 0.0;
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
    KSLocation ks_location_all{}, ks_location_up{};
    bool ks_is_normal = true, ks_is_normal_up = true;
    // 重映 / 回响频段: UP 侧只作描述统计, 不判 50/50、不做 K-S (见 Calculate);
    // DrawECDF 据此加一行说明并跳过 UP 的 K-S 标记 (UP 理论曲线由调用方传空表去掉)。
    bool up_descriptive = false;
    // 右删失(用于显示"当前已垫 N 抽")
    int censored_pity_all = 0;
    int censored_pity_up  = 0;
};

StatsResult statsAgent, statsRescreening, statsWEngine, statsReverberation, statsStandard;
HWND hOutEdit, hAgentEdit, hWEngineEdit;
// 三个说明标签也是全局 —— 窗口可缩放后每次 WM_SIZE / 滚动都要重新摆位
HWND hHintLabel = NULL, hAgentLabel = NULL, hWEngineLabel = NULL;
static int g_scrollY  = 0;   // 当前垂直滚动量 (物理像素)
static int g_contentH = 0;   // 最近一次布局算出的内容总高 (物理像素)
static HBITMAP g_hChartBmp = NULL;
int g_dpi = 96;
int   DPIScale (int value)   { return MulDiv(value, g_dpi, 96); }
float DPIScaleF(float value) { return value * (g_dpi / 96.0f); }

// -------------------------------------------------------
// CDF 表 & KS 检验
// -------------------------------------------------------
// 米哈游模型 (官方概率公示 + 社区实测软保底拐点):
//
// 代理人池(独家频段) / 常驻频段(热门卡司) — 同一 S 级出货分布:
//   基础概率 0.6%, 90 抽硬保底, 综合概率 1.6%
//   软保底拐点: 第74抽起, 每抽 +6%
//     k=1..73:  hazard = 0.006
//     k=74..89: hazard = 0.006 + (k-73) * 0.06 = 0.066, 0.126, ..., 0.966
//     k=90:     hazard = 1.0 (硬保底)
//   验证: 综合期望 ≈ 62.30 抽, 综合概率 = 1/62.30 ≈ 1.605% ✓
//
// 音擎池(音擎频段):
//   基础概率 1.0%, 80 抽硬保底, 综合概率 2.0%
//   软保底拐点: 第65抽起, 每抽 +7%
//     k=1..64:  hazard = 0.01
//     k=65..79: hazard = 0.01 + (k-64) * 0.07 = 0.08, 0.15, ..., 0.99
//     k=80:     hazard = 1.0 (硬保底)
//   验证: 综合期望 ≈ 49.71 抽, 综合概率 = 1/49.71 ≈ 2.012% ✓
//
// UP CDF: 双状态前向迭代 (Dn 没歪 / Dl 已歪):
//   "歪了下次必中"的大保底真实存在 (区别于终末地), 且状态跨期永续。
//   转移: 没歪状态出货 → winRate 毕业, (1-winRate) 转入 Dl[0];
//         已歪状态出货 → 100% 毕业 (大保底)。
//   代理人: 50% 不歪, 间隔上限 180 (89 垫 + 1 歪 + 89 垫 + 1 保底), E[首UP] ≈ 93.45
//   音擎:   75% 不歪, 间隔上限 160, E[首UP] ≈ 62.13
// 常驻频段没有 UP, 综合 K-S 对照复用 g_cdf_agent (同分布)。
// 独家重映 / 音擎回响 (2.5 起) 的常规保底按 wiki 口径与独家 / 音擎相同 (官方抽卡详情里
// 没有单独的数字), 综合 K-S 分别复用 g_cdf_agent / g_cdf_wengine; 它们每期首个 S 必为
// UP (福利保底), UP 分布与上面两条 UP CDF 不是同一个东西, 不做对照。
static double g_cdf_agent[92]       = {};  // x=0..90,  代理人/常驻池综合 (末位哨兵 91)
static double g_cdf_wengine[82]     = {};  // x=0..80,  音擎池综合
static double g_cdf_agent_up[182]   = {};  // x=0..180, 代理人池 UP
static double g_cdf_wengine_up[162] = {};  // x=0..160, 音擎池 UP
static bool   g_cdf_init = false;

static double HazardAgent(int k) {
    if (k <= 73)      return 0.006;
    else if (k <= 89) return (std::min)(1.0, 0.006 + (k - 73) * 0.06);
    else              return 1.0;
}
static double HazardWEngine(int k) {
    if (k <= 64)      return 0.01;
    else if (k <= 79) return (std::min)(1.0, 0.01 + (k - 64) * 0.07);
    else              return 1.0;
}

void InitCDFTables() {
    // 幂等保护: 仅 WinMain 启动时 (任何 worker 创建之前) 单线程调用一次,
    // bool 标志即可, 不需要 call_once
    if (g_cdf_init) return;

    // ---- 代理人/常驻池综合 CDF ----
    {
        double surv = 1.0;
        for (int i = 1; i <= 90; ++i) {
            double p = HazardAgent(i);
            g_cdf_agent[i] = g_cdf_agent[i - 1] + surv * p;
            surv *= (1.0 - p);
        }
        g_cdf_agent[91] = 1.0;
    }

    // ---- 音擎池综合 CDF ----
    {
        double surv = 1.0;
        for (int i = 1; i <= 80; ++i) {
            double p = HazardWEngine(i);
            g_cdf_wengine[i] = g_cdf_wengine[i - 1] + surv * p;
            surv *= (1.0 - p);
        }
        g_cdf_wengine[81] = 1.0;
    }

    // ---- UP CDF 双状态前向迭代 (代理人 50/50 上限 180; 音擎 75/25 上限 160) ----
    // 状态空间: Dn[s] (没歪过, 水位 s) + Dl[s] (已歪, 水位 s)
    auto buildUp = [](double* cdf, int hard_cap, int full_cap, double win_rate,
                      double (*hazard)(int)) {
        std::vector<double> Dn(hard_cap + 1, 0.0); Dn[0] = 1.0;
        std::vector<double> Dl(hard_cap + 1, 0.0);
        double cum = 0.0;
        for (int n = 1; n <= full_cap; ++n) {
            std::vector<double> newDn(hard_cap + 1, 0.0), newDl(hard_cap + 1, 0.0);
            for (int sidx = 0; sidx < hard_cap; ++sidx) {
                double h = hazard(sidx + 1);
                if (Dn[sidx] > 0) {
                    double hit = Dn[sidx] * h;
                    cum += hit * win_rate;                    // 不歪, 毕业
                    newDl[0] += hit * (1.0 - win_rate);       // 歪 → 已歪状态, 水位归 0
                    newDn[sidx + 1] += Dn[sidx] * (1.0 - h);  // 不出货, 水位 +1
                }
                if (Dl[sidx] > 0) {
                    double hit = Dl[sidx] * h;
                    cum += hit;                               // 大保底, 必毕业
                    newDl[sidx + 1] += Dl[sidx] * (1.0 - h);
                }
            }
            cdf[n] = (std::min)(1.0, cum);
            Dn = std::move(newDn); Dl = std::move(newDl);
        }
        cdf[full_cap + 1] = 1.0;   // 哨兵
    };
    buildUp(g_cdf_agent_up,   90, 180, 0.50, &HazardAgent);
    buildUp(g_cdf_wengine_up, 80, 160, 0.75, &HazardWEngine);

    g_cdf_init = true;  // 末尾置位,确保所有读者看到完整表
}

// 统计与绘图共用有效末端: 饱和到 1, 或未填充哨兵造成单调性破坏前的最后一格。
// ZZZ 各表都在硬保底 / 大保底处饱和到 1.0, 其后的哨兵格不是有效概率。
int FindCDFLastValid(std::span<const double> cdf_table) {
    if (cdf_table.empty()) return 0;
    constexpr double EPS_SAT = 1e-6;
    for (int k = 1; k < (int)cdf_table.size(); ++k) {
        if (cdf_table[k] >= 1.0 - EPS_SAT) return k;
        if (cdf_table[k] + EPS_SAT < cdf_table[k - 1]) return k - 1;
    }
    return (int)cdf_table.size() - 1;
}

// 修复:离散阶梯 CDF 的 K-S 统计量需严格对齐两条阶梯// 修复:离散阶梯 CDF 的 K-S 统计量需严格对齐两条阶梯
// 在 x 处,两条阶梯的"底":F_n(cum before x),F_theory(x-1)
// 在 x 处,两条阶梯的"顶":F_n(cum after x),F_theory(x)
// 原版用 fn_before 减 cdf_table[x] —— 拿经验阶梯底对理论阶梯顶,
// 人为引入 h_x 的单点跳跃(软保底段可高达 5%+),造成巨大伪偏差
double ComputeKS(const std::array<int, 200>& freq, int max_pity, int n,
                 std::span<const double> cdf_table, KSLocation* max_location = nullptr) {
    if (max_location) *max_location = {};
    if (n <= 0 || cdf_table.empty()) return 0.0;
    // 防御性 clamp: freq 数组容量 200,max_pity 必须 < 200 否则越界读
    if (max_pity > 199) max_pity = 199;
    // 找到 CDF 表的"有效末端" last_valid (饱和到 1 或单调性破坏前的最后一格).
    // 越过 last_valid 后, 用 cdf[last_valid] 而非 1.0 作 fallback。ZZZ 各池
    // CDF 都自然饱和到 1.0 (大保底封顶), 这里主要是定位饱和点, 避免在硬保底
    // 之后的哨兵区做无意义比较; 单调性分支是纯防御。
    const int last_valid = FindCDFLastValid(cdf_table);
    auto lookup_cdf = [&](int idx) -> double {
        if (idx < 0) return 0.0;
        if (idx > last_valid) return cdf_table[last_valid];
        return cdf_table[idx];
    };
    double max_d = 0.0;
    int cum_count = 0;
    for (int x = 1; x <= max_pity; ++x) {
        double fn_before    = (double)cum_count / n;
        double cdf_before_x = lookup_cdf(x - 1);

        cum_count += freq[x];

        double fn_after    = (double)cum_count / n;
        double cdf_after_x = lookup_cdf(x);

        double d1 = std::abs(fn_before - cdf_before_x);
        double d2 = std::abs(fn_after  - cdf_after_x);
        if (d1 > max_d) {
            max_d = d1;
            if (max_location) *max_location = {x - 1, fn_before, cdf_before_x};
        }
        if (d2 > max_d) {
            max_d = d2;
            if (max_location) *max_location = {x, fn_after, cdf_after_x};
        }
    }
    return max_d;
}

// -------------------------------------------------------
// 统计工具:t 分布 95% 双侧临界值 (α/2 = 0.025)
// -------------------------------------------------------
// 当样本量较小时(N < 30),标准正态 z=1.96 的 CI 会严重低估真实不确定性
// (因为 t 分布尾部更厚)。严格的样本 CI 应该用 t_{α/2, N-1}
//
// 实现策略:
//   df = 1, 2, 3, 4:查表(Hill 近似在低 df 误差较大,最高 0.75%)
//   df ≥ 5:用 Hill(1970) 四阶渐近展开(误差 < 0.02%)
//   df → ∞ 时收敛到 z = 1.959964
inline double TCritical95(int df) {
    // α=0.025 双侧 95% CI
    if (df <= 0) return 1.959963984540054;  // 保护
    // 低自由度查表(值来自 scipy.stats.t.ppf(0.975, df))
    static constexpr double kTable[] = {
        0.0,        // df=0 占位
        12.706205,  // df=1
        4.302653,   // df=2
        3.182446,   // df=3
        2.776445,   // df=4
    };
    if (df <= 4) return kTable[df];

    // Hill 1970 四阶展开
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

// -------------------------------------------------------
// 无偏样本方差(贝塞尔校正):s² = [Σx² - (Σx)²/N] / (N-1)
// 注意 N=1 时样本方差未定义(除零),返回 0
// -------------------------------------------------------
inline double SampleVariance(long long sum, long long sum_sq, int n) {
    if (n <= 1) return 0.0;
    // 数值稳定式:避免先算 mean 再做 E[X²]-E[X]² 的灾难性消去
    double numerator = (double)sum_sq - (double)sum * sum / (double)n;
    if (numerator < 0.0) numerator = 0.0;  // 浮点误差保护
    return numerator / (double)(n - 1);
}

// -------------------------------------------------------
// 统计核心 - bucket 已只含目标池子,无需 filter
//
// 五个池子的语义 (独家/音擎/常驻 三池与 macOS/iOS AnalyzerWrapper.mm 1:1 对齐):
//   - 独家频段 (Agent):          50% 不歪, 90 硬保底 / 74 起软保底, 有大保底
//   - 音擎频段 (WEngine):        75% 不歪, 80 硬保底 / 65 起软保底, 有大保底
//   - 独家重映 (Rescreening):    常规保底同独家频段, 每期首个 S 必为 UP (福利保底)
//   - 音擎回响 (Reverberation):  常规保底同音擎频段, 每期首个 S 必为 UP (福利保底)
//   - 常驻频段 (Standard):       无 UP 概念, 90 硬保底 / 74 起软保底, 仅综合统计
//
// 关键机制 (区别于终末地):
//   - 大保底真实存在: 歪了之后下一个 S 级【必定】是当期 UP
//     → had_non_up 状态机跟踪小/大保底状态
//   - 水位与大保底状态【永续】: 跨卡池期数保留, 全部记录按 id 升序连续推进,
//     不做任何期边界重置。删失只发生在记录末尾 (当前在途水位)。
//     重映 / 回响的水位同样在各自频段内跨期累计, 与独家 / 音擎互不相干。
//   - UP 判定二元化: 出的 S 级不在非 UP 名单里就是当期 UP
//     (UP 池里的 S 级只有"当期 UP + 歪池 S"两种来源); 名单条目可带时间窗,
//     按记录的 time 判断 (见 NonUpList)。
//
// win_5050 / lose_5050 / avg_win:
//   - had_non_up=false (小保底/50-50 阶段) 时出 UP 计入 "win";
//     had_non_up=true (大保底) 时出 UP 是必然事件, 不算"赢"。
//   - 出非 UP S 级: 仅当上一个 S 是 UP (新一轮判定失败) 才计入 lose;
//     had_non_up=true 时不重复计数 (违反规则的异常数据防御)。
//   - avg_win (count_win/sum_win): "小保底阶段成功毕业"的样本期望,
//     代理人/音擎两池都有大保底, 语义一致, 都累计。
//   - 常驻频段无 UP: 全部 UP 相关字段保持 0 / -1 sentinel。
//   - 重映 / 回响【不计】胜负与 avg_win, 也不做 UP K-S: 福利保底让每期首个 S 必为 UP,
//     而记录里没有期次标识 (gacha_id 恒为 "0"), 分不出哪个 UP 是福利、哪个是掷硬币
//     赢的。按时间间隔猜期次边界是无法用现有数据验证的启发式, 不做。若将来要算
//     福利保底感知的歪率, 前提是维护一张期次表 (各期起止时间)。
//     UP 侧仍给出 UP 个数、间隔均值、水位与经验曲线, 作为描述统计。
// -------------------------------------------------------
enum class PoolKind : uint8_t { Agent, WEngine, Standard, Rescreening, Reverberation };

StatsResult Calculate(const PullBucket& bucket, PoolKind kind, const NonUpList& non_up) {
    StatsAccumulator acc;
    const bool hasUp   = (kind != PoolKind::Standard);
    const bool judgeUp = (kind == PoolKind::Agent || kind == PoolKind::WEngine);
    const bool wengine = (kind == PoolKind::WEngine || kind == PoolKind::Reverberation);
    int current_pity = 0, pity_since_last_up = 0;
    bool had_non_up = false;  // 上一个 S 是否歪了 (小/大保底状态; 跨期永续)

    const size_t total = bucket.size();
    for (size_t i = 0; i < total; ++i) {
        ++current_pity;
        ++pity_since_last_up;

        // 非 S 级:likely 分支 (UP 池里也会出 A 级音擎/邦布, 照常推进保底进度)
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

        if (hasUp) {
            // UP 判定: 此刻不在非 UP 名单里就是当期 UP (二元化, 名单条目可带时间窗)
            bool isUP = !non_up.Contains(bucket.names[i], bucket.times[i]);
            if (isUP) {
                const int slot_up = pity_since_last_up;
                if (slot_up < 200) acc.freq_up[slot_up]++;
                if (slot_up > acc.max_pity_up) acc.max_pity_up = slot_up;
                acc.count_up++;
                acc.sum_up    += slot_up;
                acc.sum_sq_up += (long long)slot_up * slot_up;

                if (judgeUp && !had_non_up) {
                    // 小保底阶段掷中 UP = 真实的"赢" (大保底必中不计入)
                    acc.win_5050++;
                    acc.count_win++;
                    acc.sum_win += slot_all;
                }
                had_non_up = false;
                pity_since_last_up = 0;
            } else {
                // 歪了: 仅小保底阶段计 lose (大保底阶段出非 UP 违反规则, 防御性不计)
                if (judgeUp && !had_non_up) acc.lose_5050++;
                had_non_up = true;
            }
        }
        current_pity = 0;
    }

    // 右删失:遍历结束时仍有未结算的水位 → 删失样本
    // (生存分析: 进分母 risk set, 不进分子 event)。
    // ZZZ 水位永续 → 全部历史只有这【一个】末尾在途水位是删失样本。
    acc.censored_pity_all = current_pity;
    acc.censored_pity_up  = hasUp ? pity_since_last_up : 0;

    // 防御性 clamp:即使数据异常导致 max_pity / censored_pity > 199,
    // 后续 ComputeKS 与 hazard 循环的索引访问也必须安全
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
    s.up_descriptive    = hasUp && !judgeUp;

    if (acc.count_all > 0) {
        s.avg_all = (double)acc.sum_all / acc.count_all;
        // 贝塞尔校正的无偏样本方差; N=1 时 SampleVariance 返回 0(CI 也自然为 0)
        double var = SampleVariance(acc.sum_all, acc.sum_sq_all, acc.count_all);
        double std_all = std::sqrt(var);
        s.cv_all = (s.avg_all > 0) ? std_all / s.avg_all : 0;
        // CI 使用 t 分布临界值(自由度 N-1),小样本下比 z=1.96 更保守正确
        double t_crit = TCritical95(acc.count_all - 1);
        s.ci_all_err = t_crit * std_all / std::sqrt((double)acc.count_all);

        // K-S 检验:代理人/重映/常驻池用 g_cdf_agent (同分布),音擎/回响池用 g_cdf_wengine
        const std::span<const double> cdf_tbl = wengine
            ? std::span<const double>(g_cdf_wengine)   // 82
            : std::span<const double>(g_cdf_agent);    // 92
        s.ks_d_all = ComputeKS(acc.freq_all, acc.max_pity_all, acc.count_all,
                               cdf_tbl, &s.ks_location_all);
        s.ks_is_normal = (s.ks_d_all <= (1.36 / std::sqrt((double)acc.count_all)));
    }

    // Kaplan-Meier 式经验风险函数 - 综合 S 级:
    //   risk set 初值 = 全部观测样本(已毕业 + 删失)
    //   到 x 抽时 hazard[x] = freq[x] / survivors
    //   survivors 每步先减去事件(freq[x]),再减去在 x 发生的删失
    // 即使 count_all=0 也要处理:用户可能从未出 S 级但已垫 N 抽(极少见但有效)
    if (acc.count_all > 0 || acc.censored_pity_all > 0) {
        int survivors = acc.count_all + (acc.censored_pity_all > 0 ? 1 : 0);
        int max_reach_all = (std::max)(acc.max_pity_all, acc.censored_pity_all);
        if (max_reach_all > 199) max_reach_all = 199;  // 防御性 clamp
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

        // UP K-S 检验: 用双状态前向迭代得到的 g_cdf_*_up。
        // ZZZ 全部是单抽粒度判定, 无终末地武器池那种"申领聚合"需求,
        // 经验 freq_up 直接与理论 CDF 逐抽比较。
        // 重映 / 回响不做: 福利保底让 UP 间隔不再服从这两条 CDF (见函数头说明)。
        if (judgeUp) {
            const std::span<const double> cdf_up_tbl = wengine
                ? std::span<const double>(g_cdf_wengine_up)   // 162
                : std::span<const double>(g_cdf_agent_up);    // 182
            s.ks_d_up = ComputeKS(acc.freq_up, acc.max_pity_up, acc.count_up,
                                  cdf_up_tbl, &s.ks_location_up);
            s.ks_is_normal_up = (s.ks_d_up <= (1.36 / std::sqrt((double)acc.count_up)));
        }
    }

    // UP hazard 同理
    if (acc.count_up > 0 || acc.censored_pity_up > 0) {
        int survivors = acc.count_up + (acc.censored_pity_up > 0 ? 1 : 0);
        int max_reach_up = (std::max)(acc.max_pity_up, acc.censored_pity_up);
        if (max_reach_up > 199) max_reach_up = 199;  // 防御性 clamp
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
// ---------------------------------------------------------// ---------------------------------------------------------
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
// 安全读取动态长度的 Edit 控件文本
// 原版固定 wchar_t[4096] 在用户粘贴超长 UP 映射文本时会被 GetWindowTextW 截断,
// 下游解析看到的是不完整数据 → 丢失记录。
// 先 GetWindowTextLengthW 查长度再按需分配,彻底消除截断风险
inline std::wstring GetDynamicWindowText(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    // v0.1.3.3: GetWindowTextLengthW 对 RichEdit 文档明示"可能大于实际长度"(估计值)。
    // 旧写法忽略 GetWindowTextW 的实际拷贝数, 高估时 wstring 尾部残留 L'\0' 填充;
    // 这些 NUL 经 WideToUtf8 原样转换, 粘在名单最后一个 token 上 (TrimUtf8 只剥空白
    // 不剥 '\0'), 导致最后一个常驻名 / 最后一条 UP 映射永远匹配不上 JSON 里的名字,
    // 统计被静默污染。修复: 按实际拷贝数 resize。
    std::wstring buf((size_t)len + 1, L'\0');
    int copied = GetWindowTextW(hwnd, buf.data(), len + 1);
    buf.resize(copied > 0 ? (size_t)copied : 0);
    return buf;
}

// ---------------------------------------------------------
// [文件处理 - 工作线程化]
//
// 原版 WM_DROPFILES 同步调 ProcessFile + RebuildChartCache,期间窗口消息
// 循环阻塞,用户无法移动窗口/输入/最小化。重构后:
//   1) 主线程做 I/O 准备(读 GUI 文本框 + 把文件内容拷到 std::string)
//   2) Worker 线程做纯 CPU 计算(JSON 解析 + Calculate),结果写入 heap 上
//      的 ProcessOutput 对象
//   3) Worker 用 PostMessage(WM_APP_PROCESS_DONE) 把结果指针回投到主线程
//   4) 主线程在该消息处理中更新五个频段的全局 stats* + UI,然后 delete output
//
// 注意:
//   - GDI / SetWindowTextW 都不是 thread-safe,只能在主线程调
//   - stats* 是全局,WM_PAINT 通过 g_hChartBmp 间接读它们,
//     但 g_hChartBmp 由 RebuildChartCache 重建,所以只要 RebuildChartCache
//     和 stats 写入都在主线程串行,就不需要锁
//   - g_processing 标志防止 worker 跑时重复触发(双开 worker)
//   - 线程句柄保留在 g_hWorker (主线程独占): 下次 Submit 开头与 WM_DESTROY 时
//     join + CloseHandle, 保证进程退出前 worker 已完整走完 CRT 尾声 (见 WM_DESTROY)
// ---------------------------------------------------------

#define WM_APP_PROCESS_DONE  (WM_APP + 1)

// 前向声明:RebuildChartCache 定义在 DrawECDF/DrawMRL 之后,但 ProcessFile_Consume
// 需要在文件中段调用它。
void RebuildChartCache(HWND hwnd);

// 跨线程载荷:主线程构造,worker 填充结果,主线程消费后 delete
struct ProcessOutput {
    HWND        hwnd_main = NULL;  // 主窗口,worker 用 PostMessage 回投到这里

    // === 主线程预填(由 ProcessFile_Submit 设置) ===
    // 文件 buffer 用 mmap 直读,零拷贝(与原版 ProcessFile + macOS Analyzer 对齐)。
    // 三个 handle 必须存活到 Consume 阶段才能 unmap/close,因为 ExportRecord
    // 内的 string_view 都指向 mmap 区域。
    HANDLE      hFile = INVALID_HANDLE_VALUE;
    HANDLE      hMap  = NULL;
    const void* viewPtr = nullptr;
    size_t      fileSize = 0;   // v0.1.3.2: DWORD → size_t, 配合 GetFileSizeEx (不再 32 位截断)

    std::string utf8_agents;    // 来自 hAgentEdit (GUI 控件文本必须主线程 GetWindowText)
    std::string utf8_wengines;  // 来自 hWEngineEdit

    // === worker 填充 ===
    bool        ok = false;
    StatsResult statsAgent;
    StatsResult statsRescreening;
    StatsResult statsWEngine;
    StatsResult statsReverberation;
    StatsResult statsStandard;
    std::wstring outMsg;
    std::wstring errMsg;

    ~ProcessOutput() {
        // 主线程消费后调 delete 时统一清理 mmap 资源
        if (viewPtr) UnmapViewOfFile(viewPtr);
        if (hMap)    CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    }
};

// 用全局原子防双开;Win32 上 LONG volatile + InterlockedExchange 等价于 atomic_flag
static volatile LONG g_processing = 0;

// worker 线程句柄, 仅主线程读写。生命周期: Submit 创建 → (a) 下次 Submit 开头
// join+close (此刻上一个 worker 早已投递结果/自清理, 最多剩微秒级 CRT 尾声), 或
// (b) WM_DESTROY join+close (退出前兜底)。不在 Consume 里关闭: 投递结果 ≠ 线程已
// 退出, 提前关闭会失去 join 能力, 留下 "ExitProcess 终止恰好持有 CRT 堆锁的尾声
// 线程 → 退出挂死" 的风险窗口。
static HANDLE g_hWorker = NULL;

// Worker 线程入口:纯 CPU 工作,不碰任何 GUI。
// 用 _beginthreadex 启动 (见 ProcessFile_Submit), 故签名是 unsigned __stdcall(void*) ——
// 这样 worker 里用到的 CRT (std::string/unordered_map/swprintf/std::ranges::sort/异常等)
// 的 per-thread 状态能被正确初始化与清理; 裸 CreateThread 跑 CRT 在极端低内存下有终止
// 进程的风险 (见 ProcessFile_Submit 的说明)。
unsigned __stdcall ProcessFile_Worker(void* arg) {
    auto* out = (ProcessOutput*)arg;

    // 解析输入(WideToUtf8 已经在主线程做完,这里直接用 utf8 视图)
    auto nonUpAgents   = ParseNonUpListUtf8(out->utf8_agents);
    auto nonUpWEngines = ParseNonUpListUtf8(out->utf8_wengines);

    std::string_view bufferView((const char*)out->viewPtr, out->fileSize);
    if (bufferView.size() >= 3 &&
        (unsigned char)bufferView[0] == 0xEF &&
        (unsigned char)bufferView[1] == 0xBB &&
        (unsigned char)bufferView[2] == 0xBF) {
        bufferView.remove_prefix(3);
    }

    // PMR:2MB 单调缓冲池 (monotonic_buffer_resource)。v0.1.3.2 起【改放堆上】(此前在栈上)。
    //
    // 为什么从栈改到堆 (用 make_unique_for_overwrite, 而不是 std::vector<std::byte>(2MB)):
    //   - 修正 v0.1.3.1 的一处错误结论: 当时注释说"栈版只有真正写入的 ~600KB 才落到物理页",
    //     这不对。固定 2MB 栈帧 >= 1 页时 MSVC 序言会调 __chkstk 逐页探测【整块 2MB】以保证栈
    //     能安全扩展 —— 进入 worker 时这 2MB 就被全部触达/提交, 与 PMR 实际用多少无关。
    //   - 反而堆写法更省: make_unique_for_overwrite 不清零 (区别于 std::vector(2MB) / 带括号的
    //     new[]() / calloc 那种值初始化), 内存按需触页 —— 只有 PMR 真正写到的 ~600KB 才 fault-in
    //     成物理页, 不像栈版被 __chkstk 强行摸满 2MB。
    //   - 顺带: 不再需要给 worker 配 4MB 栈 (见 ProcessFile_Submit), 线程栈回默认即可,
    //     去掉了那处 STACK_SIZE_PARAM_IS_A_RESERVATION 特殊化。
    //   - (先前感到"堆版更卡"是因为当时用了会清零的写法; 本写法无清零, 不复现该开销。)
    // 关于缓存: 别再写"L1/L2 热 / TLB 不 miss"。2MB = 512 页, 远超 L1(~48KB) 和 L1 DTLB(~64 项);
    //   能保证的只是减少分配器调用 + 让 temps/bucket 集中在一段连续内存 (利于顺序访问的局部性)。
    //   真实命中率要用 PMU 实测。
    // 关于 fallback: pool 没显式指定 upstream, 默认 = get_default_resource() (= new_delete_resource)。
    //   故【不是】严格只用这 2MB: 超大导入耗尽后会 fallback 到堆而非崩溃 (有意为之, 比抛 bad_alloc
    //   退出更实用)。另注 monotonic_buffer_resource 不回收 vector 扩容前的旧块, 直到整个 pool 析构
    //   —— 一旦 reserve() 预估被大幅突破, arena 占用会比普通 allocator 涨得快。
    //
    // 生命周期: 声明顺序 arena → pool → alloc, 析构逆序 (alloc/pool 先, arena 后), 故 pool 引用
    //   的 arena 内存在 pool 存活期间始终有效; 各 pmr 容器声明在 alloc 之后, 会更早析构。
    constexpr size_t kArenaSize = 2 * 1024 * 1024;
    auto arena = std::make_unique_for_overwrite<std::byte[]>(kArenaSize);  // 堆, 不清零 (C++20)
    std::pmr::monotonic_buffer_resource pool(arena.get(), kArenaSize);
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);

    struct Temp {
        long long id;
        GachaType gt;
        RankType  rt;
        std::string_view name;
        std::string_view time;
    };
    std::pmr::vector<Temp> temps(alloc);
    temps.reserve(10000);

    const auto listScan = ReadUigfPullList(bufferView, [&](std::string_view itemStr) {
        // UIGF v4.2 nap 字段:
        //   - "gacha_type" 数字字符串 ("1"/"2"/"3"/"5"/"102"/"103")
        //   - "rank_type"  "2"/"3"/"4" (B/A/S)
        //   - "name" 物品名; "time" 区服服务器时间 (非 UP 名单的时间窗按它判断)
        // 第三方存档可能将 rank_type 写为数字, 两种标量形态都接受。
        std::string_view rankStr = ExtractJsonValue(itemStr, "rank_type", true);
        if (rankStr.empty()) rankStr = ExtractJsonValue(itemStr, "rank_type", false);
        RankType  rt = ParseRankType(rankStr);
        GachaType gt = ParseGachaType(ExtractJsonValue(itemStr, "gacha_type", true));

        // 分析对象: 独家("2") + 重映("102") + 音擎("3") + 回响("103") + 常驻("1"); 邦布("5") 跳过。
        // 注: UP 池里也会出 A 级音擎/邦布, 这些非 S 级抽数照常推进保底进度,
        //     按 gacha_type 分桶 (而非 item_type), 语义才正确。
        if (gt != GachaType::AgentUP && gt != GachaType::WEngineUP &&
            gt != GachaType::Standard && gt != GachaType::Rescreening &&
            gt != GachaType::Reverberation) return;

        std::string_view name = ExtractJsonValue(itemStr, "name", true);
        std::string_view time = ExtractJsonValue(itemStr, "time", true);

        std::string_view idStr = ExtractJsonValue(itemStr, "id", true);
        if (idStr.empty()) idStr = ExtractJsonValue(itemStr, "id", false);
        long long parsed_id = 0;
        if (!idStr.empty()) {
            std::from_chars(idStr.data(), idStr.data() + idStr.size(), parsed_id);
        }

        temps.push_back(Temp{parsed_id, gt, rt, name, time});
    });

    // 扫描可能已经交付前半段记录, 结构损坏时仍必须中止, 不能分析残缺历史。
    if (listScan == zzzjson::ArrayScan::Malformed || temps.empty()) {
        out->ok = false;
        if (listScan == zzzjson::ArrayScan::Malformed) {
            out->errMsg = L"存档文件结构损坏或不完整: 抽卡记录数组没有正常闭合, 或含有非对象元素 / 缺少分隔逗号。\n"
                          L"为避免基于残缺数据给出错误统计, 已停止分析。请检查该文件。";
        } else {
            out->errMsg = listScan == zzzjson::ArrayScan::NotFound
                ? L"没有在文件里找到抽卡记录数组 (UIGF v4.2 的 nap[0].list)。"
                : L"文件里没有可分析的抽卡记录。";
        }
        // 防御分支: WM_DESTROY 会先 join 本线程再销毁窗口, "关窗导致 HWND 失效"
        // 已不会发生; PostMessageW 仍可能因极端情况失败 (如线程消息队列满 10000 条)。
        // 失败则没人消费 out → worker 自己清理, 否则泄漏 ProcessOutput + mmap 句柄,
        // 且 g_processing 卡在 1。out 在 Submit 里已 release 给 worker, 此处归 worker 所有。
        if (!PostMessageW(out->hwnd_main, WM_APP_PROCESS_DONE, 0, (LPARAM)out)) {
            delete out;
            InterlockedExchange(&g_processing, 0);
        }
        return 0;
    }

    // 排序: ZZZ 的 id 是 19 位全局递增正整数, 单关键字升序 = 时间升序
    // (不需要终末地的"武器取负分区 + |id|"三级排序)
    auto less = [](const Temp& a, const Temp& b) { return a.id < b.id; };
    bool sorted = true;
    for (size_t i = 1; i < temps.size(); ++i) {
        if (less(temps[i], temps[i - 1])) { sorted = false; break; }
    }
    if (!sorted) std::ranges::sort(temps, less);

    // 分桶: 按 gacha_type 分到 独家 / 重映 / 音擎 / 回响 / 常驻 五个桶
    PullBucket bucketAgent        (alloc); bucketAgent.reserve(6000);
    PullBucket bucketRescreening  (alloc); bucketRescreening.reserve(2000);
    PullBucket bucketWEngine      (alloc); bucketWEngine.reserve(4000);
    PullBucket bucketReverberation(alloc); bucketReverberation.reserve(2000);
    PullBucket bucketStandard     (alloc); bucketStandard.reserve(4000);
    for (const auto& t : temps) {
        if      (t.gt == GachaType::AgentUP)       bucketAgent        .push_back(t.rt, t.name, t.time);
        else if (t.gt == GachaType::Rescreening)   bucketRescreening  .push_back(t.rt, t.name, t.time);
        else if (t.gt == GachaType::WEngineUP)     bucketWEngine      .push_back(t.rt, t.name, t.time);
        else if (t.gt == GachaType::Reverberation) bucketReverberation.push_back(t.rt, t.name, t.time);
        else                                       bucketStandard     .push_back(t.rt, t.name, t.time);
    }

    // 重映 / 回响沿用独家 / 音擎的非 UP 名单 (Starward 的做法; 官方抽卡详情显示 3.1 起
    // 复刻频段的定制调频歪池与独家频段相同, 但两者是否始终一致没有官方说明, 未证实)。
    out->statsAgent         = Calculate(bucketAgent,         PoolKind::Agent,         nonUpAgents);
    out->statsRescreening   = Calculate(bucketRescreening,   PoolKind::Rescreening,   nonUpAgents);
    out->statsWEngine       = Calculate(bucketWEngine,       PoolKind::WEngine,       nonUpWEngines);
    out->statsReverberation = Calculate(bucketReverberation, PoolKind::Reverberation, nonUpWEngines);
    out->statsStandard      = Calculate(bucketStandard,      PoolKind::Standard,      nonUpAgents);

    // 在 worker 渲染输出文本(swprintf 是 thread-safe;只有 SetWindowTextW 不是)
    wchar_t winAgentStr[64] = L"[无数据]";
    if (out->statsAgent.avg_win >= 0)
        swprintf(winAgentStr, 64, L"%.2f 抽", out->statsAgent.avg_win);
    wchar_t winWEngineStr[64] = L"[无数据]";
    if (out->statsWEngine.avg_win >= 0)
        swprintf(winWEngineStr, 64, L"%.2f 抽", out->statsWEngine.avg_win);

    wchar_t pendAgentStr[96] = L"";
    if (out->statsAgent.censored_pity_all > 0 || out->statsAgent.censored_pity_up > 0) {
        swprintf(pendAgentStr, 96, L"  [当前水位: 距上次S级 %d 抽 / 距上次 UP %d 抽]",
                 out->statsAgent.censored_pity_all, out->statsAgent.censored_pity_up);
    }
    wchar_t pendWEngineStr[96] = L"";
    if (out->statsWEngine.censored_pity_all > 0 || out->statsWEngine.censored_pity_up > 0) {
        swprintf(pendWEngineStr, 96, L"  [当前水位: 距上次S级 %d 抽 / 距上次 UP %d 抽]",
                 out->statsWEngine.censored_pity_all, out->statsWEngine.censored_pity_up);
    }
    wchar_t pendStandardStr[96] = L"";
    if (out->statsStandard.censored_pity_all > 0) {
        swprintf(pendStandardStr, 96, L"  [当前水位: 距上次S级 %d 抽]",
                 out->statsStandard.censored_pity_all);
    }
    wchar_t pendRescreeningStr[96] = L"";
    if (out->statsRescreening.censored_pity_all > 0 || out->statsRescreening.censored_pity_up > 0) {
        swprintf(pendRescreeningStr, 96, L"  [当前水位: 距上次S级 %d 抽 / 距上次 UP %d 抽]",
                 out->statsRescreening.censored_pity_all, out->statsRescreening.censored_pity_up);
    }
    wchar_t pendReverberationStr[96] = L"";
    if (out->statsReverberation.censored_pity_all > 0 || out->statsReverberation.censored_pity_up > 0) {
        swprintf(pendReverberationStr, 96, L"  [当前水位: 距上次S级 %d 抽 / 距上次 UP %d 抽]",
                 out->statsReverberation.censored_pity_all, out->statsReverberation.censored_pity_up);
    }

    auto ksLabel = [](const StatsResult& r) -> const wchar_t* {
        if (r.count_all == 0) return L"-";
        return r.ks_is_normal ? L"符合理论模型" : L"偏离过大";
    };
    auto ksUpLabel = [](const StatsResult& r) -> const wchar_t* {
        if (r.count_up == 0) return L"-";
        return r.ks_is_normal_up ? L"符合理论模型" : L"偏离过大";
    };
    const wchar_t* ksAgentLabel      = ksLabel  (out->statsAgent);
    const wchar_t* ksWEngineLabel    = ksLabel  (out->statsWEngine);
    const wchar_t* ksStandardLabel   = ksLabel  (out->statsStandard);
    const wchar_t* ksAgentUpLabel    = ksUpLabel(out->statsAgent);
    const wchar_t* ksWEngineUpLabel  = ksUpLabel(out->statsWEngine);
    const wchar_t* ksRescreeningLabel   = ksLabel(out->statsRescreening);
    const wchar_t* ksReverberationLabel = ksLabel(out->statsReverberation);

    // 8192 字符缓冲: 五段池块 (独家/音擎/重映/回响各 4 行 + 常驻 3 行) 实测约 1700 字符,
    // 留足余量避免 swprintf 截断 (截断会让 SetWindowTextW 显示残缺尾巴)。
    // 重映 / 回响的 UP 行只给描述统计: 不印理论期望、不歪率与 UP 的 K-S (见 Calculate)。
    wchar_t outMsg[8192];
    swprintf(outMsg, 8192,
        L"【独家频段 (代理人 UP 池)】 总计 S 级: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合 S 级 (含歪) 出货平均期望:     %5.2f 抽 (理论 ≈ 62.30)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽 (理论 ≈ 93.45)   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (理论 50%%) (%d胜%d负)\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls\r\n\r\n"
        L"【独家重映频段 (代理人复刻)】 总计 S 级: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合 S 级 (含歪) 出货平均期望:     %5.2f 抽 (理论 ≈ 62.30*)  [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期 UP 的平均间隔:             %5.2f 抽                           [95%% CI: %5.1f ~ %5.1f]    |   每期首个 S 必为 UP (福利保底), 不作不歪率与 K-S 判定\r\n"
        L" ▶ (* 常规保底按 wiki 口径与独家频段相同, 官方未单独公示概率)\r\n\r\n"
        L"【音擎频段 (武器 UP 池)】 总计 S 级: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合 S 级 (含歪) 出货平均期望:     %5.2f 抽 (理论 ≈ 49.71)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期限定 UP 的综合平均期望:   %5.2f 抽 (理论 ≈ 62.13)   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (理论 75%%) (%d胜%d负)\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls\r\n\r\n"
        L"【音擎回响频段 (音擎复刻)】 总计 S 级: %d | 出当期 UP: %d%ls\r\n"
        L" ▶ 综合 S 级 (含歪) 出货平均期望:     %5.2f 抽 (理论 ≈ 49.71*)  [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期 UP 的平均间隔:             %5.2f 抽                           [95%% CI: %5.1f ~ %5.1f]    |   每期首个 S 必为 UP (福利保底), 不作不歪率与 K-S 判定\r\n"
        L" ▶ (* 常规保底按 wiki 口径与音擎频段相同, 官方未单独公示概率)\r\n\r\n"
        L"【常驻频段 (热门卡司)】 总计 S 级: %d%ls\r\n"
        L" ▶ 综合 S 级出货平均期望:             %5.2f 抽 (理论 ≈ 62.30)   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S 检验偏离度 D值: %.3f (%ls)]\r\n"
        L" ▶ (常驻频段无 UP 概念; 新人保底/300抽自选不在抽卡记录内, 早期样本可能轻微偏离理论)",
        out->statsAgent.count_all, out->statsAgent.count_up, pendAgentStr,
        out->statsAgent.avg_all, (std::max)(1.0, out->statsAgent.avg_all - out->statsAgent.ci_all_err),
        out->statsAgent.avg_all + out->statsAgent.ci_all_err, out->statsAgent.cv_all * 100.0,
        out->statsAgent.ks_d_all, ksAgentLabel,
        out->statsAgent.avg_up, (std::max)(1.0, out->statsAgent.avg_up - out->statsAgent.ci_up_err),
        out->statsAgent.avg_up + out->statsAgent.ci_up_err,
        out->statsAgent.win_rate_5050 >= 0 ? out->statsAgent.win_rate_5050 * 100.0 : 0.0,
        out->statsAgent.win_5050, out->statsAgent.lose_5050,
        out->statsAgent.ks_d_up, ksAgentUpLabel,
        winAgentStr,
        out->statsRescreening.count_all, out->statsRescreening.count_up, pendRescreeningStr,
        out->statsRescreening.avg_all, (std::max)(1.0, out->statsRescreening.avg_all - out->statsRescreening.ci_all_err),
        out->statsRescreening.avg_all + out->statsRescreening.ci_all_err, out->statsRescreening.cv_all * 100.0,
        out->statsRescreening.ks_d_all, ksRescreeningLabel,
        out->statsRescreening.avg_up, (std::max)(1.0, out->statsRescreening.avg_up - out->statsRescreening.ci_up_err),
        out->statsRescreening.avg_up + out->statsRescreening.ci_up_err,
        out->statsWEngine.count_all, out->statsWEngine.count_up, pendWEngineStr,
        out->statsWEngine.avg_all, (std::max)(1.0, out->statsWEngine.avg_all - out->statsWEngine.ci_all_err),
        out->statsWEngine.avg_all + out->statsWEngine.ci_all_err, out->statsWEngine.cv_all * 100.0,
        out->statsWEngine.ks_d_all, ksWEngineLabel,
        out->statsWEngine.avg_up, (std::max)(1.0, out->statsWEngine.avg_up - out->statsWEngine.ci_up_err),
        out->statsWEngine.avg_up + out->statsWEngine.ci_up_err,
        out->statsWEngine.win_rate_5050 >= 0 ? out->statsWEngine.win_rate_5050 * 100.0 : 0.0,
        out->statsWEngine.win_5050, out->statsWEngine.lose_5050,
        out->statsWEngine.ks_d_up, ksWEngineUpLabel,
        winWEngineStr,
        out->statsReverberation.count_all, out->statsReverberation.count_up, pendReverberationStr,
        out->statsReverberation.avg_all, (std::max)(1.0, out->statsReverberation.avg_all - out->statsReverberation.ci_all_err),
        out->statsReverberation.avg_all + out->statsReverberation.ci_all_err, out->statsReverberation.cv_all * 100.0,
        out->statsReverberation.ks_d_all, ksReverberationLabel,
        out->statsReverberation.avg_up, (std::max)(1.0, out->statsReverberation.avg_up - out->statsReverberation.ci_up_err),
        out->statsReverberation.avg_up + out->statsReverberation.ci_up_err,
        out->statsStandard.count_all, pendStandardStr,
        out->statsStandard.avg_all, (std::max)(1.0, out->statsStandard.avg_all - out->statsStandard.ci_all_err),
        out->statsStandard.avg_all + out->statsStandard.ci_all_err, out->statsStandard.cv_all * 100.0,
        out->statsStandard.ks_d_all, ksStandardLabel
    );
    out->outMsg = outMsg;
    out->ok = true;    out->outMsg = outMsg;
    out->ok = true;

    // 同上的防御分支 (消息队列满等极端失败)。窗口关闭路径已由 WM_DESTROY 收口:
    // 先 join 本线程 (彼时 hwnd 仍有效, 本行 PostMessageW 必然成功入队), 再 reap 队列里
    // 这条永远不会被派发的结果消息并释放载荷 —— 见 WndProc 的 WM_DESTROY。
    // (v0.1.3.3 修正: 旧注释称"由 ExitProcess 统一回收, 无实际危害, 不加 join" —— 结论
    //  不成立: ExitProcess 会直接终止其它线程, worker 若恰好在 CRT 堆锁内被终止, 退出
    //  流程可能挂死。现已改为退出前必 join, 该风险窗口不复存在。)
    if (!PostMessageW(out->hwnd_main, WM_APP_PROCESS_DONE, 0, (LPARAM)out)) {
        delete out;
        InterlockedExchange(&g_processing, 0);
    }
    return 0;
}

// 主线程入口:做 I/O 准备 + 启动 worker。
// 返回 false 表示提交失败(应立即清理),true 表示 worker 已启动(WM_APP_PROCESS_DONE
// 会在完成时投递)。
bool ProcessFile_Submit(HWND hwnd, const std::wstring& path) {
    // 双开保护:用 InterlockedCompareExchange 原子地把 0->1
    if (InterlockedCompareExchange(&g_processing, 1, 0) != 0) {
        return false;  // 已有 worker 在跑,忽略本次拖入
    }

    auto out = std::make_unique<ProcessOutput>();
    out->hwnd_main = hwnd;

    // 主线程读 GUI 控件文本(子控件的 GetWindowTextW 不允许从 worker 调)
    out->utf8_agents   = WideToUtf8(GetDynamicWindowText(hAgentEdit));
    out->utf8_wengines = WideToUtf8(GetDynamicWindowText(hWEngineEdit));

    // 主线程做文件 mmap,所有权直接交给 ProcessOutput(零拷贝)。
    // mmap view 在 worker 持有期间一直有效,Consume 阶段 ProcessOutput 析构统一 unmap。
    // 失败路径下也由 unique_ptr<ProcessOutput> 析构正确清理(已分配的资源)。
    out->hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out->hFile == INVALID_HANDLE_VALUE) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    // v0.1.3.2: GetFileSize (32 位, 截断 >4GB) → GetFileSizeEx (64 位)。抽卡文件正常远小于 4GB,
    // 但输入来自外部文件, 用 64 位读 + 显式上界校验更稳健 (尤其 32 位构建下 size_t 仅 4GB)。
    LARGE_INTEGER fileSize64{};
    if (!GetFileSizeEx(out->hFile, &fileSize64) ||
        fileSize64.QuadPart <= 0 ||
        static_cast<unsigned long long>(fileSize64.QuadPart) >
            static_cast<unsigned long long>(SIZE_MAX)) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    out->fileSize = static_cast<size_t>(fileSize64.QuadPart);
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

    // v0.1.3.3: 回收上一个 worker 的句柄 (若有)。能走到这里说明 CAS 已拿到锁, 即上一个
    // worker 已投递结果 (Consume 清零 g_processing) 或已自清理 —— 其线程最多只剩微秒级的
    // CRT 尾声, 此处 join 几乎不阻塞, 换来句柄零泄漏 + 任意时刻至多一个未决句柄。
    if (g_hWorker) {
        WaitForSingleObject(g_hWorker, INFINITE);
        CloseHandle(g_hWorker);
        g_hWorker = NULL;
    }

    // 启动 worker。v0.1.3.2: 2MB PMR arena 已挪到堆 (见 ProcessFile_Worker), worker 栈需求很小,
    // 故 stack_size 传 0 (用 EXE 默认栈大小), 不再需要 4MB + STACK_SIZE_PARAM_IS_A_RESERVATION。
    //
    // 仍用 _beginthreadex 而非裸 CreateThread: worker 大量使用 CRT (std::string / unordered_map /
    // swprintf / std::ranges::sort / 异常), 应走 CRT 线程入口以正确初始化/清理 per-thread 状态;
    // 裸 CreateThread 跑 CRT 在极端低内存下有终止进程的风险。
    uintptr_t raw = _beginthreadex(nullptr, 0,
                                   ProcessFile_Worker, out.get(),
                                   0, nullptr);
    if (raw == 0) {
        InterlockedExchange(&g_processing, 0);
        return false;
    }
    // v0.1.3.3: 句柄不再立即 CloseHandle —— 保留在 g_hWorker 供 join (下次 Submit 开头 /
    // WM_DESTROY 兜底), 保证退出前 worker 完整走完, 杜绝 ExitProcess 截杀尾声线程。
    g_hWorker = reinterpret_cast<HANDLE>(raw);
    out.release();         // worker 接管所有权, 完成时主线程在 WM_APP_PROCESS_DONE 里 delete
    return true;
}

// 主线程消费 worker 结果. 必须在 WM_APP_PROCESS_DONE 里调用
void ProcessFile_Consume(HWND hwnd, ProcessOutput* out) {
    if (out->ok) {
        // 把结果搬到全局 (主线程独占,不需要锁)。
        // ★ 这里【每加一个池子就必须同步加一行】—— 图表绘制读的是这些全局量, 不是
        //   out-> 里的字段; 漏掉一行, 文字统计有数, 对应那行图表却始终"暂无出金数据"
        //   (Endfield-Gacha 加重构寻访时踩过这个坑)。
        statsAgent         = out->statsAgent;
        statsRescreening   = out->statsRescreening;
        statsWEngine       = out->statsWEngine;
        statsReverberation = out->statsReverberation;
        statsStandard      = out->statsStandard;
        SetWindowTextW(hOutEdit, out->outMsg.c_str());
        RebuildChartCache(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
    } else {
        SetWindowTextW(hOutEdit,
            out->errMsg.empty() ? L"处理失败,请检查文件格式" : out->errMsg.c_str());
    }
    delete out;
    InterlockedExchange(&g_processing, 0);  // 释放双开锁
}

// -------------------------------------------------------
// 图形渲染 —— 曲线坐标点用栈数组, 避免在绘图热路径里反复 new 小对象。
// -------------------------------------------------------
// ---------------------------------------------------------
// [ECDF (经验累积分布函数) 图]
//
// 设计:
//   - 离散阶梯线: ECDF(x) = (Σ_{k<=x} freq[k]) / total
//   - 同时画综合(蓝)和 UP(红)两条经验 ECDF + 两条理论 CDF(虚线)
//   - 标记 KS 偏离最大处的 D 值竖线(与 ks_d_all 一致,用户可视化检验)
//   - 右删失处理: ECDF 终点不到 1.0(因为 censored_pity 表示当前未出货)
//
// 为什么从 KDE 切换到 ECDF:
//   抽卡数据是离散整数 pity,样本量极小(n ~10),KDE 的高斯核平滑会引入虚假
//   连续性,带宽选择对结果影响巨大,在 x=1 等边界处会产生人造凸起。
//   ECDF 是离散数据的标准非参数显示,无任何参数选择,与 KS 检验直接对应。
// ---------------------------------------------------------
void DrawECDF(Gdiplus::Graphics& g, Gdiplus::Rect rect,
              const StatsResult& stats,
              std::span<const double> theory_cdf_all,
              std::span<const double> theory_cdf_up,
              const std::wstring& title, int limit_base,
              int ecdf_up_step_size = 1,
              bool show_up = true) {
    // show_up=false (常驻频段): 仅隐藏 UP 图例文字。UP 曲线本身因
    // count_up=0 + 空理论 CDF (span 长度 0) 自然消失, 不需要额外分支。
    // 重映 / 回响 (stats.up_descriptive): 调用方传空的 UP 理论 CDF, 只画经验 UP 曲线,
    // 不画 UP 的 K-S 标记, 并在标题下补一行说明。
    const auto& freq_all = stats.freq_all;
    const auto& freq_up  = stats.freq_up;
    const int count_all = stats.count_all, count_up = stats.count_up;
    const wchar_t* upNote = stats.up_descriptive
        ? L"UP 含每期首个 S 的福利保底, 仅画经验曲线, 不与理论对照"
        : nullptr;
    const float noteHeight = upNote ? DPIScaleF(18.0f) : 0.0f;
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
    // v0.1.2.1: 无出金时不再直接 return, 而是继续渲染坐标轴和理论 CDF (蓝/红虚线),
    // 让用户能看到"这个池子的理论分布长什么样"的参考曲线。原版直接 "暂无出金数据"
    // 一句话占满图框, 新池子用户体验差 (查不到任何信息).
    // 跳过的只是: 经验 ECDF 阶梯线 (drawEmpiricalECDF)、KS 偏离度标记。
    max_x = ((max_x / 10) + 1) * 10;
    // 钳到 freq 数组合法上界 199 (容量 200, Calculate 守 slot<200)。
    // 事件落在 190..199 时上一行取整会把 max_x 推到 200, 下游 freq[k] (k<=max_x)
    // 越界读。ZZZ 各池间隔理论上 <=180 (代理人 UP 大保底), >180 只可能来自异常
    // 数据, 此钳制是纯防御 (MRL 同款见 DrawMRL)。
    if (max_x > 199) max_x = 199;

    // 网格 + 坐标轴
    Gdiplus::Pen gridPen(Gdiplus::Color(255, 230, 230, 230), DPIScaleF(1.0f));
    Gdiplus::Pen axisPen(Gdiplus::Color(255, 80, 80, 80),  DPIScaleF(1.0f));
    float plotX = (float)rect.X + DPIScaleF(50.0f);
    float plotY = (float)rect.Y + DPIScaleF(40.0f) + noteHeight;
    float plotW = (float)rect.Width  - DPIScaleF(70.0f);
    float plotH = (float)rect.Height - DPIScaleF(60.0f) - noteHeight;
    if (plotW <= 0 || plotH <= 0) return;

    if (upNote) {
        Gdiplus::Font noteFont(&fontFamily, DPIScaleF(11.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush noteBrush(Gdiplus::Color(255, 120, 120, 120));
        g.DrawString(upNote, -1, &noteFont,
                     Gdiplus::PointF((float)rect.X + DPIScaleF(15.0f),
                                     (float)rect.Y + DPIScaleF(36.0f)), &noteBrush);
    }

    g.DrawLine(&axisPen, plotX, plotY,         plotX, plotY + plotH);
    g.DrawLine(&axisPen, plotX, plotY + plotH, plotX + plotW, plotY + plotH);

    auto getPt = [&](int x, double y) -> Gdiplus::PointF {
        if (y < 0) y = 0; if (y > 1) y = 1;
        return Gdiplus::PointF(plotX + (float)x / (float)max_x * plotW,
                               plotY + plotH - (float)y * plotH);
    };

    Gdiplus::Font tickFont(&fontFamily, DPIScaleF(11.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255, 120, 120, 120));

    // Y 轴 0/25/50/75/100% 网格
    for (int i = 0; i <= 4; ++i) {
        double y_val = (double)i / 4.0;
        float py = plotY + plotH - (float)y_val * plotH;
        if (i > 0) g.DrawLine(&gridPen, plotX, py, plotX + plotW, py);
        g.DrawLine(&axisPen, plotX - DPIScaleF(5.0f), py, plotX, py);
        wchar_t y_label[16]; swprintf(y_label, 16, L"%d%%", i * 25);
        float labelW = (float)wcslen(y_label) * DPIScaleF(5.5f) + DPIScaleF(8.0f);
        g.DrawString(y_label, -1, &tickFont, Gdiplus::PointF(plotX - labelW, py - DPIScaleF(6.0f)), &tickBrush);
    }
    // X 轴刻度
    int step = (max_x > 140) ? 20 : 10;
    for (int x = 0; x <= max_x; x += step) {
        float px = plotX + (float)x / (float)max_x * plotW;
        g.DrawLine(&axisPen, px, plotY + plotH, px, plotY + plotH + DPIScaleF(5.0f));
        wchar_t x_label[16]; swprintf(x_label, 16, L"%d", x);
        float xoff = (x < 10 ? 4.0f : x < 100 ? 8.0f : 12.0f) * DPIScaleF(1.0f);
        g.DrawString(x_label, -1, &tickFont,
                     Gdiplus::PointF(px - xoff, plotY + plotH + DPIScaleF(8.0f)), &tickBrush);
    }

    // 画理论 CDF (虚线).
    //
    // 形态选择:
    //   stepSize == 1 (角色 / 综合武器): 折线连相邻整数点
    //     —— 角色每抽都是真实采样点, 折线是平缓上升的曲线, dash 平滑展开。
    //     —— 旧版 stepSize==1 强制阶梯 (水平+垂直) 在大量微小 90° 角点上让
    //        dash pattern 反复重启, 视觉糊成"蛆状"小钩 —— 改成纯折线根除。
    //   stepSize > 1  (武器 UP): 真阶梯, 水平 (stepSize-1) 抽 + 垂直跳跃
    //     —— 反映"10 抽一组判定"机制: 拨内 CDF 真的不变, 阶梯是机制必然
    //
    // 自动跳跃检测 (v0.1.1):
    // 在 stepSize=1 的折线模式下用状态机:
    //   - 折线模式: Δ_k / Δ_{k-1} > JUMP_THRESHOLD (=5) → 进入阶梯模式
    //   - 阶梯模式: Δ 持续上升 (Δ_k > Δ_{k-1}) → 保持阶梯; 否则退出折线
    // 这样能正确表达"软保底响应到峰值"这一持续陡升过程, 而不只是把
    // 触发跳跃的那一个点画成阶梯。例如角色 UP CDF 在 k=66 hazard 跳跃,
    // 但 CDF 增量峰值出现在 k=69 (因为 D[s] 迭代积分需要几抽反应):
    //   k=66 (Δ=0.018, 进入阶梯) → k=67 (0.031) → k=68 (0.040) → k=69 (0.045) →
    //   k=70 (0.045 ≤ 0.045 退出阶梯) → 后续平滑衰减
    // 自动覆盖以下场景, 不需要硬编码具体 k:
    //   - 角色综合 k=30 (单点跳跃: 30 抽合并 11 次判定)
    //   - 角色综合 k=66~69 (软保底响应)
    //   - 角色 UP   k=66~69 (软保底响应)
    //   - 角色 UP   k=120 (硬保底)
    // 武器综合 k=31 比值仅 2.86, 不触发, 保持平滑折线 (软保底渐进展开是真实形态)。
    //
    // 画法: 用 GraphicsPath 攒整条路径再一次性 stroke,
    //       dash pattern 沿连续路径走, 跨拐角不重启;
    //       LineJoin=Round 让拐角圆滑过渡, 缓解 dash 实部压在 90° 角的视觉错乱。
    auto drawTheoryCDF = [&](std::span<const double> cdf,
                             int stepSize, Gdiplus::Color color) {
        const int cdf_len = (int)cdf.size();   // v0.1.3.3: span 自带长度
        if (cdf_len < 2) return;
        Gdiplus::Pen pen(color, DPIScaleF(1.5f));
        Gdiplus::REAL dash[2] = { DPIScaleF(4.0f), DPIScaleF(3.0f) };
        pen.SetDashPattern(dash, 2);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        // 与 KS / MRL 共用有效末端, 排除饱和段和未填充哨兵段。
        const int upper_eff = (std::min)(FindCDFLastValid(cdf), max_x);
        if (upper_eff < 1) return;
        Gdiplus::GraphicsPath path;
        auto p0 = getPt(0, cdf[0]);
        Gdiplus::PointF prev = p0;
        if (stepSize == 1) {
            constexpr double JUMP_THRESHOLD = 5.0;
            constexpr double MIN_PREV_DELTA = 1e-6;
            bool inStepMode = false;
            for (int k = 1; k <= upper_eff; ++k) {
                double curDelta  = cdf[k] - cdf[k-1];
                double prevDelta = (k >= 2) ? cdf[k-1] - cdf[k-2] : 0.0;
                bool drawAsStep;
                if (inStepMode) {
                    // 阶梯模式: Δ 持续上升就保持, 否则退出
                    if (curDelta > prevDelta && prevDelta > MIN_PREV_DELTA) {
                        drawAsStep = true;
                    } else {
                        inStepMode = false;
                        drawAsStep = false;
                    }
                } else {
                    // 折线模式: 检测进入条件
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
        } else {
            // 武器: 阶梯, 水平段 + 垂直段
            int k = stepSize;
            while (k <= upper_eff) {
                auto pH = getPt(k, cdf[k - stepSize]);  // 水平到拐角
                auto pV = getPt(k, cdf[k]);              // 垂直跳跃
                path.AddLine(prev, pH);
                path.AddLine(pH, pV);
                prev = pV;
                k += stepSize;
            }
            // 末段补水平到 upper_eff (如果没走到)
            if (k - stepSize < upper_eff) {
                auto pEnd = getPt(upper_eff, cdf[k - stepSize]);
                path.AddLine(prev, pEnd);
            }
        }
        g.DrawPath(&pen, &path);
    };

    // 画经验 ECDF (实阶梯线).
    // 注: 删失观测(用户当前还在垫的 cur_pity)不画在 ECDF 上 ——
    // 因为它还没事件化, 强行画一个标记反而误导(会落在 ECDF 终点 y=100% 处)。
    // MRL 图已经精确显示"已垫 X 抽 / 预期还需 Y 抽", 这里不重复。
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

    drawTheoryCDF(theory_cdf_all, 1,                 Gdiplus::Color(180, 65, 140, 240));
    drawTheoryCDF(theory_cdf_up,  ecdf_up_step_size, Gdiplus::Color(180, 240, 80, 80));
    drawEmpiricalECDF(freq_all, count_all, Gdiplus::Color(255, 65, 140, 240));
    drawEmpiricalECDF(freq_up,  count_up,  Gdiplus::Color(255, 240, 80, 80));

    // KS 标记 (v0.1.1.1: 双色, 综合蓝色标签左上 / UP 红色标签右下)
    //
    // 标签布局策略:
    //   - 蓝色 (综合): 标签贴在 KS 虚线的左上方 (anchor 右下)
    //   - 红色 (UP):   标签贴在 KS 虚线的右下方 (anchor 左上)
    //   标签优先放在标记两侧, 再限制到绘图区内; 颜色与对应 ECDF 实线一致, 用户能看出
    //   "蓝色 KS 标签 → 测的是综合 ECDF 的偏离"。
    //
    // 标签自带白色描边 (4 偏移方向先画白色底字再叠主文本), 在彩色实线上的可读性更好。
    enum class KSLabelAnchor { LeftTop, RightBottom };
    auto drawKSMarker = [&](const KSLocation& location, double d,
                            std::span<const double> cdf,
                            BYTE r, BYTE gC, BYTE b,
                            KSLabelAnchor anchor) {
        if (d <= 0.01 || location.x <= 0 || location.x > max_x) return;
        const int upper_eff = (std::min)(FindCDFLastValid(cdf), max_x);
        if (upper_eff < 1) return;
        // D 和坐标均来自统计核心, 不再在图上用另一套范围重新计算。
        // 最大点可能在理论曲线之外; 保留 D, 但不连接到未绘制的理论尾部。
        const bool outsideTheory = location.x > upper_eff;
        auto p_emp = getPt(location.x, location.empirical);
        auto p_th  = getPt(location.x, location.theory);
        if (!outsideTheory) {
            Gdiplus::Pen ksPen(Gdiplus::Color(255, r, gC, b), DPIScaleF(1.5f));
            Gdiplus::REAL dash[2] = { DPIScaleF(2.0f), DPIScaleF(2.0f) };
            ksPen.SetDashPattern(dash, 2);
            g.DrawLine(&ksPen, p_emp.X, p_emp.Y, p_th.X, p_th.Y);
        }

        wchar_t lbl[96];
        swprintf(lbl, 96, L"KS D=%.3f%ls", d,
                 outsideTheory ? L" (尾部截断参考)" : L"");
        float midY = outsideTheory ? p_emp.Y : (p_emp.Y + p_th.Y) * 0.5f;

        // 测量文字宽度,根据 anchor 计算左上角坐标
        Gdiplus::RectF box;
        g.MeasureString(lbl, -1, &tickFont, Gdiplus::PointF(0, 0), &box);

        float tx, ty;
        if (anchor == KSLabelAnchor::LeftTop) {
            // 蓝色: 标签贴虚线左上 (文字右下角对齐到虚线左上)
            tx = p_emp.X - DPIScaleF(4.0f) - box.Width;
            ty = midY - DPIScaleF(2.0f) - box.Height;
        } else {
            // 红色: 标签贴虚线右下 (文字左上角对齐到虚线右下)
            tx = p_emp.X + DPIScaleF(4.0f);
            ty = midY + DPIScaleF(2.0f);
        }

        // 首抽或量程末端取得最大差时, 完整标签也须留在绘图区内。
        tx = (std::max)(plotX, (std::min)(plotX + plotW - box.Width, tx));
        ty = (std::max)(plotY, (std::min)(plotY + plotH - box.Height, ty));

        // 白色描边
        Gdiplus::SolidBrush whiteBr(Gdiplus::Color(255, 252, 253, 255));
        for (int dx = -1; dx <= 1; dx += 2) {
            for (int dy = -1; dy <= 1; dy += 2) {
                g.DrawString(lbl, -1, &tickFont,
                             Gdiplus::PointF(tx + (float)dx, ty + (float)dy),
                             &whiteBr);
            }
        }
        // 主文本 (与对应 ECDF 实线同色)
        Gdiplus::SolidBrush mainBr(Gdiplus::Color(255, r, gC, b));
        g.DrawString(lbl, -1, &tickFont, Gdiplus::PointF(tx, ty), &mainBr);
    };
    // 蓝色 (综合): 左上
    drawKSMarker(stats.ks_location_all, stats.ks_d_all, theory_cdf_all,
                 65, 140, 240, KSLabelAnchor::LeftTop);
    // 红色 (UP): 右下。重映 / 回响不做 UP 的 K-S, 没有可标的点。
    if (!stats.up_descriptive) {
        drawKSMarker(stats.ks_location_up, stats.ks_d_up, theory_cdf_up,
                     240, 80, 80, KSLabelAnchor::RightBottom);
    }

    // 图例 (3 项水平排列: 综合实线 / UP 实线 / 理论 CDF 虚线)
    // 与 macOS / iOS 端布局对齐 —— 标题旁同一行,从右向左排,
    // 这样图例完全位于标题区(rect.Y+12 行),不会下沉到绘图区(rect.Y+40 起)。
    // 旧版图例垂直堆叠 3 行,最下面一项会落进绘图区与曲线重叠。
    Gdiplus::Font legendFont(&fontFamily, DPIScaleF(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush blueBr(Gdiplus::Color(255, 65, 140, 240));
    Gdiplus::SolidBrush redBr (Gdiplus::Color(255, 240, 80, 80));

    // 三个图例项的文字(色块在前,文字在后)
    const wchar_t* legAll  = L"综合S级 ECDF";
    const wchar_t* legUp   = L"当期限定 UP ECDF";
    const wchar_t* legThy  = L"理论 CDF (综合)";

    // 测量文字宽度,精确从右排 —— 不能用固定常量,因为不同字体/DPI 下宽度不同
    auto measureW = [&](const wchar_t* s) -> float {
        Gdiplus::RectF box;
        g.MeasureString(s, -1, &legendFont, Gdiplus::PointF(0, 0), &box);
        return box.Width;
    };
    const float swatchW    = DPIScaleF(14.0f);  // 实线/虚线色块宽度
    const float swatchGap  = DPIScaleF(6.0f);   // 色块到文字间距
    const float entryGap   = DPIScaleF(16.0f);  // 项与项之间间距
    const float legendY    = (float)rect.Y + DPIScaleF(12.0f);
    const float swatchYOff = DPIScaleF(8.0f);   // 色块在图例行内的垂直居中偏移

    // 从右往左:Theory(虚线) → UP → All
    float wAll  = measureW(legAll);
    float wUp   = measureW(legUp);
    float wThy  = measureW(legThy);
    float xRight = (float)rect.X + (float)rect.Width - DPIScaleF(12.0f);

    // 第 3 项: 理论 CDF (虚线,最右)
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

    // 第 2 项: UP ECDF (常驻频段 show_up=false 时整项跳过, 综合项左移补位)
    float xUpSw = xThySw;
    if (show_up) {
        float xUpText = xThySw - entryGap - wUp;
        xUpSw = xUpText - swatchGap - swatchW;
        g.FillRectangle(&redBr, xUpSw, legendY + swatchYOff - DPIScaleF(1.5f),
                        swatchW, DPIScaleF(3.0f));
        g.DrawString(legUp, -1, &legendFont,
                     Gdiplus::PointF(xUpText, legendY), &textBrush);
    }

    // 第 1 项: 综合 ECDF
    float xAllText = xUpSw - entryGap - wAll;
    float xAllSw   = xAllText - swatchGap - swatchW;
    g.FillRectangle(&blueBr, xAllSw, legendY + swatchYOff - DPIScaleF(1.5f),
                    swatchW, DPIScaleF(3.0f));
    g.DrawString(legAll, -1, &legendFont,
                 Gdiplus::PointF(xAllText, legendY), &textBrush);

    // 无出金时, 在绘图区中央叠加灰色提示 (v0.1.2.1)
    // 理论 CDF 已经画了, 此提示仅说明"经验数据为空", 不阻塞其它内容显示
    if (!hasData) {
        Gdiplus::Font hintFont(&fontFamily, DPIScaleF(13.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush hintBrush(Gdiplus::Color(200, 130, 130, 130));
        const wchar_t* hint = L"暂无出金数据 (仅显示理论曲线参考)";
        Gdiplus::RectF box;
        g.MeasureString(hint, -1, &hintFont, Gdiplus::PointF(0, 0), &box);
        g.DrawString(hint, -1, &hintFont,
                     Gdiplus::PointF(plotX + plotW * 0.5f - box.Width * 0.5f,
                                     plotY + plotH * 0.5f - box.Height * 0.5f),
                     &hintBrush);
    }
}

// ---------------------------------------------------------
// [MRL (Mean Residual Life) 图]
//
// MRL(t) = E[X - t | X > t] —— "已经垫了 t 抽,还要再垫多少抽的期望"
//
// 经验 MRL 计算 (从 freq 直方图):
//   MRL_emp(t) = Σ_{k>t} (k-t)·freq[k] / Σ_{k>t} freq[k]
//   分母 = 0 (即 t >= max_observed) 时 MRL 未定义
//
// 显示策略:
//   - 实线: t 处至少有 2 个观测在分子里 (Σ_{k>t} freq[k] >= 2),数值可靠
//   - 半透明虚线: 仅 1 个观测,高方差区
//   - 不画: 0 观测 (无意义)
//   - 同时画理论 MRL (虚线): 基于理论 CDF 数值积分
//   - 当前 censored_pity 位置画竖线 + "你在这里"标注 (用户决策视角的关键)
// ---------------------------------------------------------
void DrawMRL(Gdiplus::Graphics& g, Gdiplus::Rect rect,
             const std::array<int, 200>& freq_all,
             const std::array<int, 200>& freq_up,
             int count_all, int count_up,
             int censored_all, int censored_up,
             std::span<const double> theory_cdf_all,
             std::span<const double> theory_cdf_up,
             const std::wstring& title, int limit_base,
             int theory_all_cap = 0, int theory_up_cap = 0,
             bool show_up = true) {
    // show_up=false (常驻频段): 仅隐藏 UP 图例文字, UP 曲线/标注因
    // count_up=0 + 空理论 CDF 自然消失。
    //   各池 UP CDF 在 X 轴末端 (大保底处) 已饱和 (>1 - 1e-6), upper_eff 截在
    //   饱和点, 此时 (1 - cdf[upper_eff]) ≈ 0, 即使传 0 也无影响.
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
    max_x = (std::max)(max_x, (std::max)(censored_all, censored_up));
    // v0.1.2.1: 无出金时不再直接 return, 继续渲染理论 MRL 虚线作参考。
    // 经验 MRL 自然为空 (computeEmpiricalMRL 内部已防御 total==0), 当前水位仍可标注。
    // 无出金提示在最后叠加灰色字。
    max_x = ((max_x / 10) + 1) * 10;
    // 同 DrawECDF 的 199 钳制 —— 此处更严重: computeEmpiricalMRL 内
    // surv[t]/mrl[t] 在 t=200 是【栈数组越界写】(ASan 实测 stack-buffer-overflow),
    // 末尾 max_y 循环与 mrl_*[t] 读同样越界。触发条件: 任一 freq 事件落在 [190,199]。
    if (max_x > 199) max_x = 199;

    // ---- 计算经验 MRL 序列 (并记录每个 t 处的 surviving 计数) ----
    auto computeEmpiricalMRL = [&](const std::array<int, 200>& freq, int total)
        -> std::pair<std::array<double, 200>, std::array<int, 200>> {
        std::array<double, 200> mrl{}; mrl.fill(-1.0);  // -1 = undefined
        std::array<int, 200> surv{}; surv.fill(0);
        if (total == 0) return {mrl, surv};
        // 后缀和: 从最大 max_x 往回累加
        long long suf_count = 0, suf_weighted = 0;
        for (int t = max_x; t >= 0; --t) {
            // 在循环开始时 suf_count = Σ_{k>t} freq[k], 注意 k 从 t+1 开始
            // 我们要在每一步先用当前累积值算 MRL(t),再把 freq[t] 累加进去给下一轮 t-1 用
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

    // ---- 计算理论 MRL (基于理论 CDF, 可选长尾解析延伸) ----
    auto computeTheoryMRL = [&](std::span<const double> cdf, double tail_mean_excess = 0.0) {
        std::array<double, 200> tmrl{}; tmrl.fill(-1.0);
        const int cdf_len = (int)cdf.size();   // v0.1.3.3: span 自带长度
        if (cdf_len < 2) return tmrl;
        // 与 KS / ECDF 统一有效末端, 不将未填充槽位当作负的概率质量。
        const int upper_eff = FindCDFLastValid(cdf);
        if (upper_eff < 1) return tmrl;
        // 长尾点质量参数 (v0.1.2.4):
        //   tail_mass     = 1 - cdf[upper_eff]
        //   tail_position = upper_eff + tail_mean_excess
        //   仅当 tail_mean_excess > 0 且 tail_mass > 1e-9 时才追加。
        //   ZZZ 各池 CDF 都自然饱和到 1.0 (大保底封顶), 调用方不再传该参数
        //   (默认 0), 此机制保留作通用能力 (原终末地辉光池长尾延伸的遗产)。
        double tail_mass     = 1.0 - cdf[upper_eff];
        double tail_position = (double)upper_eff + tail_mean_excess;
        bool   has_tail      = (tail_mean_excess > 0.0) && (tail_mass > 1e-9);
        // 从 PDF: pdf[k] = cdf[k] - cdf[k-1], for k=1..upper_eff
        // MRL(t) = [Σ_{k>t} (k-t) · pdf[k] + (tail_position-t) · tail_mass] / (1 - cdf[t])
        for (int t = 0; t <= upper_eff - 1 && t <= max_x; ++t) {
            double surv_t = 1.0 - cdf[t];
            if (surv_t < 1e-9) break;
            double num = 0.0;
            for (int k = t + 1; k <= upper_eff; ++k) {
                double pdf_k = cdf[k] - cdf[k-1];
                num += (double)(k - t) * pdf_k;
            }
            if (has_tail) {
                num += (tail_position - (double)t) * tail_mass;
            }
            tmrl[t] = num / surv_t;
        }
        return tmrl;
    };
    auto theory_mrl_all = computeTheoryMRL(theory_cdf_all);
    auto theory_mrl_up  = computeTheoryMRL(theory_cdf_up);

    // ---- Y 轴范围: 取所有 MRL 值的最大值 ----
    double max_y = 1.0;
    for (int t = 0; t <= max_x; ++t) {
        if (mrl_all.first[t] > max_y) max_y = mrl_all.first[t];
        if (mrl_up.first[t]  > max_y) max_y = mrl_up.first[t];
        if (theory_mrl_all[t] > max_y) max_y = theory_mrl_all[t];
        if (theory_mrl_up[t]  > max_y) max_y = theory_mrl_up[t];
    }
    // 取整到 10 的倍数,留 10% 顶部空间
    max_y = std::ceil(max_y * 1.1 / 10.0) * 10.0;
    if (max_y < 10) max_y = 10;

    // ---- 网格 + 坐标轴 ----
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

    // Y 轴刻度 (单位: 抽)
    for (int i = 0; i <= 4; ++i) {
        double y_val = max_y * (double)i / 4.0;
        float py = plotY + plotH - (float)i / 4.0f * plotH;
        if (i > 0) g.DrawLine(&gridPen, plotX, py, plotX + plotW, py);
        g.DrawLine(&axisPen, plotX - DPIScaleF(5.0f), py, plotX, py);
        wchar_t y_label[16]; swprintf(y_label, 16, L"%.0f", y_val);
        float labelW = (float)wcslen(y_label) * DPIScaleF(5.5f) + DPIScaleF(8.0f);
        g.DrawString(y_label, -1, &tickFont, Gdiplus::PointF(plotX - labelW, py - DPIScaleF(6.0f)), &tickBrush);
    }
    // X 轴刻度
    int step = (max_x > 140) ? 20 : 10;
    for (int x = 0; x <= max_x; x += step) {
        float px = plotX + (float)x / (float)max_x * plotW;
        g.DrawLine(&axisPen, px, plotY + plotH, px, plotY + plotH + DPIScaleF(5.0f));
        wchar_t x_label[16]; swprintf(x_label, 16, L"%d", x);
        float xoff = (x < 10 ? 4.0f : x < 100 ? 8.0f : 12.0f) * DPIScaleF(1.0f);
        g.DrawString(x_label, -1, &tickFont,
                     Gdiplus::PointF(px - xoff, plotY + plotH + DPIScaleF(8.0f)), &tickBrush);
    }

    // ---- 画理论 MRL (虚线) ----
    // 用 GraphicsPath 一次性 stroke, dash pattern 沿连续路径走。
    // 注: 实际 theoryMRL 在 [0, cap] 区间是连续单调的(不会出现 -1 中段断开),
    // 所以单一 Path 一次构建即可,无需处理多段。
    // LineJoin=Round 让拐角圆滑 (武器 UP MRL 是锯齿状, 每 10 抽内斜率 -1 ,
    // 拐角处线段方向变化, Round 缓解 dash 实部压在拐角的视觉错乱)。
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

    // ---- 画经验 MRL ----
    //
    // 视觉编码 (v0.1.1):
    //   surv >= 2: 满色实线 2.5pt   ← 多个独立样本支撑, 统计可靠
    //   surv == 1: 半透明同色实线 1.8pt (alpha=115/255 ≈ 0.45)  ← 高方差区
    //
    // 历史: 之前 surv==1 段画虚线 (dash 4/3), 但红色 UP 理论 MRL 也是 dash 4/3,
    //       两者撞色撞样式无法分辨。改成半透明实线后, 视觉编码错开:
    //         "颜色淡 = 数据稀薄"   "虚线 = 理论参考"
    //       两个语义彻底分开, 用户一眼能看出哪条是经验数据尾巴、哪条是理论曲线。
    //
    // 实现: 实线段和半透明段分别攒到两个 GraphicsPath, 各自一次性 stroke。
    //       LineJoin=Round 让拐角圆滑过渡。
    auto drawEmpiricalMRL = [&](const std::pair<std::array<double, 200>, std::array<int, 200>>& mrl_data,
                                 BYTE r, BYTE gC, BYTE b) {
        const auto& mrl = mrl_data.first;
        const auto& surv = mrl_data.second;
        Gdiplus::Pen thickPen(Gdiplus::Color(255, r, gC, b), DPIScaleF(2.5f));
        thickPen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::Pen thinPen (Gdiplus::Color(115, r, gC, b), DPIScaleF(1.8f));
        thinPen.SetLineJoin(Gdiplus::LineJoinRound);

        // 累积:满色段进 thickPath, 半透明段进 thinPath (每段都 StartFigure 隔开)
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

    // ---- "你在这里" 竖线 (当前 censored_pity 位置) ----
    // 关键设计:
    //   - 综合 (蓝): 优先用综合理论 MRL, 否则降级到经验 MRL
    //   - UP (红):   v0.1.1 起也优先用 UP 理论 MRL (新增 g_cdf_*_up 后),
    //                否则降级到经验 MRL。有了精确的 UP 理论曲线后, 即使本次抽卡
    //                数据稀疏 / 全在同一 censored 区段内, 标注线也能给出可靠参考。
    //   - 虚线在 X 位置画出, 但标签固定在 plot 区域右上角竖排堆叠。
    //     避免: 标签贴虚线时碰到 X=1 这种边界情况会被裁切, 也避免红蓝标签互相重叠
    //     (例如两个 censored 数值接近时旧逻辑会把两段文本叠在一起)。
    //     视觉对应: 标签自带颜色, 用户能看出"蓝色标签对应蓝色虚线"。

    // (1) 先画虚线, 同时收集要展示的 (text, color) 条目
    struct CensoredLabel {
        std::wstring text;
        Gdiplus::Color color;
    };
    std::vector<CensoredLabel> censoredLabels;
    censoredLabels.reserve(2);

    auto resolveAndDrawLine = [&](int censored,
                                   const std::pair<std::array<double, 200>, std::array<int, 200>>& mrl_data,
                                   const std::array<double, 200>& tmrl,
                                   int theory_cap, bool hasTheory,
                                   BYTE r, BYTE gC, BYTE b) {
        if (censored <= 0 || censored > max_x) return;
        double y_value = -1.0;
        // 1. 优先 theory MRL (综合和 UP 都有)
        if (censored < (int)tmrl.size() && tmrl[censored] > 0
            && (theory_cap == 0 || censored <= theory_cap)) {
            y_value = tmrl[censored];
        }
        // 2. 降级经验 MRL
        if (y_value <= 0 && mrl_data.first[censored] > 0) {
            y_value = mrl_data.first[censored];
        }
        Gdiplus::Color color(255, r, gC, b);
        Gdiplus::Pen markPen(color, DPIScaleF(1.5f));
        // dash pattern 与理论 CDF/MRL 保持一致 (4/3),让所有虚线视觉风格统一
        Gdiplus::REAL dash[2] = { DPIScaleF(4.0f), DPIScaleF(3.0f) };
        markPen.SetDashPattern(dash, 2);
        // 两种曲线都没有可用 y 值时仍保留整高竖线, 让当前水位始终可见。
        const auto top = y_value > 0 ? getPt(censored, y_value) : getPt(censored, max_y);
        g.DrawLine(&markPen, top.X, top.Y, top.X, plotY + plotH);

        // 收集标签 (新格式: 单行, 用中点分隔; 右上角空间足够)
        wchar_t lbl[64];
        if (y_value > 0) {
            swprintf(lbl, 64, L"已垫 %d 抽 · 预期还需 %.1f", censored, y_value);
        } else if (hasTheory) {
            swprintf(lbl, 64, L"已垫 %d 抽 · 超出理论曲线范围", censored);
        } else {
            // 重映 / 回响的 UP 侧本就没有理论曲线 (调用方传空表), 不能说"超出"。
            swprintf(lbl, 64, L"已垫 %d 抽", censored);
        }
        censoredLabels.push_back({ std::wstring(lbl), color });
    };
    resolveAndDrawLine(censored_all, mrl_all, theory_mrl_all, theory_all_cap, !theory_cdf_all.empty(), 65, 140, 240);
    resolveAndDrawLine(censored_up,  mrl_up,  theory_mrl_up,  theory_up_cap,  !theory_cdf_up.empty(),  240, 80, 80);

    // (2) 在 plot 区域右上角内侧固定位置堆叠标签
    //     锚点右对齐, 行高约 14pt
    //
    //     图例改为水平横排后只占 rect.Y+12 那一行 (与 macOS/iOS 一致),
    //     不再下沉到绘图区,所以标签可以从 plotY+6 紧贴绘图区顶部起步。
    if (!censoredLabels.empty()) {
        Gdiplus::StringFormat fmtRight;
        fmtRight.SetAlignment(Gdiplus::StringAlignmentFar);     // 水平右对齐
        fmtRight.SetLineAlignment(Gdiplus::StringAlignmentNear); // 顶部对齐

        Gdiplus::SolidBrush whiteBr(Gdiplus::Color(255, 252, 253, 255));

        const float anchorX = plotX + plotW - DPIScaleF(6.0f);
        const float anchorY = plotY + DPIScaleF(6.0f);
        const float lineHeight = DPIScaleF(16.0f);

        for (size_t i = 0; i < censoredLabels.size(); ++i) {
            const auto& entry = censoredLabels[i];
            float ly = anchorY + (float)i * lineHeight;
            // 用一个点+右对齐 StringFormat 直接定位文字右上角
            Gdiplus::PointF pt(anchorX, ly);
            // 白色描边: 4 个对角偏移画白底字提升可读性
            for (int dx = -1; dx <= 1; dx += 2) {
                for (int dy = -1; dy <= 1; dy += 2) {
                    g.DrawString(entry.text.c_str(), -1, &tickFont,
                                 Gdiplus::PointF(pt.X + (float)dx, pt.Y + (float)dy),
                                 &fmtRight, &whiteBr);
                }
            }
            // 主文本
            Gdiplus::SolidBrush lblBrush(entry.color);
            g.DrawString(entry.text.c_str(), -1, &tickFont, pt, &fmtRight, &lblBrush);
        }
    }

    // ---- 图例 (3 项水平排列: 综合实线 / UP 实线 / 理论值虚线) ----
    // 与 macOS / iOS 端布局对齐 —— 标题旁同一行,从右向左排,
    // 这样图例完全位于标题区(rect.Y+12 行),不会下沉到绘图区(rect.Y+40 起)。
    Gdiplus::Font legendFont(&fontFamily, DPIScaleF(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush blueBr(Gdiplus::Color(255, 65, 140, 240));
    Gdiplus::SolidBrush redBr (Gdiplus::Color(255, 240, 80, 80));

    const wchar_t* legAll  = L"综合S级 剩余期望";
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

    // 第 3 项: 理论值 (虚线,最右)
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

    // 第 2 项: UP 剩余期望 (常驻频段 show_up=false 时整项跳过, 综合项左移补位)
    float xUpSw = xThySw;
    if (show_up) {
        float xUpText = xThySw - entryGap - wUp;
        xUpSw = xUpText - swatchGap - swatchW;
        g.FillRectangle(&redBr, xUpSw, legendY + swatchYOff - DPIScaleF(1.5f),
                        swatchW, DPIScaleF(3.0f));
        g.DrawString(legUp, -1, &legendFont,
                     Gdiplus::PointF(xUpText, legendY), &textBrush);
    }

    // 第 1 项: 综合剩余期望
    float xAllText = xUpSw - entryGap - wAll;
    float xAllSw   = xAllText - swatchGap - swatchW;
    g.FillRectangle(&blueBr, xAllSw, legendY + swatchYOff - DPIScaleF(1.5f),
                    swatchW, DPIScaleF(3.0f));
    g.DrawString(legAll, -1, &legendFont,
                 Gdiplus::PointF(xAllText, legendY), &textBrush);

    // 无出金时, 在绘图区中央叠加灰色提示 (v0.1.2.1)
    if (!hasData) {
        Gdiplus::Font hintFont(&fontFamily, DPIScaleF(13.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush hintBrush(Gdiplus::Color(200, 130, 130, 130));
        const wchar_t* hint = L"暂无出金数据 (仅显示理论曲线参考)";
        Gdiplus::RectF box;
        g.MeasureString(hint, -1, &hintFont, Gdiplus::PointF(0, 0), &box);
        g.DrawString(hint, -1, &hintFont,
                     Gdiplus::PointF(plotX + plotW * 0.5f - box.Width * 0.5f,
                                     plotY + plotH * 0.5f - box.Height * 0.5f),
                     &hintBrush);
    }
}

// ---------------------------------------------------------
// [响应式布局 + 垂直滚动]
//
// 五行图表的完整内容高约 1800px, 1080p 屏放不下; 而旧版窗口固定尺寸、不能缩放,
// 底部几行只能看不到。做法与 Endfield-Gacha 相同:
//   - 窗口可缩放 (WS_THICKFRAME | WS_MAXIMIZEBOX), 并加 WS_VSCROLL
//   - 所有子控件与图表的坐标由 ComputeLayout() 从客户区尺寸算出, 不再写死常量
//   - 内容总高 > 客户区高时可滚动。滚动【不重绘图表】: 图表位图按"内容坐标系"缓存
//     (高度 = contentH 而非客户区高), 滚动只改 BitBlt 的源点 Y 与子控件的 Y 偏移。
//   - 滚动条用 SIF_DISABLENOSCROLL 常驻显示 (内容装得下时变灰而非隐藏)。这样客户区
//     宽度不会因滚动条显隐而跳变, 避免 SetScrollInfo → WM_SIZE → SetScrollInfo 的
//     重入抖动。
//
// 坐标约定: 下面 ui:: 里的 k* 常量是【逻辑像素】, Layout 结构体里的字段是
//           【物理像素】(已过 DPIScale)。
// ---------------------------------------------------------
namespace ui {
    constexpr int kMarginX      = 20;   // 左右留白
    constexpr int kMarginBottom = 20;
    constexpr int kLabelW       = 110;  // "非UP S级代理人:" 比终末地的标签宽
    constexpr int kEditX        = 135;  // 标签右侧, 输入框起点
    constexpr int kHintY        = 15;
    constexpr int kRowY0        = 40;   // 第一行输入框 (非 UP 代理人名单)
    constexpr int kRowStep      = 30;   // 行距
    constexpr int kLabelDY      = 5;    // 标签相对同行输入框的垂直微调
    constexpr int kEditH        = 26;
    constexpr int kLabelH       = 20;
    constexpr int kOutY         = 105;  // 输出文本框顶端
    // 输出文本框高度 (5 个池块)。RichEdit 里一行实测约 23px, 全文 23 行约需 540px; 这里只给
    // 约 17 行, 末尾几行在框内滚动 —— 与终末地一样, 用框内滚动换取图表区更早露出。
    constexpr int kOutH         = 410;
    constexpr int kChartGapX    = 20;   // 同一行左右两图之间
    constexpr int kChartGapY    = 5;    // 上下两行之间
    constexpr int kChartMinRowH = 250;  // 单行图表最小高度
    constexpr int kChartRows    = 5;    // 独家 / 独家重映 / 音擎 / 音擎回响 / 常驻
    constexpr int kMinWindowW   = 900;  // 再窄图表就没法读了
    constexpr int kMinWindowH   = 360;
    constexpr int kScrollLine   = 30;   // 一次 SB_LINEUP/DOWN 的步长
}

struct Layout {
    int clientW = 0, clientH = 0;   // 客户区尺寸 (物理像素)
    int contentH = 0;               // 内容总高, 滚动范围的依据
    int editX = 0, editW = 0;       // 两个名单输入框
    int outY = 0, outH = 0, outW = 0;
    int chartX1 = 0, chartX2 = 0;   // 左列 / 右列 X
    int chartTop = 0, chartW = 0, chartRowH = 0;
    // 第 i 行 (0-based) 图表的顶端 Y, 内容坐标系
    int RowY(int i) const { return chartTop + i * (chartRowH + DPIScale(ui::kChartGapY)); }
};

Layout ComputeLayout(HWND hwnd) {
    Layout L;
    RECT rc; GetClientRect(hwnd, &rc);
    L.clientW = (int)rc.right; L.clientH = (int)rc.bottom;   // RECT 成员是 LONG, 显式收敛
    if (L.clientW <= 0 || L.clientH <= 0) return L;

    const int mx = DPIScale(ui::kMarginX);
    L.editX = DPIScale(ui::kEditX);
    // clamp 到一个正的最小值: 窗口被拖到极窄时 (WM_GETMINMAXINFO 之外仍可能发生,
    // 例如最小化/还原过程中的瞬时尺寸) 避免出现负宽度传给 MoveWindow/位图。
    L.editW = (std::max)(DPIScale(40), L.clientW - L.editX - mx);
    L.outW  = (std::max)(DPIScale(40), L.clientW - 2 * mx);
    L.outY  = DPIScale(ui::kOutY);
    L.outH  = DPIScale(ui::kOutH);

    L.chartTop = L.outY + L.outH + DPIScale(ui::kChartGapY);
    L.chartW   = (std::max)(DPIScale(80),
                            (L.clientW - 2 * mx - DPIScale(ui::kChartGapX)) / 2);
    L.chartX1  = mx;
    L.chartX2  = mx + L.chartW + DPIScale(ui::kChartGapX);

    // 行高: 客户区给 5 行图表剩多少。够高就平分, 不够就退回最小行高,
    // 内容溢出客户区 → 由滚动条兜住。
    const int gapsY = (ui::kChartRows - 1) * DPIScale(ui::kChartGapY);
    const int avail = L.clientH - L.chartTop - DPIScale(ui::kMarginBottom) - gapsY;
    L.chartRowH = (std::max)(DPIScale(ui::kChartMinRowH), avail / ui::kChartRows);

    L.contentH = L.chartTop + ui::kChartRows * L.chartRowH + gapsY + DPIScale(ui::kMarginBottom);
    // 平分时的整除误差会让 contentH 比 clientH 少几像素, 那样 WM_PAINT 会 BitBlt 到
    // 位图外边; 这里抬平到至少等于客户区高。
    if (L.contentH < L.clientH) L.contentH = L.clientH;
    return L;
}

// 按当前 g_scrollY 摆放全部子控件。子控件跟随内容一起滚动, 统一加 -g_scrollY 的 Y 偏移。
void LayoutChildren(HWND hwnd, const Layout& L) {
    (void)hwnd;                          // 摆位只用子窗口句柄, 父句柄留着保持接口对称
    if (!hAgentEdit) return;             // WM_CREATE 还没建完控件
    const int mx = DPIScale(ui::kMarginX);
    const int dy = -g_scrollY;

    // 先把 6 个控件的目标位置算齐, 再统一提交。分两步是为了让 DeferWindowPos 中途失败时
    // 能【整体】退回 MoveWindow —— 边算边 Defer 的话, 一旦失败, 之前已入队的那几个控件
    // 会随着 HDWP 作废而停在原位, 出现"一半控件跟着滚、一半不动"的错位。
    struct Place { HWND h; int x, y, w, hgt; };
    Place places[6];
    int n = 0;
    auto add = [&](HWND h, int x, int y, int w, int hgt) {
        if (h && n < 6) places[n++] = Place{h, x, y + dy, w, hgt};
    };
    add(hHintLabel, mx, DPIScale(ui::kHintY),
        (std::max)(DPIScale(100), L.clientW - 2 * mx), DPIScale(ui::kLabelH));
    for (int i = 0; i < 2; ++i) {
        const int rowY = DPIScale(ui::kRowY0 + i * ui::kRowStep);
        HWND lbl  = (i == 0) ? hAgentLabel : hWEngineLabel;
        HWND edit = (i == 0) ? hAgentEdit  : hWEngineEdit;
        add(lbl,  mx,      rowY + DPIScale(ui::kLabelDY), DPIScale(ui::kLabelW), DPIScale(ui::kLabelH));
        add(edit, L.editX, rowY,                          L.editW,               DPIScale(ui::kEditH));
    }
    add(hOutEdit, mx, L.outY, L.outW, L.outH);

    // 批量摆位, 减少缩放/滚动时的闪烁
    HDWP hdwp = BeginDeferWindowPos(n);
    for (int i = 0; i < n && hdwp; ++i) {
        hdwp = DeferWindowPos(hdwp, places[i].h, NULL,
                              places[i].x, places[i].y, places[i].w, places[i].hgt,
                              SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (hdwp) { EndDeferWindowPos(hdwp); return; }
    for (int i = 0; i < n; ++i)          // 兜底: 批量提交失败时逐个摆
        MoveWindow(places[i].h, places[i].x, places[i].y, places[i].w, places[i].hgt, TRUE);
}

void UpdateScrollInfo(HWND hwnd, const Layout& L) {
    g_contentH = L.contentH;
    SCROLLINFO si = { sizeof(si) };
    // SIF_DISABLENOSCROLL: 内容装得下时把滚动条置灰而不是隐藏, 客户区宽度因此恒定,
    // 不会触发 "设滚动条 → 客户区变窄 → WM_SIZE → 再设滚动条" 的重入。
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    si.nMin  = 0;
    si.nMax  = L.contentH - 1;
    si.nPage = (UINT)L.clientH;
    si.nPos  = g_scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    // 系统会按 range/page 夹紧 nPos, 读回来才是真实生效值
    si.fMask = SIF_POS;
    if (GetScrollInfo(hwnd, SB_VERT, &si)) g_scrollY = si.nPos;
}

// 滚动到指定位置。图表缓存按内容坐标系画好且与滚动无关, 所以这里【不】重建位图,
// 只重新摆子控件 + 触发一次 WM_PAINT 换 BitBlt 源点 —— 滚动因此是廉价操作。
void ScrollTo(HWND hwnd, int pos) {
    RECT rc; GetClientRect(hwnd, &rc);
    // RECT 成员是 LONG, 与 int 混用会让 std::max 的模板实参推导出现歧义 (MSVC C2672),
    // 故先显式收敛到 int 再比较。下面 ComputeLayout 里也是同样的处理。
    const int clientH = (int)rc.bottom;
    const int maxPos  = (std::max)(0, g_contentH - clientH);
    pos = (std::min)((std::max)(pos, 0), maxPos);
    if (pos == g_scrollY) return;
    g_scrollY = pos;
    SCROLLINFO si = { sizeof(si) };
    si.fMask = SIF_POS; si.nPos = g_scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    LayoutChildren(hwnd, ComputeLayout(hwnd));
    InvalidateRect(hwnd, NULL, FALSE);   // WM_ERASEBKGND 恒返回 1, 不需要擦除
}

void RebuildChartCache(HWND hwnd) {
    const Layout L = ComputeLayout(hwnd);
    // 位图不是"客户区大小", 而是【内容坐标系】的整幅高度 (contentH)。
    //   滚动时只换 BitBlt 的源点 Y, 不必重画; 只有尺寸变化才需要重建。
    //   代价是位图内存 = clientW × contentH × 4B (1260×1800 约 9MB; 4K 全屏下约 28MB),
    //   对本工具可接受, 换来的是滚动完全不掉帧。
    const int w = L.clientW, h = L.contentH;
    if (w <= 0 || h <= 0) return;
    RECT rc = {0, 0, w, h};

    HDC hdcWnd = GetDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdcWnd);
    if (g_hChartBmp) DeleteObject(g_hChartBmp);
    g_hChartBmp = CreateCompatibleBitmap(hdcWnd, w, h);
    // v0.1.3.3: DC / 位图创建失败 (极端低资源、超大窗口) 时早退。旧逻辑会把 NULL 选进
    // hdcMem 继续画 (落在默认 1x1 位图上, 不崩但全部白画); WM_PAINT 端本就有空检查,
    // 这里补对称守卫, 失败时保持 g_hChartBmp = NULL 走"无缓存"路径。
    if (!hdcMem || !g_hChartBmp) {
        if (g_hChartBmp) { DeleteObject(g_hChartBmp); g_hChartBmp = NULL; }
        if (hdcMem) DeleteDC(hdcMem);
        ReleaseDC(hwnd, hdcWnd);
        return;
    }

    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hChartBmp);
    FillRect(hdcMem, &rc, (HBRUSH)(COLOR_WINDOW + 1));

    {
        Gdiplus::Graphics g(hdcMem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // 布局全部由 ComputeLayout() 从客户区尺寸算出, 不再写死坐标:
        //   header inputs :  15 .. kOutY   (提示行 + 2 行"标签 + 名单输入框")
        //   output text   :  kOutY, 高 kOutH
        //   五行图表      :  L.RowY(0..4), 每行"左 ECDF / 右 MRL"
        //     row0 独家频段 / row1 独家重映频段 / row2 音擎频段 / row3 音擎回响频段 / row4 常驻频段
        //   复刻频段紧挨着对应的首发频段, 方便上下对照同一类卡池。
        //   每张图宽 = (客户区宽 - 两侧留白 - 中缝) / 2, 随窗口横向缩放;
        //   行高 = 剩余高度五等分, 但不低于 kChartMinRowH, 低于就靠滚动条查看。
        // 注意这里画的是【内容坐标系】(Y 不减 g_scrollY) —— 滚动发生在 WM_PAINT 的
        // BitBlt 源点上, 见 WM_PAINT / ScrollTo。

        // ===== 独家频段 (代理人 UP 池) =====
        // ECDF X 轴覆盖 UP 大保底间隔上限 180; 综合理论上限 90 (硬保底)
        DrawECDF  (g, Gdiplus::Rect(L.chartX1, L.RowY(0), L.chartW, L.chartRowH),
                   statsAgent,
                   g_cdf_agent, g_cdf_agent_up,
                   L"独家频段 (代理人 UP) 累积分布 (ECDF)", 180,
                   /*ecdf_up_step_size=*/1);
        DrawMRL   (g, Gdiplus::Rect(L.chartX2, L.RowY(0), L.chartW, L.chartRowH),
                   statsAgent.freq_all, statsAgent.freq_up,
                   statsAgent.count_all, statsAgent.count_up,
                   statsAgent.censored_pity_all, statsAgent.censored_pity_up,
                   g_cdf_agent, g_cdf_agent_up,
                   L"独家频段 (代理人 UP) 剩余抽数期望 (MRL)", 180,
                   /*theory_all_cap=*/90, /*theory_up_cap=*/180);

        // ===== 独家重映频段 (代理人复刻, 2.5 起) =====
        // 综合 S 级与独家频段同机制 (0.6% / 74 起软保底 / 90 硬保底), 复用 g_cdf_agent。
        // UP 侧【不画理论线】: 每期首个 S 必为 UP 的福利保底让"首 UP 间隔"不再服从
        // 50/50 + 大保底模型, 传空 span 后理论 UP 虚线与 UP K-S 标记自然消失,
        // 只保留经验 UP 阶梯 (DrawECDF 会在图内注明原因)。X 轴基数取综合硬保底 90;
        // 福利保底之后的 S 仍可能歪, UP 间隔可以超过 90, 这时 DrawECDF / DrawMRL
        // 会按观测到的最大间隔自动放宽 X 轴。
        DrawECDF  (g, Gdiplus::Rect(L.chartX1, L.RowY(1), L.chartW, L.chartRowH),
                   statsRescreening,
                   g_cdf_agent, std::span<const double>{},
                   L"独家重映频段 累积分布 (ECDF)", 90,
                   /*ecdf_up_step_size=*/1);
        DrawMRL   (g, Gdiplus::Rect(L.chartX2, L.RowY(1), L.chartW, L.chartRowH),
                   statsRescreening.freq_all, statsRescreening.freq_up,
                   statsRescreening.count_all, statsRescreening.count_up,
                   statsRescreening.censored_pity_all, statsRescreening.censored_pity_up,
                   g_cdf_agent, std::span<const double>{},
                   L"独家重映频段 剩余抽数期望 (MRL)", 90,
                   /*theory_all_cap=*/90, /*theory_up_cap=*/0);

        // ===== 音擎频段 (武器 UP 池) =====
        // ECDF X 轴覆盖 UP 大保底间隔上限 160; 综合理论上限 80 (硬保底)
        DrawECDF  (g, Gdiplus::Rect(L.chartX1, L.RowY(2), L.chartW, L.chartRowH),
                   statsWEngine,
                   g_cdf_wengine, g_cdf_wengine_up,
                   L"音擎频段 (武器 UP) 累积分布 (ECDF)", 160,
                   /*ecdf_up_step_size=*/1);
        DrawMRL   (g, Gdiplus::Rect(L.chartX2, L.RowY(2), L.chartW, L.chartRowH),
                   statsWEngine.freq_all, statsWEngine.freq_up,
                   statsWEngine.count_all, statsWEngine.count_up,
                   statsWEngine.censored_pity_all, statsWEngine.censored_pity_up,
                   g_cdf_wengine, g_cdf_wengine_up,
                   L"音擎频段 (武器 UP) 剩余抽数期望 (MRL)", 160,
                   /*theory_all_cap=*/80, /*theory_up_cap=*/160);

        // ===== 音擎回响频段 (音擎复刻, 2.5 起) =====
        // 与重映频段同理: 综合复用 g_cdf_wengine (1.0% / 65 起软保底 / 80 硬保底),
        // UP 侧因福利保底只画经验阶梯。
        DrawECDF  (g, Gdiplus::Rect(L.chartX1, L.RowY(3), L.chartW, L.chartRowH),
                   statsReverberation,
                   g_cdf_wengine, std::span<const double>{},
                   L"音擎回响频段 累积分布 (ECDF)", 80,
                   /*ecdf_up_step_size=*/1);
        DrawMRL   (g, Gdiplus::Rect(L.chartX2, L.RowY(3), L.chartW, L.chartRowH),
                   statsReverberation.freq_all, statsReverberation.freq_up,
                   statsReverberation.count_all, statsReverberation.count_up,
                   statsReverberation.censored_pity_all, statsReverberation.censored_pity_up,
                   g_cdf_wengine, std::span<const double>{},
                   L"音擎回响频段 剩余抽数期望 (MRL)", 80,
                   /*theory_all_cap=*/80, /*theory_up_cap=*/0);

        // ===== 常驻频段 (热门卡司) =====
        // 无 UP 概念: theory_cdf_up 传【空 span】(std::span<const double>{}) →
        // 红色 UP 系列 (理论虚线/经验阶梯/KS 标记/censored 标注) 全部自然消失;
        // show_up=false 仅隐藏 UP 图例文字。综合复用 g_cdf_agent (同分布)。
        DrawECDF  (g, Gdiplus::Rect(L.chartX1, L.RowY(4), L.chartW, L.chartRowH),
                   statsStandard,
                   g_cdf_agent, std::span<const double>{},
                   L"常驻频段 (热门卡司) 累积分布 (ECDF)", 90,
                   /*ecdf_up_step_size=*/1, /*show_up=*/false);
        DrawMRL   (g, Gdiplus::Rect(L.chartX2, L.RowY(4), L.chartW, L.chartRowH),
                   statsStandard.freq_all, statsStandard.freq_up,
                   statsStandard.count_all, statsStandard.count_up,
                   statsStandard.censored_pity_all, statsStandard.censored_pity_up,
                   g_cdf_agent, std::span<const double>{},
                   L"常驻频段 (热门卡司) 剩余抽数期望 (MRL)", 90,
                   /*theory_all_cap=*/90, /*theory_up_cap=*/0, /*show_up=*/false);
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
        hHintLabel = CreateWindowW(L"STATIC",
            L"S 级不在名单内即视为当期 UP。「名字@时间」表示从该时刻起才算非 UP (服务器时间),「名字@起~止」只在该时段内算; 重映/回响频段共用这两份名单。",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(15), DPIScale(1100), DPIScale(20), hwnd, NULL, NULL, NULL);
        // 两份名单的语义都是【会以"歪"的身份出现的 S 级】, 不是"常驻":
        //   - 前 6 项是常驻 (热门卡司) S 级, 一直都在独家/音擎频段的歪池里。
        //   - 3.1 (2026-07-29) 起「定制调频」让玩家自选歪池里的 S 级。可选范围 (官方调频详情
        //     gacha_info 的 s_group_customize_items) 在常驻 6 人之外多了 凯撒/柳/朱鸢,
        //     音擎多了 奔袭獠牙/时流贤者/防暴者Ⅵ型。它们以前都当过 UP —— 最后一次分别是
        //     2025-06/08/04 的独家/音擎频段, 此后再没当过 UP, 两期重映 (2026-01 爱丽丝/
        //     零号·安比/耀嘉音, 2026-08 琉音/柚叶/悠真) 也都不是它们。
        //     所以"3.1 之前抽到 = 当期 UP、之后抽到 = 歪"可以用一个起始时刻干净地切开。
        //   - 边界取 2026-07-28 18:00 (与 Starward 一致), 比 3.1 维护开始还早, 不会漏判。
        //   ★ 若它们将来又当 UP (多半是在重映/回响频段), 需要把那一期从时段里挖掉, 例如
        //     "柳@2026-07-28 18:00~2026-12-01 11:59, 柳@2026-12-22 12:00" (同名可写多段)。
        //   注意 防暴者Ⅵ型 的"Ⅵ"是罗马数字 U+2165, 不是字母 VI。
        hAgentLabel = CreateWindowW(L"STATIC", L"非UP S级代理人:",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(45), DPIScale(110), DPIScale(20), hwnd, NULL, NULL, NULL);
        hAgentEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"猫又,「11号」,珂蕾妲,莱卡恩,格莉丝,丽娜,"
            L"凯撒@2026-07-28 18:00,柳@2026-07-28 18:00,朱鸢@2026-07-28 18:00",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            DPIScale(135), DPIScale(40), DPIScale(1105), DPIScale(26), hwnd, NULL, NULL, NULL);
        hWEngineLabel = CreateWindowW(L"STATIC", L"非UP S级音擎:",
            WS_CHILD | WS_VISIBLE,
            DPIScale(20), DPIScale(75), DPIScale(110), DPIScale(20), hwnd, NULL, NULL, NULL);
        hWEngineEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"钢铁肉垫,硫磺石,燃狱齿轮,拘缚者,嵌合编译器,啜泣摇篮,"
            L"奔袭獠牙@2026-07-28 18:00,时流贤者@2026-07-28 18:00,防暴者\x2165型@2026-07-28 18:00",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            DPIScale(135), DPIScale(70), DPIScale(1105), DPIScale(26), hwnd, NULL, NULL, NULL);

        hOutEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"RichEdit50W",
            L"等待拖入文件...",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
            DPIScale(20), DPIScale(105), DPIScale(1220), DPIScale(410), hwnd, NULL, NULL, NULL);

        DWORD tabStops[] = {50};
        SendMessage(hOutEdit, EM_SETTABSTOPS, 1, (LPARAM)tabStops);
        SendMessage(hOutEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)GetSysColor(COLOR_3DFACE));

        for (HWND h : {hHintLabel, hAgentLabel, hAgentEdit, hWEngineLabel, hWEngineEdit, hOutEdit})
            SendMessage(h, WM_SETFONT, (WPARAM)hFont, TRUE);

        // 上面 CreateWindow 里那些坐标只是占位 —— 真正的位置一律由 LayoutChildren()
        // 按当前客户区尺寸决定, 窗口每次缩放/滚动都会重算。
        {
            const Layout L = ComputeLayout(hwnd);
            UpdateScrollInfo(hwnd, L);
            LayoutChildren(hwnd, L);
        }
        RebuildChartCache(hwnd);
        break;
    }
    // 窗口可缩放: 尺寸一变就要重排控件 + 按新宽高重画图表。
    //   图表行高/宽度都与客户区尺寸相关, 所以这里必须重建位图, 不能只 Invalidate。
    //   重建 = 10 张 GDI+ 图重画, 拖动边框时每帧都会跑一次; 实测若觉得卡, 可以改成
    //   WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE 期间只重排控件、松手后再重建图表。
    case WM_SIZE: {
        if (wParam == SIZE_MINIMIZED) break;      // 最小化时客户区为 0, 没必要折腾
        const Layout L = ComputeLayout(hwnd);
        if (L.clientW <= 0 || L.clientH <= 0) break;
        // WM_SIZE 会以【相同尺寸】重复到达 (例如 SetScrollInfo 改变滚动条状态之后),
        // 尺寸没变就没必要重画 10 张图。
        static int s_lastW = -1, s_lastH = -1;
        const bool sizeChanged = (L.clientW != s_lastW || L.clientH != s_lastH);
        s_lastW = L.clientW; s_lastH = L.clientH;

        const int maxScroll = (std::max)(0, L.contentH - L.clientH);
        if (g_scrollY > maxScroll) g_scrollY = maxScroll;   // 窗口变高时把内容拉回来
        UpdateScrollInfo(hwnd, L);
        LayoutChildren(hwnd, L);
        if (sizeChanged) RebuildChartCache(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    // 限制最小尺寸: 再小图表就没有可读性了, 且极端窄高会让布局退化。
    // g_dpi 在 CreateWindowW 之前就已设置好, 这里可以安全使用。
    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = DPIScale(ui::kMinWindowW);
        mmi->ptMinTrackSize.y = DPIScale(ui::kMinWindowH);
        break;
    }
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si) };
        si.fMask = SIF_ALL;
        if (!GetScrollInfo(hwnd, SB_VERT, &si)) break;
        int pos = si.nPos;
        const int line = DPIScale(ui::kScrollLine);
        switch (LOWORD(wParam)) {
        case SB_TOP:           pos  = si.nMin;          break;
        case SB_BOTTOM:        pos  = si.nMax;          break;
        case SB_LINEUP:        pos -= line;             break;
        case SB_LINEDOWN:      pos += line;             break;
        case SB_PAGEUP:        pos -= (int)si.nPage;    break;
        case SB_PAGEDOWN:      pos += (int)si.nPage;    break;
        // THUMBTRACK 用 nTrackPos 才能跟手 (nPos 在拖动过程中不更新)
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos  = si.nTrackPos;     break;
        default: break;
        }
        ScrollTo(hwnd, pos);
        break;
    }
    // 滚轮: 一格滚三"行"。鼠标停在输出文本框上时消息会先给那个 RichEdit
    // (它自带 WS_VSCROLL), 由它滚自己的内容; 停在图表区才滚整个窗口。
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        ScrollTo(hwnd, g_scrollY - delta * DPIScale(ui::kScrollLine) * 3 / WHEEL_DELTA);
        break;
    }
    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        wchar_t filePath[MAX_PATH];
        DragQueryFileW(hDrop, 0, filePath, MAX_PATH);
        DragFinish(hDrop);
        // 异步提交;Submit 内部做双开保护(g_processing CAS 锁)
        // 失败(已有 worker 在跑或 I/O 失败)时静默忽略,UI 上保留之前的状态
        ProcessFile_Submit(hwnd, filePath);
        break;
    }
    case WM_APP_PROCESS_DONE: {
        // worker 完成,主线程消费结果(更新全局 stats、刷新 UI)
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
            // 位图是内容坐标系的整幅图, 这里按当前滚动量偏移源点 Y 取一片。
            //   滚动之所以廉价就在于此 —— 只是换个源点, 不重画任何图表。
            BitBlt(hdc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top,
                   hdcMem, ps.rcPaint.left, ps.rcPaint.top + g_scrollY, SRCCOPY);
            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
        }
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: {
        // v0.1.3.3: 退出前先 join worker。此刻 hwnd 仍有效, worker 的 PostMessageW 仍能
        // 成功入队; worker 从不 SendMessage 回主线程 (只用非阻塞 PostMessage), 故这里等待
        // 不会死锁。等待时长 = 剩余分析时间 (本工具数据量下为毫秒级)。
        // 不等待的后果: WinMain 返回 → ExitProcess 直接终止 worker, 其若恰好持有 CRT 堆锁,
        // 退出流程可能挂死 —— 这才是真实风险 (旧注释"OS 统一回收、无实际危害"不成立)。
        if (g_hWorker) {
            WaitForSingleObject(g_hWorker, INFINITE);
            CloseHandle(g_hWorker);
            g_hWorker = NULL;
        }
        // join 之后, 若 worker 投递过结果, 该消息还躺在线程队列里且永远不会被派发
        // (窗口即将销毁) —— 在此 reap 并释放载荷, 否则 ProcessOutput + mmap 句柄泄漏到
        // 进程退出。与 Consume 不会双重 delete: 消息要么已派发 (队列里没有), 要么在队列
        // 里 (未派发), 二者互斥; reap 只删队列里取出的那份。
        MSG pending;
        while (PeekMessageW(&pending, hwnd, WM_APP_PROCESS_DONE, WM_APP_PROCESS_DONE, PM_REMOVE)) {
            delete reinterpret_cast<ProcessOutput*>(pending.lParam);
        }
        InterlockedExchange(&g_processing, 0);
        if (g_hChartBmp) { DeleteObject(g_hChartBmp); g_hChartBmp = NULL; }
        if (hFont) DeleteObject(hFont);
        PostQuitMessage(0);
        break;
    }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // v0.1.3.3: RichEdit50W 依赖 Msftedit.dll。消费级 Windows (含 N 版) 均随系统分发,
    // 仅 WinPE / 深度精简系统可能缺失 —— 旧版忽略返回值, 缺失时四个 RichEdit 创建为 NULL,
    // 后续 SendMessage / SetWindowText 全部静默落空, 界面空白且无任何提示。现显式报错退出。
    if (!LoadLibraryW(L"Msftedit.dll")) {
        MessageBoxW(NULL,
                    L"无法加载 Msftedit.dll (RichEdit 控件库)。\n"
                    L"本程序的文本界面依赖该系统组件, 请在完整版 Windows 上运行。",
                    L"组件缺失", MB_OK | MB_ICONERROR);
        return 1;
    }
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

    // 窗口【可缩放 + 可最大化 + 带垂直滚动条】。
    //   旧版为保证图表布局不被破坏, 特意去掉了 WS_THICKFRAME / WS_MAXIMIZEBOX, 代价是
    //   窗口高度必须能装下全部内容; 加入重映 / 回响两行图表后需要约 1800px,
    //   1080p 屏上底部会被截掉且无法滚动查看。现在坐标全部由 ComputeLayout() 动态算出,
    //   缩放安全; 装不下就滚动。
    DWORD dwStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL;
    // 初始窗口尺寸 (之后用户可以随意拖动):
    //   宽度: 1260, 左右留白对称 (内容右边界 x=1240, 左留白 20 → 右留白也 20)。
    //   高度: 取 5 行图表都不压缩时的完整内容高 (1810), 但不超过工作区高度
    //         的 92% —— 1440p 以上开箱即见全部内容, 1080p 上则开成能放下的高度,
    //         底部内容用滚动条查看, 不会一启动就有一半在屏幕外。
    RECT wa = {0};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const int waH = (std::max)(DPIScale(ui::kMinWindowH), (int)((wa.bottom - wa.top) * 92 / 100));
    RECT rect = {0, 0, DPIScale(1260), (std::min)(DPIScale(1810), waH)};
    AdjustWindowRectEx(&rect, dwStyle, FALSE, 0);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"绝区零抽卡记录分析与可视化",
        dwStyle, CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, hInstance, NULL);
    ShowWindow(hwnd, nCmdShow);

    MSG msg;
    // v0.1.3.3: GetMessage 的返回值是三态 (>0 取到消息 / 0 收到 WM_QUIT / -1 出错),
    // 旧写法 while (GetMessage(...)) 把 -1 当真值 —— 出错时 msg 内容未定义, 循环会带着
    // 无效消息空转。当前参数 (hwnd=NULL 过滤 + 有效指针) 下 -1 实际不可达, 属防御性修正;
    // 顺带显式用 W 版 (GetMessageW / DispatchMessageW), 与本文件全 W 系窗口代码一致,
    // 不再依赖 UNICODE 宏决定 A/W。
    for (;;) {
        BOOL ret = GetMessageW(&msg, NULL, 0, 0);
        if (ret == 0)  break;   // WM_QUIT: 正常退出
        if (ret == -1) break;   // 错误: msg 未定义, 不可 Dispatch, 直接进入清理
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
