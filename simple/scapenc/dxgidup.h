/* dxgidup.h - DXGI Desktop Duplication capture of the primary output.
 * Written fresh for the PoC (the legacy encoder used an external libwdp lib
 * that is not in this repo). Uses IDXGIOutput1::DuplicateOutput, so the
 * desktop image is always DXGI_FORMAT_B8G8R8A8_UNORM and no per-monitor DPI
 * awareness is required (unlike IDXGIOutput5::DuplicateOutput1).
 */
#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <vector>

struct DxgiDup
{
    ID3D11Device*           device = nullptr;
    ID3D11DeviceContext*    context = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    ID3D11Texture2D*        staging = nullptr;
    int                     width = 0, height = 0;
    bool                    forceFullFrame = true;
    std::vector<uint8_t>    metaBuf;

    enum AcquireResult { FRAME, TIMEOUT, NOCHANGE, AGAIN, FAIL };

    /* Creates the D3D11 device only; duplication is created on first Acquire. */
    bool Init();
    void Term();

    /* On FRAME: moveRects holds screen-to-screen copies (apply first, in
     * order), dirtyRects holds clipped dirty rects, and the staging texture
     * holds the full desktop image. moveRects is empty on any full-frame
     * fallback (then dirtyRects is one full-desktop rect).
     * Caller must call Map/Unmap around reading pixels. */
    AcquireResult Acquire(int timeoutMs, std::vector<RECT>& dirtyRects,
                          std::vector<DXGI_OUTDUPL_MOVE_RECT>& moveRects);

    bool Map(D3D11_MAPPED_SUBRESOURCE* mapped);
    void Unmap();

private:
    bool RecreateDuplication();
    void ReleaseDuplication();
};
