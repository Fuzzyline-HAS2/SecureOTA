# ESP32 Secure OTA Update Template

GitHub에서 펌웨어를 관리하고 ESP32를 무선으로 업데이트하는 템플릿입니다.  
**HMAC-SHA256 서명 검증**으로 위조 펌웨어를 차단합니다.

---

## 이게 뭔가요?

ESP32를 USB 없이 원격으로 업데이트하는 시스템입니다.

```
개발자 PC:  코드 수정 → python scripts/deploy.py → GitHub에 자동 업로드
ESP32:      서버에서 "github" 명령 수신 → GitHub에서 새 펌웨어 다운로드 → 자동 업데이트
```

**최초 1회만 USB로 플래싱** — 이후 모든 업데이트는 WiFi로 원격 처리됩니다.

---

## HMAC-SHA256 보안이란?

### 왜 필요한가요?

GitHub는 누구나 접근할 수 있는 공개 저장소입니다.  
만약 URL만 알면 누구든 가짜 펌웨어를 업로드해 기기를 망가뜨릴 수 있습니다.  
HMAC-SHA256은 **"이 펌웨어가 진짜 개발자가 만든 것인지"** 를 수학적으로 검증합니다.

### 어떻게 동작하나요?

```
[배포 시 — 개발자 PC]

  update.bin (펌웨어) ──┐
                        ├─→ HMAC-SHA256 계산 ─→ update.sig (32바이트 서명)
  비밀키 (hmac_secret) ─┘

  GitHub에 update.bin + update.sig 업로드


[업데이트 시 — ESP32 기기]

  1. update.sig (32바이트) 먼저 다운로드
  2. update.bin 스트리밍하면서 동시에 HMAC-SHA256 계산
  3. 계산된 값 vs 다운로드된 update.sig 비교
      일치 → 플래싱 커밋 (Update.end)
      불일치 → 즉시 중단 (Update.abort)  ← 위조/변조 차단
```

### HMAC 비밀키란?

비밀키는 **개발자만 아는 임의의 문자열**입니다.  
같은 파일이라도 비밀키가 다르면 완전히 다른 서명이 나옵니다.

```
"my-secret-key" + update.bin → 서명 A
"wrong-key"     + update.bin → 서명 B  (완전히 다름)
```

- 비밀키 1개로 모든 기기를 관리합니다 (기기가 100대여도 비밀키 1개)
- 비밀키를 잃어버리면 모든 기기에 USB 재플래싱이 필요합니다
- **절대 GitHub에 올리면 안 됩니다** → `.gitignore`로 자동 차단됨

---

## 시작하기

### 1단계 — 저장소 만들기

GitHub에서 **"Use this template"** → **"Create a new repository"** 로 자신의 저장소 생성 후 Clone.

```bash
git clone https://github.com/내아이디/내저장소.git
cd 내저장소
```

> 저장소는 반드시 **Public** 이어야 합니다.  
> (GitHub Raw URL로 펌웨어를 다운로드하기 때문)

---

### 2단계 — 비밀키 설정

비밀키 파일 2개를 만들고 **동일한 값**으로 설정합니다.

**`SignedOTA/secrets.h` 생성:**
```bash
# example 파일을 복사
cp SignedOTA/secrets.h.example SignedOTA/secrets.h
```
파일을 열어 비밀키 값 변경:
```cpp
const char *hmac_secret = "여기에_강력한_비밀키_입력";
```

**`scripts/secrets.py` 생성:**
```bash
cp scripts/secrets.py.example scripts/secrets.py
```
파일을 열어 동일하게 변경:
```python
HMAC_SECRET = "여기에_강력한_비밀키_입력"  # secrets.h 와 완전히 동일해야 함
```

> **비밀키 권장 형식:** 영문 대소문자 + 숫자 + 특수문자 32자 이상  
> **예시:** `xK9#mP2@qL5vN8$wR3yT6uA1sB4cD7e`  
> WiFi 비밀번호와 다른 값을 사용하세요 (펌웨어 바이너리에서 추출 가능)

---

### 3단계 — WiFi 및 URL 설정

`SignedOTA/OTA_Config.h` 를 열어 수정합니다:

```cpp
// WiFi 설정 (HAS2_Wifi.h를 사용하는 경우 이 항목은 참고용)
const char *ota_ssid     = "내_WiFi_이름";
const char *ota_password = "WiFi_비밀번호";

// GitHub Raw URL — 내아이디/내저장소로 변경
const char *firmware_url  = "https://raw.githubusercontent.com/내아이디/내저장소/main/update.bin";
const char *version_url   = "https://raw.githubusercontent.com/내아이디/내저장소/main/version.txt";
const char *signature_url = "https://raw.githubusercontent.com/내아이디/내저장소/main/update.sig";
```

---

### 4단계 — 첫 USB 플래싱

아두이노 IDE에서 `SignedOTA/Main.ino` 열기 (또는 SignedOTA 폴더를 열면 자동으로 모든 .ino 파일이 로드됨).

아두이노 IDE 설정:

| 항목 | 값 |
|------|-----|
| Board | `ESP32 Dev Module` (또는 사용 중인 보드) |
| Partition Scheme | `Minimal SPIFFS (1.9MB APP with OTA)` ← **필수** |
| Upload Speed | `115200` |

**Upload (Ctrl+U)** 후 시리얼 모니터(115200 baud)에서 확인:

```
====================================
   ESP32 모듈형 시스템 시작
====================================
모든 모듈 초기화 완료!
```

> Partition Scheme 설정 오류 시 나중에 "Space Not Enough" 오류가 발생합니다.

---

### 5단계 — 배포 테스트

#### 5-1. deploy.py 실행

```bash
python scripts/deploy.py
```

```
🚀 OTA 배포 자동화 시작...
현재 버전: v2
🔼 버전 변경: v2 → v3

⏳ [행동 필요] 아두이노 IDE 에서 Ctrl+Alt+S (Export Compiled Binary) 를 실행하세요.
   완료되면 Enter 를 누르세요...
```

#### 5-2. 아두이노 IDE에서 바이너리 내보내기

아두이노 IDE 메뉴: **스케치 → 컴파일된 바이너리 내보내기** (단축키: `Ctrl+Alt+S`)

> 이 단계는 기기에 USB 업로드하는 것이 아닙니다.  
> `SignedOTA/build/` 폴더에 `.bin` 파일만 만드는 작업입니다.

#### 5-3. 터미널에서 Enter

컴파일이 완료되면 터미널로 돌아와 **Enter** 를 누릅니다.

```
🔎 빌드 파일 탐색 중...
   발견: SignedOTA/build/.../SignedOTA.ino.bin
📦 → update.bin 복사 완료
🔏 서명 완료 → update.sig (32 bytes)
☁️ GitHub 에 업로드 중...
✅ GitHub 업로드 완료!

🎉 배포 완료! v3 이(가) GitHub 에 업로드되었습니다.
   이제 서버에서 device_state = "github" 를 전송하면 기기가 업데이트됩니다.
```

#### 5-4. OTA 트리거

서버에서 `device_state = "github"` 값을 전송합니다.  
(또는 DataChange() 함수가 연결된 방식으로 트리거)

시리얼 모니터에서 확인:

```
[DataChange] device_state 변경 감지

[OTA] OTA 업데이트 트리거됨 (device_state=github)
[OTA] 서버 버전 확인 중...
[OTA] 서버 v3 / 현재 v2
[OTA] 버전 불일치 (현재 v2 → 서버 v3) — 펌웨어 업데이트 시작
[OTA] 서명 파일 다운로드 중...
[OTA] 서명 파일 다운로드 완료
[OTA] 펌웨어 스트리밍 + HMAC 계산 시작...
[OTA] 서명 검증 중...
[OTA] 서명 검증 완료 — 펌웨어 신뢰
[OTA] OTA 완료! 3초 후 재부팅...
```

재부팅 후 버전이 v3으로 바뀌어 있으면 성공입니다.

---

## 이후 업데이트 방법

코드를 수정한 뒤:

```bash
python scripts/deploy.py
```

한 줄이면 끝입니다. USB 연결 없이 WiFi로 업데이트됩니다.

---

## DataChange() 연동 방법

실제 프로젝트에서 서버 데이터 변경 콜백이 있는 파일(예: `HAS2App.ino`)에 아래 분기를 추가합니다:

```cpp
void DataChange(String changed_var) {
  if (changed_var == "device_state") {
    if (device_state == "github") {
      checkOTA();  // OTA 업데이트 시작
    }
    // 다른 device_state 처리...
  }
}
```

`checkOTA()` 는 내부적으로:
1. WiFi 연결 상태 확인 (미연결 시 스킵)
2. GitHub 서버 버전 확인
3. 버전이 같으면 스킵, 다르면 `execOTA()` 실행

---

## 프로젝트 구조

```
프로젝트/
├── SignedOTA/                    ← Arduino 스케치 폴더
│   ├── Main.ino                  # setup / loop, DataChange() 스텁
│   ├── ota.ino                   # OTA 로직 (수정 불필요)
│   ├── OTA_Config.h              # WiFi / URL / 버전 / TLOG 매크로 설정
│   ├── secrets.h                 # 비밀키 (gitignore — GitHub 미업로드)
│   └── secrets.h.example         # 비밀키 템플릿 (이것만 GitHub에 올라감)
├── scripts/
│   ├── deploy.py                 # 배포 자동화 (버전증가 → 서명 → 푸시)
│   ├── sign_firmware.py          # HMAC-SHA256 서명 생성기
│   ├── secrets.py                # 비밀키 (gitignore — GitHub 미업로드)
│   └── secrets.py.example        # 비밀키 템플릿 (이것만 GitHub에 올라감)
├── update.bin                    # 최신 펌웨어 (GitHub에 올라감 — 기기가 다운로드)
├── update.sig                    # HMAC 서명 (GitHub에 올라감 — 기기가 검증)
└── version.txt                   # 현재 버전 번호 (deploy.py가 자동 관리)
```

---

## PC를 바꾸거나 새로 세팅할 때

```bash
git clone https://github.com/내아이디/내저장소.git
cd 내저장소

# 비밀키 파일 복원 (비밀번호 관리자 등에서 꺼내서 입력)
cp SignedOTA/secrets.h.example SignedOTA/secrets.h
cp scripts/secrets.py.example scripts/secrets.py
# 두 파일 모두 이전에 사용하던 비밀키로 수정
```

> 비밀키를 잊어버리면 모든 기기에 USB 재플래싱이 필요합니다.  
> **비밀번호 관리자(1Password, Bitwarden 등)에 반드시 보관하세요.**

---

## 자주 묻는 질문

**Q. 서명 검증 실패가 뜨는데 deploy.py로 배포했어요.**  
→ `secrets.h`의 `hmac_secret`과 `secrets.py`의 `HMAC_SECRET` 값이 다릅니다.  
→ 두 값을 동일하게 수정한 뒤 USB로 재플래싱하세요.

**Q. "서명 크기 오류: X bytes" 가 떠요.**  
→ GitHub에 `update.sig`가 없거나 파일이 깨진 경우입니다.  
→ `deploy.py`를 다시 실행하세요.

**Q. 버전 확인 실패, OTA가 안 돼요.**  
→ GitHub CDN 반영 지연입니다. 30초~1분 후 재시도하세요.

**Q. "Space Not Enough" 오류가 떠요.**  
→ Partition Scheme 설정이 잘못됐습니다.  
→ `Minimal SPIFFS (1.9MB APP with OTA)` 로 변경 후 USB 재플래싱하세요.

**Q. 기기가 여러 대인데 비밀키는 몇 개 필요해요?**  
→ 1개입니다. 모든 기기에 같은 비밀키가 내장되고, 서명도 같은 키로 합니다.

**Q. update.bin이 .gitignore에 없는 게 맞나요?**  
→ 맞습니다. `update.bin`과 `update.sig`는 GitHub에 올라가야 기기가 다운로드할 수 있습니다.  
→ 보안은 파일을 숨기는 게 아니라 HMAC 서명으로 합니다. 펌웨어를 탈취해도 비밀키 없이는 위조 불가합니다.

**Q. Ctrl+U와 Ctrl+Alt+S 의 차이가 뭔가요?**  
→ `Ctrl+U` (Upload): 컴파일 후 USB로 기기에 직접 플래싱  
→ `Ctrl+Alt+S` (Export Compiled Binary): 컴파일 후 `.bin` 파일만 로컬에 저장 (USB 불필요)  
→ OTA 배포 시에는 `Ctrl+Alt+S`로 `.bin` 파일을 만들고, `deploy.py`가 서명 후 GitHub에 올립니다.
