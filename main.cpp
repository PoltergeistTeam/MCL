// PoltergeistLauncher — C++17, Win32 + Direct3D11 + Dear ImGui.
// Vanilla / Fabric / Forge, Java auto-detect+download, offline / Ely.by / Microsoft,
// custom backgrounds from screenshots, crash explainer.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <commdlg.h>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <d3d11.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <map>
#include <set>
#include <optional>
#include <random>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <nlohmann/json.hpp>
#include <miniz.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include <stb_image.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ====== Настройка: Azure Client ID для входа через Microsoft (см. объяснение в ответе) ======
static const std::string kMsClientId = ""; // можно оставить пустым: ID вводится в приложении (вкладка «Аккаунт») и сохраняется в config.json
static std::mutex gIdMu; static std::string gMsId = kMsClientId;
static std::string MsId() { std::lock_guard<std::mutex> l(gIdMu); return gMsId; }
static void SetMsId(const std::string& v) { std::lock_guard<std::mutex> l(gIdMu); gMsId = v; }
static const std::string kAuthlibUrl = "https://github.com/yushijinhun/authlib-injector/releases/download/v1.2.5/authlib-injector-1.2.5.jar";

// ============================ Утилиты ============================
static std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), 0, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string U8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), 0, 0, 0, 0);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, 0, 0);
    return s;
}
static fs::path P(const std::string& s) { return fs::path(W(s)); }
static std::string PS(const fs::path& p) { return U8(p.wstring()); }
static std::wstring Env(const wchar_t* n) {
    wchar_t b[2048];
    DWORD l = GetEnvironmentVariableW(n, b, 2048);
    return (l && l < 2048) ? std::wstring(b, l) : L"";
}
static std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static void WriteAll(const fs::path& p, const std::string& d) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path t = p; t += L".tmp";
    { std::ofstream f(t, std::ios::binary); f.write(d.data(), (std::streamsize)d.size()); }
    fs::rename(t, p, ec);
}
static json Parse(const std::string& s) { return json::parse(s, nullptr, false); }
static std::string RandHex(int n) {
    static std::random_device rd; static const char* h = "0123456789abcdef";
    std::string s; for (int i = 0; i < n * 2; i++) s += h[rd() % 16]; return s;
}
static std::vector<std::string> Split(const std::string& s, char c) {
    std::vector<std::string> r; std::string cur; std::stringstream ss(s);
    while (std::getline(ss, cur, c)) r.push_back(cur);
    return r;
}
static std::string Q(const std::string& a) { // quoting для CreateProcess
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string r = "\""; int bs = 0;
    for (char c : a) {
        if (c == '\\') { bs++; r += c; }
        else if (c == '"') { r.append(bs + 1, '\\'); r += '"'; bs = 0; }
        else { bs = 0; r += c; }
    }
    r.append(bs, '\\'); r += "\""; return r;
}
static std::string Sub(std::string s, const std::map<std::string, std::string>& m) {
    for (auto& [k, v] : m) {
        std::string key = "${" + k + "}"; size_t p = 0;
        while ((p = s.find(key, p)) != std::string::npos) { s.replace(p, key.size(), v); p += v.size(); }
    }
    return s;
}

// ============================ Глобальное состояние ============================
static fs::path gRoot;    // папка игры (версии, библиотеки, Java, instances)
static fs::path gCfgDir;  // папка настроек лаунчера (config.json) — всегда одна и та же
static HWND gHwnd; static std::atomic<bool> gGameRunning{false};
static std::mutex gMu;
static std::deque<std::string> gLog;
static std::string gStatus = "Готов к запуску";
static std::atomic<float> gProg{0};
static std::atomic<bool> gBusy{false};
static bool gShowCrash = false;
static std::string gCrashTitle, gCrashBody;
static std::string gMsCode, gMsUri;
struct Account { int type = 0; std::string name, uuid, token, refresh; }; // 0 offline, 1 Ely.by, 2 Microsoft
static std::optional<Account> gNewAcc;
static std::atomic<bool> gAuthBusy{false};
static char bNick[32] = "Player", bElyU[128], bElyP[160], bJava[260], bMsId[96];

static void Log(const std::string& s) { std::lock_guard<std::mutex> l(gMu); gLog.push_back(s); if (gLog.size() > 3000) gLog.pop_front(); }
static void Status(const std::string& s) { { std::lock_guard<std::mutex> l(gMu); gStatus = s; } Log(s); }
static void ShowError(const std::string& t, const std::string& b) { std::lock_guard<std::mutex> l(gMu); gShowCrash = true; gCrashTitle = t; gCrashBody = b; }

// ============================ HTTP (WinHTTP) ============================
struct Resp { int status = 0; std::string body; };
static Resp Http(const std::string& url, const std::string& method = "GET", const std::string& body = "", const std::string& hdr = "") {
    Resp r; std::wstring w = W(url);
    URL_COMPONENTSW uc{}; uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(w.c_str(), 0, 0, &uc)) return r;
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength), path(uc.lpszUrlPath, uc.dwUrlPathLength + uc.dwExtraInfoLength);
    static HINTERNET ses = [] {
        HINTERNET h = WinHttpOpen(L"PoltergeistLauncher/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, 0, 0, 0);
        if (!h) h = WinHttpOpen(L"PoltergeistLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, 0, 0, 0); // старые Windows (до 8.1)
        return h;
    }();
    HINTERNET con = WinHttpConnect(ses, host.c_str(), uc.nPort, 0);
    if (!con) return r;
    HINTERNET req = WinHttpOpenRequest(con, W(method).c_str(), path.c_str(), 0, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (req) {
        std::wstring h = W(hdr);
        BOOL ok = WinHttpSendRequest(req, h.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : h.c_str(), h.empty() ? 0 : (DWORD)-1,
                                     body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0)
                  && WinHttpReceiveResponse(req, 0);
        if (ok) {
            DWORD sc = 0, sz = sizeof(sc);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &sc, &sz, WINHTTP_NO_HEADER_INDEX);
            r.status = (int)sc;
            for (;;) {
                DWORD n = 0;
                if (!WinHttpQueryDataAvailable(req, &n) || !n) break;
                size_t o = r.body.size(); r.body.resize(o + n);
                DWORD rd = 0; WinHttpReadData(req, &r.body[o], n, &rd); r.body.resize(o + rd);
            }
        }
        WinHttpCloseHandle(req);
    }
    WinHttpCloseHandle(con);
    return r;
}
static Resp PostJson(const std::string& url, const json& j) {
    return Http(url, "POST", j.dump(), "Content-Type: application/json\r\nAccept: application/json\r\n");
}
static std::string UrlEnc(const std::string& s) {
    static const char* h = "0123456789ABCDEF"; std::string r;
    for (unsigned char c : s) { if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += (char)c; else { r += '%'; r += h[c >> 4]; r += h[c & 15]; } }
    return r;
}
static std::string Form(std::initializer_list<std::pair<std::string, std::string>> kv) {
    std::string r; for (auto& [k, v] : kv) { if (!r.empty()) r += '&'; r += k + "=" + UrlEnc(v); } return r;
}
static const char* kFormHdr = "Content-Type: application/x-www-form-urlencoded\r\n";

static bool Download(const std::string& url, const fs::path& dst, bool force = false) {
    std::error_code ec;
    if (!force && fs::exists(dst, ec) && fs::file_size(dst, ec) > 0) return true;
    for (int i = 0; i < 3; i++) {
        Resp r = Http(url);
        if (r.status == 200 && !r.body.empty()) { WriteAll(dst, r.body); return true; }
        Sleep(300);
    }
    return false;
}
struct Job { std::string url; fs::path dst; };
static void DownloadAll(const std::vector<Job>& jobs, float p0, float p1) { // 8 потоков
    size_t total = jobs.size(); if (!total) return;
    std::atomic<size_t> idx{0}, done{0};
    auto worker = [&] {
        for (;;) {
            size_t i = idx++; if (i >= total) break;
            Download(jobs[i].url, jobs[i].dst);
            size_t d = ++done; if (d % 16 == 0 || d == total) gProg = p0 + (p1 - p0) * (float)d / (float)total;
        }
    };
    std::vector<std::thread> th; for (int i = 0; i < 8; i++) th.emplace_back(worker);
    for (auto& t : th) t.join();
}
static bool Unzip(const fs::path& zip, const fs::path& dst, bool skipMeta = false) {
    std::string data = ReadAll(zip); if (data.empty()) return false;
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, data.data(), data.size(), 0)) return false;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&z, i, &st) || mz_zip_reader_is_file_a_directory(&z, i)) continue;
        std::string name = st.m_filename;
        if (name.find("..") != std::string::npos) continue;
        if (skipMeta && name.rfind("META-INF", 0) == 0) continue;
        size_t sz = 0; void* p = mz_zip_reader_extract_to_heap(&z, i, &sz, 0);
        if (!p) continue;
        WriteAll(dst / P(name), std::string((const char*)p, sz));
        mz_free(p);
    }
    mz_zip_reader_end(&z);
    return true;
}

// ============================ Java ============================
struct JavaInfo { std::string exe; int major; };
static int JavaMajorFromHome(const fs::path& home) {
    std::string rel = ReadAll(home / "release");
    auto p = rel.find("JAVA_VERSION=\"");
    if (p == std::string::npos) return 0;
    std::string v = rel.substr(p + 14, 16);
    int a = atoi(v.c_str());
    if (a == 1) { auto d = v.find('.'); return d == std::string::npos ? 0 : atoi(v.c_str() + d + 1); }
    return a;
}
static std::vector<JavaInfo> FindJavas() {
    std::vector<fs::path> homes; std::error_code ec;
    auto add = [&](const fs::path& h) { if (fs::exists(h / "bin" / "java.exe", ec)) homes.push_back(h); };
    auto scan = [&](const fs::path& d) { if (!fs::is_directory(d, ec)) return; for (auto& e : fs::directory_iterator(d, ec)) if (e.is_directory(ec)) add(e.path()); };
    for (auto v : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        std::wstring pfw = Env(v); if (pfw.empty()) continue; fs::path pf = pfw;
        for (auto ven : {L"Java", L"Eclipse Adoptium", L"Microsoft", L"Zulu", L"BellSoft", L"Amazon Corretto", L"AdoptOpenJDK", L"Semeru", L"Eclipse Foundation"})
            scan(pf / ven);
    }
    std::wstring jh = Env(L"JAVA_HOME"); if (!jh.empty()) add(fs::path(jh));
    for (auto& e : fs::directory_iterator(gRoot / "runtime", ec)) scan(e.path());
    std::wstring ad = Env(L"APPDATA");
    fs::path mc = fs::path(ad) / ".minecraft" / "runtime"; // Java от официального лаунчера
    if (!ad.empty() && fs::is_directory(mc, ec))
        for (auto it = fs::recursive_directory_iterator(mc, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->path().filename() == L"java.exe") homes.push_back(it->path().parent_path().parent_path());
        }
    std::set<std::string> seen; std::vector<JavaInfo> out;
    for (auto& h : homes) {
        std::string key = PS(h); if (!seen.insert(key).second) continue;
        int m = JavaMajorFromHome(h); if (m <= 0) continue;
        out.push_back({PS(h / "bin" / "java.exe"), m});
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.major < b.major; });
    return out;
}
static std::string PickJava(int need) {
    auto js = FindJavas(); std::string best; int bm = 999;
    for (auto& j : js) if (j.major >= need && j.major < bm) { bm = j.major; best = j.exe; }
    return best;
}
static std::string EnsureJava(int need) {
    std::string j = PickJava(need); if (!j.empty()) return j;
    Status("Скачиваю Java " + std::to_string(need) + " (Temurin)...");
    fs::path zip = gRoot / "runtime" / ("temurin-" + std::to_string(need) + ".zip");
    std::string url = "https://api.adoptium.net/v3/binary/latest/" + std::to_string(need) + "/ga/windows/x64/jre/hotspot/normal/eclipse";
    if (!Download(url, zip, true)) return "";
    Unzip(zip, gRoot / "runtime" / std::to_string(need));
    std::error_code ec; fs::remove(zip, ec);
    return PickJava(need);
}
static std::string JavaExe(std::string p) { // javaw.exe -> java.exe (нужен вывод в консоль)
    auto k = p.rfind("javaw.exe"); if (k != std::string::npos) p.replace(k, 9, "java.exe"); return p;
}

// ============================ Версии ============================
struct Ver { std::string id, type, url; };
static std::vector<Ver> gVers; static std::string gLatest; static std::mutex gVerMu;

static void FetchVersions() {
    fs::path c = gRoot / "version_manifest.json";
    Resp r = Http("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json");
    std::string body = (r.status == 200) ? r.body : ReadAll(c);
    if (r.status == 200) WriteAll(c, body);
    json j = Parse(body);
    if (!j.contains("versions")) { Status("Нет интернета: список версий недоступен"); return; }
    std::lock_guard<std::mutex> l(gVerMu);
    gLatest = j["latest"].value("release", "");
    for (auto& v : j["versions"]) gVers.push_back({v.value("id", ""), v.value("type", ""), v.value("url", "")});
}
static std::set<std::string> gFabricGames, gForgeGames; // какие версии поддерживают загрузчики (пусто = не удалось получить, показываем все)
static void FetchLoaderLists() {
    std::set<std::string> fab, forge;
    Resp r = Http("https://meta.fabricmc.net/v2/versions/game");
    json j = Parse(r.body);
    if (j.is_array()) for (auto& x : j) fab.insert(x.value("version", ""));
    r = Http("https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json");
    j = Parse(r.body);
    if (j.is_object() && j.contains("promos") && j["promos"].is_object())
        for (auto it = j["promos"].begin(); it != j["promos"].end(); ++it) { size_t p = it.key().rfind('-'); if (p != std::string::npos) forge.insert(it.key().substr(0, p)); }
    std::lock_guard<std::mutex> l(gVerMu); gFabricGames = fab; gForgeGames = forge;
}
static bool RuleOk(const json& rules) {
    if (!rules.is_array() || rules.empty()) return true;
    bool ok = false;
    for (auto& r : rules) {
        bool m = true;
        if (r.contains("os")) {
            auto& o = r["os"];
            if (o.contains("name") && o["name"] != "windows") m = false;
            if (o.contains("arch") && o["arch"] == "x86") m = false;
        }
        if (r.contains("features")) m = false; // demo / custom resolution и т.п. — выключено
        if (m) ok = (r.value("action", "allow") == "allow");
    }
    return ok;
}
static json Merge(json p, const json& c) {
    for (auto it = c.begin(); it != c.end(); ++it) {
        const std::string& k = it.key(); const json& v = it.value();
        if (k == "libraries") { json l = v; for (auto& x : p["libraries"]) l.push_back(x); p["libraries"] = l; }
        else if (k == "arguments") { for (auto key : {"game", "jvm"}) if (v.contains(key)) for (auto& x : v[key]) p["arguments"][key].push_back(x); }
        else if (k != "inheritsFrom") p[k] = v;
    }
    return p;
}
static json LoadVersionJson(const std::string& id, int depth = 0) {
    fs::path p = gRoot / "versions" / P(id) / P(id + ".json");
    json j = Parse(ReadAll(p));
    if (j.is_discarded() || !j.is_object()) {
        std::string url;
        { std::lock_guard<std::mutex> l(gVerMu); for (auto& v : gVers) if (v.id == id) url = v.url; }
        if (url.empty()) return json();
        Resp r = Http(url); if (r.status != 200) return json();
        WriteAll(p, r.body); j = Parse(r.body);
        if (!j.is_object()) return json();
    }
    if (j.contains("inheritsFrom") && depth < 4) {
        json par = LoadVersionJson(j["inheritsFrom"].get<std::string>(), depth + 1);
        if (par.is_null()) return json();
        return Merge(par, j);
    }
    j["_base"] = j.value("id", id);
    return j;
}
static std::string MavenPath(std::string n) {
    std::string ext = "jar"; auto at = n.find('@');
    if (at != std::string::npos) { ext = n.substr(at + 1); n = n.substr(0, at); }
    auto p = Split(n, ':'); if (p.size() < 3) return "";
    std::string g = p[0]; std::replace(g.begin(), g.end(), '.', '/');
    std::string f = p[1] + "-" + p[2]; if (p.size() > 3) f += "-" + p[3];
    return g + "/" + p[1] + "/" + p[2] + "/" + f + "." + ext;
}
static std::string LibKey(const std::string& n) { auto p = Split(n, ':'); if (p.size() < 2) return n; return p[0] + ":" + p[1] + (p.size() > 3 ? ":" + p[3] : ""); }

struct Prep { std::string cp, assetsId, gameAssets; fs::path natives; };
static void Prepare(const json& v, const std::string& launchId, const fs::path& gameDir, Prep& o) {
    std::string base = v.value("_base", launchId);
    fs::path jar = gRoot / "versions" / P(base) / P(base + ".jar");
    if (v.contains("downloads") && v["downloads"].contains("client")) {
        Status("Проверяю клиент " + base);
        if (!Download(v["downloads"]["client"].value("url", ""), jar)) throw std::runtime_error("Не удалось скачать клиент игры");
    }
    std::vector<Job> jobs; std::vector<fs::path> nativeJars; std::set<std::string> seen; std::vector<std::string> cp;
    for (auto& lib : v["libraries"]) {
        if (!RuleOk(lib.contains("rules") ? lib["rules"] : json())) continue;
        std::string name = lib.value("name", ""), path, url;
        bool hasDl = lib.contains("downloads");
        if (hasDl && lib["downloads"].contains("artifact")) { auto& a = lib["downloads"]["artifact"]; path = a.value("path", ""); url = a.value("url", ""); }
        else if (!lib.contains("natives") && !name.empty()) { path = MavenPath(name); url = lib.value("url", "https://libraries.minecraft.net/") + path; }
        if (!path.empty() && seen.insert(name.empty() ? path : LibKey(name)).second) {
            fs::path f = gRoot / "libraries" / P(path);
            if (!fs::exists(f) && !url.empty()) jobs.push_back({url, f});
            cp.push_back(PS(f));
        }
        if (lib.contains("natives") && lib["natives"].contains("windows") && hasDl && lib["downloads"].contains("classifiers")) {
            std::string c = lib["natives"]["windows"].get<std::string>();
            auto k = c.find("${arch}"); if (k != std::string::npos) c.replace(k, 7, "64");
            auto& cls = lib["downloads"]["classifiers"];
            if (cls.contains(c)) {
                fs::path f = gRoot / "libraries" / P(cls[c].value("path", ""));
                if (!fs::exists(f)) jobs.push_back({cls[c].value("url", ""), f});
                nativeJars.push_back(f);
            }
        }
    }
    Status("Скачиваю библиотеки: " + std::to_string(jobs.size()) + " файлов");
    DownloadAll(jobs, 0.05f, 0.3f);
    o.natives = gRoot / "versions" / P(launchId) / "natives";
    std::error_code ec; fs::create_directories(o.natives, ec);
    for (auto& nj : nativeJars) Unzip(nj, o.natives, true);

    // ассеты
    o.assetsId = v["assetIndex"].value("id", "legacy");
    fs::path ip = gRoot / "assets" / "indexes" / P(o.assetsId + ".json");
    Download(v["assetIndex"].value("url", ""), ip);
    json idx = Parse(ReadAll(ip));
    o.gameAssets = PS(gRoot / "assets");
    if (idx.contains("objects")) {
        bool virt = idx.value("virtual", false), mapRes = idx.value("map_to_resources", false);
        std::vector<Job> aj;
        for (auto it = idx["objects"].begin(); it != idx["objects"].end(); ++it) {
            std::string h = it.value().value("hash", ""); if (h.size() < 2) continue;
            fs::path d = gRoot / "assets" / "objects" / h.substr(0, 2) / h;
            if (!fs::exists(d, ec)) aj.push_back({"https://resources.download.minecraft.net/" + h.substr(0, 2) + "/" + h, d});
        }
        Status("Скачиваю ресурсы игры: " + std::to_string(aj.size()) + " файлов");
        DownloadAll(aj, 0.3f, 0.95f);
        if (virt || mapRes) {
            fs::path dir = virt ? gRoot / "assets" / "virtual" / "legacy" : gameDir / "resources";
            for (auto it = idx["objects"].begin(); it != idx["objects"].end(); ++it) {
                std::string h = it.value().value("hash", ""); if (h.size() < 2) continue;
                fs::path dst = dir / P(it.key());
                if (!fs::exists(dst, ec)) { fs::create_directories(dst.parent_path(), ec); fs::copy_file(gRoot / "assets" / "objects" / h.substr(0, 2) / h, dst, ec); }
            }
            if (virt) o.gameAssets = PS(dir);
        }
    }
    cp.push_back(PS(jar));
    for (size_t i = 0; i < cp.size(); i++) o.cp += (i ? ";" : "") + cp[i];
}

// ============================ Fabric / Forge ============================
static std::string FindInstalled(const std::string& prefix, const std::string& suffix) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(gRoot / "versions", ec)) {
        std::string n = U8(e.path().filename().wstring());
        if (n.rfind(prefix, 0) == 0 && n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0 && fs::exists(e.path() / (e.path().filename().wstring() + L".json"), ec)) return n;
    }
    return "";
}
static std::string InstallFabric(const std::string& mc) {
    Status("Устанавливаю Fabric...");
    Resp r = Http("https://meta.fabricmc.net/v2/versions/loader");
    json j = Parse(r.body); std::string loader;
    if (j.is_array()) for (auto& x : j) if (x.value("stable", false)) { loader = x.value("version", ""); break; }
    if (loader.empty()) return FindInstalled("fabric-loader-", "-" + mc);
    r = Http("https://meta.fabricmc.net/v2/versions/loader/" + mc + "/" + loader + "/profile/json");
    json pj = Parse(r.body);
    if (r.status != 200 || !pj.contains("id")) throw std::runtime_error("Fabric пока не поддерживает версию " + mc);
    std::string id = pj["id"].get<std::string>();
    WriteAll(gRoot / "versions" / P(id) / P(id + ".json"), r.body);
    return id;
}
static int RunProcess(const std::string& cmd, const fs::path& cwd, std::string& captured);
static std::string InstallForge(const std::string& mc, const std::string& javaExe) {
    Status("Устанавливаю Forge (первый раз это долго)...");
    Resp r = Http("https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json");
    json j = Parse(r.body); std::string fv;
    if (j.contains("promos")) {
        auto& pr = j["promos"];
        if (pr.contains(mc + "-recommended")) fv = pr[mc + "-recommended"].get<std::string>();
        else if (pr.contains(mc + "-latest")) fv = pr[mc + "-latest"].get<std::string>();
    }
    if (fv.empty()) { std::string have = FindInstalled(mc + "-forge-", ""); if (!have.empty()) return have; throw std::runtime_error("Для Minecraft " + mc + " не нашёл Forge"); }
    std::string id = mc + "-forge-" + fv;
    std::error_code ec;
    if (fs::exists(gRoot / "versions" / P(id) / P(id + ".json"), ec)) return id;
    std::string full = mc + "-" + fv;
    fs::path inst = gRoot / "cache" / P("forge-" + full + "-installer.jar");
    if (!Download("https://maven.minecraftforge.net/net/minecraftforge/forge/" + full + "/forge-" + full + "-installer.jar", inst))
        throw std::runtime_error("Не скачался установщик Forge");
    if (!fs::exists(gRoot / "launcher_profiles.json")) WriteAll(gRoot / "launcher_profiles.json", "{\"profiles\":{}}");
    std::string cap;
    int code = RunProcess(Q(javaExe) + " -jar " + Q(PS(inst)) + " --installClient " + Q(PS(gRoot)), gRoot, cap);
    if (!fs::exists(gRoot / "versions" / P(id) / P(id + ".json"), ec)) {
        std::string have = FindInstalled(mc + "-forge-", "");
        if (!have.empty()) return have;
        throw std::runtime_error("Установщик Forge завершился с кодом " + std::to_string(code) + ". Смотри вкладку «Лог».");
    }
    return id;
}

// ============================ Запуск процесса и разбор крашей ============================
static int RunProcess(const std::string& cmd, const fs::path& cwd, std::string& captured) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cl = W(cmd);
    BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.wstring().c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); captured = "CreateProcess failed: " + std::to_string(GetLastError()); return -1; }
    std::string part; char buf[4096]; DWORD n;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) {
        part.append(buf, n); size_t p;
        while ((p = part.find('\n')) != std::string::npos) {
            std::string line = part.substr(0, p); if (!line.empty() && line.back() == '\r') line.pop_back();
            Log(line); captured += line + "\n"; part.erase(0, p + 1);
        }
        if (captured.size() > 400000) captured.erase(0, 200000);
    }
    if (!part.empty()) { Log(part); captured += part; }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    return (int)code;
}
static std::pair<std::string, std::string> Analyze(int code, const std::string& t) {
    auto has = [&](const char* s) { return t.find(s) != std::string::npos; };
    if (has("UnsupportedClassVersionError") || has("class file version")) {
        int need = 0; auto p = t.find("class file version "); if (p != std::string::npos) { int v = atoi(t.c_str() + p + 19); need = v > 44 ? v - 44 : 0; }
        return {"Не та версия Java", "Игре или моду нужна Java " + (need ? std::to_string(need) : std::string("новее")) + ". В «Настройках» выбери «Авто» или подходящую Java."};
    }
    if (has("OutOfMemoryError") || has("Java heap space") || has("Could not reserve enough space"))
        return {"Не хватает памяти", "Игре не хватило оперативной памяти. Подними лимит в «Настройках» (для модов обычно 4–6 ГБ) и закрой тяжёлые программы. Если Java 32-бит — поставь 64-бит."};
    if (has("Unrecognized VM option") || has("Could not create the Java Virtual Machine") || has("Invalid maximum heap size"))
        return {"Java не смогла стартовать", "JVM отказалась запускаться: проблема в аргументах или в 32-битной Java. Попробуй «Авто» для Java и меньше ОЗУ."};
    if (has("Found duplicate mods") || has("DuplicateModsFoundException") || has("Duplicate mod"))
        return {"Дубли модов", "В папке mods лежат две копии одного мода. Оставь одну."};
    if (has("Incompatible mod set") || has("Mod resolution failed") || has("Missing or unsupported mandatory dependencies") || has("requires") && has("mod"))
        return {"Конфликт или нехватка зависимостей модов", "Какому-то моду нужна другая версия игры/загрузчика или другой мод (часто Fabric API). Прочитай в логе строки со словом «requires» или «Missing»."};
    if (has("Mixin apply failed") || has("MixinTransformerError") || has("InvalidInjectionException"))
        return {"Мод сломал игру (Mixin)", "Один из модов не совместим с этой версией или с другим модом. Убирай моды по одному, начиная с последних добавленных."};
    if (has("NoClassDefFoundError") || has("ClassNotFoundException"))
        return {"Не найден класс (мод/библиотека)", "Чаще всего мод собран не под эту версию игры или не под этот загрузчик (Fabric/Forge). Проверь версии модов."};
    if (has("UnsatisfiedLinkError") || has("Failed to locate library") || has("lwjgl") && has("natives"))
        return {"Сломались нативные библиотеки", "Удали папку versions/<версия>/natives в папке лаунчера и запусти заново."};
    if (has("GLFW error") || has("Pixel format not accelerated") || has("WGL") || has("Failed to create window") || has("OpenGL"))
        return {"Проблема с видеокартой / OpenGL", "Обнови драйвер видеокарты. На ноутбуке запускай Java на дискретной GPU. Не работает в удалённом рабочем столе."};
    if (has("UnknownHostException") || has("SSLHandshakeException") || has("Connection timed out") || has("Failed to download"))
        return {"Проблема с сетью", "Не получилось скачать что-то по сети. Проверь интернет, VPN, антивирус/фаервол."};
    if (has("Invalid session") || has("Failed to verify username") || has("Bad login"))
        return {"Проблема с аккаунтом", "Сессия недействительна. Перелогинься во вкладке «Аккаунт»."};
    if (code == -1073741819 || code == (int)0xC0000005)
        return {"Аварийное закрытие процесса (0xC0000005)", "Чаще всего виноваты драйвер видеокарты, оверлеи (Discord, MSI Afterburner, GeForce Experience) или антивирус. Отключи оверлеи и обнови драйвер."};
    std::string hint; size_t p = t.find("Exception in thread");
    if (p == std::string::npos) p = t.find("Caused by:");
    if (p != std::string::npos) { size_t e = t.find('\n', p); hint = "\n\nПохоже на:\n" + t.substr(p, std::min<size_t>(e == std::string::npos ? 300 : e - p, 300)); }
    return {"Игра закрылась с кодом " + std::to_string(code), "Точная причина не опознана. Открой вкладку «Лог» и ищи строки с Exception / Error." + hint};
}

// ============================ Аккаунты ============================
static bool ValidNick(const std::string& n) {
    if (n.size() < 3 || n.size() > 16) return false;
    for (unsigned char c : n) if (!(isalnum(c) || c == '_')) return false;
    return true;
}
static const char* kNickHint = "Ник: 3-16 символов, только латиница, цифры и _";
static std::string JStr(const json& j, const char* k) { return (j.is_object() && j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string(); }
static std::string FirstLine(const std::string& s) { size_t p = s.find_first_of("\r\n"); return p == std::string::npos ? s : s.substr(0, p); }
static std::string OfflineUuid(const std::string& name) {
    std::string s = "OfflinePlayer:" + name; HCRYPTPROV pv = 0; HCRYPTHASH h = 0; BYTE d[16] = {0}; DWORD n = 16;
    CryptAcquireContextW(&pv, 0, 0, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(pv, CALG_MD5, 0, 0, &h);
    CryptHashData(h, (const BYTE*)s.data(), (DWORD)s.size(), 0);
    CryptGetHashParam(h, HP_HASHVAL, d, &n, 0);
    CryptDestroyHash(h); CryptReleaseContext(pv, 0);
    d[6] = (d[6] & 0x0f) | 0x30; d[8] = (d[8] & 0x3f) | 0x80;
    static const char* hx = "0123456789abcdef"; std::string r;
    for (int i = 0; i < 16; i++) { r += hx[d[i] >> 4]; r += hx[d[i] & 15]; }
    return r;
}
static Account MsChain(const std::string& msTok, const std::string& refresh) {
    json b1 = {{"Properties", {{"AuthMethod", "RPS"}, {"SiteName", "user.auth.xboxlive.com"}, {"RpsTicket", "d=" + msTok}}}, {"RelyingParty", "http://auth.xboxlive.com"}, {"TokenType", "JWT"}};
    Resp r = PostJson("https://user.auth.xboxlive.com/user/authenticate", b1);
    if (r.status == 0) throw std::runtime_error("Нет соединения с Xbox Live (интернет / VPN / фаервол)");
    json x = Parse(r.body);
    if (!x.contains("Token")) throw std::runtime_error("Xbox Live: ошибка " + std::to_string(r.status));
    std::string xbl = x["Token"].get<std::string>(), uhs = x["DisplayClaims"]["xui"][0]["uhs"].get<std::string>();
    json b2 = {{"Properties", {{"SandboxId", "RETAIL"}, {"UserTokens", json::array({xbl})}}}, {"RelyingParty", "rp://api.minecraftservices.com/"}, {"TokenType", "JWT"}};
    r = PostJson("https://xsts.auth.xboxlive.com/xsts/authorize", b2);
    x = Parse(r.body);
    if (!x.contains("Token")) {
        std::string why = "XSTS: ошибка " + std::to_string(r.status);
        if (x.contains("XErr")) { long long e = x["XErr"].get<long long>(); if (e == 2148916233LL) why = "У этой учётки Microsoft нет профиля Xbox"; else if (e == 2148916238LL) why = "Детская учётка: нужно разрешение родителя в семейной группе"; else if (e == 2148916235LL) why = "Xbox Live недоступен в стране этой учётки"; else if (e == 2148916236LL || e == 2148916237LL) why = "Нужна проверка возраста учётки Xbox (account.xbox.com)"; }
        throw std::runtime_error(why);
    }
    std::string xsts = x["Token"].get<std::string>();
    r = PostJson("https://api.minecraftservices.com/authentication/login_with_xbox", json{{"identityToken", "XBL3.0 x=" + uhs + ";" + xsts}});
    x = Parse(r.body);
    if (!x.contains("access_token")) {
        std::string em = JStr(x, "errorMessage");
        if (r.status == 403 || em.find("Invalid app registration") != std::string::npos)
            throw std::runtime_error("Mojang не одобрил этот Azure Client ID (Invalid app registration). Подай заявку: https://aka.ms/mce-reviewappid - до одобрения вход через Microsoft не заработает.");
        throw std::runtime_error("Minecraft Services: ошибка " + std::to_string(r.status) + (em.empty() ? "" : " (" + em + ")"));
    }
    std::string tok = x["access_token"].get<std::string>();
    r = Http("https://api.minecraftservices.com/minecraft/profile", "GET", "", "Authorization: Bearer " + tok + "\r\n");
    x = Parse(r.body);
    if (!x.contains("name")) throw std::runtime_error("На этой учётке нет купленной Minecraft Java Edition");
    Account a; a.type = 2; a.name = x["name"].get<std::string>(); a.uuid = x["id"].get<std::string>(); a.token = tok; a.refresh = refresh;
    return a;
}
static void MsRefresh(Account& a) {
    if (a.refresh.empty() || MsId().empty()) throw std::runtime_error("Нужно заново войти через Microsoft");
    Resp r = Http("https://login.microsoftonline.com/consumers/oauth2/v2.0/token", "POST",
                  Form({{"client_id", MsId()}, {"grant_type", "refresh_token"}, {"refresh_token", a.refresh}, {"scope", "XboxLive.signin offline_access"}}), kFormHdr);
    json j = Parse(r.body);
    if (!j.contains("access_token")) throw std::runtime_error("Сессия Microsoft истекла — войди заново");
    a = MsChain(j["access_token"].get<std::string>(), j.value("refresh_token", a.refresh));
}
static void MsLoginThread() {
    if (gAuthBusy.exchange(true)) return;
    struct Done { ~Done() { gAuthBusy = false; } } done_;
    try {
        if (MsId().empty()) throw std::runtime_error("Не задан Azure Client ID — вставь его в поле на вкладке «Аккаунт»");
        Status("Microsoft: запрашиваю код...");
        Resp r = Http("https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode", "POST",
                      Form({{"client_id", MsId()}, {"scope", "XboxLive.signin offline_access"}}), kFormHdr);
        json d = Parse(r.body);
        if (!d.contains("device_code")) {
            std::string ed = FirstLine(JStr(d, "error_description"));
            throw std::runtime_error("Microsoft не выдал код" + (ed.empty() ? std::string(". Проверь Client ID.") : ": " + ed));
        }
        std::string uri = d.value("verification_uri", "https://microsoft.com/link");
        { std::lock_guard<std::mutex> l(gMu); gMsCode = d.value("user_code", ""); gMsUri = uri; }
        ShellExecuteW(0, L"open", W(uri).c_str(), 0, 0, SW_SHOWNORMAL);
        int interval = d.value("interval", 5), left = d.value("expires_in", 900);
        while (left > 0) {
            Sleep(interval * 1000); left -= interval;
            Resp t = Http("https://login.microsoftonline.com/consumers/oauth2/v2.0/token", "POST",
                          Form({{"client_id", MsId()}, {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}, {"device_code", d["device_code"].get<std::string>()}}), kFormHdr);
            json j = Parse(t.body);
            if (j.contains("access_token")) {
                Status("Microsoft: вхожу в Xbox / Minecraft...");
                Account a = MsChain(j["access_token"].get<std::string>(), j.value("refresh_token", ""));
                { std::lock_guard<std::mutex> l(gMu); gNewAcc = a; gMsCode.clear(); }
                Status("Вошёл как " + a.name); return;
            }
            std::string e = j.value("error", "");
            if (e == "slow_down") interval += 5;
            else if (e != "authorization_pending") { std::string ed = FirstLine(JStr(j, "error_description")); throw std::runtime_error("Вход отменён или истёк (" + e + ")" + (ed.empty() ? "" : ": " + ed)); }
        }
        throw std::runtime_error("Время ожидания кода вышло");
    } catch (std::exception& e) { { std::lock_guard<std::mutex> l(gMu); gMsCode.clear(); } Status(std::string("Ошибка входа: ") + e.what()); }
}
static void ElyRefresh(Account& a) {
    Resp r = PostJson("https://authserver.ely.by/auth/validate", json{{"accessToken", a.token}});
    if (r.status == 200 || r.status == 204 || r.status == 0) return; // валиден (или нет сети - пробуем как есть)
    if (a.refresh.empty()) throw std::runtime_error("Сессия Ely.by истекла - войди заново во вкладке «Аккаунт»");
    r = PostJson("https://authserver.ely.by/auth/refresh", json{{"accessToken", a.token}, {"clientToken", a.refresh}});
    json j = Parse(r.body);
    if (!j.contains("accessToken")) throw std::runtime_error("Сессия Ely.by истекла - войди заново во вкладке «Аккаунт»");
    a.token = j["accessToken"].get<std::string>();
}
static void ElyLoginThread(std::string u, std::string p) {
    if (gAuthBusy.exchange(true)) return;
    struct Done { ~Done() { gAuthBusy = false; } } done_;
    try {
        Status("Ely.by: вхожу...");
        std::string ct = RandHex(16);
        Resp r = PostJson("https://authserver.ely.by/auth/authenticate", json{{"username", u}, {"password", p}, {"clientToken", ct}, {"requestUser", true}});
        if (r.status == 0) throw std::runtime_error("Нет соединения с authserver.ely.by (интернет / VPN / фаервол)");
        json j = Parse(r.body);
        if (!j.contains("accessToken")) {
            std::string em = JStr(j, "errorMessage");
            if (em.find("two factor") != std::string::npos || em.find("two-factor") != std::string::npos) em = "включена 2FA - пиши в поле пароля «пароль:код»";
            throw std::runtime_error("Ely.by: " + (em.empty() ? "ошибка " + std::to_string(r.status) : em));
        }
        if (!j.contains("selectedProfile") || !j["selectedProfile"].is_object()) throw std::runtime_error("У аккаунта Ely.by нет выбранного профиля");
        Account a; a.type = 1; a.token = j["accessToken"].get<std::string>(); a.refresh = ct;
        a.name = j["selectedProfile"].value("name", u); a.uuid = j["selectedProfile"].value("id", "");
        { std::lock_guard<std::mutex> l(gMu); gNewAcc = a; }
        Status("Вошёл как " + a.name + " (Ely.by)");
    } catch (std::exception& e) { Status(std::string("Ошибка входа: ") + e.what()); }
}

// ============================ Конфиг ============================
struct Config {
    std::string loader = "Vanilla", version, javaPath, bg, shotsDir;
    std::string gameDir; // пусто = стандартная
    bool snapshots = false, showBeta = false, showAlpha = false, slideshow = true;
    int ramMB = 4096, sel = -1, onLaunch = 0; // onLaunch: 0 оставить, 1 свернуть, 2 закрыть
    std::vector<Account> accs;
};
static Config cfg;
static void SaveConfig() {
    json j; j["loader"] = cfg.loader; j["version"] = cfg.version; j["java"] = cfg.javaPath; j["bg"] = cfg.bg; j["shots"] = cfg.shotsDir;
    j["snap"] = cfg.snapshots; j["slide"] = cfg.slideshow; j["ram"] = cfg.ramMB; j["sel"] = cfg.sel; j["msid"] = MsId(); j["gamedir"] = cfg.gameDir; j["beta"] = cfg.showBeta; j["alpha"] = cfg.showAlpha; j["onlaunch"] = cfg.onLaunch;
    for (auto& a : cfg.accs) j["accs"].push_back({{"type", a.type}, {"name", a.name}, {"uuid", a.uuid}, {"token", a.token}, {"refresh", a.refresh}});
    WriteAll(gCfgDir / "config.json", j.dump(2));
}
static void LoadConfig() {
    json j = Parse(ReadAll(gCfgDir / "config.json")); if (!j.is_object()) return;
    try {
        cfg.loader = j.value("loader", "Vanilla"); cfg.version = j.value("version", ""); cfg.javaPath = j.value("java", ""); cfg.bg = j.value("bg", ""); cfg.shotsDir = j.value("shots", "");
        cfg.snapshots = j.value("snap", false); cfg.slideshow = j.value("slide", true); cfg.ramMB = j.value("ram", 4096); cfg.sel = j.value("sel", -1);
        { std::string id = j.value("msid", ""); if (!id.empty()) SetMsId(id); }
        cfg.gameDir = j.value("gamedir", ""); cfg.showBeta = j.value("beta", false); cfg.showAlpha = j.value("alpha", false); cfg.onLaunch = j.value("onlaunch", 0);
        if (j.contains("accs")) for (auto& a : j["accs"]) cfg.accs.push_back({a.value("type", 0), a.value("name", ""), a.value("uuid", ""), a.value("token", ""), a.value("refresh", "")});
    } catch (...) {}
}

static void SetGameDir(const std::string& d) { // пустая строка = стандартная папка
    if (gBusy) return;
    fs::path np = d.empty() ? gCfgDir : P(d);
    std::error_code ec; fs::create_directories(np, ec);
    if (ec || !fs::is_directory(np, ec)) { Status("Не удалось использовать эту папку"); return; }
    gRoot = np; cfg.gameDir = (np == gCfgDir) ? std::string() : d; SaveConfig();
    Status("Папка игры: " + PS(gRoot));
}

// ============================ Запуск игры ============================
static void LaunchThread(Config c, Account acc) {
    gBusy = true; gProg = 0;
    try {
        if (acc.type == 2) { Status("Обновляю сессию Microsoft..."); MsRefresh(acc); { std::lock_guard<std::mutex> l(gMu); gNewAcc = acc; } }
        else if (acc.type == 1) { Status("Проверяю сессию Ely.by..."); ElyRefresh(acc); { std::lock_guard<std::mutex> l(gMu); gNewAcc = acc; } }
        Status("Получаю описание версии " + c.version);
        json v = LoadVersionJson(c.version);
        if (v.is_null()) throw std::runtime_error("Не удалось получить версию " + c.version + " (нет интернета?)");
        int need = v.contains("javaVersion") ? v["javaVersion"].value("majorVersion", 8) : 8;
        std::string javaPick = c.javaPath.empty() ? EnsureJava(need) : JavaExe(c.javaPath);
        if (javaPick.empty()) throw std::runtime_error("Java не найдена и не скачалась");
        std::string launchId = c.version;
        fs::path tmpGame = gRoot / "instances" / P(c.version);
        Prep pre; Prepare(v, c.version, tmpGame, pre); // сначала ваниль (нужна и Forge-установщику)
        if (c.loader == "Fabric") launchId = InstallFabric(c.version);
        else if (c.loader == "Forge") launchId = InstallForge(c.version, javaPick);
        if (launchId.empty()) throw std::runtime_error("Не удалось установить " + c.loader);
        json lv = launchId == c.version ? v : LoadVersionJson(launchId);
        if (lv.is_null()) throw std::runtime_error("Не нашёл описание профиля " + launchId);
        int need2 = lv.contains("javaVersion") ? lv["javaVersion"].value("majorVersion", 8) : 8;
        std::string java = c.javaPath.empty() ? EnsureJava(need2) : javaPick;
        if (java.empty()) throw std::runtime_error("Java не найдена");
        fs::path gameDir = gRoot / "instances" / P(launchId);
        std::error_code ec; fs::create_directories(gameDir, ec);
        Prep pp; Prepare(lv, launchId, gameDir, pp);

        std::map<std::string, std::string> vars = {
            {"auth_player_name", acc.name}, {"version_name", launchId}, {"game_directory", PS(gameDir)}, {"assets_root", PS(gRoot / "assets")},
            {"assets_index_name", pp.assetsId}, {"auth_uuid", acc.uuid}, {"auth_access_token", acc.type == 0 ? "0" : acc.token}, {"clientid", ""}, {"auth_xuid", ""},
            {"user_type", acc.type == 2 ? "msa" : (acc.type == 1 ? "mojang" : "legacy")}, {"version_type", lv.value("type", "release")},
            {"natives_directory", PS(pp.natives)}, {"launcher_name", "PoltergeistLauncher"}, {"launcher_version", "1.0"}, {"classpath", pp.cp},
            {"classpath_separator", ";"}, {"library_directory", PS(gRoot / "libraries")}, {"game_assets", pp.gameAssets},
            {"auth_session", acc.token}, {"user_properties", "{}"}};
        std::string base = lv.value("_base", c.version);
        auto addArgs = [&](const json& arr, std::vector<std::string>& out) {
            for (auto& a : arr) {
                auto push = [&](std::string s) {
                    if (s.rfind("-DignoreList", 0) == 0) { auto k = s.find("${version_name}"); if (k != std::string::npos) s.replace(k, 15, base); }
                    out.push_back(Sub(s, vars));
                };
                if (a.is_string()) push(a.get<std::string>());
                else if (a.is_object() && RuleOk(a.contains("rules") ? a["rules"] : json())) {
                    if (a["value"].is_string()) push(a["value"].get<std::string>());
                    else for (auto& x : a["value"]) push(x.get<std::string>());
                }
            }
        };
        std::vector<std::string> jvm, game;
        if (lv.contains("arguments")) { if (lv["arguments"].contains("jvm")) addArgs(lv["arguments"]["jvm"], jvm); if (lv["arguments"].contains("game")) addArgs(lv["arguments"]["game"], game); }
        else {
            jvm = {Sub("-Djava.library.path=${natives_directory}", vars), "-cp", pp.cp};
            for (auto& s : Split(lv.value("minecraftArguments", ""), ' ')) if (!s.empty()) game.push_back(Sub(s, vars));
        }
        std::string cmd = Q(JavaExe(java)) + " -Xmx" + std::to_string(c.ramMB) + "M -Xms" + std::to_string(std::min(c.ramMB, 512)) + "M -XX:+UseG1GC -XX:MaxGCPauseMillis=50";
        if (acc.type == 1) {
            fs::path ai = gRoot / "cache" / "authlib-injector.jar";
            if (!Download(kAuthlibUrl, ai)) throw std::runtime_error("Не скачался authlib-injector (нужен для Ely.by)");
            cmd += " " + Q("-javaagent:" + PS(ai) + "=https://authserver.ely.by/api/authlib-injector");
        }
        for (auto& s : jvm) cmd += " " + Q(s);
        cmd += " " + Q(lv.value("mainClass", "net.minecraft.client.main.Main"));
        for (auto& s : game) cmd += " " + Q(s);

        gProg = 1.0f; Status("Игра запущена (" + launchId + ")");
        gGameRunning = true; int act = c.onLaunch;
        if (act == 1 || act == 2) std::thread([act] { // свернуть/закрыть лаунчер, если игра успешно стартовала и не упала
            for (int i = 0; i < (act == 1 ? 8 : 40) && gGameRunning; i++) Sleep(500);
            if (!gGameRunning) return;
            if (act == 1) PostMessageW(gHwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0); else PostMessageW(gHwnd, WM_CLOSE, 0, 0);
        }).detach();
        std::string cap; int code = RunProcess(cmd, gameDir, cap);
        gGameRunning = false;
        if (act == 1) PostMessageW(gHwnd, WM_SYSCOMMAND, SC_RESTORE, 0); // игра закрылась — вернуть лаунчер
        WriteAll(gCfgDir / "latest.log", cap);
        if (code != 0) { auto [t, b] = Analyze(code, cap); Status("Игра завершилась с ошибкой: " + t); ShowError(t, b); }
        else Status("Игра закрыта");
    } catch (std::exception& e) {
        Status(std::string("Ошибка: ") + e.what()); ShowError("Не удалось запустить", e.what());
    }
    gBusy = false;
}
static bool AddOffline(const std::string& name) {
    if (!ValidNick(name)) { Status(kNickHint); return false; }
    for (int i = 0; i < (int)cfg.accs.size(); i++) if (cfg.accs[i].type == 0 && cfg.accs[i].name == name) { cfg.sel = i; SaveConfig(); return true; }
    Account a; a.name = name; a.uuid = OfflineUuid(name); cfg.accs.push_back(a); cfg.sel = (int)cfg.accs.size() - 1; SaveConfig();
    return true;
}
static void StartLaunch() {
    if (gBusy) return;
    if (cfg.accs.empty() && !AddOffline(bNick)) return; // нет аккаунтов - играем офлайн с ником из поля ввода
    if (cfg.sel < 0 || cfg.sel >= (int)cfg.accs.size()) cfg.sel = 0;
    SaveConfig();
    std::thread(LaunchThread, cfg, cfg.accs[cfg.sel]).detach();
}

// ============================ Фон: скриншоты ============================
static ID3D11Device* gDev; static ID3D11DeviceContext* gCtx; static IDXGISwapChain* gSwap; static ID3D11RenderTargetView* gRTV;
struct Tex { ID3D11ShaderResourceView* v = nullptr; int w = 0, h = 0; };
static Tex gBg[2]; static float gFade = 1, gSlideT = 0, gTime = 0;
static std::string gBgNow; static std::vector<std::string> gShots;
static std::mutex gImgMu; static std::vector<unsigned char> gImgData; static int gImgW, gImgH; static bool gImgReady = false;
static std::atomic<bool> gImgLoading{false};

static void LoadBgAsync(std::string path) {
    if (gImgLoading) return;
    gImgLoading = true; gBgNow = path;
    std::thread([path] {
        std::string bytes = ReadAll(P(path)); int w = 0, h = 0, ch = 0;
        unsigned char* d = bytes.empty() ? nullptr : stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &ch, 4);
        if (d) {
            int f = std::max(1, w / 1920); // быстрый box-downscale для 4K-скриншотов
            int nw = w / f, nh = h / f; std::vector<unsigned char> out((size_t)nw * nh * 4);
            for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) for (int c = 0; c < 4; c++) {
                int s = 0; for (int dy = 0; dy < f; dy++) for (int dx = 0; dx < f; dx++) s += d[(((size_t)y * f + dy) * w + (x * f + dx)) * 4 + c];
                out[((size_t)y * nw + x) * 4 + c] = (unsigned char)(s / (f * f));
            }
            stbi_image_free(d);
            std::lock_guard<std::mutex> l(gImgMu); gImgData = std::move(out); gImgW = nw; gImgH = nh; gImgReady = true;
        }
        gImgLoading = false;
    }).detach();
}
static ID3D11ShaderResourceView* MakeTex(const unsigned char* px, int w, int h) {
    D3D11_TEXTURE2D_DESC d{}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{px, (UINT)w * 4, 0}; ID3D11Texture2D* t = nullptr;
    if (FAILED(gDev->CreateTexture2D(&d, &sd, &t))) return nullptr;
    ID3D11ShaderResourceView* v = nullptr; gDev->CreateShaderResourceView(t, nullptr, &v); t->Release(); return v;
}
static void ScanShots() {
    gShots.clear(); std::error_code ec; std::vector<fs::path> dirs;
    if (!cfg.shotsDir.empty()) dirs.push_back(P(cfg.shotsDir));
    else dirs.push_back(fs::path(Env(L"APPDATA")) / ".minecraft" / "screenshots");
    for (auto& e : fs::directory_iterator(gRoot / "instances", ec)) dirs.push_back(e.path() / "screenshots");
    for (auto& d : dirs) for (auto& e : fs::directory_iterator(d, ec)) {
        std::wstring x = e.path().extension().wstring(); for (auto& ch : x) ch = (wchar_t)towlower(ch);
        if (x == L".png" || x == L".jpg" || x == L".jpeg" || x == L".bmp") gShots.push_back(PS(e.path()));
    }
}
static void NextShot() {
    if (gShots.empty()) return; static std::mt19937 rng{std::random_device{}()};
    for (int i = 0; i < 5; i++) { const std::string& s = gShots[rng() % gShots.size()]; if (s != gBgNow || gShots.size() == 1) { LoadBgAsync(s); return; } }
}
static void UpdateBackground(float dt) {
    {
        std::lock_guard<std::mutex> l(gImgMu);
        if (gImgReady) {
            if (gBg[1].v) gBg[1].v->Release();
            gBg[1] = gBg[0]; gBg[0] = {MakeTex(gImgData.data(), gImgW, gImgH), gImgW, gImgH};
            gImgData.clear(); gImgData.shrink_to_fit(); gImgReady = false; gFade = 0;
        }
    }
    gFade = std::min(1.f, gFade + dt * 1.2f);
    if (cfg.slideshow && gShots.size() > 1) { gSlideT += dt; if (gSlideT > 18.f) { gSlideT = 0; NextShot(); } }
}
static std::string PickFolder(HWND hw) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t disp[MAX_PATH] = {0};
    BROWSEINFOW bi{}; bi.hwndOwner = hw; bi.pszDisplayName = disp; bi.lpszTitle = L"Выбери папку для игры"; bi.ulFlags = BIF_NEWDIALOGSTYLE | BIF_RETURNONLYFSDIRS;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    std::string r;
    if (pidl) { wchar_t path[MAX_PATH]; if (SHGetPathFromIDListW(pidl, path)) r = U8(path); CoTaskMemFree(pidl); }
    return r;
}
static std::string PickImage(HWND hw) {
    wchar_t f[MAX_PATH] = {0}; OPENFILENAMEW o{}; o.lStructSize = sizeof(o); o.hwndOwner = hw;
    o.lpstrFilter = L"Изображения\0*.png;*.jpg;*.jpeg;*.bmp;*.tga\0Все файлы\0*.*\0"; o.lpstrFile = f; o.nMaxFile = MAX_PATH;
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o) ? U8(f) : "";
}

// ============================ UI ============================
static int gTab = 0; static float gTabFade = 1;
static const ImVec4 kAccent(0.33f, 0.78f, 0.40f, 1.f);
static const char* kTabs[] = {"Игра", "Аккаунт", "Настройки", "Лог"};

static float Anim(ImGuiID id, float target, float speed, float def = 0.f) {
    float& v = *ImGui::GetStateStorage()->GetFloatRef(id, def);
    v += (target - v) * std::min(1.f, ImGui::GetIO().DeltaTime * speed); return v;
}
static float AnimK(const char* k, float target, float speed, float def = 0.f) { return Anim(ImGui::GetID(k), target, speed, def); }

static bool FancyBtn(const char* label, ImVec2 sz, ImVec4 col, bool enabled = true) {
    ImGui::PushID(label);
    if (sz.x <= 0) sz.x = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clk = ImGui::InvisibleButton("##b", sz);
    bool hov = ImGui::IsItemHovered() && enabled, act = ImGui::IsItemActive() && enabled;
    float t = AnimK("h", hov ? 1.f : 0.f, 14.f), g = t * 2.5f - (act ? 2.5f : 0.f);
    ImVec4 c = col; if (!enabled) { c.x *= .45f; c.y *= .45f; c.z *= .45f; }
    c.x = std::min(1.f, c.x + .12f * t); c.y = std::min(1.f, c.y + .12f * t); c.z = std::min(1.f, c.z + .12f * t);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x - g, p.y - g), ImVec2(p.x + sz.x + g, p.y + sz.y + g), ImGui::GetColorU32(c), sz.y * 0.28f);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + (sz.x - ts.x) / 2, p.y + (sz.y - ts.y) / 2), IM_COL32(255, 255, 255, enabled ? 255 : 140), label);
    ImGui::PopID();
    return clk && enabled;
}
static void Open(const fs::path& p) { std::error_code ec; fs::create_directories(p, ec); ShellExecuteW(0, L"open", p.wstring().c_str(), 0, 0, SW_SHOWNORMAL); }
static std::string AccLabel(const Account& a) { return (a.type == 0 ? "[офлайн] " : a.type == 1 ? "[Ely.by] " : "[Microsoft] ") + a.name; }

static void TabBar() {
    float em = ImGui::GetFontSize(), h = em * 1.9f; ImVec2 s = ImGui::GetCursorScreenPos(); float x = s.x, tx = s.x, tw = 0;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 4; i++) {
        ImVec2 ts = ImGui::CalcTextSize(kTabs[i]); float w = ts.x + em * 1.2f;
        ImGui::SetCursorScreenPos(ImVec2(x, s.y)); ImGui::PushID(i);
        if (ImGui::InvisibleButton("##t", ImVec2(w, h)) && gTab != i) { gTab = i; gTabFade = 0; }
        float t = AnimK("h", (ImGui::IsItemHovered() || gTab == i) ? 1.f : 0.f, 12.f);
        dl->AddText(ImVec2(x + em * .6f, s.y + (h - ts.y) / 2), IM_COL32(255, 255, 255, (int)(125 + 130 * t)), kTabs[i]);
        if (gTab == i) { tx = x; tw = w; }
        x += w; ImGui::PopID();
    }
    float ux = AnimK("ux", tx, 12.f, tx), uw = AnimK("uw", tw, 12.f, tw);
    dl->AddRectFilled(ImVec2(ux + em * .4f, s.y + h), ImVec2(ux + uw - em * .4f, s.y + h + 3), ImGui::GetColorU32(kAccent), 2.f);
    ImGui::SetCursorScreenPos(ImVec2(s.x, s.y + h + em * .8f));
}
static bool VerShown(const Ver& v, const std::string& loader) { // вызывать под gVerMu
    if (v.type == "snapshot" && !cfg.snapshots) return false;
    if (v.type == "old_beta" && !cfg.showBeta) return false;
    if (v.type == "old_alpha" && !cfg.showAlpha) return false;
    if (loader == "Fabric" && !gFabricGames.empty()) return gFabricGames.count(v.id) > 0;
    if (loader == "Forge" && !gForgeGames.empty()) return gForgeGames.count(v.id) > 0;
    return true;
}
static fs::path CurrentInstanceDir() { // папка игры для выбранных загрузчика и версии
    std::string id = cfg.version; std::error_code ec;
    auto find = [&](const std::string& pre, const std::string& suf) -> std::string {
        for (auto& e : fs::directory_iterator(gRoot / "instances", ec)) {
            std::string n = U8(e.path().filename().wstring());
            if (n.rfind(pre, 0) == 0 && n.size() >= suf.size() && n.compare(n.size() - suf.size(), suf.size(), suf) == 0) return n;
        }
        return std::string();
    };
    if (cfg.loader == "Fabric") { std::string n = find("fabric-loader-", "-" + cfg.version); if (!n.empty()) id = n; }
    else if (cfg.loader == "Forge") { std::string n = find(cfg.version + "-forge-", ""); if (!n.empty()) id = n; }
    return gRoot / "instances" / P(id);
}
static void TabGame() {
    float em = ImGui::GetFontSize(); ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::TextDisabled("Версия");
    std::string vl = cfg.loader + "   " + (cfg.version.empty() ? std::string("загрузка...") : cfg.version);
    if (FancyBtn(vl.c_str(), ImVec2(0, em * 2.2f), ImVec4(1, 1, 1, 0.12f))) ImGui::OpenPopup("##vers");
    ImVec2 bmin = ImGui::GetItemRectMin(), bmax = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos(ImVec2(bmin.x, bmax.y + 4));
    ImGui::SetNextWindowSize(ImVec2(bmax.x - bmin.x, em * 19));
    if (ImGui::BeginPopup("##vers")) {
        const char* ld[] = {"Vanilla", "Fabric", "Forge"}; float sp = ImGui::GetStyle().ItemSpacing.x, bw = (ImGui::GetContentRegionAvail().x - 2 * sp) / 3;
        for (int i = 0; i < 3; i++) {
            if (i) ImGui::SameLine();
            bool sel = cfg.loader == ld[i];
            if (FancyBtn(ld[i], ImVec2(bw, em * 1.9f), sel ? kAccent : ImVec4(1, 1, 1, 0.10f)) && !sel) {
                cfg.loader = ld[i];
                std::lock_guard<std::mutex> l(gVerMu); bool ok = false;
                for (auto& v : gVers) if (v.id == cfg.version && VerShown(v, cfg.loader)) ok = true;
                if (!ok) { // выбранной версии у этого загрузчика нет — берём самую новую подходящую
                    for (auto& v : gVers) if (v.type == "release" && VerShown(v, cfg.loader)) { cfg.version = v.id; ok = true; break; }
                    if (!ok) for (auto& v : gVers) if (VerShown(v, cfg.loader)) { cfg.version = v.id; break; }
                }
            }
        }
        ImGui::Separator();
        ImGui::BeginChild("##vlist", ImVec2(0, 0), false);
        {
            std::lock_guard<std::mutex> l(gVerMu);
            for (auto& v : gVers) {
                if (!VerShown(v, cfg.loader)) continue;
                std::string lab = v.id + (v.type == "snapshot" ? "   [снапшот]" : v.type == "old_beta" ? "   [бета]" : v.type == "old_alpha" ? "   [альфа]" : "");
                bool s2 = v.id == cfg.version;
                if (ImGui::Selectable(lab.c_str(), s2)) { cfg.version = v.id; ImGui::CloseCurrentPopup(); }
            }
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::TextDisabled("Аккаунт");
    ImGui::Text("%s", cfg.sel >= 0 && cfg.sel < (int)cfg.accs.size() ? AccLabel(cfg.accs[cfg.sel]).c_str() : "Player (офлайн, будет создан)");
    ImGui::Dummy(ImVec2(0, em * .4f));
    bool busy = gBusy;
    if (FancyBtn(busy ? "Работаю..." : "ИГРАТЬ", ImVec2(0, em * 2.8f), kAccent, !busy && !cfg.version.empty())) StartLaunch();
    // прогресс
    ImVec2 p = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, h = em * .45f;
    float pr = AnimK("prog", gProg.load(), 6.f);
    dl->AddRectFilled(ImVec2(p.x, p.y + 6), ImVec2(p.x + w, p.y + 6 + h), IM_COL32(255, 255, 255, 25), h / 2);
    if (pr > 0.005f) {
        dl->AddRectFilled(ImVec2(p.x, p.y + 6), ImVec2(p.x + w * pr, p.y + 6 + h), ImGui::GetColorU32(kAccent), h / 2);
        if (busy) { float sh = fmodf(gTime * 0.8f, 1.f); float sx = p.x + (w * pr) * sh; dl->AddRectFilled(ImVec2(sx, p.y + 6), ImVec2(std::min(sx + em * 2, p.x + w * pr), p.y + 6 + h), IM_COL32(255, 255, 255, 70), h / 2); }
    }
    ImGui::Dummy(ImVec2(0, h + 12));
    std::string st; { std::lock_guard<std::mutex> l(gMu); st = gStatus; }
    ImGui::TextWrapped("%s", st.c_str());
    ImGui::Dummy(ImVec2(0, em * .3f));
    if (FancyBtn("Папка", ImVec2(0, em * 1.9f), ImVec4(1, 1, 1, 0.10f))) ImGui::OpenPopup("##folders");
    ImVec2 fmin = ImGui::GetItemRectMin(), fmax = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos(ImVec2(fmin.x, fmin.y - 4), 0, ImVec2(0, 1));
    ImGui::SetNextWindowSize(ImVec2(fmax.x - fmin.x, 0));
    if (ImGui::BeginPopup("##folders")) {
        fs::path gd = CurrentInstanceDir(); std::error_code ec;
        if (ImGui::Selectable("Открыть папку игры")) Open(gd);
        if (ImGui::Selectable("Открыть папку mods")) Open(gd / "mods");
        if (fs::exists(gd / "shaderpacks", ec) && ImGui::Selectable("Открыть папку shaderpacks")) Open(gd / "shaderpacks");
        ImGui::EndPopup();
    }
}
static void TabAccount() {
    float em = ImGui::GetFontSize(); int del = -1;
    ImGui::TextDisabled("Аккаунты");
    for (int i = 0; i < (int)cfg.accs.size(); i++) {
        ImGui::PushID(i);
        if (ImGui::Selectable(AccLabel(cfg.accs[i]).c_str(), cfg.sel == i, 0, ImVec2(ImGui::GetContentRegionAvail().x - em * 2.2f, 0))) cfg.sel = i;
        ImGui::SameLine(); if (ImGui::SmallButton("x")) del = i;
        ImGui::PopID();
    }
    if (del >= 0) { cfg.accs.erase(cfg.accs.begin() + del); if (cfg.accs.empty()) cfg.sel = -1; else if (cfg.sel == del) cfg.sel = 0; else if (cfg.sel > del) cfg.sel--; SaveConfig(); }
    ImGui::Separator(); ImGui::TextDisabled("Офлайн");
    ImGui::SetNextItemWidth(-1); ImGui::InputText("##nick", bNick, sizeof(bNick));
    ImGui::TextDisabled("%s", kNickHint);
    if (FancyBtn("Добавить офлайн-аккаунт", ImVec2(0, em * 1.9f), ImVec4(1, 1, 1, 0.12f))) AddOffline(bNick);
    ImGui::Separator(); ImGui::TextDisabled("Ely.by");
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##eu", "Логин или e-mail", bElyU, sizeof(bElyU));
    ImGui::SetNextItemWidth(-1); ImGui::InputTextWithHint("##ep", "Пароль (с 2FA: пароль:код)", bElyP, sizeof(bElyP), ImGuiInputTextFlags_Password);
    if (FancyBtn("Войти через Ely.by", ImVec2(0, em * 1.9f), ImVec4(1, 1, 1, 0.12f), !gAuthBusy) && bElyU[0]) std::thread(ElyLoginThread, std::string(bElyU), std::string(bElyP)).detach();
    ImGui::Separator(); ImGui::TextDisabled("Microsoft");
    if (bMsId[0] == 0) { std::string id = MsId(); snprintf(bMsId, sizeof(bMsId), "%s", id.c_str()); }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##msid", "Azure Client ID (portal.azure.com)", bMsId, sizeof(bMsId))) SetMsId(bMsId);
    if (FancyBtn("Войти через Microsoft", ImVec2(0, em * 1.9f), ImVec4(1, 1, 1, 0.12f), !gAuthBusy)) { SetMsId(bMsId); SaveConfig(); std::thread(MsLoginThread).detach(); }
    std::string code, uri; { std::lock_guard<std::mutex> l(gMu); code = gMsCode; uri = gMsUri; }
    if (!code.empty()) {
        ImGui::TextWrapped("Открой %s и введи код:", uri.c_str());
        ImGui::SetWindowFontScale(1.5f); ImGui::TextColored(kAccent, "%s", code.c_str()); ImGui::SetWindowFontScale(1.f);
        if (ImGui::SmallButton("Скопировать код")) ImGui::SetClipboardText(code.c_str());
    }
}
static void TabSettings() {
    float em = ImGui::GetFontSize();
    ImGui::TextDisabled("Папка игры");
    { std::string gd = PS(gRoot); ImGui::TextWrapped("%s", gd.c_str()); }
    if (FancyBtn("Выбрать папку игры...", ImVec2(0, em * 1.9f), ImVec4(1, 1, 1, 0.12f), !gBusy)) {
        std::string f = PickFolder(gHwnd); if (!f.empty()) { SetGameDir(f); ScanShots(); }
    }
    if (!cfg.gameDir.empty() && FancyBtn("Вернуть стандартную папку", ImVec2(0, em * 1.9f), ImVec4(1, 1, 1, 0.12f), !gBusy)) { SetGameDir(""); ScanShots(); }
    ImGui::TextDisabled("После запуска игры");
    ImGui::RadioButton("Оставить лаунчер как есть", &cfg.onLaunch, 0);
    ImGui::RadioButton("Свернуть лаунчер", &cfg.onLaunch, 1);
    ImGui::RadioButton("Закрыть лаунчер", &cfg.onLaunch, 2);
    ImGui::TextDisabled("Показывать в списке версий");
    ImGui::Checkbox("Снапшоты", &cfg.snapshots);
    ImGui::Checkbox("Бета-версии (old_beta)", &cfg.showBeta);
    ImGui::Checkbox("Альфа-версии (old_alpha)", &cfg.showAlpha);
    ImGui::Separator();
    ImGui::TextDisabled("Java");
    static std::vector<JavaInfo> javas; static std::atomic<bool> scanning{false}; static std::mutex jm; static bool first = true;
    if (first) { first = false; scanning = true; std::thread([] { auto j = FindJavas(); std::lock_guard<std::mutex> l(jm); javas = j; scanning = false; }).detach(); }
    ImGui::SetNextItemWidth(-1);
    std::string cur = cfg.javaPath.empty() ? "Авто (подберу и скачаю сам)" : cfg.javaPath;
    if (ImGui::BeginCombo("##java", cur.c_str())) {
        if (ImGui::Selectable("Авто (подберу и скачаю сам)", cfg.javaPath.empty())) { cfg.javaPath.clear(); bJava[0] = 0; }
        std::lock_guard<std::mutex> l(jm);
        for (auto& j : javas) { std::string t = "Java " + std::to_string(j.major) + "  —  " + j.exe; if (ImGui::Selectable(t.c_str(), cfg.javaPath == j.exe)) { cfg.javaPath = j.exe; snprintf(bJava, sizeof(bJava), "%s", j.exe.c_str()); } }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##jp", "...или свой путь к java.exe", bJava, sizeof(bJava))) cfg.javaPath = bJava;
    ImGui::TextDisabled("Память (ОЗУ)");
    MEMORYSTATUSEX ms{sizeof(ms)}; GlobalMemoryStatusEx(&ms); int maxMb = std::max(2048, (int)(ms.ullTotalPhys / (1024 * 1024) * 3 / 4));
    ImGui::SetNextItemWidth(-1); ImGui::SliderInt("##ram", &cfg.ramMB, 1024, maxMb, "%d МБ"); cfg.ramMB = std::min(cfg.ramMB, maxMb);
    ImGui::TextDisabled("Фон");
    if (FancyBtn("Выбрать картинку...", ImVec2(0, em * 1.9f), ImVec4(1, 1, 1, 0.12f))) {
        std::string f = PickImage(gHwnd); if (!f.empty()) { cfg.bg = f; cfg.slideshow = false; LoadBgAsync(f); }
    }
    if (FancyBtn("Фон из скриншотов Minecraft", ImVec2(0, em * 1.9f), ImVec4(1, 1, 1, 0.12f))) { ScanShots(); cfg.slideshow = true; NextShot(); }
    ImGui::Checkbox("Слайдшоу фонов (каждые ~18 сек)", &cfg.slideshow);
    ImGui::TextDisabled("Найдено скриншотов: %d", (int)gShots.size());
}
static void TabLog() {
    ImGui::BeginChild("log", ImVec2(0, 0), false);
    { std::lock_guard<std::mutex> l(gMu);
      ImGuiListClipper cl; cl.Begin((int)gLog.size());
      while (cl.Step()) for (int i = cl.DisplayStart; i < cl.DisplayEnd; i++) ImGui::TextUnformatted(gLog[i].c_str()); }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.f);
    ImGui::EndChild();
}
static void DrawCover(ImDrawList* dl, const Tex& t, float alpha, ImVec2 sz) {
    if (!t.v || alpha <= 0.f) return;
    float ar = (float)t.w / t.h, sr = sz.x / sz.y; ImVec2 a(0, 0), b(1, 1);
    if (ar > sr) { float k = sr / ar; a.x = (1 - k) / 2; b.x = 1 - a.x; } else { float k = ar / sr; a.y = (1 - k) / 2; b.y = 1 - a.y; }
    float z = 0.02f + 0.02f * (0.5f + 0.5f * sinf(gTime * 0.15f)); // медленный "Ken Burns"-зум
    float cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2, hw = (b.x - a.x) / 2 * (1 - z), hh = (b.y - a.y) / 2 * (1 - z);
    dl->AddImage((ImTextureID)(intptr_t)t.v, ImVec2(0, 0), sz, ImVec2(cx - hw, cy - hh), ImVec2(cx + hw, cy + hh), IM_COL32(255, 255, 255, (int)(alpha * 255)));
}
static void DrawUI(float W, float H) {
    // фон
    ImDrawList* bg = ImGui::GetBackgroundDrawList(); ImVec2 sz(W, H);
    if (!gBg[0].v) {
        float t = gTime * 0.2f;
        auto C = [&](float o) { return IM_COL32((int)(30 + 25 * sinf(t + o)), (int)(55 + 30 * sinf(t + o + 2)), (int)(90 + 40 * sinf(t + o + 4)), 255); };
        bg->AddRectFilledMultiColor(ImVec2(0, 0), sz, C(0), C(1.5f), C(3.f), C(4.5f));
    } else { if (gBg[1].v && gFade < 1.f) DrawCover(bg, gBg[1], 1.f, sz); DrawCover(bg, gBg[0], gFade, sz); }
    bg->AddRectFilledMultiColor(ImVec2(0, 0), sz, IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 20), IM_COL32(0, 0, 0, 20), IM_COL32(0, 0, 0, 120));

    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(sz);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0)); ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar(2);
    float em = ImGui::GetFontSize(), intro = AnimK("intro", 1.f, 2.5f);
    float pw = std::min(em * 21, W - em * 2), px = em * 1.5f - (1 - intro) * em * 3, py = em * 1.5f, ph = H - em * 3;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph), IM_COL32(14, 17, 26, (int)(190 * intro)), em * .9f);
    dl->AddRect(ImVec2(px, py), ImVec2(px + pw, py + ph), IM_COL32(255, 255, 255, (int)(30 * intro)), em * .9f);
    ImGui::SetCursorPos(ImVec2(px + em, py + em));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, intro);
    ImGui::BeginChild("panel", ImVec2(pw - em * 2, ph - em * 2), false, ImGuiWindowFlags_NoBackground);
    ImGui::SetWindowFontScale(1.5f); ImGui::TextColored(kAccent, "PoltergeistLauncher"); ImGui::SetWindowFontScale(1.f);
    ImGui::Dummy(ImVec2(0, em * .2f));
    TabBar();
    gTabFade = std::min(1.f, gTabFade + ImGui::GetIO().DeltaTime * 7.f);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1 - gTabFade) * em * .8f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, intro * gTabFade);
    ImGui::BeginChild("content", ImVec2(0, 0), false, ImGuiWindowFlags_NoBackground);
    if (gTab == 0) TabGame(); else if (gTab == 1) TabAccount(); else if (gTab == 2) TabSettings(); else TabLog();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();

    // окно с объяснением краша
    static std::string cT, cB;
    { std::lock_guard<std::mutex> l(gMu); if (gShowCrash) { gShowCrash = false; cT = gCrashTitle; cB = gCrashBody; ImGui::OpenPopup("Что-то пошло не так"); } }
    ImGui::SetNextWindowPos(ImVec2(W / 2, H / 2), ImGuiCond_Always, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(em * 30, W - em * 2), 0));
    if (ImGui::BeginPopupModal("Что-то пошло не так", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::SetWindowFontScale(1.25f); ImGui::TextColored(ImVec4(1.f, .45f, .4f, 1.f), "%s", cT.c_str()); ImGui::SetWindowFontScale(1.f);
        ImGui::Separator(); ImGui::TextWrapped("%s", cB.c_str()); ImGui::Dummy(ImVec2(0, em * .5f));
        if (ImGui::Button("Показать лог", ImVec2(em * 8, 0))) { gTab = 3; gTabFade = 0; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(); if (ImGui::Button("Закрыть", ImVec2(em * 8, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

// ============================ Win32 / D3D11 ============================
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static void CreateRTV() { ID3D11Texture2D* b = nullptr; gSwap->GetBuffer(0, IID_PPV_ARGS(&b)); gDev->CreateRenderTargetView(b, nullptr, &gRTV); b->Release(); }
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    switch (m) {
    case WM_SIZE:
        if (gDev && w != SIZE_MINIMIZED) { if (gRTV) { gRTV->Release(); gRTV = nullptr; } gSwap->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0); CreateRTV(); }
        return 0;
    case WM_SYSCOMMAND: if ((w & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    ImGui_ImplWin32_EnableDpiAwareness();
    float S = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    gCfgDir = fs::path(Env(L"APPDATA")) / ".mclauncher"; gRoot = gCfgDir;
    std::error_code ec; fs::create_directories(gCfgDir, ec);
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hi, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"MCL", nullptr};
    RegisterClassExW(&wc);
    gHwnd = CreateWindowW(L"MCL", L"PoltergeistLauncher", WS_OVERLAPPEDWINDOW, 100, 100, (int)(1100 * S), (int)(680 * S), nullptr, nullptr, hi, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = gHwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL fl; const D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, lv, 2, D3D11_SDK_VERSION, &sd, &gSwap, &gDev, &fl, &gCtx))) {
        MessageBoxW(nullptr, L"Не удалось создать Direct3D 11", L"PoltergeistLauncher", MB_ICONERROR); return 1;
    }
    CreateRTV();
    ShowWindow(gHwnd, SW_SHOWDEFAULT); UpdateWindow(gHwnd);

    ImGui::CreateContext(); ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr;
    {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::string dir = U8(fs::path(exe).parent_path().wstring());
        std::vector<std::string> cand = {dir + "\\font.ttf", "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
                                         "C:\\Windows\\Fonts\\tahoma.ttf", "C:\\Windows\\Fonts\\verdana.ttf", "C:\\Windows\\Fonts\\calibri.ttf"};
        bool ok = false;
        for (auto& f : cand)
            if (fs::exists(P(f)) && io.Fonts->AddFontFromFileTTF(f.c_str(), 18.f * S, nullptr, io.Fonts->GetGlyphRangesCyrillic())) { ok = true; break; }
        if (!ok) io.Fonts->AddFontDefault(); // без кириллицы: будут «?»
    }
    ImGui::StyleColorsDark(); ImGuiStyle& st = ImGui::GetStyle();
    st.FrameRounding = 8; st.PopupRounding = 10; st.GrabRounding = 8; st.ScrollbarRounding = 8; st.WindowRounding = 12; st.ChildRounding = 8;
    st.FramePadding = ImVec2(10, 7); st.ItemSpacing = ImVec2(10, 10); st.ScaleAllSizes(S);
    ImVec4* c = st.Colors;
    c[ImGuiCol_FrameBg] = ImVec4(1, 1, 1, .08f); c[ImGuiCol_FrameBgHovered] = ImVec4(1, 1, 1, .14f); c[ImGuiCol_FrameBgActive] = ImVec4(1, 1, 1, .18f);
    c[ImGuiCol_CheckMark] = kAccent; c[ImGuiCol_SliderGrab] = kAccent; c[ImGuiCol_SliderGrabActive] = kAccent;
    c[ImGuiCol_Header] = ImVec4(kAccent.x, kAccent.y, kAccent.z, .35f); c[ImGuiCol_HeaderHovered] = ImVec4(kAccent.x, kAccent.y, kAccent.z, .5f);
    c[ImGuiCol_PopupBg] = ImVec4(.08f, .09f, .13f, .98f);
    ImGui_ImplWin32_Init(gHwnd); ImGui_ImplDX11_Init(gDev, gCtx);

    LoadConfig(); snprintf(bJava, sizeof(bJava), "%s", cfg.javaPath.c_str());
    if (!cfg.gameDir.empty()) { // своя папка игры из настроек
        std::error_code e2; fs::create_directories(P(cfg.gameDir), e2);
        if (!e2 && fs::is_directory(P(cfg.gameDir), e2)) gRoot = P(cfg.gameDir); else cfg.gameDir.clear();
    }
    std::thread(FetchVersions).detach();
    std::thread(FetchLoaderLists).detach();
    ScanShots();
    if (!cfg.bg.empty() && !cfg.slideshow) LoadBgAsync(cfg.bg); else NextShot();

    bool done = false; auto last = std::chrono::steady_clock::now();
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        if (IsIconic(gHwnd)) { Sleep(60); continue; }
        auto now = std::chrono::steady_clock::now(); float dt = std::chrono::duration<float>(now - last).count(); last = now; gTime += dt;

        { std::lock_guard<std::mutex> l(gMu); if (gNewAcc) {
            bool rep = false; for (auto& a : cfg.accs) if (a.type == gNewAcc->type && a.uuid == gNewAcc->uuid) { a = *gNewAcc; rep = true; }
            if (!rep) { cfg.accs.push_back(*gNewAcc); cfg.sel = (int)cfg.accs.size() - 1; }
            gNewAcc.reset(); SaveConfig(); } }
        { std::lock_guard<std::mutex> l(gVerMu); if (cfg.version.empty() && !gLatest.empty()) cfg.version = gLatest; }
        UpdateBackground(dt);

        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        RECT rc; GetClientRect(gHwnd, &rc);
        DrawUI((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
        ImGui::Render();
        const float clear[4] = {0.05f, 0.06f, 0.09f, 1.f};
        if (gRTV) { gCtx->OMSetRenderTargets(1, &gRTV, nullptr); gCtx->ClearRenderTargetView(gRTV, clear); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); }
        gSwap->Present(1, 0); // VSync — не жжём CPU/GPU
    }
    SaveConfig();
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (gRTV) gRTV->Release(); gSwap->Release(); gCtx->Release(); gDev->Release();
    DestroyWindow(gHwnd); UnregisterClassW(L"MCL", hi);
    return 0;
}
