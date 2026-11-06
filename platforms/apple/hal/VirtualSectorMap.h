#pragma once
// Maps files to a virtual FAT-like sector space so that the core's SD streaming code
// (VoiceStreamManager: CLMT -> LBA -> FileSystem::readSectorsAsync) works on a host filesystem.
//
//   lba = kDatabaseSector + (cluster - 2) * kSectorsPerCluster + sectorInCluster
//
// Ranges are permanent, lightweight metadata (path, st_dev, st_ino, cluster range) so any CLMT
// handed out stays valid. File descriptors live only in a bounded LRU cache (kMaxCachedFds);
// on a miss the file is reopened by path and its st_dev/st_ino verified, so a replaced file
// fails the read instead of returning the wrong bytes. Thread-safe.
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class VirtualSectorMap
{
  public:
    static constexpr uint32_t kSectorSize = 512;
    static constexpr uint32_t kSectorsPerCluster = 64;
    static constexpr uint32_t kDatabaseSector = 2048;
    static constexpr uint32_t kFirstCluster = 2;
    static constexpr size_t kMaxCachedFds = 32;

    VirtualSectorMap() = default;
    ~VirtualSectorMap();
    VirtualSectorMap(const VirtualSectorMap&) = delete;
    VirtualSectorMap& operator=(const VirtualSectorMap&) = delete;

    // Returns the base cluster of the file (0 on failure). Idempotent per (st_dev, st_ino) while
    // the file still fits its existing range; a file that grew gets an additional range and the
    // old one stays valid as an alias. `path` is used to reopen after fd eviction; if empty it is
    // recovered from fd (F_GETPATH).
    uint32_t registerFile(int fd, uint64_t size, const std::string& path = std::string());

    // Reads `count` sectors starting at `lba` into buf (zero-filled past EOF). All sectors must lie
    // within one range. False if unmapped, the file was replaced/removed, or on I/O error.
    bool read(uint32_t lba, uint32_t count, uint8_t* buf);

    // Lookup only: resolves to an fd (cache-owned: valid only until the next call on this map)
    // and byte offset. Prefer read().
    bool resolve(uint32_t lba, uint32_t count, int& fd, uint64_t& offset);

    size_t cachedFdCount();
    static uint32_t clustersForSize(uint64_t size);

  private:
    struct Range
    {
        uint64_t dev;
        uint64_t ino;
        std::string path;
        uint32_t baseCluster;
        uint32_t clusterSpan; // data clusters + 1 guard
    };
    struct CachedFd
    {
        uint64_t dev;
        uint64_t ino;
        int fd;
        uint64_t lastUse;
    };
    const Range* findRange(uint32_t lba, uint32_t count, uint64_t& fileByteOffset) const;
    int fdFor(const Range& r); // mutex held
    void cacheFd(uint64_t dev, uint64_t ino, int fd); // takes ownership; mutex held

    std::mutex mutex_;
    std::vector<Range> ranges_;
    std::vector<CachedFd> cache_;
    uint64_t tick_ = 0;
    uint32_t nextCluster_ = kFirstCluster;
};
