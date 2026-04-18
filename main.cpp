#include <cstdio>
#include <cstdlib>
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

std::string_view ExtractHost(std::string_view url) {
    auto pos = url.find("://");
    if (pos == std::string_view::npos) return {};
    pos += 3;
    auto end = url.find('/', pos);
    return (end == std::string_view::npos) ? url.substr(pos) : url.substr(pos, end - pos);
}

// ---------------------------------------------------------
// [数据结构层：面向数据设计 (SoA)]
// ---------------------------------------------------------
struct ExportDataSoA {
    std::pmr::vector<long long> parsed_ids;
    std::pmr::vector<std::string_view> raw_ids;
    std::pmr::vector<std::string_view> gacha_ids;
    std::pmr::vector<std::string_view> gacha_types;
    std::pmr::vector<std::string_view> item_ids;
    std::pmr::vector<std::string_view> names;
    std::pmr::vector<std::string_view> item_types;
    std::pmr::vector<std::string_view> rank_types;
    std::pmr::vector<std::string_view> times;

    explicit ExportDataSoA(std::pmr::polymorphic_allocator<std::byte> alloc)
        : parsed_ids(alloc), raw_ids(alloc), gacha_ids(alloc), gacha_types(alloc), item_ids(alloc),
          names(alloc), item_types(alloc), rank_types(alloc), times(alloc) {}

    void reserve(size_t cap) {
        parsed_ids.reserve(cap); raw_ids.reserve(cap); gacha_ids.reserve(cap);
        gacha_types.reserve(cap); item_ids.reserve(cap); names.reserve(cap);
        item_types.reserve(cap); rank_types.reserve(cap); times.reserve(cap);
    }
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
        Write("            \"", 13); Write(key); Write("\": \"", 4); WriteEscaped(val); Write("\"", 1);
    }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);

    char urlBuffer[4096]; 
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
    
    // PMR：在 Stack 上开辟 2MB 内存池 (配合编译指令 /STACK:4194304)
    std::array<std::byte, 2 * 1024 * 1024> stackBuffer;
    std::pmr::monotonic_buffer_resource pool(stackBuffer.data(), stackBuffer.size());
    std::pmr::polymorphic_allocator<std::byte> alloc(&pool);

    ExportDataSoA pulls(alloc);
    pulls.reserve(10000); 

    // 核心修复：使用 std::deque 彻底告别扩容时的内存指针失效
    std::deque<std::string> networkPayloads;

    std::pmr::vector<long long> local_safe_ids(&pool);
    local_safe_ids.reserve(10000);

    std::string uigfFilename = "uigf_zzz.json";
    std::string savedUid;
    int savedTimezone = 0;
    
    HANDLE hFile = CreateFileA(uigfFilename.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    const char* mapData = nullptr;
    HANDLE hMap = NULL;

    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD fileSize = GetFileSize(hFile, NULL);
        if (fileSize != INVALID_FILE_SIZE && fileSize > 0) {
            hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
            if (hMap) {
                mapData = (const char*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
                if (mapData) {
                    std::string_view bufferView(mapData, fileSize);
                    
                    // 修复 UTF-8 BOM 导致读取失败的问题
                    if (bufferView.size() >= 3 && (unsigned char)bufferView[0] == 0xEF && (unsigned char)bufferView[1] == 0xBB && (unsigned char)bufferView[2] == 0xBF) {
                        bufferView.remove_prefix(3);
                    }

                    savedUid = std::string(ExtractJsonValue(bufferView, "uid", false));
                    if (savedUid.empty()) savedUid = std::string(ExtractJsonValue(bufferView, "uid", true));
                    auto tzStr = ExtractJsonValue(bufferView, "timezone", false);
                    if (!tzStr.empty()) std::from_chars(tzStr.data(), tzStr.data()+tzStr.size(), savedTimezone);
                    
                    ForEachJsonObject(bufferView, "list", [&](std::string_view itemStr) {
                        std::string_view raw_id = ExtractJsonValue(itemStr, "id", true);
                        long long parsed_id = 0;
                        if (!raw_id.empty()) std::from_chars(raw_id.data(), raw_id.data() + raw_id.size(), parsed_id);
                        
                        pulls.parsed_ids.push_back(parsed_id);
                        pulls.raw_ids.push_back(raw_id);
                        pulls.gacha_types.push_back(ExtractJsonValue(itemStr, "gacha_type", true));
                        pulls.gacha_ids.push_back(ExtractJsonValue(itemStr, "gacha_id", true));
                        pulls.item_ids.push_back(ExtractJsonValue(itemStr, "item_id", true));
                        pulls.names.push_back(ExtractJsonValue(itemStr, "name", true));
                        pulls.item_types.push_back(ExtractJsonValue(itemStr, "item_type", true));
                        pulls.rank_types.push_back(ExtractJsonValue(itemStr, "rank_type", true));
                        pulls.times.push_back(ExtractJsonValue(itemStr, "time", true));

                        local_safe_ids.push_back(parsed_id);
                    });
                }
            }
        }
        printf("成功加载本地存储的 %zu 条抽卡记录。\n", pulls.parsed_ids.size());
    } else {
        printf("未发现本地记录，将创建新文件。\n");
    }

    // 核心优化：纯粹利用自然二分查找
    std::ranges::sort(local_safe_ids);

    printf("\n========================================\n");
    printf("       开始向服务器拉取调频数据\n");
    printf("========================================\n");

    HINTERNET hSession = WinHttpOpen(L"ZZZ Gacha Tool", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET hConnect = hSession ? WinHttpConnect(hSession, Utf8ToWstring(std::string(host)).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    if (!hConnect) { printf("网络初始化失败！\n"); system("pause"); return 1; }

    std::pmr::vector<long long> sessionIds(&pool); 
    sessionIds.reserve(2000);

    std::string authkeyStr(authkey), gameBizStr(gameBiz), regionStr(region), langStr(lang);
    std::string commonParams = "authkey_ver=1&sign_type=2&authkey=" + authkeyStr 
        + "&game_biz=" + gameBizStr + "&region=" + regionStr + "&lang=" + langStr + "&size=20";

    for (const auto& poolCfg : pools) {
        printf("\n>>> 正在抓取 [%s] ...\n", poolCfg.displayName.c_str());
        bool reachedExisting = false;
        std::string endId;
        int page = 1, poolFetchedCount = 0;

        while (!reachedExisting) {
            std::string path = "/common/gacha_record/api/getGachaLog?" + commonParams 
                + "&real_gacha_type=" + poolCfg.gachaType
                + "&end_id=" + endId;

            // 数据存入 Deque，保证生命周期
            networkPayloads.emplace_back(FetchPath(hConnect, Utf8ToWstring(path)));
            std::string_view resView = networkPayloads.back();

            if (resView.empty()) { printf("  [错误] 网络请求失败。\n"); break; }

            auto retcode = ExtractJsonValue(resView, "retcode", false);
            if (retcode != "0") {
                auto msg = ExtractJsonValue(resView, "message", true);
                printf("  [提示] %.*s\n", (int)msg.size(), msg.data());
                break;
            }

            bool gotItems = false;
            std::string lastId;
            
            ForEachJsonObject(resView, "list", [&](std::string_view itemStr) {
                if (reachedExisting) return;
                gotItems = true;

                auto idStr = ExtractJsonValue(itemStr, "id", true);
                long long id = 0;
                if (!idStr.empty()) std::from_chars(idStr.data(), idStr.data()+idStr.size(), id);
                lastId = std::string(idStr);

                auto it = std::ranges::lower_bound(local_safe_ids, id);
                if (it != local_safe_ids.end() && *it == id) {
                    reachedExisting = true;
                    printf("  * 触达本地老记录 (ID: %lld)，停止追溯。\n", id);
                    return;
                }
                
                auto s_it = std::ranges::lower_bound(sessionIds, id);
                if (s_it != sessionIds.end() && *s_it == id) {
                    printf("  [警告] 遇到重复数据 (ID: %lld)，中止。\n", id);
                    reachedExisting = true; return;
                }

                if (savedUid.empty()) savedUid = std::string(ExtractJsonValue(itemStr, "uid", true));

                sessionIds.insert(s_it, id); 

                pulls.parsed_ids.push_back(id);
                pulls.raw_ids.push_back(idStr);
                pulls.gacha_types.push_back(ExtractJsonValue(itemStr, "gacha_type", true));
                pulls.gacha_ids.push_back(ExtractJsonValue(itemStr, "gacha_id", true));
                pulls.item_ids.push_back(ExtractJsonValue(itemStr, "item_id", true));
                pulls.names.push_back(ExtractJsonValue(itemStr, "name", true));
                pulls.item_types.push_back(ExtractJsonValue(itemStr, "item_type", true));
                pulls.rank_types.push_back(ExtractJsonValue(itemStr, "rank_type", true));
                pulls.times.push_back(ExtractJsonValue(itemStr, "time", true));

                poolFetchedCount++;
                printf("  获取到: %.*s (%.*s 星) [%.*s] - %.*s\n", 
                    (int)pulls.names.back().size(), pulls.names.back().data(), 
                    (int)pulls.rank_types.back().size(), pulls.rank_types.back().data(), 
                    (int)pulls.item_types.back().size(), pulls.item_types.back().data(),
                    (int)pulls.times.back().size(), pulls.times.back().data());
            });

            if (reachedExisting || !gotItems) break;
            endId = lastId;
            page++;
            Sleep(300);
        }
        printf(">>> [%s] 抓取完成，本次新增: %d 条。\n", poolCfg.displayName.c_str(), poolFetchedCount);
        Sleep(500);
    }

    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);

    printf("\n========================================\n");
    printf("已完成！总计新增 %zu 条记录。\n", sessionIds.size());

    // 创建索引数组进行排序
    std::pmr::vector<size_t> indices(pulls.parsed_ids.size(), &pool);
    std::iota(indices.begin(), indices.end(), 0);
    std::ranges::sort(indices, [&](size_t a, size_t b) {
        return pulls.parsed_ids[a] < pulls.parsed_ids[b];
    });

    if (savedTimezone == 0) {
        if (regionStr.find("cn") != std::string::npos) savedTimezone = 8;
        else if (regionStr.find("jp") != std::string::npos) savedTimezone = 9;
        else if (regionStr.find("eu") != std::string::npos) savedTimezone = 1;
        else if (regionStr.find("us") != std::string::npos) savedTimezone = -5;
        else savedTimezone = 0;
    }

    time_t rawtime; time(&rawtime);
    long long export_ts = (long long)rawtime;

    // 核心优化：原子级别安全写入机制
    std::string tempFilename = uigfFilename + ".tmp";
    HANDLE hOut = CreateFileA(tempFilename.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    
    if (hOut != INVALID_HANDLE_VALUE) {
        BufferedWriter w{hOut};
        char numBuf[24];
        
        w.Write("{\n  \"info\": {\n");
        w.Write("    \"export_timestamp\": "); w.Write(I64ToStr(export_ts, numBuf)); w.Write(",\n");
        w.Write("    \"export_app\": \"ZZZ Gacha Exporter\",\n");
        w.Write("    \"export_app_version\": \"v1.0.0\",\n");
        w.Write("    \"version\": \"v4.2\"\n  },\n");
        
        w.Write("  \"nap\": [\n    {\n");
        w.Write("      \"uid\": \""); w.Write(savedUid.empty() ? "0" : savedUid); w.Write("\",\n");
        w.Write("      \"timezone\": "); w.Write(I64ToStr(savedTimezone, numBuf)); w.Write(",\n");
        w.Write("      \"lang\": \""); w.Write(langStr); w.Write("\",\n");
        w.Write("      \"list\": [\n");

        for (size_t i = 0; i < indices.size(); ++i) {
            size_t idx = indices[i];
            w.Write("        {\n");
            w.WriteKV("gacha_id", pulls.gacha_ids[idx]); w.Write(",\n");
            w.WriteKV("gacha_type", pulls.gacha_types[idx]); w.Write(",\n");
            w.WriteKV("item_id", pulls.item_ids[idx]); w.Write(",\n");
            w.Write("            \"count\": \"1\",\n");
            w.WriteKV("time", pulls.times[idx]); w.Write(",\n");
            w.WriteKV("name", pulls.names[idx]); w.Write(",\n");
            w.WriteKV("item_type", pulls.item_types[idx]); w.Write(",\n");
            w.WriteKV("rank_type", pulls.rank_types[idx]); w.Write(",\n");
            w.WriteKV("id", pulls.raw_ids[idx]); w.Write("\n");
            w.Write("        }");
            if (i < indices.size() - 1) w.Write(",");
            w.Write("\n");
        }
        
        w.Write("      ]\n    }\n  ]\n}\n");
        w.Flush();
        CloseHandle(hOut);
        
        // 核心修复：临时文件落盘后，安全解除老文件锁
        if (mapData) UnmapViewOfFile(mapData);
        if (hMap) CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
        mapData = nullptr; hMap = NULL; hFile = INVALID_HANDLE_VALUE;

        // 最后瞬间覆盖替换
        if (MoveFileExA(tempFilename.c_str(), uigfFilename.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            printf("已成功更新记录并保存至: %s (UIGF v4.2)\n", uigfFilename.c_str());
        } else {
            printf("文件覆盖失败！请手动将 %s 重命名为 %s\n", tempFilename.c_str(), uigfFilename.c_str());
        }
    } else {
        printf("临时文件创建失败！请检查目录权限。\n");
        if (mapData) UnmapViewOfFile(mapData);
        if (hMap) CloseHandle(hMap);
        if (hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);
    }

    system("pause");
    return 0;
}
