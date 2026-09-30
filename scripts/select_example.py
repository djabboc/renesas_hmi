"""Select exactly one INIT_APP_EXPORT; changes take effect after rebuild/flash."""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent.parent
PROFILES = {
    'peripheral': ('peripheral-test.c', 'peripheral_test_start'),
    'lcd': ('lcd.c', 'lcd_test_start'),
    'touch': ('touch.c', 'gt911_demo_start'),
    'lcd-touch': ('lcd-touch.c', 'lcd_touch_start'),
    'lcd-lvgl': ('lcd-lvgl.c', 'lcd_lvgl_start'),
    'lcd-touch-lvgl': ('lcd-touch-lvgl.c', 'lcd_touch_lvgl_start'),
    'ble': ('ble.c', 'ble_start'),
    'rw007': ('rwoo7-demo.c', 'rwoo7_demo_start'),
}


def select(profile):
    filename, function = PROFILES[profile]
    pattern = re.compile(r'^(?://\s*)?INIT_APP_EXPORT\((\w+)\);', re.M)
    files = list((ROOT / 'src').glob('*.c'))
    selected = ROOT / 'src' / filename
    if not selected.is_file() or function not in pattern.findall(selected.read_text(encoding='utf-8')):
        raise RuntimeError(f'Missing entry: {function}')
    for path in files:
        source = path.read_text(encoding='utf-8')
        updated = pattern.sub(lambda m: ('' if path == selected and m[1] == function else '// ') +
                              f'INIT_APP_EXPORT({m[1]});', source)
        if updated != source:
            path.write_text(updated, encoding='utf-8')
    (ROOT / 'board/test-profile.h').write_text(
        '/* Updated by scripts/select_example.py. */\n'
        f'#define HMI_TEST_SUITE {int(profile == "peripheral")}\n', encoding='utf-8')
    active = [(p.name, m) for p in files for m in re.findall(r'^INIT_APP_EXPORT\((\w+)\);', p.read_text(encoding='utf-8'), re.M)]
    if active != [(filename, function)]:
        raise RuntimeError(f'Unexpected active entries: {active}')
    print(f'Selected {profile}: {filename} / {function}. Rebuild and flash required.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', choices=PROFILES)
    select(parser.parse_args().profile)
