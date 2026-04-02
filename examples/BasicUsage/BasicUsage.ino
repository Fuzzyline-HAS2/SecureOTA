/*
 * BasicUsage.ino — SecureOTA 라이브러리 사용 예제
 *
 * [필수 설정]
 * 1. secrets.h.example → secrets.h 로 복사 후 비밀키 입력
 * 2. 아래 URL 3개를 자신의 GitHub 저장소 주소로 변경
 * 3. scripts/secrets.py 의 HMAC_SECRET 도 동일하게 설정
 *
 * [아두이노 IDE 설정]
 * - Board           : ESP32 Dev Module (또는 사용 보드)
 * - Partition Scheme: Minimal SPIFFS (1.9MB APP with OTA)  ← 필수
 *
 * [배포]
 * python scripts/deploy.py  (매 업데이트마다)
 */

#include <WiFi.h>
#include <SecureOTA.h>
#include "secrets.h"   // HMAC_SECRET 정의 — gitignore 대상

// ============================================================
// 사용자 설정
// ============================================================

// 1. WiFi
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// 2. GitHub Raw URL — 자신의 <유저명>/<저장소명>으로 변경
//    형식: https://raw.githubusercontent.com/<유저명>/<저장소>/main/<파일>
const char* FIRMWARE_URL  = "https://raw.githubusercontent.com/YOUR_USER/YOUR_REPO/main/update.bin";
const char* VERSION_URL   = "https://raw.githubusercontent.com/YOUR_USER/YOUR_REPO/main/version.txt";
const char* SIGNATURE_URL = "https://raw.githubusercontent.com/YOUR_USER/YOUR_REPO/main/update.sig";

// 3. 현재 펌웨어 버전 — deploy.py 가 자동으로 증가시킵니다
#define FIRMWARE_VER 1

// ============================================================
// SecureOTA 인스턴스 생성
// ============================================================
SecureOTA ota(FIRMWARE_URL, VERSION_URL, SIGNATURE_URL, HMAC_SECRET, FIRMWARE_VER);

// OTA 성공 콜백 — 플래싱 완료 후 ESP.restart() 직전에 호출
// 서버의 device_state 를 "setting" 으로 되돌려줌
void onOtaSuccess() {
  Serial.println("[OTA] 업데이트 완료 — 서버에 setting 전송");
  // Firebase.setString(fbdo, "/device_state", "setting");
}

// OTA 스킵 콜백 — 이미 최신 버전일 때 호출
// 재부팅 후 서버가 아직 device_state=github 를 들고 있을 때
// setting 으로 되돌려주는 역할
void onOtaSkip() {
  Serial.println("[OTA] 최신 버전 확인 — 서버에 setting 전송");
  // Firebase.setString(fbdo, "/device_state", "setting");
}

// ============================================================
// setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n====================================");
  Serial.println("   SecureOTA BasicUsage 예제");
  Serial.println("====================================\n");

  // WiFi 연결
  Serial.printf("WiFi 연결 중: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi 연결 성공! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi 연결 실패 — OTA 비활성화");
  }

  // TelnetStream 사용 시 아래 주석 해제:
  // TelnetStream.begin(23);
  // ota.setLogStream(TelnetStream);

  // OTA 성공 콜백 — 펌웨어 교체 완료 후 서버에 setting 전송
  ota.setOnSuccess(onOtaSuccess);

  // OTA 스킵 콜백 — 이미 최신 버전일 때 서버에 setting 전송
  ota.setOnSkip(onOtaSkip);
}

void loop() {
  // 실제 프로젝트에서는 서버 데이터 수신 로직이 여기에 들어갑니다.
  // device_state == "github" 수신 시 아래를 호출하세요:
  //
  //   ota.check();
  //
  // 예시: DataChange() 함수에서 호출
  // void DataChange(String changed_var) {
  //   if (changed_var == "device_state" && device_state == "github") {
  //     ota.check();
  //   }
  // }
}
