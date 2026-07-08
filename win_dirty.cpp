// win_dirty.cpp — 화면 변경 영역(dirty rect / move rect) 검출
//
// DXGI Desktop Duplication으로 OS가 보고하는 move/dirty 메타데이터를 받아,
// detect_dirty.md의 파이프라인으로 "실제로 바뀐 최소 영역"만 남긴다:
//
//   cur ≡ apply_dirty(apply_moves(prev, move_rects), dirty_rects)   (DXGI 재구성 규약)
//
//   1. move rect를 prev에 배열 순서대로 적용 (겹침은 memmove 시맨틱으로 보호)
//   2. move 적용된 prev를 기준으로 dirty 영역을 64x64 타일 단위 비교
//      → DXGI가 과대 보고한 dirty는 여기서 탈락
//   3. 비교 결과 dst 전체가 어차피 rect로 덮이는 move는 폐기 (CopyRect 왕복 낭비 제거)
//   4. 변경 타일만 rect로 병합해 출력하고, 그 타일만 prev에 복사
//      → 검출 후 prev == cur (서버 prev와 클라이언트 프레임버퍼 동기 유지)
//
// MoveRect는 SCapEnc의 MY_DXGI_OUTDUPL_MOVE_RECT(desk/DeskHook.h)와 동일 레이아웃.
//
// 빌드(셀프테스트+라이브 데모):
//   cl /EHsc /W4 /O2 /DWIN_DIRTY_SELFTEST win_dirty.cpp d3d11.lib dxgi.lib

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <string.h>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

struct MoveRect                 // == DXGI_OUTDUPL_MOVE_RECT == MY_DXGI_OUTDUPL_MOVE_RECT
{
    POINT SourcePoint;
    RECT  DestinationRect;
};
static_assert(sizeof(MoveRect) == sizeof(DXGI_OUTDUPL_MOVE_RECT), "layout must match DXGI");

struct DirtyResult
{
    std::vector<MoveRect> moves; // 클라이언트가 먼저, 배열 순서대로 CopyRect 적용
    std::vector<RECT>     rects; // 그 다음 blit할 실제 변경 영역 (frame() 픽셀 기준)
};

// ---------------------------------------------------------------------------
// CPU 파이프라인 (DXGI와 무관한 순수 로직 — 셀프테스트 대상)
// ---------------------------------------------------------------------------

static const int TILE = 64;
static const int BPP  = 4;   // BGRA

static int64_t rect_area(const RECT& r)
{
    return (int64_t)(r.right - r.left) * (r.bottom - r.top);
}

static int64_t intersect_area(const RECT& a, const RECT& b)
{
    LONG l = max(a.left, b.left),  r = min(a.right, b.right);
    LONG t = max(a.top, b.top),    btm = min(a.bottom, b.bottom);
    return (l < r && t < btm) ? (int64_t)(r - l) * (btm - t) : 0;
}

static bool clip_rect(RECT& r, int w, int h)
{
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > w) r.right = w;
    if (r.bottom > h) r.bottom = h;
    return r.left < r.right && r.top < r.bottom;
}

// dst를 화면에 클립하면서 src 시작점도 같이 이동. src까지 화면 안일 때만 true.
static bool clip_move(RECT& dst, POINT& sp, int w, int h)
{
    if (dst.left < 0) { sp.x -= dst.left; dst.left = 0; }
    if (dst.top < 0)  { sp.y -= dst.top;  dst.top = 0; }
    if (dst.right > w)  dst.right = w;
    if (dst.bottom > h) dst.bottom = h;
    if (dst.left >= dst.right || dst.top >= dst.bottom) return false;
    return sp.x >= 0 && sp.y >= 0
        && sp.x + (dst.right - dst.left) <= w
        && sp.y + (dst.bottom - dst.top) <= h;
}

// src/dst가 겹치는 move(1px 스크롤 등)도 안전하게: 세로는 행 순서로, 가로는 memmove로 보호.
static void apply_move(uint8_t* buf, int stride, const RECT& dst, POINT sp)
{
    const int w = dst.right - dst.left;
    const int h = dst.bottom - dst.top;
    const size_t bytes = (size_t)w * BPP;

    if (dst.top > sp.y) {       // 아래 방향 이동 → 아래 행부터 복사
        for (int r = h - 1; r >= 0; --r)
            memmove(buf + (size_t)(dst.top + r) * stride + (size_t)dst.left * BPP,
                    buf + (size_t)(sp.y + r) * stride + (size_t)sp.x * BPP, bytes);
    }
    else {
        for (int r = 0; r < h; ++r)
            memmove(buf + (size_t)(dst.top + r) * stride + (size_t)dst.left * BPP,
                    buf + (size_t)(sp.y + r) * stride + (size_t)sp.x * BPP, bytes);
    }
}

// prev(이전 프레임, 검출 후 cur와 동일해짐)와 cur(이번 프레임)로부터
// 최종 전송용 move/rect 목록을 만든다. 좌표는 모두 출력(모니터) 기준 픽셀.
static void process_frame(uint8_t* prev, int prevStride,
                          const uint8_t* cur, int curStride,
                          int w, int h,
                          const MoveRect* moves, int nMoves,
                          const RECT* dirtyIn, int nDirty,
                          DirtyResult& out)
{
    out.moves.clear();
    out.rects.clear();

    std::vector<RECT> dirty;
    dirty.reserve(nDirty + nMoves);
    for (int i = 0; i < nDirty; ++i) {
        RECT r = dirtyIn[i];
        if (clip_rect(r, w, h))
            dirty.push_back(r);
    }

    // Stage 0: move를 prev에 순서대로 적용
    // 폐기 판단은 여기서 하지 않는다 — DXGI가 move 영역을 dirty로 중복 보고하는 게
    // 흔해서(detect_dirty.md 주의점 2) 면적 겹침은 신호가 못 된다. 타일 비교 후 판단.
    struct KeptMove { MoveRect m; bool feedsLaterMove; };
    std::vector<KeptMove> kept;
    for (int i = 0; i < nMoves; ++i) {
        RECT dst = moves[i].DestinationRect;
        POINT sp = moves[i].SourcePoint;

        if (!clip_move(dst, sp, w, h)) {        // 비정상 move → dst만 dirty로
            RECT r = moves[i].DestinationRect;
            if (clip_rect(r, w, h))
                dirty.push_back(r);
            continue;
        }

        bool feedsLaterMove = false;
        for (int j = i + 1; j < nMoves && !feedsLaterMove; ++j) {
            const RECT& d2 = moves[j].DestinationRect;
            RECT src2 = { moves[j].SourcePoint.x, moves[j].SourcePoint.y,
                          moves[j].SourcePoint.x + (d2.right - d2.left),
                          moves[j].SourcePoint.y + (d2.bottom - d2.top) };
            feedsLaterMove = intersect_area(dst, src2) > 0;
        }

        apply_move(prev, prevStride, dst, sp);
        KeptMove km = { { sp, dst }, feedsLaterMove };
        kept.push_back(km);
    }

    // Stage 1: dirty 영역이 걸치는 64x64 타일 마킹 (겹치는 dirty rect 중복 비교 방지)
    const int gw = (w + TILE - 1) / TILE;
    const int gh = (h + TILE - 1) / TILE;
    std::vector<uint8_t> mark((size_t)gw * gh, 0);
    std::vector<uint8_t> changed((size_t)gw * gh, 0);

    for (const RECT& d : dirty)
        for (int ty = d.top / TILE; ty <= (d.bottom - 1) / TILE; ++ty)
            for (int tx = d.left / TILE; tx <= (d.right - 1) / TILE; ++tx)
                mark[(size_t)ty * gw + tx] = 1;

    // Stage 2: 마킹된 타일만 prev(move 반영됨) vs cur 비교, 다르면 prev 갱신
    // (move가 정확했다면 move 영역 위 dirty 타일은 대부분 여기서 탈락)
    for (int ty = 0; ty < gh; ++ty) {
        const int y0 = ty * TILE, y1 = min(y0 + TILE, h);
        for (int tx = 0; tx < gw; ++tx) {
            if (!mark[(size_t)ty * gw + tx])
                continue;
            const int x0 = tx * TILE, x1 = min(x0 + TILE, w);
            const size_t bytes = (size_t)(x1 - x0) * BPP;

            bool diff = false;
            for (int y = y0; y < y1 && !diff; ++y)
                diff = 0 != memcmp(prev + (size_t)y * prevStride + (size_t)x0 * BPP,
                                   cur + (size_t)y * curStride + (size_t)x0 * BPP, bytes);
            if (!diff)
                continue;

            changed[(size_t)ty * gw + tx] = 1;
            for (int y = y0; y < y1; ++y)
                memcpy(prev + (size_t)y * prevStride + (size_t)x0 * BPP,
                       cur + (size_t)y * curStride + (size_t)x0 * BPP, bytes);
        }
    }

    // Stage 2.5: dst에 걸친 타일이 전부 변경된 move는 폐기 —
    // 어차피 rect blit이 dst 전체를 덮으므로 CopyRect는 왕복 낭비다(md 주의점 3).
    // 전부(100%)일 때만 안전: 일부라도 남으면 그 픽셀은 move에 의존한다.
    // 이후 move가 이 dst에서 픽셀을 가져가는 경우도 폐기 불가(클라이언트 재구성이 어긋남).
    for (const KeptMove& km : kept) {
        bool allCovered = !km.feedsLaterMove;
        const RECT& dst = km.m.DestinationRect;
        for (int ty = dst.top / TILE; allCovered && ty <= (dst.bottom - 1) / TILE; ++ty)
            for (int tx = dst.left / TILE; tx <= (dst.right - 1) / TILE; ++tx)
                if (!changed[(size_t)ty * gw + tx]) { allCovered = false; break; }
        if (!allCovered)
            out.moves.push_back(km.m);
    }

    // Stage 3: 변경 타일 병합 — 행 내 연속 run → 아래 행의 동일 span과 세로 확장
    struct Run { int tx0, tx1; size_t idx; };
    std::vector<Run> prevRuns, curRuns;
    for (int ty = 0; ty < gh; ++ty) {
        curRuns.clear();
        for (int tx = 0; tx < gw; ) {
            if (!changed[(size_t)ty * gw + tx]) { ++tx; continue; }
            int tx0 = tx;
            while (tx < gw && changed[(size_t)ty * gw + tx]) ++tx;

            size_t idx = (size_t)-1;
            for (const Run& pr : prevRuns)
                if (pr.tx0 == tx0 && pr.tx1 == tx) { idx = pr.idx; break; }

            if (idx != (size_t)-1) {
                out.rects[idx].bottom = min((ty + 1) * TILE, h);
            }
            else {
                RECT r = { tx0 * TILE, ty * TILE, min(tx * TILE, w), min((ty + 1) * TILE, h) };
                out.rects.push_back(r);
                idx = out.rects.size() - 1;
            }
            Run run = { tx0, tx, idx };
            curRuns.push_back(run);
        }
        prevRuns.swap(curRuns);
    }
}

// ---------------------------------------------------------------------------
// DXGI Desktop Duplication 캡처
// ---------------------------------------------------------------------------

class WinDirty
{
public:
    ~WinDirty() { term(); }

    // outputIndex: 기본 어댑터의 모니터 인덱스.
    // ponytail: 다중 GPU(모니터가 다른 어댑터에 물린 경우)는 미지원.
    //           필요해지면 EnumAdapters 루프로 확장.
    HRESULT init(int outputIndex = 0)
    {
        term();
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                       nullptr, 0, D3D11_SDK_VERSION, &mDev, nullptr, &mCtx);
        if (FAILED(hr)) return hr;

        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIOutput* output = nullptr;
        IDXGIOutput1* output1 = nullptr;

        hr = mDev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
        if (SUCCEEDED(hr)) hr = dxgiDev->GetAdapter(&adapter);
        if (SUCCEEDED(hr)) hr = adapter->EnumOutputs(outputIndex, &output);
        if (SUCCEEDED(hr)) hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
        if (SUCCEEDED(hr)) hr = output1->DuplicateOutput(mDev, &mDupl);

        SAFE_RELEASE(output1);
        SAFE_RELEASE(output);
        SAFE_RELEASE(adapter);
        SAFE_RELEASE(dxgiDev);
        if (FAILED(hr)) { term(); return hr; }

        DXGI_OUTDUPL_DESC dd;
        mDupl->GetDesc(&dd);
        if (dd.Rotation != DXGI_MODE_ROTATION_IDENTITY) {
            // ponytail: 회전 모니터는 dirty 좌표 변환이 필요해 미지원. 필요 시 회전 매핑 추가.
            term();
            return E_NOTIMPL;
        }
        mW = (int)dd.ModeDesc.Width;
        mH = (int)dd.ModeDesc.Height;
        mStride = mW * BPP;
        mPrev.assign((size_t)mStride * mH, 0);
        mFirstFrame = true;

        D3D11_TEXTURE2D_DESC td = {};
        td.Width = dd.ModeDesc.Width;
        td.Height = dd.ModeDesc.Height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = dd.ModeDesc.Format;    // 통상 DXGI_FORMAT_B8G8R8A8_UNORM
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = mDev->CreateTexture2D(&td, nullptr, &mStaging);
        if (FAILED(hr)) term();
        return hr;
    }

    void term()
    {
        SAFE_RELEASE(mStaging);
        SAFE_RELEASE(mDupl);
        SAFE_RELEASE(mCtx);
        SAFE_RELEASE(mDev);
        mPrev.clear();
    }

    // S_OK: 프레임 처리됨(out에 move/rect, 비어 있으면 실변경 없음)
    // S_FALSE: timeout — 화면 변화 없음
    // DXGI_ERROR_ACCESS_LOST 등: term() 후 init() 재시도 필요 (모드 변경, 세션 전환 등)
    HRESULT detect(DirtyResult& out, UINT timeoutMs = 16)
    {
        out.moves.clear();
        out.rects.clear();
        if (!mDupl) return E_FAIL;

        DXGI_OUTDUPL_FRAME_INFO info;
        IDXGIResource* res = nullptr;
        HRESULT hr = mDupl->AcquireNextFrame(timeoutMs, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return S_FALSE;
        if (FAILED(hr)) return hr;

        // 메타데이터: [move rect들][dirty rect들] — AccumulatedFrames > 1이면 누적치
        UINT nMove = 0, nDirty = 0;
        if (info.TotalMetadataBufferSize) {
            mMeta.resize(info.TotalMetadataBufferSize);
            UINT used = 0;
            hr = mDupl->GetFrameMoveRects((UINT)mMeta.size(),
                                          (DXGI_OUTDUPL_MOVE_RECT*)mMeta.data(), &used);
            if (FAILED(hr)) { res->Release(); mDupl->ReleaseFrame(); return hr; }
            nMove = used / sizeof(DXGI_OUTDUPL_MOVE_RECT);

            UINT moveBytes = used;
            hr = mDupl->GetFrameDirtyRects((UINT)mMeta.size() - moveBytes,
                                           (RECT*)(mMeta.data() + moveBytes), &used);
            if (FAILED(hr)) { res->Release(); mDupl->ReleaseFrame(); return hr; }
            nDirty = used / sizeof(RECT);
        }
        const MoveRect* moves = (const MoveRect*)mMeta.data();
        const RECT* dirtyRects = (const RECT*)(mMeta.data() + nMove * sizeof(MoveRect));

        // GPU → staging. ponytail: 전체 CopyResource 한 방.
        //   PCIe 대역폭이 문제되면 dirty 영역만 CopySubresourceRegion으로 업그레이드.
        ID3D11Texture2D* tex = nullptr;
        hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
        if (SUCCEEDED(hr)) mCtx->CopyResource(mStaging, tex);
        SAFE_RELEASE(tex);
        SAFE_RELEASE(res);
        mDupl->ReleaseFrame();      // 복사는 큐잉됨 — 프레임은 최대한 빨리 반납
        if (FAILED(hr)) return hr;

        D3D11_MAPPED_SUBRESOURCE map;
        hr = mCtx->Map(mStaging, 0, D3D11_MAP_READ, 0, &map);
        if (FAILED(hr)) return hr;

        if (mFirstFrame) {          // prev가 없으므로 전체 화면을 dirty로 취급
            RECT full = { 0, 0, mW, mH };
            process_frame(mPrev.data(), mStride, (const uint8_t*)map.pData, (int)map.RowPitch,
                          mW, mH, nullptr, 0, &full, 1, out);
            mFirstFrame = false;
        }
        else {
            process_frame(mPrev.data(), mStride, (const uint8_t*)map.pData, (int)map.RowPitch,
                          mW, mH, moves, (int)nMove, dirtyRects, (int)nDirty, out);
        }
        mCtx->Unmap(mStaging, 0);
        return S_OK;
        // 참고: 마우스 포인터(info.PointerShapeBufferSize)는 별도 채널 — 여기선 미처리.
    }

    // detect() 직후 prev == 현재 화면. out.rects의 픽셀 원본으로 사용.
    const uint8_t* frame() const { return mPrev.data(); }
    int width()  const { return mW; }
    int height() const { return mH; }
    int stride() const { return mStride; }

private:
    ID3D11Device* mDev = nullptr;
    ID3D11DeviceContext* mCtx = nullptr;
    IDXGIOutputDuplication* mDupl = nullptr;
    ID3D11Texture2D* mStaging = nullptr;
    std::vector<uint8_t> mPrev;
    std::vector<uint8_t> mMeta;
    int mW = 0, mH = 0, mStride = 0;
    bool mFirstFrame = true;
};

// ---------------------------------------------------------------------------
// 셀프테스트 + 라이브 데모
// ---------------------------------------------------------------------------
#ifdef WIN_DIRTY_SELFTEST

#include <assert.h>
#include <stdio.h>

static uint32_t* px(std::vector<uint8_t>& buf, int stride, int x, int y)
{
    return (uint32_t*)(buf.data() + (size_t)y * stride + (size_t)x * BPP);
}

static void fill_pattern(std::vector<uint8_t>& buf, int stride, int w, int h)
{
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            *px(buf, stride, x, y) = 0xFF000000u | (uint32_t)(x * 7 + y * 131);
}

static bool rects_contain(const std::vector<RECT>& rects, int x, int y)
{
    for (const RECT& r : rects)
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom)
            return true;
    return false;
}

int main()
{
    // T1: 겹치는 move의 memmove 시맨틱 (1행 스크롤)
    {
        const int w = 16, h = 16, stride = w * BPP;
        std::vector<uint8_t> a(stride * h), naive(stride * h);
        fill_pattern(a, stride, w, h);
        naive = a;

        RECT dst = { 2, 3, 14, 13 };            // 아래로 1px: dst.top(3) > src.y(2), 겹침
        POINT sp = { 2, 2 };
        std::vector<uint8_t> srcCopy = naive;   // naive: 원본 사본에서 복사 (겹침 없음)
        for (int r = 0; r < dst.bottom - dst.top; ++r)
            memcpy(naive.data() + (size_t)(dst.top + r) * stride + dst.left * BPP,
                   srcCopy.data() + (size_t)(sp.y + r) * stride + sp.x * BPP,
                   (size_t)(dst.right - dst.left) * BPP);
        apply_move(a.data(), stride, dst, sp);
        assert(a == naive);

        RECT dstUp = { 2, 2, 14, 12 };          // 위로 1px
        POINT spUp = { 2, 3 };
        srcCopy = naive;
        for (int r = 0; r < dstUp.bottom - dstUp.top; ++r)
            memcpy(naive.data() + (size_t)(dstUp.top + r) * stride + dstUp.left * BPP,
                   srcCopy.data() + (size_t)(spUp.y + r) * stride + spUp.x * BPP,
                   (size_t)(dstUp.right - dstUp.left) * BPP);
        apply_move(a.data(), stride, dstUp, spUp);
        assert(a == naive);
        printf("T1 apply_move overlap: OK\n");
    }

    // T2: 스크롤 + 픽셀 변경 파이프라인 — move 유지, 변경만 검출, prev == cur
    {
        const int w = 200, h = 150, stride = w * BPP;
        std::vector<uint8_t> prev(stride * h), cur;
        fill_pattern(prev, stride, w, h);
        cur = prev;

        // cur 구성: {10,30..150,100}을 20px 위로 스크롤 → dst {10,10,150,80}
        MoveRect mv = { { 10, 30 }, { 10, 10, 150, 80 } };
        apply_move(cur.data(), stride, mv.DestinationRect, mv.SourcePoint);
        for (int y = 80; y < 100; ++y)          // 스크롤로 노출된 하단 새 내용
            for (int x = 10; x < 150; ++x)
                *px(cur, stride, x, y) = 0xFFAABBCCu;
        *px(cur, stride, 170, 120) = 0xFF123456u;   // 별개의 1px 변경

        RECT dirtyIn[] = { { 10, 80, 150, 100 }, { 170, 120, 171, 121 },
                           { 10, 10, 150, 80 } };   // 마지막: DXGI가 move 영역을 dirty로 중복 보고
        DirtyResult out;
        process_frame(prev.data(), stride, cur.data(), stride, w, h,
                      &mv, 1, dirtyIn, 3, out);

        assert(out.moves.size() == 1);          // move는 살아야 함 (dirty가 80% 못 덮음)
        assert(prev == cur);                    // 검출 후 동기화 완료
        assert(rects_contain(out.rects, 170, 120));
        assert(rects_contain(out.rects, 10, 85));
        assert(!rects_contain(out.rects, 10, 15)); // move가 정확했으니 중복 dirty는 탈락
        printf("T2 scroll pipeline: OK (%zu rects)\n", out.rects.size());
    }

    // T3: dirty가 move dst를 전부 덮으면 move 폐기 + dirty 흡수
    {
        const int w = 200, h = 150, stride = w * BPP;
        std::vector<uint8_t> prev(stride * h), cur;
        fill_pattern(prev, stride, w, h);
        cur = prev;
        for (int y = 10; y < 80; ++y)
            for (int x = 10; x < 150; ++x)
                *px(cur, stride, x, y) = 0xFF445566u;   // dst 전체가 새 내용

        MoveRect mv = { { 10, 30 }, { 10, 10, 150, 80 } };
        RECT dirtyIn[] = { { 10, 10, 150, 80 } };
        DirtyResult out;
        process_frame(prev.data(), stride, cur.data(), stride, w, h,
                      &mv, 1, dirtyIn, 1, out);

        assert(out.moves.empty());              // 폐기됨
        assert(prev == cur);
        assert(rects_contain(out.rects, 10, 15));
        printf("T3 move drop heuristic: OK\n");
    }

    // T4: dirty 보고됐지만 실제 변경 없음 → rect 0개 (과대 보고 필터링)
    {
        const int w = 200, h = 150, stride = w * BPP;
        std::vector<uint8_t> prev(stride * h), cur;
        fill_pattern(prev, stride, w, h);
        cur = prev;
        RECT dirtyIn[] = { { 0, 0, 200, 150 } };
        DirtyResult out;
        process_frame(prev.data(), stride, cur.data(), stride, w, h,
                      nullptr, 0, dirtyIn, 1, out);
        assert(out.rects.empty() && out.moves.empty());
        printf("T4 false-dirty filter: OK\n");
    }

    printf("all self-tests passed\n\n");

    // 라이브 데모: 실제 화면에서 2초간 검출
    WinDirty wd;
    HRESULT hr = wd.init(0);
    if (FAILED(hr)) {
        printf("live demo skipped: DuplicateOutput failed hr=0x%08lX "
               "(RDP/보호 세션에서는 정상)\n", hr);
        return 0;
    }
    printf("live: %dx%d 캡처 시작\n", wd.width(), wd.height());
    for (int i = 0; i < 120; ++i) {
        DirtyResult out;
        hr = wd.detect(out, 16);
        if (hr == S_OK && (!out.rects.empty() || !out.moves.empty())) {
            int64_t area = 0;
            for (const RECT& r : out.rects) area += rect_area(r);
            printf("frame %3d: moves=%zu rects=%zu changed=%.1f%%\n",
                   i, out.moves.size(), out.rects.size(),
                   100.0 * area / ((int64_t)wd.width() * wd.height()));
        }
        else if (FAILED(hr)) {
            printf("detect failed hr=0x%08lX\n", hr);
            break;
        }
    }
    return 0;
}
#endif // WIN_DIRTY_SELFTEST
