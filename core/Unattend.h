#pragma once
// Windows setup customisation, written to the drive as autounattend.xml (Rufus' "Customize
// Windows installation" plus a few extras).

#include <string>

namespace bootaz {

struct WindowsOptions {
    bool bypassHardwareChecks = false;   // TPM 2.0, Secure Boot, 4 GB RAM
    bool noOnlineAccount = false;        // allow setup without a Microsoft account / internet
    std::string localAccount;            // create this local administrator ("" = none)
    bool skipPrivacyQuestions = false;   // answer the data collection questions with "no"
    bool disableBitLocker = false;       // no automatic device encryption
    std::string locale;                  // "en-US", "az-Latn-AZ", ... ("" = ask during setup)
    std::string computerName;            // "" = random name chosen by setup
    std::string architecture = "amd64";  // amd64 / arm64 / x86

    bool any() const
    {
        return bypassHardwareChecks || noOnlineAccount || !localAccount.empty() || skipPrivacyQuestions || disableBitLocker ||
               !locale.empty() || !computerName.empty();
    }
};

std::string makeAutounattend(const WindowsOptions& o);
// The current user's locale as a Windows locale name ("az_AZ.UTF-8" -> "az-Latn-AZ").
std::string systemWindowsLocale();
bool validAccountName(const std::string& name);
bool validComputerName(const std::string& name);

} // namespace bootaz
