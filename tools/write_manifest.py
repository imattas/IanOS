from __future__ import annotations

import hashlib
import pathlib
import sys


def digest(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main() -> int:
    out = pathlib.Path(sys.argv[1])
    lines = ["IanOS build manifest"]
    for raw in sys.argv[2:]:
        path = pathlib.Path(raw)
        lines.append(f"{path.name} size={path.stat().st_size} sha256={digest(path)}")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
