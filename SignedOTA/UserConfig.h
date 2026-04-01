#ifndef USER_CONFIG_H
#define USER_CONFIG_H

// ==========================================
// 사용자 설정 (USER CONFIGURATION)
// ==========================================

// 1. 와이파이 설정
const char *ssid = "SK_DA20_2.4G";
const char *password = "GGA48@6587";

// 2. 펌웨어 다운로드 주소
const char *firmware_url = "https://raw.githubusercontent.com/Fuzzyline-HAS2/"
                           "TTGO_update_test/main/update.bin";

// 버전 정보 파일 URL (version.txt)
const char *version_url = "https://raw.githubusercontent.com/Fuzzyline-HAS2/"
                          "TTGO_update_test/main/version.txt";

// 서명 파일 URL (update.sig) - 32바이트 HMAC-SHA256 서명
const char *signature_url = "https://raw.githubusercontent.com/Fuzzyline-HAS2/"
                            "TTGO_update_test/main/update.sig";

// 3. HMAC 서명 비밀키 (secrets.h에서 관리 — GitHub에 올라가지 않음)
#include "secrets.h"

// 4. 디버그 및 버전 정보
#define CURRENT_FIRMWARE_VERSION 2

// ==========================================
#endif
