"""显示原例程对应的新命令；固件内通过 MSH 选择，无需再改初始化宏。"""

import argparse

PROFILES = {
    "peripheral": "hmi_test help",
    "lcd": "hmi_test lcd-colors",
    "touch": "hmi_test touch-points",
    "lcd-touch": "hmi_test lcd-touch",
    "lcd-lvgl": "hmi_test lcd-lvgl",
    "lcd-touch-lvgl": "hmi_test lcd-touch-lvgl",
    "ble": "hmi_test rw007-adv",
    "rw007": "hmi_test rw007-info",
}


def select(profile: str) -> None:
    print(f"在板卡串口执行：{PROFILES[profile]}")
    print("每条测试命令新建线程；运行期间其他测试会被拒绝。")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", choices=PROFILES)
    select(parser.parse_args().profile)
