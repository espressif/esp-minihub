---
name: minihub-athena
description: Link Athena issues to GitLab MRs and manage issue progress status. Use when reporting a fix/MR on an Athena issue, replying with an MR link, or moving an issue's status from Not started to In progress.
---

# MiniHub Athena ↔ MR workflow

## Rules (always follow)

1. **Full MR URL, never shorthand.** When you reply on an Athena issue with an MR reference, write the COMPLETE URL:
   `https://gitlab.espressif.cn:6688/ae_group/minihub/esp-minihub/-/merge_requests/<NN>`
   Do NOT use the bare `!NN` short form (e.g. "MR !53") as the only reference — the link must be fully clickable/copyable. If an existing comment titles itself with `!NN`, rewrite its body to lead with the full URL.
2. **Flip status after replying.** After you post the reply, check the issue's status. If it is `Not started` (`NOT STARTED`), update it to `In Progress` (`In Progress`).

## Mechanics (athena CLI, macOS: /opt/homebrew/bin/athena)

Resolve a short link to get the node uuid + project db path:

```bash
athena resolve <shortid>     # e.g. SGVAXfjc -> Node ID + Project (MiniHub, <proj_uuid>)
# db_path = project/proj_<proj_uuid>.db
```

### Reply with the full MR link

Option A — create a new comment (preferred "reply"):

```bash
athena create-node "修复 MR：<title>" -p MiniHub -n <issue_uuid> -t comment \
  -d "完整 MR 链接：https://gitlab.espressif.cn:6688/ae_group/minihub/esp-minihub/-/merge_requests/<NN>

<what changed>" -j
```

Option B — update an existing comment so it leads with the full URL (**PUT**, not PATCH):

```bash
athena rest /api/nodes/<comment_uuid> -m PUT -d '{"description":"完整 MR 链接：https://gitlab.espressif.cn:6688/ae_group/minihub/esp-minihub/-/merge_requests/<NN>\n\n<context>","_db_path":"project/proj_<proj_uuid>.db"}'
```

### Change status to In Progress

```bash
athena rest /api/nodes/<issue_uuid> -m PUT -d '{"status":"In Progress","schedule_data":"{\"status\":\"in_progress\"}","_db_path":"project/proj_<proj_uuid>.db"}'
```

### Gotchas

- **Use PUT, not PATCH.** `PATCH /api/nodes/<id>` returns `405 Method Not Allowed`. Updates go through `PUT /api/nodes/<id>` (matches `AthenaClient.update_node`).
- **`_db_path` placement:** for `PUT` it goes in the **request body** (`update_node` sets `payload["_db_path"]`). For `GET /api/nodes/tree` it goes in **query params** (`-p`).
- **Nested quotes in `-d`:** escape as `\"` inside single-quoted shell strings.
- **Comment vs issue:** a comment node has `status: ""`; only the issue (task) node carries `NOT STARTED` / `In Progress`. Update the issue node, not the comment.
- **Cross-db nodes require `_db_path`.** Without it, tree/list queries return empty or 404.
- Status enum values observed: `NOT STARTED`, `In Progress`, `DONE`, `ACTION DONE`.
