#pragma once
// Small helpers shared by the core, the CLI and the GUI.

#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bootaz {

constexpr uint64_t KiB = 1024, MiB = 1024 * KiB, GiB = 1024 * MiB;

// Thrown for any failure that should stop a job; the message is user facing (English source text,
// translated by whoever shows it).
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Thrown when the user cancels a running job.
struct Cancelled : std::runtime_error {
    Cancelled() : std::runtime_error("Cancelled") {}
};

std::string humanBytes(uint64_t bytes);                  // "14.9 GB"
std::string lower(std::string s);
std::string upper(std::string s);
std::string trim(const std::string& s);
bool startsWith(const std::string& s, const std::string& prefix);
bool endsWith(const std::string& s, const std::string& suffix);
bool iequals(const std::string& a, const std::string& b);
std::vector<std::string> split(const std::string& s, char sep);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
std::string format(const std::string& fmt, const std::vector<std::string>& args);  // "%1 of %2"
std::string jsonEscape(const std::string& s);
std::string readTextFile(const std::string& path);      // empty if missing
bool fileExists(const std::string& path);

// UTF-16 (little endian on disk) <-> UTF-8, for FAT long names, Joliet and UDF.
std::u16string utf8ToUtf16(const std::string& s);
std::string utf16ToUtf8(const std::u16string& s);

// Progress of a long job. `done`/`total` are bytes (or any unit) of the current stage.
struct Progress {
    std::string stage;        // short English stage name, e.g. "Writing image"
    uint64_t done = 0;
    uint64_t total = 0;
    double bytesPerSec = 0;   // filled in by the reporter
};

// Hooks every job receives: progress, a log line, and a cancel flag polled between blocks.
struct JobContext {
    std::function<void(const Progress&)> onProgress;
    std::function<void(const std::string&)> onLog;
    std::atomic<bool>* cancel = nullptr;

    void log(const std::string& line) const;
    void progress(const std::string& stage, uint64_t done, uint64_t total) const;
    void checkCancel() const;
};

} // namespace bootaz
