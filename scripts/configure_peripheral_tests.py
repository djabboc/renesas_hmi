"""为本机 Studio 工程补全测试依赖的 include 路径；重复执行不增加重复项。"""

from pathlib import Path
import xml.etree.ElementTree as ET


def configure(project: Path) -> None:
    # .cproject 不入库，由构建脚本在本机按需补全。
    path = project / ".cproject"
    tree = ET.parse(path)
    root = tree.getroot()
    for entry in root.findall(".//sourceEntries/entry"):
        if entry.get("name") == "":
            excluded = entry.get("excluding", "").split("|")
            for folder in ("//logs", "//workspace", "//scripts"):
                if folder not in excluded:
                    excluded.append(folder)
            entry.set("excluding", "|".join(excluded))
    folders = (
        "board",
        "third_party/tinyusb/src",
        "rt-thread/components/dfs/dfs_v1/filesystems/elmfat",
        "ra/tes/dave2d/inc",
    )
    # 只修改 C/C++ 编译器搜索路径，不触碰汇编器或链接器选项。
    for option in root.findall(".//option[@valueType='includePath']"):
        if ".compiler.include.paths" not in option.get("superClass", ""):
            continue
        for folder in folders:
            value = '"${workspace_loc://${ProjName}//' + folder + '}"'
            if not any(item.get("value") == value for item in option):
                ET.SubElement(option, "listOptionValue", builtIn="false", value=value)
    data = b'<?xml version="1.0" encoding="UTF-8"?>\n<?fileVersion 4.0.0?>\n' + ET.tostring(
        root, encoding="utf-8"
    )
    if path.read_bytes() != data:
        path.write_bytes(data)


if __name__ == "__main__":
    configure(Path(__file__).resolve().parent.parent)
