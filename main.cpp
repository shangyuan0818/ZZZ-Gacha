#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <ctime>
#include <windows.h>
#include <winhttp.h>
#include <string_view>
#include <charconv>
#include <ranges>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "User32.lib")

// ---------------------------------------------------------
// [极简 JSON 解析模块 - 零分配纯净版]
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

std::wstring Utf8ToWstring(std::string_view str) {
    if (str.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    std::wstring result(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), result.data(), size);
    return result;
}

char* I64ToStr(long long val, char* buf) {
    auto [ptr, ec] = std::to_chars(buf, buf + 20, val);
    *ptr = '\0'; return buf;
}

std::string_view ExtractUrlParam(std::string_view url, std::string_view key) {
    size_t pos = url.find(key);
    if (pos == std::string_view::npos) return {};
    pos += key.length();
    size_t end = url.find('&', pos);
    return (end == std::string_view::npos) ? url.substr(pos) : url.substr(pos, end - pos);
}

// 从 URL 提取主机名 (https://HOST/path...)
std::string_view ExtractHost(std::string_view url) {
    auto pos = url.find("://");
    if (pos == std::string_view::npos) return {};
    pos += 3;
    auto end = url.find('/', pos);
    return (end == std::string_view::npos) ? url.substr(pos) : url.substr(pos, end - pos);
}

struct UIGFItem {
    std::string gacha_type, id, gacha_id, item_id, name, item_type, rank_type, time;
    long long parsed_id = 0;
};

std::string FetchPath(HINTERNET hConnect, const std::wstring& path) {
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    std::string response;
    if (hRequest) {
        if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(hRequest, NULL)) {
            DWORD dwSize = 0, dwDownloaded = 0;
            char stackBuf[8192];
            do {
                WinHttpQueryDataAvailable(hRequest, &dwSize);
                if (dwSize == 0) break;
                if (dwSize <= sizeof(stackBuf)) {
                    if (WinHttpReadData(hRequest, stackBuf, dwSize, &dwDownloaded))
                        response.append(stackBuf, dwDownloaded);
                } else {
                    std::vector<char> heapBuf(dwSize);
                    if (WinHttpReadData(hRequest, heapBuf.data(), dwSize, &dwDownloaded))
                        response.append(heapBuf.data(), dwDownloaded);
                }
            } while (dwSize > 0);
        }
        WinHttpCloseHandle(hRequest);
    }
    return response;
}

struct PoolConfig { std::string gachaType, displayName; };

struct BufferedWriter {
    HANDLE hFile;
    char buf[65536];
    DWORD pos = 0;
    void Flush() { if (pos > 0) { DWORD w; WriteFile(hFile, buf, pos, &w, NULL); pos = 0; } }
    void Write(const char* data, DWORD len) {
        while (len > 0) { DWORD space = sizeof(buf)-pos, chunk = (len<space)?len:space;
            CopyMemory(buf+pos, data, chunk); pos+=chunk; data+=chunk; len-=chunk;
            if (pos==sizeof(buf)) Flush(); }
    }
    void Write(std::string_view sv) { Write(sv.data(), (DWORD)sv.size()); }
    void WriteEscaped(std::string_view s) {
        const char* p=s.data(); const char* end=p+s.size();
        while (p<end) { const char* c=p; while(p<end&&*p!='"'&&*p!='\\')++p;
            if(p>c) Write(c,(DWORD)(p-c));
            if(p<end){if(*p=='"')Write("\\\"",2);else if(*p=='\\')Write("\\\\",2);++p;} }
    }
    void WriteKV(std::string_view key, std::string_view val) {
        Write("            \"",13); Write(key); Write("\": \"",4); WriteEscaped(val); Write("\"",1);
    }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);

    char urlBuffer[4096]; // authkey 很长，需要大 buffer
    printf("请输入您的绝区零抽卡记录链接 (getGachaLog URL):\n> ");
    if (!fgets(urlBuffer, sizeof(urlBuffer), stdin)) return 1;
    
    std::string_view inputUrl(urlBuffer);
    while (!inputUrl.empty() && (inputUrl.back()==' '||inputUrl.back()=='\n'||inputUrl.back()=='\r'||inputUrl.back()=='\t'))
        inputUrl.remove_suffix(1);

    auto authkey = ExtractUrlParam(inputUrl, "authkey=");
    if (authkey.empty()) { printf("错误: 无法提取 authkey。\n"); system("pause"); return 1; }
    
    auto gameBiz = ExtractUrlParam(inputUrl, "game_biz=");
    auto region = ExtractUrlParam(inputUrl, "region=");
    auto lang = ExtractUrlParam(inputUrl, "lang=");
    if (gameBiz.empty()) gameBiz = "nap_global";
    if (region.empty()) region = "prod_gf_jp";
    if (lang.empty()) lang = "zh-cn";
    
    auto host = ExtractHost(inputUrl);
    if (host.empty()) host = "public-operation-nap-sg.hoyoverse.com";
    
    printf("\n已识别: host=%.*s  region=%.*s  game_biz=%.*s\n",
        (int)host.size(), host.data(), (int)region.size(), region.data(), (int)gameBiz.size(), gameBiz.data());

    std::vector<PoolConfig> pools = {
        {"2", "独家频段 (限定角色)"},
        {"3", "音擎频段 (限定音擎)"},
        {"1", "常驻频段"},
        {"5", "邦布频段"},
    };
    
    std::string uigfFilename = "uigf_zzz.json";
    std::vector<UIGFItem> allRecords;
    std::unordered_set<long long> localIds;
    std::string savedUid;
    int savedTimezone = 0;
    
    // 读取本地已有记录 (UIGF v4.2 格式)
    HANDLE hFile = CreateFileA(uigfFilename.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD fileSize = GetFileSize(hFile, NULL);
        if (fileSize != INVALID_FILE_SIZE && fileSize > 0) {
            HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
            if (hMap) {
                const char* mapData = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
                if (mapData) {
                    std::string_view bv(mapData, fileSize);
                    // 提取 uid 和 timezone
                    savedUid = std::string(ExtractJsonValue(bv, "uid", false));
                    if (savedUid.empty()) savedUid = std::string(ExtractJsonValue(bv, "uid", true));
                    auto tzStr = ExtractJsonValue(bv, "timezone", false);
                    if (!tzStr.empty()) std::from_chars(tzStr.data(), tzStr.data()+tzStr.size(), savedTimezone);
                    
                    // UIGF v4.2: data 在 nap[0].list 里, ForEachJsonObject 搜索 "list" 即可
                    ForEachJsonObject(bv, "list", [&](std::string_view itemStr) {
                        UIGFItem u;
                        u.gacha_type = std::string(ExtractJsonValue(itemStr, "gacha_type", true));
                        u.id = std::string(ExtractJsonValue(itemStr, "id", true));
                        u.gacha_id = std::string(ExtractJsonValue(itemStr, "gacha_id", true));
                        u.item_id = std::string(ExtractJsonValue(itemStr, "item_id", true));
                        u.name = std::string(ExtractJsonValue(itemStr, "name", true));
                        u.item_type = std::string(ExtractJsonValue(itemStr, "item_type", true));
                        u.rank_type = std::string(ExtractJsonValue(itemStr, "rank_type", true));
                        u.time = std::string(ExtractJsonValue(itemStr, "time", true));
                        if (!u.id.empty()) std::from_chars(u.id.data(), u.id.data()+u.id.size(), u.parsed_id);
                        localIds.insert(u.parsed_id);
                        allRecords.push_back(std::move(u));
                    });
                    UnmapViewOfFile(mapData);
                }
                CloseHandle(hMap);
            }
        }
        CloseHandle(hFile);
        printf("成功加载本地存储的 %zu 条抽卡记录。\n", allRecords.size());
    } else {
        printf("未发现本地记录，将创建新文件。\n");
    }

    printf("\n========================================\n");
    printf("       开始向服务器拉取调频数据\n");
    printf("========================================\n");

    HINTERNET hSession = WinHttpOpen(L"ZZZ Gacha Tool", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = hSession ? WinHttpConnect(hSession, Utf8ToWstring(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    if (!hConnect) { printf("网络初始化失败！\n"); system("pause"); return 1; }

    std::unordered_set<long long> sessionIds;
    std::string authkeyStr(authkey), gameBizStr(gameBiz), regionStr(region), langStr(lang);
    
    // 构建公共查询参数 (authkey 已经是 URL-encoded)
    std::string commonParams = "authkey_ver=1&sign_type=2&authkey=" + authkeyStr 
        + "&game_biz=" + gameBizStr + "&region=" + regionStr + "&lang=" + langStr + "&size=20";

    for (const auto& pool : pools) {
        printf("\n>>> 正在抓取 [%s] ...\n", pool.displayName.c_str());
        bool reachedExisting = false;
        std::string endId;
        int page = 1, poolFetchedCount = 0;

        while (!reachedExisting) {
            std::string path = "/common/gacha_record/api/getGachaLog?" + commonParams 
                + "&real_gacha_type=" + pool.gachaType
                + "&end_id=" + endId;

            std::string resStr = FetchPath(hConnect, Utf8ToWstring(path));
            if (resStr.empty()) { printf("  [错误] 网络请求失败。\n"); break; }

            std::string_view resView(resStr);
            auto retcode = ExtractJsonValue(resView, "retcode", false);
            if (retcode != "0") {
                auto msg = ExtractJsonValue(resView, "message", true);
                printf("  [提示] %.*s\n", (int)msg.size(), msg.data());
                break;
            }

            // 检查是否有数据 (HoYo API: 空 list 表示没有更多数据)
            bool gotItems = false;
            std::string lastId;
            
            ForEachJsonObject(resView, "list", [&](std::string_view itemStr) {
                if (reachedExisting) return;
                gotItems = true;

                auto idStr = ExtractJsonValue(itemStr, "id", true);
                long long id = 0;
                if (!idStr.empty()) std::from_chars(idStr.data(), idStr.data()+idStr.size(), id);
                lastId = std::string(idStr);

                if (localIds.contains(id)) {
                    reachedExisting = true;
                    printf("  * 触达本地老记录 (ID: %lld)，停止追溯。\n", id);
                    return;
                }
                if (sessionIds.contains(id)) {
                    printf("  [警告] 遇到重复数据 (ID: %lld)，中止。\n", id);
                    reachedExisting = true; return;
                }

                UIGFItem u;
                u.gacha_type = std::string(ExtractJsonValue(itemStr, "gacha_type", true));
                u.id = std::string(idStr);
                u.parsed_id = id;
                u.gacha_id = std::string(ExtractJsonValue(itemStr, "gacha_id", true));
                u.item_id = std::string(ExtractJsonValue(itemStr, "item_id", true));
                u.name = std::string(ExtractJsonValue(itemStr, "name", true));
                u.item_type = std::string(ExtractJsonValue(itemStr, "item_type", true));
                u.rank_type = std::string(ExtractJsonValue(itemStr, "rank_type", true));
                u.time = std::string(ExtractJsonValue(itemStr, "time", true));
                
                // 提取 uid (从第一条记录)
                if (savedUid.empty()) {
                    savedUid = std::string(ExtractJsonValue(itemStr, "uid", true));
                }

                sessionIds.insert(id);
                allRecords.push_back(std::move(u));
                poolFetchedCount++;
                printf("  获取到: %s (%s) [%s] - %s\n", 
                    allRecords.back().name.c_str(), allRecords.back().rank_type.c_str(),
                    allRecords.back().item_type.c_str(), allRecords.back().time.c_str());
            });

            if (reachedExisting || !gotItems) break;
            endId = lastId;
            page++;
            Sleep(300);
        }
        printf(">>> [%s] 抓取完成，本次新增: %d 条。\n", pool.displayName.c_str(), poolFetchedCount);
        Sleep(500);
    }

    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);

    printf("\n========================================\n");
    printf("已完成！总计新增 %zu 条记录。\n", sessionIds.size());

    // 按 id 排序 (HoYo 的 id 本身就是时间序)
    std::ranges::sort(allRecords, {}, [](const UIGFItem& a) { return a.parsed_id; });

    // 提取 timezone (从 region 推断)
    if (savedTimezone == 0) {
        if (regionStr.find("cn") != std::string::npos) savedTimezone = 8;
        else if (regionStr.find("jp") != std::string::npos) savedTimezone = 9;
        else if (regionStr.find("eu") != std::string::npos) savedTimezone = 1;
        else if (regionStr.find("us") != std::string::npos) savedTimezone = -5;
        else savedTimezone = 0;
    }

    time_t rawtime; time(&rawtime);
    long long export_ts = (long long)rawtime;

    // UIGF v4.2 写入
    HANDLE hOut = CreateFileA(uigfFilename.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOut != INVALID_HANDLE_VALUE) {
        BufferedWriter w{hOut};
        char numBuf[24];
        
        w.Write("{\n  \"info\": {\n");
        w.Write("    \"export_timestamp\": "); w.Write(I64ToStr(export_ts, numBuf)); w.Write(",\n");
        w.Write("    \"export_app\": \"ZZZ Gacha Exporter\",\n");
        w.Write("    \"export_app_version\": \"v1.0.0\",\n");
        w.Write("    \"version\": \"v4.2\"\n  },\n");
        
        // nap 数组
        w.Write("  \"nap\": [\n    {\n");
        w.Write("      \"uid\": "); w.Write(savedUid.empty() ? "0" : savedUid); w.Write(",\n");
        w.Write("      \"timezone\": "); w.Write(I64ToStr(savedTimezone, numBuf)); w.Write(",\n");
        w.Write("      \"lang\": \""); w.Write(langStr); w.Write("\",\n");
        w.Write("      \"list\": [\n");

        for (size_t i = 0; i < allRecords.size(); ++i) {
            const auto& p = allRecords[i];
            w.Write("        {\n");
            w.WriteKV("gacha_id", p.gacha_id); w.Write(",\n");
            w.WriteKV("gacha_type", p.gacha_type); w.Write(",\n");
            w.WriteKV("item_id", p.item_id); w.Write(",\n");
            w.Write("            \"count\": \"1\",\n");
            w.WriteKV("time", p.time); w.Write(",\n");
            w.WriteKV("name", p.name); w.Write(",\n");
            w.WriteKV("item_type", p.item_type); w.Write(",\n");
            w.WriteKV("rank_type", p.rank_type); w.Write(",\n");
            w.WriteKV("id", p.id); w.Write("\n");
            w.Write("        }");
            if (i < allRecords.size() - 1) w.Write(",");
            w.Write("\n");
        }
        
        w.Write("      ]\n    }\n  ]\n}\n");
        w.Flush();
        CloseHandle(hOut);
        printf("已保存至: %s (UIGF v4.2)\n", uigfFilename.c_str());
    } else {
        printf("文件写入失败！\n");
    }

    system("pause");
    return 0;
}
