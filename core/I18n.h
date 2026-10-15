#pragma once
// Two languages: Azerbaijani (default) and English. Source strings are English; T("…") returns
// the Azerbaijani text from I18n_az.cpp when Azerbaijani is active. Values go in as %1, %2 …
// so each language keeps its own word order. Plain C++ so the core and the CLI can use it too.

#include <string>

namespace bootaz::i18n {

enum class Lang { Az, En };

Lang language();
void setLanguage(Lang lang);
const char* code(Lang lang);                 // "az" / "en"
bool parseLang(const std::string& s, Lang& out);
Lang detectLanguage();                       // BOOTAZ_LANG, then the saved choice, then the system locale
void saveLanguage(Lang lang);
std::string settingsDir();                   // per-user folder for settings and logs

std::string tr(const std::string& english);
bool hasTranslation(const std::string& english);

} // namespace bootaz::i18n

inline std::string T(const char* english) { return bootaz::i18n::tr(english); }
inline std::string T(const std::string& english) { return bootaz::i18n::tr(english); }
// Marks a string for translation where it is stored; translate it with T() where it is shown.
#define N_(s) s
