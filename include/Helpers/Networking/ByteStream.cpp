#include "ByteStream.hpp"
#include <cereal/archives/portable_binary.hpp>
#include <cereal/types/string.hpp>
#include "NetLibrary.hpp"
#include <zstd.h>
#include <cstring>

// NOTE: deliberately does NOT call setg(). We keep our own 64-bit
// [beg, cur, end) cursor and override the get virtuals so multi-GiB
// buffers work (the base get-area is 32-bit-limited on MSVC). With the
// base get area left null, sgetc/sbumpc/in_avail all fall through to
// our underflow/uflow/showmanyc overrides.
ByteMemBuf::ByteMemBuf(char* p, size_t l):
    beg(p), cur(p), end(p + l)
{
}

std::streamsize ByteMemBuf::xsgetn(char* s, std::streamsize n) {
    if(n <= 0) return 0;
    const std::streamsize avail = end - cur;
    const std::streamsize cnt = n < avail ? n : avail;
    if(cnt <= 0) return 0;
    std::memcpy(s, cur, static_cast<size_t>(cnt));
    cur += cnt;
    return cnt;
}

ByteMemBuf::int_type ByteMemBuf::underflow() {
    if(cur >= end) return traits_type::eof();
    return traits_type::to_int_type(*cur);
}

ByteMemBuf::int_type ByteMemBuf::uflow() {
    if(cur >= end) return traits_type::eof();
    return traits_type::to_int_type(*cur++);
}

std::streamsize ByteMemBuf::showmanyc() {
    return end - cur;
}

ByteMemBuf::pos_type ByteMemBuf::seekoff(off_type off, std::ios_base::seekdir dir,
                                         std::ios_base::openmode /*which*/) {
    char* base = (dir == std::ios_base::beg) ? beg
               : (dir == std::ios_base::cur) ? cur
                                             : end;
    char* np = base + off;
    if(np < beg || np > end) return pos_type(off_type(-1));
    cur = np;
    return pos_type(cur - beg);
}

ByteMemBuf::pos_type ByteMemBuf::seekpos(pos_type pos, std::ios_base::openmode which) {
    return seekoff(off_type(pos), std::ios_base::beg, which);
}

ByteMemStream::ByteMemStream(char* p, size_t l):
    std::istream(&buffer),
    buffer(p, l)
{
    rdbuf(&buffer);
}

std::vector<std::shared_ptr<std::stringstream>> fragment_message(std::string_view uncompressedView, size_t stride) {
    if(uncompressedView.length() <= stride)
        return {};

    std::vector<char> compressedData(ZSTD_compressBound(uncompressedView.size()));
    size_t trueCompressedSize = ZSTD_compress(compressedData.data(), compressedData.size(), uncompressedView.data(), uncompressedView.size(), ZSTD_CLEVEL_DEFAULT);

    std::string_view v(compressedData.data(), trueCompressedSize);

    uint64_t nextByte = 0;
    std::vector<std::shared_ptr<std::stringstream>> toRet;

    toRet.emplace_back(std::make_shared<std::stringstream>());

    size_t messFirstStride = std::min<size_t>(stride, v.size());

    {
        cereal::PortableBinaryOutputArchive m(*toRet.back());
        m((MessageCommandType)0, (uint64_t)v.length(), cereal::binary_data(v.data() + nextByte, messFirstStride));
    }

    if(messFirstStride == v.size())
        return toRet;

    nextByte += stride;
    for(;;) {
        if((v.length() - nextByte) <= stride) {
            toRet.emplace_back(std::make_shared<std::stringstream>());
            {
                cereal::PortableBinaryOutputArchive m(*toRet.back());
                m((MessageCommandType)0, cereal::binary_data(v.data() + nextByte, v.length() - nextByte));
            }
            break;
        }
        toRet.emplace_back(std::make_shared<std::stringstream>());
        {
            cereal::PortableBinaryOutputArchive m(*toRet.back());
            m((MessageCommandType)0, cereal::binary_data(v.data() + nextByte, stride));
        }
        nextByte += stride;
    }
    return toRet;
}

void uncompress_and_run_func(PartialFragmentMessage& spfm, std::function<void(cereal::PortableBinaryInputArchive& completeArchive)> runAfterDecoded) {
    std::vector<char> uncompressedData(ZSTD_getFrameContentSize(spfm.partialFragmentMessage.data(), spfm.partialFragmentMessage.size()));
    size_t trueUncompressedSize = ZSTD_decompress(uncompressedData.data(), uncompressedData.size(), spfm.partialFragmentMessage.data(), spfm.partialFragmentMessage.size());

    ByteMemStream completeStrm(uncompressedData.data(), trueUncompressedSize);
    cereal::PortableBinaryInputArchive completeArchive(completeStrm);
    runAfterDecoded(completeArchive);
    spfm.partialFragmentMessage.clear();
    spfm.partialFragmentMessageLoc = 0;
}

void decode_fragmented_message(cereal::PortableBinaryInputArchive& inArchive, PartialFragmentMessage& spfm, size_t fragmentMessageStride, std::function<void(cereal::PortableBinaryInputArchive& completeArchive)> runAfterDecoded) {
    if(spfm.partialFragmentMessage.size() == 0) {
        spfm.partialFragmentMessageLoc = 0;
        uint64_t messageSize;
        inArchive(messageSize);
        spfm.partialFragmentMessage.resize(messageSize);

        size_t messStride = std::min<size_t>(fragmentMessageStride, spfm.partialFragmentMessage.size());

        inArchive(cereal::binary_data(spfm.partialFragmentMessage.data() + spfm.partialFragmentMessageLoc, messStride));
        spfm.partialFragmentMessageLoc += messStride;

        if(messStride == spfm.partialFragmentMessage.size())
            uncompress_and_run_func(spfm, runAfterDecoded);
    }
    else if(spfm.partialFragmentMessage.size() - spfm.partialFragmentMessageLoc <= fragmentMessageStride) {
        inArchive(cereal::binary_data(spfm.partialFragmentMessage.data() + spfm.partialFragmentMessageLoc, spfm.partialFragmentMessage.size() - spfm.partialFragmentMessageLoc));
        uncompress_and_run_func(spfm, runAfterDecoded);
    }
    else {
        inArchive(cereal::binary_data(spfm.partialFragmentMessage.data() + spfm.partialFragmentMessageLoc, fragmentMessageStride));
        spfm.partialFragmentMessageLoc += fragmentMessageStride;
    }
}
