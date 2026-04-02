/*
 * ota.ino — HMAC-SHA256 서명 검증 보안 OTA 업데이트
 *
 * [보안 업데이트 흐름]
 * 1. 서명 파일(update.sig, 32바이트)을 먼저 다운로드
 * 2. 펌웨어를 스트리밍하면서 HMAC-SHA256을 동시에 계산 (mbedtls/md.h)
 * 3. 다운로드 완료 후 계산된 HMAC과 서명 파일 비교
 * 4. 일치 시에만 Update.end(true) 커밋 — 불일치 시 Update.abort()
 *
 * [사용법]
 * - checkOTA()를 DataChange() 함수 내 device_state == "github" 분기에서 호출
 * - WiFi는 HAS2_Wifi.h가 관리하므로 이 모듈에서 연결하지 않음
 * - TLOG 매크로는 OTA_Config.h에 정의됨 (Serial + TelnetStream 동시 출력)
 */

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "mbedtls/md.h"

#include "OTA_Config.h"

// -------------------------------------------------------
// 내부 함수: 서버에서 32바이트 HMAC 서명 파일 다운로드
// -------------------------------------------------------
static bool downloadSignature(uint8_t sig[32]) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);

  HTTPClient http;
  http.begin(client, String(signature_url));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    TLOGF("[OTA] 서명 다운로드 실패 (HTTP %d)\n", httpCode);
    http.end();
    client.stop();
    return false;
  }

  int len = http.getSize();
  if (len != 32) {
    TLOGF("[OTA] 서명 크기 오류: %d bytes (32 bytes 필요)\n", len);
    http.end();
    client.stop();
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  size_t bytesRead = stream->readBytes(sig, 32);
  http.end();
  client.stop();
  delay(500);

  if (bytesRead != 32) {
    TLOGF("[OTA] 서명 읽기 불완전: %d bytes\n", (int)bytesRead);
    return false;
  }
  return true;
}

// 내부 함수: HMAC-SHA256 서명 비교 (계산값 vs 다운로드값)
static bool verifySignature(const uint8_t computed[32], const uint8_t downloaded[32]) {
  return memcmp(computed, downloaded, 32) == 0;
}

// -------------------------------------------------------
// 내부 함수: GitHub 서버의 버전 확인
// -------------------------------------------------------
static int checkServerVersion() {
  TLOGLN("[OTA] 서버 버전 확인 중...");

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);

  HTTPClient http;
  http.begin(client, String(version_url));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    TLOGF("[OTA] 버전 확인 실패 (HTTP %d)\n", httpCode);
    http.end();
    client.stop();
    delay(500);
    return -1;
  }

  String versionStr = http.getString();
  versionStr.trim();
  int serverVersion = versionStr.toInt();

  TLOGF("[OTA] 서버 v%d / 현재 v%d\n", serverVersion, CURRENT_FIRMWARE_VERSION);

  http.end();
  client.stop();
  delay(500);
  return serverVersion;
}

// -------------------------------------------------------
// execOTA: 서명 검증 + 펌웨어 스트리밍 + 플래싱
// -------------------------------------------------------
static void execOTA() {
  // URL 유효성 검사
  if (String(firmware_url).indexOf("http") < 0 ||
      String(firmware_url).indexOf("REPLACE") >= 0) {
    TLOGLN("[OTA] 오류: 유효한 firmware_url을 OTA_Config.h에 설정하세요!");
    return;
  }

  // [1단계] 서명 파일 다운로드 (32바이트)
  uint8_t downloaded_sig[32];
  TLOGLN("[OTA] 서명 파일 다운로드 중...");
  if (!downloadSignature(downloaded_sig)) {
    TLOGLN("[OTA] 서명 다운로드 실패 — 업데이트 중단");
    return;
  }
  TLOGLN("[OTA] 서명 파일 다운로드 완료");

  // [2단계] 펌웨어 다운로드 준비
  TLOGLN("[OTA] 펌웨어 다운로드 시작...");
  TLOG("[OTA] URL: ");
  TLOGLN(firmware_url);

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);

  HTTPClient http;
  http.begin(client, String(firmware_url));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);

  int httpCode = http.GET();

  // [안전장치 1] HTTP 200 확인
  if (httpCode != HTTP_CODE_OK) {
    TLOGF("[OTA] 펌웨어 다운로드 실패 (HTTP %d)\n", httpCode);
    http.end();
    client.stop();
    return;
  }

  // [안전장치 2] Content-Length 검증
  int contentLength = http.getSize();
  TLOGF("[OTA] 펌웨어 크기: %d bytes\n", contentLength);

  if (contentLength <= 0 || contentLength > 2000000) {
    TLOGLN("[OTA] 오류: 잘못된 파일 크기");
    http.end();
    client.stop();
    return;
  }

  // [안전장치 3] OTA 파티션 공간 확인
  if (!Update.begin(contentLength)) {
    TLOGLN("[OTA] OTA 시작 공간 부족");
    http.end();
    client.stop();
    return;
  }

  TLOGLN("[OTA] 펌웨어 스트리밍 + HMAC 계산 시작...");

  // [3단계] 펌웨어 스트리밍 + HMAC-SHA256 동시 계산
  mbedtls_md_context_t hmac_ctx;
  mbedtls_md_init(&hmac_ctx);
  mbedtls_md_setup(&hmac_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&hmac_ctx,
    (const unsigned char*)hmac_secret, strlen(hmac_secret));

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  size_t written = 0;
  int remaining = contentLength;
  bool streamError = false;

  while (remaining > 0) {
    unsigned long t = millis();
    while (stream->available() == 0) {
      if (millis() - t > 5000) {
        TLOGLN("[OTA] 스트림 타임아웃");
        streamError = true;
        break;
      }
      delay(10);
    }
    if (streamError) break;

    int toRead = min((int)stream->available(), min(remaining, (int)sizeof(buf)));
    int bytesRead = stream->readBytes(buf, toRead);
    if (bytesRead <= 0) continue;

    // OTA 파티션에 쓰기
    size_t w = Update.write(buf, bytesRead);
    if (w != (size_t)bytesRead) {
      TLOGLN("[OTA] OTA 쓰기 오류");
      streamError = true;
      break;
    }

    // HMAC 누적 계산
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
    TLOGF("[OTA] 다운로드 불완전: %d / %d bytes\n", (int)written, contentLength);
    Update.abort();
    http.end();
    client.stop();
    return;
  }

  TLOGF("[OTA] %d bytes 다운로드 완료\n", (int)written);

  // [안전장치 5] HMAC 서명 검증 — 위조 펌웨어 차단
  TLOGLN("[OTA] 서명 검증 중...");
  if (!verifySignature(computed_sig, downloaded_sig)) {
    TLOGLN("[OTA] 서명 검증 실패! 펌웨어 변조 또는 서명 오류 — 업데이트 중단");
    Update.abort();
    http.end();
    client.stop();
    return;
  }
  TLOGLN("[OTA] 서명 검증 완료 — 펌웨어 신뢰");

  // [안전장치 6] Update 커밋 (서명 통과 후에만)
  if (!Update.end(true)) {
    TLOGF("[OTA] 업데이트 실패: %d\n", Update.getError());
    Update.abort();
    http.end();
    client.stop();
    return;
  }

  // [안전장치 7] 최종 완료 확인
  if (!Update.isFinished()) {
    TLOGLN("[OTA] 업데이트 미완료");
    http.end();
    client.stop();
    return;
  }

  TLOGLN("[OTA] OTA 완료! 3초 후 재부팅...");
  http.end();
  client.stop();
  delay(3000);
  ESP.restart();
}

// -------------------------------------------------------
// 공개 API
// -------------------------------------------------------

// OTA 업데이트를 즉시 실행 (버전 체크 없이 강제 업데이트)
// device_state == "github" 수신 시 DataChange()에서 호출
void checkOTA() {
  TLOGLN("\n[OTA] OTA 업데이트 트리거됨 (device_state=github)");

  if (WiFi.status() != WL_CONNECTED) {
    TLOGLN("[OTA] WiFi 미연결 — OTA 스킵");
    return;
  }

  int serverVersion = checkServerVersion();
  if (serverVersion == -1) {
    TLOGLN("[OTA] 버전 확인 실패 — OTA 스킵");
    return;
  }
  if (serverVersion == CURRENT_FIRMWARE_VERSION) {
    TLOGF("[OTA] 이미 최신 버전 (v%d) — OTA 스킵\n", CURRENT_FIRMWARE_VERSION);
    return;
  }

  TLOGF("[OTA] 버전 불일치 (현재 v%d → 서버 v%d) — 펌웨어 업데이트 시작\n",
        CURRENT_FIRMWARE_VERSION, serverVersion);
  delay(1000);
  execOTA();
}
