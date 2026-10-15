// Parts of the drive layer shared by Linux and Windows.

#include "Disk.h"
#include "I18n.h"

namespace bootaz {

std::string DriveInfo::displayName() const
{
    std::string name = trim(vendor + " " + model);
    if (name.empty())
        name = path;
    std::string labels;
    for (const VolumeInfo& v : volumes)
        if (!v.label.empty()) {
            labels = v.label;
            break;
        }
    std::string s = name;
    if (!labels.empty())
        s += " (" + labels + ")";
    s += " [" + humanBytes(size) + "]";
    if (!isFile)
        s += "  " + path;
    return s;
}

// ---------------------------------------------------------------- SubTarget

void SubTarget::readAt(uint64_t o, void* b, size_t n)
{
    if (o + n > size_)
        throw Error("Read past the end of the partition");
    p_.readAt(off_ + o, b, n);
}

void SubTarget::writeAt(uint64_t o, const void* b, size_t n)
{
    if (o + n > size_)
        throw Error("Write past the end of the partition");
    p_.writeAt(off_ + o, b, n);
}

} // namespace bootaz
