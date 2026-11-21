#pragma once
// UI translations from plain text files.
//
// A language is a UTF-8 text file, one phrase per line, English on the left:
//     @code = tr
//     @name = Türkçe
//     Refresh now = Şimdi yenile
//     {name} is at {percent}% = {name}: %{percent} kaldı
// Lines starting with # are comments; "\n" in a translation is a line break. Placeholders are named
// ({name}, {percent}), so a mistake in a translation can never crash the app: at worst a word is wrong.
// Missing phrases stay English.
//
// Where languages come from:
//  * the lang\*.txt files of the repository, built into the exe (adding one = adding a file, no code)
//  * a "lang" folder next to WirelessBatteryCenter.exe and %APPDATA%\WirelessBatteryCenter\lang: any .txt file there
//    appears in the Language menu without rebuilding, and one with the same @code as a built-in language
//    replaces its phrases (to fix a word locally).
// The --list / --dump output stays English on purpose (it is pasted into GitHub issues).
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace i18n {

struct Language {
    std::wstring code;  // "tr", "pt-BR", ...
    std::wstring name;  // in the language itself: "Türkçe", "Português (Brasil)"
};

// Finds the languages (built-in and user files) and applies the saved choice; call once at startup.
void Init();
// Every language that can be picked, English first.
std::vector<Language> Available();
// The saved choice: empty = follow the Windows display language.
std::wstring Choice();
// Saves the choice ("" = automatic) and switches to it.
void SetChoice(const std::wstring& code);
// True for right-to-left languages (Hebrew, Arabic): menus are laid out from the right.
bool Rtl();
// Creates %APPDATA%\WirelessBatteryCenter\lang with a template of every phrase and opens it in Explorer.
void OpenUserFolder();

}  // namespace i18n

// The translation of an English UI phrase, or the phrase itself.
const wchar_t* T(const wchar_t* en);
std::wstring T(const std::wstring& en);
// T() with named placeholders: TF(L"{name} is at {percent}%", { { L"name", n }, { L"percent", L"40" } }).
std::wstring TF(const wchar_t* en, std::initializer_list<std::pair<const wchar_t*, std::wstring>> args);
