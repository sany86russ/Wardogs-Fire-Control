"""Verify actual native UX pixels on the isolated GitHub Windows runner only.

QT_SCALE_FACTOR multiplies the native DPR; geometry uses the recorded effective
DPR, not an assumption that the runner display has 96 DPI. Qt 6.8 reference:
https://doc.qt.io/qt-6.8/highdpi.html#qt-scale-factor
"""

import argparse
import json
import math
import os
import pathlib
import struct
import subprocess
import zlib

from planning_snapshots import read_png, region_metrics


# These are the actual application flags. Some older diagnostic names omit -ui.
MODES = (
    ('ui', '--ui-snapshot'),
    ('workspace', '--workspace-ui-snapshot'),
    ('compact', '--compact-ui-snapshot'),
    ('first-start', '--first-start-ui-snapshot'),
    ('workspace-sidebar', '--workspace-sidebar-ui-snapshot'),
    ('tutorial', '--tutorial-ui-snapshot'),
    ('tutorial-keys', '--tutorial-keys-snapshot'),
    ('tutorial-help', '--tutorial-help-snapshot'),
    ('settings', '--settings-ui-snapshot'),
    ('recognition-hotkeys', '--recognition-hotkeys-snapshot'),
    ('recognition-reticle', '--recognition-reticle-snapshot'),
    ('recognition', '--recognition-snapshot'),
)
HELP_PAGES = {
    'tutorial': (0, 'helpText'),
    'tutorial-keys': (1, 'helpKeys'),
    'tutorial-help': (2, 'helpTroubleshooting'),
}
SETTINGS_PAGES = {
    'settings': (0, 'quickWorkflowSteps'),
    'recognition-hotkeys': (1, 'impactHotkey'),
    'recognition-reticle': (2, 'ghostReticlePreset'),
    'recognition': (3, 'ocrBackend'),
}
MAIN_MODES = {'ui', 'workspace', 'compact', 'first-start', 'workspace-sidebar'}


def cases(include_compact_2=False):
    result = [(language, scale, mode, flag)
              for language in ('ru', 'en')
              for scale in ('1', '1.5') for mode, flag in MODES]
    if include_compact_2:
        result.extend((language, '2', 'compact', '--compact-ui-snapshot') for language in ('ru', 'en'))
    return result


def image_path(evidence, language, scale, mode):
    return evidence / f'{language}-scale-{scale}-{mode}.png'


def verify_snapshot(path, language, scale, mode):
    pixels = read_png(path)
    width, height, _, _ = pixels
    receipt = json.loads(path.with_suffix('.png.json').read_text(encoding='utf-8'))
    errors, surfaces = [], []

    def check(value, message):
        if not value:
            errors.append(message)

    check(receipt.get('language') == language and receipt.get('mode') == mode,
          'Receipt language/mode does not match the requested actual application view')
    dpr = receipt.get('snapshot_dpr')
    if type(dpr) not in (int, float) or not math.isfinite(dpr) or not 0 < dpr <= 4:
        return {'file': path.name, 'language': language, 'scale': scale, 'mode': mode,
                'errors': errors + ['Missing or invalid effective native DPR'], 'surfaces': surfaces}
    for key, dimension in (('snapshot_width', width), ('snapshot_height', height)):
        logical = receipt.get(key)
        check(type(logical) is int and logical > 0 and abs(round(logical * dpr) - dimension) <= 1,
              f'{key}: PNG dimensions do not agree with native geometry/DPR')
    widgets = receipt.get('widgets')
    if not isinstance(widgets, list) or any(not isinstance(widget, dict) for widget in widgets):
        raise ValueError('Missing or malformed native widget inventory')

    def named(name, expected=1, visible=None):
        matches = [widget for widget in widgets if widget.get('name') == name]
        check(len(matches) == expected, f'{name}: expected {expected} widgets, received {len(matches)}')
        if visible is not None:
            for widget in matches:
                check(widget.get('visible') is visible, f'{name}: unexpected default visibility')
        return matches

    def surface(name, rectangle, minimum_foreground=20, white_bound=True):
        try:
            metrics = region_metrics(pixels, rectangle)
        except (ValueError, TypeError) as error:
            errors.append(f'{name}: {error}')
            return
        surfaces.append({'name': name, **metrics})
        check(metrics['median_luminance'] < 80, f'{name}: a light background was actually rendered')
        if white_bound:
            check(metrics['white_surface_fraction'] < .08, f'{name}: a large white surface was actually rendered')
        check(metrics['foreground_pixels_delta_at_least_100'] >= minimum_foreground,
              f'{name}: actual pixels have insufficient foreground/background contrast')

    def widget_surface(name, widget, minimum_foreground=20, label=False, meaningful_text=False):
        check(widget.get('visible') is True, f'{name}: required actual surface is hidden')
        bounds = widget.get('snapshot_rect')
        if not isinstance(bounds, dict) or not all(type(bounds.get(key)) is int for key in ('x', 'y', 'width', 'height')):
            errors.append(f'{name}: missing actual screenshot bounds')
            return
        check(bounds['width'] == widget.get('width') and bounds['height'] == widget.get('height'),
              f'{name}: the required surface is partially clipped by its viewport')
        if meaningful_text:
            text = widget.get('text')
            check(isinstance(text, str) and bool(text.strip()) and text not in ('—', 'Нет решения', 'No solution'),
                  f'{name}: actual value/instruction is empty or a placeholder')
        if label:
            if widget.get('wordWrap'):
                required = widget.get('required_height')
                check(type(required) is int and widget.get('height', 0) >= required,
                      f'{name}: wrapped text is truncated inside its label')
            else:
                text_width, content_width = widget.get('text_width'), widget.get('content_width')
                check(type(text_width) is int and type(content_width) is int and text_width <= content_width,
                      f'{name}: text is truncated inside its value column')
        left = int(bounds['x'] * dpr + .5)
        top = int(bounds['y'] * dpr + .5)
        right = int((bounds['x'] + bounds['width']) * dpr + .5)
        bottom = int((bounds['y'] + bounds['height']) * dpr + .5)
        surface(name, (left, top, right - left, bottom - top), minimum_foreground, white_bound=False)

    surface('whole actual application view', (0, 0, width, height))
    if mode in MAIN_MODES:
        sidebar = mode == 'workspace-sidebar'
        if mode in ('first-start', 'workspace-sidebar'):
            check(receipt.get('snapshot_width') == 640, 'Narrow-window fixture must actually render at logical width 640')
        if mode == 'first-start':
            check(receipt.get('map_confirmed') is False, 'A new profile must ask for its current map instead of assuming it')
        for name in ('workflowStep1', 'workflowStep2', 'workflowStep3', 'nextStep'):
            for widget in named(name, visible=True):
                widget_surface(name, widget, label=True, meaningful_text=True)
        for name in ('sidePanel', 'readinessGroup', 'readiness'):
            named(name, visible=sidebar)
        integration = receipt.get('game_integration_enabled')
        check(type(integration) is bool, 'Manual/default state needs the recorded integration mode')
        if type(integration) is bool:
            for widget in named('manualControlsToggle', visible=True):
                check(widget.get('checked') is (not integration),
                      'manualControlsToggle: automatic starts collapsed; standalone keeps manual input available')
            named('coordinatesGroup', visible=not integration)
            named('ocrGroup', visible=False)
        for widget in named('sessionDetailsToggle', visible=True):
            check(widget.get('checked') is sidebar, 'Session disclosure state does not match the fixture')
        if sidebar:
            panels, viewports = named('sidePanel', visible=True), named('mainContentViewport', visible=True)
            if len(panels) == len(viewports) == 1:
                check(panels[0].get('width', 0) <= viewports[0].get('width', 0),
                      'Expanded narrow-window history/readiness panel overflows the content width')
        if mode in ('workspace', 'workspace-sidebar'):
            for name, count in (('solutionDistance', 2), ('solutionBearing', 2), ('solutionMil', 2),
                                ('solutionTableDistance', 2), ('solutionMetricCaption', 8)):
                for widget in named(name, expected=count, visible=True):
                    widget_surface(name, widget, minimum_foreground=10, label=True, meaningful_text=True)
            for widget in named('fireControlCompact', visible=True):
                widget_surface('fireControlCompact', widget, label=True, meaningful_text=True)
            named('fireControlDetails', visible=False)
            named('fireControlSummary', visible=False)
            for widget in named('fireControlDetailsToggle', visible=True):
                check(widget.get('checked') is False, 'Ranging details must start collapsed')
    elif mode in HELP_PAGES:
        index, browser_name = HELP_PAGES[mode]
        for tabs in named('helpTabs', visible=True):
            check(isinstance(tabs.get('tabs'), list) and len(tabs['tabs']) == 3,
                  'Help must expose exactly three actual pages')
            check(tabs.get('currentIndex') == index and tabs.get('count') == 3,
                  f'{mode}: wrong actual help page selected')
        for name in ('helpText', 'helpKeys', 'helpTroubleshooting'):
            for widget in named(name, visible=name == browser_name):
                check(isinstance(widget.get('content'), str) and len(widget['content'].strip()) >= 60,
                      f'{name}: real help document is missing')
                if name == browser_name:
                    widget_surface(name, widget)
    elif mode in SETTINGS_PAGES:
        index, control_name = SETTINGS_PAGES[mode]
        for tabs in named('settingsTabs', visible=True):
            check(isinstance(tabs.get('tabs'), list) and len(tabs['tabs']) == 4,
                  'Settings must expose exactly four actual pages')
            check(tabs.get('currentIndex') == index and tabs.get('count') == 4,
                  f'{mode}: wrong actual settings page selected')
        for widget in named(control_name, visible=True):
            widget_surface(control_name, widget, minimum_foreground=10,
                           label=control_name == 'quickWorkflowSteps', meaningful_text=control_name == 'quickWorkflowSteps')
        if mode == 'recognition':
            named('coordinatePattern', visible=False)
            for widget in named('coordinatePatternDetails', visible=True):
                check(widget.get('checked') is False, 'Custom OCR pattern must start collapsed')

    visible = [widget for widget in widgets if widget.get('visible') is True and
               isinstance(widget.get('snapshot_rect'), dict) and
               widget['snapshot_rect'].get('width', 0) > 0 and widget['snapshot_rect'].get('height', 0) > 0]
    buttons = [widget for widget in visible if widget.get('type') in ('QPushButton', 'QToolButton', 'QCheckBox', 'QRadioButton')]
    return {'file': path.name, 'language': language, 'scale': scale, 'mode': mode,
            'width': width, 'height': height, 'effective_dpr': dpr, 'surfaces': surfaces,
            'descriptive_inventory': {
                'visible_widgets': len(visible), 'visible_buttons': len(buttons),
                'visible_labels': sum(widget.get('type') == 'QLabel' for widget in visible),
                'visible_text_characters': sum(len(widget.get('text', '')) for widget in visible),
                'boundary': 'Current native inventory only; no unmeasured baseline reduction claim.',
            }, 'errors': errors}


def verify_directory(evidence, include_compact_2=False, capture_errors=()):
    results = []
    for language, scale, mode, _ in cases(include_compact_2):
        path = image_path(evidence, language, scale, mode)
        try:
            result = verify_snapshot(path, language, scale, mode)
        except (OSError, ValueError, TypeError, KeyError, zlib.error, struct.error) as error:
            result = {'file': path.name, 'language': language, 'scale': scale, 'mode': mode,
                      'errors': [str(error)], 'surfaces': []}
        results.append(result)
    # QT_SCALE_FACTOR multiplies native DPI. Compare the measured ratio with
    # each corresponding factor-1 view, including a fractional native display.
    for result in results:
        baseline = next((other for other in results if other['language'] == result['language'] and
                         other['mode'] == result['mode'] and other['scale'] == '1'), None)
        if result.get('effective_dpr') and baseline and baseline.get('effective_dpr'):
            if abs(result['effective_dpr'] / baseline['effective_dpr'] - float(result['scale'])) > .02:
                result['errors'].append('Requested DPI scale was not applied to the actual native window')
    for result in results:
        if result['errors']:
            for error in result['errors']:
                print(f'FAIL: {result["file"]}: {error}')
        else:
            print(f'PASS: {result["file"]}: actual native UX surfaces are dark, readable and in the expected state')
    return {'boundary': 'Actual application widgets and native pixels on RU/EN, scale factors 1/1.5; no gameplay accuracy or full accessibility proof.',
            'screenshots': results, 'capture_errors': list(capture_errors),
            'failures': sum(bool(result['errors']) for result in results) + len(capture_errors)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-only', type=pathlib.Path,
                        help='Read existing PNGs/receipts without launching an application or writing files')
    parser.add_argument('--include-compact-2', action='store_true',
                        help='Also check the actual compact window at scale factor 2')
    args = parser.parse_args()
    if args.verify_only is not None:
        report = verify_directory(args.verify_only.resolve(strict=True), args.include_compact_2)
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return 1 if report['failures'] else 0
    # Keep this admission check before path creation or any native process.
    if os.name != 'nt' or os.environ.get('GITHUB_ACTIONS') != 'true' or os.environ.get('RUNNER_OS') != 'Windows':
        raise RuntimeError('Native UX snapshots are permitted only on the isolated GitHub Windows runner')
    root = pathlib.Path(os.environ['GITHUB_WORKSPACE']).resolve(strict=True)
    build = root / 'source/build/release'
    exe = (build / 'WarDogsDistanceCalculator.exe').resolve(strict=True)
    evidence = (build / 'Testing/ux-app').resolve()
    if not exe.is_relative_to(root) or not evidence.is_relative_to(root):
        raise RuntimeError('UX snapshot executable and output must remain inside the isolated CI checkout')
    evidence.mkdir(parents=True, exist_ok=True)
    capture_errors = []
    # Save every requested PNG/receipt first. One failed process must not hide
    # other language, DPI, help-page or settings-page diagnostics.
    for language, scale, mode, flag in cases(args.include_compact_2):
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
    report = verify_directory(evidence, args.include_compact_2, capture_errors)
    (evidence / 'ux-native-pixels.json').write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    return 1 if report['failures'] else 0


if __name__ == '__main__':
    raise SystemExit(main())
