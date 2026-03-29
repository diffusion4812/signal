// project.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "task_host_core/project.h"

// Simple JSON parser — avoids a library dependency.
// Only handles the flat structure of project.json.

// ── Internal helpers ──────────────────────────────────────────────

static const char *json_find_key(const char *json, const char *key) {
    char search[MAX_NAME_LEN + 4];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return NULL;
    pos += strlen(search);
    while (*pos == ' ' || *pos == ':' || *pos == '\t') pos++;
    return pos;
}

static int json_read_string(const char *json, const char *key,
                             char *out, size_t out_len) {
    const char *pos = json_find_key(json, key);
    if (!pos || *pos != '"') return -1;
    pos++;
    size_t i = 0;
    while (*pos && *pos != '"' && i < out_len - 1)
        out[i++] = *pos++;
    out[i] = '\0';
    return 0;
}

static int json_read_uint32(const char *json, const char *key, uint32_t *out) {
    const char *pos = json_find_key(json, key);
    if (!pos) return -1;
    *out = (uint32_t)strtoul(pos, NULL, 10);
    return 0;
}

static int json_read_int(const char *json, const char *key, int *out) {
    const char *pos = json_find_key(json, key);
    if (!pos) return -1;
    *out = (int)strtol(pos, NULL, 10);
    return 0;
}

static int json_read_bool(const char *json, const char *key, bool *out) {
    const char *pos = json_find_key(json, key);
    if (!pos) return -1;
    *out = (strncmp(pos, "true", 4) == 0);
    return 0;
}

static int json_read_uint8(const char *json, const char *key, uint8_t *out) {
    uint32_t v = 0;
    if (json_read_uint32(json, key, &v) != 0) return -1;
    *out = (uint8_t)v;
    return 0;
}

// ── Task entry parser ─────────────────────────────────────────────

static int parse_task_entry(const char *task_json,
                             task_manifest_entry_t *entry) {
    memset(entry, 0, sizeof(*entry));

    if (json_read_string (task_json, "name",                entry->name,       MAX_NAME_LEN) != 0) return -1;
    if (json_read_string (task_json, "so_path",             entry->so_path,    MAX_PATH_LEN) != 0) return -1;
    if (json_read_uint8  (task_json, "id",                 &entry->id)                       != 0) return -1;
    if (json_read_uint32 (task_json, "period_us",          &entry->period_us)                != 0) return -1;
    if (json_read_int    (task_json, "cpu_affinity",       &entry->cpu_affinity)             != 0) return -1;
    if (json_read_int    (task_json, "sched_priority",     &entry->sched_priority)           != 0) return -1;
    if (json_read_bool   (task_json, "enabled",            &entry->enabled)                  != 0) return -1;
    if (json_read_bool   (task_json, "hot_swappable",      &entry->allow_hot_swap)           != 0) return -1;

    // Optional fields — use defaults if absent
    json_read_uint32(task_json, "watchdog_timeout_ms", &entry->watchdog_timeout_ms);
    json_read_uint8 (task_json, "restart_policy",       &entry->restart_policy);
    json_read_uint32(task_json, "max_restarts",         &entry->max_restarts);
    json_read_bool  (task_json, "is_ethercat",          &entry->is_ethercat);

    return 0;
}

// ── Public API ────────────────────────────────────────────────────

int project_load(const char *path, project_manifest_t *out) {
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[project] cannot open '%s': %s\n",
                path, strerror(errno));
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 64 * 1024) {
        fprintf(stderr, "[project] file size invalid (%ld bytes)\n", size);
        fclose(f);
        return -1;
    }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);

    // ── Top-level fields ──────────────────────────────────────────
    json_read_string(buf, "name",            out->name,        MAX_NAME_LEN);
    json_read_string(buf, "description",     out->description, MAX_NAME_LEN);
    json_read_uint32(buf, "version",        &out->version);
    json_read_uint32(buf, "ec_period_us",   &out->ec_period_us);
    json_read_int   (buf, "ec_cpu_affinity",&out->ec_cpu_affinity);

    // ── Parse tasks array ─────────────────────────────────────────
    const char *tasks_pos = strstr(buf, "\"tasks\"");
    if (!tasks_pos) {
        fprintf(stderr, "[project] missing 'tasks' array\n");
        free(buf);
        return -1;
    }

    tasks_pos = strchr(tasks_pos, '[');
    if (!tasks_pos) { free(buf); return -1; }
    tasks_pos++;

    size_t task_count = 0;
    while (*tasks_pos && task_count < MAX_TASKS_PER_PROJECT) {
        // Find next object open
        const char *obj_start = strchr(tasks_pos, '{');
        if (!obj_start) break;

        // Find matching object close — scan for } accounting for nesting
        int depth = 1;
        const char *p = obj_start + 1;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            if (*p == '}') depth--;
            p++;
        }
        const char *obj_end = p;

        // Extract object text into a temp buffer
        size_t obj_len = (size_t)(obj_end - obj_start);
        char *obj_buf = malloc(obj_len + 1);
        if (!obj_buf) break;
        memcpy(obj_buf, obj_start, obj_len);
        obj_buf[obj_len] = '\0';

        task_manifest_entry_t entry;
        if (parse_task_entry(obj_buf, &entry) == 0) {
            out->tasks[task_count++] = entry;
        } else {
            fprintf(stderr, "[project] failed to parse task entry %zu\n",
                    task_count);
        }
        free(obj_buf);

        tasks_pos = obj_end;

        // Check for closing array bracket
        const char *next_obj = strchr(tasks_pos, '{');
        const char *arr_end  = strchr(tasks_pos, ']');
        if (!next_obj || (arr_end && arr_end < next_obj)) break;
    }

    out->task_count = task_count;
    free(buf);

    return project_validate(out);
}

int project_validate(const project_manifest_t *m) {
    if (strlen(m->name) == 0) {
        fprintf(stderr, "[project] missing project name\n");
        return -1;
    }
    if (m->task_count == 0) {
        fprintf(stderr, "[project] no tasks defined\n");
        return -1;
    }
    if (m->ec_period_us == 0) {
        fprintf(stderr, "[project] ec_period_us must be > 0\n");
        return -1;
    }

    // Check for duplicate task IDs or slots
    for (size_t i = 0; i < m->task_count; i++) {
        const task_manifest_entry_t *a = &m->tasks[i];
        if (a->period_us == 0) {
            fprintf(stderr, "[project] task '%s' has period_us = 0\n", a->name);
            return -1;
        }
        if (strlen(a->so_path) == 0) {
            fprintf(stderr, "[project] task '%s' missing so_path\n", a->name);
            return -1;
        }
        for (size_t j = i + 1; j < m->task_count; j++) {
            const task_manifest_entry_t *b = &m->tasks[j];
            if (a->id == b->id) {
                fprintf(stderr, "[project] duplicate task id 0x%02x ('%s' and '%s')\n",
                        a->id, a->name, b->name);
                return -1;
            }
            if (a->cpu_affinity == b->cpu_affinity &&
                a->sched_priority == b->sched_priority) {
                fprintf(stderr, "[project] WARNING: tasks '%s' and '%s' share "
                        "cpu=%d and priority=%d\n",
                        a->name, b->name,
                        a->cpu_affinity, a->sched_priority);
                // warning only — not fatal
            }
        }
    }

    return 0;
}

void project_print(const project_manifest_t *m) {
    printf("  project:     %s (v%u)\n", m->name, m->version);
    printf("  description: %s\n", m->description);
    printf("  ec_period:   %u us  cpu=%d\n", m->ec_period_us, m->ec_cpu_affinity);
    printf("  tasks (%zu):\n", m->task_count);
    for (size_t i = 0; i < m->task_count; i++) {
        const task_manifest_entry_t *t = &m->tasks[i];
        printf("    [%zu] id=0x%02x  %-12s  %6u us  cpu=%d  prio=%d  %s  %s\n",
               i, t->id, t->name, t->period_us,
               t->cpu_affinity, t->sched_priority,
               t->enabled ? "enabled" : "DISABLED",
               t->allow_hot_swap ? "hot_swappable" : "");
    }
}