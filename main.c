#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <getopt.h>
#include <cjson/cJSON.h>

/* ── i3 IPC constants ────────────────────────────────────────────────────── */

#define VERSION         "0.1.0"

#define I3_MAGIC        "i3-ipc"
#define I3_MAGIC_LEN    6
#define I3_HEADER_SIZE  14      /* magic(6) + len(4) + type(4) */

#define MSG_RUN_COMMAND 0u
#define MSG_SUBSCRIBE   2u
#define MSG_GET_TREE    4u
#define EVENT_WINDOW    0x80000003u

/* ── Marks ───────────────────────────────────────────────────────────────── */

#define MARK_V  "__bat_prev_v"
#define MARK_H  "__bat_prev_h"

/* ── Global state ────────────────────────────────────────────────────────── */

static bool  g_debug  = false;
static int   g_cmd_fd = -1;

#define MAX_SKIP 64
static double g_skip[MAX_SKIP];
static int    g_skip_n = 0;

/* ── Logging ─────────────────────────────────────────────────────────────── */

#define LOG(...) do { if (g_debug) { printf(__VA_ARGS__); fflush(stdout); } } while (0)

/* ── Skip-focus set ──────────────────────────────────────────────────────── */

static bool skip_contains(double id) {
    for (int i = 0; i < g_skip_n; i++)
        if (g_skip[i] == id) return true;
    return false;
}

static void skip_add(double id) {
    if (g_skip_n < MAX_SKIP) g_skip[g_skip_n++] = id;
}

static void skip_remove(double id) {
    for (int i = 0; i < g_skip_n; i++) {
        if (g_skip[i] == id) { g_skip[i] = g_skip[--g_skip_n]; return; }
    }
}

/* ── IPC primitives ──────────────────────────────────────────────────────── */

static int ipc_connect(void) {
    const char *path = getenv("I3SOCK");
    static char path_buf[108];  /* unix socket path max on Linux */
    if (!path) {
        FILE *fp = popen("i3 --get-socketpath 2>/dev/null", "r");
        if (!fp || !fgets(path_buf, sizeof(path_buf), fp)) {
            fprintf(stderr, "Cannot find i3 socket\n");
            exit(1);
        }
        pclose(fp);
        path_buf[strcspn(path_buf, "\n")] = '\0';
        path = path_buf;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); exit(1);
    }
    return fd;
}

static void ipc_send(int fd, uint32_t type, const char *payload) {
    uint32_t len = payload ? (uint32_t)strlen(payload) : 0;
    char hdr[I3_HEADER_SIZE];
    memcpy(hdr,                    I3_MAGIC, I3_MAGIC_LEN);
    memcpy(hdr + I3_MAGIC_LEN,     &len,     4);
    memcpy(hdr + I3_MAGIC_LEN + 4, &type,    4);
    write(fd, hdr, I3_HEADER_SIZE);
    if (len) write(fd, payload, len);
}

static char *ipc_recv(int fd, uint32_t *type_out) {
    char hdr[I3_HEADER_SIZE];
    ssize_t n = 0;
    while (n < I3_HEADER_SIZE) {
        ssize_t r = read(fd, hdr + n, I3_HEADER_SIZE - n);
        if (r <= 0) return NULL;
        n += r;
    }
    if (memcmp(hdr, I3_MAGIC, I3_MAGIC_LEN) != 0) return NULL;
    uint32_t len, type;
    memcpy(&len,  hdr + I3_MAGIC_LEN,     4);
    memcpy(&type, hdr + I3_MAGIC_LEN + 4, 4);
    if (type_out) *type_out = type;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    n = 0;
    while ((uint32_t)n < len) {
        ssize_t r = read(fd, buf + n, len - n);
        if (r <= 0) { free(buf); return NULL; }
        n += r;
    }
    buf[len] = '\0';
    return buf;
}

/* ── Command helpers ─────────────────────────────────────────────────────── */

static void run_cmd(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    LOG("  cmd: %s\n", buf);
    ipc_send(g_cmd_fd, MSG_RUN_COMMAND, buf);
    char *resp = ipc_recv(g_cmd_fd, NULL);
    free(resp);
}

static cJSON *get_tree(void) {
    ipc_send(g_cmd_fd, MSG_GET_TREE, "");
    char *resp = ipc_recv(g_cmd_fd, NULL);
    if (!resp) return NULL;
    cJSON *tree = cJSON_Parse(resp);
    free(resp);
    return tree;
}

/* ── Tree traversal ──────────────────────────────────────────────────────── */

static double node_id(cJSON *node) {
    cJSON *id = cJSON_GetObjectItem(node, "id");
    return id ? id->valuedouble : 0.0;
}

static cJSON *find_by_id(cJSON *node, double id) {
    if (!node) return NULL;
    if (node_id(node) == id) return node;
    const char *keys[] = { "nodes", "floating_nodes" };
    for (int k = 0; k < 2; k++) {
        cJSON *child;
        cJSON_ArrayForEach(child, cJSON_GetObjectItem(node, keys[k])) {
            cJSON *found = find_by_id(child, id);
            if (found) return found;
        }
    }
    return NULL;
}

static cJSON *find_marked(cJSON *node, const char *mark) {
    if (!node) return NULL;
    cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(node, "marks")) {
        if (cJSON_IsString(m) && strcmp(m->valuestring, mark) == 0) return node;
    }
    const char *keys[] = { "nodes", "floating_nodes" };
    for (int k = 0; k < 2; k++) {
        cJSON *child;
        cJSON_ArrayForEach(child, cJSON_GetObjectItem(node, keys[k])) {
            cJSON *found = find_marked(child, mark);
            if (found) return found;
        }
    }
    return NULL;
}

static cJSON *find_parent_of(cJSON *node, double target_id) {
    if (!node) return NULL;
    const char *keys[] = { "nodes", "floating_nodes" };
    for (int k = 0; k < 2; k++) {
        cJSON *child;
        cJSON_ArrayForEach(child, cJSON_GetObjectItem(node, keys[k])) {
            if (node_id(child) == target_id) return node;
            cJSON *found = find_parent_of(child, target_id);
            if (found) return found;
        }
    }
    return NULL;
}

/* Returns the workspace id containing con_id, or 0 if not found. */
static double find_workspace_of(cJSON *node, double target_id, double cur_ws) {
    if (!node) return 0.0;
    cJSON *type_obj = cJSON_GetObjectItem(node, "type");
    bool is_ws = type_obj && cJSON_IsString(type_obj) &&
                 strcmp(type_obj->valuestring, "workspace") == 0;
    double ws = is_ws ? node_id(node) : cur_ws;
    if (node_id(node) == target_id) return ws;
    const char *keys[] = { "nodes", "floating_nodes" };
    for (int k = 0; k < 2; k++) {
        cJSON *child;
        cJSON_ArrayForEach(child, cJSON_GetObjectItem(node, keys[k])) {
            double found = find_workspace_of(child, target_id, ws);
            if (found != 0.0) return found;
        }
    }
    return 0.0;
}

/* ── Skip predicate ──────────────────────────────────────────────────────── */

static bool is_floating(cJSON *con) {
    cJSON *type = cJSON_GetObjectItem(con, "type");
    if (type && cJSON_IsString(type) &&
        strcmp(type->valuestring, "floating_con") == 0) return true;
    cJSON *floating = cJSON_GetObjectItem(con, "floating");
    if (floating && cJSON_IsString(floating) &&
        strstr(floating->valuestring, "_on")) return true;
    return false;
}

/* Full skip check — requires the tree to inspect parent layout. */
static bool should_skip(cJSON *con, cJSON *tree) {
    if (is_floating(con)) return true;
    cJSON *parent = find_parent_of(tree, node_id(con));
    if (parent) {
        cJSON *layout = cJSON_GetObjectItem(parent, "layout");
        if (layout && cJSON_IsString(layout)) {
            const char *l = layout->valuestring;
            if (strcmp(l, "stacked") == 0 || strcmp(l, "tabbed") == 0)
                return true;
        }
    }
    return false;
}

/* ── Event handlers ──────────────────────────────────────────────────────── */

static void on_focus(cJSON *con) {
    double id   = node_id(con);
    cJSON *name = cJSON_GetObjectItem(con, "name");
    cJSON *rect = cJSON_GetObjectItem(con, "rect");
    double w    = cJSON_GetObjectItem(rect, "width")->valuedouble;
    double h    = cJSON_GetObjectItem(rect, "height")->valuedouble;
    LOG("\n[FOCUS] '%s' id=%.0f rect=%.0fx%.0f\n",
        name ? name->valuestring : "?", id, w, h);

    /* Only floating check here — no tree fetch on every focus event.
       Stacked/tabbed windows may get marked but on_new's should_skip catches them. */
    if (is_floating(con)) { LOG("  → skipped (floating)\n"); return; }

    if (skip_contains(id)) {
        /* on_new already remarked this window with its correct post-move rect.
           The focus event carries a stale pre-move rect — don't overwrite. */
        LOG("  → skipping stale focus event (remarked by on_new)\n");
        skip_remove(id);
        return;
    }

    const char *mark = (h > w) ? MARK_V : MARK_H;
    LOG("  → marking as %s\n", mark);
    run_cmd("unmark " MARK_V);
    run_cmd("unmark " MARK_H);
    run_cmd("[con_id=%.0f] mark --replace %s", id, mark);
}

static void on_new(cJSON *event_con) {
    double c_id = node_id(event_con);
    cJSON *name = cJSON_GetObjectItem(event_con, "name");
    LOG("\n[NEW] '%s' id=%.0f\n", name ? name->valuestring : "?", c_id);

    cJSON *tree = get_tree();
    if (!tree) return;

    cJSON *c = find_by_id(tree, c_id);
    if (!c || should_skip(c, tree)) {
        LOG("  → c skipped or not found\n");
        cJSON_Delete(tree);
        return;
    }

    cJSON *b = find_marked(tree, MARK_V);
    const char *mark = MARK_V;
    if (!b) { b = find_marked(tree, MARK_H); mark = MARK_H; }
    if (!b) {
        LOG("  → no marked window, abort\n");
        cJSON_Delete(tree);
        return;
    }

    double b_id  = node_id(b);
    cJSON *b_name = cJSON_GetObjectItem(b, "name");
    cJSON *b_rect = cJSON_GetObjectItem(b, "rect");
    double bw    = cJSON_GetObjectItem(b_rect, "width")->valuedouble;
    double bh    = cJSON_GetObjectItem(b_rect, "height")->valuedouble;
    LOG("  b='%s' id=%.0f rect=%.0fx%.0f\n",
        b_name ? b_name->valuestring : "?", b_id, bw, bh);

    if (b_id == c_id || should_skip(b, tree)) {
        LOG("  → b == c or b skipped\n");
        cJSON_Delete(tree);
        return;
    }

    double b_ws = find_workspace_of(tree, b_id, 0.0);
    double c_ws = find_workspace_of(tree, c_id, 0.0);
    if (b_ws && c_ws && b_ws != c_ws) {
        LOG("  → different workspaces, abort\n");
        cJSON_Delete(tree);
        return;
    }

    /* Recalculate split direction from b's live rect, not the stale mark name. */
    const char *split = (bh > bw) ? "v" : "h";
    mark              = (bh > bw) ? MARK_V : MARK_H;
    LOG("  → split %s on b, move c to mark %s\n", split, mark);
    run_cmd("[con_id=%.0f] split %s", b_id, split);
    run_cmd("[con_id=%.0f] move to mark %s", c_id, mark);
    cJSON_Delete(tree);

    /* Remark c with its actual post-move dimensions.
       Add c to g_skip so the upcoming stale-rect focus event doesn't overwrite it. */
    tree = get_tree();
    if (!tree) return;
    c = find_by_id(tree, c_id);
    if (c && !should_skip(c, tree)) {
        cJSON *c_rect    = cJSON_GetObjectItem(c, "rect");
        double cw        = cJSON_GetObjectItem(c_rect, "width")->valuedouble;
        double ch        = cJSON_GetObjectItem(c_rect, "height")->valuedouble;
        const char *new_mark = (ch > cw) ? MARK_V : MARK_H;
        LOG("  → remark c: rect=%.0fx%.0f → %s\n", cw, ch, new_mark);
        run_cmd("unmark " MARK_V);
        run_cmd("unmark " MARK_H);
        run_cmd("[con_id=%.0f] mark --replace %s", c_id, new_mark);
        skip_add(c_id);
        LOG("  → added %.0f to g_skip\n", c_id);
    }
    cJSON_Delete(tree);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    static const struct option long_opts[] = {
        { "debug",   no_argument, NULL, 'd' },
        { "version", no_argument, NULL, 'v' },
        { "help",    no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "dvh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'd': g_debug = true; break;
        case 'v': printf("better_autotile %s\n", VERSION); return 0;
        case 'h':
            printf("Usage: better_autotile [-d|--debug] [-v|--version]\n");
            return 0;
        default:
            fprintf(stderr, "Usage: better_autotile [-d|--debug] [-v|--version]\n");
            return 1;
        }
    }

    int evt_fd = ipc_connect();
    g_cmd_fd   = ipc_connect();

    ipc_send(evt_fd, MSG_SUBSCRIBE, "[\"window\"]");
    char *sub = ipc_recv(evt_fd, NULL);
    free(sub);

    printf("better_autotile running (debug=%s)\n", g_debug ? "true" : "false");

    for (;;) {
        uint32_t type;
        char *payload = ipc_recv(evt_fd, &type);
        if (!payload) break;

        if (type == EVENT_WINDOW) {
            cJSON *root = cJSON_Parse(payload);
            if (root) {
                cJSON *change    = cJSON_GetObjectItem(root, "change");
                cJSON *container = cJSON_GetObjectItem(root, "container");
                if (change && container && cJSON_IsString(change)) {
                    if      (strcmp(change->valuestring, "focus") == 0) on_focus(container);
                    else if (strcmp(change->valuestring, "new")   == 0) on_new(container);
                }
                cJSON_Delete(root);
            }
        }
        free(payload);
    }
    return 0;
}
