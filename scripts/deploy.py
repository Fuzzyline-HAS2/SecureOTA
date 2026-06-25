import os
import re
import sys
import shutil
import subprocess
import glob

# Windows CMD 이모지 출력을 위한 설정
sys.stdout.reconfigure(encoding='utf-8')

# ============================================================
# [사용법] 이 파일을 각 기기 저장소의 scripts/ 폴더에 복사한 뒤
#          아래 두 변수만 수정하세요.
#
#   SCRIPT_DIR  : 이 파일이 있는 scripts/ 폴더
#   BASE_DIR    : 기기 저장소 루트 (scripts/ 상위)
#                 → update.bin · update.sig · version.txt 가 여기에 저장됨
#   SKETCH_FILE : 버전 매크로(#define FIRMWARE_VER)가 있는 .ino 파일 경로
# ============================================================
SCRIPT_DIR    = os.path.dirname(os.path.abspath(__file__))
BASE_DIR      = os.path.dirname(SCRIPT_DIR)

# ── 기기 저장소에 맞게 변경 ─────────────────────────────────
SKETCH_FILE   = os.path.join(BASE_DIR, "YourSketch.ino")  # ← 변경 필요
VERSION_MACRO = "FIRMWARE_VER"
# ────────────────────────────────────────────────────────────

OUTPUT_BIN    = os.path.join(BASE_DIR, "update.bin")
OUTPUT_SIG    = os.path.join(BASE_DIR, "update.sig")
VERSION_TXT   = os.path.join(BASE_DIR, "version.txt")

# ── 파티션 관련 ──────────────────────────────────────────────
PARTITION_MACRO    = "PARTITION_VER"
OUTPUT_PART_BIN    = os.path.join(BASE_DIR, "partitions.bin")
OUTPUT_PART_SIG    = os.path.join(BASE_DIR, "partitions.sig")
PARTITION_VER_TXT  = os.path.join(BASE_DIR, "partition_version.txt")
# ────────────────────────────────────────────────────────────

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
    with open(SKETCH_FILE, "r", encoding="utf-8") as f:
        content = f.read()
    pattern = rf'#define\s+{VERSION_MACRO}\s+(\d+)'
    match = re.search(pattern, content)
    if match:
        return int(match.group(1))
    return None

def increment_version(current_ver):
    new_ver = current_ver + 1
    with open(SKETCH_FILE, "r", encoding="utf-8") as f:
        content = f.read()
    new_content = re.sub(
        rf'#define\s+{VERSION_MACRO}\s+\d+',
        f'#define {VERSION_MACRO} {new_ver}',
        content
    )
    with open(SKETCH_FILE, "w", encoding="utf-8") as f:
        f.write(new_content)
    return new_ver

# ============================================================
# 파티션 버전 관련 함수
# ============================================================
def get_current_partition_version():
    with open(SKETCH_FILE, "r", encoding="utf-8") as f:
        content = f.read()
    pattern = rf'#define\s+{PARTITION_MACRO}\s+(\d+)'
    match = re.search(pattern, content)
    if match:
        return int(match.group(1))
    return None

def increment_partition_version(current_ver):
    new_ver = current_ver + 1
    with open(SKETCH_FILE, "r", encoding="utf-8") as f:
        content = f.read()
    new_content = re.sub(
        rf'#define\s+{PARTITION_MACRO}\s+\d+',
        f'#define {PARTITION_MACRO} {new_ver}',
        content
    )
    with open(SKETCH_FILE, "w", encoding="utf-8") as f:
        f.write(new_content)
    return new_ver

# ============================================================
# 빌드된 partitions.bin 탐색
#   Arduino IDE 2.x 빌드 시 생성되는 *partitions*.bin 파일을 찾습니다.
# ============================================================
def find_partitions_bin():
    sketch_dir = os.path.dirname(SKETCH_FILE)

    search_patterns = [
        os.path.join(sketch_dir, "build", "**", "*partitions*.bin"),
        os.path.join(BASE_DIR, "build", "**", "*partitions*.bin"),
    ]

    candidates = []
    for pattern in search_patterns:
        candidates.extend(glob.glob(pattern, recursive=True))

    # 최종 배포용 파일(output_part_bin)은 제외
    candidates = [f for f in candidates if os.path.abspath(f) != os.path.abspath(OUTPUT_PART_BIN)]

    if not candidates:
        return None

    return max(candidates, key=os.path.getmtime)

# ============================================================
# 빌드된 .bin 파일 탐색
#   Arduino IDE 2.x : Ctrl+Alt+S → 스케치 폴더/build/ 아래에 생성
#   Arduino IDE 1.x : Ctrl+Alt+S → 스케치 폴더 바로 아래에 생성
# ============================================================
def find_newest_bin():
    sketch_dir = os.path.dirname(SKETCH_FILE)
    sketch_name = os.path.splitext(os.path.basename(SKETCH_FILE))[0]

    search_patterns = [
        os.path.join(sketch_dir, "build", "**", "*.bin"),  # Arduino IDE 2.x
        os.path.join(sketch_dir, "**", "*.bin"),            # Arduino IDE 1.x
        os.path.join(BASE_DIR, "build", "**", "*.bin"),    # 루트 build 폴더
    ]

    candidates = []
    for pattern in search_patterns:
        candidates.extend(glob.glob(pattern, recursive=True))

    # 최종 배포 파일 및 부트로더 관련 파일 제외
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
def git_push(version, partition_version=None):
    print("\n☁️ GitHub 에 업로드 중...")
    try:
        # version.txt 갱신
        with open(VERSION_TXT, "w", encoding="utf-8") as f:
            f.write(str(version))
        print(f"📝 version.txt → v{version}")

        # 배포 파일 + 스케치 파일(버전 변경분) 커밋
        files_to_add = [
            "update.bin",
            "update.sig",
            "version.txt",
            os.path.relpath(SKETCH_FILE, BASE_DIR).replace("\\", "/"),
        ]

        commit_msg = f"Firmware Update v{version}"

        if partition_version is not None:
            with open(PARTITION_VER_TXT, "w", encoding="utf-8") as f:
                f.write(str(partition_version))
            print(f"📝 partition_version.txt → v{partition_version}")
            files_to_add += ["partitions.bin", "partitions.sig", "partition_version.txt"]
            commit_msg += f" + Partition v{partition_version}"

        subprocess.run(["git", "-C", BASE_DIR, "add"] + files_to_add, check=True)
        subprocess.run(
            ["git", "-C", BASE_DIR, "commit", "-m", commit_msg],
            check=True
        )
        subprocess.run(["git", "-C", BASE_DIR, "push"], check=True)
        print("✅ GitHub 업로드 완료!")
    except subprocess.CalledProcessError as e:
        print(f"❌ Git 오류: {e}")
        print("   git 설치 및 저장소 연결 상태를 확인하세요.")

# ============================================================
# 메인
# ============================================================
def main():
    print("🚀 SecureOTA 배포 자동화 시작...")
    print(f"   스케치 : {SKETCH_FILE}")
    print(f"   버전 매크로 : #{VERSION_MACRO}")

    # 1. 비밀키 검증
    if HMAC_SECRET == "CHANGE_THIS_TO_YOUR_SECRET":
        print("❌ 오류: scripts/secrets.py 의 HMAC_SECRET 을 설정하세요.")
        return

    # 2. 현재 버전 읽기 및 증가
    cur_ver = get_current_version()
    if cur_ver is None:
        print(f"❌ 오류: {SKETCH_FILE} 에서 '#define {VERSION_MACRO}' 를 찾을 수 없습니다.")
        return

    print(f"\n현재 버전: v{cur_ver}")
    new_ver = increment_version(cur_ver)
    print(f"🔼 버전 변경: v{cur_ver} → v{new_ver}  ({SKETCH_FILE} 업데이트됨)")

    # 3. 아두이노 IDE 에서 바이너리 내보내기 대기
    print("\n⏳ [행동 필요] 아두이노 IDE 에서 Ctrl+Alt+S 를 실행하세요.")
    print("   (스케치 → 컴파일된 바이너리 내보내기)")
    print("   완료되면 Enter 를 누르세요...")
    input()

    # 4. .bin 파일 탐색
    print("🔎 빌드 파일 탐색 중...")
    bin_file = find_newest_bin()
    if not bin_file:
        sketch_dir = os.path.dirname(SKETCH_FILE)
        print("❌ .bin 파일을 찾을 수 없습니다.")
        print(f"   탐색 위치: {sketch_dir}/build/")
        print("   아두이노 IDE 에서 Ctrl+Alt+S 를 먼저 실행하세요.")
        return

    print(f"   발견: {os.path.relpath(bin_file, BASE_DIR)}")

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
    print("🔏 서명 완료 → update.sig (32 bytes)")

    # 7. 파티션 스키마 업데이트 여부 확인
    new_partition_ver = None
    print("\n🗂️ 파티션 스키마도 업데이트하시겠습니까? (y/N): ", end="", flush=True)
    answer = input().strip().lower()

    if answer == "y":
        # PARTITION_VER 매크로 확인
        cur_partition_ver = get_current_partition_version()
        if cur_partition_ver is None:
            print(f"❌ {SKETCH_FILE} 에서 '#define {PARTITION_MACRO}' 를 찾을 수 없습니다.")
            print(f"   .ino 파일에 '#define {PARTITION_MACRO} 1' 을 추가한 뒤 다시 시도하세요.")
        else:
            # partitions.bin 탐색
            part_bin_file = find_partitions_bin()
            if not part_bin_file:
                print("❌ partitions.bin 을 찾을 수 없습니다.")
                print("   아두이노 IDE 에서 Ctrl+Alt+S 로 바이너리를 내보낸 뒤 다시 시도하세요.")
            else:
                print(f"   발견: {os.path.relpath(part_bin_file, BASE_DIR)}")

                # partitions.bin 복사
                try:
                    shutil.copy2(part_bin_file, OUTPUT_PART_BIN)
                    print("📦 → partitions.bin 복사 완료")
                except Exception as e:
                    print(f"❌ 파일 복사 실패: {e}")
                    part_bin_file = None

                if part_bin_file:
                    # HMAC-SHA256 서명 생성
                    sign_script = os.path.join(SCRIPT_DIR, "sign_firmware.py")
                    result = subprocess.run(
                        [sys.executable, sign_script, OUTPUT_PART_BIN, HMAC_SECRET, OUTPUT_PART_SIG],
                        capture_output=True, text=True
                    )
                    if result.returncode != 0:
                        print(f"❌ 서명 실패:\n{result.stderr}")
                    else:
                        print("🔏 서명 완료 → partitions.sig (32 bytes)")
                        new_partition_ver = increment_partition_version(cur_partition_ver)
                        print(f"🔼 파티션 버전: v{cur_partition_ver} → v{new_partition_ver}")

    # 8. GitHub 푸시
    git_push(new_ver, new_partition_ver)

    print(f"\n🎉 배포 완료! 펌웨어 v{new_ver}", end="")
    if new_partition_ver is not None:
        print(f" + 파티션 v{new_partition_ver}", end="")
    print(" 이(가) GitHub 에 업로드되었습니다.")
    print("   서버에서 device_state = \"github\" 를 전송하면 기기가 업데이트됩니다.")

if __name__ == "__main__":
    main()
