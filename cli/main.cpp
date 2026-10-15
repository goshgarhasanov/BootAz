// bootaz-cli: the command-line BootAz. Every feature of the desktop app is a command here, and
// the desktop app runs this program (with administrator rights) in --json mode to do the work.

#include "Disk.h"
#include "Hash.h"
#include "I18n.h"
#include "Image.h"
#include "Jobs.h"
#include "Util.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

using namespace bootaz;

namespace {

std::atomic<bool> gCancel{false};
bool gJson = false;
bool gColor = false;
std::mutex gOut;

enum Exit { kOk = 0, kUsage = 1, kFailed = 2, kCancelled = 3, kNeedAdmin = 4 };

const char* c(const char* code) { return gColor ? code : ""; }
#define RESET c("\033[0m")
#define BOLD c("\033[1m")
#define DIM c("\033[2m")
#define RED c("\033[31m")
#define GREEN c("\033[32m")
#define YELLOW c("\033[33m")
#define BLUE c("\033[36m")

void out(const std::string& s)
{
    std::lock_guard<std::mutex> l(gOut);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

void jsonEvent(const std::string& body)
{
    out("{" + body + "}\n");
}

std::string q(const std::string& s) { return "\"" + jsonEscape(s) + "\""; }

// ---- progress output

std::string bar(double f, int width)
{
    const int full = int(f * width + 0.5);
    std::string s;
    for (int i = 0; i < width; ++i)
        s += i < full ? "█" : "░";
    return s;
}

std::string eta(double secs)
{
    if (secs <= 0 || secs > 86400 * 3)
        return "--:--";
    const int s = int(secs);
    char buf[32];
    if (s >= 3600)
        std::snprintf(buf, sizeof buf, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else
        std::snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
    return buf;
}

std::string lastStage;

std::string devTag(const std::string& dev) { return dev.empty() ? std::string() : "\"device\":" + q(dev) + ","; }

bool gMulti = false;   // several drives at once: prefix lines with the drive

JobContext makeContext(const std::string& dev = "")
{
    JobContext ctx;
    ctx.cancel = &gCancel;
    ctx.onLog = [dev](const std::string& line) {
        if (gJson) {
            jsonEvent(devTag(dev) + "\"type\":\"log\",\"text\":" + q(line));
            return;
        }
        out(std::string("\r\033[K") + DIM + (gMulti ? "[" + dev + "] " : "") + line + RESET + "\n");
    };
    ctx.onProgress = [dev](const Progress& p) {
        if (gJson) {
            char buf[256];
            std::snprintf(buf, sizeof buf, "\"type\":\"progress\",\"done\":%llu,\"total\":%llu,\"speed\":%.0f,\"stage\":",
                          static_cast<unsigned long long>(p.done), static_cast<unsigned long long>(p.total), p.bytesPerSec);
            jsonEvent(devTag(dev) + std::string(buf) + q(p.stage));
            return;
        }
        if (gMulti)
            return;   // parallel jobs: logs only, no single progress line
        std::string line = "\r\033[K";
        if (p.total) {
            const double f = std::min(1.0, double(p.done) / double(p.total));
            char pct[16];
            std::snprintf(pct, sizeof pct, "%5.1f%%", f * 100);
            line += std::string(BLUE) + bar(f, 28) + RESET + " " + BOLD + pct + RESET + "  " + humanBytes(p.done) + " / " + humanBytes(p.total);
            if (p.bytesPerSec > 0)
                line += "  " + humanBytes(uint64_t(p.bytesPerSec)) + "/s  " + T("left") + " " +
                        eta(double(p.total - std::min(p.done, p.total)) / p.bytesPerSec);
        } else {
            line += std::string(BLUE) + "…" + RESET;
        }
        line += "  " + p.stage;
        out(line);
        lastStage = p.stage;
    };
    return ctx;
}

void finishLine()
{
    if (!gJson)
        out("\r\033[K");
}

// ---- argument parsing

struct Args {
    std::string command;
    std::vector<std::string> positional;
    std::vector<std::string> devices;   // every --device given
    std::map<std::string, std::string> opts;   // "--name" -> value ("" for flags)

    bool has(const std::string& k) const { return opts.count(k) > 0; }
    std::string get(const std::string& k, const std::string& def = "") const
    {
        const auto it = opts.find(k);
        return it == opts.end() ? def : it->second;
    }
};

const std::vector<std::string> kValueOpts = {
    "--device", "--image", "--mode", "--scheme", "--target", "--fs", "--label", "--cluster", "--badblocks", "--passes",
    "--persistence", "--out", "--algo", "--lang", "--expect-id", "--win-layout", "--win-local-user", "--win-locale", "--win-computer-name", "--check"};

bool parseArgs(const std::vector<std::string>& argv, Args& a, std::string& err)
{
    for (size_t i = 0; i < argv.size(); ++i) {
        const std::string& s = argv[i];
        if (startsWith(s, "--")) {
            std::string key = s, val;
            const auto eq = s.find('=');
            if (eq != std::string::npos) {
                key = s.substr(0, eq);
                val = s.substr(eq + 1);
            } else if (std::find(kValueOpts.begin(), kValueOpts.end(), s) != kValueOpts.end()) {
                if (i + 1 >= argv.size()) {
                    err = format(T("%1 needs a value"), {s});
                    return false;
                }
                val = argv[++i];
            }
            a.opts[key] = val;
            if (key == "--device")
                a.devices.push_back(val);
        } else if (s == "-y") {
            a.opts["--yes"] = "";
        } else if (s == "-h") {
            a.opts["--help"] = "";
        } else if (a.command.empty()) {
            a.command = s;
        } else {
            a.positional.push_back(s);
        }
    }
    return true;
}

uint64_t parseSize(const std::string& s)
{
    if (s.empty())
        return 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    std::string unit = lower(trim(end ? end : ""));
    uint64_t mul = MiB;   // plain numbers are MB
    if (unit == "k" || unit == "kb" || unit == "kib") mul = KiB;
    else if (unit == "g" || unit == "gb" || unit == "gib") mul = GiB;
    else if (unit == "b") mul = 1;
    return uint64_t(v * double(mul));
}

// ---- helpers

void printError(const std::string& msg, const std::string& dev = "")
{
    finishLine();
    if (gJson)
        jsonEvent(devTag(dev) + "\"type\":\"error\",\"message\":" + q(msg));
    else
        std::fprintf(stderr, "%s%s%s %s\n", RED, T("Error:").c_str(), RESET, msg.c_str());
}

bool requireAdmin()
{
    if (haveAdminRights())
        return true;
#ifdef _WIN32
    printError(T("Administrator rights are required. Run the command prompt as Administrator."));
#else
    printError(T("Administrator rights are required. Run it again with sudo."));
#endif
    return false;
}

bool resolveDrive(const Args& a, DriveInfo& d, std::string dev = "")
{
    if (dev.empty())
        dev = a.get("--device", a.positional.empty() ? "" : a.positional.front());
    if (dev.empty()) {
        printError(T("Choose a drive with --device (see: bootaz-cli devices)"));
        return false;
    }
    if (!findDrive(dev, d, a.has("--allow-file"))) {
        printError(format(T("Drive not found: %1"), {dev}));
        return false;
    }
    if (d.system) {
        printError(T("This drive holds the running operating system and cannot be written"));
        return false;
    }
    if (!d.usb && !d.removable && !d.isFile && !a.has("--allow-fixed") && d.bus != "Virtual") {
        printError(T("This is an internal drive. Add --allow-fixed if you really want to erase it."));
        return false;
    }
    return true;
}

bool confirmErase(const Args& a, const DriveInfo& d)
{
    if (a.has("--yes") || gJson)
        return true;
    out(std::string("\n") + RED + BOLD + T("WARNING: ALL DATA ON THIS DRIVE WILL BE DESTROYED") + RESET + "\n  " + d.displayName() + "\n");
    for (const VolumeInfo& v : d.volumes)
        out("  · " + v.path + "  " + v.label + "  " + v.fsType + "  " + humanBytes(v.size) + "\n");
    out(format(T("Type %1 to continue: "), {"YES"}));
    std::string ans;
    std::getline(std::cin, ans);
    return trim(ans) == "YES" || trim(ans) == "yes" || iequals(trim(ans), "BƏLİ") || iequals(trim(ans), "beli");
}

int runJob(const std::function<void(const JobContext&)>& fn, const std::string& dev = "")
{
    JobContext ctx = makeContext(dev);
    try {
        fn(ctx);
        finishLine();
        return kOk;
    } catch (const Cancelled&) {
        finishLine();
        if (gJson)
            jsonEvent("\"type\":\"cancelled\"");
        else
            std::fprintf(stderr, "%s%s%s\n", YELLOW, T("Cancelled").c_str(), RESET);
        return kCancelled;
    } catch (const Error& e) {
        printError(T(e.what()), dev);
        return kFailed;
    } catch (const std::exception& e) {
        printError(e.what(), dev);
        return kFailed;
    }
}

// ---- commands

std::string driveJson(const DriveInfo& d)
{
    std::string vols;
    for (const VolumeInfo& v : d.volumes) {
        if (!vols.empty())
            vols += ",";
        vols += "{\"path\":" + q(v.path) + ",\"label\":" + q(v.label) + ",\"fs\":" + q(v.fsType) + ",\"mount\":" + q(v.mountPoint) +
                ",\"size\":" + std::to_string(v.size) + "}";
    }
    return "{\"path\":" + q(d.path) + ",\"id\":" + q(d.id) + ",\"name\":" + q(d.displayName()) + ",\"vendor\":" + q(d.vendor) +
           ",\"model\":" + q(d.model) + ",\"serial\":" + q(d.serial) + ",\"bus\":" + q(d.bus) + ",\"size\":" + std::to_string(d.size) +
           ",\"sector\":" + std::to_string(d.sectorSize) + ",\"usb\":" + (d.usb ? "true" : "false") +
           ",\"removable\":" + (d.removable ? "true" : "false") + ",\"system\":" + (d.system ? "true" : "false") +
           ",\"readonly\":" + (d.readOnly ? "true" : "false") + ",\"volumes\":[" + vols + "]}";
}

int cmdDevices(const Args& a)
{
    ListOptions lo;
    lo.includeFixed = a.has("--all");
    lo.includeVirtual = a.has("--virtual");
    const auto drives = listDrives(lo);
    if (a.has("--json") || gJson) {
        std::string s = "[";
        for (size_t i = 0; i < drives.size(); ++i)
            s += (i ? "," : "") + driveJson(drives[i]);
        out(s + "]\n");
        return kOk;
    }
    if (drives.empty()) {
        out(T("No USB drives found. Plug one in, or use --all to list internal drives too.") + "\n");
        return kOk;
    }
    for (const DriveInfo& d : drives) {
        std::string tags = d.bus;
        if (d.system)
            tags += std::string(", ") + RED + T("SYSTEM") + RESET;
        if (d.readOnly)
            tags += ", " + T("read-only");
        out(std::string(BOLD) + d.path + RESET + "  " + trim(d.vendor + " " + d.model) + "  " + GREEN + humanBytes(d.size) + RESET +
            "  [" + tags + "]\n");
        for (const VolumeInfo& v : d.volumes)
            out(std::string("    ") + DIM + v.path + "  " + (v.label.empty() ? "-" : v.label) + "  " + (v.fsType.empty() ? "?" : v.fsType) +
                "  " + humanBytes(v.size) + (v.mountPoint.empty() ? "" : "  → " + v.mountPoint) + RESET + "\n");
    }
    return kOk;
}

std::string yesno(bool b) { return b ? T("Yes") : T("No"); }

int cmdAnalyze(const Args& a)
{
    const std::string path = a.get("--image", a.positional.empty() ? "" : a.positional.front());
    if (path.empty()) {
        printError(T("Give the image file to analyse"));
        return kUsage;
    }
    ImageInfo i;
    try {
        i = analyzeImage(path);
    } catch (const Error& e) {
        printError(T(e.what()));
        return kFailed;
    }
    const char* kinds[] = {"unknown", "iso", "raw", "vhd", "compressed"};
    if (a.has("--json") || gJson) {
        std::string efi;
        for (const auto& e : i.efiLoaders)
            efi += (efi.empty() ? "" : ",") + q(e);
        std::string big;
        for (const auto& e : i.over4GB)
            big += (big.empty() ? "" : ",") + q(e);
        out("{\"path\":" + q(i.path) + ",\"kind\":" + q(kinds[int(i.kind)]) + ",\"compression\":" + q(compressionName(i.compression)) +
            ",\"fileSize\":" + std::to_string(i.fileSize) + ",\"dataSize\":" + std::to_string(i.dataSize) + ",\"label\":" + q(i.label) + ",\"name\":" + q(i.friendlyName) +
            ",\"fsTree\":" + q(i.fsTree) + ",\"isohybrid\":" + (i.isohybrid ? "true" : "false") +
            ",\"biosBootable\":" + (i.biosBootable() ? "true" : "false") + ",\"uefiBootable\":" + (i.uefiBootable() ? "true" : "false") +
            ",\"efiLoaders\":[" + efi + "],\"syslinux\":" + (i.hasSyslinux ? "true" : "false") + ",\"grub\":" + (i.hasGrub ? "true" : "false") +
            ",\"windows\":" + (i.isWindows ? "true" : "false") + ",\"windows11\":" + (i.isWindows11 ? "true" : "false") +
            ",\"linux\":" + (i.isLinux ? "true" : "false") + ",\"distro\":" + q(i.distro) + ",\"persistence\":" + q(i.persistenceKind) + ",\"efiSigned\":" + (i.efiSigned ? "true" : "false") +
            ",\"efiSigner\":" + q(i.efiSigner) + ",\"efiOldCa\":" + (i.efiOldMicrosoftCa ? "true" : "false") +
            ",\"filesTotal\":" + std::to_string(i.filesTotal) + ",\"fileCount\":" + std::to_string(i.fileCount) +
            ",\"largestFile\":" + std::to_string(i.largestFile) + ",\"over4GB\":[" + big + "],\"canIso\":" + (i.canIsoMode() ? "true" : "false") +
            ",\"ddRecommended\":" + (i.ddRecommended() ? "true" : "false") + "}\n");
        return kOk;
    }
    auto row = [](const std::string& k, const std::string& v) { out("  " + k + std::string(std::max<int>(1, 24 - int(utf8ToUtf16(k).size())), ' ') + v + "\n"); };
    out(std::string(BOLD) + i.fileName + RESET + "\n");
    const char* kindNames[] = {N_("Unknown"), N_("ISO image"), N_("Disk image"), N_("VHD disk image"), N_("Compressed disk image")};
    row(T("Type"), T(kindNames[int(i.kind)]) + (i.compression != Compression::None ? std::string(" (") + compressionName(i.compression) + ")" : ""));
    row(T("Size"), humanBytes(i.fileSize) + (i.dataSize && i.dataSize != i.fileSize ? " → " + humanBytes(i.dataSize) : ""));
    if (!i.friendlyName.empty())
        row(T("Name"), i.friendlyName);
    if (!i.label.empty())
        row(T("Label"), i.label);
    if (!i.fsTree.empty())
        row(T("File system"), i.fsTree);
    if (i.isWindows)
        row(T("System"), std::string("Windows") + (i.isWindows11 ? " 11" : ""));
    else if (i.isLinux)
        row(T("System"), i.distro);
    row(T("UEFI boot"), yesno(i.uefiBootable()));
    if (i.efiSigned)
        row(T("Secure Boot"), T("signed") + " (" + i.efiSigner + ")" + (i.efiOldMicrosoftCa ? "  ⚠ " + T("old 2011 certificate") : ""));
    else if (i.uefiBootable())
        row(T("Secure Boot"), T("not signed: turn Secure Boot off to boot it"));
    row(T("BIOS boot"), yesno(i.biosBootable()));
    row(T("Hybrid (isohybrid)"), yesno(i.isohybrid));
    if (i.fileCount)
        row(T("Files"), std::to_string(i.fileCount) + "  (" + humanBytes(i.filesTotal) + ")");
    if (!i.over4GB.empty())
        row(T("Files over 4 GB"), i.over4GB.front() + (i.over4GB.size() > 1 ? " …" : ""));
    if (!i.persistenceKind.empty())
        row(T("Persistence"), T("supported"));
    row(T("Recommended mode"), i.ddRecommended() ? T("DD (write as is)") : T("ISO (copy files)"));
    return kOk;
}

int writeOne(const Args& a, bool formatOnly, const DriveInfo& d, bool planOnlyOutput, Plan& planOut);

int cmdWrite(const Args& a, bool formatOnly)
{
    std::vector<std::string> devs = a.devices;
    if (devs.empty())
        devs.push_back(a.positional.empty() ? "" : a.positional.front());
    std::vector<DriveInfo> drives;
    for (const std::string& dv : devs) {
        DriveInfo d;
        if (!resolveDrive(a, d, dv))
            return kUsage;
        drives.push_back(d);
    }
    if (drives.size() == 1) {
        Plan p;
        return writeOne(a, formatOnly, drives[0], true, p);
    }
    // Several drives: plan each, ask once, then write them all in parallel.
    gMulti = true;
    std::vector<Plan> plans(drives.size());
    for (size_t i = 0; i < drives.size(); ++i) {
        Args single = a;
        single.opts["--plan-only"] = "";
        if (writeOne(single, formatOnly, drives[i], true, plans[i]) != kOk)
            return kFailed;
    }
    if (a.has("--plan-only"))
        return kOk;
    if (!requireAdmin())
        return kNeedAdmin;
    if (!a.has("--yes") && !gJson) {
        out(std::string("\n") + RED + BOLD + T("WARNING: ALL DATA ON THIS DRIVE WILL BE DESTROYED") + RESET + "\n");
        for (const auto& d : drives)
            out("  " + d.displayName() + "\n");
        out(format(T("Type %1 to continue: "), {"YES"}));
        std::string ans;
        std::getline(std::cin, ans);
        if (trim(ans) != "YES" && trim(ans) != "yes")
            return kCancelled;
    }
    std::vector<int> codes(drives.size(), kOk);
    std::vector<std::thread> threads;
    for (size_t i = 0; i < drives.size(); ++i)
        threads.emplace_back([&, i] {
            codes[i] = runJob([&](const JobContext& ctx) { runWrite(drives[i], plans[i], ctx); }, drives[i].path);
            if (codes[i] == kOk) {
                if (gJson)
                    jsonEvent(devTag(drives[i].path) + "\"type\":\"done\"");
                else
                    out(std::string(GREEN) + "✓ " + drives[i].path + "  " + T("Ready. The drive was written successfully.") + RESET + "\n");
            }
        });
    for (auto& t : threads)
        t.join();
    int worst = kOk;
    for (int c : codes)
        worst = std::max(worst, c);
    return worst;
}

int writeOne(const Args& a, bool formatOnly, const DriveInfo& d, bool planOnlyOutput, Plan& planOut)
{
    (void)planOnlyOutput;
    WriteOptions o;
    if (!formatOnly) {
        o.image = a.get("--image");
        if (o.image.empty() && a.positional.size() > 1)
            o.image = a.positional[1];
        if (o.image.empty()) {
            printError(T("Choose an image with --image"));
            return kUsage;
        }
    }
    const std::string mode = lower(a.get("--mode", "auto"));
    o.mode = mode == "iso" ? WriteMode::Iso : mode == "dd" ? WriteMode::Dd : WriteMode::Auto;
    const std::string scheme = lower(a.get("--scheme", formatOnly ? "mbr" : "gpt"));
    o.scheme = scheme == "mbr" ? Scheme::Mbr : Scheme::Gpt;
    if (a.has("--target") && !parseTarget(a.get("--target"), o.target)) {
        printError(T("--target must be uefi, bios or bios+uefi"));
        return kUsage;
    }
    if (!a.has("--target"))
        o.target = o.scheme == Scheme::Mbr ? TargetSystem::BiosUefi : TargetSystem::Uefi;
    if (a.has("--fs")) {
        if (!parseFs(a.get("--fs"), o.fs)) {
            printError(T("--fs must be fat32, ntfs, exfat, ext2, ext3 or ext4"));
            return kUsage;
        }
        o.fsAuto = false;
    }
    o.label = a.get("--label");
    if (a.has("--cluster")) {   // bytes ("4096") or with a unit ("32K")
        const std::string cl = a.get("--cluster");
        o.clusterSize = cl.find_first_not_of("0123456789") == std::string::npos ? uint32_t(std::stoul(cl)) : uint32_t(parseSize(cl));
    }
    o.quickFormat = !a.has("--full-format");
    o.badBlockPasses = a.has("--badblocks") ? std::atoi(a.get("--badblocks").c_str()) : 0;
    o.verify = !a.has("--no-verify");
    o.eject = a.has("--eject");
    o.persistenceBytes = parseSize(a.get("--persistence"));
    o.splitWim = a.has("--split-wim");
    {
        const std::string wl = lower(a.get("--win-layout", "auto"));
        o.winLayout = wl == "ntfs" ? WinLayout::Ntfs : wl == "split" ? WinLayout::Split : wl == "dual" ? WinLayout::Dual : WinLayout::Auto;
    }
    o.autorun = !a.has("--no-autorun");
    o.expectedId = a.get("--expect-id");
    o.windows.bypassHardwareChecks = a.has("--win-bypass");
    o.windows.noOnlineAccount = a.has("--win-no-online");
    o.windows.localAccount = a.get("--win-local-user");
    o.windows.skipPrivacyQuestions = a.has("--win-no-privacy");
    o.windows.disableBitLocker = a.has("--win-no-bitlocker");
    o.windows.locale = a.get("--win-locale");
    o.windows.computerName = a.get("--win-computer-name");

    const Plan plan = planWrite(d, o);
    planOut = plan;
    if (gJson) {
        std::string w, e;
        for (const auto& s : plan.warnings)
            w += (w.empty() ? "" : ",") + q(s);
        for (const auto& s : plan.errors)
            e += (e.empty() ? "" : ",") + q(s);
        jsonEvent(devTag(d.path) + "\"type\":\"plan\",\"dd\":" + std::string(plan.dd ? "true" : "false") + ",\"fs\":" + q(fsName(plan.options.fs)) +
                  ",\"scheme\":" + q(schemeName(plan.options.scheme)) + ",\"target\":" + q(targetName(plan.options.target)) +
                  ",\"label\":" + q(plan.options.label) + ",\"bios\":" + q(plan.biosLoader) + ",\"uefiNtfs\":" + (plan.uefiNtfs ? "true" : "false") +
                  ",\"dual\":" + (plan.dualPartition ? "true" : "false") + ",\"split\":" + (plan.options.splitWim ? "true" : "false") +
                  ",\"warnings\":[" + w + "],\"errors\":[" + e + "]");
        if (a.has("--plan-only"))
            return plan.errors.empty() ? kOk : kFailed;
    } else {
        out(std::string(BOLD) + T("Plan") + RESET + "\n");
        if (!formatOnly)
            out("  " + T("Image") + ": " + plan.image.fileName + "\n");
        out("  " + T("Drive") + ": " + d.displayName() + "\n");
        if (plan.dd)
            out("  " + T("Mode") + ": DD\n");
        else
            out("  " + T("Mode") + ": " + (formatOnly ? T("format") : "ISO") + " · " + schemeName(plan.options.scheme) + " · " +
                targetName(plan.options.target) + " · " + fsName(plan.options.fs) + " · \"" + plan.options.label + "\"" +
                (plan.biosLoader.empty() || plan.biosLoader == "image" ? "" : " · BIOS: " + plan.biosLoader) +
                (plan.uefiNtfs ? " · UEFI:NTFS" : "") + (plan.dualPartition ? " · FAT32 + NTFS" : "") +
                (plan.options.splitWim ? " · install.swm" : "") + "\n");
        for (const auto& w : plan.warnings)
            out(std::string("  ") + YELLOW + "! " + w + RESET + "\n");
        for (const auto& e : plan.errors)
            out(std::string("  ") + RED + "✗ " + e + RESET + "\n");
        if (a.has("--plan-only"))
            return plan.errors.empty() ? kOk : kFailed;
    }
    if (!plan.errors.empty())
        return kFailed;
    if (!d.isFile && !requireAdmin())   // image files as targets need no special rights
        return kNeedAdmin;
    if (!confirmErase(a, d))
        return kCancelled;
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = runJob([&](const JobContext& ctx) { runWrite(d, plan, ctx); }, gJson ? d.path : std::string());
    if (rc == kOk) {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (gJson)
            jsonEvent(devTag(d.path) + "\"type\":\"done\",\"seconds\":" + std::to_string(int(secs)));
        else
            out(std::string(GREEN) + BOLD + "✓ " + T("Ready. The drive was written successfully.") + RESET + DIM + "  (" + eta(secs) + ")" + RESET + "\n");
    }
    return rc;
}

int cmdBadBlocks(const Args& a)
{
    DriveInfo d;
    if (!resolveDrive(a, d))
        return kUsage;
    if (!requireAdmin())
        return kNeedAdmin;
    if (!confirmErase(a, d))
        return kCancelled;
    const int passes = std::atoi(a.get("--passes", "1").c_str());
    BadBlockReport r;
    const int rc = runJob([&](const JobContext& ctx) { r = runBadBlocks(d, passes, ctx); });
    if (rc != kOk)
        return rc;
    if (gJson) {
        jsonEvent("\"type\":\"badblocks\",\"tested\":" + std::to_string(r.testedBytes) + ",\"bad\":" + std::to_string(r.badSectors) +
                  ",\"fake\":" + (r.fakeCapacity ? "true" : "false") + ",\"real\":" + std::to_string(r.realCapacity));
        return r.badSectors ? kFailed : kOk;
    }
    if (r.fakeCapacity)
        out(std::string(RED) + BOLD + format(T("FAKE DRIVE: it reports %1 but only about %2 can really be stored"),
                                             {humanBytes(d.size), humanBytes(r.realCapacity)}) + RESET + "\n");
    if (r.badSectors) {
        out(std::string(RED) + format(T("%1 bad sector(s) found; the drive is not reliable"), {std::to_string(r.badSectors)}) + RESET + "\n");
        for (const auto& [s, e] : r.badRanges)
            out("  " + humanBytes(s) + " – " + humanBytes(e) + "\n");
        return kFailed;
    }
    out(std::string(GREEN) + "✓ " + format(T("No bad blocks found in %1"), {humanBytes(r.testedBytes)}) + RESET + "\n");
    return kOk;
}

int cmdFakeCheck(const Args& a)
{
    DriveInfo d;
    if (!resolveDrive(a, d))
        return kUsage;
    if (!requireAdmin())
        return kNeedAdmin;
    BadBlockReport r;
    const int rc = runJob([&](const JobContext& ctx) { r = quickFakeCheck(d, ctx); }, gJson ? d.path : std::string());
    if (rc != kOk)
        return rc;
    if (gJson) {
        jsonEvent(devTag(d.path) + "\"type\":\"fakecheck\",\"size\":" + std::to_string(d.size) + ",\"fake\":" +
                  (r.fakeCapacity ? "true" : "false") + ",\"real\":" + std::to_string(r.realCapacity) + ",\"bad\":" + std::to_string(r.badSectors));
        return r.fakeCapacity ? kFailed : kOk;
    }
    if (r.fakeCapacity) {
        out(std::string(RED) + BOLD + format(T("FAKE DRIVE: it reports %1 but only about %2 can really be stored"),
                                             {humanBytes(d.size), humanBytes(r.realCapacity)}) + RESET + "\n");
        return kFailed;
    }
    if (r.badSectors) {
        out(std::string(YELLOW) + format(T("%1 bad sector(s) found; the drive is not reliable"), {std::to_string(r.badSectors)}) + RESET + "\n");
        return kFailed;
    }
    out(std::string(GREEN) + "✓ " + format(T("The drive really stores %1 (quick check, your files were kept)"), {humanBytes(d.size)}) + RESET + "\n");
    return kOk;
}

int cmdSave(const Args& a)
{
    DriveInfo d;
    const std::string dev = a.get("--device", a.positional.empty() ? "" : a.positional.front());
    if (dev.empty() || !findDrive(dev, d, a.has("--allow-file"))) {
        printError(format(T("Drive not found: %1"), {dev}));
        return kUsage;
    }
    const std::string outPath = a.get("--out", a.positional.size() > 1 ? a.positional[1] : "");
    if (outPath.empty()) {
        printError(T("Give the output file with --out (.img, .img.gz, .img.xz, .img.zst or .vhd)"));
        return kUsage;
    }
    if (!d.isFile && !requireAdmin())
        return kNeedAdmin;
    const int rc = runJob([&](const JobContext& ctx) { saveDriveImage(d, outPath, a.has("--used-only"), ctx); });
    if (rc == kOk && !gJson)
        out(std::string(GREEN) + "✓ " + format(T("Saved to %1"), {outPath}) + RESET + "\n");
    if (rc == kOk && gJson)
        jsonEvent("\"type\":\"done\"");
    return rc;
}

int cmdHash(const Args& a)
{
    const std::string path = a.get("--image", a.positional.empty() ? "" : a.positional.front());
    if (path.empty()) {
        printError(T("Give the file to check"));
        return kUsage;
    }
    std::vector<HashAlgo> algos;
    for (const std::string& s : split(a.get("--algo", "md5,sha1,sha256,sha512"), ',')) {
        HashAlgo h;
        if (parseHashAlgo(trim(s), h))
            algos.push_back(h);
    }
    if (algos.empty()) {
        printError(T("--algo must list md5, sha1, sha256 or sha512"));
        return kUsage;
    }
    std::vector<std::pair<HashAlgo, std::string>> res;
    const int rc = runJob([&](const JobContext& ctx) { res = hashFile(path, algos, ctx); });
    if (rc != kOk)
        return rc;
    std::string expected = lower(trim(a.get("--check")));
    HashAlgo pubAlgo = HashAlgo::Sha256;
    std::string pub = findPublishedHash(path, pubAlgo);
    if (gJson) {
        std::string s;
        for (const auto& [h, v] : res)
            s += (s.empty() ? "" : ",") + q(hashName(h)) + ":" + q(v);
        jsonEvent("\"type\":\"hashes\"," + s + ",\"published\":" + q(pub) + ",\"publishedAlgo\":" + q(hashName(pubAlgo)));
        return kOk;
    }
    for (const auto& [h, v] : res)
        out(std::string(BOLD) + hashName(h) + RESET + std::string(9 - std::strlen(hashName(h)), ' ') + v + "\n");
    auto match = [&](const std::string& want) {
        for (const auto& [h, v] : res)
            if (v == want)
                return true;
        return false;
    };
    int status = kOk;
    if (!expected.empty()) {
        if (match(expected))
            out(std::string(GREEN) + "✓ " + T("The checksum matches") + RESET + "\n");
        else {
            out(std::string(RED) + "✗ " + T("The checksum does NOT match: the file is damaged or not the original") + RESET + "\n");
            status = kFailed;
        }
    } else if (!pub.empty()) {
        if (match(pub))
            out(std::string(GREEN) + "✓ " + format(T("Matches the published %1 checksum"), {hashName(pubAlgo)}) + RESET + "\n");
        else {
            out(std::string(RED) + "✗ " + format(T("Does NOT match the published %1 checksum"), {hashName(pubAlgo)}) + RESET + "\n");
            status = kFailed;
        }
    }
    return status;
}

void printHelp()
{
    out(std::string(BOLD) + "BootAz " + BOOTAZ_VERSION + RESET + " — " + T("bootable USB drives from ISO and disk images") + "\n" +
        DIM + "Developed By Goshgar Hasanzadeh · https://goshgarhasanov.github.io" + RESET + "\n\n" +
        T("Usage") + ":\n"
        "  bootaz-cli                               " + T("interactive wizard") + "\n"
        "  bootaz-cli devices [--all] [--json]      " + T("list USB drives (--all: internal drives too)") + "\n"
        "  bootaz-cli analyze <image> [--json]      " + T("what an image contains and how it boots") + "\n"
        "  bootaz-cli write --device D --image I    " + T("make a bootable drive") + "\n"
        "        [--mode auto|iso|dd] [--scheme gpt|mbr] [--target uefi|bios|bios+uefi]\n"
        "        [--fs fat32|ntfs|exfat] [--label L] [--cluster BYTES] [--full-format]\n"
        "        [--badblocks 1-4] [--no-verify] [--eject] [--persistence SIZE]\n"
        "        [--win-layout auto|ntfs|split|dual]  (" + T("Windows images with install.wim over 4 GB") + ")\n"
        "        [--win-bypass] [--win-no-online] [--win-local-user NAME] [--win-no-privacy]\n"
        "        [--win-no-bitlocker] [--win-locale az-Latn-AZ] [--win-computer-name NAME]\n"
        "        [--plan-only] [--yes]\n"
        "  bootaz-cli format --device D [--fs F] [--label L] [--scheme mbr|gpt]   " + T("erase and format (not bootable)") + "\n"
        "  bootaz-cli badblocks --device D [--passes 1-4]    " + T("test the drive; finds fake-capacity drives") + "\n"
        "  bootaz-cli fakecheck --device D                   " + T("quick fake-capacity check that keeps your files") + "\n"
        "  bootaz-cli restore --device D [--fs exfat]        " + T("make a boot drive an ordinary drive again") + "\n"
        "  bootaz-cli save --device D --out FILE [--used-only]   " + T("save a drive to .img / .img.gz / .xz / .zst / .vhd") + "\n"
        "  bootaz-cli hash <file> [--algo sha256,...] [--check HASH]   " + T("checksums, checked against .sha256 files") + "\n"
        "  " + T("Several --device options write the same image to several drives at once.") + "\n"
        "\n" + T("Options for every command") + ": --lang az|en, --json, --yes\n" +
        T("Exit codes: 0 done, 1 wrong usage, 2 failed, 3 cancelled, 4 administrator rights needed") + "\n");
}

// ---- interactive wizard

std::string ask(const std::string& prompt, const std::string& def = "")
{
    out(std::string(BOLD) + prompt + RESET + (def.empty() ? "" : " [" + def + "]") + ": ");
    std::string s;
    if (!std::getline(std::cin, s))
        throw Cancelled();
    s = trim(s);
    return s.empty() ? def : s;
}

int wizard()
{
    out(std::string("\n") + BLUE + BOLD + "  BootAz" + RESET + "  " + T("bootable USB drives") + DIM + "   ·  Developed By Goshgar Hasanzadeh" + RESET + "\n\n");
    try {
        std::vector<DriveInfo> drives = listDrives({});
        while (drives.empty()) {
            out(T("No USB drive found. Plug one in and press Enter (q to quit).") + " ");
            std::string s;
            if (!std::getline(std::cin, s) || trim(s) == "q")
                return kCancelled;
            drives = listDrives({});
        }
        for (size_t i = 0; i < drives.size(); ++i)
            out("  " + std::string(BOLD) + std::to_string(i + 1) + RESET + ") " + drives[i].displayName() + "\n");
        const int pick = std::atoi(ask(T("Drive"), "1").c_str());
        if (pick < 1 || pick > int(drives.size()))
            return kUsage;
        const DriveInfo d = drives[size_t(pick - 1)];

        const std::string image = ask(T("Image file (empty = only format the drive)"));
        Args a;
        a.opts["--device"] = d.path;
        if (!image.empty()) {
            a.opts["--image"] = image;
            Args an;
            an.positional = {image};
            cmdAnalyze(an);
            const std::string mode = ask(T("Mode: auto / iso / dd"), "auto");
            a.opts["--mode"] = mode;
            if (mode != "dd") {
                a.opts["--scheme"] = ask(T("Partition scheme: gpt / mbr"), "gpt");
                if (a.opts["--scheme"] == "mbr")
                    a.opts["--target"] = ask(T("Target system: bios+uefi / bios / uefi"), "bios+uefi");
                const std::string fsx = ask(T("File system: auto / fat32 / ntfs / exfat"), "auto");
                if (fsx != "auto")
                    a.opts["--fs"] = fsx;
                const ImageInfo info = analyzeImage(image);
                if (info.isWindows) {
                    if (ask(T("Remove the TPM / Secure Boot / RAM requirement? (y/n)"), "y") == "y")
                        a.opts["--win-bypass"] = "";
                    if (ask(T("Allow setup without a Microsoft account? (y/n)"), "y") == "y")
                        a.opts["--win-no-online"] = "";
                    const std::string user = ask(T("Local account name (empty = none)"));
                    if (!user.empty())
                        a.opts["--win-local-user"] = user;
                    if (ask(T("Skip the privacy questions? (y/n)"), "y") == "y")
                        a.opts["--win-no-privacy"] = "";
                    if (ask(T("Turn off automatic BitLocker encryption? (y/n)"), "n") == "y")
                        a.opts["--win-no-bitlocker"] = "";
                }
                if (!info.persistenceKind.empty()) {
                    const std::string p = ask(T("Persistence size, e.g. 4G (empty = none)"));
                    if (!p.empty())
                        a.opts["--persistence"] = p;
                }
            }
            return cmdWrite(a, false);
        }
        a.opts["--fs"] = ask(T("File system: fat32 / ntfs / exfat / ext4"), "fat32");
        a.opts["--label"] = ask(T("Volume label"), "BOOTAZ");
        return cmdWrite(a, true);
    } catch (const Cancelled&) {
        return kCancelled;
    }
}

void onSignal(int) { gCancel = true; }

int realMain(const std::vector<std::string>& argv)
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    i18n::setLanguage(i18n::detectLanguage());

    Args a;
    std::string err;
    if (!parseArgs(argv, a, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return kUsage;
    }
    if (a.has("--lang")) {
        i18n::Lang l;
        if (i18n::parseLang(a.get("--lang"), l))
            i18n::setLanguage(l);
    }
    gJson = a.has("--json") && a.command != "devices" && a.command != "analyze";
    gColor = !gJson && isatty(fileno(stdout)) && !std::getenv("NO_COLOR");
    if (a.has("--json") && (a.command == "write" || a.command == "format" || a.command == "badblocks" || a.command == "save" ||
                            a.command == "fakecheck" || a.command == "restore")) {
        // The desktop app cancels by writing "cancel" to our standard input.
        std::thread([] {
            std::string line;
            while (std::getline(std::cin, line))
                if (trim(line) == "cancel")
                    gCancel = true;
        }).detach();
    }

    if (a.has("--version")) {
        out(std::string("BootAz ") + BOOTAZ_VERSION + "\nDeveloped By Goshgar Hasanzadeh\n");
        return kOk;
    }
    if (a.has("--help") || a.command == "help") {
        printHelp();
        return kOk;
    }
    if (a.command.empty())
        return wizard();
    if (a.command == "devices" || a.command == "list")
        return cmdDevices(a);
    if (a.command == "analyze" || a.command == "info")
        return cmdAnalyze(a);
    if (a.command == "write")
        return cmdWrite(a, false);
    if (a.command == "format")
        return cmdWrite(a, true);
    if (a.command == "badblocks" || a.command == "test")
        return cmdBadBlocks(a);
    if (a.command == "fakecheck" || a.command == "fake")
        return cmdFakeCheck(a);
    if (a.command == "restore") {
        // Back to an ordinary drive: one partition over the whole drive.
        Args r = a;
        if (!r.has("--scheme"))
            r.opts["--scheme"] = "mbr";
        if (!r.has("--label"))
            r.opts["--label"] = "USB";
        return cmdWrite(r, true);
    }
    if (a.command == "save" || a.command == "backup")
        return cmdSave(a);
    if (a.command == "hash" || a.command == "checksum")
        return cmdHash(a);
    std::fprintf(stderr, "%s\n", format(T("Unknown command: %1 (see: bootaz-cli help)"), {a.command}).c_str());
    return kUsage;
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // Colours in Windows 10+ consoles.
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(utf16ToUtf8(reinterpret_cast<const char16_t*>(argv[i])));
    const int rc = realMain(args);
    // A thread may still be blocked reading standard input (cancel channel): leave without
    // running static destructors that would wait for it.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(rc);
}
#else
int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    const int rc = realMain(args);
    // A thread may still be blocked reading standard input (cancel channel): leave without
    // running static destructors that would wait for it.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(rc);
}
#endif
