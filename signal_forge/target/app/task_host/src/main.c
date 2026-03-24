#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include "task_host_core/interface.h"
#include "task_host_core/manifest.h"
#include "dbg_pubsub.h"
#include "dbg_util.h"

#if __BIG_ENDIAN__
# define htonll(x) (x)
# define ntohll(x) (x)
#else
# define htonll(x) (((uint64_t)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
# define ntohll(x) (((uint64_t)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))
#endif

#define SERVER_PORT 9000
#define BUFFER_SIZE 1024
#define MAX_SLOTS   1024

typedef BlockRegistryEntry* (*task_reg_t)(uint64_t*);
typedef void                (*task_entry_t)(TaskContext*);
typedef void                (*task_init_t)(TaskContext*);
typedef int                 (*migrate_func_t)(uint32_t, void*, void*);

typedef struct {
    uint64_t allocated_slots;
    BlockRegistryEntry* reg;
    TaskContext ctx;
    pthread_mutex_t ctx_lock;
    task_entry_t func;
    void* handle;
} Task;

Task g_task;
static dbg_publisher_t *g_pub;

static int64_t find_existing_slot_index(const Task* task, uint64_t block_id) {
    for (uint64_t i = 0; i < task->allocated_slots; i++) {
        if (task->reg[i].block_id == block_id) return (int64_t)i;
    }
    return -1;
}

static inline dbg_value_type_t dbg_value_type_from_field_type(FieldType ft)
{
    switch (ft) {
    case FIELD_TYPE_REAL: return DBG_VT_F32;
    default:              return 0;
    }
}

void update_pub(Task *task, dbg_publisher_t *pub) {
    const FieldInfo *field_info = NULL;

    dbg_pub_unregister_all_fields(pub);

    for (uint64_t s = 0; s < task->allocated_slots; s++) {
        if (task->reg[s].field_count == 0 || task->reg[s].fields == NULL) continue;

        for (uint64_t p = 0; p < task->reg[s].field_count; p++) {
            field_info = &task->reg[s].fields[p];
            void* addr = task->ctx.slots[s] + field_info->offset;
            char name[256];
            snprintf(name, sizeof(name), "%s.%s", field_info->instance, field_info->name);
            dbg_field_def_t def = {
                .field_id = field_info->field_id,
                .type     = dbg_value_type_from_field_type(field_info->type),
                .access   = DBG_ACCESS_READ_WRITE,
                .ptr      = addr,
                .name     = name
            };
            dbg_pub_register_field(pub, &def);
        }
    }
}

void hot_swap(Task *task, const char *so_path) {
    void *new_handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!new_handle) {
        fprintf(stderr, "Load Error: %s\n", dlerror());
        return;
    }

    task_reg_t     new_task_reg     = (task_reg_t)dlsym(new_handle, "task_registry");
    task_entry_t   new_task_entry   = (task_entry_t)dlsym(new_handle, "task_entry");
    task_init_t    new_task_init    = (task_init_t)dlsym(new_handle, "task_init");
    migrate_func_t new_task_migrate = (migrate_func_t)dlsym(new_handle, "task_migrate");

    if (!new_task_reg || !new_task_entry) {
        fprintf(stderr, "Invalid Library: Missing entry point or registry.\n");
        dlclose(new_handle);
        return;
    }

    uint64_t new_task_slots = 0;
    BlockRegistryEntry* new_reg = new_task_reg(&new_task_slots);

    // Build new registry and slot context arrays based on the new library
    BlockRegistryEntry* new_reg_copy = calloc(MAX_SLOTS, sizeof(BlockRegistryEntry));
    void** new_ctx_slots = calloc(MAX_SLOTS, sizeof(void*));
    if (!new_reg_copy || !new_ctx_slots) {
        fprintf(stderr, "Allocation failure during hot swap\n");
        free(new_reg_copy);
        free(new_ctx_slots);
        dlclose(new_handle);
        return;
    }

    // Track which old contexts are preserved to avoid double free
    uint8_t* old_ctx_preserved = calloc(task->allocated_slots, sizeof(uint8_t));
    if (!old_ctx_preserved) {
        fprintf(stderr, "Allocation failure during hot swap\n");
        free(new_reg_copy);
        free(new_ctx_slots);
        dlclose(new_handle);
        return;
    }

    for (uint64_t n = 0; n < new_task_slots; n++) {
        BlockRegistryEntry* new_block = &new_reg[n];

        int64_t exist_idx = find_existing_slot_index(task, new_block->block_id);
        if (exist_idx >= 0) {
            BlockRegistryEntry* existing_block = &task->reg[exist_idx];
            void* existing_ctx = task->ctx.slots[exist_idx];

            if (new_block->signature == existing_block->signature) {
                // Preserve state; update metadata to new registry
                memcpy(&new_reg_copy[n], new_block, sizeof(BlockRegistryEntry));
                new_ctx_slots[n] = existing_ctx;
                old_ctx_preserved[exist_idx] = 1;
                printf("Block ID %ld: Signature match. Preserving state.\n", new_block->block_id);
            } else {
                // Allocate new block context
                void* new_block_context = calloc(1, new_block->block_size);
                if (!new_block_context) {
                    fprintf(stderr, "Unable to allocate new block memory\n");
                    free(new_reg_copy);
                    free(new_ctx_slots);
                    free(old_ctx_preserved);
                    dlclose(new_handle);
                    return;
                }

                // Migrate data from old context to new context if supported
                if (new_task_migrate) {
                    int migrated = new_task_migrate(existing_block->signature, existing_ctx, new_block_context);
                    if (migrated) {
                        printf("Block ID %ld: Signature mismatch. Data migrated from %lX to %lX.\n",
                               new_block->block_id, existing_block->signature, new_block->signature);
                    } else {
                        printf("Block ID %ld: Signature mismatch. No migration found. State RESET.\n", new_block->block_id);
                    }
                } else {
                    printf("Block ID %ld: Signature mismatch. No migration found. State RESET.\n", new_block->block_id);
                }

                // Update registry entry and slot context
                memcpy(&new_reg_copy[n], new_block, sizeof(BlockRegistryEntry));
                new_ctx_slots[n] = new_block_context;
                // Old context will be freed after we finish building new arrays
                old_ctx_preserved[exist_idx] = 0;
            }
        } else {
            // New block not present previously: allocate fresh context
            printf("Block ID %ld: Allocating new block.\n", new_block->block_id);
            if (n >= MAX_SLOTS) {
                fprintf(stderr, "Unable to allocate new block, out of memory\n");
                free(new_reg_copy);
                free(new_ctx_slots);
                free(old_ctx_preserved);
                dlclose(new_handle);
                return;
            }

            void* new_block_context = calloc(1, new_block->block_size);
            if (!new_block_context) {
                fprintf(stderr, "Unable to allocate new block memory\n");
                free(new_reg_copy);
                free(new_ctx_slots);
                free(old_ctx_preserved);
                dlclose(new_handle);
                return;
            }

            memcpy(&new_reg_copy[n], new_block, sizeof(BlockRegistryEntry));
            new_ctx_slots[n] = new_block_context;
        }
    }

    // Free contexts for old blocks that are not present in the new registry
    for (uint64_t e = 0; e < task->allocated_slots; e++) {
        uint64_t old_id = task->reg[e].block_id;
        int64_t new_idx = -1;
        for (uint64_t n = 0; n < new_task_slots; n++) {
            if (new_reg_copy[n].block_id == old_id) { new_idx = (int64_t)n; break; }
        }
        if (new_idx < 0) {
            // Old block removed; free its context
            if (task->ctx.slots[e]) {
                free(task->ctx.slots[e]);
            }
        }
    }

    // Replace registry and contexts with new versions
    free(task->reg);
    free(task->ctx.slots);
    task->reg = new_reg_copy;
    task->ctx.slots = new_ctx_slots;
    task->allocated_slots = new_task_slots;

    void *old_handle = task->handle;
    task->handle = new_handle;
    task->func = new_task_entry;

    if (old_handle) dlclose(old_handle);
    free(old_ctx_preserved);

    // Optional: call task_init if provided (e.g., to initialize newly added blocks further)
    if (new_task_init) {
        new_task_init(&task->ctx);
    }

    printf("Successfully swapped to: %s\n", so_path);
}

void trim_leading(char *str) {
    char *start = str;
    while (isspace((unsigned char)*start)) start++;
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

void trim_trailing(char *str) {
    int len = (int)strlen(str);
    while (len > 0 && isspace((unsigned char)str[len - 1])) {
        len--;
    }
    str[len] = '\0';
}

int ends_with(const char *str, const char *suffix) {
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len) return 0;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

static int send_all(int fd, const void *data, size_t len)
{
    const char *p = (const char*)data;
    size_t remaining = len;

    while (remaining) {
        ssize_t n = send(fd, p, remaining, 0);
        if (n <= 0) return -1;
        p += (size_t)n;
        remaining -= (size_t)n;
    }
    return 0;
}

void *client_handler(void *arg)
{
    int client_fd = *((int *)arg);
    free(arg);

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;

    bytes_read = recv(client_fd, buffer, BUFFER_SIZE, 0);
    if (bytes_read <= 0) {
        close(client_fd);
        return NULL;
    }

    pthread_mutex_lock(&g_task.ctx_lock);

    /* Decide request type: SO path vs binary debug request */
    {
        int is_so = 0;

        /* Detect ".so" suffix (after trimming) */
        if (bytes_read < BUFFER_SIZE) {
            buffer[bytes_read] = '\0';
        } else {
            buffer[BUFFER_SIZE - 1] = '\0';
        }

        /* Make a working copy for trimming without risking binary corruption */
        {
            char textbuf[BUFFER_SIZE];
            size_t copy_len = (bytes_read < (ssize_t)(BUFFER_SIZE - 1)) ? (size_t)bytes_read : (size_t)(BUFFER_SIZE - 1);
            memcpy(textbuf, buffer, copy_len);
            textbuf[copy_len] = '\0';

            trim_leading(textbuf);
            trim_trailing(textbuf);

            {
                size_t len = strlen(textbuf);
                if (len >= 3 &&
                    textbuf[len - 3] == '.' &&
                    textbuf[len - 2] == 's' &&
                    textbuf[len - 1] == 'o') {
                    is_so = 1;
                }
            }

            if (is_so) {
                hot_swap(&g_task, textbuf);
                update_pub(&g_task, g_pub);
                pthread_mutex_unlock(&g_task.ctx_lock);
                close(client_fd);
                return NULL;
            }
        }
    }

    ///* Binary DebugRequest path */
    //if ((size_t)bytes_read < sizeof(DebugRequest)) {
    //    pthread_mutex_unlock(&g_task.ctx_lock);
    //    close(client_fd);
    //    return NULL;
    //}
//
    //{
    //    DebugRequest req;
    //    memcpy(&req, buffer, sizeof(req));
//
    //    uint64_t found_slot = (uint64_t)-1;
    //    const FieldInfo *field_info = NULL;
//
    //    for (uint64_t s = 0; s < g_task.allocated_slots; s++) {
    //        if (g_task.reg[s].field_count == 0 || g_task.reg[s].fields == NULL) continue;
//
    //        for (uint64_t p = 0; p < g_task.reg[s].field_count; p++) {
    //            if (g_task.reg[s].fields[p].field_id == req.field_id) {
    //                found_slot = s;
    //                field_info = &g_task.reg[s].fields[p];
    //                break;
    //            }
    //        }
    //        if (field_info) break;
    //    }
//
    //    if (field_info && found_slot != (uint64_t)-1 && g_task.ctx.slots[found_slot]) {
    //        char *base = (char *)g_task.ctx.slots[found_slot] + field_info->offset;
//
    //        /* Bounds check: ensure the field fits in the slot block */
    //        if ((size_t)field_info->offset + sizeof(float) <= g_task.reg[found_slot].block_size) {
//
    //            /* WRITE */
    //            if (req.op == DBG_OP_WRITE) {
//
    //                if (req.value_type != DBG_VT_F32 || req.value_len != sizeof(float)) {
    //                    fprintf(stderr, "Type mismatch for field_id=%llu\n",
    //                            (unsigned long long)req.field_id);
    //                    goto reply_read; /* still reply with current value */
    //                }
//
    //                *(float *)base = req.value.f32;
    //            }
//
    //    reply_read:
    //            /* READ (always return current value) */
    //            {
    //                float value = *(float *)base;
//
    //                DebugReply reply;
    //                memset(&reply, 0, sizeof(reply));
    //                reply.tx_id      = req.tx_id;
    //                reply.version    = req.version;
    //                reply.field_id   = field_info->field_id;
    //                reply.value_type = DBG_VT_F32;
    //                reply.value.f32  = value;
//
    //                (void)send_all(client_fd, &reply, sizeof(reply));
    //            }
//
    //        } else {
    //            fprintf(stderr,
    //                    "Bounds error: slot=%lu field_id=%llu offset=%lu block_size=%lu\n",
    //                    (unsigned long)found_slot,
    //                    (unsigned long long)field_info->field_id,
    //                    (unsigned long)field_info->offset,
    //                    (unsigned long)g_task.reg[found_slot].block_size);
    //        }
    //    }

    //}

    pthread_mutex_unlock(&g_task.ctx_lock);
    close(client_fd);
    return NULL;
}

void *server_background_thread(void *arg) {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(SERVER_PORT);

    bind(server_fd, (struct sockaddr *)&address, sizeof(address));
    listen(server_fd, 5);

    printf("[System] Control server started on port %d\n", SERVER_PORT);

    while (1) {
        int *new_sock = calloc(1, sizeof(int));
        *new_sock = accept(server_fd, NULL, NULL);
        if (*new_sock < 0) { free(new_sock); continue; }

        pthread_t tid;
        pthread_create(&tid, NULL, client_handler, new_sock);
        pthread_detach(tid);
    }
    return NULL;
}

int main() {
    pthread_t server_thread;
    if (pthread_create(&server_thread, NULL, server_background_thread, NULL) != 0) {
        perror("Could not create server thread");
        return 1;
    }
    pthread_detach(server_thread);

    g_task.allocated_slots = 0;
    g_task.reg =       calloc(MAX_SLOTS, sizeof(BlockRegistryEntry));
    g_task.ctx.slots = calloc(MAX_SLOTS, sizeof(void*));
    pthread_mutex_init(&g_task.ctx_lock, NULL);
    g_task.func = NULL;
    g_task.handle = NULL;

    // Create publisher
    dbg_pub_config_t cfg = DBG_PUB_CONFIG_DEFAULT;
    g_pub = dbg_pub_create(&cfg);

    while (1) {
        pthread_mutex_lock(&g_task.ctx_lock);
        if (g_task.func) {
            g_task.func(&g_task.ctx);
        }
        dbg_pub_send_all(g_pub);
        dbg_pub_poll_config(g_pub);

        pthread_mutex_unlock(&g_task.ctx_lock);

        usleep(100000);
    }

    return 0;
}