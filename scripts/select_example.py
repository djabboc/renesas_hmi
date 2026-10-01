"""选择唯一 INIT_APP_EXPORT；修改后必须重新构建、烧录。"""

import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
PROFILES = {
    "peripheral": ("peripheral-test.c", "peripheral_test_start"),
    "lcd": ("lcd.c", "lcd_test_start"),
    "touch": ("touch.c", "gt911_demo_start"),
    "lcd-touch": ("lcd-touch.c", "lcd_touch_start"),
    "lcd-lvgl": ("lcd-lvgl.c", "lcd_lvgl_start"),
    "lcd-touch-lvgl": ("lcd-touch-lvgl.c", "lcd_touch_lvgl_start"),
    "ble": ("ble.c", "ble_start"),
    "rw007": ("rwoo7-demo.c", "rwoo7_demo_start"),
}
ENTRY_PATTERN = re.compile(r"^(?://\s*)?INIT_APP_EXPORT\((\w+)\);", re.MULTILINE)
ACTIVE_PATTERN = re.compile(r"^INIT_APP_EXPORT\((\w+)\);", re.MULTILINE)


def select(profile: str) -> None:
    """先校验目标入口，再屏蔽其他入口，最后核对唯一有效入口。"""
    filename, function = PROFILES[profile]
    files = list((ROOT / "src").glob("*.c"))
    selected = ROOT / "src" / filename
    if not selected.is_file():
        raise RuntimeError(f"Missing source: {selected}")
    if function not in ENTRY_PATTERN.findall(selected.read_text(encoding="utf-8")):
        raise RuntimeError(f"Missing entry: {function}")

    for path in files:
        source = path.read_text(encoding="utf-8")

        def replace_entry(match: re.Match) -> str:
            enabled = path == selected and match[1] == function
            prefix = "" if enabled else "// "
            return f"{prefix}INIT_APP_EXPORT({match[1]});"

        updated = ENTRY_PATTERN.sub(replace_entry, source)
        if updated != source:
            path.write_text(updated, encoding="utf-8")

    # 测试套件拥有 LED；其他持续例程继续使用原有主线程行为。
    (ROOT / "board/test-profile.h").write_text(
        "/* Updated by scripts/select_example.py. */\n"
        f'#define HMI_TEST_SUITE {int(profile == "peripheral")}\n',
        encoding="utf-8",
    )
    active_entries = []
    for path in files:
        for entry in ACTIVE_PATTERN.findall(path.read_text(encoding="utf-8")):
            active_entries.append((path.name, entry))
    if active_entries != [(filename, function)]:
        raise RuntimeError(f"Unexpected active entries: {active_entries}")
    print(f"Selected {profile}: {filename} / {function}. Rebuild and flash required.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=PROFILES)
    select(parser.parse_args().profile)
