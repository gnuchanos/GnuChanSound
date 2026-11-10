#define _POSIX_C_SOURCE 200809L

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define APP_NAME "GnuChanSound"
#define MAX_DEVICES 128
#define DEVICE_NAME 256
#define COMMAND_OUTPUT 32768
#define VOLUME_MAX 150
#define WINDOW_DEFAULT_HEIGHT 700
#define WINDOW_MIN_WIDTH 680
#define WINDOW_MIN_HEIGHT 640
#define WPCTL_TIMEOUT_SECONDS 5
#define STATUS_TOP 564
#define STATUS_HEIGHT 54
#define REFRESH_TOP (STATUS_TOP + 8)
#define REFRESH_HEIGHT 38
#define FOOTER_MIN_HEIGHT 660

typedef struct {
    unsigned int id;
    char name[DEVICE_NAME];
    int is_default;
    int volume;
    int muted;
    int volume_known;
} AudioDevice;

typedef struct {
    AudioDevice output[MAX_DEVICES];
    AudioDevice input[MAX_DEVICES];
    size_t output_count;
    size_t input_count;
    size_t output_selected;
    size_t input_selected;
    int have_status;
    char message[512];
} AppState;

typedef struct {
    Display *display;
    int screen;
    Window window;
    Pixmap buffer;
    int buffer_width;
    int buffer_height;
    XftDraw *draw;
    XftFont *font;
    XftColor background;
    XftColor panel;
    XftColor panel_edge;
    XftColor text;
    XftColor muted_text;
    XftColor accent;
    XftColor track;
    XftColor green;
    XftColor red;
    GC gc;
    int width;
    int height;
    Atom wm_delete;
} Ui;

static AppState app;
static int language_turkish;
static volatile sig_atomic_t keep_running = 1;

static const char *tr(const char *english, const char *turkish) {
    return language_turkish ? turkish : english;
}

static void load_language(void) {
    char path[1024];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) {
        snprintf(path, sizeof(path), "%s/GnuChanSettings/system.conf", xdg);
    } else if (home && *home) {
        snprintf(path, sizeof(path), "%s/.config/GnuChanSettings/system.conf",
                 home);
    } else {
        return;
    }

    FILE *file = fopen(path, "r");
    if (!file) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *p = line;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '#' || strncmp(p, "ApplicationLanguage", 19) != 0) {
            continue;
        }
        p += 19;
        while (isspace((unsigned char)*p)) {
            p++;
        }
        if (*p++ != '=') {
            continue;
        }
        while (isspace((unsigned char)*p)) {
            p++;
        }
        char *end = p + strlen(p);
        while (end > p && isspace((unsigned char)end[-1])) {
            *--end = '\0';
        }
        if (*p == '"') {
            p++;
            end = strchr(p, '"');
            if (end) {
                *end = '\0';
            }
        }
        language_turkish = strcmp(p, "Turkish") == 0;
        break;
    }
    fclose(file);
}

static void set_message(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(app.message, sizeof(app.message), format, args);
    va_end(args);
}

/* Spawn only fixed executable names and argument arrays; never invoke a shell. */
static int timeout_remaining_ms(const struct timespec *deadline) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    long long nanoseconds =
        ((long long)deadline->tv_sec - now.tv_sec) * 1000000000LL +
        deadline->tv_nsec - now.tv_nsec;
    if (nanoseconds <= 0) {
        return 0;
    }
    long long milliseconds = (nanoseconds + 999999LL) / 1000000LL;
    return milliseconds > INT_MAX ? INT_MAX : (int)milliseconds;
}

static void stop_child(pid_t pid) {
    if (kill(-pid, SIGKILL) != 0) {
        (void)kill(pid, SIGKILL);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
    }
}

static int wait_for_child(pid_t pid, int *status,
                          const struct timespec *deadline) {
    for (;;) {
        pid_t result = waitpid(pid, status, WNOHANG);
        if (result == pid) {
            return 1;
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        int remaining = timeout_remaining_ms(deadline);
        if (remaining == 0) {
            return 0;
        }
        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = remaining < 10 ? remaining * 1000000L : 10000000L
        };
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        }
    }
}

static int run_wpctl(char *const argv[], char *output, size_t output_size) {
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
        snprintf(output, output_size, "clock_gettime: %s", strerror(errno));
        return -1;
    }
    deadline.tv_sec += WPCTL_TIMEOUT_SECONDS;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        snprintf(output, output_size, "pipe: %s", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        snprintf(output, output_size, "fork: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        (void)setpgid(0, 0);
        close(pipefd[0]);
        (void)dup2(pipefd[1], STDOUT_FILENO);
        (void)dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], argv);
        dprintf(STDERR_FILENO, "%s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    close(pipefd[1]);
    size_t used = 0;
    char chunk[1024];
    int timed_out = 0;
    int read_error = 0;
    for (;;) {
        int remaining = timeout_remaining_ms(&deadline);
        if (remaining == 0) {
            timed_out = 1;
            break;
        }
        struct pollfd descriptor = {
            .fd = pipefd[0],
            .events = POLLIN | POLLHUP
        };
        int ready = poll(&descriptor, 1, remaining);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready == 0) {
            timed_out = 1;
            break;
        }
        if (ready < 0) {
            read_error = errno;
            break;
        }

        ssize_t got = read(pipefd[0], chunk, sizeof(chunk));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            read_error = errno;
            break;
        }
        if (got == 0) {
            break;
        }
        size_t available = output_size > used + 1 ? output_size - used - 1 : 0;
        size_t copy = (size_t)got < available ? (size_t)got : available;
        if (copy) {
            memcpy(output + used, chunk, copy);
            used += copy;
        }
    }
    close(pipefd[0]);
    output[used] = '\0';

    int status = 0;
    if (timed_out) {
        stop_child(pid);
        snprintf(output, output_size, "%s timed out after %d seconds",
                 argv[0], WPCTL_TIMEOUT_SECONDS);
        return 124;
    }
    if (read_error != 0) {
        stop_child(pid);
        snprintf(output, output_size, "read: %s", strerror(read_error));
        return -1;
    }
    int waited = wait_for_child(pid, &status, &deadline);
    if (waited == 0) {
        stop_child(pid);
        snprintf(output, output_size, "%s timed out after %d seconds",
                 argv[0], WPCTL_TIMEOUT_SECONDS);
        return 124;
    }
    if (waited < 0) {
        snprintf(output, output_size, "waitpid: %s", strerror(errno));
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static void trim_name(char *name) {
    char *volume = strstr(name, " [vol:");
    if (volume) {
        *volume = '\0';
    }
    size_t len = strlen(name);
    while (len && isspace((unsigned char)name[len - 1])) {
        name[--len] = '\0';
    }
    char *start = name;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    if (start != name) {
        memmove(name, start, strlen(start) + 1);
    }
}

static int parse_device_line(char *line, AudioDevice *device) {
    char *number = line;
    while (*number && !isdigit((unsigned char)*number)) {
        number++;
    }
    if (!*number) {
        return 0;
    }
    errno = 0;
    char *after = NULL;
    unsigned long id = strtoul(number, &after, 10);
    if (errno || after == number || *after != '.' || after[1] != ' ' ||
        id > 0x7fffffffUL) {
        return 0;
    }
    char *name = after + 2;
    trim_name(name);
    if (!*name) {
        return 0;
    }
    device->id = (unsigned int)id;
    device->is_default = 0;
    device->volume = 0;
    device->muted = 0;
    device->volume_known = 0;
    snprintf(device->name, sizeof(device->name), "%s", name);
    for (char *p = line; p < number; p++) {
        if (*p == '*') {
            device->is_default = 1;
            break;
        }
    }
    return 1;
}

static void parse_device_text(const char *text, AppState *state) {
    state->output_count = 0;
    state->input_count = 0;
    int section = 0;
    const char *cursor = text;
    while (*cursor) {
        char line[1024];
        size_t length = 0;
        while (cursor[length] && cursor[length] != '\n' &&
               length < sizeof(line) - 1) {
            length++;
        }
        memcpy(line, cursor, length);
        line[length] = '\0';
        cursor += length;
        if (*cursor == '\n') {
            cursor++;
        }

        if (strstr(line, "Sinks:")) {
            section = 1;
            continue;
        }
        if (strstr(line, "Sources:")) {
            section = 2;
            continue;
        }
        if (strstr(line, "endpoints:") || strstr(line, "Filters:") ||
            strstr(line, "Streams:") || strstr(line, "Devices:")) {
            section = 0;
            continue;
        }
        if (!section) {
            continue;
        }

        AudioDevice parsed;
        if (!parse_device_line(line, &parsed)) {
            continue;
        }
        AudioDevice *list = section == 1 ? state->output : state->input;
        size_t *count = section == 1 ? &state->output_count :
                                      &state->input_count;
        if (*count < MAX_DEVICES) {
            list[(*count)++] = parsed;
        }
    }
}

static size_t find_id(const AudioDevice *devices, size_t count,
                      unsigned int id) {
    for (size_t i = 0; i < count; i++) {
        if (devices[i].id == id) {
            return i;
        }
    }
    return count;
}

static int parse_volume_output(const char *output, int *percent, int *muted) {
    char *volume = strstr(output, "Volume:");
    if (!volume) {
        return 0;
    }
    volume += strlen("Volume:");
    while (isspace((unsigned char)*volume)) {
        volume++;
    }
    errno = 0;
    char *end = NULL;
    double value = strtod(volume, &end);
    if (end == volume || errno == ERANGE || !isfinite(value)) {
        return 0;
    }
    if (value <= 0.0) {
        *percent = 0;
    } else if (value >= (double)VOLUME_MAX / 100.0) {
        *percent = VOLUME_MAX;
    } else {
        *percent = (int)(value * 100.0 + 0.5);
    }
    *muted = strstr(end, "MUTED") != NULL;
    return 1;
}

static void read_device_volume(AudioDevice *device) {
    char id[24];
    snprintf(id, sizeof(id), "%u", device->id);
    char *argv[] = { "wpctl", "get-volume", id, NULL };
    char output[1024];
    int result = run_wpctl(argv, output, sizeof(output));
    int percent = 0;
    int muted = 0;
    if (result != 0 || !parse_volume_output(output, &percent, &muted)) {
        device->volume_known = 0;
        return;
    }
    device->volume = percent;
    device->muted = muted;
    device->volume_known = 1;
}

static void refresh_devices(void) {
    char *argv[] = { "wpctl", "status", NULL };
    char output[COMMAND_OUTPUT];
    int result = run_wpctl(argv, output, sizeof(output));
    if (result != 0) {
        app.have_status = 0;
        app.output_count = 0;
        app.input_count = 0;
        app.output_selected = 0;
        app.input_selected = 0;
        if (result == 127 || strstr(output, "No such file or directory")) {
            set_message("%s",
                        tr("wpctl is not installed. Install PipeWire and "
                           "WirePlumber (wpctl, usually pipewire-bin).",
                           "wpctl kurulu değil. PipeWire ve WirePlumber'ı "
                           "kurun (wpctl, genellikle pipewire-bin)."));
        } else {
            set_message("%s: %s",
                        tr("Audio backend failed", "Ses arka ucu başarısız"),
                        output[0] ? output :
                        tr("wpctl status returned an error.",
                           "wpctl status hata döndürdü."));
        }
        return;
    }

    unsigned int previous_output = 0;
    unsigned int previous_input = 0;
    if (app.output_count && app.output_selected < app.output_count) {
        previous_output = app.output[app.output_selected].id;
    }
    if (app.input_count && app.input_selected < app.input_count) {
        previous_input = app.input[app.input_selected].id;
    }
    parse_device_text(output, &app);
    app.output_selected = find_id(app.output, app.output_count, previous_output);
    app.input_selected = find_id(app.input, app.input_count, previous_input);
    if (app.output_selected == app.output_count) {
        for (size_t i = 0; i < app.output_count; i++) {
            if (app.output[i].is_default) {
                app.output_selected = i;
                break;
            }
        }
        if (app.output_selected == app.output_count && app.output_count) {
            app.output_selected = 0;
        }
    }
    if (app.input_selected == app.input_count) {
        for (size_t i = 0; i < app.input_count; i++) {
            if (app.input[i].is_default) {
                app.input_selected = i;
                break;
            }
        }
        if (app.input_selected == app.input_count && app.input_count) {
            app.input_selected = 0;
        }
    }

    app.have_status = 1;
    if (app.output_count) {
        read_device_volume(&app.output[app.output_selected]);
    }
    if (app.input_count) {
        read_device_volume(&app.input[app.input_selected]);
    }
    if (!app.output_count && !app.input_count) {
        set_message("%s", tr("PipeWire is running, but no devices were found.",
                             "PipeWire çalışıyor ancak aygıt bulunamadı."));
    } else if (!app.output_count) {
        set_message("%s", tr("No output devices found.",
                             "Çıkış aygıtı bulunamadı."));
    } else if (!app.input_count) {
        set_message("%s", tr("No input devices found.",
                             "Giriş aygıtı bulunamadı."));
    } else {
        set_message("%s", tr("Audio devices refreshed.",
                             "Ses aygıtları yenilendi."));
    }
}

static AudioDevice *selected_device(int input) {
    if (input) {
        return app.input_count && app.input_selected < app.input_count ?
               &app.input[app.input_selected] : NULL;
    }
    return app.output_count && app.output_selected < app.output_count ?
           &app.output[app.output_selected] : NULL;
}

static void set_default_device(int input) {
    AudioDevice *device = selected_device(input);
    if (!device) {
        set_message("%s", tr("No device is selected.", "Aygıt seçilmedi."));
        return;
    }
    char id[24];
    snprintf(id, sizeof(id), "%u", device->id);
    char *argv[] = { "wpctl", "set-default", id, NULL };
    char output[2048];
    int result = run_wpctl(argv, output, sizeof(output));
    if (result != 0) {
        set_message("%s: %s",
                    tr("Could not set the default device",
                       "Varsayılan aygıt ayarlanamadı"),
                    output[0] ? output : "wpctl set-default failed");
    } else {
        set_message("%s: %s",
                    tr("Default device changed", "Varsayılan aygıt değişti"),
                    device->name);
        refresh_devices();
    }
}

static void set_device_volume(AudioDevice *device, int percent) {
    if (!device) {
        set_message("%s", tr("No device is selected.", "Aygıt seçilmedi."));
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > VOLUME_MAX) {
        percent = VOLUME_MAX;
    }
    if (device->volume_known && device->volume == percent) {
        return;
    }
    char id[24];
    char amount[24];
    snprintf(id, sizeof(id), "%u", device->id);
    snprintf(amount, sizeof(amount), "%d%%", percent);
    char *argv[] = { "wpctl", "set-volume", id, amount, NULL };
    char output[2048];
    if (run_wpctl(argv, output, sizeof(output)) != 0) {
        set_message("%s: %s",
                    tr("Could not change volume", "Ses düzeyi değiştirilemedi"),
                    output[0] ? output : "wpctl set-volume failed");
    } else {
        device->volume = percent;
        device->volume_known = 1;
        set_message("%s: %d%%",
                    tr("Volume set", "Ses düzeyi ayarlandı"), percent);
    }
}

static void toggle_mute(AudioDevice *device) {
    if (!device) {
        set_message("%s", tr("No device is selected.", "Aygıt seçilmedi."));
        return;
    }
    char id[24];
    snprintf(id, sizeof(id), "%u", device->id);
    char *argv[] = { "wpctl", "set-mute", id,
                     device->muted ? "0" : "1", NULL };
    char output[2048];
    if (run_wpctl(argv, output, sizeof(output)) != 0) {
        set_message("%s: %s",
                    tr("Could not change mute state", "Sessiz durumu değişmedi"),
                    output[0] ? output : "wpctl set-mute failed");
    } else {
        device->muted = !device->muted;
        set_message("%s", device->muted ?
                    tr("Device muted.", "Aygıt sessize alındı.") :
                    tr("Device unmuted.", "Aygıtın sesi açıldı."));
    }
}

static void color(Ui *ui, XftColor *value, const char *name) {
    (void)XftColorAllocName(ui->display,
                            DefaultVisual(ui->display, ui->screen),
                            DefaultColormap(ui->display, ui->screen),
                            name, value);
}

static void fill_rect(Ui *ui, XftColor *fill, int x, int y, int width,
                      int height) {
    XSetForeground(ui->display, ui->gc, fill->pixel);
    XFillRectangle(ui->display, ui->buffer, ui->gc, x, y,
                   (unsigned int)width, (unsigned int)height);
}

static void draw_text(Ui *ui, XftColor *ink, int x, int baseline,
                      const char *text) {
    XftDrawStringUtf8(ui->draw, ink, ui->font, x, baseline,
                      (const FcChar8 *)text, (int)strlen(text));
}

static int text_pixel_width(Ui *ui, const char *text, size_t length) {
    XGlyphInfo extents;
    XftTextExtentsUtf8(ui->display, ui->font, (const FcChar8 *)text,
                       (int)length, &extents);
    return (int)extents.xOff;
}

static void draw_text_fit(Ui *ui, XftColor *ink, int x, int baseline,
                          int max_width, const char *text) {
    if (max_width <= 0) {
        return;
    }
    size_t length = strlen(text);
    if (text_pixel_width(ui, text, length) <= max_width) {
        draw_text(ui, ink, x, baseline, text);
        return;
    }

    static const char ellipsis[] = "\xe2\x80\xa6";
    int ellipsis_width = text_pixel_width(ui, ellipsis, sizeof(ellipsis) - 1);
    if (ellipsis_width > max_width) {
        return;
    }

    char *fitted = malloc(length + sizeof(ellipsis));
    if (!fitted) {
        return;
    }
    size_t fitted_length = length;
    do {
        if (fitted_length > 0) {
            fitted_length--;
            while (fitted_length > 0 &&
                   ((unsigned char)text[fitted_length] & 0xc0) == 0x80) {
                fitted_length--;
            }
        }
        memcpy(fitted, text, fitted_length);
        memcpy(fitted + fitted_length, ellipsis, sizeof(ellipsis));
    } while (fitted_length > 0 &&
             text_pixel_width(ui, fitted, fitted_length + sizeof(ellipsis) - 1)
                 > max_width);

    draw_text(ui, ink, x, baseline, fitted);
    free(fitted);
}

static int device_name_max_width(int window_width) {
    const int device_name_x = 36 + 70;
    const int next_button_x = window_width - 292;
    return next_button_x - device_name_x - 12;
}

static int status_message_max_width(int window_width) {
    const int message_x = 52;
    const int refresh_button_x = window_width - 174;
    return refresh_button_x - message_x - 12;
}

static int footer_visible(int window_height) {
    return window_height >= FOOTER_MIN_HEIGHT;
}

static void draw_button(Ui *ui, int x, int y, int width, int height,
                        const char *label, int active) {
    fill_rect(ui, active ? &ui->accent : &ui->panel_edge,
              x, y, width, height);
    int label_width = (int)strlen(label) * 7;
    int text_x = x + (width - label_width) / 2;
    if (text_x < x + 7) {
        text_x = x + 7;
    }
    draw_text(ui, &ui->text, text_x, y + height / 2 + 5, label);
}

static int resize_buffer(Ui *ui) {
    if (ui->buffer && ui->buffer_width == ui->width &&
        ui->buffer_height == ui->height) {
        return 1;
    }

    Pixmap buffer = XCreatePixmap(ui->display, ui->window,
                                  (unsigned int)ui->width,
                                  (unsigned int)ui->height,
                                  (unsigned int)DefaultDepth(ui->display,
                                                             ui->screen));
    if (!buffer) {
        return 0;
    }
    XftDraw *draw = XftDrawCreate(ui->display, buffer,
                                  DefaultVisual(ui->display, ui->screen),
                                  DefaultColormap(ui->display, ui->screen));
    if (!draw) {
        XFreePixmap(ui->display, buffer);
        return 0;
    }

    if (ui->draw) {
        XftDrawDestroy(ui->draw);
    }
    if (ui->buffer) {
        XFreePixmap(ui->display, ui->buffer);
    }
    ui->buffer = buffer;
    ui->buffer_width = ui->width;
    ui->buffer_height = ui->height;
    ui->draw = draw;
    return 1;
}

static void copy_to_window(Ui *ui, int x, int y, unsigned int width,
                           unsigned int height) {
    if (!ui->buffer || width == 0 || height == 0) {
        return;
    }
    XCopyArea(ui->display, ui->buffer, ui->window, ui->gc, x, y, width,
              height, x, y);
}

static void draw_panel(Ui *ui, int top, const char *title, int input) {
    int x = 36;
    int width = ui->width - 72;
    fill_rect(ui, &ui->panel, x, top, width, 230);
    XSetForeground(ui->display, ui->gc, ui->panel_edge.pixel);
    XDrawRectangle(ui->display, ui->buffer, ui->gc, x, top,
                   (unsigned int)width, 229);
    draw_text(ui, &ui->accent, x + 20, top + 30, title);

    AudioDevice *device = selected_device(input);
    char name[DEVICE_NAME + 48];
    if (device) {
        snprintf(name, sizeof(name), "%s%s", device->name,
                 device->is_default ?
                 tr("  (default)", "  (varsayılan)") : "");
    } else {
        snprintf(name, sizeof(name), "%s",
                 app.have_status ?
                 tr("No device found", "Aygıt bulunamadı") :
                 tr("Waiting for audio backend", "Ses arka ucu bekleniyor"));
    }
    int row_y = top + 48;
    draw_button(ui, x + 16, row_y, 40, 40, "<", 0);
    draw_text_fit(ui, &ui->text, x + 70, row_y + 25,
                  device_name_max_width(ui->width), name);
    draw_button(ui, ui->width - 292, row_y, 40, 40, ">", 0);
    draw_button(ui, ui->width - 238, row_y, 202, 40,
                tr("Set as default", "Varsayılan yap"),
                device && device->is_default);

    char volume_label[80];
    if (device && device->volume_known) {
        snprintf(volume_label, sizeof(volume_label), "%s %d%%",
                 tr("Volume", "Ses"), device->volume);
    } else {
        snprintf(volume_label, sizeof(volume_label), "%s",
                 tr("Volume unavailable", "Ses düzeyi alınamadı"));
    }
    draw_text(ui, &ui->muted_text, x + 20, top + 117, volume_label);

    const int slider_left = x + 22;
    const int slider_width = width - 44;
    const int slider_y = top + 138;
    fill_rect(ui, &ui->track, slider_left, slider_y, slider_width, 8);
    if (device && device->volume_known && slider_width > 0) {
        int filled = slider_width * device->volume / VOLUME_MAX;
        if (filled > 0) {
            fill_rect(ui, &ui->accent, slider_left, slider_y, filled, 8);
        }
        int knob_x = slider_left + filled;
        fill_rect(ui, &ui->text, knob_x - 4, slider_y - 4, 8, 16);
    }

    draw_button(ui, x + 16, top + 164, 170, 42,
                device && device->muted ?
                tr("Unmute", "Sesi aç") : tr("Mute", "Sessize al"),
                device && device->muted);
    draw_text(ui, &ui->muted_text, x + 202, top + 190,
              tr("Click the bar to set volume (0–150%).",
                 "Ses düzeyi için çubuğa tıklayın (%0–150)."));
}

static void draw_ui(Ui *ui) {
    if (!resize_buffer(ui)) {
        return;
    }
    fill_rect(ui, &ui->background, 0, 0, ui->width, ui->height);
    draw_text(ui, &ui->text, 38, 42, APP_NAME);
    draw_text(ui, &ui->muted_text, 184, 42,
              tr("PipeWire audio control", "PipeWire ses denetimi"));
    draw_panel(ui, 62,
               tr("OUTPUT · speakers/headphones",
                  "ÇIKIŞ · hoparlör/kulaklık"), 0);
    draw_panel(ui, 308,
               tr("INPUT · microphones",
                  "GİRİŞ · mikrofonlar"), 1);

    fill_rect(ui, &ui->panel, 36, STATUS_TOP, ui->width - 72,
              STATUS_HEIGHT);
    draw_text(ui, &ui->muted_text, 52, STATUS_TOP + 22,
              tr("Status", "Durum"));
    draw_text_fit(ui, app.have_status ? &ui->text : &ui->red,
                  52, STATUS_TOP + 42, status_message_max_width(ui->width),
                  app.message[0] ? app.message :
                  tr("Connecting to PipeWire…",
                     "PipeWire'a bağlanılıyor…"));
    draw_button(ui, ui->width - 174, REFRESH_TOP, 138, REFRESH_HEIGHT,
                tr("Refresh", "Yenile"), 0);
    if (footer_visible(ui->height)) {
        draw_text_fit(ui, &ui->muted_text, 38, ui->height - 22,
                      ui->width - 76,
                      tr("←/→ device • +/- volume • M mute • Tab switch I/O • R refresh • Q quit",
                         "←/→ aygıt • +/- ses • M sessiz • Tab giriş/çıkış • R yenile • Q çık"));
    }
    copy_to_window(ui, 0, 0, (unsigned int)ui->width,
                   (unsigned int)ui->height);
    XFlush(ui->display);
}

static int parse_self_test(void) {
    const char fixture[] =
        "Audio\n"
        " ├─ Sinks:\n"
        " │  *   52. Built-in Output [vol: 0.75]\n"
        " │      53. HDMI Output [vol: 1.00]\n"
        " ├─ Sink endpoints:\n"
        " ├─ Sources:\n"
        " │      60. Webcam Microphone [vol: 0.80]\n"
        " │  *   61. Built-in Microphone [vol: 0.55]\n"
        " ├─ Source endpoints:\n";
    AppState parsed = {0};
    parse_device_text(fixture, &parsed);
    if (parsed.output_count != 2 || parsed.input_count != 2 ||
        parsed.output[0].id != 52 || !parsed.output[0].is_default ||
        strcmp(parsed.output[1].name, "HDMI Output") != 0 ||
        parsed.input[1].id != 61 || !parsed.input[1].is_default) {
        fprintf(stderr, "wpctl status parser self-test failed\n");
        return 1;
    }
    int percent = -1;
    int muted = 0;
    if (!parse_volume_output("Volume: 1e300", &percent, &muted) ||
        percent != VOLUME_MAX ||
        !parse_volume_output("Volume: 0.75 MUTED", &percent, &muted) ||
        percent != 75 || !muted ||
        !parse_volume_output("Volume: -1", &percent, &muted) ||
        percent != 0) {
        fprintf(stderr, "wpctl volume parser self-test failed\n");
        return 1;
    }
    if (device_name_max_width(WINDOW_MIN_WIDTH) <= 0 ||
        status_message_max_width(WINDOW_MIN_WIDTH) <= 0 ||
        REFRESH_TOP + REFRESH_HEIGHT > STATUS_TOP + STATUS_HEIGHT ||
        footer_visible(WINDOW_MIN_HEIGHT) ||
        !footer_visible(WINDOW_DEFAULT_HEIGHT)) {
        fprintf(stderr, "minimum-window layout self-test failed\n");
        return 1;
    }
    puts("wpctl status parser self-test passed");
    puts("minimum-window layout self-test passed");
    return 0;
}

static void move_selection(int input, int delta) {
    size_t count = input ? app.input_count : app.output_count;
    size_t *selected = input ? &app.input_selected : &app.output_selected;
    if (!count) {
        return;
    }
    if (delta < 0) {
        *selected = *selected == 0 ? count - 1 : *selected - 1;
    } else {
        *selected = (*selected + 1) % count;
    }
    AudioDevice *device = selected_device(input);
    if (device) {
        read_device_volume(device);
        set_message("%s: %s",
                    tr("Selected", "Seçildi"), device->name);
    }
}

static int slider_percent(int x, int width, int pointer_x) {
    int track_left = 58;
    int track_width = width - 116;
    (void)x;
    if (track_width <= 0) {
        return 0;
    }
    int relative = pointer_x - track_left;
    if (relative < 0) {
        relative = 0;
    }
    if (relative > track_width) {
        relative = track_width;
    }
    return relative * VOLUME_MAX / track_width;
}

static int init_ui(Ui *ui) {
    memset(ui, 0, sizeof(*ui));
    ui->display = XOpenDisplay(NULL);
    if (!ui->display) {
        fprintf(stderr, "%s\n",
                tr("Cannot open the X display.", "X ekranı açılamıyor."));
        return 0;
    }
    ui->screen = DefaultScreen(ui->display);
    ui->width = 820;
    ui->height = WINDOW_DEFAULT_HEIGHT;
    ui->window = XCreateSimpleWindow(
        ui->display, RootWindow(ui->display, ui->screen), 80, 80,
        (unsigned int)ui->width, (unsigned int)ui->height, 0,
        BlackPixel(ui->display, ui->screen),
        WhitePixel(ui->display, ui->screen));
    XStoreName(ui->display, ui->window, APP_NAME);
    XClassHint class_hint = { .res_name = "gnuchansound",
                              .res_class = "GnuChanSound" };
    XSetClassHint(ui->display, ui->window, &class_hint);
    XSizeHints hints = {0};
    hints.flags = PMinSize;
    hints.min_width = WINDOW_MIN_WIDTH;
    hints.min_height = WINDOW_MIN_HEIGHT;
    XSetWMNormalHints(ui->display, ui->window, &hints);
    ui->wm_delete = XInternAtom(ui->display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(ui->display, ui->window, &ui->wm_delete, 1);
    XSelectInput(ui->display, ui->window,
                 ExposureMask | StructureNotifyMask | ButtonPressMask |
                 ButtonReleaseMask | PointerMotionMask | KeyPressMask);

    ui->font = XftFontOpenName(ui->display, ui->screen, "sans-12");
    if (!ui->font) {
        fprintf(stderr, "%s\n",
                tr("Cannot open the sans-12 font.", "sans-12 yazı tipi açılamıyor."));
        XCloseDisplay(ui->display);
        return 0;
    }
    ui->gc = XCreateGC(ui->display, ui->window, 0, NULL);
    color(ui, &ui->background, "#17131f");
    color(ui, &ui->panel, "#211b2b");
    color(ui, &ui->panel_edge, "#40334f");
    color(ui, &ui->text, "#f3eaff");
    color(ui, &ui->muted_text, "#b8a9c9");
    color(ui, &ui->accent, "#a855f7");
    color(ui, &ui->track, "#51445e");
    color(ui, &ui->green, "#53d769");
    color(ui, &ui->red, "#ff6b6b");
    XSetWindowBackground(ui->display, ui->window, ui->background.pixel);
    XMapWindow(ui->display, ui->window);
    return 1;
}

static void cleanup_ui(Ui *ui) {
    if (!ui->display) {
        return;
    }
    if (ui->draw) {
        XftDrawDestroy(ui->draw);
    }
    if (ui->buffer) {
        XFreePixmap(ui->display, ui->buffer);
    }
    if (ui->font) {
        XftFontClose(ui->display, ui->font);
    }
    XFreeGC(ui->display, ui->gc);
    XftColorFree(ui->display, DefaultVisual(ui->display, ui->screen),
                 DefaultColormap(ui->display, ui->screen), &ui->background);
    XftColorFree(ui->display, DefaultVisual(ui->display, ui->screen),
                 DefaultColormap(ui->display, ui->screen), &ui->panel);
    XftColorFree(ui->display, DefaultVisual(ui->display, ui->screen),
                 DefaultColormap(ui->display, ui->screen), &ui->panel_edge);
    XftColorFree(ui->display, DefaultVisual(ui->display, ui->screen),
                 DefaultColormap(ui->display, ui->screen), &ui->text);
    XftColorFree(ui->display, DefaultVisual(ui->display, ui->screen),
                 DefaultColormap(ui->display, ui->screen), &ui->muted_text);
    XftColorFree(ui->display, DefaultVisual(ui->display, ui->screen),
                 DefaultColormap(ui->display, ui->screen), &ui->accent);
    XftColorFree(ui->display, DefaultVisual(ui->display, ui->screen),
                 DefaultColormap(ui->display, ui->screen), &ui->track);
    XftColorFree(ui->display, DefaultVisual(ui->display, ui->screen),
                 DefaultColormap(ui->display, ui->screen), &ui->green);
    XftColorFree(ui->display, DefaultVisual(ui->display, ui->screen),
                 DefaultColormap(ui->display, ui->screen), &ui->red);
    XCloseDisplay(ui->display);
}

static void signal_stop(int signal_number) {
    (void)signal_number;
    keep_running = 0;
}

int main(int argc, char **argv) {
    load_language();
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        puts("GnuChanSound " GNUCHAN_SOUND_VERSION);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--self-test") == 0) {
        return parse_self_test();
    }
    if (argc > 1 && strcmp(argv[1], "--check-backend") == 0) {
        refresh_devices();
        puts(app.message);
        return app.have_status ? 0 : 1;
    }
    if (argc > 1) {
        fprintf(stderr, "%s\n", tr("Usage: GnuChanSound [--version|--self-test|--check-backend]",
                                   "Kullanım: GnuChanSound [--version|--self-test|--check-backend]"));
        return 2;
    }

    Ui ui;
    if (!init_ui(&ui)) {
        return 1;
    }
    signal(SIGINT, signal_stop);
    signal(SIGTERM, signal_stop);
    int active_input = 0;
    int dragging_slider = 0;
    int dragging_input = 0;
    refresh_devices();
    draw_ui(&ui);

    while (keep_running) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(ConnectionNumber(ui.display), &read_set);
        struct timeval timeout = { .tv_sec = 2, .tv_usec = 0 };
        int ready = select(ConnectionNumber(ui.display) + 1,
                           &read_set, NULL, NULL, &timeout);
        if (ready == 0) {
            refresh_devices();
            draw_ui(&ui);
            continue;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_message("%s: %s",
                        tr("X event wait failed", "X olay beklemesi başarısız"),
                        strerror(errno));
            break;
        }
        int resize_pending = 0;
        while (XPending(ui.display)) {
            XEvent event;
            XNextEvent(ui.display, &event);
            if (event.type == Expose) {
                copy_to_window(&ui, event.xexpose.x, event.xexpose.y,
                               (unsigned int)event.xexpose.width,
                               (unsigned int)event.xexpose.height);
                if (event.xexpose.count == 0) {
                    XFlush(ui.display);
                }
            } else if (event.type == ConfigureNotify) {
                if (ui.width != event.xconfigure.width ||
                    ui.height != event.xconfigure.height) {
                    ui.width = event.xconfigure.width;
                    ui.height = event.xconfigure.height;
                    resize_pending = 1;
                }
            } else if (event.type == ClientMessage &&
                       (Atom)event.xclient.data.l[0] == ui.wm_delete) {
                keep_running = 0;
            } else if (event.type == KeyPress) {
                KeySym key = XLookupKeysym(&event.xkey, 0);
                if (key == XK_q || key == XK_Q || key == XK_Escape) {
                    keep_running = 0;
                } else if (key == XK_Tab) {
                    active_input = !active_input;
                } else if (key == XK_r || key == XK_R) {
                    refresh_devices();
                } else if (key == XK_Left || key == XK_Right) {
                    move_selection(active_input, key == XK_Left ? -1 : 1);
                } else if (key == XK_m || key == XK_M) {
                    toggle_mute(selected_device(active_input));
                } else if (key == XK_plus || key == XK_equal ||
                           key == XK_KP_Add) {
                    AudioDevice *device = selected_device(active_input);
                    set_device_volume(device,
                                      device && device->volume_known ?
                                      device->volume + 5 : 5);
                } else if (key == XK_minus || key == XK_KP_Subtract) {
                    AudioDevice *device = selected_device(active_input);
                    set_device_volume(device,
                                      device && device->volume_known ?
                                      device->volume - 5 : 0);
                }
                draw_ui(&ui);
            } else if (event.type == ButtonPress) {
                int x = event.xbutton.x;
                int y = event.xbutton.y;
                int first_top = 62;
                int second_top = 308;
                int top = (y >= second_top) ? second_top : first_top;
                int input = top == second_top;
                int panel_left = 36;
                int panel_width = ui.width - 72;
                int slider_left = panel_left + 22;
                int slider_right = panel_left + panel_width - 22;
                active_input = input;

                if (y >= top + 48 && y <= top + 88) {
                    if (x <= panel_left + 56) {
                        move_selection(input, -1);
                    } else if (x >= ui.width - 292 &&
                               x <= ui.width - 252) {
                        move_selection(input, 1);
                    } else if (x >= ui.width - 238) {
                        set_default_device(input);
                    }
                } else if (y >= top + 132 && y <= top + 152 &&
                           x >= slider_left && x <= slider_right) {
                    dragging_slider = 1;
                    dragging_input = input;
                    set_device_volume(selected_device(input),
                                      slider_percent(0, ui.width, x));
                } else if (y >= top + 164 && y <= top + 206 &&
                           x >= panel_left + 16 && x <= panel_left + 186) {
                    toggle_mute(selected_device(input));
                } else if (y >= REFRESH_TOP &&
                           y <= REFRESH_TOP + REFRESH_HEIGHT &&
                           x >= ui.width - 174 && x <= ui.width - 36) {
                    refresh_devices();
                }
                draw_ui(&ui);
            } else if (event.type == MotionNotify && dragging_slider &&
                       (event.xmotion.state & Button1Mask)) {
                set_device_volume(selected_device(dragging_input),
                                  slider_percent(0, ui.width,
                                                 event.xmotion.x));
                draw_ui(&ui);
            } else if (event.type == ButtonRelease) {
                dragging_slider = 0;
            }
        }
        if (resize_pending) {
            draw_ui(&ui);
        }
    }

    cleanup_ui(&ui);
    return 0;
}
