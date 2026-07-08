/* dirty_verify.h - refine DXGI move/dirty metadata down to the pixels that
 * actually changed. Shared by scapenc (production) and the win_dirty.cpp
 * prototype at the repo root (whose self-test covers this exact code).
 *
 * Reconstruction contract (DXGI):
 *   cur == apply_dirty(apply_moves(prev, moves), dirty)
 *
 * Pipeline (from detect_dirty.md / win_dirty.cpp):
 *   1. apply move rects to prev in array order (overlap-safe via memmove)
 *   2. compare the dirty areas of prev(moved) vs cur in blocks
 *      -> over-reported dirty dies here
 *   3. drop moves whose destination ends up fully re-sent as pixels anyway
 *   4. merge changed cells into rects; copy only those into prev
 *      -> after the call prev == cur (prev mirrors the client framebuffer)
 *
 * Block-size adaptation ported from legacy check_changed_bits
 * (RC5XWIN/SCapEnc/desk/DesktopThread.cpp): per candidate rect,
 *   width >= 32 -> 64px blocks, 16..31 -> 16px, < 16 -> 8px.
 * Wide rects scan with little bookkeeping, narrow ones stay tight.
 * Change tracking lives in a global 8px cell grid so blocks of different
 * sizes still merge/dedup consistently (the legacy row-precision start for
 * small rects is approximated by the 8px cell granularity).
 *
 * Compare loops use plain CRT memcmp on purpose. Measured on a 64MB
 * DRAM-bound working set (2026-07): CRT memcmp ~9.3GB/s at 256B rows vs
 * ~10.6GB/s for hand-written AVX2 - the loop is memory-bandwidth-bound, so
 * custom SIMD buys <15%. /arch:AVX2 does NOT speed memcmp up (the CRT is a
 * prebuilt lib) and would break the encoder's deliberate runtime AVX2
 * dispatch (scap_332dither.h) on pre-Haswell CPUs. Keep /O2, no /arch.
 */
#pragma once
#include <windows.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <string.h>
#include <vector>

#define SCAP_DV_CELL 8  /* change-tracking granularity, divides every block size */
#define SCAP_DV_BPP  4  /* BGRA */

static __inline int ScapDvBlockSize(const RECT& r) /* legacy check_changed_bits */
{
    int w = (int)(r.right - r.left);
    if (w >= 32) return 64;
    if (w >= 16) return 16;
    return 8;
}

static __inline bool ScapDvClipRect(RECT& r, int w, int h)
{
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > w) r.right = w;
    if (r.bottom > h) r.bottom = h;
    return r.left < r.right && r.top < r.bottom;
}

/* Clip dst to the screen, shifting the source point along. True only when the
 * clipped source rect is fully on-screen too. */
static __inline bool ScapDvClipMove(RECT& dst, POINT& sp, int w, int h)
{
    if (dst.left < 0) { sp.x -= dst.left; dst.left = 0; }
    if (dst.top < 0)  { sp.y -= dst.top;  dst.top = 0; }
    if (dst.right > w)  dst.right = w;
    if (dst.bottom > h) dst.bottom = h;
    if (dst.left >= dst.right || dst.top >= dst.bottom) return false;
    return sp.x >= 0 && sp.y >= 0 &&
           sp.x + (dst.right - dst.left) <= w &&
           sp.y + (dst.bottom - dst.top) <= h;
}

/* Overlapping src/dst (1px scrolls) stay safe: row order by direction,
 * memmove within a row. */
static __inline void ScapDvApplyMove(uint8_t* buf, int stride,
                                     const RECT& dst, POINT sp)
{
    const int w = (int)(dst.right - dst.left);
    const int h = (int)(dst.bottom - dst.top);
    const size_t bytes = (size_t)w * SCAP_DV_BPP;

    if (dst.top > sp.y) /* downward move: copy bottom row first */
    {
        for (int r = h - 1; r >= 0; --r)
            memmove(buf + (size_t)(dst.top + r) * stride + (size_t)dst.left * SCAP_DV_BPP,
                    buf + (size_t)(sp.y + r) * stride + (size_t)sp.x * SCAP_DV_BPP, bytes);
    }
    else
    {
        for (int r = 0; r < h; ++r)
            memmove(buf + (size_t)(dst.top + r) * stride + (size_t)dst.left * SCAP_DV_BPP,
                    buf + (size_t)(sp.y + r) * stride + (size_t)sp.x * SCAP_DV_BPP, bytes);
    }
}

/* prev: last state sent to the client (updated in place; equals cur on
 * return). cur: this frame. All coordinates are frame-absolute pixels.
 * outMoves/outRects: what to actually transmit (moves first, in order). */
static void ScapVerifyFrame(uint8_t* prev, int prevStride,
                            const uint8_t* cur, int curStride, int w, int h,
                            const DXGI_OUTDUPL_MOVE_RECT* moves, int nMoves,
                            const RECT* dirtyIn, int nDirty,
                            std::vector<DXGI_OUTDUPL_MOVE_RECT>& outMoves,
                            std::vector<RECT>& outRects)
{
    outMoves.clear();
    outRects.clear();

    std::vector<RECT> dirty;
    dirty.reserve((size_t)nDirty + nMoves);
    for (int i = 0; i < nDirty; ++i)
    {
        RECT r = dirtyIn[i];
        if (ScapDvClipRect(r, w, h))
            dirty.push_back(r);
    }

    /* Stage 0: apply moves to prev in order. Drop decisions wait until after
     * the block compare - DXGI routinely re-reports move areas as dirty, so
     * area overlap alone is no signal (detect_dirty.md note 2). */
    struct KeptMove { DXGI_OUTDUPL_MOVE_RECT m; bool feedsLaterMove; };
    std::vector<KeptMove> kept;
    for (int i = 0; i < nMoves; ++i)
    {
        RECT dst = moves[i].DestinationRect;
        POINT sp = moves[i].SourcePoint;

        if (!ScapDvClipMove(dst, sp, w, h)) /* malformed move -> dst as dirty */
        {
            RECT r = moves[i].DestinationRect;
            if (ScapDvClipRect(r, w, h))
                dirty.push_back(r);
            continue;
        }

        bool feedsLaterMove = false;
        for (int j = i + 1; j < nMoves && !feedsLaterMove; ++j)
        {
            const RECT& d2 = moves[j].DestinationRect;
            RECT s2 = { moves[j].SourcePoint.x, moves[j].SourcePoint.y,
                        moves[j].SourcePoint.x + (d2.right - d2.left),
                        moves[j].SourcePoint.y + (d2.bottom - d2.top) };
            feedsLaterMove = dst.left < s2.right && s2.left < dst.right &&
                             dst.top < s2.bottom && s2.top < dst.bottom;
        }

        ScapDvApplyMove(prev, prevStride, dst, sp);
        KeptMove km = { { sp, dst }, feedsLaterMove };
        kept.push_back(km);
    }

    /* Stage 1: mark the 8px cells the dirty rects touch (dedup: overlapping
     * candidates get compared once - marks are cleared as blocks are done). */
    const int gw = (w + SCAP_DV_CELL - 1) / SCAP_DV_CELL;
    const int gh = (h + SCAP_DV_CELL - 1) / SCAP_DV_CELL;
    std::vector<uint8_t> mark((size_t)gw * gh, 0);
    std::vector<uint8_t> changed((size_t)gw * gh, 0);

    for (const RECT& d : dirty)
        for (int cy = (int)d.top / SCAP_DV_CELL; cy <= (int)(d.bottom - 1) / SCAP_DV_CELL; ++cy)
            for (int cx = (int)d.left / SCAP_DV_CELL; cx <= (int)(d.right - 1) / SCAP_DV_CELL; ++cx)
                mark[(size_t)cy * gw + cx] = 1;

    /* Stage 2: per candidate rect, compare prev(moved) vs cur in blocks sized
     * by the legacy heuristic. A block with any differing row is changed as a
     * whole (early exit), and prev is updated so later overlapping candidates
     * compare equal. Blocks are screen-grid aligned so cells line up. */
    for (const RECT& d : dirty)
    {
        const int B = ScapDvBlockSize(d);
        for (int by = (int)d.top / B * B; by < d.bottom; by += B)
        {
            const int y1 = (by + B < h) ? by + B : h;
            for (int bx = (int)d.left / B * B; bx < d.right; bx += B)
            {
                const int x1 = (bx + B < w) ? bx + B : w;
                const int c0x = bx / SCAP_DV_CELL, c1x = (x1 - 1) / SCAP_DV_CELL;
                const int c0y = by / SCAP_DV_CELL, c1y = (y1 - 1) / SCAP_DV_CELL;

                bool pending = false;
                for (int cy = c0y; cy <= c1y && !pending; ++cy)
                    for (int cx = c0x; cx <= c1x; ++cx)
                        if (mark[(size_t)cy * gw + cx]) { pending = true; break; }
                if (!pending)
                    continue; /* already compared via an earlier rect */

                const size_t bytes = (size_t)(x1 - bx) * SCAP_DV_BPP;
                int y0d = -1;
                for (int y = by; y < y1; ++y)
                    if (memcmp(prev + (size_t)y * prevStride + (size_t)bx * SCAP_DV_BPP,
                               cur + (size_t)y * curStride + (size_t)bx * SCAP_DV_BPP,
                               bytes))
                    { y0d = y; break; }

                for (int cy = c0y; cy <= c1y; ++cy)
                    for (int cx = c0x; cx <= c1x; ++cx)
                        mark[(size_t)cy * gw + cx] = 0;
                if (y0d < 0)
                    continue;

                /* Changed block: shrink to the actual diff bounding box at
                 * cell precision (TigerVNC ComparingUpdateTracker style) -
                 * last differing row from the bottom, then left/right cell
                 * columns scanned over the differing rows only. Rows and
                 * columns outside the box are equal, so re-sending only the
                 * box keeps prev == client. */
                int y1d = y1 - 1;
                while (y1d > y0d &&
                       0 == memcmp(prev + (size_t)y1d * prevStride + (size_t)bx * SCAP_DV_BPP,
                                   cur + (size_t)y1d * curStride + (size_t)bx * SCAP_DV_BPP,
                                   bytes))
                    --y1d;

                int c0d = c0x, c1d = c1x;
                for (; c0d < c1x; ++c0d)
                {
                    const int sx = c0d * SCAP_DV_CELL;
                    const size_t sb = (size_t)(((c0d + 1) * SCAP_DV_CELL < x1)
                                     ? SCAP_DV_CELL : x1 - sx) * SCAP_DV_BPP;
                    bool colDiff = false;
                    for (int y = y0d; y <= y1d && !colDiff; ++y)
                        colDiff = 0 != memcmp(prev + (size_t)y * prevStride + (size_t)sx * SCAP_DV_BPP,
                                              cur + (size_t)y * curStride + (size_t)sx * SCAP_DV_BPP, sb);
                    if (colDiff) break;
                }
                for (; c1d > c0d; --c1d)
                {
                    const int sx = c1d * SCAP_DV_CELL;
                    const size_t sb = (size_t)(((c1d + 1) * SCAP_DV_CELL < x1)
                                     ? SCAP_DV_CELL : x1 - sx) * SCAP_DV_BPP;
                    bool colDiff = false;
                    for (int y = y0d; y <= y1d && !colDiff; ++y)
                        colDiff = 0 != memcmp(prev + (size_t)y * prevStride + (size_t)sx * SCAP_DV_BPP,
                                              cur + (size_t)y * curStride + (size_t)sx * SCAP_DV_BPP, sb);
                    if (colDiff) break;
                }

                for (int cy = y0d / SCAP_DV_CELL; cy <= y1d / SCAP_DV_CELL; ++cy)
                    for (int cx = c0d; cx <= c1d; ++cx)
                        changed[(size_t)cy * gw + cx] = 1;

                /* prev takes the whole block - pixels outside the box are
                 * equal anyway, and one straight copy beats edge slicing */
                for (int y = by; y < y1; ++y)
                    memcpy(prev + (size_t)y * prevStride + (size_t)bx * SCAP_DV_BPP,
                           cur + (size_t)y * curStride + (size_t)bx * SCAP_DV_BPP,
                           bytes);
            }
        }
    }

    /* Stage 2.5: a move whose destination cells all changed is pointless -
     * the rect blit covers it (only safe at 100%, and never when a later
     * move sources from this destination). */
    for (const KeptMove& km : kept)
    {
        bool allCovered = !km.feedsLaterMove;
        const RECT& dst = km.m.DestinationRect;
        for (int cy = (int)dst.top / SCAP_DV_CELL;
             allCovered && cy <= (int)(dst.bottom - 1) / SCAP_DV_CELL; ++cy)
            for (int cx = (int)dst.left / SCAP_DV_CELL;
                 cx <= (int)(dst.right - 1) / SCAP_DV_CELL; ++cx)
                if (!changed[(size_t)cy * gw + cx]) { allCovered = false; break; }
        if (!allCovered)
            outMoves.push_back(km.m);
    }

    /* Stage 3: merge changed cells - horizontal runs, extended down while the
     * next row has the identical span. */
    struct Run { int cx0, cx1; size_t idx; };
    std::vector<Run> prevRuns, curRuns;
    for (int cy = 0; cy < gh; ++cy)
    {
        curRuns.clear();
        for (int cx = 0; cx < gw; )
        {
            if (!changed[(size_t)cy * gw + cx]) { ++cx; continue; }
            int cx0 = cx;
            while (cx < gw && changed[(size_t)cy * gw + cx]) ++cx;

            size_t idx = (size_t)-1;
            for (const Run& pr : prevRuns)
                if (pr.cx0 == cx0 && pr.cx1 == cx) { idx = pr.idx; break; }

            if (idx != (size_t)-1)
            {
                outRects[idx].bottom = ((cy + 1) * SCAP_DV_CELL < h)
                                     ? (cy + 1) * SCAP_DV_CELL : h;
            }
            else
            {
                RECT r = { cx0 * SCAP_DV_CELL, cy * SCAP_DV_CELL,
                           (cx * SCAP_DV_CELL < w) ? cx * SCAP_DV_CELL : w,
                           ((cy + 1) * SCAP_DV_CELL < h) ? (cy + 1) * SCAP_DV_CELL : h };
                outRects.push_back(r);
                idx = outRects.size() - 1;
            }
            Run run = { cx0, cx, idx };
            curRuns.push_back(run);
        }
        prevRuns.swap(curRuns);
    }
}
