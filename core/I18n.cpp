#include "I18n.h"
#include "Util.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bootaz::i18n {

// Defined in I18n_az.cpp.
extern const std::pair<const char*, const char*> kAzerbaijani[];
extern const int kAzerbaijaniCount;

namespace {

Lang gLang = Lang::Az;

const std::unordered_map<std::string, std::string>& dictionary()
{
    static const std::unordered_map<std::string, std::string> d = [] {
        std::unordered_map<std::string, std::string> m;
        m.reserve(size_t(kAzerbaijaniCount));
        for (int i = 0; i < kAzerbaijaniCount; ++i)
            m.emplace(kAzerbaijani[i].first, kAzerbaijani[i].second);
        return m;
    }();
    return d;
}

std::string envVar(const char* name)
{
    const char* v = std::getenv(name);
    return v ? v : "";
}

std::string settingsFile() { return settingsDir() + "/language"; }

} // namespace

Lang language() { return gLang; }
void setLanguage(Lang lang) { gLang = lang; }
const char* code(Lang lang) { return lang == Lang::Az ? "az" : "en"; }

bool parseLang(const std::string& s, Lang& out)
{
    const std::string k = lower(trim(s));
    if (startsWith(k, "az")) { out = Lang::Az; return true; }
    if (startsWith(k, "en")) { out = Lang::En; return true; }
    return false;
}

std::string settingsDir()
{
    std::string base;
#ifdef _WIN32
    base = envVar("APPDATA");
    if (base.empty())
        base = envVar("USERPROFILE");
    base += "\\BootAz";
#else
    // Under pkexec / sudo the real user's folder is still the right place.
    std::string home = envVar("HOME");
    const std::string sudoUser = envVar("SUDO_USER");
    if (!sudoUser.empty() && (home.empty() || home == "/root"))
        home = "/home/" + sudoUser;
    base = envVar("XDG_CONFIG_HOME");
    if (base.empty() || startsWith(base, "/root"))
        base = home + "/.config";
    base += "/BootAz";
#endif
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

Lang detectLanguage()
{
    Lang l;
    if (parseLang(envVar("BOOTAZ_LANG"), l))
        return l;
    if (parseLang(readTextFile(settingsFile()), l))
        return l;
    return Lang::Az;   // the app is made for Azerbaijan; English is one click away
}

void saveLanguage(Lang lang)
{
    std::ofstream f(settingsFile(), std::ios::trunc);
    f << code(lang) << '\n';
}

std::string tr(const std::string& english)
{
    if (gLang == Lang::En || english.empty())
        return english;
    const auto& d = dictionary();
    const auto it = d.find(english);
    return it == d.end() ? english : it->second;
}

bool hasTranslation(const std::string& english) { return dictionary().count(english) > 0; }

} // namespace bootaz::i18n
