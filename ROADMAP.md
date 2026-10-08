# план релиза linux-vsr

## готово (v1)

- браузерный ярус: `libvsr.so` перехватывает `glShaderSource`/`dlsym`,
  патчит yuv-фрагменты webrender (cas/easu), бейдж-индикатор, хот-релоад
- нейро-ярус: `libvsr_rt` поверх dlopen vfx sdk, модель в vram один раз,
  `vf_vsr` для mpv (sw путь плюс zero-copy cuda вход через npp)
- управление: `linux-vsr-ctl` (режим, резкость, бейдж, профили, модели),
  `linux-vsr-setup` (интерактивный дамп и вердикт), `linux-vsr-play` (ютуб)
- тесты: `make test`, `make test-neural`, валидация glsl через glslang

## планы

### браузерные GPU backends
- сделано: нативный OpenGL/GLSL hook для Firefox/Zen, автоопределение AMD/NVIDIA через DRM,
  ручной выбор `VSR_BACKEND=auto|amd|nvidia|generic` и запись backend в hit-log
- следующий шаг: отдельный Chromium/ANGLE путь; на Wayland Chromium обычно использует
  ANGLE/Vulkan и не вызывает перехваченный `glShaderSource`
- после этого: Vulkan `vkCreateShaderModule`/SPIR-V слой с безопасным байпасом при
  неизвестной схеме шейдера
- NVIDIA RTX Video Super Resolution AI и AMD shader path считаются разными backend-ами:
  текущий браузерный hook даёт vendor-neutral GLSL апскейл, а AI inference требует
  отдельного compute/SDK конвейера без подмены названия в диагностике

### zero-copy выход в vo (следующий шаг нейро-яруса)
- сейчас: вход zero-copy (cuda nv12 -> npp rgba -> infer), выход через
  download в rgb24 для vo
- цель: отдавать `IMGFMT_CUDA` дальше по цепочке, vo забирает кадр
  через cuda-interop без единой копии на cpu
- экономия около 1 мс на 720p, смысл есть только после замеров
- риск: капризы vo с cuda-кадрами под wayland, нужен фолбэк на download

### chromium и производные
- снять дампы шейдеров skia/graphite через `linux-vsr-setup`
- завести детект и замену luma-семплинга как сделали для webrender
- проверить sandbox-флаги `--no-sandbox` для доводки прелоада

### бейдж и режимы
- опция один бейдж вместо двух (ориентация уже определяется опытом)
- привязка `watermark_size` к размеру кадра, а не доли текстуры
- `VSR_MODE=off` как полный байпас без пересборки шейдеров

### не цели
- windows reshade/dlss-мосты: нужен движок с depth и motion vectors,
  у плоского видео их нет, плюс чужые dll из дискордов
- инжект нейросети в браузерный процесс: нет cuda-контекста и песочница,
  для ютуба есть `linux-vsr-play` через mpv
