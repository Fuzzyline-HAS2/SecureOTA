import os
import re
import sys
import shutil
import subprocess
import glob

sys.stdout.reconfigure(encoding='utf-8')

_CORE_DIR     = os.path.dirname(os.path.abspath(__file__))  # SecureOTA/scripts/
VERSION_MACRO = "FIRMWARE_VER"
PART_MACRO    = "PARTITION_VER"


def _get_version(sketch_file, macro):
    with open(sketch_file, "r", encoding="utf-8") as f:
        content = f.read()
    match = re.search(rf'#define\s+{macro}\s+(\d+)', content)
    return int(match.group(1)) if match else None


def _increment_version(sketch_file, macro, current_ver):
    new_ver = current_ver + 1
    with open(sketch_file, "r", encoding="utf-8") as f:
        content = f.read()
    new_content = re.sub(
        rf'#define\s+{macro}\s+\d+',
        f'#define {macro} {new_ver}',
        content
    )
    with open(sketch_file, "w", encoding="utf-8") as f:
        f.write(new_content)
    return new_ver


def _find_newest_bin(base_dir, output_bin):
    patterns = [
        os.path.join(base_dir, "build", "**", "*.bin"),
        os.path.join(base_dir, "**", "*.bin"),
    ]
    exclude = ["merged", "bootloader", "partitions", "boot_app"]
    candidates = []
    for p in patterns:
        candidates.extend(glob.glob(p, recursive=True))
    candidates = [
        f for f in candidates
        if not any(kw in os.path.basename(f).lower() for kw in exclude)
        and os.path.abspath(f) != os.path.abspath(output_bin)
    ]
    return max(candidates, key=os.path.getmtime) if candidates else None


def _find_partitions_bin(base_dir, output_part_bin):
    patterns = [
        os.path.join(base_dir, "build", "**", "*partitions*.bin"),
    ]
    candidates = []
    for p in patterns:
        candidates.extend(glob.glob(p, recursive=True))
    candidates = [f for f in candidates if os.path.abspath(f) != os.path.abspath(output_part_bin)]
    return max(candidates, key=os.path.getmtime) if candidates else None


def _sign(input_file, secret, output_file):
    sign_script = os.path.join(_CORE_DIR, "sign_firmware.py")
    result = subprocess.run(
        [sys.executable, sign_script, input_file, secret, output_file],
        capture_output=True, text=True
    )
    return result.returncode == 0, result.stderr


def _git_push(base_dir, sketch_file, version, partition_version=None):
    print("\n☁️ GitHub 에 업로드 중...")
    try:
        with open(os.path.join(base_dir, "version.txt"), "w", encoding="utf-8") as f:
            f.write(str(version))
        print(f"📝 version.txt → v{version}")

        files_to_add = [
            "update.bin", "update.sig", "version.txt",
            os.path.relpath(sketch_file, base_dir).replace("\\", "/"),
        ]
        commit_msg = f"Firmware Update v{version}"

        if partition_version is not None:
            with open(os.path.join(base_dir, "partition_version.txt"), "w", encoding="utf-8") as f:
                f.write(str(partition_version))
            print(f"📝 partition_version.txt → v{partition_version}")
            files_to_add += ["partitions.bin", "partitions.sig", "partition_version.txt"]
            commit_msg += f" + Partition v{partition_version}"

        subprocess.run(["git", "-C", base_dir, "add"] + files_to_add, check=True)
        subprocess.run(["git", "-C", base_dir, "commit", "-m", commit_msg], check=True)
        subprocess.run(["git", "-C", base_dir, "push"], check=True)
        print("✅ GitHub 업로드 완료!")
    except subprocess.CalledProcessError as e:
        print(f"❌ Git 오류: {e}")


def run_deploy(base_dir):
    # 비밀키 로드 (장치 repo 의 scripts/secrets.py)
    device_scripts = os.path.join(base_dir, "scripts")
    sys.path.insert(0, device_scripts)
    try:
        from secrets import HMAC_SECRET
    except ImportError:
        print("❌ 오류: scripts/secrets.py 파일이 없습니다.")
        print("   secrets.py.example 을 secrets.py 로 복사한 뒤 비밀키를 설정하세요.")
        return
    finally:
        sys.path.pop(0)

    if HMAC_SECRET == "CHANGE_THIS_TO_YOUR_SECRET":
        print("❌ 오류: scripts/secrets.py 의 HMAC_SECRET 을 설정하세요.")
        return

    # 스케치 파일 자동 감지 (Arduino 규칙: 폴더명 == 스케치명)
    dir_name    = os.path.basename(base_dir)
    sketch_file = os.path.join(base_dir, dir_name + ".ino")
    if not os.path.exists(sketch_file):
        ino_files   = [f for f in os.listdir(base_dir) if f.endswith(".ino")]
        sketch_file = os.path.join(base_dir, ino_files[0]) if ino_files else None
    if not sketch_file:
        print("❌ 오류: .ino 파일을 찾을 수 없습니다.")
        return

    output_bin      = os.path.join(base_dir, "update.bin")
    output_sig      = os.path.join(base_dir, "update.sig")
    output_part_bin = os.path.join(base_dir, "partitions.bin")
    output_part_sig = os.path.join(base_dir, "partitions.sig")

    print("🚀 SecureOTA 배포 자동화 시작...")
    print(f"   스케치 : {sketch_file}")

    # 버전 읽기 및 증가
    cur_ver = _get_version(sketch_file, VERSION_MACRO)
    if cur_ver is None:
        print(f"❌ 오류: '#define {VERSION_MACRO}' 를 찾을 수 없습니다.")
        return
    print(f"\n현재 버전: v{cur_ver}")
    new_ver = _increment_version(sketch_file, VERSION_MACRO, cur_ver)
    print(f"🔼 버전 변경: v{cur_ver} → v{new_ver}")

    # 아두이노 IDE 빌드 대기
    print("\n" + "="*55)
    print("⚠️  [필수] 아두이노 IDE 재로드 후 컴파일!")
    print("   1) 파일 변경 알림 → [Reload] 클릭")
    print("   2) Ctrl+Alt+S (컴파일된 바이너리 내보내기)")
    print("="*55)
    print("   완료되면 Enter 를 누르세요...")
    input()

    # 펌웨어 .bin 탐색 및 복사
    print("🔎 빌드 파일 탐색 중...")
    bin_file = _find_newest_bin(base_dir, output_bin)
    if not bin_file:
        print("❌ .bin 파일을 찾을 수 없습니다.")
        return
    print(f"   발견: {os.path.relpath(bin_file, base_dir)}")
    try:
        shutil.copy2(bin_file, output_bin)
        print("📦 → update.bin 복사 완료")
    except Exception as e:
        print(f"❌ 파일 복사 실패: {e}")
        return

    # 펌웨어 서명
    ok, err = _sign(output_bin, HMAC_SECRET, output_sig)
    if not ok:
        print(f"❌ 서명 실패:\n{err}")
        return
    print("🔏 서명 완료 → update.sig")

    # 파티션 업데이트 (선택)
    new_partition_ver = None
    print("\n🗂️ 파티션 스키마도 업데이트하시겠습니까? (y/N): ", end="", flush=True)
    if input().strip().lower() == "y":
        cur_pver = _get_version(sketch_file, PART_MACRO)
        if cur_pver is None:
            print(f"❌ '#define {PART_MACRO}' 를 찾을 수 없습니다.")
        else:
            part_bin_file = _find_partitions_bin(base_dir, output_part_bin)
            if not part_bin_file:
                print("❌ partitions.bin 을 찾을 수 없습니다. Ctrl+Alt+S 후 다시 시도하세요.")
            else:
                print(f"   발견: {os.path.relpath(part_bin_file, base_dir)}")
                try:
                    shutil.copy2(part_bin_file, output_part_bin)
                    print("📦 → partitions.bin 복사 완료")
                except Exception as e:
                    print(f"❌ 파일 복사 실패: {e}")
                    part_bin_file = None
                if part_bin_file:
                    ok, err = _sign(output_part_bin, HMAC_SECRET, output_part_sig)
                    if not ok:
                        print(f"❌ 서명 실패:\n{err}")
                    else:
                        print("🔏 서명 완료 → partitions.sig")
                        new_partition_ver = _increment_version(sketch_file, PART_MACRO, cur_pver)
                        print(f"🔼 파티션 버전: v{cur_pver} → v{new_partition_ver}")

    _git_push(base_dir, sketch_file, new_ver, new_partition_ver)
    print(f"\n🎉 배포 완료! 펌웨어 v{new_ver}", end="")
    if new_partition_ver is not None:
        print(f" + 파티션 v{new_partition_ver}", end="")
    print(" 이(가) GitHub 에 업로드되었습니다.")
    print("   서버에서 device_state = \"github\" 를 전송하면 기기가 업데이트됩니다.")
