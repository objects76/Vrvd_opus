#include "dxgidup.h"

template <class T> static void SafeRelease(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

bool DxgiDup::Init()
{
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   0, nullptr, 0, D3D11_SDK_VERSION, &device,
                                   &fl, &context);
    return SUCCEEDED(hr);
}

void DxgiDup::Term()
{
    ReleaseDuplication();
    SafeRelease(context);
    SafeRelease(device);
}

void DxgiDup::ReleaseDuplication()
{
    SafeRelease(staging);
    SafeRelease(dup);
}

/* Find the primary output (desktop coords contain 0,0; fallback: output 0)
 * on the device's adapter and duplicate it. */
bool DxgiDup::RecreateDuplication()
{
    ReleaseDuplication();

    IDXGIDevice* dxgiDev = nullptr;
    if (FAILED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)))
        return false;
    IDXGIAdapter* adapter = nullptr;
    HRESULT hr = dxgiDev->GetAdapter(&adapter);
    dxgiDev->Release();
    if (FAILED(hr))
        return false;

    IDXGIOutput* output = nullptr, *first = nullptr;
    for (UINT i = 0; adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_OUTPUT_DESC desc;
        if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop &&
            desc.DesktopCoordinates.left <= 0 && desc.DesktopCoordinates.top <= 0 &&
            desc.DesktopCoordinates.right > 0 && desc.DesktopCoordinates.bottom > 0)
            break;
        if (!first) { first = output; first->AddRef(); }
        output->Release();
        output = nullptr;
    }
    if (!output)
        output = first; /* fallback */
    else
        SafeRelease(first);
    adapter->Release();
    if (!output)
        return false;

    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    output->Release();
    if (FAILED(hr))
        return false;

    hr = output1->DuplicateOutput(device, &dup);
    output1->Release();
    if (FAILED(hr)) /* E_ACCESSDENIED on secure desktop, ACCESS_LOST, ... */
        return false;

    DXGI_OUTDUPL_DESC dd;
    dup->GetDesc(&dd);
    width = (int)dd.ModeDesc.Width;
    height = (int)dd.ModeDesc.Height;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = dd.ModeDesc.Width;
    td.Height = dd.ModeDesc.Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging)))
    {
        ReleaseDuplication();
        return false;
    }

    forceFullFrame = true;
    return true;
}

DxgiDup::AcquireResult DxgiDup::Acquire(int timeoutMs, std::vector<RECT>& dirtyRects,
                                        std::vector<DXGI_OUTDUPL_MOVE_RECT>& moveRects)
{
    dirtyRects.clear();
    moveRects.clear();

    if (!dup && !RecreateDuplication())
        return AGAIN;

    DXGI_OUTDUPL_FRAME_INFO fi;
    IDXGIResource* res = nullptr;
    HRESULT hr = dup->AcquireNextFrame((UINT)timeoutMs, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        return TIMEOUT;
    if (hr == DXGI_ERROR_ACCESS_LOST)
    {
        ReleaseDuplication(); /* recreated on next call */
        return AGAIN;
    }
    if (FAILED(hr))
        return FAIL;

    /* Mouse-only update: no new desktop image. */
    if (fi.LastPresentTime.QuadPart == 0 && !forceFullFrame)
    {
        res->Release();
        dup->ReleaseFrame();
        return NOCHANGE;
    }

    /* Collect move + dirty metadata before releasing the frame. Metadata is
     * fetched even on a full-frame pass so the move diagnostics count every
     * move DXGI ever reports, including ones we then discard. */
    bool fullFrame = forceFullFrame || fi.AccumulatedFrames > 1;
    long long unmovedHere = 0;
    lastRawMoves = 0;
    if (fi.TotalMetadataBufferSize > 0)
    {
        metaBuf.resize(fi.TotalMetadataBufferSize);
        UINT moveBytes = 0, dirtyBytes = 0;
        if (FAILED(dup->GetFrameMoveRects((UINT)metaBuf.size(),
                       (DXGI_OUTDUPL_MOVE_RECT*)metaBuf.data(), &moveBytes)) ||
            FAILED(dup->GetFrameDirtyRects((UINT)(metaBuf.size() - moveBytes),
                       (RECT*)(metaBuf.data() + moveBytes), &dirtyBytes)))
        {
            fullFrame = true;
        }
        else
        {
            /* Forward moves as copy ops (coordinates instead of pixels).
             * src == dst "unmoved" moves (DXGI reports these occasionally)
             * are a no-op copy - demote the destination to dirty, which is
             * always safe because we CopyResource the full current frame.
             * A move not fully on the desktop violates the DXGI contract
             * ("moved to another location within the same image"); a later
             * move could chain on its destination, so don't patch it - punt
             * to a full frame. */
            const DXGI_OUTDUPL_MOVE_RECT* mv =
                (const DXGI_OUTDUPL_MOVE_RECT*)metaBuf.data();
            lastRawMoves = (int)(moveBytes / sizeof(*mv));
            statRawMoves += lastRawMoves;
            for (UINT i = 0; i < moveBytes / sizeof(*mv) && !fullFrame; ++i)
            {
                const RECT& d = mv[i].DestinationRect;
                const POINT& s = mv[i].SourcePoint;
                if (d.left < 0 || d.top < 0 ||
                    d.right > width || d.bottom > height ||
                    d.right <= d.left || d.bottom <= d.top ||
                    s.x < 0 || s.y < 0 ||
                    s.x + (d.right - d.left) > width ||
                    s.y + (d.bottom - d.top) > height)
                    fullFrame = true;
                else if (s.x != d.left || s.y != d.top)
                    moveRects.push_back(mv[i]);
                else
                {
                    dirtyRects.push_back(d);
                    ++unmovedHere;
                }
            }
            if (!fullFrame)
            {
                const RECT* dr = (const RECT*)(metaBuf.data() + moveBytes);
                for (UINT i = 0; i < dirtyBytes / sizeof(*dr); ++i)
                    dirtyRects.push_back(dr[i]);
            }
        }
    }
    else if (!fullFrame)
    {
        /* Frame presented but no metadata reported - treat as full frame. */
        fullFrame = true;
    }

    ID3D11Texture2D* tex = nullptr;
    hr = res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex);
    res->Release();
    if (FAILED(hr))
    {
        dup->ReleaseFrame();
        return FAIL;
    }
    context->CopyResource(staging, tex);
    tex->Release();
    dup->ReleaseFrame(); /* the staging copy is ours; don't block the duplicator */

    /* Clip to the desktop, drop empties, fall back to one full rect when the
     * dirty area gets large (ponytail: avoids pathological many-rect frames;
     * upgrade path would be rect coalescing). */
    RECT full = { 0, 0, width, height };
    long dirtyArea = 0;
    std::vector<RECT> clipped;
    for (RECT r : dirtyRects)
    {
        RECT c;
        if (IntersectRect(&c, &r, &full))
        {
            clipped.push_back(c);
            dirtyArea += (c.right - c.left) * (c.bottom - c.top);
        }
    }
    /* ponytail: moves whose destination is later covered by dirty rects are
     * not demoted - the decoder just overwrites; upgrade path is an
     * overlap-area heuristic (drop move when dirty covers >~80% of it). */
    if (fullFrame || (clipped.empty() && moveRects.empty()) ||
        dirtyArea > (long)width * height * 6 / 10)
    {
        clipped.assign(1, full);
        moveRects.clear(); /* full pixels are coming; copies are pointless */
    }
    dirtyRects.swap(clipped);

    ++statFrames;
    statUnmoved += unmovedHere;
    statForwarded += (long long)moveRects.size();
    statDroppedFull += lastRawMoves - unmovedHere - (long long)moveRects.size();

    forceFullFrame = false;
    return FRAME;
}

bool DxgiDup::Map(D3D11_MAPPED_SUBRESOURCE* mapped)
{
    return staging && SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, mapped));
}

void DxgiDup::Unmap()
{
    if (staging)
        context->Unmap(staging, 0);
}
