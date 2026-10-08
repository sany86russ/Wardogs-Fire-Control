"""Capture the actual application theme only on an isolated Windows CI runner."""
import json
import os
import pathlib
import struct
import subprocess


def main():
    if os.name != 'nt' or os.environ.get('GITHUB_ACTIONS') != 'true' or os.environ.get('RUNNER_OS') != 'Windows':
        raise RuntimeError('Application snapshots are permitted only on the isolated GitHub Windows runner')
    root = pathlib.Path(os.environ['GITHUB_WORKSPACE']).resolve()
    build = root / 'source/build/release'
    exe = (build / 'WarDogsDistanceCalculator.exe').resolve(strict=True)
    evidence = (build / 'Testing/planning-app').resolve()
    if not exe.is_relative_to(root) or not evidence.is_relative_to(root):
        raise RuntimeError('Snapshot inputs and output must stay in the CI checkout')
    evidence.mkdir(parents=True, exist_ok=True)
    for language in ('ru', 'en'):
        for mode in ('planning', 'planning-positions', 'planning-times', 'planning-profiles'):
            path = evidence / f'{language}-{mode}.png'
            subprocess.run([str(exe), f'--language={language}', f'--{mode}-ui-snapshot={path}'],
                           check=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
            data = path.read_bytes()
            if data[:8] != b'\x89PNG\r\n\x1a\n' or len(data) < 1000:
                raise RuntimeError(f'Invalid screenshot: {path.name}')
            width, height = struct.unpack('>II', data[16:24])
            if width < 400 or height < 300:
                raise RuntimeError(f'Unexpected screenshot dimensions: {path.name}')
            receipt = json.loads(path.with_suffix('.png.json').read_text(encoding='utf-8'))
            if receipt.get('language') != language:
                raise RuntimeError(f'Unexpected snapshot language: {path.name}')
            print(f'{path.name}: {width}x{height}, actual application theme, temporary data only')


if __name__ == '__main__':
    main()
