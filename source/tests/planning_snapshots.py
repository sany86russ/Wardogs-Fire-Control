"""Capture on Windows CI and verify actual application pixels without dependencies."""
import argparse
import binascii
import json
import os
import pathlib
import struct
import subprocess
import zlib


MODES = ('planning', 'planning-positions', 'planning-times', 'planning-profiles')


def read_png(path):
    """Decode the bounded RGB/RGBA PNG output produced by QWidget::grab()."""
    data = path.read_bytes()
    if len(data) > 16 * 1024 * 1024 or data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('Invalid or oversized PNG')
    position = 8
    header = None
    compressed = bytearray()
    ended = False
    while position + 12 <= len(data):
        length = struct.unpack_from('>I', data, position)[0]
        end = position + 12 + length
        if end > len(data):
            raise ValueError('Truncated PNG chunk')
        kind = data[position + 4:position + 8]
        chunk = data[position + 8:position + 8 + length]
        crc = struct.unpack_from('>I', data, position + 8 + length)[0]
        if binascii.crc32(kind + chunk) & 0xffffffff != crc:
            raise ValueError('PNG checksum mismatch')
        if kind == b'IHDR':
            if header is not None or position != 8 or length != 13:
                raise ValueError('Invalid PNG header')
            header = struct.unpack('>IIBBBBB', chunk)
        elif kind == b'IDAT':
            compressed.extend(chunk)
        elif kind == b'IEND':
            if length or end != len(data):
                raise ValueError('Invalid PNG end')
            ended = True
            break
        position = end
    if not header or not ended:
        raise ValueError('Incomplete PNG')
    width, height, depth, colour_type, compression, filtering, interlace = header
    if not (400 <= width <= 4096 and 300 <= height <= 4096 and width * height <= 8_000_000):
        raise ValueError('Unexpected screenshot dimensions')
    if depth != 8 or colour_type not in (2, 6) or compression or filtering or interlace:
        raise ValueError('Unsupported screenshot PNG encoding')
    channels = 3 if colour_type == 2 else 4
    stride = width * channels
    expected = height * (stride + 1)
    decoder = zlib.decompressobj()
    raw = decoder.decompress(compressed, expected + 1)
    if len(raw) != expected or not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
        raise ValueError('Invalid screenshot pixel data')
    pixels = bytearray(height * stride)
    for y in range(height):
        filter_type = raw[y * (stride + 1)]
        row = raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)]
        if filter_type > 4:
            raise ValueError('Unknown PNG row filter')
        start = y * stride
        for x, value in enumerate(row):
            left = pixels[start + x - channels] if x >= channels else 0
            above = pixels[start + x - stride] if y else 0
            upper_left = pixels[start + x - stride - channels] if y and x >= channels else 0
            if filter_type == 1:
                value += left
            elif filter_type == 2:
                value += above
            elif filter_type == 3:
                value += (left + above) // 2
            elif filter_type == 4:
                predictor = left + above - upper_left
                distances = (abs(predictor - left), abs(predictor - above), abs(predictor - upper_left))
                value += (left, above, upper_left)[distances.index(min(distances))]
            pixels[start + x] = value & 255
    return width, height, channels, pixels


def region_metrics(image, rectangle):
    width, height, channels, pixels = image
    x, y, area_width, area_height = rectangle
    if x < 0 or y < 0 or area_width <= 0 or area_height <= 0 or x + area_width > width or y + area_height > height:
        raise ValueError('Screenshot region lies outside the rendered image')
    histogram = [0] * 256
    white_pixels = 0
    for row in range(y, y + area_height):
        offset = (row * width + x) * channels
        for column in range(area_width):
            at = offset + column * channels
            red, green, blue = pixels[at:at + 3]
            if channels == 4:
                alpha = pixels[at + 3]
                red, green, blue = [(component * alpha + 255 * (255 - alpha)) // 255
                                    for component in (red, green, blue)]
            brightness = (2126 * red + 7152 * green + 722 * blue) // 10000
            histogram[brightness] += 1
            if brightness >= 190 and max(red, green, blue) - min(red, green, blue) <= 35:
                white_pixels += 1
    count = area_width * area_height
    cumulative = 0
    median = 0
    for brightness, frequency in enumerate(histogram):
        cumulative += frequency
        if cumulative >= (count + 1) // 2:
            median = brightness
            break
    foreground = sum(histogram[min(255, median + 100):]) if median + 100 <= 255 else 0
    return {'rectangle': list(rectangle), 'pixels': count, 'median_luminance': median,
            'white_surface_fraction': white_pixels / count,
            'foreground_pixels_delta_at_least_100': foreground}


def verify_snapshot(path, language, mode, require_geometry=True):
    image = read_png(path)
    width, height, _, _ = image
    receipt = json.loads(path.with_suffix('.png.json').read_text(encoding='utf-8'))
    errors = []
    surfaces = []

    def verify_surface(name, rectangle, foreground=False, white_bound=True):
        metrics = region_metrics(image, rectangle)
        surfaces.append({'name': name, **metrics})
        if metrics['median_luminance'] >= 80:
            errors.append(f'{name}: rendered background is light (median {metrics["median_luminance"]})')
        if white_bound and metrics['white_surface_fraction'] >= .08:
            errors.append(f'{name}: large light surface occupies {metrics["white_surface_fraction"]:.1%}')
        if foreground and metrics['foreground_pixels_delta_at_least_100'] < 20:
            errors.append(f'{name}: fewer than 20 visible pixels contrast with the background by 100')

    verify_surface('whole actual application dialog', (0, 0, width, height), foreground=True)
    if receipt.get('language') != language or receipt.get('mode') != mode:
        errors.append('Screenshot receipt language or mode mismatch')
    dpr = receipt.get('snapshot_dpr')
    if type(dpr) not in (int, float) or not 0 < dpr <= 4:
        if require_geometry:
            errors.append('Missing or invalid native snapshot DPI metadata')
        return {'file': path.name, 'width': width, 'height': height, 'surfaces': surfaces,
                'geometry_available': False, 'errors': errors}
    widgets = receipt.get('widgets', [])
    required = {'planningPageViewport': False, 'planningCoordinates': True}
    if mode == 'planning':
        required['flightTimeResult'] = True
    elif mode == 'planning-positions':
        required['planningMissionsViewport'] = False  # Empty data is valid in a fresh fixture.
    elif mode == 'planning-times':
        required['planningMeasurementsViewport'] = False
    elif mode == 'planning-profiles':
        required['planningSourcesViewport'] = True
        required['weaponProfileInfo'] = True
    for name, foreground in required.items():
        matches = [widget for widget in widgets if widget.get('name') == name and widget.get('visible')]
        if len(matches) != 1:
            errors.append(f'{name}: expected exactly one visible actual application surface')
            continue
        bounds = matches[0].get('snapshot_rect', {})
        if not all(type(bounds.get(key)) is int for key in ('x', 'y', 'width', 'height')):
            errors.append(f'{name}: native visible geometry is missing')
            continue
        # The diagnostic geometry is nonnegative. Match Qt's nearest-integer
        # pixel rounding and scale endpoints, so fractional DPI cannot add a
        # stray pixel by rounding origin and extent independently.
        left = int(bounds['x'] * dpr + .5)
        top = int(bounds['y'] * dpr + .5)
        right = int((bounds['x'] + bounds['width']) * dpr + .5)
        bottom = int((bounds['y'] + bounds['height']) * dpr + .5)
        rectangle = (left, top, right - left, bottom - top)
        verify_surface(name, rectangle, foreground=foreground, white_bound=not foreground)
    return {'file': path.name, 'width': width, 'height': height, 'surfaces': surfaces,
            'geometry_available': True, 'errors': errors}


def verify_directory(evidence, require_geometry):
    results = []
    for language in ('ru', 'en'):
        for mode in MODES:
            path = evidence / f'{language}-{mode}.png'
            try:
                result = verify_snapshot(path, language, mode, require_geometry)
            except (OSError, ValueError, TypeError, KeyError, zlib.error, struct.error) as error:
                result = {'file': path.name, 'errors': [str(error)]}
            results.append(result)
            for error in result['errors']:
                print(f'FAIL: {path.name}: {error}')
            if not result['errors']:
                print(f'PASS: {path.name}: actual application pixels are dark and readable')
    return {'boundary': 'Actual application-themed native screenshot pixels; no gameplay or long-term runtime proof.',
            'failures': sum(bool(result['errors']) for result in results), 'screenshots': results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-only', type=pathlib.Path,
                        help='Read existing PNGs/receipts without launching any application or writing files')
    parser.add_argument('--require-geometry', action='store_true',
                        help='Require current native widget bounds when verifying existing receipts')
    arguments = parser.parse_args()
    if arguments.verify_only is not None:
        report = verify_directory(arguments.verify_only.resolve(strict=True), arguments.require_geometry)
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 1 if report['failures'] else 0
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
        for mode in MODES:
            path = evidence / f'{language}-{mode}.png'
            subprocess.run([str(exe), f'--language={language}', f'--{mode}-ui-snapshot={path}'],
                           check=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
    report = verify_directory(evidence, require_geometry=True)
    (evidence / 'planning-app-contrast.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return 1 if report['failures'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
