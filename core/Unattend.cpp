#include "Unattend.h"
#include "Util.h"

#include <cctype>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace bootaz {

namespace {

std::string xmlEscape(const std::string& s)
{
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&apos;"; break;
        default: o += c;
        }
    }
    return o;
}

std::string component(const std::string& name, const std::string& arch, const std::string& body)
{
    return "    <component name=\"" + name + "\" processorArchitecture=\"" + arch +
           "\" publicKeyToken=\"31bf3856ad364e35\" language=\"neutral\" versionScope=\"nonSxS\" "
           "xmlns:wcm=\"http://schemas.microsoft.com/WMIConfig/2002/State\">\n" +
           body + "    </component>\n";
}

std::string runSync(const std::vector<std::string>& cmds, const char* element = "RunSynchronous",
                    const char* item = "RunSynchronousCommand", const char* pathTag = "Path")
{
    if (cmds.empty())
        return {};
    std::string s = std::string("      <") + element + ">\n";
    int order = 1;
    for (const auto& c : cmds) {
        s += std::string("        <") + item + " wcm:action=\"add\">\n";
        s += "          <Order>" + std::to_string(order++) + "</Order>\n";
        s += std::string("          <") + pathTag + ">" + xmlEscape(c) + "</" + pathTag + ">\n";
        s += std::string("        </") + item + ">\n";
    }
    s += std::string("      </") + element + ">\n";
    return s;
}

} // namespace

bool validAccountName(const std::string& n)
{
    if (n.empty() || n.size() > 20)
        return false;
    for (unsigned char c : n)
        if (c < 0x20 || std::string("\"/\\[]:;|=,+*?<>@").find(char(c)) != std::string::npos)
            return false;
    return trim(n) == n && n.back() != '.';
}

bool validLocale(const std::string& l)
{
    if (l.empty() || l.size() > 20)
        return false;
    for (unsigned char c : l)
        if (!std::isalnum(c) && c != '-')
            return false;
    return true;
}

bool validComputerName(const std::string& n)
{
    if (n.empty() || n.size() > 15)
        return false;
    bool digitsOnly = true;
    for (unsigned char c : n) {
        if (!std::isalnum(c) && c != '-')
            return false;
        if (!std::isdigit(c))
            digitsOnly = false;
    }
    return !digitsOnly && n.front() != '-' && n.back() != '-';
}

std::string systemWindowsLocale()
{
#ifdef _WIN32
    wchar_t buf[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(buf, LOCALE_NAME_MAX_LENGTH) > 0)
        return utf16ToUtf8(reinterpret_cast<const char16_t*>(buf));
    return "en-US";
#else
    std::string l;
    for (const char* v : {"LC_ALL", "LC_MESSAGES", "LANG"})
        if (const char* e = std::getenv(v); e && *e) {
            l = e;
            break;
        }
    l = l.substr(0, l.find('.'));
    l = l.substr(0, l.find('@'));
    if (l.empty() || l == "C" || l == "POSIX")
        return "en-US";
    for (char& c : l)
        if (c == '_')
            c = '-';
    // Windows names a few locales with their script.
    if (l == "az-AZ") return "az-Latn-AZ";
    if (l == "uz-UZ") return "uz-Latn-UZ";
    if (l == "sr-RS") return "sr-Latn-RS";
    return l;
#endif
}

std::string makeAutounattend(const WindowsOptions& o)
{
    const std::string a = o.architecture.empty() ? "amd64" : o.architecture;
    std::string x = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                    "<!-- Written by BootAz -->\n"
                    "<unattend xmlns=\"urn:schemas-microsoft-com:unattend\">\n";

    // windowsPE: runs from the USB before setup checks the hardware.
    std::string pe;
    if (o.bypassHardwareChecks) {
        std::vector<std::string> cmds;
        for (const char* v : {"BypassTPMCheck", "BypassSecureBootCheck", "BypassRAMCheck", "BypassStorageCheck", "BypassCPUCheck"})
            cmds.push_back(std::string("reg add HKLM\\SYSTEM\\Setup\\LabConfig /v ") + v + " /t REG_DWORD /d 1 /f");
        pe += component("Microsoft-Windows-Setup", a, runSync(cmds) + "      <UserData>\n        <AcceptEula>true</AcceptEula>\n      </UserData>\n");
    }
    if (!pe.empty())
        x += "  <settings pass=\"windowsPE\">\n" + pe + "  </settings>\n";

    // specialize: registry settings that must exist before the first-run wizard (OOBE).
    std::vector<std::string> spec;
    if (o.noOnlineAccount)
        spec.push_back("reg add HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\OOBE /v BypassNRO /t REG_DWORD /d 1 /f");
    if (o.disableBitLocker)
        spec.push_back("reg add HKLM\\SYSTEM\\CurrentControlSet\\Control\\BitLocker /v PreventDeviceEncryption /t REG_DWORD /d 1 /f");
    std::string specBody;
    if (!spec.empty())
        specBody += component("Microsoft-Windows-Deployment", a, runSync(spec));
    if (!o.computerName.empty())
        specBody += component("Microsoft-Windows-Shell-Setup", a, "      <ComputerName>" + xmlEscape(o.computerName) + "</ComputerName>\n");
    if (!specBody.empty())
        x += "  <settings pass=\"specialize\">\n" + specBody + "  </settings>\n";

    // oobeSystem: the first-run wizard.
    std::string oobe;
    if (!o.locale.empty()) {
        const std::string l = xmlEscape(o.locale);
        oobe += component("Microsoft-Windows-International-Core", a,
                          // Display language stays as installed: only formats and keyboard follow the locale.
                          "      <InputLocale>" + l + "</InputLocale>\n      <SystemLocale>" + l + "</SystemLocale>\n"
                          "      <UserLocale>" + l + "</UserLocale>\n");
    }
    std::string shell;
    if (o.noOnlineAccount || o.skipPrivacyQuestions || !o.localAccount.empty()) {
        shell += "      <OOBE>\n";
        if (o.noOnlineAccount || !o.localAccount.empty())
            shell += "        <HideOnlineAccountScreens>true</HideOnlineAccountScreens>\n";
        if (o.skipPrivacyQuestions)
            shell += "        <ProtectYourPC>3</ProtectYourPC>\n";
        shell += "      </OOBE>\n";
    }
    if (!o.localAccount.empty()) {
        const std::string n = xmlEscape(o.localAccount);
        shell += "      <UserAccounts>\n        <LocalAccounts>\n          <LocalAccount wcm:action=\"add\">\n"
                 "            <Name>" + n + "</Name>\n            <DisplayName>" + n + "</DisplayName>\n"
                 "            <Group>Administrators;Power Users</Group>\n"
                 "            <Password>\n              <Value></Value>\n              <PlainText>true</PlainText>\n            </Password>\n"
                 "          </LocalAccount>\n        </LocalAccounts>\n      </UserAccounts>\n";
        // Empty password for the first sign-in, then Windows asks for a new one.
        shell += runSync({"net user \"" + o.localAccount + "\" /logonpasswordchg:yes", "net accounts /maxpwage:unlimited"},
                         "FirstLogonCommands", "SynchronousCommand", "CommandLine");
    }
    if (!shell.empty())
        oobe += component("Microsoft-Windows-Shell-Setup", a, shell);
    if (!oobe.empty())
        x += "  <settings pass=\"oobeSystem\">\n" + oobe + "  </settings>\n";

    x += "</unattend>\n";
    return x;
}

} // namespace bootaz
