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
#include <functional>

class SecureOTA {
public:
  // --------------------------------------------------------
  // 생성자
  //   firmware_url    : GitHub Raw URL (update.bin)
  //   version_url     : GitHub Raw URL (version.txt)
  //   signature_url   : GitHub Raw URL (update.sig)
  //   hmac_secret     : HMAC 비밀키 (secrets.h 에서 관리 권장)
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
  // OTA 성공 콜백
  //   플래싱 완료 후 ESP.restart() 직전에 호출됨
  //   사용 예: ota.setOnSuccess([]() {
  //              Firebase.setString(fbdo, "/device_state", "setting");
  //            });
  // --------------------------------------------------------
  void setOnSuccess(std::function<void()> callback);

  // --------------------------------------------------------
  // OTA 스킵 콜백
  //   이미 최신 버전이라 업데이트 불필요할 때 호출됨
  //   서버가 아직 device_state=github 를 들고 있을 때
  //   setting 으로 되돌려주는 용도
  //   사용 예: ota.setOnSkip([]() {
  //              Firebase.setString(fbdo, "/device_state", "setting");
  //            });
  // --------------------------------------------------------
  void setOnSkip(std::function<void()> callback);

  // --------------------------------------------------------
  // --------------------------------------------------------
  // OTA 확인 및 실행
  //   - WiFi 연결 상태 확인
  //   - GitHub 서버 버전 확인
  //   - 버전 불일치 시 서명 검증 후 펌웨어 업데이트
  //   device_state == "github" 수신 시 DataChange() 에서 호출
  // --------------------------------------------------------
  void check();

  // --------------------------------------------------------
  // 파티션 스키마 OTA 설정 (선택)
  //   partition_url         : GitHub Raw URL (partitions.bin)
  //   partition_sig_url     : GitHub Raw URL (partitions.sig)
  //   partition_version_url : GitHub Raw URL (partition_version.txt)
  //   current_partition_version : 현재 파티션 버전 (#define PARTITION_VER)
  //
  //   호출 시 check() 에서 파티션 버전도 함께 확인하며,
  //   불일치 시 파티션 테이블을 플래싱한 뒤 재부팅합니다.
  //   재부팅 후 check() 가 다시 호출되면 펌웨어 OTA 를 진행합니다.
  // --------------------------------------------------------
  void setPartitionUpdate(const char* partition_url,
                          const char* partition_sig_url,
                          const char* partition_version_url,
                          int current_partition_version);

private:
  const char* _firmware_url;
  const char* _version_url;
  const char* _signature_url;
  const char* _hmac_secret;
  int         _current_version;
  Stream*     _log_stream;                // 추가 출력 스트림 (기본값: nullptr)
  std::function<void()> _on_success;      // OTA 성공 콜백 (기본값: nullptr)
  std::function<void()> _on_skip;         // OTA 스킵 콜백 (기본값: nullptr)

  const char* _partition_url;             // partitions.bin URL
  const char* _partition_sig_url;         // partitions.sig URL
  const char* _partition_version_url;     // partition_version.txt URL
  int         _current_partition_version; // 현재 파티션 버전
  bool        _partition_update_enabled;  // setPartitionUpdate() 호출 여부

  // 내부 로그 출력 헬퍼
  void _print(const char* msg);
  void _println(const char* msg);
  void _printf(const char* fmt, ...);

  // OTA 내부 로직
  bool _downloadSignature(uint8_t sig[32], const char* url);
  bool _verifySignature(const uint8_t computed[32], const uint8_t downloaded[32]);
  int  _checkServerVersion();
  void _execOTA();

  // 파티션 OTA 내부 로직
  int  _checkServerPartitionVersion();
  bool _execPartitionOTA();
};

#endif // SECURE_OTA_H
