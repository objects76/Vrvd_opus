# simple — Desktop Duplication → 256-color → zlib loopback PoC

레거시 `RC5XWIN\ScapEnc` / `RC5XWIN\SCapDec2`에서 **ZipEnc 경로만** 추려 새로 작성한 최소 모듈.
목적: DesktopDuplication으로 캡처한 화면을 256컬러+zlib으로 압축 전송하고, 해제해서
변경된 화면이 잘 오는지 확인하는 것. 그 외 기능(JPEG/Jex/LZO/커서/타일캐시 등)은 없음.

## 구성

| 폴더 | 산출물 | 내용 |
|---|---|---|
| `scapenc` | scapenc.dll | DXGI Desktop Duplication(`IDXGIOutput1::DuplicateOutput`, BGRA32) → 변경 검증(`dirty_verify.h`: DXGI move/dirty를 prev 프레임과 블록 비교해 실제 변경 픽셀만 남김 — 블록 크기는 레거시 `check_changed_bits`의 적응형 휴리스틱, rect 폭 ≥32→64px / 16~31→16px / <16→8px, 8px 셀 그리드로 병합; 과대 보고 프레임은 NOCHANGE로 탈락) → RGB332 + Bayer 4x4 dithering으로 8bpp 변환 → 프레임 payload 전체를 메모리에 모은 뒤 `compress2` 1회. 새 뷰어 접속 시 `ScapEnc_RequestFullFrame()`으로 전체 프레임 재전송 |
| `scapdec` | scapdec.dll | `uncompress` → rect별 8bpp DIBSection 캔버스 blit → `BitBlt` 페인트 |
| `common` | (헤더) | wire format(`scap_packet.h`), 양자화 어댑터(`scap_palette.h` — 사용처는 이것만 include, `ScapQuantInit`/`ScapQuantPixel` 공통 인터페이스). 백엔드 선택: 기본은 RGB332+Bayer dither(`scap_332dither.h`, 디더 패턴은 프레임 절대좌표 고정이라 결정적 — `research/2026-07-08-32bpp_RGB_이미지를_256컬러로_변환하기.md` 참조; rect 변환은 런타임 CPU 감지로 AVX2 경로 자동 사용, 미지원 CPU는 스칼라 폴백, 출력은 byte-exact 동일), `scap_palette.h`의 `SCAP_USE_256MAP`을 켜면 레거시 240색 팔레트+LUT(`scap_256map.h`) |
| `zlib` | zlib.lib | 공식 zlib 1.3.1 소스 vendoring (compress2/uncompress에 필요한 파일만) |
| `test` | test.exe | 루프백: 캡처→인코딩→디코딩→창 표시. 타이틀바에 fps/패킷 크기/상태 |
| `streamserver` | streamserver.exe | 콘솔 서버: scapenc 캡처 패킷을 TCP 44300으로 스트리밍(길이 프리픽스 프레이밍). 한 번에 한 클라이언트 |
| `viewer` | viewer.exe | GUI 클라이언트: streamserver에 접속→scapdec 디코딩→창 표시. `test`의 루프백을 네트워크로 분리한 것 |

`common/scap_stream.h`가 서버·클라이언트 공통 프레이밍(`[u32 len][len bytes: scap 패킷]`)과 소켓 헬퍼를 담는다. scap 패킷은 디코딩엔 자기서술적이지만 헤더에 압축(on-wire) 길이가 없어 스트림에서 경계를 못 잡으므로 길이 프리픽스가 필요하다.

역방향(뷰어→서버) 마우스 입력은 같은 소켓에 고정 크기 `ScapInputMsg`로 흐른다(프레임과 방향이 달라 공존). 좌표는 뷰어가 캔버스(=서버 데스크톱) 크기로 0~65535 정규화해 보내고, 서버는 `SendInput(MOUSEEVENTF_ABSOLUTE)`로 그대로 주입한다(서버는 해상도를 몰라도 됨). 지원: move, L/R down·up, wheel. 뷰어는 좌표계를 물리 픽셀(DXGI 캡처 기준)로 맞추려고 `SetProcessDPIAware`를 호출한다 — 없으면 배율 디스플레이에서 클릭 위치가 어긋난다.

## 빌드

Visual Studio 2026 (v145 툴셋), Windows SDK 10.x. Debug/Release × Win32/x64.

```
msbuild simple\simple.sln -p:Configuration=Release -p:Platform=x64
```

산출물: `simple\bin\<Platform>\<Configuration>\` (test.exe와 DLL이 같은 폴더).

## 실행/검증

- `test.exe` — 주모니터의 256컬러 미러 창. 창 드래그 시 잔상 없음(매 프레임 전체
  CopyResource라 stale 픽셀 없음), UAC/잠금 후 자동 복구, 해상도 변경 시 캔버스
  재생성 확인. DXGI move rect는 픽셀 재전송 대신 copy op(VNC CopyRect 상당,
  12바이트 좌표)로 전송되어 스크롤/창 이동 시 패킷이 크게 줄어든다 —
  `research/2026-07-08-move-rect-packet-reduction.md` 참조.
- `test.exe -selftest` — 비대화형 파이프라인 검증(캡처 1프레임 → 디코딩 → 크기/영역
  일치 확인). exit code 0 = 통과.
- `test.exe -packettest` — 디코더 와이어포맷 단위 테스트(DXGI 불필요): copy op의
  순서 보존·겹침(스크롤) 방향·dirty와의 적용 순서·경계/절단 패킷 거부를 손으로 만든
  패킷과 레퍼런스 모델 비교로 검증. exit code 0 = 통과, 10+N = 케이스 N 실패.

### 스트리밍 (server ↔ viewer)

```
streamserver.exe            # TCP 44300에서 대기, 접속한 뷰어에 캡처 패킷 스트리밍
viewer.exe [host]           # host(기본 127.0.0.1):44300 접속 → 화면 표시
```

- 같은 PC: `streamserver.exe` 실행 후 `viewer.exe` 실행 → 뷰어 창에 미러 화면.
  다른 PC: 서버 IP를 인자로 `viewer.exe 192.168.x.y`.
- `streamserver.exe -selftest` — scapenc 없이 프레이밍 왕복만 검증(알려진 버퍼를
  루프백 소켓으로 `ScapSendFrame`→`ScapRecvFrame`, 바이트 일치 확인). exit code 0 = 통과.
  캡처 파이프라인은 `test.exe -selftest`가 담당.

## Wire format

```
Packet = ScapFrameHdr(16B) + compress2 blob
ScapFrameHdr: u32 magic 'SCP1' | u16 width | u16 height | u16 rectCount | u16 reserved | u32 rawSize
Payload(해제 후) = rectCount × { u16 x,y,w,h + u8 pixels[w*h] }   // 8bpp, row padding 없음
```

팔레트는 양쪽이 아는 고정 테이블이라 전송하지 않음. 레거시 packet/스트리밍 deflate
포맷과 호환되지 않음(의도된 재설계).
