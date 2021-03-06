#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <xcb/xcb.h>
#include <xcb/xcb_event.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xinerama.h>
#include "types.h"
#include "settings.h"
#include "messages.h"
#include "rules.h"
#include "events.h"
#include "common.h"
#include "helpers.h"
#include "window.h"
#include "bspwm.h"
#include "tree.h"
#include "ewmh.h"

void quit(void)
{
    running = false;
}

void cleanup(void)
{
    while (mon_head != NULL)
        remove_monitor(mon_head);
    while (rule_head != NULL)
        remove_rule(rule_head);
    free(frozen_pointer);
}

void register_events(void)
{
    uint32_t values[] = {ROOT_EVENT_MASK};
    xcb_generic_error_t *e = xcb_request_check(dpy, xcb_change_window_attributes_checked(dpy, root, XCB_CW_EVENT_MASK, values));
    if (e != NULL) {
        xcb_disconnect(dpy);
        err("Another window manager is already running.\n");
    }
}

void setup(void)
{
    ewmh_init();
    screen = xcb_setup_roots_iterator(xcb_get_setup(dpy)).data;
    if (screen == NULL)
        err("Can't acquire the default screen.\n");
    root = screen->root;
    register_events();

    screen_width = screen->width_in_pixels;
    screen_height = screen->height_in_pixels;
    root_depth = screen->root_depth;

    uint32_t mask = XCB_CW_EVENT_MASK;
    uint32_t values[] = {XCB_EVENT_MASK_POINTER_MOTION};
    motion_recorder = xcb_generate_id(dpy);
    xcb_create_window(dpy, XCB_COPY_FROM_PARENT, motion_recorder, root, 0, 0, screen_width, screen_height, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, mask, values);

    xcb_atom_t net_atoms[] = {ewmh->_NET_SUPPORTED,
                              ewmh->_NET_DESKTOP_NAMES,
                              ewmh->_NET_NUMBER_OF_DESKTOPS,
                              ewmh->_NET_CURRENT_DESKTOP,
                              ewmh->_NET_CLIENT_LIST,
                              ewmh->_NET_ACTIVE_WINDOW,
                              ewmh->_NET_WM_DESKTOP,
                              ewmh->_NET_WM_STATE,
                              ewmh->_NET_WM_STATE_FULLSCREEN,
                              ewmh->_NET_WM_WINDOW_TYPE,
                              ewmh->_NET_WM_WINDOW_TYPE_DOCK,
                              ewmh->_NET_WM_WINDOW_TYPE_NOTIFICATION,
                              ewmh->_NET_WM_WINDOW_TYPE_DIALOG,
                              ewmh->_NET_WM_WINDOW_TYPE_UTILITY,
                              ewmh->_NET_WM_WINDOW_TYPE_TOOLBAR};

    xcb_ewmh_set_supported(ewmh, default_screen, LENGTH(net_atoms), net_atoms);

    xcb_intern_atom_reply_t *iar = xcb_intern_atom_reply(dpy, xcb_intern_atom(dpy, 0, strlen("_COMPTON_SHADOW"), "_COMPTON_SHADOW"), NULL);

    if (iar != NULL) {
        compton_shadow = iar->atom;
        free(iar);
    }

    monitor_uid = desktop_uid = client_uid = rule_uid = 0;
    mon = last_mon = mon_head = mon_tail = NULL;

    bool xinerama_is_active = false;

    if (xcb_get_extension_data(dpy, &xcb_xinerama_id)->present) {
        xcb_xinerama_is_active_reply_t *xia = xcb_xinerama_is_active_reply(dpy, xcb_xinerama_is_active(dpy), NULL);
        if (xia != NULL) {
            xinerama_is_active = xia->state;
            free(xia);
        }
    }

    if (xinerama_is_active) {
        xcb_xinerama_query_screens_reply_t *xsq = xcb_xinerama_query_screens_reply(dpy, xcb_xinerama_query_screens(dpy), NULL);
        xcb_xinerama_screen_info_t *xsi = xcb_xinerama_query_screens_screen_info(xsq);
        int n = xcb_xinerama_query_screens_screen_info_length(xsq);
        for (int i = 0; i < n; i++) {
            xcb_xinerama_screen_info_t info = xsi[i];
            xcb_rectangle_t rect = (xcb_rectangle_t) {info.x_org, info.y_org, info.width, info.height};
            add_monitor(&rect);
        }
        free(xsq);
    } else {
        warn("Xinerama is inactive.");
        xcb_rectangle_t rect = (xcb_rectangle_t) {0, 0, screen_width, screen_height};
        add_monitor(&rect);
    }

    for (monitor_t *m = mon_head; m != NULL; m = m->next)
        add_desktop(m, NULL);

    ewmh_update_number_of_desktops();
    ewmh_update_desktop_names();
    ewmh_update_current_desktop();
    rule_head = rule_tail = NULL;
    frozen_pointer = make_pointer_state();
    split_mode = MODE_AUTOMATIC;
    visible = true;
    exit_status = 0;
}

int main(int argc, char *argv[])
{
    fd_set descriptors;
    char socket_path[MAXLEN];
    char *fifo_path = NULL;
    int sock_fd, ret_fd, dpy_fd, sel, n;
    struct sockaddr_un sock_address;
    size_t rsplen = 0;
    char msg[BUFSIZ] = {0};
    char rsp[BUFSIZ] = {0};
    xcb_generic_event_t *event;
    char opt;

    while ((opt = getopt(argc, argv, "vs:")) != -1) {
        switch (opt) {
            case 'v':
                printf("%s\n", VERSION);
                exit(EXIT_SUCCESS);
                break;
            case 's':
                fifo_path = optarg;
                break;
        }
    }

    running = true;
    dpy = xcb_connect(NULL, &default_screen);

    if (xcb_connection_has_error(dpy))
        err("Can't open the default display.\n");

    setup();

    dpy_fd = xcb_get_file_descriptor(dpy);

    char *sp = getenv(SOCKET_ENV_VAR);
    strncpy(socket_path, (sp == NULL ? DEFAULT_SOCKET_PATH : sp), sizeof(socket_path));

    sock_address.sun_family = AF_UNIX;
    strncpy(sock_address.sun_path, socket_path, sizeof(sock_address.sun_path));
    unlink(socket_path);

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (sock_fd == -1)
        err("Couldn't create the socket.\n");

    if (bind(sock_fd, (struct sockaddr *) &sock_address, sizeof(sock_address)) == -1)
        err("Couldn't bind a name to the socket.\n");

    if (listen(sock_fd, SOMAXCONN) == -1)
        err("Couldn't listen to the socket.\n");

    sel = MAX(sock_fd, dpy_fd) + 1;

    if (fifo_path != NULL) {
        int fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
        if (fifo_fd != -1)
            status_fifo = fdopen(fifo_fd, "w");
        else
            warn("Couldn't open status fifo.\n");
    }

    load_settings();
    run_autostart();
    ewmh_update_wm_name();

    while (running) {

        xcb_flush(dpy);

        FD_ZERO(&descriptors);
        FD_SET(sock_fd, &descriptors);
        FD_SET(dpy_fd, &descriptors);

        if (select(sel, &descriptors, NULL, NULL, NULL) > 0) {

            if (FD_ISSET(sock_fd, &descriptors)) {
                ret_fd = accept(sock_fd, NULL, 0);
                if (ret_fd > 0 && (n = recv(ret_fd, msg, sizeof(msg), 0)) > 0) {
                    msg[n] = '\0';
                    process_message(msg, rsp);
                    rsplen = strlen(rsp);
                    if (rsp[rsplen - 1] == '\n')
                        rsp[--rsplen] = '\0';
                    send(ret_fd, rsp, rsplen, 0);
                    close(ret_fd);
                    rsp[0] = '\0';
                }
            }

            if (FD_ISSET(dpy_fd, &descriptors)) {
                while ((event = xcb_poll_for_event(dpy)) != NULL) {
                    handle_event(event);
                    free(event);
                }
            }

        }

        if (xcb_connection_has_error(dpy))
            err("The server has closed the connection.\n");
    }

    cleanup();
    close(sock_fd);
    if (status_fifo != NULL)
        fclose(status_fifo);
    xcb_ewmh_connection_wipe(ewmh);
    xcb_destroy_window(dpy, motion_recorder);
    free(ewmh);
    xcb_flush(dpy);
    xcb_disconnect(dpy);
    return exit_status;
}
