#pragma once
// Platform.h FileSystem over a host folder standing in for the SD card. Core paths are
// absolute-from-card-root ("/Projects/x") and are mapped under the root folder.
#include "Platform.h"
#include "VirtualSectorMap.h"

#include <string>
#include <vector>

class FileSystemPosix : public FileSystem
{
  public:
    explicit FileSystemPosix(std::string root);
    ~FileSystemPosix();

    const std::string& root() const { return root_; }
    // Host path for a card path ("/a/b" -> "<root>/a/b").
    std::string hostPath(const char* cardPath) const;

    bool isSdCardInserted() override { return true; }
    void setMountEventCallback(CardStateChangeCallback funct) override { mountCallback_ = funct; }

    bool exists(const char* path) override;
    bool rename(const char* oldPath, const char* newPath) override;
    bool mkdir(uint8_t hidden, const char* path, bool pFlag = true) override;
    bool remove(const char* path) override;
    bool removeDirWithFiles(const char* path) override;
    bool removeOnlyDirFiles(const char* path) override;

    size_t getFileSize(const char* path) override;
    size_t getFileSize(FileHandle fileHandle) override;
    size_t getFileAvailable(FileHandle fileHandle) override;

    std::tuple<bool, FileHandle> openFile(const char* filepath,
                                          FileOpenMode openMode = kRead,
                                          FileHandle* fileHandle = nullptr) override;
    int32_t readFile(void* destBuf, size_t bufLen, FileHandle file) override;
    int32_t writeFile(const void* data, size_t dataSize, FileHandle fileHandle) override;
    bool seekFileCur(int offset, FileHandle fileHandle) override;
    bool seekFile(uint32_t pos, FileHandle fileHandle) override;
    void closeFile(FileHandle file, bool deleteOnClose = true) override;
    void endFile(FileHandle file) override;
    bool isFileOpen(FileHandle file) override;
    bool enableFastSeek(FileHandle fileHandle,
                        uint32_t* clmtBuffer,
                        uint32_t clmtElementCount,
                        uint32_t* outDatabaseSector = nullptr,
                        uint32_t* outSectorsPerCluster = nullptr) override;
    bool readSectorsAsync(uint8_t* buffer, uint32_t startSector, uint32_t sectorCount) override;
    bool pollAsyncRead(bool* outSuccess = nullptr) override;
    bool waitAsyncRead() override;
    bool isAsyncReadBusy() override;

    int createInstrumentsList(const char* path,
                              char** list,
                              uint8_t list_length,
                              uint32_t* sizeOut) override;
    int countContainedFiles(const char* path) override;
    int createFilesList(const char* path,
                        uint8_t start_line,
                        char** list,
                        uint8_t list_length,
                        uint16_t max_used_memory,
                        enListFileFilter filter = enListFileFilter_all,
                        uint16_t skip_count = 0,
                        uint16_t max_files_in_subdir = 0,
                        enListFileFilter folderSizeFilter = enListFileFilter_all) override;
    void listDirContents(const char* dirpath,
                         DirContentType contentType,
                         std::function<bool(const char* name)> nameFilter,
                         char* namesBuffer,
                         int namesBufferSize,
                         int maxNameLen,
                         int& outNamesCount,
                         char** fileList) override;

    int readNbFiles(const char* romPath) override;
    bool nesDir_open(const char* path) override;
    bool nesDir_readItem(char* path, uint8_t* isDir, uint8_t* isHid, uint32_t* size) override;
    void nesDir_close() override;

    VirtualSectorMap& sectorMap() { return sectorMap_; }

  private:
    struct DirEntry
    {
        std::string name;
        bool isDir;
        bool hidden;
        uint64_t size;
    };
    struct Handle;

    // Lists a card directory (no "." / ".."), sorted like the core's browser lists.
    bool readDir(const char* cardPath, std::vector<DirEntry>& out) const;
    bool filterEntry(const DirEntry& e, enListFileFilter filter, size_t dirPathLen) const;
    int countInSubDir(const std::string& hostDir, enListFileFilter filter, size_t dirPathLen) const;
    bool deleteContents(const std::string& hostDir) const;

    std::string root_;
    CardStateChangeCallback mountCallback_ = nullptr;
    VirtualSectorMap sectorMap_;
    bool lastAsyncSuccess_ = true;
    std::vector<DirEntry> nesEntries_;
    size_t nesPos_ = 0;
};
