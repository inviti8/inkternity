#pragma once
#include <iostream>
#include <sstream>
#include <memory>
#include <vector>
#include <cereal/archives/portable_binary.hpp>

// Read-only streambuf over a caller-owned byte array.
//
// Originally this just called setg(p, p, p + l) and relied on the base
// class's get-area (eback/gptr/egptr) accounting. That breaks for buffers
// larger than 2 GiB on MSVC: setg/the get area truncate the range to a
// 32-bit int, so egptr()-gptr() goes NEGATIVE and sgetn() returns 0 on the
// very first read. A canvas whose *decompressed* size exceeds 2 GiB then
// fails to load with "Failed to read 1 bytes from input stream! Read 0".
//
// To support multi-GiB canvases we keep our own 64-bit cursor and override
// the get operations, leaving the base get-area pointers null so every read
// routes through these overrides (sgetn -> xsgetn; sgetc/sbumpc ->
// underflow/uflow; in_avail -> showmanyc).
class ByteMemBuf : public std::basic_streambuf<char> {
    public:
        ByteMemBuf(char* p, size_t l);
        // Bytes consumed so far (cursor advance from the start).
        std::ptrdiff_t bytes_consumed() const { return cur - beg; }
    protected:
        std::streamsize xsgetn(char* s, std::streamsize n) override;
        int_type underflow() override;
        int_type uflow() override;
        std::streamsize showmanyc() override;
        pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                         std::ios_base::openmode which) override;
        pos_type seekpos(pos_type pos, std::ios_base::openmode which) override;
    private:
        char* beg;
        char* cur;
        char* end;
};
class ByteMemStream : public std::istream {
    public:
        ByteMemStream(char* p, size_t l);
        std::ptrdiff_t bytes_consumed() const { return buffer.bytes_consumed(); }
    private:
        ByteMemBuf buffer;
};

struct PartialFragmentMessage {
    std::string partialFragmentMessage;
    uint64_t partialFragmentMessageLoc = 0;
};

std::vector<std::shared_ptr<std::stringstream>> fragment_message(std::string_view uncompressedView, size_t stride);
void decode_fragmented_message(cereal::PortableBinaryInputArchive& inArchive, PartialFragmentMessage& spfm, size_t fragmentMessageStride, std::function<void(cereal::PortableBinaryInputArchive& completeArchive)> runAfterDecoded);
