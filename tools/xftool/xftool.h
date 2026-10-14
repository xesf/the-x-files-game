/* xftool: command-line access to the game's files. */
#ifndef XFTOOL_H
#define XFTOOL_H

typedef struct {
    const char *name;
    const char *usage;
    int (*run)(int argc, char **argv); /* argv[0] is the command name */
} xftool_cmd;

extern const xftool_cmd xftool_pff_cmds[];
extern const xftool_cmd xftool_hot_cmds[];
extern const xftool_cmd xftool_mov_cmds[];

int xftool_write_file(const char *path, const void *data, unsigned long size);

#endif
