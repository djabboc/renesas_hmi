"""Idempotently add the bounded test suite's dependencies to RT-Thread Studio."""
from pathlib import Path
import xml.etree.ElementTree as ET


def configure(project: Path) -> None:
    path = project / '.cproject'
    tree = ET.parse(path)
    root = tree.getroot()
    folders = ('board', 'third_party/tinyusb/src',
               'rt-thread/components/dfs/dfs_v1/filesystems/elmfat',
               'ra/tes/dave2d/inc')
    for option in root.findall(".//option[@valueType='includePath']"):
        if '.compiler.include.paths' not in option.get('superClass', ''):
            continue
        for folder in folders:
            value = '"${workspace_loc://${ProjName}//' + folder + '}"'
            if not any(item.get('value') == value for item in option):
                ET.SubElement(option, 'listOptionValue', builtIn='false', value=value)
    data = b'<?xml version="1.0" encoding="UTF-8"?>\n<?fileVersion 4.0.0?>\n' + ET.tostring(root, encoding='utf-8')
    if path.read_bytes() != data:
        path.write_bytes(data)


if __name__ == '__main__':
    configure(Path(__file__).resolve().parent.parent)
