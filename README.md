# AdLcom 패치판
A AdLib Driver for serial port.

이 adlcom 패치판은 adlipt를 시리얼포트용으로 패치한 adlcom을 쉐도우 레지스터 캐시 최적화한 패치판입니다.
시리얼널모뎀케이블을 통해서 PC에서 재생할 수 있고, 혹은 MCU를 이용한 에뮬을 통해서 재생가능합니다.
저는 esp32-s3에 ymfm코어를 올려서 재생할 것입니다. 아마도 opl3duo 도 가능할 것으로 보입니다만, 제게는 없어서 모르겠습니다.
실행은 adlcom opl3 로 하시면 되고, 경우에 따라서 adlcom opl3 nopatch로도 사용해 보시기 바랍니다.
이 프로그램은 AI를 이용하여 제작되었습니다.






<img width="1199" height="1519" alt="esp_ymfm" src="https://github.com/user-attachments/assets/7f283c5a-6328-4a1d-a0d5-45ce7220ee77" />

저항과 콘덴서는 안정성을 올리기 위한 것으로 없으면 생략하셔도 됩니다.



# adlcom ElectricRay Patch 수정 내용 정리

> 원본: josephillips85/adlcom (GitHub 공식 릴리즈)  
> 패치: ElectricRay (https://cafe.naver.com/olddos)  
> 빌드 환경: Open Watcom 1.9 (필수 — 2.0 이상은 세그먼트 구조 문제로 동작 안 함)  
> 대상 하드웨어: Samsung Sens P30 (16550A UART, Windows 9x EMM386)

---

## 빌드 환경 변경

### 원본
- `build.sh` (Linux/Unix 셸 스크립트)
- 빌드 컴파일러 버전 명시 없음

### 패치
- `build19.bat` (Windows 배치 파일) 추가
- **Open Watcom 1.9** 고정 사용 (`C:\watcom19`)

### 이유
Open Watcom 2.0으로 빌드하면 `RESIDENT` 세그먼트와 `_TEXT` 세그먼트가 물리적으로 분리된다.
EMM386의 `AX=4A15` (I/O 가상화) 호출 시 콜백 주소가 단일 세그먼트(CS=0x0000)를 기준으로 계산되는데,
세그먼트가 분리되면 핸들러 주소 계산이 틀어져 즉시 크래시가 발생한다.

OW 1.9는 `order` 지시어에 따라 모든 CODE 세그먼트를 `0000:xxxx` 단일 세그먼트로 묶어준다.

```
OW 2.0 빌드 (실패):
  RESIDENT: 0000:0008
  _TEXT:    0060:000A  ← 분리됨 → EMM386 크래시

OW 1.9 빌드 (성공):
  RESIDENT: 0000:0008
  _TEXT:    0000:040C  ← 단일 세그먼트 연속 → 정상 동작
```

---

## 파일별 수정 내용

### 1. `res_glue.s`

#### 1-1. `shadow_regs` 정상 할당

```asm
/* 원본 (공식 소스에는 없던 것) */
_shadow_regs:

/* 패치 */
_shadow_regs:
        db 64 dup (0FFh, 0FFh, 0FFh, 0FFh, 0FFh, 0FFh, 0FFh, 0FFh)
        ; = 512 bytes, 0xFF로 초기화
```

**이유**: `res_opl2.c`의 shadow cache 로직이 bank*256 + reg 방식으로
0~511 인덱스를 사용한다. 1바이트 할당 상태에서는 OOB write → 메모리 오염 → 크래시.
0xFF 초기화는 첫 write 시 반드시 cache miss가 되도록 보장한다.

---

#### 1-2. `emulate_opl3_write_high_address` bank 번호 수정

```c
/* 원본 */
WRITE_REG_NUKE(ax, 0, com_port);  /* bank=0 (잘못됨) */

/* 패치 */
WRITE_REG_NUKE(ax, 1, com_port);  /* bank=1 (high bank, 올바름) */
```

**이유**: OPL3 high bank(포트 0x38A/0x38B)에 대한 레지스터 write는
Nuke.YKT 프로토콜에서 bank=1로 전송해야 한다.
bank=0으로 전송하면 OPL3 칩이 low bank 레지스터로 잘못 인식한다.

---

### 2. `res_opl2.c`

#### 2-1. shadow register cache 추가 (핵심 최적화)

```c
/* 원본 */
else {
    WRITE_VAL_NUKE(ax, com_port);
}

/* 패치 */
else {
    /* Shadow register cache: 0x00~0x9F (오퍼레이터 레지스터)만 캐싱
     * 0xA0~0xFF (채널/KON/F-Num)은 bypass → 항상 전송 */
    int reg_lo = (int)(opl3Reg & 0xFF);
    if (reg_lo < 0xA0) {
        int current_bank = (address & 0x100) ? 1 : 0;
        int cache_idx = (current_bank * 256) + reg_lo;
        if (shadow_regs[cache_idx] == (unsigned char)ax) {
            return ax;  /* 중복 전송 skip */
        }
        shadow_regs[cache_idx] = (unsigned char)ax;
    }
    WRITE_VAL_NUKE(ax, com_port);
}
```

**이유**: 시리얼 대역폭(115200 baud = 최대 3,840 writes/sec)이 제한적이므로
값이 변하지 않은 레지스터의 중복 전송을 제거해 실질 처리량을 높인다.

캐시 적용 범위:
| 범위 | 내용 | 처리 |
|------|------|------|
| `0x00~0x9F` | 오퍼레이터 레지스터 (MULT, KSL, AR/DR, SL/RR 등) | 캐시 → 중복 skip |
| `0xA0~0xFF` | 채널 레지스터 (F-Num, KON, Rhythm, FB/ALG) | bypass → 항상 전송 |

채널 레지스터를 bypass하는 이유: F-Num, KON은 노트마다 변하며
캐시 hit 오판 시 노트가 울리지 않는 치명적 오류가 발생한다.

---

#### 2-2. UART 전송 방식 변경: 고정 딜레이 → THRE 폴링

```c
/* 원본 */
outp(port, sendBuffer[0]);
delay(port, DELAY_SERIAL_DATA);   /* 35회 고정 IN 루프 */
outp(port, sendBuffer[1]);
delay(port, DELAY_SERIAL_DATA);

/* 패치 */
while (!(inp(port + 5) & 0x20));  /* THRE(LSR bit5) 폴링 */
outp(port, sendBuffer[0]);
while (!(inp(port + 5) & 0x20));
outp(port, sendBuffer[1]);
```

동일한 변경이 적용된 위치:
- `WRITE_VAL_NUKE()` — OPL 레지스터 값 전송
- `emulate_opl2_write_address()` — 레지스터 번호 전송 후
- `emulate_opl3_write_high_address()` — high bank 레지스터 번호 전송 후
- `hw_reset()` — 초기화 시 전송

**이유**: 원본의 `delay(port, 35)`는 35회 IN 명령을 실행하는 고정 루프다.
CPU 속도에 따라 실제 대기 시간이 달라지며, 빠른 CPU에서는 UART가 준비되기 전에
다음 byte를 전송해 데이터 유실이 발생한다.
반면 느린 CPU에서는 불필요한 대기로 처리량이 낮아진다.

THRE 폴링은 UART의 Transmitter Holding Register가 실제로 비워졌을 때만
다음 byte를 전송하므로, CPU 속도와 무관하게 안정적으로 동작한다.

---

## 수정 파일 목록 요약

| 파일 | 수정 여부 | 주요 변경 내용 |
|------|-----------|----------------|
| `res_opl2.c` | ✅ | shadow cache 추가, THRE 폴링, bank 수정 |
| `res_glue.s` | ✅ | shadow_regs 512B 정상 할당 |
| `cmdline.c` | ❌ | 원본 그대로 |
| `cmdline.h` | ❌ | 원본 그대로 |
| `cmdline.rl` | ❌ | 원본 그대로 |
| `cputype.h` | ❌ | 원본 그대로 |
| `resident.h` | ❌ | 원본 그대로 |
| `jadlcom.c` | ❌ | 원본 그대로 |
| `jadlcom.wl` | ❌ | 원본 그대로 |
| `jlm.h` | ❌ | 원본 그대로 |
| `patchpe.py` | ❌ | 원본 그대로 |
| `adlcom.wl` | ✅ | libpath 추가 (OW 1.9 경로) |
| `build19.bat` | 🆕 | OW 1.9용 Windows 빌드 스크립트 신규 추가 |

---

## 빌드 방법

### 필수 환경
- **Open Watcom 1.9** (`C:\watcom19`에 풀 설치)
- Windows (build19.bat 사용)
- Python (patchpe.py 실행용)

### wlsystem.lnk 경로 패치 (최초 1회)

OW 1.9의 wlink는 `%WATCOM%/lib...` 형태의 경로를 Windows에서 제대로 찾지 못한다.
아래 PowerShell 명령으로 절대 경로로 교체해야 한다:

```powershell
# binnt\wlink.lnk 패치
$c = Get-Content "C:\watcom19\binnt\wlink.lnk" -Raw
$c = $c -replace '@%watcom%\\binw\\wlsystem.lnk','@C:\watcom19\binw\wlsystem.lnk'
Set-Content "C:\watcom19\binnt\wlink.lnk" $c -Encoding ASCII

# binw\wlsystem.lnk 패치
$c = Get-Content "C:\watcom19\binw\wlsystem.lnk" -Raw
$c = $c -replace '%WATCOM%/','C:\watcom19\'
$c = $c -replace '%watcom%/','C:\watcom19\'
Set-Content "C:\watcom19\binw\wlsystem.lnk" $c -Encoding ASCII
```

### 빌드 실행

```bat
cd C:\Users\tiram\Documents\claude\adlcom_electricray
build19.bat
```

---

## 사용법

```bat
adlcom COM1 opl3
```

옵션:
- `nopatch`: 게임의 OPL status read(`IN AL,DX`)를 NOP으로 패치하지 않음.  
  일부 게임/재생기에서 자연스러운 write 간격이 생겨 음질이 향상될 수 있음.

---

## 알려진 한계

- 115200 baud 한계로 매우 빠른 OPL3 음악에서 일부 드럼/타이밍 불안정 가능
- shadow cache 미적용 레지스터(0xA0~0xFF)는 항상 전송되므로 대역폭 소모
- ESP32 + ymfm 기반 OPL3 에뮬레이터 조합에서 검증됨



## Copying

Copyright © 2023 Jose Phillips
Copyright © 2020 Peter De Wachter (pdewacht@gmail.com)

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
