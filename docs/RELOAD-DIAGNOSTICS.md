# Диагностика распознавания SPH-2 / SPH-2 recognition diagnostics

## RU

Если на стрельбище помощник работает, а в бою иногда не находит стрелки,
нужен исходный кадр именно неудачного распознавания. Обычный скриншот,
сделанный позже, может показывать уже другое состояние мини-игры.

Сначала разберите уже сохранённый цикл вне игры. Если исходных кадров нет,
при следующей запланированной перезарядке можно записать один цикл:

1. Откройте **Помощники → SPH-2 · перезарядка**.
2. Включите **Записать один цикл для диагностики** и сохраните настройки.
3. Вернитесь в игру, включите помощник назначенной клавишей (по умолчанию F8)
   и запустите обычную перезарядку клавишей R.
4. После цикла нажмите **Открыть папку диагностики** в настройках помощника.
   Для разбора нужна последняя папка целиком: `session.json` и PNG.

Запись использует те же кадры, которые помощник уже считывает: один перед
каждой комбинацией, максимум четыре. Дополнительных снимков, выстрелов,
перезарядок и нажатий она не создаёт. При ошибке сохраняются кадры до остановки.
PNG и JSON записываются в фоне после завершения ввода; F8 и повторные R
продолжают работать по прежним правилам. Флажок одноразовый и не сохраняется
в профиле между запусками программы.

Автопоиск сначала снимает всю клиентскую область игры, затем найденную полосу
с отступами. Положение полосы сохраняется в памяти между перезарядками;
другое окно, разрешение или область поиска требуют нового определения.
После ошибки распознавания положение сбрасывается для следующего R.
При ручном выборе снимается выбранная область. Исходные кадры
могут содержать игровой чат и имена; файлы остаются локально, автоматической
отправки нет. Папка: `%LOCALAPPDATA%\WardogsFireControl\diagnostics\reload`.
Лимит хранения исходных кадров в памяти — 64 МиБ; пропуск изображения из-за
лимита явно указан в JSON. Белая и зелёная маски используют точные пороги
исходной цветовой сегментации; отдельные счётчики в JSON показывают работу
восстановления по контрастным границам.

`session.json` содержит размеры и область кадра, направления, уверенность,
причину отказа, число фигур и строк, время захвата и распознавания. Успешная
отправка клавиш сама по себе не подтверждает принятие комбинации игрой.

Для разработчика: `wardogs_reload_vision_probe.exe --session <session.json>`
повторяет распознавание всех сохранённых исходных кадров без игры и ввода.
Прежний режим `--image <frame.png>` также работает.

## EN

If the helper works at the shooting range but misses arrows in combat, inspect
the exact failed capture. A screenshot taken later can show a different HUD state.
Replay an existing session first. If none exists, enable **Record one cycle for
diagnostics** under **Helpers → SPH-2 · reload**, save, then use the normal
F8 → R workflow during your next planned reload. Afterwards, use **Open
diagnostics folder** and keep the entire latest folder, including JSON and PNGs.

The recorder retains the same runtime captures: one before each combination,
up to four. It adds no capture, shot, reload or key press. Failed cycles retain
the captures available before stopping. PNG/JSON writing runs after input ends.
The option is consumed once and is not persisted across application launches.

Automatic search initially records the whole game client, then the tracked
strip. Its position is retained in memory across reloads; a different window,
resolution or search region requires discovery again. Recognition failure
discards the position for the next R. Manual search records the selected region.
Images may include chat or
player names. Files stay local under
`%LOCALAPPDATA%\WardogsFireControl\diagnostics\reload`; nothing is uploaded.
Raw captures have a 64 MiB memory limit, with omissions explicitly recorded.
White/green masks use the initial segmentation's exact thresholds; separate
JSON counters identify contrast-boundary recovery. The JSON records geometry,
recognition results, failure reasons, candidate counts and capture/recognition
timings. Sent keys do not prove that the game accepted the sequence.

Developers can replay a session without game input using
`wardogs_reload_vision_probe.exe --session <session.json>`, or inspect one image
with `--image <frame.png>`.
