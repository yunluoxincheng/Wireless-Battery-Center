// UI translations from plain text files. See i18n.h.
#include "i18n.h"
#include "product.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <unordered_map>

namespace {

const auto& kSettingsKey = product::SettingsKey;

// One language: its built-in text and/or a user's file (whose phrases win), parsed only when chosen.
struct Source {
    std::wstring code, name;
    bool rtl = false;
    std::vector<std::string> texts;  // UTF-8 file contents: built-in first, then user files
};

std::map<std::wstring, Source> g_sources;           // by lower-case code
std::unordered_map<std::wstring, std::wstring> g_phrases;  // English -> translation, for the active language
std::wstring g_choice;                               // saved choice, "" = automatic
bool g_rtl = false;
std::string g_template;                              // lang/_template.txt, every English phrase

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (iswspace(s[a]) || s[a] == 0xFEFF)) a++;  // also a byte-order mark
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// "\n" -> line break, "\\" -> backslash.
std::wstring Unescape(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[++i];
            out += n == L'n' ? L'\n' : n;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Calls fn(key, value) for every "key = value" line; metadata keys start with '@'.
template <class Fn>
void ForEachLine(const std::string& utf8, Fn fn) {
    std::wstring text = Wide(utf8);
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = Trim(text.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty() || line[0] == L'#') continue;
        size_t eq = line.find(L" = ");
        if (eq == std::wstring::npos) continue;
        fn(Trim(line.substr(0, eq)), Trim(line.substr(eq + 3)));
    }
}

// Reads @code / @name / @rtl from one file and files it under its language.
void AddSource(const std::string& utf8, const std::wstring& fallbackCode) {
    std::wstring code = fallbackCode, name;
    bool rtl = false;
    ForEachLine(utf8, [&](const std::wstring& k, const std::wstring& v) {
        if (k == L"@code") code = v;
        else if (k == L"@name") name = v;
        else if (k == L"@rtl") rtl = v == L"1" || Lower(v) == L"yes" || Lower(v) == L"true";
    });
    if (code.empty() || Lower(code) == L"en") return;  // English is the source text itself
    Source& s = g_sources[Lower(code)];
    if (s.code.empty()) s.code = code;
    if (!name.empty()) s.name = name;
    if (rtl) s.rtl = true;
    s.texts.push_back(utf8);
}

// The built-in languages: build.bat / the Makefile join lang\*.txt into one resource, each file
// introduced by a line "@file <name>".
void LoadBuiltIn() {
    HRSRC r = FindResourceW(nullptr, L"LANGS", MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL h = r ? LoadResource(nullptr, r) : nullptr;
    const char* data = h ? (const char*)LockResource(h) : nullptr;
    if (!data) return;
    std::string all(data, SizeofResource(nullptr, r));
    size_t pos = 0;
    while ((pos = all.find("@file ", pos)) != std::string::npos) {
        if (pos > 0 && all[pos - 1] != '\n') { pos += 6; continue; }
        size_t eol = all.find('\n', pos);
        std::string file = all.substr(pos + 6, eol == std::string::npos ? std::string::npos : eol - pos - 6);
        while (!file.empty() && (file.back() == '\r' || file.back() == ' ')) file.pop_back();
        size_t next = eol == std::string::npos ? all.size() : all.find("\n@file ", eol);
        std::string body = eol == std::string::npos ? "" : all.substr(eol + 1, (next == std::string::npos ? all.size() : next) - eol - 1);
        if (file == "_template") g_template = body;
        else if (!file.empty() && file[0] != '_') AddSource(body, Wide(file));
        pos = next == std::string::npos ? all.size() : next + 1;
    }
}

std::wstring UserFolder(const wchar_t* productFolder = L"WirelessBatteryCenter") {
    wchar_t path[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path))) return L"";
    return std::wstring(path) + L"\\" + productFolder + L"\\lang";
}

void LoadFolder(const std::wstring& dir) {
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*.txt").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring file = fd.cFileName;
        if (file.empty() || file[0] == L'_' || fd.nFileSizeLow > 512 * 1024) continue;  // the template, or not a phrase list
        HANDLE h = CreateFileW((dir + L"\\" + file).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::string data(fd.nFileSizeLow, '\0');
        DWORD got = 0;
        ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr);
        CloseHandle(h);
        data.resize(got);
        AddSource(data, file.substr(0, file.size() - 4));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

std::wstring ExeFolder() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p = path;
    size_t slash = p.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"" : p.substr(0, slash);
}

// The language to use for the automatic choice: the first Windows display language there is a file for.
std::wstring AutoCode() {
    ULONG count = 0, size = 0;
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &size) || !size) return L"";
    std::wstring buf(size, 0);
    if (!GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf.data(), &size)) return L"";
    for (const wchar_t* p = buf.c_str(); *p; p += wcslen(p) + 1) {
        std::wstring tag = Lower(p);  // "pt-br", "zh-tw", "de-de", "zh-hant-tw"
        if (tag.rfind(L"zh-", 0) == 0) {  // Chinese: traditional for Taiwan / Hong Kong / Macau / Hant
            bool trad = tag.find(L"hant") != std::wstring::npos || tag.find(L"-tw") != std::wstring::npos ||
                        tag.find(L"-hk") != std::wstring::npos || tag.find(L"-mo") != std::wstring::npos;
            std::wstring want = trad ? L"zh-tw" : L"zh-cn";
            if (g_sources.count(want)) return want;
        }
        if (g_sources.count(tag)) return tag;
        std::wstring base = tag.substr(0, tag.find(L'-'));
        if (base == L"en") return L"";
        if (g_sources.count(base)) return base;
        for (const auto& [code, s] : g_sources)  // "pt" in Windows, only "pt-BR" here
            if (code.rfind(base + L"-", 0) == 0) return code;
    }
    return L"";
}

void Apply() {
    g_phrases.clear();
    g_rtl = false;
    std::wstring code = g_choice.empty() ? AutoCode() : Lower(g_choice);
    auto it = g_sources.find(code);
    if (it == g_sources.end()) return;  // English
    g_rtl = it->second.rtl;
    for (const std::string& text : it->second.texts)  // built-in first, so a user's file wins
        ForEachLine(text, [](const std::wstring& k, const std::wstring& v) {
            if (!k.empty() && k[0] != L'@' && !v.empty()) g_phrases[Unescape(k)] = Unescape(v);
        });
}

}  // namespace

namespace i18n {

void Init() {
    g_sources.clear();
    LoadBuiltIn();
    LoadFolder(ExeFolder() + L"\\lang");
    LoadFolder(UserFolder(L"WirelessBattery"));
    LoadFolder(UserFolder());
    wchar_t buf[64] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    g_choice = RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"Language", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS ? buf : L"";
    Apply();
}

std::vector<Language> Available() {
    std::vector<Language> out{ { L"en", L"English" } };
    for (const auto& [code, s] : g_sources) out.push_back({ s.code, s.name.empty() ? s.code : s.name });
    std::sort(out.begin() + 1, out.end(), [](const Language& a, const Language& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

std::wstring Choice() { return g_choice; }

void SetChoice(const std::wstring& code) {
    g_choice = code;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"Language", REG_SZ, code.c_str(), (DWORD)((code.size() + 1) * sizeof(wchar_t)));
    Apply();
}

bool Rtl() { return g_rtl; }

void OpenUserFolder() {
    std::wstring dir = UserFolder();
    if (dir.empty()) return;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    std::wstring tmpl = dir + L"\\_template.txt";  // refreshed every time, so it always lists the current phrases
    HANDLE h = CreateFileW(tmpl.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, g_template.data(), (DWORD)g_template.size(), &w, nullptr);
        CloseHandle(h);
    }
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace i18n

const wchar_t* T(const wchar_t* en) {
    if (g_phrases.empty()) return en;
    auto it = g_phrases.find(en);
    return it == g_phrases.end() ? en : it->second.c_str();
}

std::wstring T(const std::wstring& en) { return T(en.c_str()); }

std::wstring TF(const wchar_t* en, std::initializer_list<std::pair<const wchar_t*, std::wstring>> args) {
    std::wstring s = T(en);
    for (const auto& [name, value] : args) {
        std::wstring tag = L"{" + std::wstring(name) + L"}";
        for (size_t at = s.find(tag); at != std::wstring::npos; at = s.find(tag, at + value.size())) s.replace(at, tag.size(), value);
    }
    return s;
}
