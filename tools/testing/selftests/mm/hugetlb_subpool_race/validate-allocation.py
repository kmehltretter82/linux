#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path


def require(text: str, pattern: str, label: str, errors: list[str]) -> None:
    if re.search(pattern, text, re.MULTILINE) is None:
        errors.append(f"missing {label}: {pattern}")


def main() -> int:
    if len(sys.argv) != 3 or sys.argv[2] not in {"1", "4"}:
        print("usage: validate.py CONSOLE 1|4", file=sys.stderr)
        return 2
    text = Path(sys.argv[1]).read_text(errors="replace")
    cpus = sys.argv[2]
    errors: list[str] = []
    expected = [
        (r"^ALLOCPROOF-RESULT PASS$", "guest pass result"),
        (r"^ALLOCPROOF-DONE PASS$", "guest completion"),
        (rf"^ALLOCPROOF-BOOT cpus={cpus} hpage=2097152 ", "boot identity"),
        (r"^ALLOCPROOF-START target_pid=[0-9]+$", "controller identity"),
        (r"hugetlb-p4-test: pause stage=1 .* chg=1 get=1 accounted=0 total=7 free=7 resv=4 used=5 subpool_resv=0", "pre-allocation pause"),
        (r"^ALLOCPROOF-POOL stage=pause1-after-fill total=7 free=7 reserved=7 surplus=0$", "first capacity fill"),
        (r"hugetlb-p4-test: pause stage=2 .* chg=1 get=1 accounted=0 total=7 free=7 resv=7 used=5 subpool_resv=0", "failed-allocation pause"),
        (r"^ALLOCPROOF-POOL stage=pause2-hold-reduced total=7 free=7 reserved=6 surplus=0$", "same-subpool release"),
        (r"^ALLOCPROOF-POOL stage=pause2-after-second-fill total=7 free=7 reserved=7 surplus=0$", "released-capacity theft"),
        (r"hugetlb-p4-test: cleanup .* chg=1 get=1 put=0 accounted=0 delta=1 ret=-12 total=7 free=7 resv=7 used=3 subpool_resv=1", "failed positive correction"),
        (r"^ALLOCPROOF-TARGET mapped=1 sigbus=1 errno=0 fail_hits=0 timed_out=0 stage=3 reserved=7$", "target fault result"),
        (r"^ALLOCPROOF-ASSERT positive-correction-failed delta=1 visible_reserved=7 result=PASS$", "proof assertion"),
        (r"^ALLOCPROOF-POOL stage=files-removed total=7 free=7 reserved=3 surplus=0$", "visible under-backing"),
        (r"^ALLOCPROOF-POOL stage=unmounted total=7 free=7 reserved=18446744073709551615 surplus=0$", "unmount underflow"),
        (r"^ALLOCPROOF-UNMOUNT-CHECK reserved=18446744073709551615 expected=ULONG_MAX result=PASS$", "underflow assertion"),
    ]
    for pattern, label in expected:
        require(text, pattern, label, errors)

    diagnostic = re.search(
        r"BUG:|WARNING:|Oops:|Kernel panic|KASAN:|UBSAN:|Bad page", text
    )
    if diagnostic:
        errors.append(f"kernel diagnostic present: {diagnostic.group(0)}")
    result = {
        "result": "PASS" if not errors else "FAIL",
        "variant": "full-v3-allocation-cleanup",
        "vcpus": int(cpus),
        "errors": errors,
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
