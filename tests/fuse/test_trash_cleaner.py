import os
import argparse
import time
import shutil
from datetime import datetime, timedelta, timezone

UTC8_TZ = timezone(timedelta(hours=8))
DATE_FORMAT = "%Y%m%d_%H%M"


def format_date(t: datetime) -> str:
    assert t.tzinfo
    return t.astimezone(tz=UTC8_TZ).strftime(DATE_FORMAT)


def make_trash(trash_dir: str, mode=0o755):
    tmp_path = f"{trash_dir}.tmp"
    os.makedirs(tmp_path)
    for i in range(0, 10):
        os.makedirs(os.path.join(tmp_path, f"subdir-{i}"))
    for i in range(0, 10):
        with open(os.path.join(tmp_path, "file-{i}"), "w") as f:
            f.write("some txt")
    os.chmod(tmp_path, mode)
    os.rename(tmp_path, trash_dir)
    return trash_dir


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("trash", type=str, help="path to trash directory")
    args = parser.parse_args()
    trash = args.trash

    invalid_dir = make_trash(os.path.join(trash, "invalid_name"))
    nonutf8_dir = os.path.join(trash.encode('utf8'), b"\xee\x80\x81\x82")
    os.mkdir(nonutf8_dir)

    now = datetime.now(tz=UTC8_TZ)
    befores = [
        now - 2 * timedelta(minutes=10),
        now - timedelta(minutes=10),
    ]
    afters = [now + timedelta(minutes=10), now + 2 * timedelta(minutes=10)]

    expired = os.path.join(
        trash, f"expired-{format_date(befores[0])}-{format_date(befores[1])}"
    )
    make_trash(expired)
    expired_no_perm = os.path.join(
        trash, f"expired_no_perm-{format_date(befores[0])}-{format_date(befores[1])}"
    )
    make_trash(expired_no_perm, 0o000)
    expired_invalid = os.path.join(
        trash, f"expired-invalid-{format_date(befores[0])}-{format_date(befores[1])}"
    )
    make_trash(expired_invalid)
    expired_file = os.path.join(
        trash, f"expiredfile-{format_date(befores[0])}-{format_date(befores[1])}"
    )
    with open(expired_file, "w") as f:
        f.write(expired_file)
    expired_invalid_ts = os.path.join(
        trash, f"expired-{format_date(befores[1])}-{format_date(befores[0])}"
    )
    make_trash(expired_invalid_ts)

    notexpired = [
        os.path.join(
            trash, f"notexpired-{format_date(befores[1])}-{format_date(afters[0])}"
        ),
        os.path.join(
            trash, f"notexpired-{format_date(afters[0])}-{format_date(afters[1])}"
        ),
    ]
    for p in notexpired:
        make_trash(p)

    print("sleep 180s")
    time.sleep(180)

    exists = [
        nonutf8_dir,
        invalid_dir, expired_no_perm, expired_invalid, expired_invalid_ts, expired_file
    ] + notexpired
    notexists = [ expired ]

    print("check exists")
    for e in exists:
        assert os.path.exists(e), f"{e} not exists"
    print("check not exists")
    for ne in notexists:
        assert not os.path.exists(ne), f"{ne} exists"
    print("test finished, clean")
    for e in exists:
        os.chmod(e, 0o777)
        if os.path.isdir(e):
            shutil.rmtree(e)
        else:
            os.unlink(e)
    print("clean finished")