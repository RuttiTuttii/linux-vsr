# linux-vsr neural tier: real rtx vsr models via nvidia vfx sdk

two tiers, two jobs:
- browser video -> gl shader hook (`libvsr.so`, cas + badge, zero config)
- files and heavy lifting -> neural models here (tensorrt, real time)

## setup

```bash
python3 -m venv /tmp/vsr-neural
/tmp/vsr-neural/bin/pip install -r neural/requirements-neural.txt
```

## smoke test

```bash
/tmp/vsr-neural/bin/python neural/smoke_vsr.py --w 1280 --h 720 --scale 2
# expect avg_ms ~2-3 on rtx 5070, outputs in /tmp/vsr_neural_out/
```

## notes

- needs nvidia gpu with tensor cores, driver 570+, cuda 12+
- first `load()` downloads model blobs (~1gb) and takes ~15s once
- `nvidia-vfx` is proprietary (nvidia sla), personal use is fine
- windows reshade/dlss bridges do not apply here: dlss needs
  color+depth+motion vectors from a game engine, flat video has none
