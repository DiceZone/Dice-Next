// Android 10+ 禁止从应用数据目录 exec 用户二进制（W^X）。
// 本库经 LD_PRELOAD 注入托管子进程：把对应用目录里 ELF 的 execve
// 改成执行 /system/bin/linker64（系统分区，允许执行），由链接器加载该 ELF。
// 不要 System.load 进 ART 主进程。

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char** environ;

static int elf_class(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    unsigned char mag[5];
    ssize_t n = read(fd, mag, 5);
    close(fd);
    if (n < 5 || mag[0] != 0x7f || mag[1] != 'E' || mag[2] != 'L' || mag[3] != 'F') return 0;
    if (mag[4] == 1 || mag[4] == 2) return mag[4];
    return 0;
}

static int should_wrap(const char* path) {
    if (!path || path[0] != '/') return 0;
    if (strncmp(path, "/system/", 8) == 0 || strncmp(path, "/vendor/", 8) == 0 ||
        strncmp(path, "/apex/", 6) == 0 || strncmp(path, "/bin/", 5) == 0) return 0;
    return elf_class(path) != 0;
}

static int wrap_execve(int (*real)(const char*, char* const[], char* const[]),
                       const char* path, char* const argv[], char* const envp[]) {
    const int klass = elf_class(path);
    const char* linker = klass == 1 ? "/system/bin/linker" : "/system/bin/linker64";
    int n = 0;
    while (argv && argv[n]) n++;
    char** wrapped = (char**)malloc((size_t)(n + 2) * sizeof(char*));
    if (!wrapped) return real(path, argv, envp);
    wrapped[0] = (char*)linker;
    for (int i = 0; i < n; i++) wrapped[i + 1] = argv[i];
    wrapped[n + 1] = NULL;
    int rc = real(linker, wrapped, envp);
    int err = errno;
    free(wrapped);
    errno = err;
    return rc;
}

int execve(const char* path, char* const argv[], char* const envp[]) {
    static int (*real)(const char*, char* const[], char* const[]) = NULL;
    if (!real) {
        real = (int (*)(const char*, char* const[], char* const[]))dlsym(RTLD_NEXT, "execve");
        if (!real) {
            errno = ENOSYS;
            return -1;
        }
    }
    if (should_wrap(path)) return wrap_execve(real, path, argv, envp);
    return real(path, argv, envp);
}

int execv(const char* path, char* const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char* file, char* const argv[]) {
    static int (*real)(const char*, char* const[]) = NULL;
    if (!real) real = (int (*)(const char*, char* const[]))dlsym(RTLD_NEXT, "execvp");
    if (file && strchr(file, '/') && should_wrap(file)) return execve(file, argv, environ);
    if (!real) {
        errno = ENOSYS;
        return -1;
    }
    return real(file, argv);
}

int execvpe(const char* file, char* const argv[], char* const envp[]) {
    static int (*real)(const char*, char* const[], char* const[]) = NULL;
    if (!real) real = (int (*)(const char*, char* const[], char* const[]))dlsym(RTLD_NEXT, "execvpe");
    if (file && strchr(file, '/') && should_wrap(file)) return execve(file, argv, envp);
    if (!real) return execve(file, argv, envp);
    return real(file, argv, envp);
}
