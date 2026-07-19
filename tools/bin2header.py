from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 5:
        print("usage: bin2header.py <input.bin> <output.hpp> <namespace> <symbol>", file=sys.stderr)
        return 1
    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    namespace = sys.argv[3]
    symbol = sys.argv[4]
    data = source.read_bytes()
    output.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#pragma once",
        "#include <stddef.h>",
        "#include <stdint.h>",
        f"namespace {namespace} {{",
        f"constexpr uint8_t {symbol}[] = {{",
    ]
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i:i + 12])
        lines.append(f"    {chunk},")
    lines.extend([
        "};",
        f"constexpr size_t {symbol}_size = sizeof({symbol});",
        f"}} // namespace {namespace}",
        "",
    ])
    output.write_text("\n".join(lines), encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
