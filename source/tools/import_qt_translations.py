"""Extract selected Qt Widgets Russian messages from a local official TS file."""
from __future__ import annotations

import argparse
import json
import pathlib
import xml.etree.ElementTree as ET

CONTEXTS = frozenset({
    "QAbstractSpinBox", "QAccessibleActionInterface", "QComboBox",
    "QDialogButtonBox", "QFileDialog", "QFileSystemModel", "QLineEdit",
    "QMessageBox", "QPlatformTheme", "QScrollBar", "QTabBar",
    "QWidgetTextControl",
})


def extract(source: pathlib.Path) -> list[dict]:
    root = ET.parse(source).getroot()
    if root.tag != "TS" or root.get("language", "").split("_")[0] != "ru":
        raise ValueError("Expected a Russian Qt Linguist TS document")
    messages = []
    seen = {}
    for context in root.findall("context"):
        name = context.findtext("name", "")
        if name not in CONTEXTS:
            continue
        for message in context.findall("message"):
            translation = message.find("translation")
            if translation is None or translation.get("type") in {"unfinished", "obsolete", "vanished"}:
                continue
            original = message.findtext("source", "")
            forms = translation.findall("numerusform")
            value = ["".join(form.itertext()) for form in forms] if forms else "".join(translation.itertext())
            if not original or not value or (isinstance(value, list) and not all(value)):
                continue
            key = name, original
            if key in seen:
                if seen[key] != value:
                    raise ValueError(f"Conflicting context/source translations: {name}/{original}")
                continue
            seen[key] = value
            messages.append({"context": name, "source": original, "translation": value})
    if not messages:
        raise ValueError("No completed translations found in the selected contexts")
    return messages


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path, help="Local qtbase_ru.ts file")
    parser.add_argument("output", type=pathlib.Path, help="Extracted JSON catalog")
    args = parser.parse_args()
    entries = extract(args.source)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(entries, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Extracted {len(entries)} Qt stock messages from {args.source}")


if __name__ == "__main__":
    main()
