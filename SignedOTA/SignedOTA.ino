/*
 * SignedOTA: HMAC-SHA256 서명 검증을 포함한 보안 OTA 업데이트
 *
 * [보안 업데이트 흐름]
 * 1. 서명 파일(update.sig, 32바이트)을 먼저 다운로드
 * 2. 펌웨어를 스트리밍하면서 HMAC-SHA256을 동시에 계산
 * 3. 다운로드 완료 후 계산된 HMAC과 서명 파일 비교
 * 4. 일치 시에만 플래싱 커밋 — 불일치 시 즉시 중단
 */

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "mbedtls/md.h"

#include "UserConfig.h"

// =======================================================

// 서버에서 32바이트 HMAC 서명 파일을 다운로드하는 함수
bool downloadSignature(uint8_t sig[32]) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);

  HTTPClient http;
  http.begin(client, String(signature_url));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(15000);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[서명] 다운로드 실패 (HTTP: %d)\n", httpCode);
    http.end();
    client.stop();
    return false;
  }

  int len = http.getSize();
  if (len != 32) {
    Serial.printf("[서명] 크기 오류: %d bytes (32 bytes 필요)\n", len);
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
    Serial.printf("[서명] 읽기 불완전: %d bytes\n", bytesRead);
    return false;
  }
  return true;
}

// HMAC-SHA256 서명 검증 함수 (계산값 vs 다운로드값 비교)
bool verifySignature(const uint8_t computed[32], const uint8_t downloaded[32]) {
  return memcmp(computed, downloaded, 32) == 0;
}

// =======================================================

// 서버의 버전 정보를 확인하는 함수
int checkServerVersion() {
  Serial.println("[OTA 모듈] 서버 버전 확인 중...");

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);

  http.begin(client, String(version_url));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String versionStr = http.getString();
    versionStr.trim();
    int serverVersion = versionStr.toInt();

    Serial.printf("[OTA 모듈] 서버 버전: %d, 현재 버전: %d\n", serverVersion,
                  CURRENT_FIRMWARE_VERSION);

    http.end();
    client.stop();
    delay(500);
    return serverVersion;
  } else {
    Serial.printf("[OTA 모듈] ⚠️ 버전 확인 실패 (HTTP 코드: %d)\n", httpCode);
    http.end();
    client.stop();
    delay(500);
    return -1;
  }
}

// URL에서 펌웨어를 다운로드하고 OTA 업데이트를 실행하는 함수
void execOTA() {
  // URL 유효성 검사
  if (String(firmware_url).indexOf("http") < 0 ||
      String(firmware_url).indexOf("REPLACE") >= 0) {
    Serial.println("❌ 오류: 유효한 firmware_url을 설정해주세요!");
    return;
  }

  // [1단계] 서명 파일 다운로드 (32바이트)
  uint8_t downloaded_sig[32];
  Serial.println("[OTA] 서명 파일 다운로드 중...");
  if (!downloadSignature(downloaded_sig)) {
    Serial.println("❌ 서명 파일 다운로드 실패. 업데이트 중단");
    return;
  }
  Serial.println("[OTA] 서명 파일 다운로드 완료");

  // [2단계] 펌웨어 다운로드 준비
  Serial.println("클라우드 OTA 업데이트를 시작합니다...");
  Serial.println("대상 URL: " + String(firmware_url));

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(30000);

  http.begin(client, String(firmware_url));
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(30000);

  int httpCode = http.GET();

  // [안전장치 1] HTTP 200 OK가 아니면 즉시 중단
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("❌ 펌웨어 다운로드 실패 (HTTP 코드: %d)\n", httpCode);
    if (httpCode > 0) {
      Serial.printf("❌ 에러: %s\n", http.errorToString(httpCode).c_str());
    } else {
      Serial.println("❌ 연결 실패. 네트워크를 확인하세요.");
    }
    http.end();
    client.stop();
    return;
  }

  // [안전장치 2] Content-Length 검증
  int contentLength = http.getSize();
  Serial.printf("다운로드 크기: %d bytes\n", contentLength);

  if (contentLength <= 0 || contentLength > 2000000) {
    Serial.println("❌ 오류: 잘못된 파일 크기");
    http.end();
    client.stop();
    return;
  }

  // [안전장치 3] Update 시작 가능 여부 확인
  if (!Update.begin(contentLength)) {
    Serial.println("❌ OTA를 시작할 공간이 부족합니다.");
    http.end();
    client.stop();
    return;
  }

  Serial.println("OTA 업데이트를 시작합니다. 잠시만 기다려주세요...");

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
    // 데이터 수신 대기 (최대 5초)
    unsigned long t = millis();
    while (stream->available() == 0) {
      if (millis() - t > 5000) {
        Serial.println("❌ 스트림 타임아웃");
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
      Serial.println("❌ OTA 쓰기 오류");
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

  // [안전장치 4] 완전히 다운로드되었는지 확인
  if (streamError || written != (size_t)contentLength) {
    Serial.printf("❌ 다운로드 불완전: %d / %d bytes\n", written, contentLength);
    Update.abort();
    http.end();
    client.stop();
    return;
  }

  Serial.printf("✅ %d bytes 다운로드 완료\n", written);

  // [안전장치 5] HMAC 서명 검증 — 위조 펌웨어 차단
  Serial.println("[OTA] 서명 검증 중...");
  if (!verifySignature(computed_sig, downloaded_sig)) {
    Serial.println("❌ 서명 검증 실패! 펌웨어가 변조되었거나 서명이 잘못되었습니다.");
    Serial.println("❌ 업데이트를 중단합니다.");
    Update.abort();
    http.end();
    client.stop();
    return;
  }
  Serial.println("✅ 서명 검증 완료! 펌웨어가 신뢰할 수 있습니다.");

  // [안전장치 6] Update 종료 및 커밋 (서명 검증 통과 후에만)
  if (!Update.end(true)) {
    Serial.printf("❌ 업데이트 실패: %d\n", Update.getError());
    Update.abort();
    http.end();
    client.stop();
    return;
  }

  // [안전장치 7] 최종 확인
  if (!Update.isFinished()) {
    Serial.println("❌ 업데이트가 완전히 종료되지 않았습니다.");
    http.end();
    client.stop();
    return;
  }

  Serial.println("✅ OTA 완료!");
  Serial.println("업데이트가 성공적으로 완료되었습니다. 3초 후 재부팅합니다...");

  http.end();
  client.stop();
  delay(3000);

  ESP.restart(); // 모든 검증을 통과한 경우에만 재부팅
}

void initOTA() {
  Serial.println("\n[OTA 모듈] 초기화 시작...");

  Serial.print("[OTA 모듈] 와이파이 연결 중: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 20) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  Serial.println("OTA UPDATE 되지롱");

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[OTA 모듈] 와이파이 연결 성공!");
    Serial.print("[OTA 모듈] 할당된 IP 주소: ");
    Serial.println(WiFi.localIP());

    int serverVersion = checkServerVersion();

    if (serverVersion == -1) {
      Serial.println("[OTA 모듈] ⚠️ 버전 확인 실패. OTA 스킵");
    } else if (serverVersion != CURRENT_FIRMWARE_VERSION) {
      Serial.printf("[OTA 모듈] 🔄 버전 불일치 감지! (현재: v%d → 서버: v%d)\n",
                    CURRENT_FIRMWARE_VERSION, serverVersion);
      Serial.println("[OTA 모듈] 5초 후 펌웨어 다운로드를 시작합니다...");
      delay(5000);
      checkOTA();
    } else {
      Serial.printf("[OTA 모듈] ✅ 서버와 동일한 버전 (v%d)\n",
                    CURRENT_FIRMWARE_VERSION);
      Serial.println("[OTA 모듈] OTA 스킵");
    }
  } else {
    Serial.println("\n[OTA 모듈] ❌ 와이파이 연결 실패!");
    Serial.println("[OTA 모듈] 3초 후 자동 재부팅합니다...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("[OTA 모듈] 초기화 완료\n");
}

// OTA 업데이트를 확인하고 실행하는 함수 (언제든지 호출 가능)
void checkOTA() { execOTA(); }
