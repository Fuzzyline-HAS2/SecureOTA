#ifndef SECURE_OTA_H
#define SECURE_OTA_H

/*
 * SecureOTA.h — HMAC-SHA256 서명 검증 보안 OTA 라이브러리
 *
 * [사용법]
 *   #include <SecureOTA.h>
 *
 *   SecureOTA ota(firmware_url, version_url, signature_url,
 *                 hmac_secret, FIRMWARE_VER);
 *
 *   // DataChange() 내 device_state == "github" 분기에서:
 *   ota.check();
 *
 * [선택] TelnetStream 동시 출력:
 *   ota.setLogStream(TelnetStream);
 *
 * [의존 라이브러리]
 *   - ESP32 Arduino Core (HTTPClient, Update, WiFiClientSecure, mbedtls)
 *   - TelnetStream (선택, setLogStream 사용 시)
 */

#include <Arduino.h>
#include <Stream.h>

class SecureOTA {
public:
  // --------------------------------------------------------
  // 생성자
  //   firmware_url  : GitHub Raw URL (update.bin)
  //   version_url   : GitHub Raw URL (version.txt)
  //   signature_url : GitHub Raw URL (update.sig)
  //   hmac_secret   : HMAC 비밀키 (secrets.h 에서 관리 권장)
  //   current_version : 현재 펌웨어 버전 (deploy.py 가 자동 증가)
  // --------------------------------------------------------
  SecureOTA(const char* firmware_url,
            const char* version_url,
            const char* signature_url,
            const char* hmac_secret,
            int current_version);

  // --------------------------------------------------------
  // 추가 로그 출력 스트림 설정 (예: TelnetStream)
  // Serial 은 항상 출력됨. 이 메서드로 추가 스트림 지정 가능.
  // --------------------------------------------------------
  void setLogStream(Stream& stream);

  // --------------------------------------------------------
  // OTA 확인 및 실행
  //   - WiFi 연결 상태 확인
  //   - GitHub 서버 버전 확인
  //   - 버전 불일치 시 서명 검증 후 펌웨어 업데이트
  //   device_state == "github" 수신 시 DataChange() 에서 호출
  // --------------------------------------------------------
  void check();

private:
  const char* _firmware_url;
  const char* _version_url;
  const char* _signature_url;
  const char* _hmac_secret;
  int         _current_version;
  Stream*     _log_stream;  // 추가 출력 스트림 (기본값: nullptr)

  // 내부 로그 출력 헬퍼
  void _print(const char* msg);
  void _println(const char* msg);
  void _printf(const char* fmt, ...);

  // OTA 내부 로직
  bool _downloadSignature(uint8_t sig[32]);
  bool _verifySignature(const uint8_t computed[32], const uint8_t downloaded[32]);
  int  _checkServerVersion();
  void _execOTA();
};

#endif // SECURE_OTA_H
