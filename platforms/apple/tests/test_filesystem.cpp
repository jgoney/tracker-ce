#include "FileSystemPosix.h"
#include "VirtualSectorMap.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fcntl.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace
{
struct TempRoot
{
    fs::path path;
    TempRoot()
    {
        std::string tmpl = (fs::temp_directory_path() / "tracker_fs_XXXXXX").string();
        REQUIRE(mkdtemp(tmpl.data()) != nullptr);
        path = tmpl;
    }
    ~TempRoot()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void touch(const std::string& rel, const std::string& content = "x")
    {
        fs::create_directories((path / rel).parent_path());
        std::ofstream(path / rel, std::ios::binary) << content;
    }
};

uint8_t patternByte(size_t i) { return (uint8_t)((i * 31 + (i >> 8) * 7 + 5) & 0xff); }

std::vector<uint8_t> makePattern(size_t n)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i)
    {
        v[i] = patternByte(i);
    }
    return v;
}

void writeAll(FileSystemPosix& f, const char* path, const std::vector<uint8_t>& data)
{
    auto [ok, fh] = f.openFile(path, FileSystem::kWriteCreateNew);
    REQUIRE(ok);
    REQUIRE(f.writeFile(data.data(), data.size(), fh) == (int32_t)data.size());
    f.closeFile(fh);
}

// FatFs: lba = database + (cluster - 2) * csize + sector-in-cluster, via the CLMT.
uint32_t lbaForOffset(const uint32_t* clmt, uint32_t db, uint32_t csize, uint32_t byteOffset)
{
    uint32_t sectorIdx = byteOffset / 512;
    uint32_t cl = sectorIdx / csize;
    uint32_t csect = sectorIdx % csize;
    const uint32_t* tbl = clmt + 1;
    for (;;)
    {
        uint32_t ncl = *tbl++;
        REQUIRE(ncl != 0);
        uint32_t start = *tbl++;
        if (cl < ncl)
        {
            return db + (start + cl - 2) * csize + csect;
        }
        cl -= ncl;
    }
}
} // namespace

TEST_CASE("test_open_read_write_seek")
{
    TempRoot t;
    FileSystemPosix f(t.path.string());
    REQUIRE(f.isSdCardInserted());

    REQUIRE_FALSE(std::get<0>(f.openFile("/nope.bin")));

    auto [ok, fh] = f.openFile("/a.bin", FileSystem::kWriteCreateNew);
    REQUIRE(ok);
    REQUIRE(f.isFileOpen(fh));
    const char msg[] = "0123456789";
    REQUIRE(f.writeFile(msg, 10, fh) == 10);
    REQUIRE(f.getFileSize(fh) == 10);
    REQUIRE(f.seekFile(2, fh));
    char buf[16] = {};
    REQUIRE(f.readFile(buf, 3, fh) == 3);
    REQUIRE(std::string(buf, 3) == "234");
    REQUIRE(f.getFileAvailable(fh) == 5);
    REQUIRE(f.seekFileCur(-3, fh));
    REQUIRE(f.readFile(buf, 2, fh) == 2);
    REQUIRE(std::string(buf, 2) == "23");
    REQUIRE(f.seekFile(4, fh));
    f.endFile(fh); // truncate at position
    REQUIRE(f.getFileSize("/a.bin") == 4);
    f.closeFile(fh, false); // keep the handle object for reuse
    REQUIRE_FALSE(f.isFileOpen(fh));

    // reopen into the same handle in append mode
    FileSystem::FileHandle h2 = fh;
    auto [ok2, fh2] = f.openFile("/a.bin", FileSystem::kWriteAppend, &h2);
    REQUIRE(ok2);
    REQUIRE(fh2 == fh);
    REQUIRE(f.writeFile("ZZ", 2, fh2) == 2);
    f.closeFile(fh2); // frees
    REQUIRE(f.getFileSize("/a.bin") == 6);

    // read-only open: seek past EOF clamps, read at EOF returns 0, write fails
    auto [ok3, fh3] = f.openFile("/a.bin");
    REQUIRE(ok3);
    REQUIRE(f.seekFile(1000, fh3));
    REQUIRE(f.readFile(buf, 4, fh3) == 0);
    REQUIRE(f.writeFile("q", 1, fh3) == -1);
    f.closeFile(fh3);

    // kWriteOpen keeps existing content
    auto [ok4, fh4] = f.openFile("/a.bin", FileSystem::kWriteOpen);
    REQUIRE(ok4);
    REQUIRE(f.getFileSize(fh4) == 6);
    f.closeFile(fh4);

    // a directory cannot be opened as a file
    REQUIRE(f.mkdir(0, "/dir"));
    REQUIRE_FALSE(std::get<0>(f.openFile("/dir")));
}

TEST_CASE("test_mkdir_rename_remove_dirs")
{
    TempRoot t;
    FileSystemPosix f(t.path.string());

    REQUIRE(f.mkdir(0, "/Projects"));
    REQUIRE(f.mkdir(0, "/Projects")); // already exists: ok, like FatFs path in SdCard::mkdir
    REQUIRE(f.exists("/Projects"));
    REQUIRE(f.mkdir(0, "/Projects/p1/deep") == false); // single level, parent missing
    REQUIRE(f.mkdir(0, "/Projects/p1"));
    REQUIRE(f.mkdir(0, "/Projects/p1/deep"));
    t.touch("Projects/p1/file.txt");
    t.touch("Projects/p1/deep/f2.txt");

    REQUIRE_FALSE(f.rename("/Projects/p1", "/Projects")); // destination exists
    REQUIRE(f.rename("/Projects/p1", "/Projects/p2"));
    REQUIRE_FALSE(f.exists("/Projects/p1"));
    REQUIRE(f.exists("/Projects/p2/file.txt"));

    REQUIRE_FALSE(f.remove("/Projects/p2")); // not empty
    REQUIRE(f.removeOnlyDirFiles("/Projects/p2"));
    REQUIRE(f.exists("/Projects/p2"));
    REQUIRE_FALSE(f.exists("/Projects/p2/file.txt"));
    REQUIRE_FALSE(f.exists("/Projects/p2/deep"));

    t.touch("Projects/p2/x/y.txt");
    REQUIRE(f.removeDirWithFiles("/Projects/p2"));
    REQUIRE_FALSE(f.exists("/Projects/p2"));

    t.touch("Projects/lone.txt");
    REQUIRE(f.remove("/Projects/lone.txt"));
    REQUIRE_FALSE(f.remove("/Projects/lone.txt"));
    REQUIRE(f.remove("/Projects"));
}

namespace
{
struct List
{
    char* items[32] = {};
    ~List()
    {
        for (auto* p : items)
        {
            delete[] p;
        }
    }
};
} // namespace

TEST_CASE("test_create_files_list_wave_filter")
{
    TempRoot t;
    FileSystemPosix f(t.path.string());
    t.touch("a.wav");
    t.touch("B.WAV");
    t.touch("c.txt");
    t.touch(".hidden.wav");
    t.touch("._meta.wav");
    t.touch("e.PTI");
    t.touch("d/inner.wav");
    t.touch("__MACOSX/x.wav");

    List l;
    int n = f.createFilesList("/", 0, l.items, 32, 4096, FileSystem::enListFileFilter_wave_inst);
    // wave_inst accepts .pti too; folders are excluded by the wave filters
    REQUIRE(n == 3);
    CHECK(std::string(l.items[0]) == "a.wav");
    CHECK(std::string(l.items[1]) == "B.WAV");
    CHECK(std::string(l.items[2]) == "e.PTI");
}

TEST_CASE("test_create_files_list_core_usage_order")
{
    TempRoot t;
    FileSystemPosix f(t.path.string());
    t.touch("a.wav");
    t.touch("B.WAV");
    t.touch("c.txt");
    t.touch(".hidden.wav");
    t.touch("d/inner.wav");

    // The core lists folders (folder filter) and then lists waves into the rest of the
    // array, as in fileManagerBrowse.cpp:206/251. Expect [d, a.wav, B.WAV]; folders carry a '/' prefix.
    List l;
    int folders = f.createFilesList("/", 0, l.items, 32, 4096, FileSystem::enListFileFilter_folder);
    REQUIRE(folders == 1);
    int n = f.createFilesList("/", 0, l.items + folders, 32 - folders, 4096,
                              FileSystem::enListFileFilter_wave);
    REQUIRE(n == 2);
    CHECK(std::string(l.items[0]) == "/d");
    CHECK(std::string(l.items[1]) == "a.wav");
    CHECK(std::string(l.items[2]) == "B.WAV");

    // all: folders first then files in natural order; hidden skipped
    List la;
    n = f.createFilesList("/", 0, la.items, 32, 4096, FileSystem::enListFileFilter_all);
    REQUIRE(n == 4);
    CHECK(std::string(la.items[0]) == "/d");
    CHECK(std::string(la.items[3]) == "c.txt");

    List lw;
    n = f.createFilesList("/", 0, lw.items, 32, 4096, FileSystem::enListFileFilter_wave);
    REQUIRE(n == 2);
    CHECK(std::string(lw.items[0]) == "a.wav");
    CHECK(std::string(lw.items[1]) == "B.WAV");

    List lf;
    n = f.createFilesList("/", 0, lf.items, 32, 4096, FileSystem::enListFileFilter_folder);
    REQUIRE(n == 1);
    CHECK(std::string(lf.items[0]) == "/d");

    List lm;
    REQUIRE(f.createFilesList("/missing", 0, lm.items, 32, 4096) == FileSystem::kListError);
    REQUIRE(f.countContainedFiles("/") == 3); // d, a.wav, B.WAV (not c.txt, not hidden)
}

TEST_CASE("test_list_dir_contents_and_instruments")
{
    TempRoot t;
    FileSystemPosix f(t.path.string());
    t.touch("Projects/zeta/x");
    t.touch("Projects/Alpha/x");
    t.touch("Projects/.hid/x");
    t.touch("Projects/file.mt");
    t.touch("inst/1 kick.pti", "12345");
    t.touch("inst/12 snare.pti", "123");
    t.touch("inst/99 bad.pti");
    t.touch("inst/nonum.pti");

    char names[256];
    char* list[8];
    int count = 8;
    f.listDirContents("/Projects", FileSystem::kFolders, nullptr, names, sizeof(names), 32, count, list);
    REQUIRE(count == 2);
    CHECK(std::string(list[0]) == "/Alpha");
    CHECK(std::string(list[1]) == "/zeta");

    count = 8;
    f.listDirContents("/Projects", FileSystem::kFiles, nullptr, names, sizeof(names), 32, count, list);
    REQUIRE(count == 1);
    CHECK(std::string(list[0]) == "file.mt");

    char* inst[48] = {};
    uint32_t sz = 0;
    REQUIRE(f.createInstrumentsList("/inst", inst, 48, &sz) == 2);
    CHECK(std::string(inst[0]) == "kick");
    CHECK(std::string(inst[11]) == "snare");
    CHECK(sz == 8);
    for (auto* p : inst)
    {
        delete[] p;
    }
}

TEST_CASE("test_virtual_sector_roundtrip")
{
    TempRoot t;
    FileSystemPosix f(t.path.string());
    const auto data = makePattern(1024 * 1024);
    writeAll(f, "/big.bin", data);

    auto [ok, fh] = f.openFile("/big.bin");
    REQUIRE(ok);
    uint32_t clmt[64] = { 64 };
    uint32_t db = 0, csize = 0;
    REQUIRE(f.enableFastSeek(fh, clmt, 64, &db, &csize));
    CHECK(db == VirtualSectorMap::kDatabaseSector);
    CHECK(csize == VirtualSectorMap::kSectorsPerCluster);
    CHECK(clmt[0] == 4);
    CHECK(clmt[1] == 32); // 1 MB / 32 KB clusters
    CHECK(clmt[2] >= 2);
    CHECK(clmt[3] == 0);

    const uint32_t off = 300000;
    uint32_t lba = lbaForOffset(clmt, db, csize, off);
    uint8_t buf[4 * 512];
    REQUIRE(f.readSectorsAsync(buf, lba, 4));
    bool success = false;
    REQUIRE(f.pollAsyncRead(&success));
    REQUIRE(success);
    REQUIRE(f.waitAsyncRead());
    REQUIRE_FALSE(f.isAsyncReadBusy());
    const size_t base = (off / 512) * 512;
    REQUIRE(std::memcmp(buf, data.data() + base, sizeof(buf)) == 0);

    // the fd may be closed afterwards; the map owns its own descriptor
    f.closeFile(fh);
    REQUIRE(f.readSectorsAsync(buf, lba, 4));
    REQUIRE(std::memcmp(buf, data.data() + base, sizeof(buf)) == 0);

    // a read that crosses the last data cluster boundary into the guard cluster yields zeros
    uint32_t lastLba = lbaForOffset(clmt, db, csize, 1024 * 1024 - 512);
    REQUIRE(f.readSectorsAsync(buf, lastLba, 2));
    CHECK(std::memcmp(buf, data.data() + 1024 * 1024 - 512, 512) == 0);
    for (int i = 512; i < 1024; ++i)
    {
        REQUIRE(buf[i] == 0);
    }

    // unmapped LBAs fail
    REQUIRE_FALSE(f.readSectorsAsync(buf, 10, 1));
    REQUIRE_FALSE(f.readSectorsAsync(buf, db + 100000, 1));
}

TEST_CASE("test_two_files_disjoint_sectors")
{
    TempRoot t;
    FileSystemPosix f(t.path.string());
    auto a = makePattern(100000);
    std::vector<uint8_t> b(70000);
    for (size_t i = 0; i < b.size(); ++i)
    {
        b[i] = (uint8_t)(255 - patternByte(i));
    }
    writeAll(f, "/a.bin", a);
    writeAll(f, "/b.bin", b);

    auto [oka, fa] = f.openFile("/a.bin");
    auto [okb, fb] = f.openFile("/b.bin");
    REQUIRE((oka && okb));
    uint32_t ca[64] = { 64 }, cb[64] = { 64 };
    uint32_t db = 0, cs = 0;
    REQUIRE(f.enableFastSeek(fa, ca, 64, &db, &cs));
    REQUIRE(f.enableFastSeek(fb, cb, 64, &db, &cs));
    CHECK(ca[2] == 2); // first file starts at cluster 2
    // ceil(100000 / 32768) = 4 clusters + 1 guard
    CHECK(cb[2] == ca[2] + 4 + 1);
    CHECK(ca[2] + ca[1] <= cb[2]);

    uint8_t buf[512];
    for (uint32_t off : { 0u, 512u, 40000u, 99840u })
    {
        REQUIRE(f.readSectorsAsync(buf, lbaForOffset(ca, db, cs, off), 1));
        REQUIRE(std::memcmp(buf, a.data() + off / 512 * 512, std::min<size_t>(512, a.size() - off / 512 * 512)) == 0);
    }
    for (uint32_t off : { 0u, 33000u, 69632u })
    {
        REQUIRE(f.readSectorsAsync(buf, lbaForOffset(cb, db, cs, off), 1));
        REQUIRE(std::memcmp(buf, b.data() + off / 512 * 512, std::min<size_t>(512, b.size() - off / 512 * 512)) == 0);
    }
    // a span from file A into B's range is rejected
    uint32_t lastA = lbaForOffset(ca, db, cs, 99840);
    REQUIRE_FALSE(f.readSectorsAsync(buf, lastA, 64 * 6));
    f.closeFile(fa);
    f.closeFile(fb);
}

TEST_CASE("test_fast_seek_idempotent_on_reopen")
{
    TempRoot t;
    FileSystemPosix f(t.path.string());
    writeAll(f, "/s.bin", makePattern(200000));
    writeAll(f, "/other.bin", makePattern(1000));

    uint32_t c1[64] = { 64 }, c2[64] = { 64 }, c3[64] = { 64 };
    auto [ok, fh] = f.openFile("/s.bin");
    REQUIRE(ok);
    REQUIRE(f.enableFastSeek(fh, c1, 64));
    f.closeFile(fh);

    auto [ok2, fh2] = f.openFile("/s.bin");
    REQUIRE(ok2);
    REQUIRE(f.enableFastSeek(fh2, c2, 64));
    REQUIRE(c1[2] == c2[2]);
    REQUIRE(c1[1] == c2[1]);

    auto [ok3, fo] = f.openFile("/other.bin");
    REQUIRE(ok3);
    REQUIRE(f.enableFastSeek(fo, c3, 64));
    REQUIRE(c3[2] != c1[2]);

    // reading through the reopened file still works
    std::vector<uint8_t> exp = makePattern(200000);
    uint8_t buf[512];
    REQUIRE(f.readSectorsAsync(buf, VirtualSectorMap::kDatabaseSector + (c2[2] - 2) * 64 + 3, 1));
    REQUIRE(std::memcmp(buf, exp.data() + 3 * 512, 512) == 0);

    REQUIRE_FALSE(f.enableFastSeek(fh2, c2, 3)); // table too small
    REQUIRE_FALSE(f.enableFastSeek(nullptr, c2, 64));
    f.closeFile(fh2);
    f.closeFile(fo);
}

TEST_CASE("test_fd_cache_bounded")
{
    TempRoot t;
    FileSystemPosix f(t.path.string());
    constexpr int kFiles = 100;
    uint32_t clmt[kFiles][64];
    for (int i = 0; i < kFiles; ++i)
    {
        std::vector<uint8_t> d(3000, (uint8_t)i);
        writeAll(f, ("/f" + std::to_string(i) + ".bin").c_str(), d);
    }
    for (int i = 0; i < kFiles; ++i)
    {
        auto [ok, fh] = f.openFile(("/f" + std::to_string(i) + ".bin").c_str());
        REQUIRE(ok);
        clmt[i][0] = 64;
        REQUIRE(f.enableFastSeek(fh, clmt[i], 64));
        f.closeFile(fh);
        REQUIRE(f.sectorMap().cachedFdCount() <= VirtualSectorMap::kMaxCachedFds);
    }
    uint8_t buf[512];
    for (int i = 0; i < kFiles; ++i)
    {
        uint32_t lba = lbaForOffset(clmt[i], 2048, 64, 1024);
        REQUIRE(f.readSectorsAsync(buf, lba, 1));
        REQUIRE(buf[0] == (uint8_t)i);
        REQUIRE(buf[511] == (uint8_t)i);
        REQUIRE(f.sectorMap().cachedFdCount() <= VirtualSectorMap::kMaxCachedFds);
    }
    // real process fd usage: none beyond the cache (stdio + cache + slack)
    int open = 0;
    for (int fd = 3; fd < 512; ++fd)
    {
        if (fcntl(fd, F_GETFD) != -1)
        {
            ++open;
        }
    }
    CHECK(open <= (int)VirtualSectorMap::kMaxCachedFds + 8);
}

TEST_CASE("test_old_range_survives_growth")
{
    TempRoot t;
    FileSystemPosix f(t.path.string());
    auto small = makePattern(50000);
    writeAll(f, "/g.bin", small);
    uint32_t c1[64] = { 64 }, c2[64] = { 64 };
    {
        auto [ok, fh] = f.openFile("/g.bin");
        REQUIRE(ok);
        REQUIRE(f.enableFastSeek(fh, c1, 64));
        f.closeFile(fh);
    }
    auto big = makePattern(400000);
    writeAll(f, "/g.bin", big); // same inode (O_TRUNC), grew
    {
        auto [ok, fh] = f.openFile("/g.bin");
        REQUIRE(ok);
        REQUIRE(f.enableFastSeek(fh, c2, 64));
        f.closeFile(fh);
    }
    REQUIRE(c2[2] != c1[2]);
    uint8_t buf[512];
    REQUIRE(f.readSectorsAsync(buf, lbaForOffset(c1, 2048, 64, 20000), 1));
    REQUIRE(std::memcmp(buf, big.data() + 20000 / 512 * 512, 512) == 0);
    REQUIRE(f.readSectorsAsync(buf, lbaForOffset(c2, 2048, 64, 300000), 1));
    REQUIRE(std::memcmp(buf, big.data() + 300000 / 512 * 512, 512) == 0);
}

TEST_CASE("test_replaced_file_read_fails_cleanly")
{
    TempRoot t;
    FileSystemPosix f(t.path.string());
    writeAll(f, "/r.bin", makePattern(10000));
    uint32_t c[64] = { 64 };
    {
        auto [ok, fh] = f.openFile("/r.bin");
        REQUIRE(ok);
        REQUIRE(f.enableFastSeek(fh, c, 64));
        f.closeFile(fh);
    }
    uint8_t buf[512];
    uint32_t lba = lbaForOffset(c, 2048, 64, 512);
    REQUIRE(f.readSectorsAsync(buf, lba, 1));
    // evict it from the fd cache
    for (int i = 0; i < 40; ++i)
    {
        std::string p = "/e" + std::to_string(i) + ".bin";
        writeAll(f, p.c_str(), makePattern(600));
        uint32_t ce[64] = { 64 };
        auto [ok, fh] = f.openFile(p.c_str());
        REQUIRE(ok);
        REQUIRE(f.enableFastSeek(fh, ce, 64));
        f.closeFile(fh);
    }
    // replace via rename with different content
    writeAll(f, "/new.bin", std::vector<uint8_t>(10000, 0xEE));
    fs::remove(t.path / "r.bin");
    REQUIRE(f.rename("/new.bin", "/r.bin"));
    REQUIRE_FALSE(f.readSectorsAsync(buf, lba, 1));
    bool success = true;
    REQUIRE(f.pollAsyncRead(&success));
    REQUIRE_FALSE(success);
}
