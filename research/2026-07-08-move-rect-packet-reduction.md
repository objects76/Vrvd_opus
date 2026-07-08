---
title: "DXGI_OUTDUPL_MOVE_RECT를 이용한 전송 패킷량 절감 연구"
created: 2026-07-08
tags:
  - research
  - scapenc
---

# DXGI_OUTDUPL_MOVE_RECT를 이용한 전송 패킷량 절감

## 결론 요약

Move rect는 "픽셀 데이터" 대신 "복사 명령(좌표)"을 보내라고 OS가 주는 힌트다.
Microsoft 공식 문서가 명시한 설계 목적 자체가 원격 전송량 절감이다:

> "Suppose the desktop image was transmitted over a slow connection to your remote
> client app. The amount of data that is sent over the connection is reduced by
> receiving only data about how your client app must move regions of pixels rather
> than actual pixel data. To process the moves, your client app must have stored
> the complete last image." — Desktop Duplication API (MS Learn)

현재 scapenc는 move rect의 DestinationRect를 dirty로 편입시켜 **픽셀을 그대로 재전송**하고
있다(dxgidup.cpp:143-151). 이를 VNC CopyRect 방식의 "copy op"로 바꾸면
전체 화면 스크롤 시 rect 하나당 수 MB(1920×1080 8bpp 기준 ~2MB raw)가 **12바이트 좌표**로
줄어든다. 스크롤/창 이동이 절감 폭이 가장 큰 시나리오다.

## 1. 실제 제품들의 처리 방식 비교

| 제품 | move 처리 | 이유 |
| --- | --- | --- |
| VNC/RFB (RFC 6143 §7.7.2) | **CopyRect 인코딩**: rect 헤더 + 소스 좌표 4바이트만 전송 | 클라이언트 프레임버퍼에 이미 있는 픽셀 재사용. 명시 용도가 "창 이동, 스크롤" |
| RDP (레거시 MS-RDPEGDI) | ScreenBlt drawing order + bitmap cache | 그리기 연산을 인코딩, 픽셀 재전송 회피 |
| RDP (모던 MS-RDPEGFX, Win8+) | **RDPGFX_CMDID_SURFACETOSURFACE** PDU: 서피스 내/간 복사 명령 | 스크롤 콘텐츠를 재인코딩 없이 클라이언트에서 복사 |
| x11vnc | X11엔 move 힌트가 없어서 **스크롤을 휴리스틱으로 추정**해서까지 CopyRect 생성 (`-scrollcopyrect`, `-wirecopyrect`) | 절감 효과가 커서 추정 비용을 감수 |
| WebRTC/Chromium (dxgi_output_duplicator.cc) | move의 source+destination을 **둘 다 dirty로 편입**, 복사 안 함 | 후단이 VP8/H.264 비디오 코덱 → motion estimation이 코덱 안에 이미 있음 |
| RC5XWIN SCapEnc (DesktopThread.cpp:483-497) | destination만 dirty로 편입 (WebRTC와 유사) | 단순화 |

핵심 분기: **후단이 비디오 코덱이면 move를 dirty로 뭉개는 게 맞고(코덱이 알아서 함),
우리처럼 rect+zlib 프로토콜이면 CopyRect 방식이 정답**이다. WebRTC를 따라 하면 안 되는
지점이 여기다.

## 2. DXGI move rect 규약 (공식 문서 기준)

- `DXGI_OUTDUPL_MOVE_RECT` = `{ POINT SourcePoint; RECT DestinationRect; }`.
  source와 destination 크기는 항상 동일(스트레치 없음).
- **재구성 순서 규약**: "an application must first process all move RECTs before it
  processes dirty RECTs" (GetFrameMoveRects Remarks). move 배열은 주어진 순서대로 적용.
- 프레임 누적 시(AccumulatedFrames > 1) OS가 update region을 **coalescing**하여 실제
  변경 안 된 픽셀까지 포함할 수 있음 → 현재 코드의 full-frame 폴백은 안전한 처리.
- WebRTC 소스 주석: DXGI가 **source == destination인 "unmoved" move rect를 반환하는
  경우가 있음** → 필터링 필요.
- RFC 6143 호환 규칙: CopyRect의 source는 같은 업데이트에서 앞서 갱신된 픽셀을 포함하지
  말 것 → "move 먼저, DXGI 배열 순서대로"를 지키면 자동으로 만족.

## 3. scapenc/scapdec 적용 설계

전제: scapdec의 8bpp DIBSection 캔버스(scapdec.cpp)가 곧 "클라이언트가 보관한 직전
프레임"이므로, 수신측 prev frame 요구사항은 **이미 충족**되어 있다. 서버측 추가 버퍼도
필요 없다(move는 DXGI가 이미 검증한 사실이므로 서버는 좌표만 중계).

### 와이어 포맷 (scap_packet.h)

```c
/* ScapFrameHdr.reserved -> moveCount 로 전용 */
typedef struct ScapMoveRect      /* 12 bytes, payload 선두에 moveCount개 */
{
    uint16_t srcX, srcY;         /* source top-left */
    uint16_t x, y, w, h;         /* destination rect */
} ScapMoveRect;
/* payload = moveCount x ScapMoveRect + rectCount x { ScapRectHdr + pixels } */
```

### 인코더 (dxgidup.cpp / scapenc.cpp)

1. `Acquire()`가 move rect를 dirty로 밀어넣는 대신 별도 `std::vector<ScapMoveRect>`로 반환.
2. `SourcePoint == DestinationRect.topleft`인 unmoved rect는 dirty로 강등 (WebRTC 필터).
3. full-frame 폴백(모드 변경, AccumulatedFrames>1, dirty 60% 초과) 시 move는 전부 버림
   — 전체 픽셀이 가므로 불필요.
4. 휴리스틱: destination이 dirty rect들로 ~80% 이상 덮이면 move를 버리고 dirty로 흡수
   (클라이언트 복사 연산 낭비 방지, 기존 research 문서 §실무 주의점 3).

### 디코더 (scapdec.cpp)

pixel rect 루프 전에 move를 배열 순서대로 적용:

```c
/* 행 단위 복사. src/dst가 겹칠 수 있으므로 방향 인지(memmove 시맨틱):
 * dstY > srcY 이면 아래 행부터 위로 복사 */
```

8bpp 캔버스에서 행당 `memmove` 한 번씩이면 끝. 경계 검증(src/dst 모두 캔버스 안)은
신뢰 경계이므로 필수.

### 기대 절감량

| 시나리오 | 현재 (dirty 재전송) | copy op 적용 후 |
| --- | --- | --- |
| 1080p 문서 전체 스크롤 | ~2MB raw → zlib 후 수백 KB/프레임 | 12 bytes + 신규 노출 스트립(수십 KB) |
| 창 드래그 이동 | 창 면적 × 2 (이전+새 위치 dirty) | 12 bytes + 노출된 배경 영역 |
| 타이핑/동영상 | 변화 없음 (move 미발생) | 변화 없음 |

### 주의사항 / 미검증 항목

- **move rect 실전 발생 빈도는 OS 버전·GPU·시나리오 의존**이며 공식 문서에 보장이 없다.
  DWM 합성 경로에 따라 스크롤에서 move가 안 나오고 dirty만 나올 수 있음(미검증 — 계측
  필요). 따라서 move는 "있으면 큰 절감, 없어도 동작 동일"인 순수 추가 최적화로 설계해야
  하고, 아래 §4의 서버측 기법들이 move 부재를 보완한다.
- 프로토콜 버전 호환: SCAP_MAGIC 갱신 또는 reserved==0 유지로 구분.

## 4. 추가로 이용 가능한 기술·알고리즘 (웹 조사)

우선순위 순 (이 파이프라인 기준 절감효과 ÷ 구현비용):

1. **Copy op (본 문서의 주제)** — 스크롤/창이동에서 최대 절감. 구현비용 소.
2. **서버측 prev frame + 타일 검증** — DXGI dirty rect는 보수적(coalescing으로 과대보고).
   서버가 prev frame(8bpp 변환 후 버퍼)을 유지하고 dirty 영역을 64×64 타일로 비교해
   실제 변경 타일만 전송. x11vnc·기존 research 문서(2026-07-08-Windows_10_원격_제어_기술.md)의
   표준 알고리즘. 변경 없는 타일이 걸러지는 만큼 절감. 구현비용 중.
3. **XOR delta + zlib** — 2번의 prev frame이 생기면 공짜로 얹을 수 있음: 변경 타일을
   `cur XOR prev`로 보내면 유사 콘텐츠에서 0이 대량 발생 → zlib이 극도로 잘 압축.
   디코더는 XOR 복원. 구현비용 소(2번 전제).
4. **타일 해시 캐시 (RDP bitmap cache / RDPEGFX SurfaceToCache-CacheToSurface 방식)** —
   타일 해시(xxHash 등)를 서버·클라이언트가 LRU 캐시로 공유, 재등장 타일은 캐시 인덱스만
   전송. 창 전환·알트탭·반복 UI에서 효과 큼. RDP는 디스크 영속 캐시까지 씀. 구현비용 중상.
5. **압축기 교체: zstd** — zlib 대비 동급 압축률에서 수 배 빠르고, 레벨 올리면 압축률도
   우위. 이미 프레임 단위 one-shot 구조라 교체 지점이 한 곳(compress2/uncompress).
   의존성 추가가 유일한 비용.
6. **Solid fill 감지 (RDPEGFX SOLIDFILL 상당)** — 단색 rect는 픽셀 배열 대신
   "색 1바이트 + rect" op로. 콘솔/문서 배경에서 효과. 구현비용 소, 절감은 zlib이 이미
   단색을 잘 압축하므로 제한적 — 후순위.
7. **하드웨어 비디오 인코딩 (NVENC/QSV H.264)** — 업계 방향(RDP도 AVC444 모드)이지만
   8bpp palette+zlib PoC 범위 밖. 장기 과제로만 기록.

## 5. (2026-07-08 추가) move rect를 받기 위한 Desktop Duplication 설정 파라미터?

**결론: 없다. 수신(캡처) 측에서 move rect 생성을 켜는 파라미터는 존재하지 않는다.**

- `IDXGIOutput1::DuplicateOutput(device, &dup)` — 파라미터 없음.
- `IDXGIOutput5::DuplicateOutput1(device, Flags, formats...)` — Flags는
  `DXGI_OUTDUPL_FLAG` 비트필드인데, SDK 헤더(10.0.26100 / 10.0.28000 `dxgi1_5.h:93`)에
  정의된 값은 `DXGI_OUTDUPL_COMPOSITED_UI_CAPTURE_ONLY = 1` 하나뿐(보호 콘텐츠 UI
  캡처용). move rect와 무관.

move rect는 **생산자(프레임을 그리는 쪽) 주도**로만 만들어진다:

- 앱이 flip-model 스왑체인에서 `IDXGISwapChain1::Present1`에
  `DXGI_PRESENT_PARAMETERS.pScrollRect/pScrollOffset`으로 "이 영역은 이전 프레임에서
  스크롤됐다"고 **명시 선언**해야 OS가 그 사실을 안다. 공식 문서: *"The runtime also
  uses the scrolled rectangle to optimize presentation in terminal server and
  indirect display scenarios."* — 이 경로가 원격/중계 계층(move rect 포함)으로
  전달되는 스크롤 힌트의 근원이다. DISCARD/SEQUENTIAL 스왑 이펙트에서는 지원 안 됨.
- DWM은 픽셀 내용에서 스크롤을 **추론하지 않는다**. 따라서 Present1 스크롤 힌트를
  주지 않는 앱(사실상 대부분 — VirtualDub 개발자 Avery Lee: "I couldn't find
  anything that uses it yet")의 스크롤·창 이동은 dirty로만 보고된다.

### 본 리포지토리 실측 (Win10 19045, 하드웨어 GPU 로컬 데스크톱)

raw 메타데이터 레벨 계측(`GetFrameMoveRects`를 full-frame 여부와 무관하게 항상 호출)
후에도: notepad 스크롤 30회 / conhost `dir /s` 연속 스크롤 / 창 25px 단위 연속 이동
모두 **rawMoves = 0**. 이 환경에서 DXGI는 move rect를 발행하지 않는다(확정).
move rect가 나올 개연성이 있는 환경: 터미널 서버(RDP 세션 내 캡처), indirect
display — pScrollRect 문서가 명시한 최적화 대상 시나리오. (개연성 수준, 미실측)

### 시사점

copy op 인프라는 무비용이므로 유지하되, 이 환경의 실효 절감은 §4의 2·3번
(서버측 prev frame + 타일 검증, XOR delta)에서 온다.

## Source

1. [IDXGIOutputDuplication::GetFrameMoveRects — MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgioutputduplication-getframemoverects) — move→dirty 처리 순서 규약, 버퍼 시맨틱 (공식 1차)
2. [Desktop Duplication API — MS Learn](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api) — move rect의 설계 목적이 전송량 절감임을 명시, coalescing 동작, 공식 샘플 코드 (공식 1차)
3. [RFC 6143 §7.7.2 CopyRect](https://www.rfc-editor.org/rfc/rfc6143.html) — 좌표 4바이트 인코딩, 스크롤/창이동 용도, source 순서 제약 (표준 1차)
4. [MS-RDPEGFX RDPGFX_CMDID_SURFACETOSURFACE](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegfx/da5c75f9-cd99-450c-98c4-014a496942b0) — 모던 RDP의 복사 명령 PDU (공식 스펙 1차)
5. [MS-RDPEGDI Bitmap Caches](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpegdi/2bf92588-42bd-4527-8b3e-b90c56e292d2), [MS-RDPBCGR Persistent Bitmap Caches](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpbcgr/2da3e165-d1ba-4b65-8909-7a0f7f858d69) — 타일 캐시 설계 (공식 스펙 1차)
6. [WebRTC dxgi_output_duplicator.cc](https://github.com/pristineio/webrtc-mirror/blob/master/webrtc/modules/desktop_capture/win/dxgi_output_duplicator.cc) — move를 dirty로 flatten, unmoved move rect 필터 (소스코드 1차)
7. [Microsoft DXGIDesktopDuplication 샘플 DuplicationManager.cpp](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/DXGIDesktopDuplication/cpp/DuplicationManager.cpp) — 메타데이터 버퍼 처리 순서 (공식 샘플 1차)
8. [x11vnc man page](https://linux.die.net/man/1/x11vnc) — scrollcopyrect/wirecopyrect 휴리스틱 (1차 문서)
9. 로컬 소스: `simple/scapenc/dxgidup.cpp:128-158`, `simple/scapenc/scapenc.cpp:74-114`, `simple/scapdec/scapdec.cpp:140-159`, `simple/common/scap_packet.h`, `RC5XWIN/SCapEnc/desk/DesktopThread.cpp:475-507` (소스 1차)
10. Windows SDK `dxgi1_5.h:93-96` (10.0.26100/10.0.28000) — `DXGI_OUTDUPL_FLAG` 정의 확인 (1차, 소스 레벨)
11. [DXGI_PRESENT_PARAMETERS — MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/ns-dxgi1_2-dxgi_present_parameters) — pScrollRect의 terminal server/indirect display 최적화 명시 (공식 1차)
12. [IDXGIOutput5::DuplicateOutput1 — MS Learn](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_5/nf-dxgi1_5-idxgioutput5-duplicateoutput1) — Flags 파라미터 시맨틱 (공식 1차)
13. [VirtualDub: Screen recording with WDDM 1.2](https://www.virtualdub.org/blog2/entry_356.html) — scroll rect가 scroll-on-Present 대응이며 사용 앱을 못 찾았다는 실측 관찰 (평판 있는 3차)
14. 본 리포지토리 실측 (2026-07-08): raw 계측으로 3개 시나리오 rawMoves=0 확인 — `simple/scapenc/dxgidup.cpp` 진단 카운터, `moveRect.log`
15. 미검증/추정으로 명시한 항목: RDP 세션/indirect display에서의 move rect 발생 (개연성 있음, 미실측)
