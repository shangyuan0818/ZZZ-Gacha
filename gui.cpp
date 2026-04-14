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
#include <string_view>
#include <charconv>
#include <ranges>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comctl32.lib")

// ---------------------------------------------------------
// [极简 JSON 模块 - 零分配纯净版 + 内存映射支持]
// ---------------------------------------------------------
size_t FindJsonKey(std::string_view source, std::string_view key, size_t startPos = 0) {
    while (true) {
        size_t pos = source.find(key, startPos);
        if (pos == std::string_view::npos) return std::string_view::npos;
        if (pos > 0 && source[pos - 1] == '"' && 
            (pos + key.length() < source.length()) && source[pos + key.length()] == '"')
            return pos - 1;
        startPos = pos + key.length();
    }
}
std::string_view ExtractJsonValue(std::string_view source, std::string_view key, bool isString) {
    size_t pos = FindJsonKey(source, key);
    if (pos == std::string_view::npos) return {};
    pos = source.find(':', pos + key.length() + 2);
    if (pos == std::string_view::npos) return {};
    pos++;
    while (pos < source.length() && (source[pos]==' '||source[pos]=='\t'||source[pos]=='\n'||source[pos]=='\r')) pos++;
    if (isString) {
        if (pos >= source.length() || source[pos] != '"') return {};
        pos++; auto endPos = pos;
        while (endPos < source.length() && source[endPos] != '"') { if (source[endPos]=='\\') endPos++; endPos++; }
        return (endPos < source.length()) ? source.substr(pos, endPos-pos) : std::string_view{};
    } else {
        auto endPos = pos;
        while (endPos < source.length() && source[endPos]!=','&&source[endPos]!='}'&&source[endPos]!=']'&&source[endPos]!=' '&&source[endPos]!='\n'&&source[endPos]!='\r') endPos++;
        return source.substr(pos, endPos-pos);
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
    int depth = 0; size_t objStart = 0;
    for (size_t i = pos; i < source.length(); ++i) {
        char c = source[i];
        if (c == '"') { for (++i; i < source.length(); ++i) { if (source[i]=='\\'){++i;continue;} if (source[i]=='"')break; } continue; }
        if (c == '{') { if (depth==0) objStart=i; depth++; }
        else if (c == '}') { depth--; if (depth==0) cb(source.substr(objStart, i-objStart+1)); }
        else if (c == ']' && depth == 0) break;
    }
}

int g_dpi = 96;
int DPIScale(int value) { return MulDiv(value, g_dpi, 96); }
float DPIScaleF(float value) { return value * (g_dpi / 96.0f); }

std::wstring Utf8ToWstring(std::string_view str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
    return result;
}

struct Pull {
    std::wstring name, item_type, rank_type, gacha_type;
    long long id;
};

struct Stats {
    std::vector<int> all_pities, up_pities, up_win_pities;  
    double avg_all = 0.0, avg_up = 0.0, avg_win = -1.0; 
    std::unordered_map<int, int> freq_all, freq_up;
    double std_all = 0.0, std_up = 0.0, cv_all = 0.0, cv_up = 0.0;
    double ci_all_err = 0.0, ci_up_err = 0.0;
    int win_5050 = 0, lose_5050 = 0;
    double win_rate_5050 = -1.0;
    int max_pity_all = 0, max_pity_up = 0;
    std::vector<double> hazard_all, hazard_up;
    double ks_d_all = 0.0;
    bool ks_is_normal = true; 
};

Stats statsAgent, statsWEngine;
HWND hOutEdit, hCharEdit, hWepEdit;
static HBITMAP g_hChartBmp = NULL;

std::unordered_set<std::wstring> ParseCommaSeparated(const std::wstring& text) {
    std::unordered_set<std::wstring> result; std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L'\uFF0C') {
            cur.erase(0, cur.find_first_not_of(L" \t\r\n")); 
            if (!cur.empty()) cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
            if (!cur.empty()) result.insert(cur);
            cur.clear();
        } else cur += c;
    }
    cur.erase(0, cur.find_first_not_of(L" \t\r\n")); 
    if (!cur.empty()) cur.erase(cur.find_last_not_of(L" \t\r\n") + 1);
    if (!cur.empty()) result.insert(cur);
    return result;
}

// -------------------------------------------------------
// 绝区零理论 CDF 预计算表 (启动时一次性生成)
// 独家频段 (90-pity): base 0.6%, 第73抽起软保底 +6%/抽, 第90抽100%
// 音擎频段 (80-pity): base 0.8%, 第63抽起软保底 +7%/抽, 第80抽100%
// -------------------------------------------------------
static double g_cdf_agent[91] = {};
static double g_cdf_wengine[81] = {};

void InitCDFTables() {
    double surv = 1.0;
    for (int i = 1; i <= 90; ++i) {
        double p = (i <= 72) ? 0.006 : (i <= 89) ? 0.006 + (i - 72) * 0.06 : 1.0;
        if (p > 1.0) p = 1.0;
        g_cdf_agent[i] = g_cdf_agent[i-1] + surv * p;
        surv *= (1.0 - p);
    }
    surv = 1.0;
    for (int i = 1; i <= 80; ++i) {
        double p = (i <= 62) ? 0.008 : (i <= 79) ? 0.008 + (i - 62) * 0.07 : 1.0;
        if (p > 1.0) p = 1.0;
        g_cdf_wengine[i] = g_cdf_wengine[i-1] + surv * p;
        surv *= (1.0 - p);
    }
}

double ComputeKS(const std::unordered_map<int, int>& freq, int max_pity, int n, const double* cdf_table, int cdf_len) {
    if (n == 0) return 0.0;
    double max_d = 0.0; int cum_count = 0;
    for (int x = 1; x <= max_pity; ++x) {
        auto it = freq.find(x); int count_x = (it != freq.end()) ? it->second : 0;
        double f_val = (x < cdf_len) ? cdf_table[x] : 1.0;
        double fn_before = (double)cum_count / n; cum_count += count_x;
        double fn_after  = (double)cum_count / n;
        double d1 = std::abs(fn_before - f_val), d2 = std::abs(fn_after - f_val);
        if (d1 > max_d) max_d = d1;
        if (d2 > max_d) max_d = d2;
    }
    return max_d;
}

// -------------------------------------------------------
// 绝区零调频规则:
//   独家频段(gacha_type=2): 90抽保底S级代理人, 50/50歪了下次必UP
//   音擎频段(gacha_type=3): 80抽保底S级音擎, 75/25歪了下次必UP
//   邦布频段(gacha_type=5): 80抽保底, 必出选中的UP邦布
//   常驻频段(gacha_type=1): 90抽保底, 混合池
//
//   targetGachaType: L"2" = 独家频段, L"3" = 音擎频段
// -------------------------------------------------------
Stats Calculate(const std::vector<Pull>& pulls, const std::wstring& targetGachaType,
                const std::unordered_set<std::wstring>& standard_names) {
    Stats s;
    int current_pity = 0, pity_since_last_up = 0;
    bool had_non_up = false;
    long long sum_all = 0, sum_sq_all = 0, sum_up = 0, sum_sq_up = 0, sum_win = 0;
    
    for (const auto& p : pulls) {
        // ZZZ: 按 gacha_type 过滤 (不按 item_type，因为池内所有抽都计入 pity)
        if (p.gacha_type != targetGachaType) continue;
        
        current_pity++; pity_since_last_up++;
        
        if (p.rank_type == L"4") { // S级
            s.all_pities.push_back(current_pity); s.freq_all[current_pity]++;
            if (current_pity > s.max_pity_all) s.max_pity_all = current_pity;
            sum_all += current_pity; sum_sq_all += (long long)current_pity * current_pity;
            
            // UP 判定: 不在常驻名单中 = 限定UP
            bool isUP = !standard_names.contains(p.name);
            
            if (isUP) {
                s.up_pities.push_back(pity_since_last_up); s.freq_up[pity_since_last_up]++;
                if (pity_since_last_up > s.max_pity_up) s.max_pity_up = pity_since_last_up;
                sum_up += pity_since_last_up; sum_sq_up += (long long)pity_since_last_up * pity_since_last_up;
                if (!had_non_up) { s.up_win_pities.push_back(current_pity); s.win_5050++; sum_win += current_pity; }
                had_non_up = false; pity_since_last_up = 0;
            } else {
                if (!had_non_up) s.lose_5050++;
                had_non_up = true;
            }
            current_pity = 0;
        }
    }
    
    size_t na = s.all_pities.size(), nu = s.up_pities.size(), nw = s.up_win_pities.size();
    bool isWEngine = (targetGachaType == L"3");
    
    if (na > 0) {
        s.avg_all = (double)sum_all / na;
        double var = (double)sum_sq_all / na - s.avg_all * s.avg_all;
        s.std_all = std::sqrt(var > 0 ? var : 0);
        s.cv_all = (s.avg_all > 0) ? s.std_all / s.avg_all : 0;
        s.ci_all_err = 1.96 * s.std_all / std::sqrt((double)na);
        
        s.hazard_all.resize(s.max_pity_all + 1, 0.0);
        int survivors = (int)na;
        for (int x = 1; x <= s.max_pity_all; ++x) {
            auto it = s.freq_all.find(x); int cnt = (it != s.freq_all.end()) ? it->second : 0;
            if (survivors > 0) s.hazard_all[x] = (double)cnt / survivors;
            survivors -= cnt;
        }
        
        const double* cdf_tbl = isWEngine ? g_cdf_wengine : g_cdf_agent;
        int cdf_len = isWEngine ? 81 : 91;
        s.ks_d_all = ComputeKS(s.freq_all, s.max_pity_all, (int)na, cdf_tbl, cdf_len);
        s.ks_is_normal = (s.ks_d_all <= 1.36 / std::sqrt((double)na));
    }
    
    if (nu > 0) {
        s.avg_up = (double)sum_up / nu;
        double var = (double)sum_sq_up / nu - s.avg_up * s.avg_up;
        s.std_up = std::sqrt(var > 0 ? var : 0);
        s.cv_up = (s.avg_up > 0) ? s.std_up / s.avg_up : 0;
        s.ci_up_err = 1.96 * s.std_up / std::sqrt((double)nu);
        
        s.hazard_up.resize(s.max_pity_up + 1, 0.0);
        int survivors = (int)nu;
        for (int x = 1; x <= s.max_pity_up; ++x) {
            auto it = s.freq_up.find(x); int cnt = (it != s.freq_up.end()) ? it->second : 0;
            if (survivors > 0) s.hazard_up[x] = (double)cnt / survivors;
            survivors -= cnt;
        }
    }
    
    if (nw > 0) s.avg_win = (double)sum_win / nw;
    if (s.win_5050 + s.lose_5050 > 0) s.win_rate_5050 = (double)s.win_5050 / (s.win_5050 + s.lose_5050);
    return s;
}

void ProcessFile(const std::wstring& path) {
    wchar_t charBuf[1024]; GetWindowTextW(hCharEdit, charBuf, 1024);
    auto stdAgents = ParseCommaSeparated(charBuf);
    
    wchar_t wepBuf[4096]; GetWindowTextW(hWepEdit, wepBuf, 4096);
    auto stdWEngines = ParseCommaSeparated(wepBuf);

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD fileSize = GetFileSize(hFile, NULL);
    if (fileSize == INVALID_FILE_SIZE || fileSize == 0) { CloseHandle(hFile); return; }
    HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!hMap) { CloseHandle(hFile); return; }
    const char* mapData = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (!mapData) { CloseHandle(hMap); CloseHandle(hFile); return; }

    std::string_view bufferView(mapData, fileSize);
    if (bufferView.size() >= 3 && (unsigned char)bufferView[0]==0xEF && (unsigned char)bufferView[1]==0xBB && (unsigned char)bufferView[2]==0xBF)
        bufferView.remove_prefix(3);
    
    std::vector<Pull> pulls; pulls.reserve(2000);

    // UIGF v4.2: data 在 nap[0].list 里, ForEachJsonObject 搜索 "list" 即可找到
    ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
        Pull p;
        p.name = Utf8ToWstring(ExtractJsonValue(itemStr, "name", true));
        p.item_type = Utf8ToWstring(ExtractJsonValue(itemStr, "item_type", true));
        p.rank_type = Utf8ToWstring(ExtractJsonValue(itemStr, "rank_type", true));
        p.gacha_type = Utf8ToWstring(ExtractJsonValue(itemStr, "gacha_type", true));
        
        auto idStr = ExtractJsonValue(itemStr, "id", true);
        if (idStr.empty()) idStr = ExtractJsonValue(itemStr, "id", false);
        long long pid = 0;
        if (!idStr.empty()) std::from_chars(idStr.data(), idStr.data() + idStr.size(), pid);
        p.id = pid;
        pulls.push_back(std::move(p));
    });
    
    UnmapViewOfFile(mapData); CloseHandle(hMap); CloseHandle(hFile);
    if (pulls.empty()) { SetWindowTextW(hOutEdit, L"JSON 解析失败或无数据。"); return; }
    
    // ZZZ 的 id 本身就是时间序，直接排序
    std::ranges::sort(pulls, {}, &Pull::id);
    
    // 独家频段(gacha_type=2): S级代理人, 用代理人常驻名单判定UP
    statsAgent = Calculate(pulls, L"2", stdAgents);
    // 音擎频段(gacha_type=3): S级音擎, 用音擎常驻名单判定UP
    statsWEngine = Calculate(pulls, L"3", stdWEngines);
    
    wchar_t winA[64] = L"[无数据]", winW[64] = L"[无数据]";
    if (statsAgent.avg_win >= 0) swprintf(winA, 64, L"%.2f 抽", statsAgent.avg_win);
    if (statsWEngine.avg_win >= 0) swprintf(winW, 64, L"%.2f 抽", statsWEngine.avg_win);

    wchar_t outMsg[2048];
    swprintf(outMsg, 2048,
        L"【独家频段 (限定代理人)】 总计S级: %zu | 出当期 UP: %zu\r\n"
        L" ▶ 综合S级 (含歪) 出货平均期望:     %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S D: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期UP的综合平均期望:         %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (%d胜%d负)\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls\r\n\r\n"
        L"【音擎频段 (限定音擎)】 总计S级: %zu | 出当期 UP: %zu\r\n"
        L" ▶ 综合S级 (含歪) 出货平均期望:     %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   波动率 (CV): %5.1f%%\t[K-S D: %.3f (%ls)]\r\n"
        L" ▶ 抽到当期UP的综合平均期望:         %5.2f 抽   [95%% CI: %5.1f ~ %5.1f]    |   真实不歪率: %5.1f%% (%d胜%d负)\r\n"
        L" ▶ 赢下小保底 (不歪) 的出货期望:     %ls",
        statsAgent.all_pities.size(), statsAgent.up_pities.size(),
        statsAgent.avg_all, (std::max)(1.0, statsAgent.avg_all-statsAgent.ci_all_err), statsAgent.avg_all+statsAgent.ci_all_err, statsAgent.cv_all*100.0, statsAgent.ks_d_all, (statsAgent.all_pities.empty()?L"-":(statsAgent.ks_is_normal?L"符合理论模型":L"拒绝原假设: 偏离过大")),
        statsAgent.avg_up, (std::max)(1.0, statsAgent.avg_up-statsAgent.ci_up_err), statsAgent.avg_up+statsAgent.ci_up_err, statsAgent.win_rate_5050>=0?statsAgent.win_rate_5050*100.0:0.0, statsAgent.win_5050, statsAgent.lose_5050, winA,
        statsWEngine.all_pities.size(), statsWEngine.up_pities.size(),
        statsWEngine.avg_all, (std::max)(1.0, statsWEngine.avg_all-statsWEngine.ci_all_err), statsWEngine.avg_all+statsWEngine.ci_all_err, statsWEngine.cv_all*100.0, statsWEngine.ks_d_all, (statsWEngine.all_pities.empty()?L"-":(statsWEngine.ks_is_normal?L"符合理论模型":L"拒绝原假设: 偏离过大")),
        statsWEngine.avg_up, (std::max)(1.0, statsWEngine.avg_up-statsWEngine.ci_up_err), statsWEngine.avg_up+statsWEngine.ci_up_err, statsWEngine.win_rate_5050>=0?statsWEngine.win_rate_5050*100.0:0.0, statsWEngine.win_5050, statsWEngine.lose_5050, winW
    );
    SetWindowTextW(hOutEdit, outMsg);
}

// -------------------------------------------------------
// 绘图函数 (与终末地版逻辑一致, 仅更新标签)
// -------------------------------------------------------
void DrawKDE(Gdiplus::Graphics& g, Gdiplus::Rect rect, const std::unordered_map<int,int>& freq_all, const std::unordered_map<int,int>& freq_up, const std::wstring& title, int limit_base) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 252, 253, 255)); g.FillRectangle(&bgBrush, rect);
    Gdiplus::FontFamily ff(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&ff, DPIScaleF(15.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 40, 40, 40));
    g.DrawString(title.c_str(), -1, &titleFont, Gdiplus::PointF((float)rect.X+DPIScaleF(15.0f),(float)rect.Y+DPIScaleF(12.0f)), &textBrush);
    if (freq_all.empty() && freq_up.empty()) { Gdiplus::Font ef(&ff, DPIScaleF(14.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel); Gdiplus::SolidBrush eb(Gdiplus::Color(255,150,150,150)); g.DrawString(L"暂无数据",-1,&ef,Gdiplus::PointF((float)rect.X+(float)rect.Width/2.0f-DPIScaleF(30.0f),(float)rect.Y+(float)rect.Height/2.0f),&eb); return; }

    int max_x = limit_base;
    for (auto&[v,c]:freq_all) if(v>max_x) max_x=v;
    for (auto&[v,c]:freq_up) if(v>max_x) max_x=v;
    max_x = ((max_x/10)+1)*10;
    double bandwidth = 4.0;
    auto calcKDE = [&](const std::unordered_map<int,int>& freqs) {
        std::vector<double> curve(max_x+1, 0.0); int total=0; for(auto&[v,c]:freqs) total+=c; if(!total) return curve;
        int spread=(int)(4.0*bandwidth)+1;
        for(auto&[v,c]:freqs){int lo=(std::max)(1,v-spread),hi=(std::min)(max_x,v+spread); for(int x=lo;x<=hi;++x){double u=(x-v)/bandwidth; curve[x]+=c*std::exp(-0.5*u*u);}}
        double inv=1.0/total; for(int x=1;x<=max_x;++x) curve[x]*=inv; return curve;
    };
    auto kde_all=calcKDE(freq_all), kde_up=calcKDE(freq_up);
    double max_y=0.0001; for(double v:kde_all) max_y=(std::max)(max_y,v); for(double v:kde_up) max_y=(std::max)(max_y,v); max_y*=1.25;

    float plotX=(float)rect.X+DPIScaleF(55.0f),plotY=(float)rect.Y+DPIScaleF(45.0f),plotW=(float)rect.Width-DPIScaleF(85.0f),plotH=(float)rect.Height-DPIScaleF(75.0f);
    Gdiplus::Pen axisPen(Gdiplus::Color(255,150,150,150),DPIScaleF(1.5f)); Gdiplus::Pen gridPen(Gdiplus::Color(255,235,235,235),DPIScaleF(1.0f));
    g.DrawLine(&axisPen,plotX,plotY+plotH,plotX+plotW,plotY+plotH); g.DrawLine(&axisPen,plotX,plotY,plotX,plotY+plotH);
    auto getPt=[&](int x,double y){return Gdiplus::PointF(plotX+(float)x/(float)max_x*plotW,plotY+plotH-(float)(y/max_y)*plotH);};
    Gdiplus::Font tickFont(&ff,DPIScaleF(11.0f),Gdiplus::FontStyleRegular,Gdiplus::UnitPixel); Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255,120,120,120));
    for(int i=0;i<=4;++i){float py=plotY+plotH-(float)i/4.0f*plotH; if(i>0) g.DrawLine(&gridPen,plotX,py,plotX+plotW,py); g.DrawLine(&axisPen,plotX-DPIScaleF(5.0f),py,plotX,py); wchar_t yl[32]; swprintf(yl,32,L"%.1f%%",(max_y/4.0)*i*100.0); float lw=(float)wcslen(yl)*DPIScaleF(5.5f)+DPIScaleF(8.0f); g.DrawString(yl,-1,&tickFont,Gdiplus::PointF(plotX-lw,py-DPIScaleF(6.0f)),&tickBrush);}
    int step=(max_x>140)?20:10; for(int x=0;x<=max_x;x+=step){float px=plotX+(float)x/(float)max_x*plotW; g.DrawLine(&axisPen,px,plotY+plotH,px,plotY+plotH+DPIScaleF(5.0f)); wchar_t xl[16]; swprintf(xl,16,L"%d",x); float xo=(x<10?4.0f:x<100?8.0f:12.0f)*DPIScaleF(1.0f); g.DrawString(xl,-1,&tickFont,Gdiplus::PointF(px-xo,plotY+plotH+DPIScaleF(8.0f)),&tickBrush);}
    if(!freq_all.empty()){std::vector<Gdiplus::PointF> pts; pts.push_back(getPt(0,0)); for(int x=1;x<=max_x;x++) pts.push_back(getPt(x,kde_all[x])); Gdiplus::Pen pen(Gdiplus::Color(255,65,140,240),DPIScaleF(2.5f)); g.DrawCurve(&pen,pts.data(),(int)pts.size(),0.3f);}
    if(!freq_up.empty()){std::vector<Gdiplus::PointF> pts; pts.push_back(getPt(0,0)); for(int x=1;x<=max_x;x++) pts.push_back(getPt(x,kde_up[x])); Gdiplus::Pen pen(Gdiplus::Color(255,240,80,80),DPIScaleF(2.5f)); g.DrawCurve(&pen,pts.data(),(int)pts.size(),0.3f);}
    Gdiplus::SolidBrush bb(Gdiplus::Color(255,65,140,240)),rb(Gdiplus::Color(255,240,80,80)); float lx=(float)rect.X+(float)rect.Width-DPIScaleF(190.0f);
    g.DrawString(L"\x2501\x2501 \x7EFC\x5408S\x7EA7 (\x542B\x6B6A)",-1,&tickFont,Gdiplus::PointF(lx,(float)rect.Y+DPIScaleF(15.0f)),&bb);
    g.DrawString(L"\x2501\x2501 \x5F53\x671FUP",-1,&tickFont,Gdiplus::PointF(lx,(float)rect.Y+DPIScaleF(35.0f)),&rb);
}

void DrawHazard(Gdiplus::Graphics& g, Gdiplus::Rect rect, const std::vector<double>& hazard_all, const std::vector<double>& hazard_up, const std::wstring& title, int limit_base) {
    Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255,252,253,255)); g.FillRectangle(&bgBrush, rect);
    Gdiplus::FontFamily ff(L"Microsoft YaHei");
    Gdiplus::Font titleFont(&ff,DPIScaleF(15.0f),Gdiplus::FontStyleBold,Gdiplus::UnitPixel);
    Gdiplus::SolidBrush textBrush(Gdiplus::Color(255,40,40,40));
    g.DrawString(title.c_str(),-1,&titleFont,Gdiplus::PointF((float)rect.X+DPIScaleF(15.0f),(float)rect.Y+DPIScaleF(12.0f)),&textBrush);
    if(hazard_all.empty()&&hazard_up.empty()) return;
    int max_x=limit_base; if(!hazard_all.empty()&&(int)hazard_all.size()-1>max_x) max_x=(int)hazard_all.size()-1; if(!hazard_up.empty()&&(int)hazard_up.size()-1>max_x) max_x=(int)hazard_up.size()-1; max_x=((max_x/10)+1)*10;
    double max_y=0.1; for(double v:hazard_all) max_y=(std::max)(max_y,v); for(double v:hazard_up) max_y=(std::max)(max_y,v); if(max_y>0.8) max_y=1.05; else max_y=std::ceil(max_y*10)/10.0+0.1;
    float plotX=(float)rect.X+DPIScaleF(55.0f),plotY=(float)rect.Y+DPIScaleF(45.0f),plotW=(float)rect.Width-DPIScaleF(85.0f),plotH=(float)rect.Height-DPIScaleF(75.0f);
    Gdiplus::Pen axisPen(Gdiplus::Color(255,150,150,150),DPIScaleF(1.5f)); Gdiplus::Pen gridPen(Gdiplus::Color(255,235,235,235),DPIScaleF(1.0f));
    g.DrawLine(&axisPen,plotX,plotY+plotH,plotX+plotW,plotY+plotH); g.DrawLine(&axisPen,plotX,plotY,plotX,plotY+plotH);
    auto getPt=[&](int x,double y){return Gdiplus::PointF(plotX+(float)x/(float)max_x*plotW,plotY+plotH-(float)(y/max_y)*plotH);};
    Gdiplus::Font tickFont(&ff,DPIScaleF(11.0f),Gdiplus::FontStyleRegular,Gdiplus::UnitPixel); Gdiplus::SolidBrush tickBrush(Gdiplus::Color(255,120,120,120));
    for(int i=0;i<=4;++i){float py=plotY+plotH-(float)i/4.0f*plotH; if(i>0) g.DrawLine(&gridPen,plotX,py,plotX+plotW,py); g.DrawLine(&axisPen,plotX-DPIScaleF(5.0f),py,plotX,py); wchar_t yl[32]; swprintf(yl,32,L"%.0f%%",(max_y/4.0)*i*100.0); float lw=(float)wcslen(yl)*DPIScaleF(5.5f)+DPIScaleF(8.0f); g.DrawString(yl,-1,&tickFont,Gdiplus::PointF(plotX-lw,py-DPIScaleF(6.0f)),&tickBrush);}
    int step=(max_x>140)?20:10; for(int x=0;x<=max_x;x+=step){float px=plotX+(float)x/(float)max_x*plotW; g.DrawLine(&axisPen,px,plotY+plotH,px,plotY+plotH+DPIScaleF(5.0f)); wchar_t xl[16]; swprintf(xl,16,L"%d",x); float xo=(x<10?4.0f:x<100?8.0f:12.0f)*DPIScaleF(1.0f); g.DrawString(xl,-1,&tickFont,Gdiplus::PointF(px-xo,plotY+plotH+DPIScaleF(8.0f)),&tickBrush);}
    float barW=(std::max)(1.5f,plotW/max_x*0.4f);
    if(hazard_all.size()>1){Gdiplus::SolidBrush b(Gdiplus::Color(180,65,140,240)); for(size_t x=1;x<hazard_all.size();x++) if(hazard_all[x]>0){Gdiplus::PointF p=getPt((int)x,hazard_all[x]); g.FillRectangle(&b,p.X-barW,p.Y,barW,plotY+plotH-p.Y);}}
    if(hazard_up.size()>1){Gdiplus::SolidBrush b(Gdiplus::Color(180,240,80,80)); for(size_t x=1;x<hazard_up.size();x++) if(hazard_up[x]>0){Gdiplus::PointF p=getPt((int)x,hazard_up[x]); g.FillRectangle(&b,p.X,p.Y,barW,plotY+plotH-p.Y);}}
    Gdiplus::SolidBrush bb(Gdiplus::Color(255,65,140,240)),rb(Gdiplus::Color(255,240,80,80)); float lx=(float)rect.X+(float)rect.Width-DPIScaleF(160.0f);
    g.DrawString(L"\x25A0 \x7EFC\x5408S\x7EA7\x6761\x4EF6\x6982\x7387",-1,&tickFont,Gdiplus::PointF(lx,(float)rect.Y+DPIScaleF(15.0f)),&bb);
    g.DrawString(L"\x25A0 UP\x6761\x4EF6\x6982\x7387",-1,&tickFont,Gdiplus::PointF(lx,(float)rect.Y+DPIScaleF(35.0f)),&rb);
}

void RebuildChartCache(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc); int w=rc.right, h=rc.bottom; if(w<=0||h<=0) return;
    HDC hdcWnd=GetDC(hwnd); HDC hdcMem=CreateCompatibleDC(hdcWnd);
    if(g_hChartBmp) DeleteObject(g_hChartBmp);
    g_hChartBmp=CreateCompatibleBitmap(hdcWnd,w,h);
    HBITMAP hOld=(HBITMAP)SelectObject(hdcMem,g_hChartBmp);
    FillRect(hdcMem,&rc,(HBRUSH)(COLOR_WINDOW+1));
    {
        Gdiplus::Graphics g(hdcMem); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        DrawKDE(g,Gdiplus::Rect(DPIScale(20),DPIScale(330),DPIScale(600),DPIScale(250)),statsAgent.freq_all,statsAgent.freq_up,L"\x72EC\x5BB6\x9891\x6BB5 S\x7EA7\x6838\x5BC6\x5EA6",85);
        DrawHazard(g,Gdiplus::Rect(DPIScale(640),DPIScale(330),DPIScale(600),DPIScale(250)),statsAgent.hazard_all,statsAgent.hazard_up,L"\x72EC\x5BB6\x9891\x6BB5\x7ECF\x9A8C\x98CE\x9669\x51FD\x6570",85);
        DrawKDE(g,Gdiplus::Rect(DPIScale(20),DPIScale(585),DPIScale(600),DPIScale(250)),statsWEngine.freq_all,statsWEngine.freq_up,L"\x97F3\x64CE\x9891\x6BB5 S\x7EA7\x6838\x5BC6\x5EA6",80);
        DrawHazard(g,Gdiplus::Rect(DPIScale(640),DPIScale(585),DPIScale(600),DPIScale(250)),statsWEngine.hazard_all,statsWEngine.hazard_up,L"\x97F3\x64CE\x9891\x6BB5\x7ECF\x9A8C\x98CE\x9669\x51FD\x6570",80);
    }
    SelectObject(hdcMem,hOld); DeleteDC(hdcMem); ReleaseDC(hwnd,hdcWnd);
}

static HFONT hFont = NULL;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch(msg) {
    case WM_CREATE: {
        DragAcceptFiles(hwnd, TRUE);
        hFont = CreateFontW(-DPIScale(13),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Microsoft YaHei");

        HWND hL1=CreateWindowW(L"STATIC",L"支持通过常驻S级名单自动判定UP/歪。请保持名单最新。",WS_CHILD|WS_VISIBLE,DPIScale(20),DPIScale(15),DPIScale(1000),DPIScale(20),hwnd,NULL,NULL,NULL);
        HWND hLC=CreateWindowW(L"STATIC",L"常驻S级代理人:",WS_CHILD|WS_VISIBLE,DPIScale(20),DPIScale(45),DPIScale(110),DPIScale(20),hwnd,NULL,NULL,NULL);
        hCharEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"RichEdit50W",L"格莉丝,丽娜,珂蕾妲,猫又,「11号」,莱卡恩",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,DPIScale(135),DPIScale(40),DPIScale(1100),DPIScale(26),hwnd,NULL,NULL,NULL);
        HWND hLW=CreateWindowW(L"STATIC",L"常驻S级音擎:",WS_CHILD|WS_VISIBLE,DPIScale(20),DPIScale(75),DPIScale(110),DPIScale(20),hwnd,NULL,NULL,NULL);
        hWepEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"RichEdit50W",L"嵌合编译器,钢铁肉垫,啜泣摇篮,拘束者,硫磺石,燃狱齿轮",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,DPIScale(135),DPIScale(70),DPIScale(1100),DPIScale(26),hwnd,NULL,NULL,NULL);

        hOutEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"RichEdit50W",L"等待拖入 UIGF JSON 文件...",WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_READONLY|WS_VSCROLL,DPIScale(20),DPIScale(105),DPIScale(1220),DPIScale(215),hwnd,NULL,NULL,NULL);        DWORD tabStops[]={200}; SendMessage(hOutEdit,EM_SETTABSTOPS,1,(LPARAM)tabStops);
        SendMessage(hOutEdit,EM_SETBKGNDCOLOR,0,(LPARAM)GetSysColor(COLOR_3DFACE));

        for(HWND h:{hL1,hLC,hCharEdit,hLW,hWepEdit,hOutEdit}) SendMessage(h,WM_SETFONT,(WPARAM)hFont,TRUE);
        RebuildChartCache(hwnd);
        break;
    }
    case WM_DROPFILES: {
        HDROP hDrop=(HDROP)wParam; wchar_t fp[MAX_PATH];
        DragQueryFileW(hDrop,0,fp,MAX_PATH); DragFinish(hDrop);
        ProcessFile(fp); RebuildChartCache(hwnd); InvalidateRect(hwnd,NULL,FALSE);
        break;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc=BeginPaint(hwnd,&ps);
        if(g_hChartBmp){HDC hdcMem=CreateCompatibleDC(hdc); HBITMAP hOld=(HBITMAP)SelectObject(hdcMem,g_hChartBmp);
            BitBlt(hdc,ps.rcPaint.left,ps.rcPaint.top,ps.rcPaint.right-ps.rcPaint.left,ps.rcPaint.bottom-ps.rcPaint.top,hdcMem,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);
            SelectObject(hdcMem,hOld); DeleteDC(hdcMem);}
        EndPaint(hwnd,&ps); break;
    }
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: {
        if(g_hChartBmp){DeleteObject(g_hChartBmp);g_hChartBmp=NULL;}
        if(hFont)DeleteObject(hFont);
        PostQuitMessage(0); break;
    }}
    return DefWindowProc(hwnd,msg,wParam,lParam);
}

int WINAPI WinMain(HINSTANCE hInstance,HINSTANCE,LPSTR,int nCmdShow) {
    LoadLibrary(L"Msftedit.dll");
    SetProcessDPIAware();
    HDC hdc=GetDC(NULL); g_dpi=GetDeviceCaps(hdc,LOGPIXELSX); ReleaseDC(NULL,hdc);
    ULONG_PTR gdipToken; Gdiplus::GdiplusStartupInput si; Gdiplus::GdiplusStartup(&gdipToken,&si,NULL);
    InitCDFTables();

    WNDCLASSW wc={0}; wc.lpfnWndProc=WndProc; wc.hInstance=hInstance;
    wc.hCursor=LoadCursor(NULL,IDC_ARROW); wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    wc.lpszClassName=L"ZZZStatsClass"; RegisterClassW(&wc);

    DWORD dwStyle=(WS_OVERLAPPEDWINDOW^WS_THICKFRAME^WS_MAXIMIZEBOX)|WS_CLIPCHILDREN;
    RECT r={0,0,DPIScale(1280),DPIScale(870)}; AdjustWindowRectEx(&r,dwStyle,FALSE,0);
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"绝区零调频记录分析与可视化",dwStyle,CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,NULL,NULL,hInstance,NULL);    ShowWindow(hwnd,nCmdShow);

    MSG msg; while(GetMessage(&msg,NULL,0,0)){TranslateMessage(&msg);DispatchMessage(&msg);}
    Gdiplus::GdiplusShutdown(gdipToken);
    return 0;
}
