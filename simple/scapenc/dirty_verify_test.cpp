// dirty_verify_test.cpp — dirty_verify.h(ScapVerifyFrame) 테스트 하니스.
// (구 win_dirty.cpp — 파이프라인 본체가 dirty_verify.h로 옮겨진 뒤 남은
// 셀프테스트+데모를 사용처 옆으로 이동)
//
//   1) 순수 로직 셀프테스트 (T1~T6) — scapenc이 쓰는 코드 그대로 검증
//   2) DXGI Desktop Duplication 라이브 데모
//
// 블록 크기는 레거시 check_changed_bits의 적응형 휴리스틱
// (rect 폭 >=32 → 64px, 16~31 → 16px, <16 → 8px), 변경 추적은 8px 셀
// 그리드 + 블록 bbox 축소(TigerVNC 스타일) — 상세는 dirty_verify.h 주석.
//
// 빌드(셀프테스트+라이브 데모, scapenc.dll 빌드와 무관한 단독 실행 파일):
//   cl /EHsc /W4 /O2 /utf-8 /DWIN_DIRTY_SELFTEST dirty_verify_test.cpp d3d11.lib dxgi.lib

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "dirty_verify.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

typedef DXGI_OUTDUPL_MOVE_RECT MoveRect;
static const int BPP = SCAP_DV_BPP;

struct DirtyResult
{
    std::vector<MoveRect> moves; // 클라이언트가 먼저, 배열 순서대로 CopyRect 적용
    std::vector<RECT>     rects; // 그 다음 blit할 실제 변경 영역 (frame() 픽셀 기준)
};

static int64_t rect_area(const RECT& r)
{
    return (int64_t)(r.right - r.left) * (r.bottom - r.top);
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
            ScapVerifyFrame(mPrev.data(), mStride, (const uint8_t*)map.pData, (int)map.RowPitch,
                            mW, mH, nullptr, 0, &full, 1, out.moves, out.rects);
            mFirstFrame = false;
        }
        else {
            ScapVerifyFrame(mPrev.data(), mStride, (const uint8_t*)map.pData, (int)map.RowPitch,
                            mW, mH, moves, (int)nMove, dirtyRects, (int)nDirty,
                            out.moves, out.rects);
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
        ScapDvApplyMove(a.data(), stride, dst, sp);
        assert(a == naive);

        RECT dstUp = { 2, 2, 14, 12 };          // 위로 1px
        POINT spUp = { 2, 3 };
        srcCopy = naive;
        for (int r = 0; r < dstUp.bottom - dstUp.top; ++r)
            memcpy(naive.data() + (size_t)(dstUp.top + r) * stride + dstUp.left * BPP,
                   srcCopy.data() + (size_t)(spUp.y + r) * stride + spUp.x * BPP,
                   (size_t)(dstUp.right - dstUp.left) * BPP);
        ScapDvApplyMove(a.data(), stride, dstUp, spUp);
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
        ScapDvApplyMove(cur.data(), stride, mv.DestinationRect, mv.SourcePoint);
        for (int y = 80; y < 100; ++y)          // 스크롤로 노출된 하단 새 내용
            for (int x = 10; x < 150; ++x)
                *px(cur, stride, x, y) = 0xFFAABBCCu;
        *px(cur, stride, 170, 120) = 0xFF123456u;   // 별개의 1px 변경

        RECT dirtyIn[] = { { 10, 80, 150, 100 }, { 170, 120, 171, 121 },
                           { 10, 10, 150, 80 } };   // 마지막: DXGI가 move 영역을 dirty로 중복 보고
        DirtyResult out;
        ScapVerifyFrame(prev.data(), stride, cur.data(), stride, w, h,
                        &mv, 1, dirtyIn, 3, out.moves, out.rects);

        assert(out.moves.size() == 1);          // move는 살아야 함 (dirty가 전부 못 덮음)
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
        ScapVerifyFrame(prev.data(), stride, cur.data(), stride, w, h,
                        &mv, 1, dirtyIn, 1, out.moves, out.rects);

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
        ScapVerifyFrame(prev.data(), stride, cur.data(), stride, w, h,
                        nullptr, 0, dirtyIn, 1, out.moves, out.rects);
        assert(out.rects.empty() && out.moves.empty());
        printf("T4 false-dirty filter: OK\n");
    }

    // T5: 적응형 블록 크기 — 좁은 rect(<16px)는 8px 셀 정밀도로 검출되어야
    // 하고(64px 타일이면 이 검사는 실패), 넓은 rect는 블록 단위로 잡힌다.
    {
        const int w = 200, h = 150, stride = w * BPP;
        std::vector<uint8_t> prev(stride * h), cur;
        fill_pattern(prev, stride, w, h);
        cur = prev;
        *px(cur, stride, 100, 100) = 0xFF654321u;   // 1px 변경
        RECT dirtyIn[] = { { 100, 100, 101, 101 } };
        DirtyResult out;
        ScapVerifyFrame(prev.data(), stride, cur.data(), stride, w, h,
                        nullptr, 0, dirtyIn, 1, out.moves, out.rects);
        assert(out.rects.size() == 1);
        assert(rects_contain(out.rects, 100, 100));
        assert(rect_area(out.rects[0]) <= 8 * 8);   // 8px 블록 정밀도
        assert(prev == cur);
        printf("T5 adaptive block size: OK (%lldpx)\n", (long long)rect_area(out.rects[0]));
    }

    // T6: 변경 블록 bbox 축소 (TigerVNC 스타일) — 넓은 dirty rect(64px 블록)
    // 안의 1px 변경도 8px 셀 정밀도로 좁혀져야 한다. 블록 전체 마킹이면 실패.
    {
        const int w = 200, h = 150, stride = w * BPP;
        std::vector<uint8_t> prev(stride * h), cur;
        fill_pattern(prev, stride, w, h);
        cur = prev;
        *px(cur, stride, 100, 100) = 0xFF654321u;   // 1px 변경
        RECT dirtyIn[] = { { 0, 0, 200, 150 } };    // 폭 200 → 64px 블록
        DirtyResult out;
        ScapVerifyFrame(prev.data(), stride, cur.data(), stride, w, h,
                        nullptr, 0, dirtyIn, 1, out.moves, out.rects);
        assert(out.rects.size() == 1);
        assert(rects_contain(out.rects, 100, 100));
        assert(rect_area(out.rects[0]) <= 8 * 8);   // 블록(64x64)이 아니라 셀(8x8)
        assert(prev == cur);
        printf("T6 block bbox refine: OK (%lldpx)\n", (long long)rect_area(out.rects[0]));
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
