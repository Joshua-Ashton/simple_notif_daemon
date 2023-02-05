#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/signalfd.h>
#include <unistd.h>

#if defined(HAVE_LIBSYSTEMD)
#include <systemd/sd-bus.h>
#elif defined(HAVE_LIBELOGIND)
#include <elogind/sd-bus.h>
#elif defined(HAVE_BASU)
#include <basu/sd-bus.h>
#endif

enum sfd_event {
    SFD_EVENT_DBUS,
    SFD_EVENT_SIGNAL,

    SFD_EVENT_COUNT, // keep last
};

struct sfd_state {
    struct pollfd fds[SFD_EVENT_COUNT];
    sd_bus* bus;
    sd_bus_slot *xdg_slot;
    int signalfd;
    uint32_t last_notif_id;
};

static const char *service_name = "org.freedesktop.Notifications";
static const char *service_interface = "org.freedesktop.Notifications";
static const char *service_path = "/org/freedesktop/Notifications";

static int handle_get_capabilities(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	sd_bus_message *reply = NULL;
	int ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_open_container(reply, 'a', "s");
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_message_close_container(reply);
	if (ret < 0) {
		return ret;
	}

	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}

	sd_bus_message_unref(reply);
	return 0;
}

static int handle_notify(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	struct sfd_state *state = data;
	int ret = 0;

	const char *app_name, *app_icon, *summary, *body;
	uint32_t replaces_id;
	ret = sd_bus_message_read(msg, "susss", &app_name, &replaces_id, &app_icon,
		&summary, &body);
	if (ret < 0) {
		return ret;
	}

    if (*body) {
        printf("%s: %s - %s\n", app_name, summary, body);
    } else {
        printf("%s: %s\n", app_name, summary);
    }
	fflush(stdout);

    return sd_bus_reply_method_return(msg, "u", ++state->last_notif_id);
}

static int handle_close_notification(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	uint32_t id;
	int ret = sd_bus_message_read(msg, "u", &id);
	if (ret < 0) {
		return ret;
	}

    /* do nothing. */

	return sd_bus_reply_method_return(msg, "");
}

static int handle_get_server_information(sd_bus_message *msg, void *data,
		sd_bus_error *ret_error) {
	const char *name = "simple_notif_daemon";
	const char *vendor = "Joshua Ashton";
	const char *version = "1.0.0";
	const char *spec_version = "1.2";
	return sd_bus_reply_method_return(msg, "ssss", name, vendor, version,
		spec_version);
}

static const sd_bus_vtable service_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("GetCapabilities", "", "as", handle_get_capabilities, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", handle_notify, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("CloseNotification", "u", "", handle_close_notification, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetServerInformation", "", "ssss", handle_get_server_information, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("ActionInvoked", "us", 0),
	SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
	SD_BUS_VTABLE_END
};

static int init_dbus_xdg(struct sfd_state *state) {
	return sd_bus_add_object_vtable(state->bus, &state->xdg_slot, service_path,
		service_interface, service_vtable, state);
}

static void finish_dbus(struct sfd_state *state) {
	sd_bus_slot_unref(state->xdg_slot);
    state->xdg_slot = NULL;
	sd_bus_flush_close_unref(state->bus);
    state->bus = NULL;
}

static int init_signalfd() {
	sigset_t mask;
	int sfd;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGQUIT);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
		fprintf(stderr, "sigprocmask: %s", strerror(errno));
		return -1;
	}

	if ((sfd = signalfd(-1, &mask, SFD_NONBLOCK)) == -1) {
		fprintf(stderr, "signalfd: %s", strerror(errno));
		return -1;
	}

	return sfd;
}

static bool init_dbus(struct sfd_state *state) {
    int ret = 0;

    ret = sd_bus_open_user(&state->bus);
    if (ret < 0) {
		fprintf(stderr, "Failed to connect to user bus: %s\n", strerror(-ret));
		goto error;
    }

	ret = init_dbus_xdg(state);
	if (ret < 0) {
		fprintf(stderr, "Failed to initialize XDG interface: %s\n", strerror(-ret));
		goto error;
	}

	ret = sd_bus_request_name(state->bus, service_name, 0);
	if (ret < 0) {
		fprintf(stderr, "Failed to acquire service name: %s\n", strerror(-ret));
		if (ret == -EEXIST) {
			fprintf(stderr, "Is a notification daemon already running?\n");
		}
		goto error;
	}

	return true;

error:
	finish_dbus(state);
	return false;
}

static void finish_state(struct sfd_state *state) {
    close(state->signalfd);
    finish_dbus(state);
    *state = (struct sfd_state){0};
}

static bool init_state(struct sfd_state *state) {
    if (!init_dbus(state)) {
        finish_state(state);
        return false;
    }

	if ((state->signalfd = init_signalfd()) == -1) {
        finish_state(state);
		return false;
	}

	state->fds[SFD_EVENT_SIGNAL] = (struct pollfd){
		.fd = state->signalfd,
		.events = POLLIN,
	};

	state->fds[SFD_EVENT_DBUS] = (struct pollfd){
		.fd = sd_bus_get_fd(state->bus),
		.events = POLLIN,
	};

    return true;
}

static bool run_loop(struct sfd_state *state) {
    bool running = true;

    int ret = 0;
	// Unprocessed messages can be queued up by synchronous sd_bus methods. We
	// need to process these.
	do {
		ret = sd_bus_process(state->bus, NULL);
	} while (ret > 0);

	if (ret < 0) {
		return false;
	}

    while (running) {
        errno = 0;

        // Flush any D-Bus requests we may have generated.
        sd_bus_flush(state->bus);

		ret = poll(state->fds, SFD_EVENT_COUNT, -1);
		if (!running) {
			ret = 0;
			break;
		}

		if (ret < 0) {
			fprintf(stderr, "failed to poll(): %s\n", strerror(errno));
			break;
		}

		for (size_t i = 0; i < SFD_EVENT_COUNT; ++i) {
			if (state->fds[i].revents & POLLHUP) {
				running = false;
				break;
			}
			if (state->fds[i].revents & POLLERR) {
				fprintf(stderr, "failed to poll() socket %zu\n", i);
				ret = -1;
				break;
			}
		}

		if (!running || ret < 0) {
			break;
		}

		if (state->fds[SFD_EVENT_SIGNAL].revents & POLLIN) {
			break;
		}

		if (state->fds[SFD_EVENT_DBUS].revents & POLLIN) {
			do {
				ret = sd_bus_process(state->bus, NULL);
			} while (ret > 0);

			if (ret < 0) {
				fprintf(stderr, "failed to process D-Bus: %s\n",
					strerror(-ret));
				break;
			}
		}
		if (state->fds[SFD_EVENT_DBUS].revents & POLLOUT) {
			ret = sd_bus_flush(state->bus);
			if (ret < 0) {
				fprintf(stderr, "failed to flush D-Bus: %s\n",
					strerror(-ret));
				break;
			}
		}
    }

    return ret == 0 ? true : false;
}

int main(int argc, char *argv[]) {
    struct sfd_state state = {0};
    bool success;

    if (!init_state(&state)) {
        return EXIT_FAILURE;
    }

    success = run_loop(&state);
    finish_state(&state);
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}