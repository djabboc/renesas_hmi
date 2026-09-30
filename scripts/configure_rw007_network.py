"""Add the demo's single-thread lwIP sources to the local Studio build settings.

The project keeps .cproject untracked. This idempotent step makes the network
demo reproducible without enabling RT-Thread's separate socket/WLAN stack.
"""
from pathlib import Path
import xml.etree.ElementTree as ET


def configure(project: Path) -> None:
    path = project / ".cproject"
    tree = ET.parse(path)
    root = tree.getroot()
    base = project / "rt-thread/components/net"
    source = base / "lwip/lwip-2.1.2/src"
    selected = set((source / "core").glob("*.c"))
    selected.update((source / "core/ipv4").glob("*.c"))
    selected.add(source / "netif/ethernet.c")
    excludes: list[str] = []

    def walk(directory: Path) -> None:
        for child in sorted(directory.iterdir()):
            if child.is_dir():
                if any(child in item.parents for item in selected):
                    walk(child)
                else:
                    excludes.append("//" + child.relative_to(project).as_posix())
            elif child.suffix in {".c", ".cpp", ".S", ".s"} and child not in selected:
                excludes.append("//" + child.relative_to(project).as_posix())

    walk(base)
    for entry in root.findall(".//sourceEntries/entry"):
        if entry.get("name") != "":
            continue
        old = entry.get("excluding", "").split("|")
        preserved = [value for value in old if not value.lstrip("/").startswith("rt-thread/components/net")]
        entry.set("excluding", "|".join(preserved + excludes))
    for option in root.findall(".//option[@valueType='includePath']"):
        if ".compiler.include.paths" not in option.get("superClass", ""):
            continue
        for folder in ("board/rw007_net", "rt-thread/components/net/lwip/lwip-2.1.2/src/include"):
            value = '"${workspace_loc://${ProjName}//' + folder + '}"'
            if not any(item.get("value") == value for item in option):
                ET.SubElement(option, "listOptionValue", builtIn="false", value=value)
    updated = b'<?xml version="1.0" encoding="UTF-8"?>\n<?fileVersion 4.0.0?>\n' + ET.tostring(root, encoding="utf-8")
    if path.read_bytes() != updated:
        path.write_bytes(updated)


if __name__ == "__main__":
    configure(Path(__file__).resolve().parent.parent)
