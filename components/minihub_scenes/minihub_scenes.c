/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "minihub_scenes.h"

#include <string.h>
#include "esp_log.h"
#include "minihub_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "minihub_scenes";

#define MAX_SCENES 8
#define MAX_SCENE_CMDS 16

typedef struct {
    char point_id[MINIHUB_POINT_ID_MAX];
    char cmd[MINIHUB_CMD_MAX];
    int delay_ms;
} scene_cmd_t;

typedef struct {
    char id[32];
    char name[64];
    scene_cmd_t cmds[MAX_SCENE_CMDS];
    size_t cmd_count;
} scene_t;

static scene_t s_scenes[MAX_SCENES];
static size_t s_scene_count;

static esp_err_t load_scenes(const cJSON *scenes_cfg)
{
    s_scene_count = 0;
    if (!scenes_cfg) {
        return ESP_OK;
    }

    const cJSON *scenes = cJSON_GetObjectItem(scenes_cfg, "scenes");
    if (!cJSON_IsArray(scenes)) {
        return ESP_OK;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, scenes) {
        if (s_scene_count >= MAX_SCENES) {
            break;
        }
        scene_t *scene = &s_scenes[s_scene_count];
        memset(scene, 0, sizeof(*scene));

        const cJSON *id = cJSON_GetObjectItem(item, "id");
        const cJSON *name = cJSON_GetObjectItem(item, "name");
        const cJSON *commands = cJSON_GetObjectItem(item, "commands");

        if (cJSON_IsString(id)) {
            strncpy(scene->id, id->valuestring, sizeof(scene->id) - 1);
        }
        if (cJSON_IsString(name)) {
            strncpy(scene->name, name->valuestring, sizeof(scene->name) - 1);
        }

        if (cJSON_IsArray(commands)) {
            cJSON *cmd_item = NULL;
            cJSON_ArrayForEach(cmd_item, commands) {
                if (scene->cmd_count >= MAX_SCENE_CMDS) {
                    break;
                }
                scene_cmd_t *sc = &scene->cmds[scene->cmd_count];
                const cJSON *pid = cJSON_GetObjectItem(cmd_item, "point_id");
                const cJSON *cmd = cJSON_GetObjectItem(cmd_item, "cmd");
                const cJSON *delay = cJSON_GetObjectItem(cmd_item, "delay_ms");
                if (cJSON_IsString(pid)) {
                    strncpy(sc->point_id, pid->valuestring, sizeof(sc->point_id) - 1);
                }
                if (cJSON_IsString(cmd)) {
                    strncpy(sc->cmd, cmd->valuestring, sizeof(sc->cmd) - 1);
                }
                if (cJSON_IsNumber(delay)) {
                    sc->delay_ms = delay->valueint;
                }
                scene->cmd_count++;
            }
        }
        ESP_LOGI(TAG, "loaded scene '%s' (%u commands)", scene->name, (unsigned)scene->cmd_count);
        s_scene_count++;
    }
    return ESP_OK;
}

esp_err_t minihub_scenes_init(const cJSON *scenes_cfg)
{
    return load_scenes(scenes_cfg);
}

esp_err_t minihub_scenes_reload(const cJSON *scenes_cfg)
{
    return load_scenes(scenes_cfg);
}

esp_err_t minihub_scenes_invoke(const char *scene_id)
{
    if (!scene_id) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s_scene_count; i++) {
        const scene_t *scene = &s_scenes[i];
        if (strcmp(scene->id, scene_id) != 0) {
            continue;
        }

        ESP_LOGI(TAG, "invoke scene '%s'", scene->name);
        for (size_t j = 0; j < scene->cmd_count; j++) {
            const scene_cmd_t *sc = &scene->cmds[j];
            if (sc->delay_ms > 0) {
                vTaskDelay(pdMS_TO_TICKS(sc->delay_ms));
            }
            minihub_command_t cmd = {0};
            strncpy(cmd.point_id, sc->point_id, sizeof(cmd.point_id) - 1);
            strncpy(cmd.cmd, sc->cmd, sizeof(cmd.cmd) - 1);
            esp_err_t err = minihub_command_invoke(&cmd);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "scene cmd %s.%s failed", sc->point_id, sc->cmd);
            }
        }
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

cJSON *minihub_scenes_list_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < s_scene_count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", s_scenes[i].id);
        cJSON_AddStringToObject(item, "name", s_scenes[i].name);
        cJSON_AddNumberToObject(item, "commands", (double)s_scenes[i].cmd_count);
        cJSON_AddItemToArray(arr, item);
    }
    return arr;
}
