# RTSP → USB H.265 recorder

Host-static module `rtsp_cam` (RPC at `POST /api/modules/rtsp_cam/rpc`).

## Lab default

- URL: `rtsp://172.168.51.89:554/stream1` (TP-LINK H.265 main stream)
- Files: `/usb0/minihub/cam0/YYYYMMDD_HHMMSS.h265`
- Segment length: 1–60 minutes (`segment_sec` 60–3600; default 600 = 10 min)

## RPC

| Method | Params | Notes |
|--------|--------|-------|
| `status` | — | usb_mounted, recording, segment_sec, segment_min, bytes_written, last_error |
| `config` | `url`, `cam_id`, `segment_sec` (60–3600) | Persist to NVS; blocked while recording |
| `start` | optional same as config | Requires USB mounted |
| `stop` | — | |
| `list_files` | `limit` | Files under `/usb0/minihub/<cam_id>/*.h265` |

```bash
curl -s -X POST http://HUB/api/modules/rtsp_cam/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"config","params":{"segment_sec":600,"url":"rtsp://172.168.51.89:554/stream1"}}'

curl -s -X POST http://HUB/api/modules/rtsp_cam/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"start"}'

curl -s -X POST http://HUB/api/modules/rtsp_cam/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"status"}'

curl -s -X POST http://HUB/api/modules/rtsp_cam/rpc \
  -H 'Content-Type: application/json' \
  -d '{"method":"stop"}'
```

Play `.h265` in VLC on a PC after pulling the stick.
