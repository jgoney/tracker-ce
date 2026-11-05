#include "VirtualSectorMap.h"

#include <sys/stat.h>
#include <unistd.h>

VirtualSectorMap::~VirtualSectorMap()
{
    for (auto& e : entries_)
    {
        ::close(e.fd);
    }
}

uint32_t VirtualSectorMap::clustersForSize(uint64_t size)
{
    const uint64_t clusterBytes = uint64_t(kSectorsPerCluster) * kSectorSize;
    return static_cast<uint32_t>((size + clusterBytes - 1) / clusterBytes);
}

uint32_t VirtualSectorMap::registerFile(int fd, uint64_t size)
{
    struct stat st;
    if (fd < 0 || ::fstat(fd, &st) != 0)
    {
        return 0;
    }
    const uint32_t needed = clustersForSize(size) + 1; // +1 guard cluster, like the spec'd layout

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : entries_)
    {
        if (e.dev == uint64_t(st.st_dev) && e.ino == uint64_t(st.st_ino))
        {
            // Same file reopened: refresh our descriptor (the old file object may be gone).
            int newFd = ::dup(fd);
            if (newFd >= 0)
            {
                ::close(e.fd);
                e.fd = newFd;
            }
            if (needed <= e.clusterSpan)
            {
                return e.baseCluster;
            }
            // Grew past its range: needs a new range.
            e.baseCluster = nextCluster_;
            e.clusterSpan = needed;
            nextCluster_ += needed;
            return e.baseCluster;
        }
    }
    int own = ::dup(fd);
    if (own < 0)
    {
        return 0;
    }
    Entry e{ uint64_t(st.st_dev), uint64_t(st.st_ino), own, nextCluster_, needed };
    nextCluster_ += needed;
    entries_.push_back(e);
    return e.baseCluster;
}

bool VirtualSectorMap::resolve(uint32_t lba, uint32_t count, int& fd, uint64_t& offset)
{
    if (lba < kDatabaseSector || count == 0)
    {
        return false;
    }
    const uint64_t rel = lba - kDatabaseSector;
    const uint64_t cluster = kFirstCluster + rel / kSectorsPerCluster;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& e : entries_)
    {
        if (cluster < e.baseCluster || cluster >= uint64_t(e.baseCluster) + e.clusterSpan)
        {
            continue;
        }
        const uint64_t fileSector = rel - uint64_t(e.baseCluster - kFirstCluster) * kSectorsPerCluster;
        const uint64_t spanSectors = uint64_t(e.clusterSpan) * kSectorsPerCluster;
        if (fileSector + count > spanSectors)
        {
            return false;
        }
        fd = e.fd;
        offset = fileSector * kSectorSize;
        return true;
    }
    return false;
}
