#include "VirtualSectorMap.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

VirtualSectorMap::~VirtualSectorMap()
{
    for (auto& c : cache_)
    {
        ::close(c.fd);
    }
}

uint32_t VirtualSectorMap::clustersForSize(uint64_t size)
{
    const uint64_t clusterBytes = uint64_t(kSectorsPerCluster) * kSectorSize;
    return static_cast<uint32_t>((size + clusterBytes - 1) / clusterBytes);
}

size_t VirtualSectorMap::cachedFdCount()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

void VirtualSectorMap::cacheFd(uint64_t dev, uint64_t ino, int fd)
{
    for (auto& c : cache_)
    {
        if (c.dev == dev && c.ino == ino)
        {
            ::close(c.fd);
            c.fd = fd;
            c.lastUse = ++tick_;
            return;
        }
    }
    if (cache_.size() >= kMaxCachedFds)
    {
        size_t lru = 0;
        for (size_t i = 1; i < cache_.size(); ++i)
        {
            if (cache_[i].lastUse < cache_[lru].lastUse)
            {
                lru = i;
            }
        }
        ::close(cache_[lru].fd); // safe: every pread happens under mutex_
        cache_.erase(cache_.begin() + lru);
    }
    cache_.push_back({ dev, ino, fd, ++tick_ });
}

uint32_t VirtualSectorMap::registerFile(int fd, uint64_t size, const std::string& pathIn)
{
    struct stat st;
    if (fd < 0 || ::fstat(fd, &st) != 0)
    {
        return 0;
    }
    std::string path = pathIn;
    if (path.empty())
    {
        char buf[MAXPATHLEN];
        if (::fcntl(fd, F_GETPATH, buf) == 0)
        {
            path = buf;
        }
    }
    const uint32_t needed = clustersForSize(size) + 1; // +1 guard cluster
    const uint64_t dev = uint64_t(st.st_dev), ino = uint64_t(st.st_ino);

    std::lock_guard<std::mutex> lock(mutex_);
    int own = ::fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (own >= 0)
    {
        cacheFd(dev, ino, own);
    }
    Range* newest = nullptr;
    for (auto& r : ranges_)
    {
        if (r.dev == dev && r.ino == ino)
        {
            if (!path.empty())
            {
                r.path = path;
            }
            if (r.clusterSpan >= needed)
            {
                return r.baseCluster; // idempotent
            }
            newest = &r;
        }
    }
    (void)newest; // older ranges stay valid as aliases of this file
    if (own < 0 && path.empty())
    {
        return 0;
    }
    ranges_.push_back({ dev, ino, path, nextCluster_, needed });
    nextCluster_ += needed;
    return ranges_.back().baseCluster;
}

const VirtualSectorMap::Range* VirtualSectorMap::findRange(uint32_t lba,
                                                           uint32_t count,
                                                           uint64_t& fileByteOffset) const
{
    if (lba < kDatabaseSector || count == 0)
    {
        return nullptr;
    }
    const uint64_t rel = lba - kDatabaseSector;
    const uint64_t cluster = kFirstCluster + rel / kSectorsPerCluster;
    for (const auto& r : ranges_)
    {
        if (cluster < r.baseCluster || cluster >= uint64_t(r.baseCluster) + r.clusterSpan)
        {
            continue;
        }
        const uint64_t fileSector = rel - uint64_t(r.baseCluster - kFirstCluster) * kSectorsPerCluster;
        if (fileSector + count > uint64_t(r.clusterSpan) * kSectorsPerCluster)
        {
            return nullptr;
        }
        fileByteOffset = fileSector * kSectorSize;
        return &r;
    }
    return nullptr;
}

int VirtualSectorMap::fdFor(const Range& r)
{
    for (auto& c : cache_)
    {
        if (c.dev == r.dev && c.ino == r.ino)
        {
            c.lastUse = ++tick_;
            return c.fd;
        }
    }
    if (r.path.empty())
    {
        return -1;
    }
    int fd = ::open(r.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return -1;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 || uint64_t(st.st_dev) != r.dev || uint64_t(st.st_ino) != r.ino)
    {
        ::close(fd); // replaced file: refuse
        return -1;
    }
    cacheFd(r.dev, r.ino, fd);
    return fd;
}

bool VirtualSectorMap::resolve(uint32_t lba, uint32_t count, int& fd, uint64_t& offset)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const Range* r = findRange(lba, count, offset);
    if (r == nullptr)
    {
        return false;
    }
    fd = fdFor(*r);
    return fd >= 0;
}

bool VirtualSectorMap::read(uint32_t lba, uint32_t count, uint8_t* buf)
{
    std::lock_guard<std::mutex> lock(mutex_); // held across pread: eviction can't close the fd
    uint64_t offset = 0;
    const Range* r = findRange(lba, count, offset);
    if (r == nullptr)
    {
        return false;
    }
    const int fd = fdFor(*r);
    if (fd < 0)
    {
        return false;
    }
    const size_t total = size_t(count) * kSectorSize;
    size_t done = 0;
    while (done < total)
    {
        ssize_t n = ::pread(fd, buf + done, total - done, off_t(offset + done));
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (n == 0)
        {
            break;
        }
        done += size_t(n);
    }
    if (done < total)
    {
        memset(buf + done, 0, total - done);
    }
    return true;
}
