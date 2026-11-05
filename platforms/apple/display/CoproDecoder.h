#pragma once
// Executes the FT812 coprocessor command stream against Ft812Model's memories, as EVE does:
// every command expands into display-list words at ram_dl[dlOffset]; CMD_SWAP publishes.
// Task 5a scope: list and memory commands (DLSTART, SWAP, APPEND, MEMCPY, MEMSET, MEMZERO,
// MEMWRITE) and raw display-list words. Everything else is logged once and skipped by the length
// FT812.cpp encodes it with.
#include <cstddef>
#include <cstdint>
#include <vector>

class Ft812Model;

class CoproDecoder
{
  public:
    explicit CoproDecoder(Ft812Model& model) : model_(model) {}

    // Appends bytes to the stream and executes every complete command. An incomplete trailing
    // command waits for more bytes (commands may straddle endCoProList / writeDataToCMD calls).
    void feed(const uint8_t* data, size_t len);

    // Bytes received but not yet executed (a partial command).
    size_t pendingBytes() const { return buf_.size() - pos_; }

  private:
    // Returns false if the command at pos_ is incomplete.
    bool step();
    uint32_t word(size_t at) const;
    void skipUnknown(uint32_t opcode, size_t len);
    void badRange(uint32_t opcode);
    void writeDl(uint32_t w);
    // Returns the byte length of a NUL-terminated, 4-padded string at `at`, or 0 if incomplete.
    size_t paddedStringLen(size_t at) const;
    // Image data after CMD_LOADIMAGE: consume until the JPEG EOI / PNG IEND chunk.
    bool swallowImage();

    Ft812Model& model_;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0;
    bool loggedOnce_[256] = {};
    bool loggedRange_[256] = {};
    bool loggedOverflow_ = false;
    bool inImage_ = false;
    int imageKind_ = 0; // 0 unknown yet, 1 JPEG, 2 PNG
    uint32_t imageTail_ = 0;
};
