# Vibe Mode factory image (not a Host product)

This directory is a nested ESP-IDF project that emits `vibe_mode.bin` for the
`app,factory` partition. It is built by the Host CMake target `vibe_mode`
(`../project_include.cmake`), the same idea as the IDF bootloader subproject.

Do **not** treat this as an `application/` product variant. Product Hosts live
under `application/minihub` and `application/minihub_lcd`. Shared stage/apply
APIs for the running Host are the parent component (`../`).

Standalone rebuild:

```bash
./tools/build_firmware.py -app minihub vibe
```
