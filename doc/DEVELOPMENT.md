# Разработка

## Требования

- macOS (сборка проверена на Apple Silicon).
- Command Line Tools (или Xcode): `xcode-select -p`.
- Python 3 — для локального venv с `cmake` и `ninja`.
- CMake ≥ 3.22 и Ninja. Они ставятся **локально** в `Somnia/.tools`;
  в систему ничего не устанавливается.

## Тулчейн (локальный)

```bash
cd ~/projects/audio/Somnia
python3 -m venv .tools
.tools/bin/pip install --upgrade pip
.tools/bin/pip install cmake ninja
```

JUCE подтягивается через CMake `FetchContent` в `build/_deps/juce-src`
(версия зафиксирована в `CMakeLists.txt` — сейчас **8.0.6**).

## Конфигурация и сборка

```bash
cd ~/projects/audio/Somnia
.tools/bin/cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_MAKE_PROGRAM="$PWD/.tools/bin/ninja"
.tools/bin/cmake --build build --config Release
```

Повторная сборка после правок кода — только вторая команда.
После правок `CMakeLists.txt` — снова обе.

В `CMakeLists.txt` включён `COPY_PLUGIN_AFTER_BUILD TRUE`, поэтому артефакты
сразу копируются:

- VST3 → `~/Library/Audio/Plug-Ins/VST3/Somnia.vst3`
- AU → `~/Library/Audio/Plug-Ins/Components/Somnia.component`
- Standalone → `build/Somnia_artefacts/Release/Standalone/Somnia.app`

### Особенность Apple-линкера

На macOS 27 (Tahoe) линкер отказывается LTO-линковать статический архив
JUCE `*_SharedCode`, если в командной строке не передан target triple. В
`CMakeLists.txt` для этого добавлен workaround:

```cmake
add_link_options("-target" "${SOMNIA_OSX_ARCH}-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET}")
```

Если сборка переносится на другую машину, этот блок можно оставить — он
безвреден для обычных конфигураций.

## Проверка AU

```bash
auval -v aufx Smna Strv      # должно быть: AU VALIDATION SUCCEEDED
```

Коды: тип `aufx`, подтип `Smna`, производитель `Strv`.

## Тесты

Все офлайн-тесты собираются вместе с плагином. Запуск:

```bash
cd ~/projects/audio/Somnia
for t in TapeTest DegraderTest DspTest PresetTest PluginTest; do
    "./build/Somnia${t}_artefacts/Release/Somnia${t}" || exit 1
done
```

| Тест | Что проверяет |
|------|----------------|
| `SomniaTapeTest` | Free-запись ~1 с, воспроизведение, четыре направления × varispeed × splice, synced-петля 0.5 с, тишина при STOP, NaN/Inf на входе, CLEAR |
| `SomniaDegraderTest` | Чистый путь, wear/oxide режут 8 кГц, dropouts, hiss на тишине, экстремальные настройки, NaN |
| `SomniaDspTest` | 2 с шумовой петли + ~42 с экстремальных настроек; выход конечен, ограничен, NaN-устойчив |
| `SomniaPresetTest` | Round-trip save → change → load, список заводских пресетов и XML |
| `SomniaPluginTest` | Дефолты, free-запись 1.2 с, воспроизведение, synced 1/1 = 2 с при 120 BPM, Random не трогает mix/output/transport, state round-trip, рендер редактора в PNG |

`PluginTest` сохраняет PNG редактора во временный каталог
(`somnia_editor.png`) — удобно смотреть UI без запуска хоста.

Тестовые сборки определяют `SOMNIA_TEST_BUILD=1`, поэтому `DebugLogger` пишет
во временный файл и не затирает пользовательский журнал.

## Карта кода

```
Source/
  PluginProcessor.h/.cpp      процессор: параметры, BPM-синк, транспорт, цепь, Random
  PluginEditor.h/.cpp         UI: корпус, кассета, транспорт, секции, тема, пресеты
  PresetManager.h             файловые пресеты (save/load XML-состояния)
  FactoryPresets.h            вшитые заводские пресеты (10 штук)
  DebugLogger.h/.cpp          «чёрный ящик»: снапшоты уровней, аномалии, лог-файл
  dsp/
    TapeLoop.h                кассетная петля: запись/чтение, varispeed, направления, overdub
    TapeDegrader.h            wear, dropouts, splice, oxide, wow/flutter, hiss, sat
    Adsr.h                    линейный ADSR-гейт петли
    Limiter.h                 выходной пиковый лимитер
  ui/
    SomniaLookAndFeel.h/.cpp  палитра, темы, нобы, IconButton
    TapeDisplay.h/.cpp        единый дисплей: карта ленты, бобины, редактор границ лупа
Tests/
  TapeLoopTest.cpp            офлайн-тест ленты
  DegraderTest.cpp            офлайн-тест деградера
  DspStressTest.cpp           стресс-тест всей DSP-цепи
  PresetTest.cpp              round-trip пресетов
  PluginTest.cpp              интеграционный тест процессора и редактора
doc/                          эта документация
```

## Как добавить параметр

1. Добавить id в `somnia::ids` (`Source/PluginProcessor.h`).
2. Создать параметр в `createParameterLayout()` (`Source/PluginProcessor.cpp`).
3. Добавить указатель в `RawParams` (`PluginProcessor.h`) и биндинг в
   `bindRawParams()` — в `processBlock` параметр читается как
   `rawParams.<имя>->load()` (без строковых лукапов в audio-потоке).
4. Добавить контрол в редакторе (`addKnob` / `addCombo`) и в `resized()`.
5. Если параметр влияет на DSP, передать его в `TapeLoop::Params`,
   `TapeDegrader::Params` или обработать в `processBlock`.
6. Обновить [PARAMETERS.md](PARAMETERS.md).

Для параметра, который должен попадать в пресеты, достаточно APVTS — состояние
сохраняется целиком. Если параметр добавляется в заводские пресеты, нужно
обновить `FactoryPresets.h` и поднять `kFactoryPresetVersion`, чтобы
установщик обновил файлы в пользовательском каталоге.

## Пресеты и тема

- Пресеты: `~/Library/Application Support/Somnia/Presets/*.somniapreset`.
- Версия заводских пресетов: `~/Library/Application Support/Somnia/Presets/factory_version.txt`.
- Тема: `~/Library/Application Support/Somnia/theme.txt`.
- Лог: `~/Library/Application Support/Somnia/somnia_debug.log`
  (ротация в `somnia_debug.prev.log` при 4 МБ).

## Отладка

`DebugLogger` ловит редкие баги (скачок громкости, залипшая головка, NaN):

- audio thread пишет `BlockSnapshot` в lock-free FIFO;
- фоновый поток ведёт историю 256 блоков и пишет лог;
- heartbeat — раз в 2 секунды;
- при аномалии выгружается вся история, похожие записи коалесятся 5 секунд;
- кнопка **LOG** в хедере открывает файл, красный бейдж — аномалия записана.

Флаги: `NONFINITE`, `INPUT_HOT`, `LOOP_HOT`, `OUT_HOT`, `HEAVY_GR`.
Подробнее — в [ARCHITECTURE.md](ARCHITECTURE.md), раздел «Диагностика».

## Полезно

- `randomizeParameters()` — пример программного изменения параметров через
  `setValueNotifyingHost`.
- `kLoopBeats` — таблица длин петли в целых нотах.
- `TapeLoop::Params` — все параметры ленты в одном месте.
- `TapeDegrader::Params` — все параметры деградации.
- `getDisplayEnvelopeLevel()` / `getDisplayEnvelopeStage()` — телеметрия ADSR
  для визуализации в TAPE MAP.
