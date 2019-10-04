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

#define MAX_EVENTS 5

void err_exit(int, char *);
void process_events();
int process_dev(struct libevdev *dev, void (* fn)(struct input_event *));
void switch_fn(struct input_event *ev);
void keyboard_fn(struct input_event *ev);
int add_event(int fd, struct libevdev *dev);

char *KEY_STATE[] = { "up", "down" };
struct libevdev *dev = NULL;
struct libevdev *switch_dev = NULL;
int switch_id_vendor = 0x07b4;
int switch_id_product = 0x0218;
int epoll_fd;
struct epoll_event events[MAX_EVENTS];
int next_event;
int key_pressed;

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

        process_events();
        perror("epoll_wait failed");
        exit(1);
    }
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
        printf("Next event rc=%d\n", rc);
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
            printf("Ctrl %s\n", KEY_STATE[ev->value]);
            if(ev->value == 0 && !key_pressed) {
                printf("Esc down\nEsc up\n");
            } else {
                key_pressed = 0;
            }
            break;
        case BTN_RIGHT:
            printf("Shift %s\n", KEY_STATE[ev->value]);
            if(ev->value == 0 && !key_pressed) {
                printf("Tab down\nTab up\n");
            } else {
                key_pressed = 0;
            }
            break;
        case BTN_MIDDLE:
            printf("Alt %s\n", KEY_STATE[ev->value]);
            if(ev->value == 0 && !key_pressed) {
                printf("Right mouse down\nRight mouse up\n");
            } else {
                key_pressed = 0;
            }
            break;
        }
    }
}

void keyboard_fn(struct input_event *ev) {
    if(ev->type == EV_KEY) {
        key_pressed = 1;
        printf("Keyboard event\n");
    }
}


int add_event(int fd, struct libevdev *dev) {
    struct epoll_event ev;
    ev.data.ptr = dev;
    ev.events = EPOLLIN;
    int rc = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);
    printf("epoll_ctl(%d, %d, %d, ..)=>%d\n", epoll_fd, EPOLL_CTL_ADD, fd, rc);
    return rc;
}

void err_exit(int rc, char *msg) {
    fprintf(stderr, "%s: %s\n", msg, strerror(-rc));
    exit(1);
}
