"""Verify native application pixels for the unified workflow, on Windows CI only."""
import json
import os
import pathlib
import subprocess
from planning_snapshots import read_png, region_metrics


def verify(path, language, mode):
    pixels=read_png(path, minimum_height=128 if mode=='vehicle-pinned' else 300)
    receipt=json.loads(path.with_suffix('.png.json').read_text(encoding='utf-8'))
    assert receipt['language']==language and receipt['mode']==mode
    dpr=receipt['snapshot_dpr']
    assert 0<dpr<=4
    required={'solutionDistance':2,'solutionBearing':2,'solutionMil':2,'solutionTableDistance':2,'solutionMetricCaption':8}
    if mode=='fire-control': required.update({'fireControlSummary':1,'terrainAssistance':1})
    surfaces=[]
    for name,count in required.items():
        widgets=[w for w in receipt['widgets'] if w['name']==name and w['visible']]
        assert len(widgets)==count,(name,len(widgets),count)
        for widget in widgets:
            rect=widget['snapshot_rect']
            assert rect['width']>0 and rect['height']>0,(name,'clipped out of viewport')
            assert widget.get('text') and widget['text'] not in ('—','Нет решения','No solution')
            if name.startswith('solution'):
                assert rect['width']==widget['width'] and rect['height']==widget['height'],(name,'partially clipped')
                if widget.get('wordWrap'):
                    assert widget['height']>=widget['required_height'],(name,'wrapped caption truncated')
                else:
                    assert widget['text_width']<=widget['content_width'],(name,'text truncated within its value column')
            x=int(rect['x']*dpr+.5); y=int(rect['y']*dpr+.5)
            right=int((rect['x']+rect['width'])*dpr+.5); bottom=int((rect['y']+rect['height'])*dpr+.5)
            metrics=region_metrics(pixels,(x,y,right-x,bottom-y))
            assert metrics['median_luminance']<80,(name,'light background')
            assert metrics['foreground_pixels_delta_at_least_100']>=10,(name,'text lacks visible contrast')
            surfaces.append({'name':name,'text':widget['text'],**metrics})
    return {'file':path.name,'language':language,'mode':mode,'surfaces':surfaces}


def main():
    if os.name!='nt' or os.environ.get('GITHUB_ACTIONS')!='true' or os.environ.get('RUNNER_OS')!='Windows':
        raise RuntimeError('Native snapshots are permitted only on the isolated GitHub Windows runner')
    root=pathlib.Path(os.environ['GITHUB_WORKSPACE']).resolve()
    build=root/'source/build/release'
    exe=(build/'WarDogsDistanceCalculator.exe').resolve(strict=True)
    evidence=build/'Testing/fire-control-app'
    evidence.mkdir(parents=True,exist_ok=True)
    results=[]
    for language in ('ru','en'):
        for mode in ('fire-control','vehicle-pinned'):
            path=evidence/f'{language}-{mode}.png'
            subprocess.run([str(exe),f'--language={language}',f'--{mode}-ui-snapshot={path}'],
                           check=True,timeout=30,creationflags=subprocess.CREATE_NO_WINDOW)
            results.append(verify(path,language,mode))
            print('PASS',path.name,'four metrics and ranging have visible native pixels')
    (evidence/'fire-control-pixels.json').write_text(json.dumps({
        'boundary':'Native application pixels and layout only; no game accuracy claim.',
        'screenshots':results,'failures':0},ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__': main()
