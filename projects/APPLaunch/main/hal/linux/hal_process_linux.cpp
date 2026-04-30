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
#include <linux/input.h>
#include <linux/uinput.h>

extern "C" {
    extern void keyboard_pause(void);
    extern void keyboard_resume(void);
}

static const char *get_kbd_device()
{
    const char *env = getenv("APPLAUNCH_LINUX_KEYBOARD_DEVICE");
    return env ? env : "/dev/input/by-path/platform-3f804000.i2c-event";
}

static int create_uinput_kbd()
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        perror("[hal] open /dev/uinput");
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_REP);
    for (int i = 0; i < KEY_MAX; i++)
        ioctl(fd, UI_SET_KEYBIT, i);

    struct uinput_setup setup = {};
    snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "APPLaunch-vkbd");
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1234;
    setup.id.product = 0x5678;
    setup.id.version = 1;

    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0) {
        perror("[hal] UI_DEV_SETUP");
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("[hal] UI_DEV_CREATE");
        close(fd);
        return -1;
    }

    usleep(200000);
    return fd;
}

static void forward_event(int uinput_fd, const struct input_event *ev)
{
    write(uinput_fd, ev, sizeof(*ev));
}

static const int ESC_HOLD_SEC = 3;

int hal_process_exec_blocking(const char *exec_path, volatile int *home_key_flag)
{
    (void)home_key_flag;

    keyboard_pause();

    int evfd = open(get_kbd_device(), O_RDONLY);
    if (evfd < 0) {
        perror("[hal] open evdev");
        keyboard_resume();
        return -1;
    }

    if (ioctl(evfd, EVIOCGRAB, 1) < 0) {
        perror("[hal] EVIOCGRAB");
        close(evfd);
        keyboard_resume();
        return -1;
    }
    printf("[hal] Grabbed evdev exclusively\n");

    int uifd = create_uinput_kbd();
    if (uifd < 0) {
        ioctl(evfd, EVIOCGRAB, 0);
        close(evfd);
        keyboard_resume();
        return -1;
    }
    printf("[hal] Created uinput virtual keyboard\n");

    pid_t pid = fork();
    if (pid < 0) {
        ioctl(uifd, UI_DEV_DESTROY);
        close(uifd);
        ioctl(evfd, EVIOCGRAB, 0);
        close(evfd);
        keyboard_resume();
        return -1;
    }
    if (pid == 0) {
        close(evfd);
        close(uifd);
        // Put child in its own session/process group so any grandchildren it
        // forks (e.g. a wrapper script launching a GUI binary) can be killed
        // together via killpg(). Without setsid() a SIGKILL on the shell
        // leaves orphaned children writing to the framebuffer.
        setsid();
        execlp("/bin/sh", "sh", "-c", exec_path, (char *)NULL);
        _exit(127);
    }

    fcntl(evfd, F_SETFL, O_RDONLY | O_NONBLOCK);

    auto esc_down_since = std::chrono::steady_clock::time_point{};
    bool esc_down = false;
    int status = 0;

    while (true) {
        int r = waitpid(pid, &status, WNOHANG);
        if (r > 0) break;
        if (r < 0) { status = -1; break; }

        struct input_event ev;
        while (read(evfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY) {
                const char *st = (ev.value == 1) ? "DOWN" :
                                 (ev.value == 0) ? "UP"   :
                                 (ev.value == 2) ? "REPEAT" : "???";
                printf("[HAL-EXT] evdev code=%u value=%d(%s) (to child via uinput)\n",
                       ev.code, ev.value, st);
            }
            if (ev.type == EV_KEY && ev.code == KEY_ESC) {
                if (ev.value == 1) {
                    esc_down = true;
                    esc_down_since = std::chrono::steady_clock::now();
                    printf("[HAL-EXT] ESC DOWN\n");
                } else if (ev.value == 0) {
                    esc_down = false;
                    printf("[HAL-EXT] ESC UP\n");
                }
            } else {
                forward_event(uifd, &ev);
            }
        }

        if (esc_down) {
            auto held_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - esc_down_since).count();
            if (held_ms >= ESC_HOLD_SEC * 1000) {
                // Child was launched with setsid(), so its pid == pgid.
                // killpg() targets the entire process group so any
                // wrapper-spawned grandchildren (e.g. NC2000) die too.
                printf("[hal] ESC held %ldms, SIGTERM pgrp %d\n", (long)held_ms, pid);
                killpg(pid, SIGTERM);
                auto t0 = std::chrono::steady_clock::now();
                while (waitpid(pid, &status, WNOHANG) == 0) {
                    if (std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - t0).count() >= 3) {
                        printf("[hal] SIGKILL pgrp %d\n", pid);
                        killpg(pid, SIGKILL);
                        waitpid(pid, &status, 0);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    ioctl(uifd, UI_DEV_DESTROY);
    close(uifd);
    ioctl(evfd, EVIOCGRAB, 0);
    close(evfd);

    keyboard_resume();

    printf("[hal] Returned to launcher\n");
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
    // pid here is the session/pgid leader (spawned with setsid()).
    // Use killpg() so children the app forked are killed too, and
    // escalate SIGINT -> SIGTERM -> SIGKILL because GUI apps like the
    // NC2000 (文曲星) emulator commonly mask SIGINT.
    killpg(pid, SIGINT);
    auto start = std::chrono::steady_clock::now();
    bool term_sent = false;
    while (true) {
        int status;
        if (waitpid(pid, &status, WNOHANG) != 0) return;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
        if (!term_sent && elapsed >= grace_ms / 2) {
            killpg(pid, SIGTERM);
            term_sent = true;
        }
        if (elapsed >= grace_ms) {
            killpg(pid, SIGKILL);
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
        // New session/process group so hal_process_stop() can killpg()
        // the whole tree.
        setsid();
        execlp("/bin/sh", "sh", "-c", exec_path, (char *)NULL);
        _exit(127);
    }
    return (hal_pid_t)pid;
}

void hal_process_stop(hal_pid_t pid)
{
    if (pid <= 0) return;
    // pid is the pgid (child was spawned with setsid()); kill the
    // whole group so forked grandchildren go down too.
    killpg((pid_t)pid, SIGTERM);
    int status;
    waitpid((pid_t)pid, &status, WNOHANG);
}

void hal_system_shutdown(void)
{
    printf("[HAL] shutdown\n");
    system("sudo shutdown -h now");
}

void hal_system_reboot(void)
{
    printf("[HAL] reboot\n");
    system("sudo reboot");
}
