/* screencast-portal.c
 *
 * Copyright 2022 Georges Basile Stavracas Neto <georges.stavracas@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "pipewire.h"
#include "portal.h"

#include <gio/gunixfdlist.h>
#include <unistd.h>

enum portal_capture_type {
	PORTAL_CAPTURE_TYPE_MONITOR = 1 << 0,
	PORTAL_CAPTURE_TYPE_WINDOW = 1 << 1,
	PORTAL_CAPTURE_TYPE_VIRTUAL = 1 << 2,
};

enum portal_cursor_mode {
	PORTAL_CURSOR_MODE_HIDDEN = 1 << 0,
	PORTAL_CURSOR_MODE_EMBEDDED = 1 << 1,
	PORTAL_CURSOR_MODE_METADATA = 1 << 2,
};

enum obs_portal_capture_type {
	OBS_PORTAL_CAPTURE_TYPE_MONITOR = PORTAL_CAPTURE_TYPE_MONITOR,
	OBS_PORTAL_CAPTURE_TYPE_WINDOW = PORTAL_CAPTURE_TYPE_WINDOW,
	OBS_PORTAL_CAPTURE_TYPE_UNIFIED = PORTAL_CAPTURE_TYPE_MONITOR | PORTAL_CAPTURE_TYPE_WINDOW,
};

struct screencast_portal_capture {
	enum obs_portal_capture_type capture_type;

	GCancellable *cancellable;

	char *session_handle;
	char *restore_token;

	obs_source_t *source;
	obs_data_t *settings;

	uint32_t pipewire_node;
	bool cursor_visible;

	obs_pipewire *obs_pw;
	obs_pipewire_stream *obs_pw_stream;

	GDBusProxy *login_session;
	bool session_locked;
	bool restore_after_unlock;
	int restart_pending;
	bool destroyed;
	int reached_streaming;
	int idle_refs;
	guint restore_timer;
	gint64 restore_not_before;
	gint64 restore_wait_started;
	bool restore_wait_logged;
};

/* The portal only skips the share dialog once the saved screen is back in
 * Mutter's monitor list. Asking in the same moment the lock tears the cast
 * down makes GNOME show the picker instead. */
#define SCREENCAST_RESTORE_SETTLE_USEC (2 * G_USEC_PER_SEC)
#define SCREENCAST_RESTORE_POLL_MS 500
#define SCREENCAST_RESTORE_GIVE_UP_USEC (120 * G_USEC_PER_SEC)

/* ------------------------------------------------- */

static GDBusProxy *screencast_proxy = NULL;

static void ensure_screencast_portal_proxy(void)
{
	g_autoptr(GError) error = NULL;
	if (!screencast_proxy) {
		screencast_proxy = g_dbus_proxy_new_sync(portal_get_dbus_connection(), G_DBUS_PROXY_FLAGS_NONE, NULL,
							 "org.freedesktop.portal.Desktop",
							 "/org/freedesktop/portal/desktop",
							 "org.freedesktop.portal.ScreenCast", NULL, &error);

		if (error) {
			blog(LOG_WARNING, "[portals] Error retrieving D-Bus proxy: %s", error->message);
			return;
		}
	}
}

static GDBusProxy *get_screencast_portal_proxy(void)
{
	ensure_screencast_portal_proxy();
	return screencast_proxy;
}

static uint32_t get_available_capture_types(void)
{
	g_autoptr(GVariant) cached_source_types = NULL;
	uint32_t available_source_types;

	ensure_screencast_portal_proxy();

	if (!screencast_proxy)
		return 0;

	cached_source_types = g_dbus_proxy_get_cached_property(screencast_proxy, "AvailableSourceTypes");
	available_source_types = cached_source_types ? g_variant_get_uint32(cached_source_types) : 0;

	return available_source_types;
}

static uint32_t get_available_cursor_modes(void)
{
	g_autoptr(GVariant) cached_cursor_modes = NULL;
	uint32_t available_cursor_modes;

	ensure_screencast_portal_proxy();

	if (!screencast_proxy)
		return 0;

	cached_cursor_modes = g_dbus_proxy_get_cached_property(screencast_proxy, "AvailableCursorModes");
	available_cursor_modes = cached_cursor_modes ? g_variant_get_uint32(cached_cursor_modes) : 0;

	return available_cursor_modes;
}

static uint32_t get_screencast_version(void)
{
	g_autoptr(GVariant) cached_version = NULL;
	uint32_t version;

	ensure_screencast_portal_proxy();

	if (!screencast_proxy)
		return 0;

	cached_version = g_dbus_proxy_get_cached_property(screencast_proxy, "version");
	version = cached_version ? g_variant_get_uint32(cached_version) : 0;

	return version;
}

/* ------------------------------------------------- */

static const char *capture_type_to_string(enum obs_portal_capture_type capture_type)
{
	switch (capture_type) {
	case OBS_PORTAL_CAPTURE_TYPE_MONITOR:
		return "monitor";
	case OBS_PORTAL_CAPTURE_TYPE_WINDOW:
		return "window";
	case OBS_PORTAL_CAPTURE_TYPE_UNIFIED:
		return "monitor and window";
	default:
		return "unknown";
	}
}

/* ------------------------------------------------- */

static void on_screencast_stream_state(enum pw_stream_state state, void *data);
static void create_session(struct screencast_portal_capture *capture);
static void schedule_screencast_restore(struct screencast_portal_capture *capture);
static void arm_screencast_restore(struct screencast_portal_capture *capture, guint delay_ms);
static void screencast_restore_failed(struct screencast_portal_capture *capture);

static void on_pipewire_remote_opened_cb(GObject *source, GAsyncResult *res, void *user_data)
{
	struct obs_pipewire_connect_stream_info connect_info;
	struct screencast_portal_capture *capture;
	g_autoptr(GUnixFDList) fd_list = NULL;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;
	int pipewire_fd;
	int fd_index;

	capture = user_data;
	result = g_dbus_proxy_call_with_unix_fd_list_finish(G_DBUS_PROXY(source), &fd_list, res, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR, "[pipewire] Error retrieving pipewire fd: %s", error->message);
		screencast_restore_failed(capture);
		return;
	}

	g_variant_get(result, "(h)", &fd_index, &error);

	pipewire_fd = g_unix_fd_list_get(fd_list, fd_index, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR, "[pipewire] Error retrieving pipewire fd: %s", error->message);
		screencast_restore_failed(capture);
		return;
	}

	capture->obs_pw = obs_pipewire_connect_fd(pipewire_fd, NULL, NULL);

	if (!capture->obs_pw) {
		screencast_restore_failed(capture);
		return;
	}

	connect_info = (struct obs_pipewire_connect_stream_info){
		.stream_name = "OBS Studio",
		.stream_properties = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
						       PW_KEY_MEDIA_ROLE, "Screen", NULL),
		.state_callback = on_screencast_stream_state,
		.state_callback_data = capture,
		.screencast =
			{
				.cursor_visible = capture->cursor_visible,
			},
	};

	capture->obs_pw_stream =
		obs_pipewire_connect_stream(capture->obs_pw, capture->source, capture->pipewire_node, &connect_info);
}

static void open_pipewire_remote(struct screencast_portal_capture *capture)
{
	GVariantBuilder builder;

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);

	g_dbus_proxy_call_with_unix_fd_list(get_screencast_portal_proxy(), "OpenPipeWireRemote",
					    g_variant_new("(oa{sv})", capture->session_handle, &builder),
					    G_DBUS_CALL_FLAGS_NONE, -1, NULL, capture->cancellable,
					    on_pipewire_remote_opened_cb, capture);
}

/* ------------------------------------------------- */

static void on_start_response_received_cb(GVariant *parameters, void *user_data)
{
	struct screencast_portal_capture *capture = user_data;
	g_autoptr(GVariant) stream_properties = NULL;
	g_autoptr(GVariant) streams = NULL;
	g_autoptr(GVariant) result = NULL;
	GVariantIter iter;
	uint32_t response;
	size_t n_streams;

	g_variant_get(parameters, "(u@a{sv})", &response, &result);

	if (response != 0) {
		blog(LOG_WARNING, "[pipewire] Failed to start screencast, denied or cancelled by user");
		screencast_restore_failed(capture);
		return;
	}

	streams = g_variant_lookup_value(result, "streams", G_VARIANT_TYPE_ARRAY);

	g_variant_iter_init(&iter, streams);

	n_streams = g_variant_iter_n_children(&iter);
	if (n_streams != 1) {
		blog(LOG_WARNING, "[pipewire] Received more than one stream when only one was expected. "
				  "This is probably a bug in the desktop portal implementation you are "
				  "using.");

		// The KDE Desktop portal implementation sometimes sends an invalid
		// response where more than one stream is attached, and only the
		// last one is the one we're looking for. This is the only known
		// buggy implementation, so let's at least try to make it work here.
		for (size_t i = 0; i < n_streams - 1; i++) {
			g_autoptr(GVariant) throwaway_properties = NULL;
			uint32_t throwaway_pipewire_node;

			g_variant_iter_loop(&iter, "(u@a{sv})", &throwaway_pipewire_node, &throwaway_properties);
		}
	}

	g_variant_iter_loop(&iter, "(u@a{sv})", &capture->pipewire_node, &stream_properties);

	if (get_screencast_version() >= 4) {
		g_autoptr(GVariant) restore_token = NULL;

		g_clear_pointer(&capture->restore_token, bfree);

		restore_token = g_variant_lookup_value(result, "restore_token", G_VARIANT_TYPE_STRING);
		if (restore_token)
			capture->restore_token = bstrdup(g_variant_get_string(restore_token, NULL));

		obs_source_save(capture->source);
	}

	blog(LOG_INFO, "[pipewire] source selected, setting up screencast");

	open_pipewire_remote(capture);
}

static void on_started_cb(GObject *source, GAsyncResult *res, void *user_data)
{
	struct screencast_portal_capture *capture = user_data;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;

	result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR, "[pipewire] Error selecting screencast source: %s", error->message);
		if (capture)
			screencast_restore_failed(capture);
		return;
	}
}

static void start(struct screencast_portal_capture *capture)
{
	GVariantBuilder builder;
	char *request_token;
	char *request_path;

	portal_create_request_path(&request_path, &request_token);

	blog(LOG_INFO, "[pipewire] Asking for %s", capture_type_to_string(capture->capture_type));

	portal_signal_subscribe(request_path, capture->cancellable, on_start_response_received_cb, capture);

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));

	g_dbus_proxy_call(get_screencast_portal_proxy(), "Start",
			  g_variant_new("(osa{sv})", capture->session_handle, "", &builder), G_DBUS_CALL_FLAGS_NONE, -1,
			  capture->cancellable, on_started_cb, capture);

	bfree(request_token);
	bfree(request_path);
}

/* ------------------------------------------------- */

static void on_select_source_response_received_cb(GVariant *parameters, void *user_data)
{
	struct screencast_portal_capture *capture = user_data;
	g_autoptr(GVariant) ret = NULL;
	uint32_t response;

	blog(LOG_DEBUG, "[pipewire] Response to select source received");

	g_variant_get(parameters, "(u@a{sv})", &response, &ret);

	if (response != 0) {
		blog(LOG_WARNING, "[pipewire] Failed to select source, denied or cancelled by user");
		screencast_restore_failed(capture);
		return;
	}

	start(capture);
}

static void on_source_selected_cb(GObject *source, GAsyncResult *res, void *user_data)
{
	struct screencast_portal_capture *capture = user_data;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;

	result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR, "[pipewire] Error selecting screencast source: %s", error->message);
		if (capture)
			screencast_restore_failed(capture);
		return;
	}
}

static void select_source(struct screencast_portal_capture *capture)
{
	GVariantBuilder builder;
	uint32_t available_cursor_modes;
	char *request_token;
	char *request_path;

	portal_create_request_path(&request_path, &request_token);

	portal_signal_subscribe(request_path, capture->cancellable, on_select_source_response_received_cb, capture);

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "types", g_variant_new_uint32(capture->capture_type));
	g_variant_builder_add(&builder, "{sv}", "multiple", g_variant_new_boolean(FALSE));
	g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));

	available_cursor_modes = get_available_cursor_modes();

	if (available_cursor_modes & PORTAL_CURSOR_MODE_METADATA)
		g_variant_builder_add(&builder, "{sv}", "cursor_mode",
				      g_variant_new_uint32(PORTAL_CURSOR_MODE_METADATA));
	else if ((available_cursor_modes & PORTAL_CURSOR_MODE_EMBEDDED) && capture->cursor_visible)
		g_variant_builder_add(&builder, "{sv}", "cursor_mode",
				      g_variant_new_uint32(PORTAL_CURSOR_MODE_EMBEDDED));
	else
		g_variant_builder_add(&builder, "{sv}", "cursor_mode", g_variant_new_uint32(PORTAL_CURSOR_MODE_HIDDEN));

	if (get_screencast_version() >= 4) {
		g_variant_builder_add(&builder, "{sv}", "persist_mode", g_variant_new_uint32(2));
		if (capture->restore_token && *capture->restore_token) {
			g_variant_builder_add(&builder, "{sv}", "restore_token",
					      g_variant_new_string(capture->restore_token));
		}
	}

	g_dbus_proxy_call(get_screencast_portal_proxy(), "SelectSources",
			  g_variant_new("(oa{sv})", capture->session_handle, &builder), G_DBUS_CALL_FLAGS_NONE, -1,
			  capture->cancellable, on_source_selected_cb, capture);

	bfree(request_token);
	bfree(request_path);
}

/* ------------------------------------------------- */

static void on_create_session_response_received_cb(GVariant *parameters, void *user_data)
{
	struct screencast_portal_capture *capture = user_data;
	g_autoptr(GVariant) session_handle_variant = NULL;
	g_autoptr(GVariant) result = NULL;
	uint32_t response;

	g_variant_get(parameters, "(u@a{sv})", &response, &result);

	if (response != 0) {
		blog(LOG_WARNING, "[pipewire] Failed to create session, denied or cancelled by user");
		screencast_restore_failed(capture);
		return;
	}

	blog(LOG_INFO, "[pipewire] Screencast session created");

	session_handle_variant = g_variant_lookup_value(result, "session_handle", NULL);
	capture->session_handle = g_variant_dup_string(session_handle_variant, NULL);

	select_source(capture);
}

static void on_session_created_cb(GObject *source, GAsyncResult *res, void *user_data)
{
	struct screencast_portal_capture *capture = user_data;
	g_autoptr(GVariant) result = NULL;
	g_autoptr(GError) error = NULL;

	result = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
	if (error) {
		if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			blog(LOG_ERROR, "[pipewire] Error creating screencast session: %s", error->message);
		if (capture)
			screencast_restore_failed(capture);
		return;
	}
}

static void create_session(struct screencast_portal_capture *capture)
{
	GVariantBuilder builder;
	char *session_token;
	char *request_token;
	char *request_path;

	portal_create_request_path(&request_path, &request_token);
	portal_create_session_path(NULL, &session_token);

	portal_signal_subscribe(request_path, capture->cancellable, on_create_session_response_received_cb, capture);

	g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
	g_variant_builder_add(&builder, "{sv}", "handle_token", g_variant_new_string(request_token));
	g_variant_builder_add(&builder, "{sv}", "session_handle_token", g_variant_new_string(session_token));

	g_dbus_proxy_call(get_screencast_portal_proxy(), "CreateSession", g_variant_new("(a{sv})", &builder),
			  G_DBUS_CALL_FLAGS_NONE, -1, capture->cancellable, on_session_created_cb, capture);

	bfree(session_token);
	bfree(request_token);
	bfree(request_path);
}

/* ------------------------------------------------- */

static void screencast_restore_failed(struct screencast_portal_capture *capture)
{
	g_atomic_int_set(&capture->restart_pending, FALSE);
}

static void teardown_pipewire_capture(struct screencast_portal_capture *capture)
{
	g_clear_pointer(&capture->obs_pw_stream, obs_pipewire_stream_destroy);
	g_clear_pointer(&capture->obs_pw, obs_pipewire_destroy);
}

static void close_portal_session(struct screencast_portal_capture *capture)
{
	if (!capture->session_handle)
		return;

	g_dbus_connection_call(portal_get_dbus_connection(), "org.freedesktop.portal.Desktop", capture->session_handle,
			       "org.freedesktop.portal.Session", "Close", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL,
			       NULL, NULL);
	g_clear_pointer(&capture->session_handle, g_free);
}

static void refresh_session_locked(struct screencast_portal_capture *capture)
{
	g_autoptr(GVariant) locked = NULL;

	if (!capture->login_session)
		return;

	locked = g_dbus_proxy_get_cached_property(capture->login_session, "LockedHint");
	if (locked)
		capture->session_locked = g_variant_get_boolean(locked);
}

static bool mutter_has_logical_monitor(bool *known)
{
	g_autoptr(GDBusConnection) bus = NULL;
	g_autoptr(GVariant) state = NULL;
	g_autoptr(GVariant) logical = NULL;
	g_autoptr(GError) error = NULL;

	*known = false;
	bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
	if (!bus)
		return false;

	state = g_dbus_connection_call_sync(bus, "org.gnome.Mutter.DisplayConfig", "/org/gnome/Mutter/DisplayConfig",
					    "org.gnome.Mutter.DisplayConfig", "GetCurrentState", NULL,
					    G_VARIANT_TYPE("(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})"),
					    G_DBUS_CALL_FLAGS_NONE, 200, NULL, &error);
	if (!state)
		return false;

	*known = true;
	logical = g_variant_get_child_value(state, 1);
	return g_variant_n_children(logical) > 0;
}

static gboolean restore_screencast_idle(gpointer data)
{
	struct screencast_portal_capture *capture = data;
	gint64 now;
	bool monitors_known = false;
	bool monitors_ready;
	bool gave_up;

	if (capture->destroyed)
		goto done;

	refresh_session_locked(capture);

	if (capture->session_locked) {
		capture->restore_after_unlock = true;
		capture->restore_not_before = 0;
		capture->restore_wait_started = 0;
		capture->restore_wait_logged = false;
		goto done;
	}

	if (g_atomic_int_get(&capture->restart_pending))
		goto done;

	if (!obs_source_showing(capture->source))
		goto done;

	if (!g_atomic_int_get(&capture->reached_streaming) && !capture->restore_after_unlock)
		goto done;

	if (obs_pipewire_stream_accepts_frames(capture->obs_pw_stream))
		goto done;

	if (!capture->restore_token || !*capture->restore_token) {
		blog(LOG_WARNING, "[pipewire] Screen capture stopped and there is no restore token");
		goto done;
	}

	now = g_get_monotonic_time();
	if (capture->restore_wait_started == 0)
		capture->restore_wait_started = now;

	monitors_ready = mutter_has_logical_monitor(&monitors_known);
	gave_up = now - capture->restore_wait_started >= SCREENCAST_RESTORE_GIVE_UP_USEC;
	if ((!monitors_known || !monitors_ready) && !gave_up) {
		capture->restore_after_unlock = true;
		capture->restore_not_before = 0;
		if (!capture->restore_wait_logged) {
			blog(LOG_INFO, "[pipewire] Waiting for the screen to come back before restoring the saved share");
			capture->restore_wait_logged = true;
		}
		arm_screencast_restore(capture, SCREENCAST_RESTORE_POLL_MS);
		goto done;
	}

	/* Same token OBS sends on startup. Give Mutter a moment after unlock so
	 * that token still matches the monitor and the share dialog stays closed. */
	if (capture->restore_not_before == 0) {
		capture->restore_not_before = now + SCREENCAST_RESTORE_SETTLE_USEC;
		arm_screencast_restore(capture, SCREENCAST_RESTORE_SETTLE_USEC / 1000);
		goto done;
	}
	if (now < capture->restore_not_before) {
		gint64 left_ms = (capture->restore_not_before - now) / 1000;

		arm_screencast_restore(capture, (guint)(left_ms < 1 ? 1 : left_ms));
		goto done;
	}

	capture->restore_after_unlock = false;
	capture->restore_not_before = 0;
	capture->restore_wait_started = 0;
	capture->restore_wait_logged = false;
	g_atomic_int_set(&capture->restart_pending, TRUE);
	g_atomic_int_set(&capture->reached_streaming, FALSE);
	blog(LOG_INFO, "[pipewire] Restoring the previously shared screen");

	teardown_pipewire_capture(capture);
	close_portal_session(capture);
	create_session(capture);

done:
	if (g_atomic_int_dec_and_test(&capture->idle_refs) && capture->destroyed)
		bfree(capture);
	return G_SOURCE_REMOVE;
}

static gboolean screencast_restore_timeout(gpointer data)
{
	struct screencast_portal_capture *capture = data;

	capture->restore_timer = 0;
	if (!capture->destroyed)
		schedule_screencast_restore(capture);
	return G_SOURCE_REMOVE;
}

static void arm_screencast_restore(struct screencast_portal_capture *capture, guint delay_ms)
{
	if (capture->restore_timer)
		g_source_remove(capture->restore_timer);
	capture->restore_timer = g_timeout_add(delay_ms, screencast_restore_timeout, capture);
}

static void schedule_screencast_restore(struct screencast_portal_capture *capture)
{
	g_atomic_int_inc(&capture->idle_refs);
	g_idle_add(restore_screencast_idle, capture);
}

static void on_screencast_stream_state(enum pw_stream_state state, void *data)
{
	struct screencast_portal_capture *capture = data;
	int was_streaming = g_atomic_int_get(&capture->reached_streaming);

	if (state == PW_STREAM_STATE_STREAMING) {
		g_atomic_int_set(&capture->reached_streaming, TRUE);
		g_atomic_int_set(&capture->restart_pending, FALSE);
		return;
	}

	if (was_streaming && (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_ERROR ||
			      state == PW_STREAM_STATE_UNCONNECTED))
		schedule_screencast_restore(capture);
}

static void on_login_session_properties_changed(GDBusProxy *proxy, GVariant *changed_properties,
						const char **invalidated_properties, gpointer user_data)
{
	struct screencast_portal_capture *capture = user_data;
	gboolean locked = FALSE;

	UNUSED_PARAMETER(proxy);
	UNUSED_PARAMETER(invalidated_properties);

	if (!g_variant_lookup(changed_properties, "LockedHint", "b", &locked))
		return;

	capture->session_locked = locked;
	if (locked) {
		blog(LOG_INFO, "[pipewire] Session locked, screen capture will resume on unlock");
		return;
	}

	blog(LOG_INFO, "[pipewire] Session unlocked");
	if (!obs_pipewire_stream_accepts_frames(capture->obs_pw_stream))
		g_atomic_int_set(&capture->restart_pending, FALSE);
	schedule_screencast_restore(capture);
}

static GDBusProxy *login_session_proxy_new(GDBusConnection *bus, const char *path)
{
	g_autoptr(GError) error = NULL;
	GDBusProxy *proxy;

	proxy = g_dbus_proxy_new_sync(bus, G_DBUS_PROXY_FLAGS_NONE, NULL, "org.freedesktop.login1", path,
				      "org.freedesktop.login1.Session", NULL, &error);
	if (!proxy)
		blog(LOG_WARNING, "[pipewire] Could not open login session %s: %s", path, error->message);
	return proxy;
}

static bool is_graphical_user_session(GDBusProxy *proxy)
{
	g_autoptr(GVariant) session_class = NULL;
	g_autoptr(GVariant) session_type = NULL;
	const char *class_str;
	const char *type_str;

	session_class = g_dbus_proxy_get_cached_property(proxy, "Class");
	session_type = g_dbus_proxy_get_cached_property(proxy, "Type");
	class_str = session_class ? g_variant_get_string(session_class, NULL) : NULL;
	type_str = session_type ? g_variant_get_string(session_type, NULL) : NULL;

	if (g_strcmp0(class_str, "user") != 0)
		return false;
	return g_strcmp0(type_str, "x11") == 0 || g_strcmp0(type_str, "wayland") == 0;
}

static GDBusProxy *take_if_graphical_session(GDBusConnection *bus, const char *path)
{
	GDBusProxy *proxy = login_session_proxy_new(bus, path);

	if (proxy && !is_graphical_user_session(proxy))
		g_clear_object(&proxy);
	return proxy;
}

/* OBS started from a terminal is in the user slice, not the seat session, so
 * GetSessionByPID fails. The share dialog is tied to the graphical session. */
static GDBusProxy *find_graphical_login_session(GDBusConnection *bus, GDBusProxy *manager)
{
	g_autoptr(GVariant) by_pid = NULL;
	g_autoptr(GVariant) listed = NULL;
	g_autoptr(GVariant) sessions = NULL;
	GVariantIter iter;
	const char *id;
	const char *user;
	const char *seat;
	const char *path;
	const char *env_id;
	guint32 uid;
	GDBusProxy *fallback = NULL;

	by_pid = g_dbus_proxy_call_sync(manager, "GetSessionByPID", g_variant_new("(u)", (guint32)getpid()),
					G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
	if (by_pid) {
		GDBusProxy *proxy;

		g_variant_get(by_pid, "(o)", &path);
		proxy = take_if_graphical_session(bus, path);
		if (proxy)
			return proxy;
	}

	env_id = getenv("XDG_SESSION_ID");
	if (env_id && *env_id) {
		g_autoptr(GVariant) by_id = NULL;

		by_id = g_dbus_proxy_call_sync(manager, "GetSession", g_variant_new("(s)", env_id),
					       G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
		if (by_id) {
			GDBusProxy *proxy;

			g_variant_get(by_id, "(o)", &path);
			proxy = take_if_graphical_session(bus, path);
			if (proxy)
				return proxy;
		}
	}

	listed = g_dbus_proxy_call_sync(manager, "ListSessions", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
	if (!listed)
		return NULL;

	sessions = g_variant_get_child_value(listed, 0);
	g_variant_iter_init(&iter, sessions);
	while (g_variant_iter_next(&iter, "(susso)", &id, &uid, &user, &seat, &path)) {
		GDBusProxy *proxy;
		g_autoptr(GVariant) active = NULL;

		UNUSED_PARAMETER(id);
		UNUSED_PARAMETER(user);
		UNUSED_PARAMETER(seat);

		if (uid != (guint32)getuid())
			continue;

		proxy = take_if_graphical_session(bus, path);
		if (!proxy)
			continue;

		active = g_dbus_proxy_get_cached_property(proxy, "Active");
		if (active && g_variant_get_boolean(active)) {
			g_clear_object(&fallback);
			return proxy;
		}

		if (!fallback)
			fallback = proxy;
		else
			g_object_unref(proxy);
	}

	return fallback;
}

static void watch_session_lock(struct screencast_portal_capture *capture)
{
	g_autoptr(GDBusConnection) system_bus = NULL;
	g_autoptr(GDBusProxy) manager = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) session_id = NULL;
	const char *id = NULL;

	system_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (!system_bus) {
		blog(LOG_WARNING, "[pipewire] Could not watch session lock: %s", error->message);
		return;
	}

	manager = g_dbus_proxy_new_sync(system_bus, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL,
					"org.freedesktop.login1", "/org/freedesktop/login1",
					"org.freedesktop.login1.Manager", NULL, &error);
	if (!manager) {
		blog(LOG_WARNING, "[pipewire] Could not watch session lock: %s", error->message);
		return;
	}

	capture->login_session = find_graphical_login_session(system_bus, manager);
	if (!capture->login_session) {
		blog(LOG_WARNING, "[pipewire] Could not watch session lock: no graphical login session");
		return;
	}

	session_id = g_dbus_proxy_get_cached_property(capture->login_session, "Id");
	if (session_id)
		id = g_variant_get_string(session_id, NULL);
	blog(LOG_INFO, "[pipewire] Watching screen lock on session %s", id ? id : "?");

	refresh_session_locked(capture);

	g_signal_connect(capture->login_session, "g-properties-changed", G_CALLBACK(on_login_session_properties_changed),
			 capture);
}

static gboolean init_screencast_capture(struct screencast_portal_capture *capture)
{
	GDBusConnection *connection;
	GDBusProxy *proxy;

	capture->cancellable = g_cancellable_new();
	connection = portal_get_dbus_connection();
	if (!connection)
		return FALSE;
	proxy = get_screencast_portal_proxy();
	if (!proxy)
		return FALSE;

	blog(LOG_INFO, "PipeWire initialized");

	if (!capture->login_session)
		watch_session_lock(capture);

	create_session(capture);

	return TRUE;
}

static bool reload_session_cb(obs_properties_t *properties, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(properties);
	UNUSED_PARAMETER(property);

	struct screencast_portal_capture *capture = data;

	g_clear_pointer(&capture->restore_token, bfree);
	g_clear_pointer(&capture->obs_pw_stream, obs_pipewire_stream_destroy);
	g_clear_pointer(&capture->obs_pw, obs_pipewire_destroy);

	if (capture->session_handle) {
		blog(LOG_DEBUG, "[pipewire] Cleaning previous session %s", capture->session_handle);
		g_dbus_connection_call(portal_get_dbus_connection(), "org.freedesktop.portal.Desktop",
				       capture->session_handle, "org.freedesktop.portal.Session", "Close", NULL, NULL,
				       G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);

		g_clear_pointer(&capture->session_handle, g_free);
	}

	init_screencast_capture(capture);

	return false;
}

/* obs_source_info methods */

static const char *screencast_portal_desktop_capture_get_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("PipeWireDesktopCapture");
}

static const char *screencast_portal_window_capture_get_name(void *data)
{
	UNUSED_PARAMETER(data);
	return obs_module_text("PipeWireWindowCapture");
}

static void *screencast_portal_desktop_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct screencast_portal_capture *capture;

	capture = bzalloc(sizeof(struct screencast_portal_capture));
	capture->capture_type = OBS_PORTAL_CAPTURE_TYPE_MONITOR;
	capture->cursor_visible = obs_data_get_bool(settings, "ShowCursor");
	capture->restore_token = bstrdup(obs_data_get_string(settings, "RestoreToken"));
	capture->source = source;

	init_screencast_capture(capture);

	return capture;
}
static void *screencast_portal_window_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct screencast_portal_capture *capture;

	capture = bzalloc(sizeof(struct screencast_portal_capture));
	capture->capture_type = OBS_PORTAL_CAPTURE_TYPE_WINDOW;
	capture->cursor_visible = obs_data_get_bool(settings, "ShowCursor");
	capture->restore_token = bstrdup(obs_data_get_string(settings, "RestoreToken"));
	capture->source = source;

	init_screencast_capture(capture);

	return capture;
}

static void *screencast_portal_capture_create(obs_data_t *settings, obs_source_t *source)
{
	struct screencast_portal_capture *capture;

	capture = bzalloc(sizeof(struct screencast_portal_capture));
	capture->capture_type = OBS_PORTAL_CAPTURE_TYPE_UNIFIED;
	capture->cursor_visible = obs_data_get_bool(settings, "ShowCursor");
	capture->restore_token = bstrdup(obs_data_get_string(settings, "RestoreToken"));
	capture->source = source;

	init_screencast_capture(capture);

	return capture;
}

static void screencast_portal_capture_destroy(void *data)
{
	struct screencast_portal_capture *capture = data;

	if (!capture)
		return;

	capture->destroyed = true;

	if (capture->restore_timer) {
		g_source_remove(capture->restore_timer);
		capture->restore_timer = 0;
	}

	if (capture->login_session) {
		g_signal_handlers_disconnect_by_data(capture->login_session, capture);
		g_clear_object(&capture->login_session);
	}

	close_portal_session(capture);

	g_clear_pointer(&capture->restore_token, bfree);

	teardown_pipewire_capture(capture);
	g_cancellable_cancel(capture->cancellable);
	g_clear_object(&capture->cancellable);
	if (g_atomic_int_get(&capture->idle_refs) == 0)
		bfree(capture);
}

static void screencast_portal_capture_save(void *data, obs_data_t *settings)
{
	struct screencast_portal_capture *capture = data;

	obs_data_set_string(settings, "RestoreToken", capture->restore_token);
}

static void screencast_portal_capture_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "ShowCursor", true);
	obs_data_set_default_string(settings, "RestoreToken", NULL);
}

static obs_properties_t *screencast_portal_capture_get_properties(void *data)
{
	struct screencast_portal_capture *capture = data;
	const char *reload_string_id;
	obs_properties_t *properties;

	switch (capture->capture_type) {
	case OBS_PORTAL_CAPTURE_TYPE_MONITOR:
		reload_string_id = "PipeWireSelectMonitor";
		break;
	case OBS_PORTAL_CAPTURE_TYPE_WINDOW:
		reload_string_id = "PipeWireSelectWindow";
		break;
	case OBS_PORTAL_CAPTURE_TYPE_UNIFIED:
		reload_string_id = "PipeWireSelectScreenCast";
		break;
	default:
		return NULL;
	}

	properties = obs_properties_create();
	obs_properties_add_button2(properties, "Reload", obs_module_text(reload_string_id), reload_session_cb, capture);
	obs_properties_add_bool(properties, "ShowCursor", obs_module_text("ShowCursor"));

	return properties;
}

static void screencast_portal_capture_update(void *data, obs_data_t *settings)
{
	struct screencast_portal_capture *capture = data;

	capture->cursor_visible = obs_data_get_bool(settings, "ShowCursor");

	if (capture->obs_pw_stream)
		obs_pipewire_stream_set_cursor_visible(capture->obs_pw_stream, capture->cursor_visible);
}

static void screencast_portal_capture_show(void *data)
{
	struct screencast_portal_capture *capture = data;

	if (capture->obs_pw_stream)
		obs_pipewire_stream_show(capture->obs_pw_stream);
	if (capture->restore_after_unlock)
		schedule_screencast_restore(capture);
}

static void screencast_portal_capture_hide(void *data)
{
	struct screencast_portal_capture *capture = data;

	if (capture->obs_pw_stream)
		obs_pipewire_stream_hide(capture->obs_pw_stream);
}

static uint32_t screencast_portal_capture_get_width(void *data)
{
	struct screencast_portal_capture *capture = data;

	if (capture->obs_pw_stream)
		return obs_pipewire_stream_get_width(capture->obs_pw_stream);
	else
		return 0;
}

static uint32_t screencast_portal_capture_get_height(void *data)
{
	struct screencast_portal_capture *capture = data;

	if (capture->obs_pw_stream)
		return obs_pipewire_stream_get_height(capture->obs_pw_stream);
	else
		return 0;
}

static void screencast_portal_capture_video_render(void *data, gs_effect_t *effect)
{
	struct screencast_portal_capture *capture = data;

	if (capture->obs_pw_stream)
		obs_pipewire_stream_video_render(capture->obs_pw_stream, effect);
}

void screencast_portal_load(void)
{
	uint32_t available_capture_types = get_available_capture_types();
	bool desktop_capture_available = (available_capture_types & PORTAL_CAPTURE_TYPE_MONITOR) != 0;
	bool window_capture_available = (available_capture_types & PORTAL_CAPTURE_TYPE_WINDOW) != 0;

	if (available_capture_types == 0) {
		blog(LOG_INFO, "[pipewire] No capture sources available");
		return;
	}

	blog(LOG_INFO, "[pipewire] Available capture sources:");
	if (desktop_capture_available)
		blog(LOG_INFO, "[pipewire]     - Monitor source");
	if (window_capture_available)
		blog(LOG_INFO, "[pipewire]     - Window source");

	// Desktop capture
	const struct obs_source_info screencast_portal_desktop_capture_info = {
		.id = "pipewire-desktop-capture-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_CAP_OBSOLETE,
		.get_name = screencast_portal_desktop_capture_get_name,
		.create = screencast_portal_desktop_capture_create,
		.destroy = screencast_portal_capture_destroy,
		.save = screencast_portal_capture_save,
		.get_defaults = screencast_portal_capture_get_defaults,
		.get_properties = screencast_portal_capture_get_properties,
		.update = screencast_portal_capture_update,
		.show = screencast_portal_capture_show,
		.hide = screencast_portal_capture_hide,
		.get_width = screencast_portal_capture_get_width,
		.get_height = screencast_portal_capture_get_height,
		.video_render = screencast_portal_capture_video_render,
		.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,
	};
	if (desktop_capture_available)
		obs_register_source(&screencast_portal_desktop_capture_info);

	// Window capture
	const struct obs_source_info screencast_portal_window_capture_info = {
		.id = "pipewire-window-capture-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_CAP_OBSOLETE,
		.get_name = screencast_portal_window_capture_get_name,
		.create = screencast_portal_window_capture_create,
		.destroy = screencast_portal_capture_destroy,
		.save = screencast_portal_capture_save,
		.get_defaults = screencast_portal_capture_get_defaults,
		.get_properties = screencast_portal_capture_get_properties,
		.update = screencast_portal_capture_update,
		.show = screencast_portal_capture_show,
		.hide = screencast_portal_capture_hide,
		.get_width = screencast_portal_capture_get_width,
		.get_height = screencast_portal_capture_get_height,
		.video_render = screencast_portal_capture_video_render,
		.icon_type = OBS_ICON_TYPE_WINDOW_CAPTURE,
	};
	if (window_capture_available)
		obs_register_source(&screencast_portal_window_capture_info);

	// Screen capture (monitor and window)
	const struct obs_source_info screencast_portal_capture_info = {
		.id = "pipewire-screen-capture-source",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
		.get_name = screencast_portal_desktop_capture_get_name,
		.create = screencast_portal_capture_create,
		.destroy = screencast_portal_capture_destroy,
		.save = screencast_portal_capture_save,
		.get_defaults = screencast_portal_capture_get_defaults,
		.get_properties = screencast_portal_capture_get_properties,
		.update = screencast_portal_capture_update,
		.show = screencast_portal_capture_show,
		.hide = screencast_portal_capture_hide,
		.get_width = screencast_portal_capture_get_width,
		.get_height = screencast_portal_capture_get_height,
		.video_render = screencast_portal_capture_video_render,
		.icon_type = OBS_ICON_TYPE_DESKTOP_CAPTURE,
	};
	obs_register_source(&screencast_portal_capture_info);
}

void screencast_portal_unload(void)
{
	g_clear_object(&screencast_proxy);
}
