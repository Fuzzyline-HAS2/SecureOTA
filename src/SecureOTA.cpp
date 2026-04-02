/*
 * SecureOTA.cpp — HMAC-SHA256 서명 검증 보안 OTA 구현
 *
 * [보안 업데이트 흐름]
 * 1. update.sig (32바이트) 먼저 다운로드
 * 2. update.bin 스트리밍하면서 HMAC-SHA256 동시 계산 (mbedtls/md.h)
 * 3. 완료 후 계산값 vs 다운로드값 비교
 * 4. 일치 시에만 Update.end(true) 커밋 — 불일치 시 Update.abort()
 */

#include "SecureOTA.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "mbedtls/md.h"
#include <stdarg.h>

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
    _on_success(nullptr)
{}

void SecureOTA::setLogStream(Stream& stream) {
  _log_stream = &stream;
}

void SecureOTA::setOnSuccess(std::function<void()> callback) {
  _on_success = callback;
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

bool SecureOTA::_downloadSignature(uint8_t sig[32]) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);

  HTTPClient http;
  http.begin(client, String(_signature_url));
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
// 내부: GitHub 서버 버전 확인
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
  if (!_downloadSignature(downloaded_sig)) {
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

  int serverVersion = _checkServerVersion();
  if (serverVersion == -1) {
    _println("[OTA] 버전 확인 실패 — OTA 스킵");
    return;
  }
  if (serverVersion == _current_version) {
    _printf("[OTA] 이미 최신 버전 (v%d) — OTA 스킵\n", _current_version);
    return;
  }

  _printf("[OTA] 버전 불일치 (현재 v%d → 서버 v%d) — 펌웨어 업데이트 시작\n",
          _current_version, serverVersion);
  delay(1000);
  _execOTA();
}
