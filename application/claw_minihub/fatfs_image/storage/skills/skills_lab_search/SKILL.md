---
{
  "name": "skills_lab_search",
  "description": "Search ESP-Skill hub skills (games/demo Lua apps). NOT for MiniHub Store 插件/适配器/miot/米家 — those use minihub_hub + GET /api/store/catalog.",
  "metadata": {
    "cap_groups": [
      "cap_http_request",
      "cap_boards"
    ],
    "manage_mode": "web"
  }
}
---

# Skills Lab Search

Use this skill when the user wants to search or discover **ESP-Skill hub** skills (board demos, games, Lua apps).

## Do NOT use for MiniHub

If the user asks about **插件 / 可安装的插件 / Store 插件 / 适配器 / miot / 米家 / MiniHub modules**:

1. Stop. Do **not** call `https://skills-lab.esp-claw.com/...`.
2. Activate `minihub_hub` and call `hub.api` GET `/api/store/catalog`.
3. Those are MiniHub Store `.mhub` modules, not Skill hub entries.

## When to use

- The user wants ESP-Claw Skill hub skills (explicitly: 技能 hub / Skill hub / ESP-Skill / flappybird 类 demo).
- The user provides a topic for board Lua/demo skills and does **not** mean home-automation plugins.

## Hard rules

1. Before searching, make sure `cap_http_request` (or web search path used below) is available.
2. Prefer English keywords unless the user provides a specific query.
3. Always URL encode the full query string before appending to `https://skills-lab.esp-claw.com/api/search?q=`.
4. Use the search API endpoint directly. Do not scrape the web UI.
5. Tags/peripherals/categories: `https://skills-lab.esp-claw.com/raw/tags.json`.
6. Preserve advanced syntax: `t:"tag"`, `p:"peripheral"`, `c:"category"`.
7. Use `featured=true` only when the user wants featured **Skill hub** skills.
8. If the user asks to list all available **skills** (技能), request featured skills with `featured=true` and no `q`. If they said **插件** in a MiniHub/home context, see **Do NOT use for MiniHub** above.
9. After presenting results, remind that install needs exact skill id via `skills_lab_downloader`.

## Workflow

1. Confirm this is Skill hub, not MiniHub Store.
2. Featured list (Skill hub only):

```text
https://skills-lab.esp-claw.com/api/search?featured=true
```

3. Keyword search:

```text
https://skills-lab.esp-claw.com/api/search?q=<url_encoded_query>
```

4. Present exact skill ids; install only after the user picks one → `skills_lab_downloader`.
