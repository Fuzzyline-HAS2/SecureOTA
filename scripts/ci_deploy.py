"""
ci_deploy.py — GitHub Actions용 non-interactive 배포 오케스트레이션

deploy_core.py 의 기존 helper(_get_version, _increment_version, _sign,
_publish_release, _commit_and_push_sketch)를 그대로 재사용한다. run_deploy()는
input() 프롬프트로 로컬 Arduino IDE 컴파일을 기다리므로 CI에서는 쓸 수 없어
별도 스크립트로 분리했다 — 컴파일(arduino-cli compile)은 워크플로에서 이
스크립트의 bump 와 publish 사이에 실행된다.

사용법:
    python ci_deploy.py bump <base_dir> [--partition]
        .ino 의 FIRMWARE_VER(및 --partition 시 PARTITION_VER) 를 증가시키고
        "NEW_VER=<n>" / "NEW_PARTITION_VER=<n 또는 빈 문자열>" 을 stdout 에 출력.
        (arduino-cli compile 전에 실행 — 새 버전이 바이너리에 임베드돼야 함)

    python ci_deploy.py publish <base_dir> <version> [<partition_version>]
        환경변수 HMAC_SECRET / GITHUB_TOKEN 필요.
        update.bin(+partitions.bin) 서명 → GitHub Release 업로드 →
        .ino 버전 변경분 커밋 & 푸시.
"""

import os
import sys

_CORE_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _CORE_DIR)
import deploy_core as core


def cmd_bump(args):
    if not args:
        raise SystemExit("사용법: ci_deploy.py bump <base_dir> [--partition]")
    base_dir = args[0]
    want_partition = "--partition" in args[1:]

    sketch_file = core._find_sketch_file(base_dir)
    if not sketch_file:
        raise SystemExit(f"오류: {base_dir} 에서 .ino 파일을 찾을 수 없습니다.")

    cur_ver = core._get_version(sketch_file, core.VERSION_MACRO)
    if cur_ver is None:
        raise SystemExit(f"오류: '#define {core.VERSION_MACRO}' 를 찾을 수 없습니다.")
    new_ver = core._increment_version(sketch_file, core.VERSION_MACRO, cur_ver)
    print(f"버전: v{cur_ver} -> v{new_ver}", file=sys.stderr)

    new_partition_ver = ""
    if want_partition:
        cur_pver = core._get_version(sketch_file, core.PART_MACRO)
        if cur_pver is None:
            raise SystemExit(f"오류: '#define {core.PART_MACRO}' 를 찾을 수 없습니다.")
        new_partition_ver = core._increment_version(sketch_file, core.PART_MACRO, cur_pver)
        print(f"파티션 버전: v{cur_pver} -> v{new_partition_ver}", file=sys.stderr)

    print(f"NEW_VER={new_ver}")
    print(f"NEW_PARTITION_VER={new_partition_ver}")


def cmd_publish(args):
    if len(args) < 2:
        raise SystemExit("사용법: ci_deploy.py publish <base_dir> <version> [<partition_version>]")
    base_dir = args[0]
    version = int(args[1])
    partition_version = int(args[2]) if len(args) > 2 and args[2] != "" else None

    hmac_secret = os.environ.get("HMAC_SECRET")
    github_token = os.environ.get("GITHUB_TOKEN")
    if not hmac_secret:
        raise SystemExit("오류: HMAC_SECRET 환경변수가 없습니다.")
    if not github_token:
        raise SystemExit("오류: GITHUB_TOKEN 환경변수가 없습니다.")

    ok, err = core._sign(
        os.path.join(base_dir, "update.bin"), hmac_secret,
        os.path.join(base_dir, "update.sig"),
    )
    if not ok:
        raise SystemExit(f"서명 실패:\n{err}")
    print("서명 완료 -> update.sig", file=sys.stderr)

    if partition_version is not None:
        ok, err = core._sign(
            os.path.join(base_dir, "partitions.bin"), hmac_secret,
            os.path.join(base_dir, "partitions.sig"),
        )
        if not ok:
            raise SystemExit(f"파티션 서명 실패:\n{err}")
        print("서명 완료 -> partitions.sig", file=sys.stderr)

    if not core._publish_release(base_dir, version, partition_version, github_token):
        raise SystemExit("Release 업로드 실패")

    sketch_file = core._find_sketch_file(base_dir)
    if not sketch_file:
        raise SystemExit(f"오류: {base_dir} 에서 .ino 파일을 찾을 수 없습니다.")
    core._commit_and_push_sketch(base_dir, sketch_file, version, partition_version)


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ("bump", "publish"):
        print(__doc__)
        raise SystemExit(1)
    {"bump": cmd_bump, "publish": cmd_publish}[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    main()
