"""Write the resource fixtures used by the core's tests (tests/fixtures/).

The fixtures exercise what the embedding and loading must survive: tabs,
quotes, backslashes, digits after non-ASCII bytes (octal escapes), a BOM,
CRLF line ends, comments and a long line. Run `make embed-fixtures` after
changing this file; CI checks the generated files are up to date.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent / "tests" / "fixtures" / "resources"

SAMPLE = (
    "﻿code\tname\tnote\theight\r\n"
    "# comment line\r\n"
    "BR\tBrasíl\t\"quoted\" \\ back\\slash\t173.6\r\n"
    "JP\t日本\té1é2 digits after accents\t172.1\r\n"
    "XX\tLong\t" + "ab\\\"" * 300 + "\t1\r\n"
)

WORDS = "Deer\nOtter\nCafé\n"


def main():
    (ROOT / "lite").mkdir(parents=True, exist_ok=True)
    (ROOT / "lite" / "sample.tsv").write_bytes(SAMPLE.encode("utf-8"))
    (ROOT / "lite" / "animals.words").write_bytes(WORDS.encode("utf-8"))
    print(f"wrote fixtures under {ROOT}")


if __name__ == "__main__":
    main()
