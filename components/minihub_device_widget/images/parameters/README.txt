# Parameter icons (optional)

Optional control icons for `minihub_device_widget_image_name(device, parameter, …)`.

Naming (first existing file under `/data/images/parameters/` wins):

1. CSV `image` column (bare file name), if set
2. `<device>_<parameter>.svg`
3. `<parameter>.svg`

Interactive controls are CSS widgets in `minihub_web` (`device_widgets.css`).
Device avatars live in `../devices/`.
