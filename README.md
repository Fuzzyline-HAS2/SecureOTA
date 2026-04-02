# SecureOTA — ESP32 보안 OTA 라이브러리

HMAC-SHA256 서명 검증으로 위조 펌웨어를 차단하는 ESP32 OTA 업데이트 라이브러리입니다.  
GitHub에 펌웨어를 올리면 WiFi로 자동 업데이트됩니다.

---

## 특징

- **HMAC-SHA256 서명 검증** — 펌웨어 스트리밍 중 동시에 계산, 완료 후 비교
- **명령형 트리거** — 부팅 시 자동 실행 없이 서버 명령(`device_state = "github"`)으로만 실행
- **간단한 API** — 생성자에 URL·비밀키·버전만 전달, `ota.check()` 한 줄로 실행
- **TelnetStream 지원** — `setLogStream()` 으로 원격 로그 출력 추가 가능

---

## HMAC-SHA256 보안 원리

```
[배포 시 — 개발자 PC]

  update.bin (펌웨어) ──┐
                        ├─→ HMAC-SHA256 계산 ─→ update.sig (32바이트 서명)
  비밀키 (hmac_secret) ─┘

  GitHub 에 update.bin + update.sig 업로드


[업데이트 시 — ESP32 기기]

  1. update.sig (32바이트) 먼저 다운로드
  2. update.bin 스트리밍하면서 동시에 HMAC-SHA256 계산
  3. 계산값 vs 다운로드된 서명 비교
      일치 → Update.end(true) 플래싱 커밋
      불일치 → Update.abort()  ← 위조 펌웨어 즉시 차단
```

비밀키 1개로 모든 기기를 관리합니다. 비밀키는 **절대 GitHub에 올리면 안 됩니다** (`.gitignore` 자동 처리됨).

---

## 라이브러리 설치

### 방법 A — ZIP 설치 (권장)

1. GitHub 에서 **Code → Download ZIP**
2. 아두이노 IDE: **스케치 → 라이브러리 포함 → .ZIP 라이브러리 추가**

### 방법 B — 직접 복사

라이브러리 폴더에 복사:
```
Windows: C:\Users\<사용자>\Documents\Arduino\libraries\SecureOTA\
Mac/Linux: ~/Arduino/libraries/SecureOTA/
```

---

## 빠른 시작

### 1. 비밀키 파일 만들기

```bash
# 라이브러리 예제 폴더에서
cp examples/BasicUsage/secrets.h.example examples/BasicUsage/secrets.h

# 또는 내 프로젝트 스케치 폴더에서
cp /path/to/SecureOTA/examples/BasicUsage/secrets.h.example MySketch/secrets.h
```

`secrets.h` 편집:
```cpp
#define HMAC_SECRET "강력한_랜덤_비밀키_32자_이상"
```

`scripts/secrets.py` 편집 (동일한 값):
```python
HMAC_SECRET = "강력한_랜덤_비밀키_32자_이상"
```

### 2. 스케치에서 사용

```cpp
#include <WiFi.h>
#include <SecureOTA.h>
#include "secrets.h"       // HMAC_SECRET 정의 — gitignore 대상

// 현재 버전 (deploy.py 가 자동 증가)
#define FIRMWARE_VER 1

// GitHub Raw URL — 자신의 저장소 주소로 변경
SecureOTA ota(
  "https://raw.githubusercontent.com/MY_USER/MY_REPO/main/update.bin",
  "https://raw.githubusercontent.com/MY_USER/MY_REPO/main/version.txt",
  "https://raw.githubusercontent.com/MY_USER/MY_REPO/main/update.sig",
  HMAC_SECRET,
  FIRMWARE_VER
);

void setup() {
  Serial.begin(115200);
  WiFi.begin("SSID", "PASSWORD");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  // TelnetStream 동시 출력 (선택)
  // TelnetStream.begin(23);
  // ota.setLogStream(TelnetStream);
}

void loop() { }

// 서버에서 device_state = "github" 수신 시 DataChange() 에서 호출
void DataChange(String changed_var) {
  if (changed_var == "device_state" && device_state == "github") {
    ota.check();
  }
}
```

### 3. 아두이노 IDE 파티션 설정 (필수)

| 항목 | 값 |
|------|-----|
| Board | `ESP32 Dev Module` (또는 사용 보드) |
| Partition Scheme | **`Minimal SPIFFS (1.9MB APP with OTA)`** ← 필수 |

### 4. 첫 USB 플래싱

**Ctrl+U** 로 초기 펌웨어 업로드.

### 5. 이후 배포 — `python scripts/deploy.py`

```
① python scripts/deploy.py 실행
② 아두이노 IDE 에서 Ctrl+Alt+S (컴파일된 바이너리 내보내기)
③ 터미널 Enter → 자동으로 서명 + GitHub 푸시
④ 서버에서 device_state = "github" 전송
⑤ 시리얼 모니터에서 OTA 완료 확인
```

---

## deploy.py 사용자 설정

`scripts/deploy.py` 상단의 두 변수를 자신의 프로젝트에 맞게 변경합니다:

```python
# ── 프로젝트에 맞게 변경 ────────────────────────────────────
SKETCH_FILE   = os.path.join(BASE_DIR, "examples", "BasicUsage", "BasicUsage.ino")
VERSION_MACRO = "FIRMWARE_VER"
# ────────────────────────────────────────────────────────────
```

**예시 — HAS2 프로젝트에 적용 시:**
```python
SKETCH_FILE   = "C:/Users/ok/HAS2/HAS2.ino"   # 자신의 스케치 파일 경로
VERSION_MACRO = "FIRMWARE_VER"                  # 스케치 내 버전 매크로 이름
```

deploy.py 가 자동으로 처리하는 것:
- `#define FIRMWARE_VER X` 값 증가 (스케치 파일 수정)
- `update.bin` 서명 → `update.sig` 생성
- `version.txt`, `update.bin`, `update.sig`, 스케치 파일 → GitHub 푸시

---

## API

```cpp
// 생성자
SecureOTA ota(firmware_url, version_url, signature_url, hmac_secret, current_version);

// 추가 로그 스트림 (선택) — TelnetStream 등
ota.setLogStream(TelnetStream);

// OTA 확인 및 실행
// WiFi 연결 확인 → 서버 버전 확인 → 버전 불일치 시 서명 검증 후 업데이트
ota.check();
```

---

## 프로젝트 구조 (라이브러리)

```
SecureOTA/                        ← 라이브러리 루트 = GitHub 저장소
├── src/
│   ├── SecureOTA.h               # 클래스 선언
│   └── SecureOTA.cpp             # OTA 로직 구현
├── examples/
│   └── BasicUsage/
│       ├── BasicUsage.ino        # 사용 예제 (FIRMWARE_VER 매크로 포함)
│       └── secrets.h.example     # 비밀키 템플릿
├── scripts/
│   ├── deploy.py                 # 배포 자동화 (SKETCH_FILE 변수 수정 필요)
│   ├── sign_firmware.py          # HMAC-SHA256 서명 생성기
│   ├── secrets.py                # Python 비밀키 (gitignore)
│   └── secrets.py.example        # Python 비밀키 템플릿
├── library.properties            # Arduino 라이브러리 메타데이터
├── keywords.txt                  # 아두이노 IDE 문법 강조
├── .gitignore
├── README.md
├── update.bin                    # 최신 펌웨어 (GitHub 에 올라감)
├── update.sig                    # HMAC 서명 (GitHub 에 올라감)
└── version.txt                   # 서버 버전 (deploy.py 자동 관리)
```

---

## 자주 묻는 질문

**Q. 서명 검증 실패가 뜨는데 deploy.py 로 배포했어요.**  
→ `secrets.h`의 `HMAC_SECRET`과 `scripts/secrets.py`의 `HMAC_SECRET` 값이 다릅니다.  
→ 두 값을 동일하게 수정한 뒤 **USB로 재플래싱**하세요 (기기에 내장된 키가 바뀌어야 함).

**Q. "#define FIRMWARE_VER 를 찾을 수 없다"는 오류가 떠요.**  
→ `deploy.py` 의 `SKETCH_FILE` 경로가 자신의 스케치 파일을 가리키지 않습니다.  
→ `SKETCH_FILE` 을 자신의 `.ino` 파일 경로로 변경하세요.  
→ 스케치에 `#define FIRMWARE_VER 1` 줄이 있는지 확인하세요.

**Q. "Space Not Enough" 오류가 떠요.**  
→ Partition Scheme 을 `Minimal SPIFFS (1.9MB APP with OTA)` 로 변경 후 USB 재플래싱.

**Q. Ctrl+U 와 Ctrl+Alt+S 의 차이가 뭔가요?**  
→ `Ctrl+U`: 컴파일 + USB 업로드 (기기에 직접 플래싱)  
→ `Ctrl+Alt+S`: 컴파일 + `.bin` 파일만 로컬 저장 (USB 불필요, OTA 배포 시 사용)

**Q. update.bin 이 .gitignore 에 없는 게 맞나요?**  
→ 맞습니다. 기기가 GitHub Raw URL 로 다운로드하기 위해 반드시 올라가야 합니다.  
→ 보안은 파일 공개 여부가 아닌 HMAC 서명으로 보장됩니다.

**Q. 기기가 여러 대인데 비밀키는 몇 개 필요해요?**  
→ 1개입니다. 모든 기기에 같은 비밀키를 내장하고, 서명도 같은 키 하나로 합니다.
