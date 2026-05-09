#include "xxfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(HAVE_READLINE)
#include <readline/readline.h>
#include <readline/history.h>
#endif

static struct xxfs *g_fs;
static char g_cwd[XXFS_MAX_PATH] = "/";

static void path_resolve(const char *input, char *out)
{
    if (input[0] == '/') {
        snprintf(out, XXFS_MAX_PATH, "%s", input);
    } else {
        snprintf(out, XXFS_MAX_PATH, "%s/%s", g_cwd, input);
    }
    char norm[XXFS_MAX_PATH];
    u32 nlen;
    char tmp[XXFS_MAX_PATH];
    snprintf(tmp, XXFS_MAX_PATH, "%s", out);
    const char *p = tmp;
    u32 len = 0;
    if (*p != '/') norm[len++] = '/';
    while (*p) {
        if (*p == '/' && len > 0 && norm[len-1] == '/') { p++; continue; }
        if (*p == '.' && (*(p+1) == '/' || *(p+1) == 0) &&
            (len == 0 || norm[len-1] == '/')) { p++; continue; }
        if (*p == '.' && *(p+1) == '.' && (*(p+2) == '/' || *(p+2) == 0)) {
            p += 2;
            if (len > 1) {
                len--;
                while (len > 1 && norm[len-1] != '/') len--;
            }
            continue;
        }
        norm[len++] = *p++;
        if (len >= XXFS_MAX_PATH - 1) break;
    }
    while (len > 1 && norm[len-1] == '/') len--;
    norm[len] = 0;
    memcpy(out, norm, len + 1);
}

static void cmd_ls(const char *arg)
{
    char path[XXFS_MAX_PATH];
    if (arg && arg[0])
        path_resolve(arg, path);
    else
        snprintf(path, XXFS_MAX_PATH, "%s", g_cwd);

    struct xxfs_readdir_ctx ctx;
    int rc = xxfs_readdir(g_fs, path, &ctx);
    if (rc == XXFS_ENOENT) {
        printf("  ls: %s: No such file or directory\n", path);
        return;
    }
    if (rc == XXFS_ENOTDIR) {
        printf("  ls: %s: Not a directory\n", path);
        return;
    }
    if (rc != XXFS_OK) {
        printf("  ls: %s: error %d\n", path, rc);
        return;
    }

    if (ctx.count == 0) {
        printf("  (empty directory)\n");
    } else {
        for (u32 i = 0; i < ctx.count; i++) {
            const char *type = "file";
            if (ctx.entries[i].dc_type == XXFS_FT_DIR) type = "dir ";
            else if (ctx.entries[i].dc_type == XXFS_FT_LNK) type = "link";
            printf("  %s  %s\n", type, ctx.entries[i].dc_name);
        }
    }
    xxfs_readdir_free(&ctx);
}

static void cmd_cd(const char *arg)
{
    if (!arg || !arg[0]) {
        snprintf(g_cwd, XXFS_MAX_PATH, "/");
        return;
    }
    char path[XXFS_MAX_PATH];
    path_resolve(arg, path);
    struct xxfs_inode ino;
    if (xxfs_stat(g_fs, path, &ino) != XXFS_OK) {
        printf("  cd: %s: not found\n", path);
        return;
    }
    if (ino.i_file_type != XXFS_FT_DIR) {
        printf("  cd: %s: not a directory\n", path);
        return;
    }
    snprintf(g_cwd, XXFS_MAX_PATH, "%s", path);
}

static void cmd_pwd(void)
{
    printf("  %s\n", g_cwd);
}

static void cmd_stat(const char *arg)
{
    if (!arg || !arg[0]) { printf("  usage: stat <path>\n"); return; }
    char path[XXFS_MAX_PATH];
    path_resolve(arg, path);
    struct xxfs_inode ino;
    if (xxfs_stat(g_fs, path, &ino) != XXFS_OK) {
        printf("  stat: %s: not found\n", path);
        return;
    }
    const char *type_str = "unknown";
    switch (ino.i_file_type) {
    case XXFS_FT_REG:  type_str = "regular"; break;
    case XXFS_FT_DIR:  type_str = "directory"; break;
    case XXFS_FT_LNK:  type_str = "symlink"; break;
    case XXFS_FT_BLK:  type_str = "blockdev"; break;
    case XXFS_FT_CHR:  type_str = "chardev"; break;
    case XXFS_FT_FIFO: type_str = "fifo"; break;
    case XXFS_FT_SOCK: type_str = "socket"; break;
    }
    printf("  File: %s\n", ino.i_name);
    printf("  Type: %s\n", type_str);
    printf("  Mode: %o\n", ino.i_mode);
    printf("  Uid:  %d  Gid: %d\n", ino.i_uid, ino.i_gid);
    printf("  Size: %lu bytes\n", (unsigned long)ino.i_size);
    printf("  Blocks: %lu\n", (unsigned long)ino.i_blocks);
    printf("  Links: %u\n", ino.i_nlinks);
    printf("  Access: %lu\n", (unsigned long)ino.i_atime);
    printf("  Modify: %lu\n", (unsigned long)ino.i_mtime);
    printf("  Change: %lu\n", (unsigned long)ino.i_ctime);
    printf("  Birth:  %lu\n", (unsigned long)ino.i_btime);
    printf("  Gen:    %lu\n", (unsigned long)ino.i_generation);
    printf("  CRC:    0x%08x\n", ino.i_checksum);
    if (ino.i_file_type == XXFS_FT_LNK) {
        char target[XXFS_INLINE_MAX + 1];
        u32 tlen = sizeof(target);
        if (xxfs_readlink(g_fs, path, target, &tlen) == XXFS_OK) {
            target[tlen] = 0;
            printf("  Target: %s\n", target);
        }
    }
}

static void cmd_mkdir(const char *arg)
{
    if (!arg || !arg[0]) { printf("  usage: mkdir <path>\n"); return; }
    char path[XXFS_MAX_PATH];
    path_resolve(arg, path);
    int rc = xxfs_mkdir(g_fs, path, 0755, 0, 0);
    if (rc != XXFS_OK) printf("  mkdir: failed (rc=%d)\n", rc);
    else printf("  created directory: %s\n", path);
}

static void cmd_touch(const char *arg)
{
    if (!arg || !arg[0]) { printf("  usage: touch <path>\n"); return; }
    char path[XXFS_MAX_PATH];
    path_resolve(arg, path);
    int rc = xxfs_create(g_fs, path, 0644, 0, 0);
    if (rc == XXFS_EEXIST) printf("  touch: already exists\n");
    else if (rc != XXFS_OK) printf("  touch: failed (rc=%d)\n", rc);
    else printf("  created: %s\n", path);
}

static void cmd_rm(const char *arg)
{
    if (!arg || !arg[0]) { printf("  usage: rm <path>\n"); return; }
    char path[XXFS_MAX_PATH];
    path_resolve(arg, path);
    int rc = xxfs_unlink(g_fs, path);
    if (rc == XXFS_ENOENT) printf("  rm: not found\n");
    else if (rc == XXFS_EISDIR) printf("  rm: is a directory (use rmdir)\n");
    else if (rc != XXFS_OK) printf("  rm: failed (rc=%d)\n", rc);
    else printf("  removed: %s\n", path);
}

static void cmd_rmdir(const char *arg)
{
    if (!arg || !arg[0]) { printf("  usage: rmdir <path>\n"); return; }
    char path[XXFS_MAX_PATH];
    path_resolve(arg, path);
    int rc = xxfs_rmdir(g_fs, path);
    if (rc == XXFS_ENOENT) printf("  rmdir: not found\n");
    else if (rc == XXFS_ENOTDIR) printf("  rmdir: not a directory\n");
    else if (rc != XXFS_OK) printf("  rmdir: failed (rc=%d)\n", rc);
    else printf("  removed directory: %s\n", path);
}

static void cmd_cat(const char *arg)
{
    if (!arg || !arg[0]) { printf("  usage: cat <path>\n"); return; }
    char path[XXFS_MAX_PATH];
    path_resolve(arg, path);
    char buf[65536];
    u32 rb;
    int rc = xxfs_read(g_fs, path, buf, 0, sizeof(buf) - 1, &rb);
    if (rc != XXFS_OK) { printf("  cat: read failed (rc=%d)\n", rc); return; }
    buf[rb] = 0;
    printf("%s", buf);
    if (rb > 0 && buf[rb-1] != '\n') printf("\n");
}

static void cmd_write(const char *arg)
{
    if (!arg || !arg[0]) { printf("  usage: write <path> <content>\n"); return; }
    char path_buf[XXFS_MAX_PATH], resolved[XXFS_MAX_PATH];
    const char *space = strchr(arg, ' ');
    if (!space) { printf("  usage: write <path> <content>\n"); return; }
    u32 plen = (u32)(space - arg);
    if (plen >= XXFS_MAX_PATH) plen = XXFS_MAX_PATH - 1;
    memcpy(path_buf, arg, plen);
    path_buf[plen] = 0;
    path_resolve(path_buf, resolved);
    const char *content = space + 1;
    u32 clen = (u32)strlen(content);
    u32 written;
    int rc = xxfs_write(g_fs, resolved, content, 0, clen, &written);
    if (rc != XXFS_OK) printf("  write: failed (rc=%d)\n", rc);
    else printf("  wrote %u bytes to %s\n", written, resolved);
}

static void cmd_chmod(const char *arg)
{
    if (!arg || !arg[0]) { printf("  usage: chmod <mode> <path>\n"); return; }
    char mode_str[8];
    const char *space = strchr(arg, ' ');
    if (!space) { printf("  usage: chmod <mode> <path>\n"); return; }
    u32 mlen = (u32)(space - arg);
    if (mlen >= sizeof(mode_str)) mlen = sizeof(mode_str) - 1;
    memcpy(mode_str, arg, mlen);
    mode_str[mlen] = 0;
    u16 mode = (u16)strtol(mode_str, NULL, 8);
    char path[XXFS_MAX_PATH];
    path_resolve(space + 1, path);
    int rc = xxfs_chmod(g_fs, path, mode);
    if (rc != XXFS_OK) printf("  chmod: failed (rc=%d)\n", rc);
    else printf("  mode changed to %o\n", mode);
}

static void cmd_chown(const char *arg)
{
    if (!arg || !arg[0]) { printf("  usage: chown <uid>:<gid> <path>\n"); return; }
    const char *sp1 = strchr(arg, ' ');
    if (!sp1) { printf("  usage: chown <uid>:<gid> <path>\n"); return; }
    char ug_str[32];
    u32 ulen = (u32)(sp1 - arg);
    if (ulen >= sizeof(ug_str)) ulen = sizeof(ug_str) - 1;
    memcpy(ug_str, arg, ulen);
    ug_str[ulen] = 0;
    u16 uid = 0, gid = 0;
    const char *colon = strchr(ug_str, ':');
    if (colon) {
        uid = (u16)atoi(ug_str);
        gid = (u16)atoi(colon + 1);
    } else {
        uid = (u16)atoi(ug_str);
    }
    char path[XXFS_MAX_PATH];
    path_resolve(sp1 + 1, path);
    int rc = xxfs_chown(g_fs, path, uid, gid);
    if (rc != XXFS_OK) printf("  chown: failed (rc=%d)\n", rc);
    else printf("  owner changed to %d:%d\n", uid, gid);
}

static void cmd_rename(const char *arg)
{
    if (!arg || !arg[0]) { printf("  usage: rename <old> <new>\n"); return; }
    const char *sp = strchr(arg, ' ');
    if (!sp) { printf("  usage: rename <old> <new>\n"); return; }
    char old_buf[XXFS_MAX_PATH], new_buf[XXFS_MAX_PATH];
    char old_path[XXFS_MAX_PATH], new_path[XXFS_MAX_PATH];
    u32 olen = (u32)(sp - arg);
    if (olen >= XXFS_MAX_PATH) olen = XXFS_MAX_PATH - 1;
    memcpy(old_buf, arg, olen);
    old_buf[olen] = 0;
    snprintf(new_buf, XXFS_MAX_PATH, "%s", sp + 1);
    path_resolve(old_buf, old_path);
    path_resolve(new_buf, new_path);
    int rc = xxfs_rename(g_fs, old_path, new_path);
    if (rc != XXFS_OK) printf("  rename: failed (rc=%d)\n", rc);
    else printf("  renamed %s -> %s\n", old_path, new_path);
}

static void cmd_symlink(const char *arg)
{
    if (!arg || !arg[0]) { printf("  usage: symlink <target> <linkpath>\n"); return; }
    const char *sp = strchr(arg, ' ');
    if (!sp) { printf("  usage: symlink <target> <linkpath>\n"); return; }
    char target[XXFS_MAX_PATH], linkpath[XXFS_MAX_PATH];
    u32 tlen = (u32)(sp - arg);
    if (tlen >= XXFS_MAX_PATH) tlen = XXFS_MAX_PATH - 1;
    memcpy(target, arg, tlen);
    target[tlen] = 0;
    path_resolve(sp + 1, linkpath);
    int rc = xxfs_symlink(g_fs, target, linkpath);
    if (rc != XXFS_OK) printf("  symlink: failed (rc=%d)\n", rc);
    else printf("  symlink: %s -> %s\n", linkpath, target);
}

static void cmd_df(void)
{
    struct xxfs_fs_info info;
    if (xxfs_info(g_fs, &info) != XXFS_OK) return;
    printf("  Filesystem: XXFS v%u\n", info.version);
    printf("  Block size: %u\n", info.block_size);
    printf("  Total blocks: %lu\n", (unsigned long)info.block_count);
    printf("  Free blocks: %lu\n", (unsigned long)info.free_blocks);
    printf("  Total inodes: %lu\n", (unsigned long)info.inodes_count);
    printf("  COW generation: %lu\n", (unsigned long)info.cow_generation);
    u64 used = info.block_count - info.free_blocks;
    printf("  Used: %lu blocks (%lu MB)\n",
           (unsigned long)used, (unsigned long)(used * info.block_size / (1024*1024)));
}

static void cmd_help(void)
{
    printf("  XXFS Shell Commands:\n");
    printf("  ls [path]          - list directory\n");
    printf("  cd <path>          - change directory\n");
    printf("  pwd                - print working directory\n");
    printf("  stat <path>        - show file info\n");
    printf("  touch <path>       - create empty file\n");
    printf("  mkdir <path>       - create directory\n");
    printf("  rm <path>          - remove file\n");
    printf("  rmdir <path>       - remove directory\n");
    printf("  cat <path>         - read file content\n");
    printf("  write <p> <text>   - write text to file\n");
    printf("  chmod <mode> <p>   - change permissions\n");
    printf("  chown <u:g> <p>    - change owner\n");
    printf("  rename <old> <new> - rename file\n");
    printf("  symlink <t> <l>    - create symlink\n");
    printf("  df                 - show filesystem info\n");
    printf("  sync               - sync to disk\n");
    printf("  help               - show this help\n");
    printf("  exit               - exit shell\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "usage: xxfs-shell <image>\n");
        return 1;
    }

    const char *img = argv[1];
    g_fs = xxfs_mount(img, XXFS_FLAG_SYNC);
    if (!g_fs) {
        fprintf(stderr, "failed to mount %s\n", img);
        return 1;
    }

    printf("XXFS Shell - mounted %s\n", img);
    printf("Type 'help' for commands, 'exit' to quit.\n\n");

    while (1) {
        char prompt[128];
        snprintf(prompt, sizeof(prompt), "xxfs:%s> ", g_cwd);

        char *line;
#if defined(HAVE_READLINE)
        line = readline(prompt);
        if (!line) break;
        if (line[0]) add_history(line);
#else
        char buf[4096];
        printf("%s", prompt);
        fflush(stdout);
        if (!fgets(buf, sizeof(buf), stdin)) break;
        buf[strcspn(buf, "\n")] = 0;
        line = buf;
#endif
        if (line[0] == 0) {
#if defined(HAVE_READLINE)
            free(line);
#endif
            continue;
        }

        char cmd[64] = {0};
        char *arg = NULL;
        const char *sp = strchr(line, ' ');
        if (sp) {
            u32 clen = (u32)(sp - line);
            if (clen >= sizeof(cmd)) clen = sizeof(cmd) - 1;
            memcpy(cmd, line, clen);
            arg = (char *)(sp + 1);
        } else {
            snprintf(cmd, sizeof(cmd), "%s", line);
        }

        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
#if defined(HAVE_READLINE)
            free(line);
#endif
            break;
        }
        else if (strcmp(cmd, "ls") == 0) cmd_ls(arg);
        else if (strcmp(cmd, "cd") == 0) cmd_cd(arg);
        else if (strcmp(cmd, "pwd") == 0) cmd_pwd();
        else if (strcmp(cmd, "stat") == 0) cmd_stat(arg);
        else if (strcmp(cmd, "touch") == 0) cmd_touch(arg);
        else if (strcmp(cmd, "mkdir") == 0) cmd_mkdir(arg);
        else if (strcmp(cmd, "rm") == 0) cmd_rm(arg);
        else if (strcmp(cmd, "rmdir") == 0) cmd_rmdir(arg);
        else if (strcmp(cmd, "cat") == 0) cmd_cat(arg);
        else if (strcmp(cmd, "write") == 0) cmd_write(arg);
        else if (strcmp(cmd, "chmod") == 0) cmd_chmod(arg);
        else if (strcmp(cmd, "chown") == 0) cmd_chown(arg);
        else if (strcmp(cmd, "rename") == 0) cmd_rename(arg);
        else if (strcmp(cmd, "symlink") == 0) cmd_symlink(arg);
        else if (strcmp(cmd, "df") == 0) cmd_df();
        else if (strcmp(cmd, "sync") == 0) { xxfs_sync(g_fs); printf("  synced\n"); }
        else if (strcmp(cmd, "help") == 0) cmd_help();
        else printf("  unknown command: %s (type 'help')\n", cmd);

#if defined(HAVE_READLINE)
        free(line);
#endif
    }

    xxfs_umount(g_fs);
    printf("Goodbye.\n");
    return 0;
}
