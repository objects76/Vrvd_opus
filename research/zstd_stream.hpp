// zstd_stream.hpp
// Header-only C++17 streaming wrapper for zstd, designed for remote-desktop
// style pipelines: per-frame flush, cross-frame history, session reset
// (reconnect / recording boundary), fixed-size output chunking.
//
// Requires: libzstd >= 1.4.0 (advanced API), link with -lzstd
#pragma once

#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace zs {

// ---------------------------------------------------------------- utilities
inline void check(size_t code, const char* where) {
    if (ZSTD_isError(code))
        throw std::runtime_error(std::string(where) + ": " + ZSTD_getErrorName(code));
}

// Sink receives compressed/decompressed output.
// Called zero or more times per call; `data` is only valid during the call.
using Sink = std::function<void(const void* data, size_t size)>;

enum class Flush {
    None,   // ZSTD_e_continue : buffer internally, emit only full blocks
    Frame,  // ZSTD_e_flush    : block boundary, decoder can restore everything
            //                   so far. History (window) is preserved.
    End     // ZSTD_e_end      : end zstd frame. Next compress() starts a new
            //                   frame but window history is still preserved.
};

// ---------------------------------------------------------------- options
struct CompressorOptions {
    int    level                = 3;    // 1..3 for realtime, up to 22
    int    windowLog            = 24;   // 16MB history; 0 = zstd default
    bool   longDistanceMatching = true; // required for large windows at low levels
    int    nbWorkers            = 0;    // 0 = single thread (history is sequential anyway)
    size_t outChunkSize         = 16 * 1024; // sink granularity (output buffer)
    // Optional: pledged source size for one-shot-equivalent header optimization.
    // Leave 0 for open-ended streams (remote desktop).
    unsigned long long pledgedSrcSize = 0;
};

struct DecompressorOptions {
    int    windowLogMax = 24;        // must be >= encoder windowLog
    size_t outChunkSize = 128 * 1024;
};

// ================================================================ Compressor
class StreamCompressor {
public:
    explicit StreamCompressor(CompressorOptions opt = {})
        : opt_(opt), cctx_(ZSTD_createCCtx()), out_(opt.outChunkSize) {
        if (!cctx_) throw std::runtime_error("ZSTD_createCCtx failed");
        applyParams();
    }

    ~StreamCompressor() { ZSTD_freeCCtx(cctx_); }

    StreamCompressor(const StreamCompressor&)            = delete;
    StreamCompressor& operator=(const StreamCompressor&) = delete;
    StreamCompressor(StreamCompressor&& o) noexcept
        : opt_(o.opt_), cctx_(o.cctx_), out_(std::move(o.out_)) { o.cctx_ = nullptr; }

    // Compress `size` bytes. Output is delivered to `sink` in chunks of at
    // most opt.outChunkSize. With Flush::Frame, everything consumed so far is
    // guaranteed decodable by the peer after this call returns.
    void compress(const void* data, size_t size, const Sink& sink,
                  Flush flush = Flush::None) {
        ZSTD_inBuffer in{data, size, 0};
        const ZSTD_EndDirective mode = toDirective(flush);
        size_t remaining;
        do {
            ZSTD_outBuffer out{out_.data(), out_.size(), 0};
            remaining = ZSTD_compressStream2(cctx_, &out, &in, mode);
            check(remaining, "ZSTD_compressStream2");
            if (out.pos) sink(out.dst, out.pos);
        } while (mode == ZSTD_e_continue ? (in.pos < in.size)
                                         : (remaining != 0 || in.pos < in.size));
    }

    // Convenience: per-frame call for the remote-desktop path.
    void compressFrame(const void* data, size_t size, const Sink& sink) {
        compress(data, size, sink, Flush::Frame);
    }

    // ------------------------------------------------------------ write/flush
    //   write(data, size, sink);  // compress incrementally (ZSTD_e_continue)
    //   ...
    //   flush(sink);              // frame boundary: peer can decode everything so far
    void write(const void* data, int size, const Sink& sink) {
        if (size < 0) throw std::runtime_error("StreamCompressor: negative size");
        compress(data, static_cast<size_t>(size), sink, Flush::None);
    }

    void flush(const Sink& sink) {
        compress(nullptr, 0, sink, Flush::Frame);
    }

    // Drop all history and start a fresh session.
    // Use at: client reconnect, recording start boundary, periodic seek points.
    // Caller must follow up with a key/full frame at the protocol level.
    void reset() {
        check(ZSTD_CCtx_reset(cctx_, ZSTD_reset_session_only), "CCtx_reset");
        applyParams(); // session reset keeps params, but re-pledge src size
    }

    // Change level on the fly (adaptive pipelines). Takes effect at the next
    // zstd block; history is preserved.
    void setLevel(int level) {
        opt_.level = level;
        check(ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, level),
              "set level");
    }

    int  level()  const { return opt_.level; }
    const CompressorOptions& options() const { return opt_; }

private:
    static ZSTD_EndDirective toDirective(Flush f) {
        switch (f) {
            case Flush::Frame: return ZSTD_e_flush;
            case Flush::End:   return ZSTD_e_end;
            default:           return ZSTD_e_continue;
        }
    }

    void applyParams() {
        check(ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, opt_.level),
              "set level");
        if (opt_.windowLog > 0)
            check(ZSTD_CCtx_setParameter(cctx_, ZSTD_c_windowLog, opt_.windowLog),
                  "set windowLog");
        check(ZSTD_CCtx_setParameter(cctx_, ZSTD_c_enableLongDistanceMatching,
                                     opt_.longDistanceMatching ? 1 : 0),
              "set LDM");
        if (opt_.nbWorkers > 0)
            check(ZSTD_CCtx_setParameter(cctx_, ZSTD_c_nbWorkers, opt_.nbWorkers),
                  "set nbWorkers");
        if (opt_.pledgedSrcSize > 0)
            check(ZSTD_CCtx_setPledgedSrcSize(cctx_, opt_.pledgedSrcSize),
                  "pledge src size");
    }

    CompressorOptions opt_;
    ZSTD_CCtx*        cctx_;
    std::vector<char> out_;
};

// ============================================================== Decompressor
class StreamDecompressor {
public:
    explicit StreamDecompressor(DecompressorOptions opt = {})
        : opt_(opt), dctx_(ZSTD_createDCtx()), out_(opt.outChunkSize) {
        if (!dctx_) throw std::runtime_error("ZSTD_createDCtx failed");
        applyParams();
    }

    ~StreamDecompressor() { ZSTD_freeDCtx(dctx_); }

    StreamDecompressor(const StreamDecompressor&)            = delete;
    StreamDecompressor& operator=(const StreamDecompressor&) = delete;
    StreamDecompressor(StreamDecompressor&& o) noexcept
        : opt_(o.opt_), dctx_(o.dctx_), out_(std::move(o.out_)) { o.dctx_ = nullptr; }

    // Feed any amount of compressed bytes (partial packets are fine).
    // Decoded output is delivered to `sink` as it becomes available.
    void decompress(const void* data, size_t size, const Sink& sink) {
        ZSTD_inBuffer in{data, size, 0};
        bool outFull;
        do {
            ZSTD_outBuffer out{out_.data(), out_.size(), 0};
            check(ZSTD_decompressStream(dctx_, &out, &in),
                  "ZSTD_decompressStream");
            if (out.pos) sink(out.dst, out.pos);
            // If the output buffer filled up, the decoder may still hold
            // buffered data even though the input is fully consumed.
            outFull = (out.pos == out.size);
        } while (in.pos < in.size || outFull);
    }

    // Must mirror encoder reset (reconnect / recording boundary).
    void reset() {
        check(ZSTD_DCtx_reset(dctx_, ZSTD_reset_session_only), "DCtx_reset");
        applyParams();
    }

private:
    void applyParams() {
        check(ZSTD_DCtx_setParameter(dctx_, ZSTD_d_windowLogMax, opt_.windowLogMax),
              "set windowLogMax");
    }

    DecompressorOptions opt_;
    ZSTD_DCtx*          dctx_;
    std::vector<char>   out_;
};

} // namespace zs
