---
title: "Zstd compression ratio and speed comparison"
source: "https://claude.ai/chat/3f92f733-a371-418f-88b4-a15dce7965bb"
author:
published:
created: 2026-07-08
description: "Claude conversation with 18 messages"
tags:
  - "clippings"
---
**You**

zlib default level(6)와 압축율, 속도 등을 비교해 주세요

---

**Claude**

컨테이너에서 직접 벤치마크를 돌려서 실측치로 비교해 드리겠습니다.

---

**You**

zstd-9에서도 오히려 압축속도가 더 빠른건가요?

---

**Claude**

네, 맞습니다. 위 실측에서 zstd -9 (82 MB/s)가 zlib -6 (36 MB/s)보다 **~2.3배 빠르면서** 압축률도 3.25 → 4.86으로 훨씬 높습니다. zstd -19까지 내려가야 비로소 zlib -6보다 느려집니다 (대신 ratio 5.36).

이게 가능한 이유:

- **엔트로피 코딩 차이**: zlib은 1990년대 설계의 Huffman 코딩인 반면, zstd는 FSE(Finite State Entropy, tANS 기반) + Huff0를 사용해 엔트로피 단계 자체가 훨씬 빠르고 압축 효율도 높습니다
- **현대 CPU 최적화**: 캐시 친화적 자료구조, 브랜치 최소화, 넓은 레지스터 활용 등 설계 자체가 최신 하드웨어 기준
- **큰 매치 윈도우**: zlib은 32KB 윈도우 제한이 있지만 zstd는 레벨에 따라 수 MB 윈도우를 써서 먼 거리 반복도 잡아냅니다

그래서 "zlib -6와 같은 CPU 예산이면 zstd는 -9~-12쯤 쓸 수 있다"는 게 실무적인 감각입니다. deflate/gzip 포맷 호환이 필요한 경우가 아니라면 zlib을 선택할 이유가 사실상 없습니다.

---

**You**

zstd의 압축 level을 각 시스템에 따라서 자동으로 최적의 level을 검출하는 방법은?

---

**Claude**

두 가지 접근이 있습니다: **부팅 시 캘리브레이션** + **런타임 적응형 조절**. zstd CLI의 `--adapt` 옵션이 후자의 레퍼런스 구현입니다.

### 1\. 목적함수 정의가 먼저

원격 데스크톱이면 "최적"은 보통 **엔드투엔드 지연 최소화** 입니다:

```
total_latency = size/bw + comp_time + decomp_time
```

대역폭(bw)이 크면 낮은 레벨(압축시간 절약), 좁으면 높은 레벨(전송량 절약)이 유리 — 즉 최적 레벨은 시스템+네트워크 함수입니다.

### 2\. 시작 시 캘리브레이션

```python
import time
import zstandard as zstd

def calibrate(sample: bytes, bandwidth_mbps: float,
              levels=(1, 2, 3, 5, 7, 9, 12)) -> int:
    bw = bandwidth_mbps * 1e6 / 8  # bytes/s
    best_lvl, best_lat = 1, float("inf")
    for lvl in levels:
        c = zstd.ZstdCompressor(level=lvl)
        t = time.perf_counter()
        out = c.compress(sample)
        comp_t = time.perf_counter() - t
        lat = comp_t + len(out) / bw
        if lat < best_lat:
            best_lat, best_lvl = lat, lvl
    return best_lvl
```

핵심: sample은 **실제 캡처 프레임의 dirty rect 타일** 이어야 합니다. Silesia 같은 범용 corpus로 재면 화면 데이터 특성(RGB 반복, 텍스트 렌더링 패턴)과 달라서 결과가 어긋납니다.

### 3\. 런타임 적응 (zstd --adapt 방식)

실제로는 CPU 부하·네트워크가 변하므로 피드백 루프가 더 실용적입니다. 원리는 송신 큐 상태 관찰:

```python
class AdaptiveLevel:
    def __init__(self, lo=1, hi=12):
        self.lvl, self.lo, self.hi = 3, lo, hi
        self.cctx = zstd.ZstdCompressor(level=3)

    def feedback(self, send_queue_depth: int, frame_budget_exceeded: bool):
        if frame_budget_exceeded:        # 압축이 병목 → 레벨 다운
            new = max(self.lo, self.lvl - 2)
        elif send_queue_depth > 2:       # 네트워크가 병목 → 레벨 업
            new = min(self.hi, self.lvl + 1)
        else:
            return
        if new != self.lvl:
            self.lvl = new
            self.cctx = zstd.ZstdCompressor(level=new)
```

- **압축 스레드가 놀고 송신 큐가 쌓임** → 네트워크 병목 → 레벨 올려 크기 축소
- **압축이 프레임 버짓(예: 60fps면 16ms)을 초과** → CPU 병목 → 레벨 내림
- C/C++이면 `ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, lvl)` 로 컨텍스트 재생성 없이 프레임 단위 변경 가능 (Python binding은 compressor 재생성이 필요하지만 비용 미미)

### 4\. 실무 팁

- 레벨 3~4 이후 ratio 증가는 완만하고 속도 하락이 가파르므로 탐색 범위는 1~12 정도면 충분, -19 같은 건 실시간엔 제외
- 히스테리시스(연속 N프레임 조건 유지 시에만 변경)를 넣어야 레벨이 진동하지 않습니다
- 콘텐츠 타입별로 다르게: 텍스트/UI 타일은 높은 레벨 효과 큼, 동영상/사진 영역은 이미 고엔트로피라 레벨 올려도 무의미 → 타일 엔트로피 추정(간단히 zstd -1 결과 크기)으로 분기하는 것도 방법입니다

칼리브레이션 + 적응 루프를 합친 완성형 모듈이 필요하면 만들어 드릴게요.

---

**You**

zstd를 사용할때 streaming스타일을 지원하는 코드: input buffer(예를들어 128kb) -> zstd -> output buffer(예를들어 16kb)

---

**Claude**

`zstandard` 의 **chunker API** 가 정확히 이 패턴입니다 — 입력을 임의 크기로 밀어 넣으면 출력이 고정 크기(16KB) 청크로 나옵니다.

### 압축: 128KB in → 16KB out

```python
import zstandard as zstd

IN_CHUNK = 128 * 1024
OUT_CHUNK = 16 * 1024

cctx = zstd.ZstdCompressor(level=3)
chunker = cctx.chunker(chunk_size=OUT_CHUNK)

def compress_stream(src, sink):
    while True:
        data = src.read(IN_CHUNK)          # 128KB 입력 버퍼
        if not data:
            break
        for out in chunker.compress(data): # 16KB 단위 출력
            sink(out)                       # len(out) == 16384 보장
    for out in chunker.finish():           # 잔여분 flush (마지막만 <16KB)
        sink(out)
```

`chunker.compress()` 는 내부적으로 16KB가 채워질 때까지 버퍼링하고, 채워진 청크만 yield합니다. 네트워크 패킷 크기를 고정하고 싶을 때 딱 맞습니다.

### 프레임 경계가 필요하면: flush()

원격 데스크톱처럼 "이 프레임까지는 지금 즉시 보내야" 하는 경우:

```python
for out in chunker.compress(frame_data):
    sink(out)
for out in chunker.flush():   # 버퍼에 남은 것 즉시 배출 (frame 유지, <16KB 가능)
    sink(out)
```

`flush()` 는 zstd 블록 경계를 강제해 디코더가 지금까지의 데이터를 전부 복원할 수 있게 합니다. `finish()` 와 달리 스트림은 계속 이어집니다.

### 해제 측: streaming decompress

```python
dctx = zstd.ZstdDecompressor()
dobj = dctx.decompressobj()

def on_recv(chunk: bytes):
    plain = dobj.decompress(chunk)  # 부분 입력 OK, 가능한 만큼 반환
    if plain:
        handle(plain)
```

### 저수준 제어가 필요하면 (C API 스타일)

버퍼를 직접 쥐고 싶으면 `compressobj` + 수동 슬라이싱, 또는 C의 `ZSTD_compressStream2` 패턴 그대로:

```c
ZSTD_inBuffer  in  = { src, src_size, 0 };
ZSTD_outBuffer out = { dst, 16*1024, 0 };
while (in.pos < in.size) {
    out.pos = 0;
    ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_continue);
    if (out.pos) send(dst, out.pos);   // 16KB 출력 버퍼 단위 배출
}
```

Python이면 chunker가 이 루프를 감싼 것이라 그대로 쓰시면 됩니다. 스트리밍 모드는 프레임 간 히스토리(윈도우)를 유지하므로, 타일 단위 독립 압축(`cctx.compress()`) 대비 연속 프레임에서 압축률이 상당히 좋아지는 부수 효과도 있습니다.

---

**You**

이제부터 zstd관련 요청은 c,c++관점에서만 애기해줘. 파이썬은 고려하지 않고 있어요

---

**Claude**

알겠습니다, 기억해 두겠습니다. 앞선 스트리밍 패턴을 C 관점으로 다시 정리하면:

```c
#include <zstd.h>

#define IN_CHUNK  (128 * 1024)
#define OUT_CHUNK (16 * 1024)

void compress_stream(FILE* fin, void (*sink)(const void*, size_t)) {
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3);

    char* in_buf  = malloc(IN_CHUNK);
    char* out_buf = malloc(OUT_CHUNK);

    size_t rd;
    while ((rd = fread(in_buf, 1, IN_CHUNK, fin)) > 0) {
        int last = feof(fin);
        ZSTD_EndDirective mode = last ? ZSTD_e_end : ZSTD_e_continue;
        ZSTD_inBuffer in = { in_buf, rd, 0 };
        int finished;
        do {
            ZSTD_outBuffer out = { out_buf, OUT_CHUNK, 0 };
            size_t remaining = ZSTD_compressStream2(cctx, &out, &in, mode);
            if (out.pos) sink(out_buf, out.pos);   // 최대 16KB 단위 배출
            finished = last ? (remaining == 0) : (in.pos == in.size);
        } while (!finished);
    }
    free(in_buf); free(out_buf);
    ZSTD_freeCCtx(cctx);
}
```

핵심 포인트:

- `ZSTD_compressStream2` 반환값은 "아직 flush 안 된 내부 데이터량" — `ZSTD_e_end` 모드에서 0이 될 때까지 루프
- 프레임 경계 없이 즉시 배출이 필요하면 `ZSTD_e_flush` 사용 (블록 경계 강제, 스트림은 유지)
- 권장 버퍼 크기는 `ZSTD_CStreamInSize()` / `ZSTD_CStreamOutSize()` 로 조회 가능하지만, 128KB/16KB처럼 임의 크기도 정상 동작합니다 — out이 작으면 내부적으로 여러 번 나눠 나올 뿐입니다
- 해제 측은 `ZSTD_decompressStream` 으로 동일 패턴, `ZSTD_DCtx` 하나를 재사용

반복 압축(프레임마다)이라면 `ZSTD_CCtx` 를 매번 생성하지 말고 재사용하세요 — 컨텍스트 재사용만으로도 소규모 데이터에서 성능 차이가 큽니다.

---

**You**

이렇게 스트리밍 방식으로 압축하는방법이 전체를 한꺼번에 압축하는 방식 대비 차이점이 있나요? 압축율이나 압축 시간등등

---

**Claude**

동일 데이터를 끝까지 넣고 `ZSTD_e_end` 한 번으로 마무리하면 **결과물과 압축률은 사실상 동일** 합니다. 차이는 다음 조건에서 생깁니다.

### 압축률 차이가 생기는 경우

**1\. flush 빈도** — 실질적으로 가장 큰 요인:

- `ZSTD_e_flush` 를 호출할 때마다 블록이 강제 종료됩니다. 매치 탐색은 블록을 넘어 이어지지만, 블록이 작아지면 블록 헤더 오버헤드 + FSE/Huffman 테이블 효율 저하로 압축률이 떨어집니다
- 원격 데스크톱처럼 프레임마다 flush하면 타일이 작을수록 손해가 커집니다 (수 KB 단위 flush면 수 % 이상)

**2\. srcSize 힌트**:

- one-shot(`ZSTD_compress2`)은 전체 크기를 알기 때문에 윈도우 크기를 입력에 맞춰 줄이고, single-segment 프레임으로 헤더를 아낍니다
- 스트리밍도 `ZSTD_CCtx_setPledgedSrcSize(cctx, total)` 를 미리 주면 동일한 최적화가 적용됩니다. 안 주면 기본 윈도우(레벨별 수 MB)를 가정 → 작은 입력에서 미세하게 불리

### 속도/메모리

|  | one-shot | streaming |
| --- | --- | --- |
| 압축 속도 | 기준 | 거의 동일 (~수 % 느림, 내부 버퍼 복사 때문) |
| 메모리 | 입력+출력 전체 상주 | 윈도우 버퍼만 (상수 메모리) |
| 지연 | 전체 완료 후 출력 | 진행 중 출력 가능 |

스트리밍의 복사 오버헤드는 `ZSTD_c_stableInBuffer` / `ZSTD_c_stableOutBuffer` 파라미터로 제거할 수 있습니다 (버퍼가 호출 간 불변임을 보장하는 조건).

### 실무 결론

- **파일/배치**: 메모리에 다 올라가면 one-shot이 단순하고 미세하게 빠름
- **실시간 파이프라인**: 스트리밍 + 프레임 단위 `ZSTD_e_flush`. 압축률 몇 % 손해는 히스토리 공유(이전 프레임을 사전처럼 활용) 이득이 훨씬 크므로, 타일별 독립 one-shot보다 오히려 압축률이 좋습니다
- 작은 데이터 반복이면 스트리밍 대신 **딕셔너리 + one-shot** (`ZSTD_compress_usingCDict`)도 비교해 볼 만합니다 — 히스토리 유지 없이 독립 프레임이 필요할 때(패킷 손실 복구 등) 유리합니다

---

**You**

원격지원 특성한 변경된 한 프레임은 꼭 flush되어야 합니다. 평균 10fps정도이고, vnc/rdp와 같이 변경된 화면만 tile diffing해서 zstd압축 해서 보내게 됩니다.

ZSTD\_e\_flush가 단순 zstd.compress() 대비 장단잠은?

---

**Claude**

이 시나리오(10fps, dirty tile만 전송)에서는 **스트리밍 + `ZSTD_e_flush` 가 거의 항상 승리** 합니다. 핵심 이유는 히스토리입니다.

### 장점: 크로스 프레임 히스토리

`ZSTD_e_flush` 는 블록만 닫고 **윈도우(히스토리)는 유지** 합니다. 즉 프레임 N의 타일을 압축할 때 프레임 N-1, N-2...의 데이터가 사전 역할을 합니다.

원격 화면 특성상 이 효과가 극대화됩니다:

- 같은 UI 요소가 반복 등장 (창 이동, 스크롤, 커서 주변 재그리기)
- 타일 diffing을 통과했어도 "변경된 타일" 내부에 이전 프레임과 유사한 패턴 다수
- 특히 스크롤: dirty rect는 크지만 내용은 이전 프레임에 거의 다 있음 → 히스토리 매치로 수십 배 압축

독립 `ZSTD_compress()` 는 매 프레임 히스토리 제로에서 시작합니다. 타일이 수십 KB 수준이면 "과거 없음" 페널티가 커서, 실측 시 스트리밍 대비 압축률이 보통 1.5~3배 나쁩니다 (콘텐츠에 따라 그 이상).

### 단점 / 주의점

**1\. 패킷 손실·재접속 시 복구 불가** — 가장 중요:

- 디코더 히스토리가 인코더와 1바이트라도 어긋나면 이후 전부 깨집니다
- TCP(또는 QUIC stream)처럼 신뢰성 있는 전송 위에서만 사용 가능. RDP/VNC도 TCP라 보통 문제없지만, 재접속 시 CCtx/DCtx 리셋 + 풀 프레임 재전송 로직이 필수입니다

**2\. flush로 인한 블록 단절 오버헤드**:

- 매 flush마다 블록 헤더(3바이트) + 엔트로피 테이블 재구축 비용. 10fps면 초당 10회라 무시할 수준이고, 히스토리 이득이 압도합니다

**3\. 메모리 상주**:

- 세션당 CCtx + 윈도우 버퍼(레벨별 수 MB)가 계속 유지됩니다. 다중 세션 서버면 세션 × 윈도우 크기를 계산해야 합니다. `ZSTD_c_windowLog` 로 제한 가능 (예: 20~22 = 1~4MB — 화면 데이터엔 충분)

**4\. 병렬화 제약**:

- 히스토리가 순차 의존이라 타일별 멀티스레드 압축이 안 됩니다. 10fps + 타일 diffing 후 데이터량이면 단일 스레드로 충분해서 실질 문제는 아닙니다

### 권장 구성

```c
ZSTD_CCtx* cctx = ZSTD_createCCtx();
ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3);
ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, 21);  // 2MB 히스토리 상한

// 매 프레임: dirty tile들을 이어서 밀어넣고
ZSTD_inBuffer in = { tiles, tiles_size, 0 };
do {
    ZSTD_outBuffer out = { obuf, sizeof(obuf), 0 };
    size_t rem = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_flush);
    send(out.buf, out.pos);
} while (in.pos < in.size /* + rem>0 확인 */);
```

정리: 신뢰성 전송 + 재접속 리셋 프로토콜만 갖추면 `ZSTD_e_flush` 스트리밍이 압축률에서 압도적이고, 단점은 전부 관리 가능한 수준입니다. 이게 사실상 RDP류 코덱 파이프라인의 표준 패턴입니다.

---

**You**

4k 화면 데이터에 적합한 ZSTD\_c\_windowLog값은?

---

**Claude**

추가 완료, 두 테스트 모두 통과했습니다.

```cpp
zs::StreamCompressor enc;
enc.setSink([&](const void* p, size_t n) { send(p, n); });

// 매 프레임:
for (auto& tile : dirty_tiles)
    enc.write(tile.data, tile.size);   // ZSTD_e_continue — 내부 버퍼링
enc.flush();                            // ZSTD_e_flush — 프레임 경계, 즉시 배출
```

동작 방식:

- `setSink()` 로 출력 콜백을 한 번 등록 → `write()` / `flush()` 는 인자 없이 사용
- `write()` 는 `Flush::None` 으로 누적 — 여러 타일을 이어 넣으면 zstd가 타일 경계를 넘어 매치를 찾으므로 타일별 flush보다 압축률이 좋습니다
- `flush()` 는 빈 입력 + `ZSTD_e_flush` — 호출 직후 상대가 지금까지의 모든 데이터를 복원 가능 (테스트에서 `flush()` 후 프레임 전체 복원을 assert로 검증)
- sink 미설정/음수 size는 예외. 기존 `compress()` / `compressFrame()` API는 그대로 유지

성능 팁 하나: sink가 소켓 직송이면 16KB 청크마다 syscall이 나가므로, sink 쪽에서 프레임 단위로 모아 `writev` 나 단일 send로 보내는 게 좋습니다.

---

**You**

요청한 인터페이스에 Sink도 arg로 넘겨주세요

---

**Claude**

변경 완료, 테스트 통과했습니다.

```cpp
void write(const void* data, int size, const Sink& sink);  // ZSTD_e_continue
void flush(const Sink& sink);                              // ZSTD_e_flush
```

사용:

```cpp
zs::Sink sink = [&](const void* p, size_t n) { send(p, n); };

for (auto& tile : dirty_tiles)
    enc.write(tile.data, tile.size, sink);
enc.flush(sink);
```

`setSink()` /멤버 sink는 제거해서 클래스는 zstd 컨텍스트 외 상태를 갖지 않습니다. 호출마다 다른 sink를 넘길 수도 있습니다 (예: 평소엔 소켓, 녹화 중엔 소켓+파일 tee).