#include "../hal_process.h"
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>

extern "C" {
    extern volatile int LVGL_HOME_KEY_FLAGE;
}

/*
 * TCA8418 keyboard sends rapid DOWN+UP pairs even when held.
 * We count ESC presses: 5 presses within 5 seconds = kill child.
 */
static const int ESC_KILL_COUNT = 5;
static const int ESC_KILL_WINDOW_SEC = 5;

int hal_process_exec_blocking(const char *exec_path, volatile int *home_key_flag)
{
    (void)home_key_flag;

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execlp("/bin/sh", "sh", "-c", exec_path, (char *)NULL);
        _exit(127);
    }

    int esc_count = 0;
    auto esc_window_start = std::chrono::steady_clock::now();
    bool prev_esc_state = false;
    int status = 0;

    while (true) {
        int r = waitpid(pid, &status, WNOHANG);
        if (r > 0) break;
        if (r < 0) { status = -1; break; }

        bool esc_now = LVGL_HOME_KEY_FLAGE != 0;
        if (esc_now && !prev_esc_state) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - esc_window_start).count();
            if (elapsed > ESC_KILL_WINDOW_SEC) {
                esc_count = 0;
                esc_window_start = now;
            }
            esc_count++;
            printf("[hal] ESC press #%d/%d (window %lds)\n",
                   esc_count, ESC_KILL_COUNT, (long)elapsed);

            if (esc_count >= ESC_KILL_COUNT) {
                printf("[hal] ESC x%d, killing child %d\n", esc_count, pid);
                kill(pid, SIGTERM);
                auto t0 = std::chrono::steady_clock::now();
                while (waitpid(pid, &status, WNOHANG) == 0) {
                    if (std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - t0).count() >= 3) {
                        kill(pid, SIGKILL);
                        waitpid(pid, &status, 0);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                break;
            }
        }
        prev_esc_state = esc_now;

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

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
