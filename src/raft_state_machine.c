#include "raft_state_machine.h"
#include "raft_dedup.h"

#include <stdlib.h>
#include <string.h>

void raft_kv_init(raft_kv_store_t *store)
{
    if (store != NULL) memset(store, 0, sizeof(*store));
}

void raft_kv_destroy(raft_kv_store_t *store)
{
    if (store == NULL) return;
    for (size_t i = 0; i < store->count; ++i) free(store->items[i].value);
    raft_kv_init(store);
}

int raft_kv_valid_key(const uint8_t *key, size_t key_len)
{
    if (key == NULL || key_len == 0 || key_len > RAFT_KV_MAX_KEY_BYTES) return 0;
    for (size_t i = 0; i < key_len; ++i) {
        uint8_t c = key[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return 0;
    }
    return 1;
}

static size_t locate(const raft_kv_store_t *store, const uint8_t *key,
                     size_t key_len)
{
    for (size_t i = 0; i < store->count; ++i)
        if (strlen(store->items[i].key) == key_len &&
            memcmp(store->items[i].key, key, key_len) == 0) return i;
    return store->count;
}

int raft_kv_get(const raft_kv_store_t *store, const uint8_t *key, size_t key_len,
                const uint8_t **value, size_t *value_len)
{
    if (store == NULL || value == NULL || value_len == NULL ||
        store->count > RAFT_KV_MAX_KEYS || !raft_kv_valid_key(key, key_len))
        return -1;
    size_t i = locate(store, key, key_len);
    *value = i == store->count ? NULL : store->items[i].value;
    *value_len = i == store->count ? 0 : store->items[i].value_len;
    return i == store->count ? 0 : 1;
}

/* return 1 for recognized command, 0 for deterministic no-op, -1 for OOM */
static int execute(raft_kv_store_t *store, const raft_log_entry_t *entry)
{
    const uint8_t *cmd = entry->command;
    size_t len = entry->command_len;
    int is_set = len >= 4 && memcmp(cmd, "SET ", 4) == 0;
    int is_del = len >= 4 && memcmp(cmd, "DEL ", 4) == 0;
    if (!is_set && !is_del) return 0;
    const size_t beginning = 4;
    size_t end = beginning;
    if (is_set) {
        while (end < len && cmd[end] != ' ') ++end;
        if (end == len) return 0; /* SET requires a value separator. */
    } else {
        end = len;
    }
    const size_t key_len = end - beginning;
    if (!raft_kv_valid_key(cmd + beginning, key_len)) return 0;
    const size_t i = locate(store, cmd + beginning, key_len);
    if (is_del) {
        if (i < store->count) {
            free(store->items[i].value);
            --store->count;
            if (i < store->count)
                memmove(&store->items[i], &store->items[i + 1],
                        (store->count - i) * sizeof(store->items[i]));
            memset(&store->items[store->count], 0, sizeof(store->items[0]));
        }
        return 1; /* DEL of absent key is successful no-op. */
    }
    const size_t value_len = len - (end + 1u);
    if (value_len > RAFT_KV_MAX_VALUE_BYTES ||
        (i == store->count && store->count == RAFT_KV_MAX_KEYS)) return 0;
    uint8_t *copy = NULL;
    if (value_len != 0) {
        copy = malloc(value_len);
        if (copy == NULL) return -1;
        memcpy(copy, cmd + end + 1u, value_len);
    }
    raft_kv_item_t *item = NULL;
    if (i == store->count) {
        item = &store->items[store->count++];
        memcpy(item->key, cmd + beginning, key_len);
        item->key[key_len] = '\0';
    } else {
        item = &store->items[i];
        free(item->value);
    }
    item->value = copy;
    item->value_len = value_len;
    return 1;
}

raft_status_t raft_apply_committed(raft_node_t *node, raft_kv_store_t *store,
                                   raft_index_t *out_applied)
{
    if (node == NULL || store == NULL || out_applied == NULL ||
        store->count > RAFT_KV_MAX_KEYS ||
        store->session_count > RAFT_KV_MAX_SESSIONS || !raft_node_is_valid(node))
        return RAFT_ERR_INVALID_ARGUMENT;
    raft_index_t completed = 0;
    while (node->last_applied < node->commit_index) {
        raft_index_t next = node->last_applied + 1u;
        const raft_log_entry_t *entry = raft_log_at(&node->log, next);
        if (entry == NULL) return RAFT_ERR_INVALID_ARGUMENT;
        int action = 0;
        raft_dedup_entry_t identity;
        int wrapped = raft_dedup_parse(entry->command, entry->command_len, &identity);
        if (wrapped == 1) {
            size_t slot=0;
            for (; slot<store->session_count; ++slot)
                if (store->sessions[slot].client_id == identity.client_id) break;
            /* Duplicate or out-of-order committed request: consume its index,
             * never execute the command a second time. */
            if (slot == store->session_count ||
                identity.request_id > store->sessions[slot].request_id) {
                if (slot < store->session_count ||
                    store->session_count < RAFT_KV_MAX_SESSIONS) {
                    raft_log_entry_t plain={entry->term,(uint8_t *)identity.command,
                                            identity.command_len};
                    action = execute(store, &plain);
                    if (action > 0) {
                        if (slot == store->session_count) ++store->session_count;
                        raft_kv_session_t *rec=&store->sessions[slot];
                        rec->client_id=identity.client_id;
                        rec->request_id=identity.request_id;
                        rec->index=next;
                        rec->term=entry->term;
                        rec->command_len=(uint16_t)identity.command_len;
                        memcpy(rec->command,identity.command,identity.command_len);
                    }
                }
            }
        } else if (wrapped == 0) {
            action = execute(store, entry);
        }
        if (action < 0) {
            *out_applied = completed;
            return RAFT_ERR_NO_MEMORY;
        }
        if (action) ++store->executed_commands;
        else ++store->ignored_commands;
        node->last_applied = next;
        ++completed;
    }
    *out_applied = completed;
    return RAFT_OK;
}
