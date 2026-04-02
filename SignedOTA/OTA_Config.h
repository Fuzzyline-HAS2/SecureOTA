#ifndef OTA_CONFIG_H
#define OTA_CONFIG_H

// ============================================================
// OTA_Config.h — HMAC-SHA256 보안 OTA 설정
// ============================================================

// ------------------------------------------------------------
// 1. WiFi 설정
//    HAS2_Wifi.h 가 전역으로 ssid / password 를 선언하므로
//    이 파일에서는 ota_ssid / ota_password 로 선언 (충돌 방지)
//    checkOTA() 는 HAS2 가 WiFi를 연결한 이후에 호출됨
// ------------------------------------------------------------
const char *ota_ssid     = "YOUR_WIFI_SSID";
const char *ota_password = "YOUR_WIFI_PASSWORD";

// ------------------------------------------------------------
// 2. GitHub Raw URL
//    update.bin · update.sig · version.txt 는 저장소 루트에 위치
//    형식: https://raw.githubusercontent.com/<유저명>/<레포명>/main/<파일명>
// ------------------------------------------------------------
const char *firmware_url  = "https://raw.githubusercontent.com/"
                            "Fuzzyline-HAS2/TTGO_OTA_update_template/main/update.bin";

const char *version_url   = "https://raw.githubusercontent.com/"
                            "Fuzzyline-HAS2/TTGO_OTA_update_template/main/version.txt";

const char *signature_url = "https://raw.githubusercontent.com/"
                            "Fuzzyline-HAS2/TTGO_OTA_update_template/main/update.sig";

// ------------------------------------------------------------
// 3. HMAC 서명 비밀키
//    secrets.h 는 .gitignore 대상 — GitHub에 올라가지 않음
//    secrets.h.example 을 secrets.h 로 복사 후 값 변경
// ------------------------------------------------------------
#include "secrets.h"

// ------------------------------------------------------------
// 4. 펌웨어 버전 (deploy.py 가 자동 증가)
// ------------------------------------------------------------
#define CURRENT_FIRMWARE_VERSION 2

// ------------------------------------------------------------
// 5. TLOG 매크로 — Serial + TelnetStream 동시 출력
//    TelnetStream 라이브러리 필요: https://github.com/jandrassy/TelnetStream
//    HAS2 메인 헤더에 이미 정의되어 있으면 이 블록은 무시됨
// ------------------------------------------------------------
#ifndef TLOG
#include <TelnetStream.h>
#define TLOG(...)   do { Serial.print(__VA_ARGS__);   TelnetStream.print(__VA_ARGS__);   } while(0)
#define TLOGLN(...) do { Serial.println(__VA_ARGS__); TelnetStream.println(__VA_ARGS__); } while(0)
#define TLOGF(...)  do { Serial.printf(__VA_ARGS__);  TelnetStream.printf(__VA_ARGS__);  } while(0)
#endif

// ============================================================
#endif // OTA_CONFIG_H
