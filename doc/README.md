# Somnia — кассетный лупер-дезинтегратор

> Третий плагин коллекции Starshinov. Идея — авангардный tape recorder в духе
> William Basinski «Disintegration Loops»: петля записывается на ленту, а каждый
> проход медленно снимает с неё верх, окислы, куски и саму память о материале.

## Что это

Somnia — **эффект**, а не синтезатор. Вход записывается в кассетную петлю до
60 секунд, играет через варьируемую скорость и направление, проходит через
минимальный ADSR и цепь деградации, после чего смешивается с сухим сигналом.
Петля может быть свободной (REC → STOP) или привязанной к темпу хоста.

Главное отличие от обычного лупера: **запись не является чистой**. При
overdub в ленту возвращается уже деградировавший сигнал, поэтому с каждым
проходом петля теряет верх, становится тише, «дышит», рвётся и в конце концов
рассыпается. Это и есть механика Disintegration Loops, собранная в одном
инструменте.

Ключевые возможности:

- кассетная петля до 60 секунд, стерео;
- свободная запись или BPM-синхронизация (1/1 … 16/1);
- varispeed ±24 полутона;
- направления Forward / Reverse / Ping-Pong / Random;
- overdub с возвратом деградировавшего сигнала;
- минимальный линейный ADSR с настоящим release-хвостом и визуализацией
  стадии в TAPE MAP;
- деградация: wear, dropouts, splice, oxide, wow/flutter, hiss, saturation,
  tone;
- глобальный dry/wet, выходной уровень и прозрачный лимитер;
- синхронизация PLAY/STOP с транспортом DAW (опция, по умолчанию включена);
- единый дисплей **TAPE MAP**: бобины, карта всей ленты, playhead, REC/PLAY,
  ADSR, PASS, BPM, WEAR и редактирование границ лупа прямо на карте;
- транспорт REC / PLAY / STOP / REW / FF / CLEAR;
- пресеты, тёмная и мягкая светлая тема, компактный режим;
- «чёрный ящик» `DebugLogger` для отлова редких скачков и NaN.

## Текущий статус

- Версия: `1.0.0`.
- Форматы: **VST3**, **AU**, **Standalone**.
- Сборка Release проходит без ошибок.
- Все офлайн-тесты зелёные (`TapeTest`, `DegraderTest`, `DspTest`,
  `PresetTest`, `PluginTest`).
- AU проходит `auval` (`AU VALIDATION SUCCEEDED`).

## Быстрый старт

```bash
cd ~/projects/audio/Somnia

# один раз: локальный тулчейн, в систему ничего не ставится
python3 -m venv .tools
.tools/bin/pip install --upgrade pip
.tools/bin/pip install cmake ninja

# конфигурация и сборка
.tools/bin/cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_MAKE_PROGRAM="$PWD/.tools/bin/ninja"
.tools/bin/cmake --build build --config Release
```

Если каталог `.tools` уже существует, шаги с `venv` можно пропустить.

Артефакты и места установки:

| Формат     | Путь                                                                 |
|------------|----------------------------------------------------------------------|
| VST3       | `~/Library/Audio/Plug-Ins/VST3/Somnia.vst3`                          |
| AU         | `~/Library/Audio/Plug-Ins/Components/Somnia.component`               |
| Standalone | `build/Somnia_artefacts/Release/Standalone/Somnia.app`               |

Подробности по сборке, тестам и отладке — в [DEVELOPMENT.md](DEVELOPMENT.md).

## Документация

- [ARCHITECTURE.md](ARCHITECTURE.md) — сигнальная цепь, модули DSP, карта файлов.
- [PARAMETERS.md](PARAMETERS.md) — все параметры, режимы, пресеты и Random.
- [DEVELOPMENT.md](DEVELOPMENT.md) — тулчейн, сборка, тесты, как добавить параметр.
- [ROADMAP.md](ROADMAP.md) — что сделано и что дальше.

## Лицензия / происхождение

Код полностью оригинальный. Взят только общий визуальный и UX-стиль коллекции
Starshinov (корпус, нобы, кассетный дисплей); закрытые прошивки и чужие
реализации не использовались.
