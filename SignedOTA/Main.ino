/*
 * Main.ino — 모듈형 ESP32 시스템 진입점
 *
 * 이 파일은 프로젝트의 유일한 setup() · loop() 를 포함합니다.
 * 모든 모듈은 init / update 패턴을 따르며, 여기서 호출됩니다.
 *
 * [OTA 트리거 방식]
 * - 부팅 시 자동 OTA 실행 안 함
 * - 서버에서 device_state = "github" 수신 시 DataChange() 에서 checkOTA() 호출
 * - WiFi 는 HAS2_Wifi.h 가 관리 (이 파일에서 WiFi 초기화 안 함)
 *
 * [새 모듈 추가 방법]
 * 1. NewModule.ino 파일 생성
 * 2. initNewModule() / updateNewModule() 정의
 * 3. 아래 setup() 에 initNewModule() 추가
 * 4. 아래 loop() 에 updateNewModule() 추가
 */

// HAS2_Wifi.h 가 있는 프로젝트에서는 아래 include 활성화
// #include "HAS2_Wifi.h"

void setup() {
  Serial.begin(115200);
  delay(2000);

  TLOGLN("\n====================================");
  TLOGLN("   ESP32 모듈형 시스템 시작");
  TLOGLN("====================================\n");

  // WiFi 초기화는 HAS2_Wifi 모듈이 담당
  // initHAS2Wifi();

  // OTA 는 부팅 시 자동 실행하지 않음
  // checkOTA() 는 DataChange() → device_state == "github" 에서만 호출됨

  // 여기에 새 모듈 초기화 추가
  // initNewModule();

  TLOGLN("====================================");
  TLOGLN("   모든 모듈 초기화 완료!");
  TLOGLN("====================================\n");
}

void loop() {
  // 각 모듈의 업데이트 함수 호출

  // 여기에 새 모듈 업데이트 추가
  // updateNewModule();
}

// -------------------------------------------------------
// DataChange — 서버 데이터 변경 콜백
//
// 실제 프로젝트에서는 이 함수가 [DataChange가 있는 파일명].ino 에 위치합니다.
// device_state 변수가 변경될 때 서버 라이브러리가 이 함수를 호출합니다.
//
// 실제 프로젝트 적용 시 아래 블록을 해당 파일의 DataChange() 함수 내에
// device_state 분기로 추가하세요:
//
//   if (device_state == "github") {
//     checkOTA();
//   }
// -------------------------------------------------------
void DataChange(String changed_var) {
  // 예시: device_state 변수 변경 처리
  if (changed_var == "device_state") {
    // extern String device_state; // 실제 프로젝트에서 전역 변수 참조
    // if (device_state == "github") {
    //   checkOTA();
    // }

    // 템플릿 동작 확인용 (실제 프로젝트에서는 위 코드로 교체)
    TLOGLN("[DataChange] device_state 변경 감지");
    TLOGLN("[DataChange] device_state == \"github\" 수신 시 checkOTA() 호출됨");
  }
}
