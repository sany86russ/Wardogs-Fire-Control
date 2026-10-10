"""Check actual mini-card pixels only on the isolated GitHub Windows runner."""

import argparse
import json
import math
import os
import pathlib
import struct
import subprocess
import zlib

from planning_snapshots import read_png, region_metrics


MODES = (
    ('pinned', '--pinned-ui-snapshot'),
    ('pinned-locked', '--pinned-locked-ui-snapshot'),
    ('pinned-menu', '--pinned-menu-ui-snapshot'),
    ('vehicle-pinned', '--vehicle-pinned-ui-snapshot'),
    ('vehicle-pinned-locked', '--vehicle-pinned-locked-ui-snapshot'),
    ('vehicle-pinned-menu', '--vehicle-pinned-menu-ui-snapshot'),
)
CONTEXT_NAMES = {
    'caption': 'pinnedContextCaption',
    'status': 'pinnedWorkflowStatus',
}


def cases():
    return [(language, scale, mode, flag) for language in ('ru', 'en')
            for scale in ('1', '1.5', '2') for mode, flag in MODES]


def image_path(evidence, language, scale, mode):
    return evidence / f'{language}-scale-{scale}-{mode}.png'


def verify_snapshot(path, language, scale, mode):
    pixels = read_png(path, minimum_height=96, minimum_width=128)
    width, height, _, _ = pixels
    receipt = json.loads(path.with_suffix('.png.json').read_text(encoding='utf-8'))
    errors, surfaces = [], []

    def check(condition, message):
        if not condition:
            errors.append(message)

    check(receipt.get('language') == language and receipt.get('mode') == mode,
          'Native receipt language/mode does not match the requested mini-card')
    dpr = receipt.get('snapshot_dpr')
    if type(dpr) not in (int, float) or not math.isfinite(dpr) or not 0 < dpr <= 4:
        return {'file': path.name, 'language': language, 'scale': scale, 'mode': mode,
                'errors': errors + ['Missing or invalid effective native DPR'], 'surfaces': surfaces}
    for key, actual in (('snapshot_width', width), ('snapshot_height', height)):
        logical = receipt.get(key)
        check(type(logical) is int and logical > 0 and abs(round(logical * dpr) - actual) <= 1,
              f'{key}: native PNG does not agree with logical geometry/DPR')
    widgets = receipt.get('widgets')
    if not isinstance(widgets, list) or any(not isinstance(widget, dict) for widget in widgets):
        raise ValueError('Missing or malformed native widget inventory')

    def named(name, count=1, visible=True):
        matches = [widget for widget in widgets if widget.get('name') == name]
        check(len(matches) == count, f'{name}: expected {count} actual widgets, found {len(matches)}')
        for widget in matches:
            check(widget.get('visible') is visible, f'{name}: unexpected actual visibility')
        return matches

    def surface(name, rectangle, foreground=10, white_bound=False):
        try:
            metrics = region_metrics(pixels, rectangle)
        except (ValueError, TypeError) as error:
            errors.append(f'{name}: {error}')
            return
        surfaces.append({'name': name, **metrics})
        check(metrics['median_luminance'] < 80, f'{name}: actual background is light')
        if white_bound:
            check(metrics['white_surface_fraction'] < .08, f'{name}: large white surface was actually rendered')
        check(metrics['foreground_pixels_delta_at_least_100'] >= foreground,
              f'{name}: actual foreground does not have measurable contrast')

    def widget_surface(name, widget, label=False, text=False, button=False):
        bounds = widget.get('snapshot_rect')
        if not isinstance(bounds, dict) or not all(type(bounds.get(key)) is int
                                                  for key in ('x', 'y', 'width', 'height')):
            errors.append(f'{name}: missing actual screenshot geometry')
            return
        check(bounds['width'] == widget.get('width') and bounds['height'] == widget.get('height'),
              f'{name}: actual widget is partially clipped by its viewport')
        if button:
            check(widget.get('width', 0) >= 24 and widget.get('height', 0) >= 24,
                  f'{name}: interactive button is smaller than its usable target')
        if text:
            value = widget.get('text')
            check(isinstance(value, str) and bool(value.strip()) and value not in ('—', 'No solution', 'Нет решения'),
                  f'{name}: visible caption/command is empty or a placeholder')
        if label:
            if widget.get('wordWrap'):
                required = widget.get('required_height')
                check(type(required) is int and widget.get('height', 0) >= required,
                      f'{name}: wrapped text is truncated inside the label')
            else:
                advance, content = widget.get('text_width'), widget.get('content_width')
                check(type(advance) is int and type(content) is int and advance <= content,
                      f'{name}: complete text does not fit its value column')
                lines = widget.get('text', '').split('\n')
                if len(lines) > 1:
                    line_height = widget.get('text_line_height')
                    check(type(line_height) is int and line_height > 0 and
                          line_height * len(lines) <= widget.get('height', 0),
                          f'{name}: complete multiline text does not fit its label height')
        x = int(bounds['x'] * dpr + .5)
        y = int(bounds['y'] * dpr + .5)
        right = int((bounds['x'] + bounds['width']) * dpr + .5)
        bottom = int((bounds['y'] + bounds['height']) * dpr + .5)
        surface(name, (x, y, right - x, bottom - y))

    surface('whole native mini-card or popup', (0, 0, width, height), foreground=20, white_bound=True)
    locked, popup = mode.endswith('-locked'), mode.endswith('-menu')
    vehicle = mode.startswith('vehicle-')
    check(receipt.get('pinned_locked') is locked, 'Mini-card lock state does not match the actual fixture')
    check(receipt.get('pinned_always_on_top') is True, 'Default mini-card must retain its always-on-top preference')
    commands = receipt.get('main_commands')
    expected_arcs = ['low', 'high'] if vehicle else ['mortar']
    valid_commands = isinstance(commands, list) and all(isinstance(command, dict) for command in commands)
    check(valid_commands and [command.get('arc') for command in commands] == expected_arcs,
          'Main-window command receipt is missing its actual weapon/trajectory rows')
    if valid_commands:
        for command in commands:
            check(command.get('available') is True, 'Ready native fixture must use an available calculated command')
            check(type(command.get('selected')) is bool, 'Main command must record its actual effective selection')
        check(sum(command.get('selected') is True for command in commands) == 1,
              'Exactly one available command must be selected in the native ready fixture')

    if popup:
        for name in ('pinnedLockButton', 'ghostReticleToggle', 'pinnedReturnButton', 'pinnedTopmostButton'):
            for widget in named(name):
                widget_surface(name, widget, button=True)
        for name in ('pinnedOpacitySlider', 'ghostReticleOpacitySlider', 'pinnedUnlockHotkey'):
            for widget in named(name):
                widget_surface(name, widget)
        editors = named('qt_keysequenceedit_lineedit')
        parents = named('pinnedUnlockHotkey')
        hotkey = receipt.get('pinned_unlock_hotkey')
        for editor in editors:
            # QKeySequenceEdit can shrink while its styled QLineEdit keeps its
            # minimum height. Its own frame then fits, but cuts off the glyphs.
            widget_surface('actual unlock shortcut editor', editor, text=True)
            check(editor.get('type') == 'QLineEdit' and editor.get('enabled') is True,
                  'Recovery shortcut must use its actual enabled internal text editor')
            check(isinstance(hotkey, str) and bool(hotkey.strip()) and editor.get('text') == hotkey,
                  'Recovery shortcut editor must show the complete current shortcut')
            bounds = editor.get('snapshot_rect')
            if len(parents) == 1 and isinstance(bounds, dict):
                parent = parents[0].get('snapshot_rect')
                if isinstance(parent, dict) and all(type(rect.get(key)) is int
                        for rect in (bounds, parent) for key in ('x', 'y', 'width', 'height')):
                    check(bounds['x'] >= parent['x'] and bounds['y'] >= parent['y'] and
                          bounds['x'] + editor.get('width', 0) <= parent['x'] + parent['width'] and
                          bounds['y'] + editor.get('height', 0) <= parent['y'] + parent['height'],
                          'Recovery shortcut editor must fit completely inside its outer control')
            content = editor.get('text_content_rect')
            if not isinstance(content, dict) or not all(type(content.get(key)) is int
                    for key in ('x', 'y', 'width', 'height')):
                errors.append('Recovery shortcut editor is missing its actual styled text viewport')
                continue
            check(content['x'] >= 0 and content['y'] >= 0 and content['width'] > 0 and content['height'] > 0 and
                  content['x'] + content['width'] <= editor.get('width', 0) and
                  content['y'] + content['height'] <= editor.get('height', 0),
                  'Recovery shortcut text viewport must fit inside the actual internal editor')
            advance, line_height = editor.get('text_width'), editor.get('text_line_height')
            check(type(advance) is int and advance > 0 and advance <= content['width'] and
                  type(line_height) is int and line_height > 0 and line_height <= content['height'],
                  'Complete recovery shortcut must fit its actual text viewport width and height')
            if isinstance(bounds, dict) and all(type(bounds.get(key)) is int
                    for key in ('x', 'y', 'width', 'height')):
                left = bounds['x'] + content['x']
                top = bounds['y'] + content['y']
                visible_right = min(left + content['width'], bounds['x'] + bounds['width'])
                visible_bottom = min(top + content['height'], bounds['y'] + bounds['height'])
                check(visible_right == left + content['width'] and visible_bottom == top + content['height'],
                      'Recovery shortcut text viewport must remain fully visible through its parent clipping')
                x, y = int(left * dpr + .5), int(top * dpr + .5)
                right, bottom = int(visible_right * dpr + .5), int(visible_bottom * dpr + .5)
                surface('actual unlock shortcut text pixels', (x, y, right - x, bottom - y), foreground=20)
        for widget in named('pinnedUnlockLabel'):
            widget_surface('pinnedUnlockLabel', widget, label=True, text=True)
        for widget in named('pinnedLockButton'):
            check(widget.get('checked') is False and widget.get('enabled') is True,
                  'Unlocked popup must expose an enabled lock action')
        for widget in named('pinnedTopmostButton'):
            check(widget.get('checked') is True and widget.get('enabled') is True,
                  'Popup must expose the current always-on-top preference')
    else:
        for name in ('pinnedHeaderLockButton', 'pinnedControlsButton', 'pinnedHeaderReturnButton'):
            for widget in named(name):
                widget_surface(name, widget, button=True)
                if not locked:
                    check(widget.get('enabled') is True, f'{name}: unlocked header action must remain enabled')
        for widget in named('pinnedHeaderLockButton'):
            check(widget.get('checked') is locked, 'Header lock indicator does not match native input state')
        for widget in named('pinnedWeaponCaption'):
            widget_surface('pinnedWeaponCaption', widget, label=True, text=True)
            check(widget.get('text') == ('SPH-2' if vehicle else 'L81'),
                  'Mini-card header must identify the actual calculated weapon')
        hotkey = receipt.get('pinned_unlock_hotkey')
        check(isinstance(hotkey, str) and bool(hotkey.strip()),
              'Mini-card must record its actual recovery shortcut')
        for widget in named('pinnedLockHint', visible=locked):
            if locked:
                widget_surface('pinnedLockHint', widget, label=True, text=True)
                hint, detail = widget.get('text', ''), widget.get('toolTip', '')
                check(isinstance(hotkey, str) and bool(hotkey) and hotkey in hint and hotkey in detail,
                      'Locked mini-card must visibly explain its actual recovery shortcut')
                check(widget.get('accessibleDescription', '') == detail,
                      'Locked recovery instruction must retain its accessible description')
        if vehicle:
            fields = (('solutionDistance', 'distance'), ('solutionBearing', 'bearing'),
                      ('solutionMil', 'mil'), ('solutionTableDistance', 'table_distance'))
            for name, field in fields:
                values = named(name, count=2)
                for index, widget in enumerate(values):
                    widget_surface(name, widget, label=True, text=True)
                    if valid_commands and len(commands) == 2 and index < 2:
                        check(widget.get('text') == commands[index].get(field),
                              f'{name}: floating {expected_arcs[index]} value differs from the actual main command')
            for widget in named('solutionMetricCaption', count=8):
                widget_surface('solutionMetricCaption', widget, label=True, text=True)
            for index, widget in enumerate(named('solutionArc', count=2)):
                widget_surface('solutionArc', widget, label=True, text=True)
                if valid_commands and len(commands) == 2 and index < 2:
                    check(('✓' in widget.get('text', '')) is commands[index].get('selected'),
                          'Floating selection marker differs from the effective available main trajectory')
        else:
            for name, field in (('pinnedDistance', 'distance'), ('pinnedBearing', 'bearing'), ('pinnedMortarMil', 'mil')):
                for widget in named(name):
                    widget_surface(name, widget, label=True, text=True)
                    if valid_commands and len(commands) == 1:
                        expected = commands[0].get(field)
                        actual = widget.get('text', '')
                        check(isinstance(expected, str) and bool(expected) and
                              (actual.endswith(expected) if field == 'mil' else actual == expected),
                              f'{name}: floating value differs from the actual main mortar command')
        context = receipt.get('pinned_context')
        check(isinstance(context, dict), 'Native mini-card must record its persistent context and current status')
        if isinstance(context, dict):
            check(isinstance(context.get('caption'), str) and bool(context['caption'].strip()),
                  'Ready mini-card must show its persistent weapon/map context')
            detail = context.get('detail')
            check(isinstance(detail, str) and bool(detail.strip()),
                  'Ready mini-card must retain its calculation limitations in the context detail')
            for widget in named('pinnedContextCaption'):
                check(widget.get('toolTip', '') == detail and widget.get('accessibleDescription', '') == detail,
                      'Persistent context tooltip/accessibility must retain the expected calculation limitations')
            for field, name in CONTEXT_NAMES.items():
                expected = context.get(field)
                check(isinstance(expected, str), f'{name}: expected context text is missing')
                if isinstance(expected, str):
                    for widget in named(name, visible=bool(expected)):
                        check(widget.get('text', '') == expected, f'{name}: actual context differs from its receipt')
                        if expected:
                            widget_surface(name, widget, label=True, text=True)

    return {'file': path.name, 'language': language, 'scale': scale, 'mode': mode,
            'effective_dpr': dpr, 'width': width, 'height': height,
            'errors': errors, 'surfaces': surfaces}


def verify_directory(evidence, capture_errors=()):
    results = []
    for language, scale, mode, _ in cases():
        path = image_path(evidence, language, scale, mode)
        try:
            result = verify_snapshot(path, language, scale, mode)
        except (OSError, ValueError, TypeError, KeyError, zlib.error, struct.error) as error:
            result = {'file': path.name, 'language': language, 'scale': scale, 'mode': mode,
                      'errors': [str(error)], 'surfaces': []}
        results.append(result)
    for result in results:
        baseline = next((other for other in results if other['language'] == result['language'] and
                         other['mode'] == result['mode'] and other['scale'] == '1'), None)
        if result.get('effective_dpr') and baseline and baseline.get('effective_dpr'):
            if abs(result['effective_dpr'] / baseline['effective_dpr'] - float(result['scale'])) > .02:
                result['errors'].append('Requested DPI factor was not applied to the actual native mini-card')
        if result['errors']:
            for error in result['errors']:
                print(f'FAIL: {result["file"]}: {error}')
        else:
            print(f'PASS: {result["file"]}: native commands, context and controls remain visible and readable')
    return {'boundary': '36 actual native mini-card/popup views, RU/EN, factors 1/1.5/2; '
                        'no physical mixed-monitor, gameplay accuracy or real fullscreen proof.',
            'screenshots': results, 'capture_errors': list(capture_errors),
            'failures': sum(bool(result['errors']) for result in results) + len(capture_errors)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-only', type=pathlib.Path,
                        help='Read existing native PNG/JSON without launching processes or writing files')
    args = parser.parse_args()
    if args.verify_only is not None:
        report = verify_directory(args.verify_only.resolve(strict=True))
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 1 if report['failures'] else 0
    if os.name != 'nt' or os.environ.get('GITHUB_ACTIONS') != 'true' or os.environ.get('RUNNER_OS') != 'Windows':
        raise RuntimeError('Native mini-card snapshots are permitted only on the isolated GitHub Windows runner')
    root = pathlib.Path(os.environ['GITHUB_WORKSPACE']).resolve(strict=True)
    build = root / 'source/build/release'
    exe = (build / 'WarDogsDistanceCalculator.exe').resolve(strict=True)
    evidence = (build / 'Testing/pinned-card-app').resolve()
    if not exe.is_relative_to(root) or not evidence.is_relative_to(root):
        raise RuntimeError('Mini-card snapshot executable/output must remain inside the isolated CI checkout')
    evidence.mkdir(parents=True, exist_ok=True)
    capture_errors = []
    for language, scale, mode, flag in cases():
        path = image_path(evidence, language, scale, mode)
        environment = os.environ.copy()
        environment['QT_QPA_PLATFORM'] = 'windows'
        environment['QT_SCALE_FACTOR'] = scale
        environment['QT_SCALE_FACTOR_ROUNDING_POLICY'] = 'PassThrough'
        environment.pop('QT_SCREEN_SCALE_FACTORS', None)
        try:
            subprocess.run([str(exe), f'--language={language}', f'{flag}={path}'],
                           check=True, timeout=30, env=environment,
                           creationflags=subprocess.CREATE_NO_WINDOW)
        except (OSError, subprocess.SubprocessError) as error:
            capture_errors.append({'file': path.name, 'error': str(error)})
    # Retain all requested actual surfaces before aggregating any failures.
    report = verify_directory(evidence, capture_errors)
    (evidence / 'pinned-card-native-pixels.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return 1 if report['failures'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
