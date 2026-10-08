# заметки по архитектуре linux-vsr

этот документ фиксирует целевую архитектуру, границу уже работающей части и
эксперименты, которые нужно пройти до слова «готово». Для каждой крупной идеи
указаны точка входа, fallback и проверяемый результат.

## цель продукта

linux-vsr должен включаться при открытии поддержанного браузера через launcher.
Браузер продолжает владеть декодированием, звуком, перемоткой, субтитрами,
цветом и выводом. linux-vsr меняет только этап масштабирования видео и умеет
вернуться к исходному pipeline без перезапуска браузера.

три уровня цели:

1. **shader-уровень:** малозатратная spatial обработка внутри GPU-композитора
   браузера, без копирования кадров через CPU;
2. **compute-уровень:** полноценный Vulkan/GL compute pass для браузеров без
   доступного GLSL fragment shader;
3. **AI-уровень:** отдельный NVIDIA VFX SDK или другой vendor runtime с
   GPU-буферами. Наличие NVIDIA-карты само по себе не означает AI VSR.

первый уровень — базовый и надёжный. Второй и третий добавляются постепенно:
при ошибке браузер остаётся на исходном compositor или shader fallback.

## текущее состояние

| путь | состояние | следующий признак готовности |
|---|---|---|
| Firefox/Zen WebRender + OpenGL | проверен локальным видео и hit-log | добавить версии браузера |
| AMD OpenGL | тот же GLSL, DRM vendor определяется | проверить на AMD-карте |
| NVIDIA OpenGL | проверен на RTX 5070, backend пишется | измерить стоимость и HDR |
| Chromium/Brave launcher | ANGLE/OpenGL маршрут есть | снять Skia/ANGLE video shaders |
| чистый Vulkan | только безопасный SPIR-V parser | сделать официальный Vulkan layer |
| NVIDIA AI VFX | отдельный neural runtime для mpv | подключить GPU-кадры без CPU |
| browser test stand | локальный MP4, профиль, JSON report | расширить browser matrix |

стенд браузера — источник истины для локального интеграционного результата:

    python3 tests/browser_stand.py --browser /usr/bin/zen-browser \
      --seconds 12 --output /tmp/linux-vsr-browser-stand

он создаёт короткий 4:2:0 MP4 через ffmpeg, открывает локальную HTML-страницу,
запускает изолированный профиль и сохраняет report.json, browser.log, shader
dumps, compile errors и hit-log текущего запуска. Глобальный /tmp/vsr_hits.log
не используется, поэтому старый успех не создаёт ложный результат. Опция
strict предназначена для машины, где hit уже ожидается.

verdict:

- PASS_PATCHED — текущий shader записан в hit-log;
- FAIL_COMPILE_ERRORS — драйвер отверг сгенерированный shader;
- OBSERVED_VIDEO_NO_HIT — video shader найден, но patcher не совпал;
- OBSERVED_SHADERS_NO_VIDEO_MARKER — графический путь есть, video marker нет;
- NO_SHADER_DUMPS — браузер не дошёл до перехватываемого API.

## уровни и границы ответственности

### launcher

bin/linux-vsr отвечает за процесс, конфиг, выбор браузера и окружение детей.
Политика shader не должна жить в shell. Launcher загружает libvsr.so, передаёт
явные env-переопределения и даёт Chromium-family ANGLE/OpenGL маршрут с opt-out
VSR_CHROMIUM_GL=0.

для Firefox-family сейчас нужен существующий компромисс с sandbox, чтобы
библиотека дошла до GPU/content children. Следующий шаг — явный
unsafe-browser-hook, чтобы режим был виден пользователю.

### hook

libvsr.so находит настоящие GL entry points, склеивает source chunks, проверяет
признаки YUV-видеопрохода и отправляет исходный текст или ограниченную замену.
Правило fail-open:

- отсутствующий символ означает forward или skip;
- битый source не переписывается;
- неизвестный layout отправляется как есть;
- лимиты памяти обязательны;
- ошибка компиляции записывается, но браузер не останавливается.

пути диагностики задаются VSR_DUMP_DIR, VSR_HIT_LOG и VSR_COMPILE_LOG. Это
нужно для параллельных запусков и честных доказательств.

### patcher

patcher работает по сигнатурам, а не по номерам версий. Каждая сигнатура имеет
positive video marker, точный luma anchor, fragment-stage check, replacement с
сохранением bounds и coordinate names, regression fixture из реального dump.

для каждой новой сигнатуры нужен negative fixture. Широкое совпадение, которое
задевает color correction или UI shader, является регрессией даже при успешной
компиляции GLSL.

### shader generator

генераторы создают ограниченный GLSL с C numeric locale. cas — установленный
adaptive sharpen путь, easu — directional spatial путь, directional —
экспериментальная vendor-neutral резкость. Название directional не выдаёт
короткую fragment-функцию за полный NVIDIA NIS SDK.

полный NVIDIA Image Scaling SDK — compute-интеграция с coefficients, config
buffers, dispatch geometry и HDR modes. Его официальные GLSL sources можно
подключить позднее, но одно имя режима не заменяет интеграцию ресурсов.

## контракт vendor backends

VSR_BACKEND=auto|amd|nvidia|generic означает:

- выбор разрешённой реализации;
- запись реально найденного DRM vendor.

сейчас код делает detection и пишет vendor в hit-log. Он не выдаёт общий GLSL
shader за NVIDIA RTX AI или закрытую AMD-драйверную функцию.

следующий контракт должен показывать capabilities:

    vendor=nvidia
    glsl_hook=available
    vulkan_layer=missing
    vfx_ai=missing
    dmabuf_interop=unknown

CLI и launcher смогут объяснить выбор или downgrade. auto должен выбирать
самый быстрый проверенный путь, а не самый амбициозный.

## стратегия по браузерам

### Firefox и Zen

оставить текущий WebRender/OpenGL эталоном. Локальный fixture stand уже
демонстрирует нужный цикл: локальный YUV-клип, video fragment, patch и hit без
сети.

дальше нужно собрать dumps со стабильной и nightly версий, добавить NV12/P010
и обе ориентации texture coordinates, проверить rotation/crop и hot reload во
время проигрывания.

### Chromium и производные

три варианта:

1. **ANGLE/OpenGL:** дешёвый путь, переиспользует текущий hook. Launcher уже
   умеет его включать; нужны local video dump и Skia/ANGLE signatures;
2. **ANGLE/Vulkan или Graphite:** более нативно для Wayland, но потребует
   Vulkan layer или browser post-process hook;
3. **network relay или extension:** полезно для AI-экспериментов, но меняет
   байты или заменяет player и не является native compositor path.

порядок: сначала ANGLE/OpenGL и сигнатуры, затем Vulkan. Chromium принимается
только при current-run hit и нулевых compile errors.

### чистый Vulkan

предыдущий raw vkGetInstanceProcAddr interpose уронил Chromium и не должен
возвращаться. Безопасный план — официальный Vulkan layer:

1. manifest с уникальным именем и путём библиотеки;
2. instance/device dispatch tables через loader chain;
3. vkCreateShaderModule только после инициализации dispatch;
4. defensive checks SPIR-V: header, bounds, execution model, decorations;
5. неизвестные модули пропускать byte-for-byte;
6. сначала только логировать fragment modules;
7. трансформацию добавлять после реального Chromium fixture и spirv-val;
8. дать полный opt-out VSR_VULKAN_LAYER=0.

SPIR-V нельзя менять как текст. Нужны sampled YUV images, descriptor bindings,
helper и финальное luma value. Если это нельзя доказать, module проходит без
изменений.

## NVIDIA AI bridge

официальная NVIDIA VFX документация описывает Linux VSR как GPU-buffer filter с
quality, denoise, deblur и high-bitrate modes. Существующий neural runtime
полезен как основа: он лениво грузит SDK и держит модель в VRAM для mpv.

browser bridge не должен вызывать AI из glShaderSource. Практичная схема:

    browser video texture
            |
            +-- GL/Vulkan interop или dmabuf export
            v
    vsr-ai helper process
            |  persistent VFX session, bounded queue, CUDA stream
            v
    GPU output texture / dmabuf
            |
            v
    browser compositor

нужны ownership contract и fences, одна persistent session на GPU process,
очередь на один-два кадра, NV12/P010/RGBA/BGRA negotiation, shader fallback и
отдельная заметка о лицензии NVIDIA runtime.

первый полезный milestone — standalone dmabuf-to-VFX probe на synthetic frame.
Только после fence и round-trip его можно соединять с браузером. Пока честная
AI-точка входа — linux-vsr-play.

## AMD path

AMD лучше поддерживать vendor-neutral. CAS/FSR-style GLSL переносим и проверяем
на реальной AMD-карте, а не выбираем только по имени. Позднее возможны Vulkan
compute, subgroup/FP16 checks, VAAPI/DMABUF negotiation, FidelityFX sources и
per-GPU tuning после timestamp measurements.

необязательная AMD-компонента не должна ломать fallback. generic остаётся
рабочим режимом для того же fragment shader.

## производительность и качество

фиксированное обещание 0.02 ms нужно заменить измерениями: GPU timestamps,
frame time p50/p95/p99, dropped/late frames, dimensions, pixel format, compile
time, recompile count, VRAM и AI queue depth.

quality fixtures: текст на градиенте, диагонали, тонкие UI borders, grain,
compression blocks, тёмные сцены, HDR/PQ и chroma edges.

рядом с результатом хранятся config, vendor, browser build, driver, mode и
commit SHA. Картинки остаются в ignored output, summaries — CI artifacts.

## безопасность и отказоустойчивость

LD_PRELOAD и отключение sandbox — опасные process controls. Launcher должен
показывать browser/backend перед exec, сохранять opt-outs, не менять окружение
несвязанных приложений, использовать private 0700 diagnostic directories,
писать config атомарно, никогда не выполнять URL или shader contents как shell
code и fail-open возвращаться в исходный browser pipeline.

AI и relay connectors должны быть opt-in. Network relay обязан проверять HTTPS
googlevideo origin, сохранять range semantics и не становиться скрытым default.

## gates готовности

backend считается browser-ready только если:

1. local fixture stand даёт current-run hit;
2. shader/module валидируется;
3. браузер играет минимум десять секунд без crash;
4. compile-error log пуст;
5. off не даёт hit и rewrite;
6. malformed и non-video shaders остаются byte-for-byte;
7. private diagnostic directory работает;
8. performance измерена на целевой карте;
9. документация описывает точное ограничение.

## порядок реализации

1. держать fixture stand зелёным и добавлять Firefox/Zen fixtures;
2. снять Chromium ANGLE/OpenGL local-video dumps и добавить signatures;
3. сделать capability reporting и timestamps;
4. реализовать logging-only Vulkan layer;
5. проверить один реальный Chromium SPIR-V module;
6. сделать standalone NVIDIA dmabuf/VFX probe;
7. подключить AI только за explicit opt-in;
8. проверить AMD hardware и качество fallback;
9. упаковать desktop install/uninstall и versioned diagnostics.

## ссылки

- https://docs.nvidia.com/maxine/vfx/1.2.0.0/Filters/VideoSuperResolution.html
- https://github.com/NVIDIAGameWorks/NVIDIAImageScaling
- https://github.com/KhronosGroup/Vulkan-Loader
