import os
import re
import sys
import json
import shutil
import subprocess
import glob
import urllib.request
import urllib.error
from urllib.parse import quote

GITHUB_API = "https://api.github.com"

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


def _find_sketch_file(base_dir):
    """Arduino 규칙(폴더명 == 스케치명)으로 .ino 파일을 자동 감지. 없으면 None."""
    dir_name = os.path.basename(base_dir)
    sketch_file = os.path.join(base_dir, dir_name + ".ino")
    if os.path.exists(sketch_file):
        return sketch_file
    ino_files = [f for f in os.listdir(base_dir) if f.endswith(".ino")]
    return os.path.join(base_dir, ino_files[0]) if ino_files else None


def _sign(input_file, secret, output_file):
    sign_script = os.path.join(_CORE_DIR, "sign_firmware.py")
    result = subprocess.run(
        [sys.executable, sign_script, input_file, secret, output_file],
        capture_output=True, text=True
    )
    return result.returncode == 0, result.stderr


def _push_with_retry(base_dir, max_attempts=5):
    """push 시도 → 원격이 앞서가 거부되면(다른 배포/수동 push와 경합) 최신으로
    rebase 후 재시도한다. 이 스크립트가 만드는 커밋은 버전 macro/바이너리 등
    자기 device 폴더 파일만 바꾸므로 다른 변경과 충돌할 가능성이 낮아 rebase로
    안전하게 재시도할 수 있다. 진짜 충돌(같은 device를 동시에 두 곳에서 배포하는
    경우 등)이면 rebase가 실패하므로 그 자리에서 중단하고 사람이 확인하게 한다."""
    branch = _get_current_branch(base_dir)
    for attempt in range(1, max_attempts + 1):
        if subprocess.run(["git", "-C", base_dir, "push"]).returncode == 0:
            return
        if attempt == max_attempts:
            break
        print(f"   ⚠️ push 거부됨(원격이 앞서감) — 최신으로 재동기화 후 재시도 ({attempt}/{max_attempts})")
        subprocess.run(["git", "-C", base_dir, "fetch", "origin", branch], check=True)

        # update.bin/update.sig처럼 git엔 추적되지만 이 커밋에는 안 실은 파일이
        # (Release 에셋으로만 올라가고 git 커밋 대상이 아님) 빌드 단계에서 로컬에
        # 수정된 채로 남아있으면, rebase가 "unstaged changes"로 거부된다.
        # 커밋하지 않고(레거시처럼 바이너리를 git 히스토리에 남기지 않도록) 잠깐
        # stash로 치웠다가 rebase 직후 그대로 복원한다.
        stash_needed = subprocess.run(["git", "-C", base_dir, "diff", "--quiet"]).returncode != 0
        if stash_needed:
            subprocess.run(
                ["git", "-C", base_dir, "stash", "push", "-u", "-m", "push-retry: build artifacts"],
                check=True,
            )

        rebase_ok = subprocess.run(["git", "-C", base_dir, "rebase", f"origin/{branch}"]).returncode == 0
        if not rebase_ok:
            subprocess.run(["git", "-C", base_dir, "rebase", "--abort"])

        if stash_needed:
            if subprocess.run(["git", "-C", base_dir, "stash", "pop"]).returncode != 0:
                raise RuntimeError("rebase 후 build artifacts(stash) 복원 실패 — 수동 확인이 필요합니다.")

        if not rebase_ok:
            raise RuntimeError("원격 변경과 충돌해 자동 rebase 실패 — 수동 확인이 필요합니다.")
    raise RuntimeError(f"{max_attempts}회 재시도했지만 push에 실패했습니다.")


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
        _push_with_retry(base_dir)
        print("✅ GitHub 업로드 완료!")
    except (subprocess.CalledProcessError, RuntimeError) as e:
        print(f"❌ Git 오류: {e}")


# ============================================================
# GitHub Releases 기반 배포 (secrets.py 에 GITHUB_TOKEN 이 있을 때만)
#   — raw 파일을 main 브랜치에 커밋하는 대신, 기기(폴더)별 고정 태그
#     Release 에 바이너리를 asset 으로 올린다. 기기는 항상
#     .../releases/download/<고정 태그>/<file> 을 바라보므로
#     브랜치 전략(main 외 브랜치 사용)과 OTA 배포가 분리된다.
#
#   태그를 "latest"로 하지 않는 이유: 한 저장소에 여러 기기 종류(폴더)가
#   들어가면 "latest"는 저장소 전체에서 하나뿐이라, 다른 기기를 배포하는
#   순간 이 기기가 그 기기의 바이너리를 받아버린다. 그래서 태그를 기기
#   폴더명 그대로(예: HAS1_itembox) 고정 채널로 쓰고, 배포마다 그 태그의
#   Release를 계속 덮어쓴다(재시도 시 태그 충돌을 재사용하는 로직을
#   그대로 상시 메커니즘으로 씀).
# ============================================================

def _get_remote_repo(base_dir):
    result = subprocess.run(
        ["git", "-C", base_dir, "remote", "get-url", "origin"],
        capture_output=True, text=True, check=True
    )
    url = result.stdout.strip()
    m = re.search(r'github\.com[:/]([^/]+)/([^/.]+?)(\.git)?$', url)
    if not m:
        raise ValueError(f"origin이 GitHub 저장소가 아닙니다: {url}")
    return m.group(1), m.group(2)


def _get_current_branch(base_dir):
    result = subprocess.run(
        ["git", "-C", base_dir, "rev-parse", "--abbrev-ref", "HEAD"],
        capture_output=True, text=True, check=True
    )
    return result.stdout.strip()


def _github_api_request(url, token, method="GET", data=None, content_type="application/json"):
    headers = {
        "Authorization": f"Bearer {token}",
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "SecureOTA-deploy",
    }
    if data is not None:
        headers["Content-Type"] = content_type
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req) as resp:
        body = resp.read()
        return json.loads(body) if body else None


def _commit_and_push_sketch(base_dir, sketch_file, version, partition_version):
    print("\n📝 소스 변경사항(.ino 버전 증가) 커밋 중...")
    rel_sketch = os.path.relpath(sketch_file, base_dir).replace("\\", "/")
    commit_msg = f"Firmware v{version}"
    if partition_version is not None:
        commit_msg += f" + Partition v{partition_version}"
    try:
        subprocess.run(["git", "-C", base_dir, "add", rel_sketch], check=True)
        if subprocess.run(["git", "-C", base_dir, "diff", "--cached", "--quiet"]).returncode == 0:
            print("   변경사항 없음 — 커밋 스킵")
            return
        subprocess.run(["git", "-C", base_dir, "commit", "-m", commit_msg], check=True)
        _push_with_retry(base_dir)
        print("✅ 소스 커밋 & 푸시 완료")
    except (subprocess.CalledProcessError, RuntimeError) as e:
        print(f"❌ Git 오류: {e}")
        raise


def _generate_changelog(base_dir, version, max_entries=20):
    """직전 배포(Firmware v{version-1} 커밋) 이후 이 기기 폴더(base_dir)에 걸린
    커밋들의 제목 줄만 간단히 모아 변경 로그를 만든다. --oneline은 커밋 메시지의
    첫 줄(제목)만 뽑아주므로 자동으로 "간략한" 목록이 된다. 직전 버전 커밋을
    못 찾으면(첫 배포 등) 최근 max_entries개까지만 보여준다."""
    try:
        result = subprocess.run(
            ["git", "-C", base_dir, "log", "--oneline", "--no-decorate", "--", "."],
            capture_output=True, text=True, check=True,
            encoding="utf-8",  # Windows 기본 로케일(cp949)로 디코드하면 한글 커밋 메시지에서 깨짐
        )
    except subprocess.CalledProcessError:
        return ""

    prev_bump = re.compile(rf'^[0-9a-f]+\s+Firmware v{version - 1}(\s|\+|$)')
    this_bump = re.compile(rf'^[0-9a-f]+\s+Firmware v{version}(\s|\+|$)')

    entries = []
    for line in result.stdout.strip().splitlines():
        if prev_bump.match(line):
            break  # 직전 배포 지점 — 그 이전 커밋은 이미 지난 릴리즈에서 다뤄짐
        if this_bump.match(line):
            continue  # 이번 배포 자체의 버전 범프 커밋은 "변경 내용"이 아니라서 제외
        entries.append(re.sub(r'^[0-9a-f]+\s+', '', line))
        if len(entries) >= max_entries:
            break

    return "\n".join(f"- {e}" for e in entries)


def _publish_release(base_dir, version, partition_version, github_token):
    print("\n☁️ GitHub Release 로 업로드 중...")
    owner, repo = _get_remote_repo(base_dir)
    branch = _get_current_branch(base_dir)
    tag = os.path.basename(base_dir)

    with open(os.path.join(base_dir, "version.txt"), "w", encoding="utf-8") as f:
        f.write(str(version))
    assets = ["update.bin", "update.sig", "version.txt"]
    name = f"Firmware v{version}"

    if partition_version is not None:
        with open(os.path.join(base_dir, "partition_version.txt"), "w", encoding="utf-8") as f:
            f.write(str(partition_version))
        assets += ["partitions.bin", "partitions.sig", "partition_version.txt"]
        name += f" + Partition v{partition_version}"

    changelog = _generate_changelog(base_dir, version)
    body = f"{name}\n\n{changelog}" if changelog else name

    release_payload = json.dumps({
        "tag_name": tag,
        "target_commitish": branch,
        "name": name,
        "body": body,
        "draft": False,
        "prerelease": False,
    }).encode("utf-8")

    try:
        release = _github_api_request(
            f"{GITHUB_API}/repos/{owner}/{repo}/releases",
            github_token, method="POST",
            data=release_payload,
        )
    except urllib.error.HTTPError as e:
        resp_body = e.read().decode("utf-8", "ignore")
        if e.code == 422 and "already_exists" in resp_body:
            # 기기 폴더명 고정 태그를 계속 재사용하는 게 정상 흐름이라(첫 배포 이후엔
            # 항상 이 분기를 탐) 매 배포마다 최신 버전/변경 로그로 제목·본문을 갱신한다.
            # (예전엔 기존 Release를 GET만 하고 안 바꿔서, 최초 배포 이후 제목/설명이
            #  영원히 그대로 남아있는 문제가 있었음)
            print(f"   ℹ️ Release {tag} 이미 존재 — 최신 버전/변경 로그로 갱신")
            try:
                existing = _github_api_request(
                    f"{GITHUB_API}/repos/{owner}/{repo}/releases/tags/{tag}",
                    github_token,
                )
                release = _github_api_request(
                    f"{GITHUB_API}/repos/{owner}/{repo}/releases/{existing['id']}",
                    github_token, method="PATCH",
                    data=release_payload,
                )
            except urllib.error.HTTPError as e2:
                print(f"❌ 기존 Release 갱신 실패: {e2.code} {e2.read().decode('utf-8', 'ignore')}")
                return False
        else:
            print(f"❌ Release 생성 실패: {e.code} {resp_body}")
            return False

    upload_url = release["upload_url"].split("{")[0]
    existing_assets = {a["name"]: a["id"] for a in release.get("assets", [])}
    for fname in assets:
        if fname in existing_assets:
            try:
                _github_api_request(
                    f"{GITHUB_API}/repos/{owner}/{repo}/releases/assets/{existing_assets[fname]}",
                    github_token, method="DELETE",
                )
                print(f"   🗑️  기존 {fname} 삭제 (재업로드 위해)")
            except urllib.error.HTTPError as e:
                print(f"❌ 기존 {fname} 삭제 실패: {e.code} {e.read().decode('utf-8', 'ignore')}")
                return False
        with open(os.path.join(base_dir, fname), "rb") as f:
            file_data = f.read()
        try:
            _github_api_request(
                f"{upload_url}?name={quote(fname)}",
                github_token, method="POST",
                data=file_data, content_type="application/octet-stream",
            )
            print(f"   ⬆️  {fname} ({len(file_data):,} bytes) 업로드 완료")
        except urllib.error.HTTPError as e:
            print(f"❌ {fname} 업로드 실패: {e.code} {e.read().decode('utf-8', 'ignore')}")
            return False

    print(f"✅ GitHub Release {tag} 업로드 완료! ({owner}/{repo}, branch={branch})")
    return True


def run_deploy(base_dir):
    # 비밀키 로드 (장치 repo 의 scripts/secrets.py)
    device_scripts = os.path.join(base_dir, "scripts")
    sys.path.insert(0, device_scripts)
    try:
        import secrets as _device_secrets
    except ImportError:
        print("❌ 오류: scripts/secrets.py 파일이 없습니다.")
        print("   secrets.py.example 을 secrets.py 로 복사한 뒤 비밀키를 설정하세요.")
        return
    finally:
        sys.path.pop(0)

    HMAC_SECRET = getattr(_device_secrets, "HMAC_SECRET", None)
    GITHUB_TOKEN = getattr(_device_secrets, "GITHUB_TOKEN", None) or None

    if not HMAC_SECRET or HMAC_SECRET == "CHANGE_THIS_TO_YOUR_SECRET":
        print("❌ 오류: scripts/secrets.py 의 HMAC_SECRET 을 설정하세요.")
        return

    # 스케치 파일 자동 감지 (Arduino 규칙: 폴더명 == 스케치명)
    sketch_file = _find_sketch_file(base_dir)
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

    if GITHUB_TOKEN:
        _commit_and_push_sketch(base_dir, sketch_file, new_ver, new_partition_ver)
        if not _publish_release(base_dir, new_ver, new_partition_ver, GITHUB_TOKEN):
            return
    else:
        _git_push(base_dir, sketch_file, new_ver, new_partition_ver)

    print(f"\n🎉 배포 완료! 펌웨어 v{new_ver}", end="")
    if new_partition_ver is not None:
        print(f" + 파티션 v{new_partition_ver}", end="")
    if GITHUB_TOKEN:
        print(" 이(가) GitHub Release 로 업로드되었습니다.")
    else:
        print(" 이(가) GitHub 에 업로드되었습니다.")
    print("   서버에서 device_state = \"github\" 를 전송하면 기기가 업데이트됩니다.")
