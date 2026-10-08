"""Check the embedded RU/EN catalogue against user-facing source literals."""
import argparse
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parents[1]
LITERALS = re.compile(r'QStringLiteral\(((?:\s*"(?:[^"\\]|\\.)*"\s*)+)\)')
STRINGS = re.compile(r'"(?:[^"\\]|\\.)*"')
CYRILLIC = re.compile(r'[А-Яа-яЁё]')
PLACEHOLDERS = re.compile(r'%L?(?:\d+|n)')
# Scan strings independently of their C++/Qt wrapper, including wide literals
# and raw-string syntax. Comment and character tokens are consumed so quoted
# examples in comments cannot be mistaken for application messages.
CPP_TOKENS = re.compile(
    r'(?P<comment>//[^\n]*|/\*[\s\S]*?\*/)'
    r'|(?P<raw>(?:u8|u|U|L)?R"(?P<delimiter>[^\s()\\]{0,16})\([\s\S]*?\)(?P=delimiter)")'
    r'|(?P<string>(?:u8|u|U|L)?"(?:\\[\s\S]|[^"\\])*")'
    r"|(?P<char>(?:u8|u|U|L)?'(?:\\[\s\S]|[^'\\])*')"
)
NON_UI_LITERALS = {
    # OCR recognizes these game inputs; they are not displayed application UI.
    ('src/ocr.cpp', 'ВСЕ'),
    ('src/ocr.cpp', 'xXхХyYуУ'),
}


def without_comments(content):
    return CPP_TOKENS.sub(
        lambda match: re.sub(r'[^\n]', ' ', match[0])
        if match.lastgroup == 'comment' else match[0], content)


def decoded_string(token):
    if re.match(r'(?:u8|u|U|L)?R"', token):
        opening = token.index('(')
        delimiter = token[token.index('"') + 1:opening]
        return token[opening + 1:-(len(delimiter) + 2)]
    return json.loads(token[token.index('"'):])


def raw_literals(path):
    content = path.read_text(encoding='utf-8-sig')
    for match in CPP_TOKENS.finditer(content):
        if match.lastgroup not in {'string', 'raw'}:
            continue
        # JSON decoding suffices for display messages. Do not silently ignore
        # an unsupported C++ escape if a future Cyrillic message introduces it.
        if not CYRILLIC.search(match[0]):
            continue
        value = decoded_string(match[0])
        yield value, content.count('\n', 0, match.start()) + 1


def literals(path):
    content = without_comments(path.read_text(encoding='utf-8-sig'))
    for match in LITERALS.finditer(content):
        value = ''.join(json.loads(token) for token in STRINGS.findall(match[1]))
        if CYRILLIC.search(value):
            yield value, content.count('\n', 0, match.start()) + 1


def validate_qt_catalog(path, entries, failures):
    messages = {}
    for index, entry in enumerate(entries):
        context, source, target = entry.get('context'), entry.get('source'), entry.get('translation')
        if not isinstance(context, str) or not context or not isinstance(source, str) or not source:
            failures.append(f'{path.name}:{index}: missing Qt context/source')
            continue
        forms = target if isinstance(target, list) else [target]
        if not forms or any(not isinstance(value, str) or not value for value in forms):
            failures.append(f'{path.name}:{index}: empty Qt translation: {context}/{source}')
            continue
        key = context, source
        if key in messages and messages[key] != target:
            failures.append(f'{path.name}:{index}: conflicting Qt translation: {context}/{source}')
        messages[key] = target
        for value in forms:
            if sorted(PLACEHOLDERS.findall(source)) != sorted(PLACEHOLDERS.findall(value)):
                failures.append(f'{path.name}:{index}: Qt placeholder mismatch: {context}/{source}')
    return len(messages)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--extract-main', action='store_true')
    args = parser.parse_args()
    directory = ROOT / 'translations'
    directory.mkdir(exist_ok=True)
    if args.extract_main:
        values = dict(literals(ROOT / 'src/main_window.cpp'))
        (directory / 'main.todo.json').write_text(json.dumps(
            [{'ru': value, 'en': ''} for value in values], ensure_ascii=False,
            indent=2), encoding='utf-8')
        print(f'Extracted {len(values)} main window messages')
        return
    catalog = {}
    qt_count = 0
    failures = []
    for path in sorted(directory.glob('*.json')):
        if path.name.endswith('.todo.json'):
            continue
        entries = json.loads(path.read_text(encoding='utf-8-sig'))
        if path.name == 'qtbase_ru.json':
            qt_count += validate_qt_catalog(path, entries, failures)
            continue
        for entry in entries:
            source, target = entry['ru'], entry['en']
            if not source or not target or CYRILLIC.search(target):
                failures.append(f'{path.name}: empty or Cyrillic English translation: {source}')
            if sorted(PLACEHOLDERS.findall(source)) != sorted(PLACEHOLDERS.findall(target)):
                failures.append(f'{path.name}: placeholder mismatch: {source}')
            if source in catalog and catalog[source] != target:
                failures.append(f'{path.name}: conflicting translation: {source}')
            catalog[source] = target
    sources = sorted((ROOT / 'src').glob('*.cpp')) + sorted((ROOT / 'include/wardogs').glob('*.hpp'))
    raw_count = 0
    for path in sources:
        if path.name == 'localization.cpp':
            continue
        for source, line in literals(path):
            if source not in catalog:
                failures.append(f'{path.name}:{line}: missing translation: {source}')
        for source, line in raw_literals(path):
            raw_count += 1
            if (path.relative_to(ROOT).as_posix(), source) in NON_UI_LITERALS:
                continue
            # Adjacent C++ literals are extracted individually here. They must
            # be covered by a whole catalog message or one of its fragments;
            # QStringLiteral above still requires an exact joined message.
            if not any(source in message for message in catalog):
                failures.append(f'{path.relative_to(ROOT)}:{line}: missing raw literal translation: {source}')
    for failure in failures:
        print(failure)
    print(f'{len(catalog)} application messages; {qt_count} Qt stock messages; '
          f'{raw_count} raw Cyrillic literals checked; {len(failures)} defects')
    raise SystemExit(bool(failures))


if __name__ == '__main__':
    main()
