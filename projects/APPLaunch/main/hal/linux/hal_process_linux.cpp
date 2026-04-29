#include "../hal_process.h"
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <linux/input.h>

extern "C" {
    extern void keyboard_pause(void);
    extern void keyboard_resume(void);
}

static const char *get_kbd_device()
{
    const char *env = getenv("APPLAUNCH_LINUX_KEYBOARD_DEVICE");
    return env ? env : "/dev/input/by-path/platform-3f804000.i2c-event";
}

int hal_process_exec_blocking(const char *exec_path, volatile int *home_key_flag)
{
    (void)home_key_flag;

    keyboard_pause();

    pid_t pid = fork();
    if (pid < 0) { keyboard_resume(); return -1; }
    if (pid == 0) {
        execlp("/bin/sh", "sh", "-c", exec_path, (char *)NULL);
        _exit(127);
    }

    int evfd = open(get_kbd_device(), O_RDONLY | O_NONBLOCK);
    if (evfd >= 0) {
        ioctl(evfd, EVIOCGRAB, 1);
    }

    auto home_pressed_since = std::chrono::steady_clock::time_point{};
    bool home_held = false;
    int status = 0;

    while (true) {
        int r = waitpid(pid, &status, WNOHANG);
        if (r > 0) break;
        if (r < 0) { status = -1; break; }

        if (evfd >= 0) {
            struct input_event ev;
            while (read(evfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
                if (ev.type == EV_KEY && ev.code == KEY_ESC) {
                    if (ev.value == 1) {
                        home_held = true;
                        home_pressed_since = std::chrono::steady_clock::now();
                        printf("[hal] ESC pressed\n");
                    } else if (ev.value == 0) {
                        home_held = false;
                        printf("[hal] ESC released\n");
                    }
                }
            }
        }

        if (home_held) {
            auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - home_pressed_since).count();
            if (secs >= 5) {
                printf("[hal] ESC held %lds, SIGTERM %d\n", (long)secs, pid);
                kill(pid, SIGTERM);
                auto t0 = std::chrono::steady_clock::now();
                while (waitpid(pid, &status, WNOHANG) == 0) {
                    if (std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - t0).count() >= 3) {
                        printf("[hal] SIGKILL %d\n", pid);
                        kill(pid, SIGKILL);
                        waitpid(pid, &status, 0);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (evfd >= 0) {
        ioctl(evfd, EVIOCGRAB, 0);
        close(evfd);
    }

    keyboard_resume();

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

int hal_process_check_lock(const char *lock_path, int *holder_pid)
{
    *holder_pid = 0;
    int fd = open(lock_path, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return -1;
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_GETLK, &fl) == -1) { close(fd); return -1; }
    close(fd);
    if (fl.l_type != F_UNLCK) {
        *holder_pid = fl.l_pid;
        return fl.l_pid;
    }
    return 0;
}

void hal_process_kill(int pid, int grace_ms)
{
    if (pid <= 0) return;
    kill(pid, SIGINT);
    auto start = std::chrono::steady_clock::now();
    while (true) {
        int status;
        if (waitpid(pid, &status, WNOHANG) != 0) return;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= grace_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

hal_pid_t hal_process_spawn(const char *exec_path)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execlp("/bin/sh", "sh", "-c", exec_path, (char *)NULL);
        _exit(127);
    }
    return (hal_pid_t)pid;
}

void hal_process_stop(hal_pid_t pid)
{
    if (pid <= 0) return;
    kill((pid_t)pid, SIGTERM);
    int status;
    waitpid((pid_t)pid, &status, WNOHANG);
}
