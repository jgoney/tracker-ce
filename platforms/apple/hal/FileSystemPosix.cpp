#include "FileSystemPosix.h"

#include "util_string.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// Open file. closeFile(fh, false) closes the descriptor but keeps the object so the caller can
// reopen into it via openFile(path, mode, &fh); closeFile(fh, true) frees it. Same as the FatFs
// implementation's SdFile pool slots (platforms/imx/source/FileSystemImpl.cpp:313-334).
struct FileSystemPosix::Handle
{
    int fd = -1;
    bool writable = false;
    std::string path; // host path
};

namespace
{
constexpr size_t kMaxPath = 255; // SdDir::dir_path size, used by the wave/inst length check

bool isHiddenStat(const std::string& name, const struct stat& st)
{
    return (!name.empty() && name[0] == '.') || (st.st_flags & UF_HIDDEN) != 0;
}

bool isMacMetaFile(const std::string& n) { return n.size() >= 2 && n[0] == '.' && n[1] == '_'; }
bool isMacMetaFolder(const std::string& n) { return n == "__MACOSX"; }

bool endsWithExt3(const std::string& n, const char* ext) // ext like ".wav", case-insensitive
{
    const size_t len = n.size();
    if (len < 5)
    {
        return false;
    }
    if (n[len - 4] != '.')
    {
        return false;
    }
    for (int i = 1; i <= 3; ++i)
    {
        if (tolower((unsigned char)n[len - 4 + i]) != ext[i])
        {
            return false;
        }
    }
    return true;
}

std::string toLower(const std::string& s)
{
    std::string r = s;
    for (auto& c : r)
    {
        c = (char)tolower((unsigned char)c);
    }
    return r;
}
} // namespace

FileSystemPosix::FileSystemPosix(std::string root) : root_(std::move(root))
{
    while (root_.size() > 1 && root_.back() == '/')
    {
        root_.pop_back();
    }
}

FileSystemPosix::~FileSystemPosix() = default;

std::string FileSystemPosix::hostPath(const char* cardPath) const
{
    assert(cardPath != nullptr);
    assert(strchr(cardPath, '\\') == nullptr && "card paths use '/'; fix the caller in core");
    std::string p = root_;
    if (cardPath[0] != '/')
    {
        p += '/';
    }
    p += cardPath;
    return p;
}

bool FileSystemPosix::exists(const char* path)
{
    struct stat st;
    return ::stat(hostPath(path).c_str(), &st) == 0;
}

// FatFs f_rename fails if the destination exists (FR_EXIST); POSIX rename() would overwrite.
bool FileSystemPosix::rename(const char* oldPath, const char* newPath)
{
    struct stat st;
    const std::string to = hostPath(newPath);
    if (::lstat(to.c_str(), &st) == 0)
    {
        return false;
    }
    return ::rename(hostPath(oldPath).c_str(), to.c_str()) == 0;
}

// Port of SdCard::mkdir (platforms/imx/sdmmc/sd/SD.cpp:33-57): single level (pFlag unused),
// an already-existing directory is success, `hidden` sets the hidden attribute.
bool FileSystemPosix::mkdir(uint8_t hidden, const char* path, bool)
{
    const std::string p = hostPath(path);
    if (::mkdir(p.c_str(), 0755) != 0 && errno != EEXIST)
    {
        return false;
    }
    if (hidden)
    {
        ::chflags(p.c_str(), UF_HIDDEN);
    }
    return true;
}

// f_unlink removes a file or an empty directory; so does remove().
bool FileSystemPosix::remove(const char* path)
{
    return ::remove(hostPath(path).c_str()) == 0;
}

bool FileSystemPosix::deleteContents(const std::string& hostDir) const
{
    DIR* d = ::opendir(hostDir.c_str());
    if (d == nullptr)
    {
        return false;
    }
    bool ok = true;
    while (dirent* de = ::readdir(d))
    {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
        {
            continue;
        }
        const std::string child = hostDir + "/" + de->d_name;
        struct stat st;
        if (::lstat(child.c_str(), &st) != 0)
        {
            ok = false;
            continue;
        }
        if (S_ISDIR(st.st_mode))
        {
            ok = deleteContents(child) && ::rmdir(child.c_str()) == 0 && ok;
        }
        else
        {
            ok = (::unlink(child.c_str()) == 0) && ok;
        }
    }
    ::closedir(d);
    return ok;
}

bool FileSystemPosix::removeDirWithFiles(const char* path)
{
    const std::string p = hostPath(path);
    return deleteContents(p) && ::rmdir(p.c_str()) == 0;
}

bool FileSystemPosix::removeOnlyDirFiles(const char* path)
{
    return deleteContents(hostPath(path));
}

size_t FileSystemPosix::getFileSize(const char* path)
{
    struct stat st;
    if (::stat(hostPath(path).c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    {
        return 0;
    }
    return (size_t)st.st_size;
}

size_t FileSystemPosix::getFileSize(FileHandle fileHandle)
{
    auto* h = (Handle*)fileHandle;
    struct stat st;
    if (h == nullptr || h->fd < 0 || ::fstat(h->fd, &st) != 0)
    {
        return 0;
    }
    return (size_t)st.st_size;
}

size_t FileSystemPosix::getFileAvailable(FileHandle fileHandle)
{
    auto* h = (Handle*)fileHandle;
    assert(h);
    if (h == nullptr || h->fd < 0)
    {
        return 0;
    }
    off_t pos = ::lseek(h->fd, 0, SEEK_CUR);
    size_t size = getFileSize(fileHandle);
    return (pos >= 0 && size > (size_t)pos) ? size - (size_t)pos : 0;
}

// Mirrors FileSystemImpl::openFile (FileSystemImpl.cpp:268-338): a null/empty `fileHandle`
// allocates a handle; a non-null one is reopened in place.
std::tuple<bool, FileSystem::FileHandle> FileSystemPosix::openFile(const char* filepath,
                                                                   FileOpenMode openMode,
                                                                   FileHandle* fileHandle)
{
    Handle* h;
    if (fileHandle == nullptr)
    {
        h = new Handle;
    }
    else if (*fileHandle == nullptr)
    {
        h = new Handle;
        *fileHandle = h;
    }
    else
    {
        h = (Handle*)*fileHandle;
        if (h->fd >= 0)
        {
            ::close(h->fd);
            h->fd = -1;
        }
    }

    int flags = O_CLOEXEC;
    bool append = false;
    switch (openMode)
    {
        case kWriteOpen:
            flags |= O_RDWR | O_CREAT;
            break;
        case kWriteAppend:
            flags |= O_RDWR | O_CREAT;
            append = true;
            break;
        case kWriteCreateNew:
            flags |= O_RDWR | O_CREAT | O_TRUNC;
            break;
        default:
            flags |= O_RDONLY;
            break;
    }

    h->path = hostPath(filepath);
    int fd = ::open(h->path.c_str(), flags, 0644);
    struct stat st;
    if (fd >= 0 && (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))) // FatFs can't f_open a dir
    {
        ::close(fd);
        fd = -1;
    }
    if (fd >= 0 && append)
    {
        ::lseek(fd, 0, SEEK_END);
    }
    h->fd = fd;
    h->writable = (openMode != kRead);

    const bool isOpen = fd >= 0;
    if (!isOpen && fileHandle == nullptr)
    {
        delete h;
        h = nullptr;
    }
    return std::make_tuple(isOpen, (FileHandle)h);
}

int32_t FileSystemPosix::readFile(void* destBuf, size_t bufLen, FileHandle file)
{
    auto* h = (Handle*)file;
    assert(h && h->fd >= 0);
    if (h == nullptr || h->fd < 0)
    {
        return -1;
    }
    for (;;)
    {
        ssize_t n = ::read(h->fd, destBuf, bufLen);
        if (n < 0 && errno == EINTR)
        {
            continue;
        }
        return (int32_t)n;
    }
}

int32_t FileSystemPosix::writeFile(const void* data, size_t dataSize, FileHandle fileHandle)
{
    auto* h = (Handle*)fileHandle;
    assert(h && h->fd >= 0);
    if (h == nullptr || h->fd < 0 || !h->writable)
    {
        return -1;
    }
    size_t done = 0;
    while (done < dataSize)
    {
        ssize_t n = ::write(h->fd, (const char*)data + done, dataSize - done);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return done ? (int32_t)done : -1;
        }
        done += (size_t)n;
    }
    return (int32_t)done;
}

bool FileSystemPosix::seekFile(uint32_t pos, FileHandle fileHandle)
{
    auto* h = (Handle*)fileHandle;
    assert(h && h->fd >= 0);
    if (h == nullptr || h->fd < 0)
    {
        return false;
    }
    // f_lseek on a read-only file clamps to the file size.
    if (!h->writable)
    {
        size_t size = getFileSize(fileHandle);
        if (pos > size)
        {
            pos = (uint32_t)size;
        }
    }
    return ::lseek(h->fd, (off_t)pos, SEEK_SET) >= 0;
}

bool FileSystemPosix::seekFileCur(int offset, FileHandle fileHandle)
{
    auto* h = (Handle*)fileHandle;
    assert(h && h->fd >= 0);
    if (h == nullptr || h->fd < 0)
    {
        return false;
    }
    if (offset == 0)
    {
        return true;
    }
    off_t cur = ::lseek(h->fd, 0, SEEK_CUR);
    if (cur < 0 || cur + offset < 0)
    {
        return false;
    }
    return seekFile((uint32_t)(cur + offset), fileHandle);
}

void FileSystemPosix::closeFile(FileHandle file, bool deleteOnClose)
{
    auto* h = (Handle*)file;
    if (h == nullptr)
    {
        return;
    }
    if (h->fd >= 0)
    {
        ::close(h->fd);
        h->fd = -1;
    }
    if (deleteOnClose)
    {
        delete h;
    }
}

// SdFile::end() is f_truncate: cut the file at the current position.
void FileSystemPosix::endFile(FileHandle file)
{
    auto* h = (Handle*)file;
    assert(h && h->fd >= 0);
    if (h == nullptr || h->fd < 0)
    {
        return;
    }
    off_t cur = ::lseek(h->fd, 0, SEEK_CUR);
    if (cur >= 0)
    {
        (void)::ftruncate(h->fd, cur);
    }
}

bool FileSystemPosix::isFileOpen(FileHandle file)
{
    auto* h = (Handle*)file;
    return h != nullptr && h->fd >= 0;
}

// Registers the file as one contiguous fragment in the virtual sector space and writes the
// FatFs CLMT: [0] = used table size (elements), then {clusterCount, startCluster}, 0-terminated.
bool FileSystemPosix::enableFastSeek(FileHandle fileHandle,
                                     uint32_t* clmtBuffer,
                                     uint32_t clmtElementCount,
                                     uint32_t* outDatabaseSector,
                                     uint32_t* outSectorsPerCluster)
{
    auto* h = (Handle*)fileHandle;
    if (h == nullptr || h->fd < 0 || clmtBuffer == nullptr || clmtElementCount < 4)
    {
        return false;
    }
    struct stat st;
    if (::fstat(h->fd, &st) != 0)
    {
        return false;
    }
    const uint32_t base = sectorMap_.registerFile(h->fd, (uint64_t)st.st_size, h->path);
    if (base == 0)
    {
        return false;
    }
    clmtBuffer[0] = 4;
    clmtBuffer[1] = VirtualSectorMap::clustersForSize((uint64_t)st.st_size);
    clmtBuffer[2] = base;
    clmtBuffer[3] = 0;
    if (outDatabaseSector != nullptr)
    {
        *outDatabaseSector = VirtualSectorMap::kDatabaseSector;
    }
    if (outSectorsPerCluster != nullptr)
    {
        *outSectorsPerCluster = VirtualSectorMap::kSectorsPerCluster;
    }
    return true;
}

// Synchronous: the "async" read completes before returning. Bytes past EOF read as zero.
bool FileSystemPosix::readSectorsAsync(uint8_t* buffer, uint32_t startSector, uint32_t sectorCount)
{
    lastAsyncSuccess_ = buffer != nullptr && sectorMap_.read(startSector, sectorCount, buffer);
    return lastAsyncSuccess_;
}

bool FileSystemPosix::pollAsyncRead(bool* outSuccess)
{
    if (outSuccess != nullptr)
    {
        *outSuccess = lastAsyncSuccess_;
    }
    return true;
}

bool FileSystemPosix::waitAsyncRead()
{
    return true;
}

bool FileSystemPosix::isAsyncReadBusy()
{
    return false;
}

// --- directory listing -----------------------------------------------------------------------

bool FileSystemPosix::readDir(const char* cardPath, std::vector<DirEntry>& out) const
{
    const std::string host = hostPath(cardPath);
    DIR* d = ::opendir(host.c_str());
    if (d == nullptr)
    {
        return false;
    }
    while (dirent* de = ::readdir(d))
    {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
        {
            continue;
        }
        struct stat st;
        if (::stat((host + "/" + de->d_name).c_str(), &st) != 0)
        {
            continue;
        }
        DirEntry e;
        e.name = de->d_name;
        e.isDir = S_ISDIR(st.st_mode);
        e.hidden = isHiddenStat(e.name, st);
        e.size = (uint64_t)st.st_size;
        out.push_back(std::move(e));
    }
    ::closedir(d);
    // FatFs lists in directory (creation) order, which is not reproducible on a host
    // filesystem; use folders first, then the core's natural-order comparison.
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDir != b.isDir)
        {
            return a.isDir;
        }
        return compareStrings(a.name.c_str(), b.name.c_str());
    });
    return true;
}

// Port of SdDir::filterFileEntry (platforms/imx/sdmmc/sd/SD.cpp:304-428).
bool FileSystemPosix::filterEntry(const DirEntry& e, enListFileFilter filter, size_t dirPathLen) const
{
    if (e.hidden)
    {
        return false;
    }
    if (e.isDir && filter != enListFileFilter_folder && filter != enListFileFilter_all)
    {
        return false;
    }
    const size_t len = e.name.size();
    switch (filter)
    {
        case enListFileFilter_all:
            return true;
        case enListFileFilter_folder:
            return e.isDir && !isMacMetaFolder(e.name);
        case enListFileFilter_wave_inst:
        case enListFileFilter_wave:
        {
            if (len < 5)
            {
                return false;
            }
            if (!endsWithExt3(e.name, ".wav"))
            {
                if (filter != enListFileFilter_wave_inst || !endsWithExt3(e.name, ".pti"))
                {
                    return false;
                }
            }
            if (dirPathLen + len > 253)
            {
                return false;
            }
            return !isMacMetaFile(e.name);
        }
        case enListFileFilter_firmware:
        {
            constexpr char kStart[] = "TrackerPlus"; // TRACKER_MINI would use "TrackerMini"
            const size_t startLen = sizeof(kStart) - 1;
            if (len < startLen + 4)
            {
                return false;
            }
            if (e.name.compare(len - 4, 4, ".ptf") != 0)
            {
                return false;
            }
            if (e.name.compare(0, startLen, kStart) != 0)
            {
                return false;
            }
            return !isMacMetaFile(e.name);
        }
        case enListFileFilter_mod:
        {
            const std::string low = toLower(e.name);
            return low.find(".mod") != std::string::npos || low.find(".it") != std::string::npos;
        }
        case enListFileFilter_game:
        {
            if (isMacMetaFile(e.name))
            {
                return false;
            }
            return toLower(e.name).find(".nes") != std::string::npos;
        }
    }
    return false;
}

int FileSystemPosix::countInSubDir(const std::string& cardDir, enListFileFilter filter, size_t dirPathLen) const
{
    std::vector<DirEntry> entries;
    if (!readDir(cardDir.c_str(), entries))
    {
        return 0;
    }
    int count = 0;
    for (const auto& e : entries)
    {
        if (filterEntry(e, filter, dirPathLen))
        {
            ++count;
        }
    }
    return count;
}

// Port of SdDir::createFilesList (SD.cpp:438-554); entries are allocated with new char[].
int FileSystemPosix::createFilesList(const char* path,
                                     uint8_t start_line,
                                     char** list,
                                     uint8_t list_length,
                                     uint16_t max_used_memory,
                                     enListFileFilter filter,
                                     uint16_t skip_count,
                                     uint16_t max_files_in_subdir,
                                     enListFileFilter folderSizeFilter)
{
    std::vector<DirEntry> entries;
    if (!readDir(path, entries))
    {
        return kListError;
    }
    for (uint8_t i = 0; i < list_length; i++)
    {
        if (list[i] != nullptr)
        {
            delete[] list[i];
            list[i] = nullptr;
        }
    }
    uint8_t n = start_line;
    if (max_used_memory < 255)
    {
        return n;
    }
    const size_t dirPathLen = std::min(strlen(path), kMaxPath - 1);
    uint16_t memory_used = 0;

    for (const auto& e : entries)
    {
        if (n >= list_length || memory_used > max_used_memory - 255)
        {
            break;
        }
        if (!filterEntry(e, filter, dirPathLen))
        {
            continue;
        }
        if (skip_count > 0)
        {
            skip_count--;
            continue;
        }

        uint16_t len = (uint16_t)e.name.size() + (e.isDir ? 2 : 1);
        list[n] = new char[len];
        memory_used += len;

        if (e.isDir)
        {
            snprintf(list[n], len, "/%s", e.name.c_str());
            std::string sub = std::string(path) + "/" + e.name;
            int files_count = countInSubDir(sub, folderSizeFilter, dirPathLen);

            if (max_files_in_subdir != 0 && files_count > max_files_in_subdir)
            {
                uint16_t offset = 0;
                len += 4 + 2 * 5;
                while (files_count > 0 && n < list_length - 1)
                {
                    if (list[n] != nullptr)
                    {
                        delete[] list[n];
                        memory_used -= len;
                    }
                    list[n] = new char[len];
                    memory_used += len;
                    snprintf(list[n], len, "/%s (%d-%d)", e.name.c_str(), offset + 1,
                             offset + max_files_in_subdir);
                    files_count -= max_files_in_subdir;
                    n++;
                    offset += max_files_in_subdir;
                }
                n--;
            }
        }
        else
        {
            snprintf(list[n], len, "%s", e.name.c_str());
        }
        n++;
    }
    return n;
}

// Port of SdDir::createInstrumentsList (SD.cpp:556-655): "<1-48> <name>.pti" -> list[idx-1].
int FileSystemPosix::createInstrumentsList(const char* path,
                                           char** list,
                                           uint8_t list_length,
                                           uint32_t* sizeOut)
{
    std::vector<DirEntry> entries;
    if (!readDir(path, entries))
    {
        return kListError;
    }
    uint8_t n = 0;
    for (const auto& e : entries)
    {
        if (n >= list_length)
        {
            break;
        }
        if (e.hidden || e.isDir)
        {
            continue;
        }
        const size_t nameLen = e.name.size();
        if (nameLen < 5 || !endsWithExt3(e.name, ".pti"))
        {
            continue;
        }
        const char* p = e.name.c_str();
        int idx1, idx2 = -1;
        if (*p >= '0' && *p <= '9')
        {
            idx1 = *p - '0';
        }
        else
        {
            continue;
        }
        p++;
        if (*p >= '0' && *p <= '9')
        {
            idx2 = *p - '0';
            p++;
        }
        if (*p != ' ')
        {
            continue;
        }
        p++;
        if (idx2 >= 0)
        {
            idx1 = idx1 * 10 + idx2;
        }
        if (idx1 < 1 || idx1 > 48)
        {
            continue;
        }
        idx1--;
        if (list[idx1] != nullptr)
        {
            continue;
        }
        int len = (int)nameLen - (int)(p - e.name.c_str()) - 4;
        if (len < 0)
        {
            continue;
        }
        list[idx1] = new char[len + 1];
        memcpy(list[idx1], p, len);
        list[idx1][len] = 0;
        if (sizeOut != nullptr)
        {
            *sizeOut += (uint32_t)e.size;
        }
        n++;
    }
    return n;
}

// Port of SdDir::countContainedFiles (SD.cpp:657-705): non-hidden folders, and .wav files.
int FileSystemPosix::countContainedFiles(const char* path)
{
    std::vector<DirEntry> entries;
    if (!readDir(path, entries))
    {
        return 0;
    }
    const size_t dirPathLen = std::min(strlen(path), kMaxPath - 1);
    int n = 0;
    for (const auto& e : entries)
    {
        if (e.hidden)
        {
            continue;
        }
        if (!e.isDir)
        {
            if (e.name.size() < 5 || !endsWithExt3(e.name, ".wav") || dirPathLen + e.name.size() > 253)
            {
                continue;
            }
        }
        n++;
    }
    return n;
}

// Port of FileSystemImpl::listDirContents (FileSystemImpl.cpp:~480-590).
void FileSystemPosix::listDirContents(const char* dirPath,
                                      DirContentType contentType,
                                      std::function<bool(const char* name)> nameFilter,
                                      char* namesBuffer,
                                      int namesBufferSize,
                                      int maxNameLen,
                                      int& count,
                                      char** fileList)
{
    assert(count != 0);
    assert(fileList != nullptr);
    std::vector<DirEntry> entries;
    if (!readDir(dirPath, entries))
    {
        count = 0;
        return;
    }
    int listPos = 0;
    for (const auto& e : entries)
    {
        if (listPos >= count)
        {
            break;
        }
        if (e.hidden)
        {
            continue;
        }
        int nameLen;
        if (contentType == kFolders)
        {
            if (!e.isDir || isMacMetaFolder(e.name))
            {
                continue;
            }
            if (nameFilter != nullptr && !nameFilter(e.name.c_str()))
            {
                continue;
            }
            nameLen = (int)e.name.size() + 2;
        }
        else if (contentType == kFiles)
        {
            if (e.isDir || isMacMetaFile(e.name))
            {
                continue;
            }
            if (nameFilter != nullptr && !nameFilter(e.name.c_str()))
            {
                continue;
            }
            nameLen = (int)e.name.size() + 1;
        }
        else
        {
            continue;
        }
        if (nameLen > namesBufferSize)
        {
            break;
        }
        if (nameLen > maxNameLen)
        {
            continue;
        }
        fileList[listPos] = namesBuffer;
        char* dst = namesBuffer;
        if (e.isDir)
        {
            *dst++ = '/';
        }
        memcpy(dst, e.name.c_str(), e.name.size() + 1);
        namesBuffer += nameLen;
        namesBufferSize -= nameLen;
        listPos++;
    }
    count = listPos;
    std::sort(fileList, fileList + count, [](const char* a, const char* b) { return compareStrings(a, b); });
}

// Port of FileSystemImpl::readNbFiles (FileSystemImpl.cpp:~595-640): counts everything but
// "." and "..". (Game ROMs are out of scope for the Apple port, but this is cheap.)
int FileSystemPosix::readNbFiles(const char* romPath)
{
    std::vector<DirEntry> entries;
    if (!readDir(romPath, entries))
    {
        return 0;
    }
    return (int)entries.size();
}

bool FileSystemPosix::nesDir_open(const char* path)
{
    nesEntries_.clear();
    nesPos_ = 0;
    return readDir(path, nesEntries_);
}

bool FileSystemPosix::nesDir_readItem(char* path, uint8_t* isDir, uint8_t* isHid, uint32_t* size)
{
    if (nesPos_ >= nesEntries_.size())
    {
        return false;
    }
    const DirEntry& e = nesEntries_[nesPos_++];
    *isDir = e.isDir;
    *isHid = e.hidden;
    if (size != nullptr)
    {
        *size = (uint32_t)e.size;
    }
    strcpy(path, e.name.c_str());
    return true;
}

void FileSystemPosix::nesDir_close()
{
    nesEntries_.clear();
    nesPos_ = 0;
}
