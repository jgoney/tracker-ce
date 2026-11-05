#pragma once
// Maps files to a virtual FAT-like sector space so that the core's SD streaming code
// (VoiceStreamManager: CLMT -> LBA -> FileSystem::readSectorsAsync) works on a host filesystem.
//
//   lba = kDatabaseSector + (cluster - 2) * kSectorsPerCluster + sectorInCluster
//
// Every registered file owns a disjoint cluster range starting at cluster 2. Thread-safe.
#include <cstdint>
#include <mutex>
#include <vector>

class VirtualSectorMap
{
  public:
    static constexpr uint32_t kSectorSize = 512;
    static constexpr uint32_t kSectorsPerCluster = 64;
    static constexpr uint32_t kDatabaseSector = 2048;
    static constexpr uint32_t kFirstCluster = 2;

    VirtualSectorMap() = default;
    ~VirtualSectorMap();
    VirtualSectorMap(const VirtualSectorMap&) = delete;
    VirtualSectorMap& operator=(const VirtualSectorMap&) = delete;

    // Returns the base cluster of the file. Idempotent per (st_dev, st_ino): re-registering the
    // same file returns the same base cluster, as long as it still fits in the cluster range it
    // was given (a file that grew gets a fresh range). The map keeps its own dup() of fd, so the
    // caller may close fd afterwards. Returns 0 on failure.
    uint32_t registerFile(int fd, uint64_t size);

    // Resolves `count` sectors starting at `lba` to a file descriptor and byte offset. All the
    // sectors must lie within the cluster range of a single registered file.
    bool resolve(uint32_t lba, uint32_t count, int& fd, uint64_t& offset);

    static uint32_t clustersForSize(uint64_t size);

  private:
    struct Entry
    {
        uint64_t dev;
        uint64_t ino;
        int fd;
        uint32_t baseCluster;
        uint32_t clusterSpan; // clusters reserved (data clusters + 1 guard)
    };
    std::mutex mutex_;
    std::vector<Entry> entries_;
    uint32_t nextCluster_ = kFirstCluster;
};
