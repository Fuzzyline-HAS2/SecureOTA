import os
import re
import sys
import shutil
import subprocess
import glob

# Windows CMD 이모지 출력을 위한 설정
sys.stdout.reconfigure(encoding='utf-8')

# ============================================================
# 경로 설정
#   SCRIPT_DIR  : scripts/ 폴더 (이 파일의 위치)
#   BASE_DIR    : 저장소 루트 (SCRIPT_DIR 의 상위)
#   ARDUINO_DIR : 스케치 폴더 (SignedOTA/)
#   CONFIG_FILE : ARDUINO_DIR/OTA_Config.h
#   update.bin · update.sig · version.txt → BASE_DIR (저장소 루트)
# ============================================================
SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
BASE_DIR    = os.path.dirname(SCRIPT_DIR)
ARDUINO_DIR = os.path.join(BASE_DIR, "SignedOTA")
CONFIG_FILE = os.path.join(ARDUINO_DIR, "OTA_Config.h")

OUTPUT_BIN  = os.path.join(BASE_DIR, "update.bin")
OUTPUT_SIG  = os.path.join(BASE_DIR, "update.sig")
VERSION_TXT = os.path.join(BASE_DIR, "version.txt")

# ============================================================
# 비밀키: scripts/secrets.py 에서 관리 (GitHub 비공개)
# ============================================================
try:
    sys.path.insert(0, SCRIPT_DIR)
    from secrets import HMAC_SECRET
except ImportError:
    print("❌ 오류: scripts/secrets.py 파일이 없습니다.")
    print("   secrets.py.example 을 secrets.py 로 복사한 뒤 비밀키를 설정하세요.")
    sys.exit(1)

# ============================================================
# 버전 관련 함수
# ============================================================
def get_current_version():
    with open(CONFIG_FILE, "r", encoding="utf-8") as f:
        content = f.read()
    match = re.search(r'#define CURRENT_FIRMWARE_VERSION (\d+)', content)
    if match:
        return int(match.group(1))
    return None

def increment_version(current_ver):
    new_ver = current_ver + 1
    with open(CONFIG_FILE, "r", encoding="utf-8") as f:
        content = f.read()
    new_content = re.sub(
        r'#define CURRENT_FIRMWARE_VERSION \d+',
        f'#define CURRENT_FIRMWARE_VERSION {new_ver}',
        content
    )
    with open(CONFIG_FILE, "w", encoding="utf-8") as f:
        f.write(new_content)
    return new_ver

# ============================================================
# 빌드된 .bin 파일 탐색
# ============================================================
def find_newest_bin():
    search_patterns = [
        os.path.join(ARDUINO_DIR, "build", "**", "*.bin"),
        os.path.join(BASE_DIR,    "build", "**", "*.bin"),
    ]

    candidates = []
    for pattern in search_patterns:
        candidates.extend(glob.glob(pattern, recursive=True))

    # 배포용 파일 및 부트로더 관련 파일 제외
    exclude_keywords = ["update", "merged", "bootloader", "partitions", "boot_app"]
    candidates = [
        f for f in candidates
        if not any(kw in os.path.basename(f).lower() for kw in exclude_keywords)
    ]

    if not candidates:
        return None

    return max(candidates, key=os.path.getmtime)

# ============================================================
# GitHub 푸시
# ============================================================
def git_push(version):
    print("\n☁️ GitHub 에 업로드 중...")
    try:
        # version.txt 갱신
        with open(VERSION_TXT, "w", encoding="utf-8") as f:
            f.write(str(version))
        print(f"📝 version.txt → {version}")

        subprocess.run(["git", "-C", BASE_DIR, "add",
                        "update.bin", "update.sig", "version.txt",
                        os.path.relpath(CONFIG_FILE, BASE_DIR)],
                       check=True)
        subprocess.run(["git", "-C", BASE_DIR, "commit",
                        "-m", f"Firmware Update v{version}"],
                       check=True)
        subprocess.run(["git", "-C", BASE_DIR, "push"], check=True)
        print("✅ GitHub 업로드 완료!")
    except subprocess.CalledProcessError as e:
        print(f"❌ Git 오류: {e}")
        print("   git 설치 및 저장소 연결 상태를 확인하세요.")

# ============================================================
# 메인
# ============================================================
def main():
    print("🚀 OTA 배포 자동화 시작...")

    # 1. 비밀키 검증
    if HMAC_SECRET == "CHANGE_THIS_TO_YOUR_SECRET":
        print("❌ 오류: scripts/secrets.py 의 HMAC_SECRET 을 설정하세요.")
        print("   SignedOTA/secrets.h 의 hmac_secret 과 동일한 값이어야 합니다.")
        return

    # 2. 버전 증가
    cur_ver = get_current_version()
    if cur_ver is None:
        print(f"❌ 오류: {CONFIG_FILE} 에서 버전을 찾을 수 없습니다.")
        return

    print(f"현재 버전: v{cur_ver}")
    new_ver = increment_version(cur_ver)
    print(f"🔼 버전 변경: v{cur_ver} → v{new_ver}")

    # 3. 아두이노 IDE 에서 컴파일 대기
    print("\n⏳ [행동 필요] 아두이노 IDE 에서 Ctrl+Alt+S (Export Compiled Binary) 를 실행하세요.")
    print("   완료되면 Enter 를 누르세요...")
    input()

    # 4. 빌드 파일 탐색
    print("🔎 빌드 파일 탐색 중...")
    bin_file = find_newest_bin()
    if not bin_file:
        print("❌ 오류: .bin 파일을 찾을 수 없습니다.")
        print(f"   탐색 위치: {ARDUINO_DIR}/build 또는 {BASE_DIR}/build")
        print("   아두이노 IDE 에서 Ctrl+Alt+S (Export Compiled Binary) 를 먼저 실행하세요.")
        return

    print(f"   발견: {bin_file}")

    # 5. update.bin 으로 복사
    try:
        shutil.copy2(bin_file, OUTPUT_BIN)
        print(f"📦 → update.bin 복사 완료")
    except Exception as e:
        print(f"❌ 파일 복사 실패: {e}")
        return

    # 6. HMAC-SHA256 서명 생성
    sign_script = os.path.join(SCRIPT_DIR, "sign_firmware.py")
    result = subprocess.run(
        [sys.executable, sign_script, OUTPUT_BIN, HMAC_SECRET, OUTPUT_SIG],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        print(f"❌ 서명 실패:\n{result.stderr}")
        return
    print(f"🔏 서명 완료 → update.sig (32 bytes)")

    # 7. GitHub 푸시
    git_push(new_ver)

    print(f"\n🎉 배포 완료! v{new_ver} 이(가) GitHub 에 업로드되었습니다.")
    print("   이제 서버에서 device_state = \"github\" 를 전송하면 기기가 업데이트됩니다.")

if __name__ == "__main__":
    main()
