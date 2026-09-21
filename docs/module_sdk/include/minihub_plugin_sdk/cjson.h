/*
 * SPDX-FileCopyrightText: 2026 MiniHub Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif


/**
 * @file cjson.h
 * @brief Plugin SDK — cJSON export subset (Dave Gamble layout).
 *
 * Host links ``espressif__cjson``. Only allowlisted ``cJSON_*`` symbols resolve.
 * Each API carries an ``@note`` with its component header origin.
 */

/** @brief Invalid JSON node type.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
#define cJSON_Invalid (0)
/** @brief JSON false literal type flag.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
#define cJSON_False   (1 << 0)
/** @brief JSON true literal type flag.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
#define cJSON_True    (1 << 1)
/** @brief JSON null literal type flag.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
#define cJSON_NULL    (1 << 2)
/** @brief JSON number type flag.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
#define cJSON_Number  (1 << 3)
/** @brief JSON string type flag.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
#define cJSON_String  (1 << 4)
/** @brief JSON array type flag.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
#define cJSON_Array   (1 << 5)
/** @brief JSON object type flag.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
#define cJSON_Object  (1 << 6)
/** @brief JSON raw fragment type flag.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
#define cJSON_Raw     (1 << 7)

/** @brief cJSON tree node.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
#ifdef DOXYGEN
typedef struct {
    void *next;
    void *prev;
    void *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;
#else
typedef struct cJSON {
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;
#endif

/** @brief cJSON boolean type alias.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
typedef int cJSON_bool;

/** @brief Allocate an empty JSON object.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_CreateObject(void);
/** @brief Allocate an empty JSON array.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_CreateArray(void);
/** @brief Allocate a JSON string node.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_CreateString(const char *string);
/** @brief Allocate a JSON number node.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_CreateNumber(double num);
/** @brief Allocate a JSON boolean node.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_CreateBool(cJSON_bool boolean);
/** @brief Allocate a JSON null node.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_CreateNull(void);
/** @brief Parse a JSON string into a tree.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_Parse(const char *value);
/** @brief Serialize tree to compact text (caller frees).
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
char *cJSON_PrintUnformatted(const cJSON *item);
/** @brief Free a cJSON tree.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
void cJSON_Delete(cJSON *item);
/** @brief Deep-copy a cJSON subtree.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_Duplicate(const cJSON *item, cJSON_bool recurse);
/** @brief Get object member by key.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_GetObjectItem(const cJSON *const object, const char *const string);
/** @brief Get object member by key (case-sensitive).
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *const object, const char *const string);
/** @brief Get array element by index.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
/** @brief Return array element count.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
int cJSON_GetArraySize(const cJSON *array);
/** @brief Add item to object (transfers ownership).
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON_bool cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
/** @brief Append item to array (transfers ownership).
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON_bool cJSON_AddItemToArray(cJSON *array, cJSON *item);
/** @brief Add string field to object.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_AddStringToObject(cJSON *object, const char *name, const char *string);
/** @brief Add number field to object.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_AddNumberToObject(cJSON *object, const char *name, double number);
/** @brief Add boolean field to object.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_AddBoolToObject(cJSON *object, const char *name, cJSON_bool boolean);
/** @brief Add true field to object.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_AddTrueToObject(cJSON *object, const char *name);
/** @brief Add false field to object.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_AddFalseToObject(cJSON *object, const char *name);
/** @brief Add array field to object.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON *cJSON_AddArrayToObject(cJSON *object, const char *name);
/** @brief Remove object member by key.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
void cJSON_DeleteItemFromObject(cJSON *object, const char *string);
/** @brief Replace object member by key.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON_bool cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem);
/** @brief Test whether node is an array.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON_bool cJSON_IsArray(const cJSON *const item);
/** @brief Test whether node is a boolean.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON_bool cJSON_IsBool(const cJSON *const item);
/** @brief Test whether node is JSON false.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON_bool cJSON_IsFalse(const cJSON *const item);
/** @brief Test whether node is JSON true.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON_bool cJSON_IsTrue(const cJSON *const item);
/** @brief Test whether node is JSON null.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON_bool cJSON_IsNull(const cJSON *const item);
/** @brief Test whether node is a number.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON_bool cJSON_IsNumber(const cJSON *const item);
/** @brief Test whether node is an object.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON_bool cJSON_IsObject(const cJSON *const item);
/** @brief Test whether node is a string.
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
cJSON_bool cJSON_IsString(const cJSON *const item);

/** @brief Iterate ``array`` children (Dave Gamble macro).
 *  @note Component: espressif/cjson ``cJSON/cJSON.h`` */
#define cJSON_ArrayForEach(element, array) \
    for ((element) = ((array) != NULL) ? (array)->child : NULL; \
         (element) != NULL; \
         (element) = (element)->next)

#ifdef __cplusplus
}
#endif
