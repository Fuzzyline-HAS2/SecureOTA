/*
 * SecureOTA.cpp — HMAC-SHA256 서명 검증 보안 OTA 구현
 *
 * [보안 업데이트 흐름]
 * 1. update.sig (32바이트) 먼저 다운로드
 * 2. update.bin 스트리밍하면서 HMAC-SHA256 동시 계산 (mbedtls/md.h)
 * 3. 완료 후 계산값 vs 다운로드값 비교
 * 4. 일치 시에만 Update.end(true) 커밋 — 불일치 시 Update.abort()
 *
 * [파티션 업데이트 흐름]
 * 1. partitions.sig (32바이트) 다운로드
 * 2. partitions.bin 전체 버퍼 다운로드 후 HMAC-SHA256 계산
 * 3. 서명 검증 통과 시 0x8000 에 플래시 직접 쓰기
 * 4. 재부팅 → 새 파티션 테이블 적용 → 이후 check() 에서 펌웨어 OTA 진행
 */

#include "SecureOTA.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "mbedtls/md.h"
#include "esp_idf_version.h"
#include <stdarg.h>

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  #include "esp_flash.h"
#else
  extern "C" {
    #include "esp_spi_flash.h"
  }
#endif

static const uint32_t PARTITION_TABLE_OFFSET = 0x8000;
static const uint32_t PARTITION_TABLE_SEC_SIZE = 0x1000;  // 4KB
static const int      PARTITION_BIN_MAX_SIZE   = 3072;    // 0xC00

// ============================================================
// 생성자 / 설정
// ============================================================

SecureOTA::SecureOTA(const char* firmware_url,
                     const char* version_url,
                     const char* signature_url,
                     const char* hmac_secret,
                     int current_version)
  : _firmware_url(firmware_url),
    _version_url(version_url),
    _signature_url(signature_url),
    _hmac_secret(hmac_secret),
    _current_version(current_version),
    _log_stream(nullptr),
    _on_success(nullptr),
    _on_skip(nullptr),
    _partition_url(nullptr),
    _partition_sig_url(nullptr),
    _partition_version_url(nullptr),
    _current_partition_version(0),
    _partition_update_enabled(false)
{}

void SecureOTA::setLogStream(Stream& stream) {
  _log_stream = &stream;
}

void SecureOTA::setOnSuccess(std::function<void()> callback) {
  _on_success = callback;
}

void SecureOTA::setOnSkip(std::function<void()> callback) {
  _on_skip = callback;
}

void SecureOTA::setPartitionUpdate(const char* partition_url,
                                   const char* partition_sig_url,
                                   const char* partition_version_url,
                                   int current_partition_version) {
  _partition_url              = partition_url;
  _partition_sig_url          = partition_sig_url;
  _partition_version_url      = partition_version_url;
  _current_partition_version  = current_partition_version;
  _partition_update_enabled   = true;
}

// ============================================================
// 로그 헬퍼
// ============================================================

void SecureOTA::_print(const char* msg) {
  Serial.print(msg);
  if (_log_stream) _log_stream->print(msg);
}

void SecureOTA::_println(const char* msg) {
  Serial.println(msg);
  if (_log_stream) _log_stream->println(msg);
}

void SecureOTA::_printf(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
  if (_log_stream) _log_stream->print(buf);
}

// ============================================================
// 내부: 서버에서 32바이트 HMAC 서명 파일 다운로드
// ============================================================

bool SecureOTA::_downloadSignature(uint8_t sig[32], const char* url) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);

  HTTPClient http;
  http.begin(client, String(url));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    _printf("[OTA] 서명 다운로드 실패 (HTTP %d)\n", httpCode);
    http.end();
    client.stop();
    return false;
  }

  int len = http.getSize();
  if (len != 32) {
    _printf("[OTA] 서명 크기 오류: %d bytes (32 bytes 필요)\n", len);
    http.end();
    client.stop();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  size_t bytesRead = stream->readBytes(sig, 32);
  http.end();
  client.stop();
  delay(500);

  if (bytesRead != 32) {
    _printf("[OTA] 서명 읽기 불완전: %d bytes\n", (int)bytesRead);
    return false;
  }
  return true;
}

// ============================================================
// 내부: HMAC-SHA256 서명 비교
// ============================================================

bool SecureOTA::_verifySignature(const uint8_t computed[32], const uint8_t downloaded[32]) {
  return memcmp(computed, downloaded, 32) == 0;
}

// ============================================================
// 내부: GitHub 펌웨어 서버 버전 확인
// ============================================================

int SecureOTA::_checkServerVersion() {
  _println("[OTA] 서버 버전 확인 중...");

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);

  HTTPClient http;
  http.begin(client, String(_version_url));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    _printf("[OTA] 버전 확인 실패 (HTTP %d)\n", httpCode);
    http.end();
    client.stop();
    delay(500);
    return -1;
  }

  String versionStr = http.getString();
  versionStr.trim();
  int serverVersion = versionStr.toInt();

  _printf("[OTA] 서버 v%d / 현재 v%d\n", serverVersion, _current_version);

  http.end();
  client.stop();
  delay(500);
  return serverVersion;
}

// ============================================================
// 내부: GitHub 파티션 서버 버전 확인
// ============================================================

int SecureOTA::_checkServerPartitionVersion() {
  _println("[PRT] 파티션 버전 확인 중...");

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);

  HTTPClient http;
  http.begin(client, String(_partition_version_url));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    _printf("[PRT] 파티션 버전 확인 실패 (HTTP %d)\n", httpCode);
    http.end();
    client.stop();
    delay(500);
    return -1;
  }

  String versionStr = http.getString();
  versionStr.trim();
  int serverVersion = versionStr.toInt();

  _printf("[PRT] 서버 v%d / 현재 v%d\n", serverVersion, _current_partition_version);

  http.end();
  client.stop();
  delay(500);
  return serverVersion;
}

// ============================================================
// 내부: 파티션 테이블 다운로드 + 서명 검증 + 플래시 쓰기
// ============================================================

bool SecureOTA::_execPartitionOTA() {
  // [1단계] 서명 파일 다운로드 (32바이트)
  uint8_t downloaded_sig[32];
  _println("[PRT] 파티션 서명 파일 다운로드 중...");
  if (!_downloadSignature(downloaded_sig, _partition_sig_url)) {
    _println("[PRT] 서명 다운로드 실패 — 파티션 업데이트 중단");
    return false;
  }
  _println("[PRT] 서명 파일 다운로드 완료");

  // [2단계] partitions.bin 전체 다운로드 (최대 3072 bytes)
  _println("[PRT] 파티션 테이블 다운로드 중...");

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);

  HTTPClient http;
  http.begin(client, String(_partition_url));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    _printf("[PRT] 파티션 다운로드 실패 (HTTP %d)\n", httpCode);
    http.end();
    client.stop();
    return false;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0 || contentLength > PARTITION_BIN_MAX_SIZE) {
    _printf("[PRT] 잘못된 파티션 테이블 크기: %d bytes\n", contentLength);
    http.end();
    client.stop();
    return false;
  }

  uint8_t* partBuf = (uint8_t*)malloc(contentLength);
  if (!partBuf) {
    _println("[PRT] 메모리 할당 실패");
    http.end();
    client.stop();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  int bytesRead = stream->readBytes(partBuf, contentLength);
  http.end();
  client.stop();
  delay(500);

  if (bytesRead != contentLength) {
    _printf("[PRT] 다운로드 불완전: %d / %d bytes\n", bytesRead, contentLength);
    free(partBuf);
    return false;
  }
  _printf("[PRT] %d bytes 다운로드 완료\n", bytesRead);

  // [3단계] HMAC-SHA256 계산
  mbedtls_md_context_t hmac_ctx;
  mbedtls_md_init(&hmac_ctx);
  mbedtls_md_setup(&hmac_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&hmac_ctx,
    (const unsigned char*)_hmac_secret, strlen(_hmac_secret));
  mbedtls_md_hmac_update(&hmac_ctx, partBuf, bytesRead);

  uint8_t computed_sig[32];
  mbedtls_md_hmac_finish(&hmac_ctx, computed_sig);
  mbedtls_md_free(&hmac_ctx);

  // [4단계] 서명 검증 — 위조 파티션 테이블 차단
  _println("[PRT] 서명 검증 중...");
  if (!_verifySignature(computed_sig, downloaded_sig)) {
    _println("[PRT] 서명 검증 실패! 파티션 변조 또는 서명 오류 — 업데이트 중단");
    free(partBuf);
    return false;
  }
  _println("[PRT] 서명 검증 완료 — 파티션 테이블 신뢰");

  // [5단계] 파티션 테이블 플래시 직접 쓰기 (0x8000)
  _println("[PRT] 파티션 테이블 플래싱 중...");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  esp_err_t err = esp_flash_erase_region(NULL, PARTITION_TABLE_OFFSET, PARTITION_TABLE_SEC_SIZE);
  if (err == ESP_OK) {
    err = esp_flash_write(NULL, partBuf, PARTITION_TABLE_OFFSET, (uint32_t)bytesRead);
  }
#else
  esp_err_t err = spi_flash_erase_sector(PARTITION_TABLE_OFFSET / PARTITION_TABLE_SEC_SIZE);
  if (err == ESP_OK) {
    err = spi_flash_write(PARTITION_TABLE_OFFSET, partBuf, (size_t)bytesRead);
  }
#endif

  free(partBuf);

  if (err != ESP_OK) {
    _printf("[PRT] 플래시 쓰기 실패: 0x%x\n", (int)err);
    return false;
  }

  _println("[PRT] 파티션 테이블 업데이트 완료!");
  return true;
}

// ============================================================
// 내부: 서명 검증 + 펌웨어 스트리밍 + 플래싱
// ============================================================

void SecureOTA::_execOTA() {
  // URL 유효성 검사
  if (String(_firmware_url).indexOf("http") < 0 ||
      String(_firmware_url).indexOf("YOUR_") >= 0) {
    _println("[OTA] 오류: 유효한 firmware_url을 설정하세요!");
    return;
  }

  // [1단계] 서명 파일 다운로드 (32바이트)
  uint8_t downloaded_sig[32];
  _println("[OTA] 서명 파일 다운로드 중...");
  if (!_downloadSignature(downloaded_sig, _signature_url)) {
    _println("[OTA] 서명 다운로드 실패 — 업데이트 중단");
    return;
  }
  _println("[OTA] 서명 파일 다운로드 완료");

  // [2단계] 펌웨어 다운로드 준비
  _println("[OTA] 펌웨어 다운로드 시작...");
  _print("[OTA] URL: ");
  _println(_firmware_url);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);

  HTTPClient http;
  http.begin(client, String(_firmware_url));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);

  int httpCode = http.GET();

  // [안전장치 1] HTTP 200 확인
  if (httpCode != HTTP_CODE_OK) {
    _printf("[OTA] 펌웨어 다운로드 실패 (HTTP %d)\n", httpCode);
    http.end();
    client.stop();
    return;
  }

  // [안전장치 2] Content-Length 검증
  int contentLength = http.getSize();
  _printf("[OTA] 펌웨어 크기: %d bytes\n", contentLength);

  if (contentLength <= 0 || contentLength > 2000000) {
    _println("[OTA] 오류: 잘못된 파일 크기");
    http.end();
    client.stop();
    return;
  }

  // [안전장치 3] OTA 파티션 공간 확인
  if (!Update.begin(contentLength)) {
    _println("[OTA] OTA 시작 공간 부족");
    http.end();
    client.stop();
    return;
  }

  _println("[OTA] 펌웨어 스트리밍 + HMAC 계산 시작...");

  // [3단계] 펌웨어 스트리밍 + HMAC-SHA256 동시 계산
  mbedtls_md_context_t hmac_ctx;
  mbedtls_md_init(&hmac_ctx);
  mbedtls_md_setup(&hmac_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&hmac_ctx,
    (const unsigned char*)_hmac_secret, strlen(_hmac_secret));

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t written = 0;
  int remaining = contentLength;
  bool streamError = false;

  while (remaining > 0) {
    unsigned long t = millis();
    while (stream->available() == 0) {
      if (millis() - t > 5000) {
        _println("[OTA] 스트림 타임아웃");
        streamError = true;
        break;
      }
      delay(10);
    }
    if (streamError) break;

    int toRead = min((int)stream->available(), min(remaining, (int)sizeof(buf)));
    int bytesRead = stream->readBytes(buf, toRead);
    if (bytesRead <= 0) continue;

    size_t w = Update.write(buf, bytesRead);
    if (w != (size_t)bytesRead) {
      _println("[OTA] OTA 쓰기 오류");
      streamError = true;
      break;
    }

    mbedtls_md_hmac_update(&hmac_ctx, buf, bytesRead);
    written += bytesRead;
    remaining -= bytesRead;
  }

  // HMAC 최종값 추출
  uint8_t computed_sig[32];
  mbedtls_md_hmac_finish(&hmac_ctx, computed_sig);
  mbedtls_md_free(&hmac_ctx);

  // [안전장치 4] 완전 다운로드 확인
  if (streamError || written != (size_t)contentLength) {
    _printf("[OTA] 다운로드 불완전: %d / %d bytes\n", (int)written, contentLength);
    Update.abort();
    http.end();
    client.stop();
    return;
  }

  _printf("[OTA] %d bytes 다운로드 완료\n", (int)written);

  // [안전장치 5] HMAC 서명 검증 — 위조 펌웨어 차단
  _println("[OTA] 서명 검증 중...");
  if (!_verifySignature(computed_sig, downloaded_sig)) {
    _println("[OTA] 서명 검증 실패! 펌웨어 변조 또는 서명 오류 — 업데이트 중단");
    Update.abort();
    http.end();
    client.stop();
    return;
  }
  _println("[OTA] 서명 검증 완료 — 펌웨어 신뢰");

  // [안전장치 6] Update 커밋 (서명 통과 후에만)
  if (!Update.end(true)) {
    _printf("[OTA] 업데이트 실패: %d\n", Update.getError());
    Update.abort();
    http.end();
    client.stop();
    return;
  }

  // [안전장치 7] 최종 완료 확인
  if (!Update.isFinished()) {
    _println("[OTA] 업데이트 미완료");
    http.end();
    client.stop();
    return;
  }

  _println("[OTA] OTA 완료!");
  http.end();
  client.stop();

  // 성공 콜백 실행 (서버 상태 전송 등)
  if (_on_success) {
    _println("[OTA] 성공 콜백 실행 중...");
    _on_success();
    delay(2000);  // 콜백(네트워크 전송 등) 완료 대기
  }

  _println("[OTA] 3초 후 재부팅...");
  delay(3000);
  ESP.restart();
}

// ============================================================
// 공개 API: check()
// ============================================================

void SecureOTA::check() {
  _println("\n[OTA] OTA 업데이트 트리거됨 (device_state=github)");

  if (WiFi.status() != WL_CONNECTED) {
    _println("[OTA] WiFi 미연결 — OTA 스킵");
    return;
  }

  // [파티션] 파티션 스키마 버전 확인 (setPartitionUpdate() 호출 시에만)
  if (_partition_update_enabled) {
    int serverPartitionVer = _checkServerPartitionVersion();
    if (serverPartitionVer == -1) {
      _println("[PRT] 파티션 버전 확인 실패 — 파티션 업데이트 스킵");
    } else if (serverPartitionVer != _current_partition_version) {
      _printf("[PRT] 파티션 버전 불일치 (현재 v%d → 서버 v%d) — 파티션 업데이트 시작\n",
              _current_partition_version, serverPartitionVer);
      if (_execPartitionOTA()) {
        _println("[PRT] 파티션 업데이트 완료 — 3초 후 재부팅 (새 파티션 테이블 적용)");
        _println("[PRT] 재부팅 후 check() 가 다시 호출되면 펌웨어 OTA 를 진행합니다.");
        delay(3000);
        ESP.restart();
      } else {
        _println("[PRT] 파티션 업데이트 실패 — 펌웨어 업데이트 계속 진행");
      }
    } else {
      _printf("[PRT] 파티션 최신 버전 (v%d) — 스킵\n", _current_partition_version);
    }
  }

  // [펌웨어] 서버 버전 확인
  int serverVersion = _checkServerVersion();
  if (serverVersion == -1) {
    _println("[OTA] 버전 확인 실패 — OTA 스킵");
    return;
  }
  if (serverVersion == _current_version) {
    _printf("[OTA] 이미 최신 버전 (v%d) — OTA 스킵\n", _current_version);
    if (_on_skip) {
      _println("[OTA] 스킵 콜백 실행 중...");
      _on_skip();
    }
    return;
  }

  _printf("[OTA] 버전 불일치 (현재 v%d → 서버 v%d) — 펌웨어 업데이트 시작\n",
          _current_version, serverVersion);
  delay(1000);
  _execOTA();
}
