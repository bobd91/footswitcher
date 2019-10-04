/* Work in progress */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <glob.h>
#include <string.h>
#include <errno.h>
#include <sys/epoll.h>

#include <linux/input.h>

#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#define MAX_EVENTS 5
#define UP 0
#define DOWN 1

void err_exit(int, char *);
void process_events();
int process_dev(struct libevdev *dev, void (* fn)(struct input_event *));
void switch_fn(struct input_event *ev);
void keyboard_fn(struct input_event *ev);
int add_event(int fd, struct libevdev *dev);
int create_uinput();
int send_keys(int, int, int);
int send_key(int, int);

char *KEY_STATE[] = { "up", "down" };
struct libevdev *dev = NULL;
struct libevdev *switch_dev = NULL;
int switch_id_vendor = 0x07b4;
int switch_id_product = 0x0218;
int epoll_fd;
struct epoll_event events[MAX_EVENTS];
int next_event;
int key_pressed;
struct libevdev_uinput *uidev;

int main(int argc, char **argv) {
    epoll_fd = epoll_create(1);
    printf("epoll_fd=%d\n", epoll_fd);
    glob_t inputs;
    int rc = glob("/dev/input/event*", GLOB_ERR, NULL, &inputs);
    if(!rc) {
        char **paths = inputs.gl_pathv;
        char *path;
        while((path = *paths++) && next_event <= MAX_EVENTS) {
            int fd = open(path, O_RDONLY|O_NONBLOCK);
            printf("%s fd=%d\n", path, fd);
            if(fd) {
                int rc;
                if(0 <= libevdev_new_from_fd(fd, &dev)) {
                    if(libevdev_get_id_vendor(dev) == switch_id_vendor
                            && libevdev_get_id_product(dev) == switch_id_product) {
                        rc = libevdev_grab(dev, LIBEVDEV_GRAB);
                        if(!rc) {
                            rc = add_event(fd, dev);
                            if(rc) {
                                perror("epoll_ctl failed");
                                exit(1);
                            }
                            next_event++;
                            switch_dev = dev;
                        } else {
                            err_exit(rc, "Failed to grab footswitch");
                        }
                    } else if(libevdev_has_event_code(dev, EV_KEY, KEY_A)) {
                        rc = add_event(fd, dev);
                        if(rc) {
                            perror("epoll_ctl failed");
                            exit(1);
                        }
                        next_event++;
                    } else {
                        libevdev_free(dev);
                        close(fd);
                        printf("close fd=%d\n", fd);
                    }
                } else {
                    close(fd);
                    printf("close fd=%d\n", fd);
                }
            }
        }
        globfree(&inputs);

        if(NULL == switch_dev) {
            fprintf(stderr, "Foot switch not found\n");
            exit(2);
        }

        if(next_event > MAX_EVENTS) {
            fprintf(stderr, "Too many keyboards\n");
            exit(3);
        }

        if(rc = create_uinput()) {
            err_exit(rc, "Failed to create uinput");
        }

        printf("Uinput device node=%s\n", libevdev_uinput_get_devnode(uidev));

        process_events();
        perror("epoll_wait failed");
        exit(1);
    }
}

int create_uinput() {
    int rc;
    dev = libevdev_new();
    libevdev_set_name(dev, "footswitcher");
    libevdev_enable_event_type(dev, EV_KEY);
    libevdev_enable_event_code(dev, EV_KEY, KEY_LEFTCTRL, NULL);
    libevdev_enable_event_code(dev, EV_KEY, KEY_LEFTSHIFT, NULL);
    libevdev_enable_event_code(dev, EV_KEY, KEY_LEFTALT, NULL);
    libevdev_enable_event_code(dev, EV_KEY, KEY_ESC, NULL);
    libevdev_enable_event_code(dev, EV_KEY, KEY_TAB, NULL);
    libevdev_enable_event_code(dev, EV_KEY, BTN_MIDDLE, NULL);
    return libevdev_uinput_create_from_device(dev,
        LIBEVDEV_UINPUT_OPEN_MANAGED,
        &uidev);
}

void process_events() {
    int nfds;
    int rc = 0;;
    while(!rc) {
        nfds = epoll_wait(epoll_fd, events, next_event, -1);
        if(nfds == -1) {
            return;
        }
        for(int i = 0 ; i < nfds ; i++) {
            struct libevdev *dev = events[i].data.ptr;
            if(dev == switch_dev) {
                rc = process_dev(dev, switch_fn);
            } else {
                rc = process_dev(dev, keyboard_fn);
            }
            if(rc) break;
        }
    }
    err_exit(rc, "Failed to process device");
}

int process_dev(struct libevdev *dev, void (* fn)(struct input_event *)) {
    int rc;
    do {
        struct input_event ev;
        rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL|LIBEVDEV_READ_FLAG_BLOCKING, &ev);
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            // Resync
            while (rc == LIBEVDEV_READ_STATUS_SYNC) {
                (* fn)(&ev);
                rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
            }
            // Resynched
        } else if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
            (* fn)(&ev);
        }
    } while (rc == LIBEVDEV_READ_STATUS_SYNC || rc == LIBEVDEV_READ_STATUS_SUCCESS);

    return rc == -EAGAIN ? 0 : rc;
}

void switch_fn(struct input_event *ev) {
    if(ev->type == EV_KEY) {
        switch(ev->code) {
        case BTN_LEFT:
            send_keys(KEY_LEFTCTRL, KEY_ESC, ev->value);
            break;
        case BTN_RIGHT:
            send_keys(KEY_LEFTALT, KEY_TAB, ev->value);
            break;
        case BTN_MIDDLE:
            send_keys(KEY_LEFTSHIFT, BTN_MIDDLE, ev->value);
            break;
        }
    }
}

int send_keys(int up_down_key, int after_up_key, int up_down) {
    send_key(up_down_key, up_down);
    if(UP == up_down && !key_pressed) {
        send_key(after_up_key, DOWN);
        send_key(after_up_key, UP);
    } else {
        key_pressed = 0;
    }
}

int send_key(int key, int value) {
    int rc = libevdev_uinput_write_event(uidev, EV_KEY, key, value);
    if(rc)  {
        err_exit(rc, "Failed to write uinput event");
    }
    rc = libevdev_uinput_write_event(uidev, EV_SYN, SYN_REPORT, 0);
    if(rc) {
        err_exit(rc, "Failed to write uinput sync event");
    }
}

void keyboard_fn(struct input_event *ev) {
    if(ev->type == EV_KEY) {
        key_pressed = 1;
    }
}


int add_event(int fd, struct libevdev *dev) {
    struct epoll_event ev;
    ev.data.ptr = dev;
    ev.events = EPOLLIN;
    int rc = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    return rc;
}

void err_exit(int rc, char *msg) {
    fprintf(stderr, "%s: %s\n", msg, strerror(-rc));
    exit(1);
}
