// pread/pwrite for libounce: newlib declares them but Horizon homebrew has no implementation.
// The SDK uses native fsFileRead/fsFileWrite for files on fsdev devices (sdmc:), which take an
// offset and run in parallel; this is the fallback for other descriptors (e.g. romfs:). A
// process-wide lock keeps the seek + transfer + restore sequence atomic.
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>

static pthread_mutex_t g_positional_io = PTHREAD_MUTEX_INITIALIZER;

static ssize_t positional(int fd, void* buf, size_t count, off_t offset, int write_op) {
    pthread_mutex_lock(&g_positional_io);
    const off_t saved = lseek(fd, 0, SEEK_CUR);
    ssize_t result = -1;
    if (saved >= 0 && lseek(fd, offset, SEEK_SET) == offset) {
        result = write_op ? write(fd, buf, count) : read(fd, buf, count);
        const int err = errno;
        lseek(fd, saved, SEEK_SET);
        errno = err;
    }
    pthread_mutex_unlock(&g_positional_io);
    return result;
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    return positional(fd, buf, count, offset, 0);
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
    return positional(fd, (void*)buf, count, offset, 1);
}
