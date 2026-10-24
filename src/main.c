#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <unistd.h>
#include <libayatana-appindicator/app-indicator.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#include "app_config.h"
#include <sensors/sensors.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static GtkApplication *application;
static GtkWidget *window, *status, *total_label, *details, *enabled, *watts, *temperature;
static GtkStatusIcon *tray;
static AppIndicator *indicator;
static GtkWidget *tray_summary;
static char *indicator_icon_dir;
static char *indicator_icon_paths[3];

static gboolean tray_available(void)
{
    if (indicator) {
        gboolean connected = FALSE;
        g_object_get(indicator, "connected", &connected, NULL);
        return connected;
    }
    return tray && gtk_status_icon_is_embedded(tray);
}
static GdkPixbuf *tray_icons[3];
static guint timer;
static gboolean sensors_ready;
static unsigned previous_alarm;
static char *config_path;
static gboolean start_in_tray, waiting_for_tray, changing_autostart;
static char *autostart_path;
static guint startup_timer;

static gint local_options(GApplication *app, GVariantDict *options, gpointer unused)
{
    (void)app; (void)unused;
    start_in_tray = g_variant_dict_contains(options, "start-in-tray");
    return -1;
}

/* Desktop Entry Exec quoting is different from shell quoting. */
static char *desktop_quote(const char *path)
{
    GString *quoted = g_string_new("\"");
    for (const char *p = path; *p; ++p) {
        if (*p == '%') g_string_append(quoted, "%%");
        else {
            if (*p == '\\' || *p == '"' || *p == '$' || *p == '`')
                g_string_append_c(quoted, '\\');
            g_string_append_c(quoted, *p);
        }
    }
    g_string_append_c(quoted, '"');
    return g_string_free(quoted, FALSE);
}

static void autostart_changed(GtkToggleButton *button, gpointer unused)
{
    (void)unused;
    if (changing_autostart) return;
    gboolean on = gtk_toggle_button_get_active(button);
    GError *error = NULL;
    gboolean success = FALSE;
    if (on) {
        const char *appimage = g_getenv("APPIMAGE");
        char *executable = appimage && g_path_is_absolute(appimage) ?
            g_strdup(appimage) : g_file_read_link("/proc/self/exe", &error);
        if (executable && g_file_test(executable, G_FILE_TEST_IS_EXECUTABLE)) {
            char *quoted = desktop_quote(executable);
            char *exec = g_strconcat(quoted, " --start-in-tray", NULL);
            GKeyFile *entry = g_key_file_new();
            g_key_file_set_string(entry, "Desktop Entry", "Type", "Application");
            g_key_file_set_string(entry, "Desktop Entry", "Name", APP_TITLE);
            g_key_file_set_string(entry, "Desktop Entry", "Exec", exec);
            g_key_file_set_string(entry, "Desktop Entry", "Icon", "mjd-psu-monitor");
            g_key_file_set_boolean(entry, "Desktop Entry", "Terminal", FALSE);
            g_key_file_set_boolean(entry, "Desktop Entry", "X-GNOME-Autostart-enabled", TRUE);
            char *dir = g_path_get_dirname(autostart_path);
            if (g_mkdir_with_parents(dir, 0700) == 0)
                success = g_key_file_save_to_file(entry, autostart_path, &error);
            g_free(dir); g_key_file_unref(entry); g_free(exec); g_free(quoted);
        }
        g_free(executable);
    } else {
        success = !g_file_test(autostart_path, G_FILE_TEST_EXISTS) || g_unlink(autostart_path) == 0;
    }
    if (!success) {
        changing_autostart = TRUE;
        gtk_toggle_button_set_active(button, !on);
        changing_autostart = FALSE;
        GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
            GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
            "Could not update login startup: %s", error ? error->message :
            "check configuration folder permissions and executable location");
        gtk_dialog_run(GTK_DIALOG(dialog)); gtk_widget_destroy(dialog);
    }
    g_clear_error(&error);
}

static gboolean finish_tray_start(gpointer unused)
{
    (void)unused;
    static unsigned attempts;
    if (tray_available()) {
        waiting_for_tray = FALSE;
        startup_timer = 0;
        return G_SOURCE_REMOVE;
    }
    if (++attempts < 20) return G_SOURCE_CONTINUE;
    waiting_for_tray = FALSE;
    startup_timer = 0;
    gtk_widget_show_all(window);
    return G_SOURCE_REMOVE;
}


/* Embed the packaging icon so source builds also work without installation. */
static GdkPixbuf *load_app_icon(const char *badge)
{
    const char *end = strstr(APP_ICON_SVG, "</svg>");
    if (!end) return NULL;
    char *svg = g_strdup_printf("%.*s%s</svg>", (int)(end - APP_ICON_SVG), APP_ICON_SVG, badge);
    GError *error = NULL;
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new_with_type("svg", &error);
    GdkPixbuf *result = NULL;
    if (loader) {
        gdk_pixbuf_loader_set_size(loader, 48, 48);
        gboolean written = gdk_pixbuf_loader_write(loader, (const guchar *)svg, strlen(svg), &error);
        if (written && gdk_pixbuf_loader_close(loader, &error)) {
            GdkPixbuf *pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
            if (pixbuf) result = g_object_ref(pixbuf);
        } else if (!written) {
            gdk_pixbuf_loader_close(loader, NULL);
        }
        g_object_unref(loader);
    }
    if (error) g_warning("Cannot load app icon: %s", error->message);
    g_clear_error(&error);
    g_free(svg);
    return result;
}

static void set_tray_icon(unsigned state)
{
    if (indicator) {
        const char *icons[] = {"mjd-psu-monitor", "dialog-warning", "dialog-error"};
        app_indicator_set_icon_full(indicator,
            indicator_icon_paths[state] ? indicator_icon_paths[state] : icons[state], APP_TITLE);
        return;
    }
    if (tray_icons[state]) gtk_status_icon_set_from_pixbuf(tray, tray_icons[state]);
    else gtk_status_icon_set_from_icon_name(tray, state ? "dialog-warning" : "mjd-psu-monitor");
}

static void notify(const char *message)
{
    GNotification *n = g_notification_new(APP_TITLE " alarm");
    g_notification_set_body(n, message);
    g_notification_set_priority(n, G_NOTIFICATION_PRIORITY_URGENT);
    g_application_send_notification(G_APPLICATION(application), "psu-alarm", n);
    g_object_unref(n);
    if (gtk_widget_get_realized(window)) gdk_window_beep(gtk_widget_get_window(window));
}

static void save_settings(GtkWidget *widget, gpointer unused)
{
    (void)widget; (void)unused;
    GKeyFile *key = g_key_file_new();
    g_key_file_set_boolean(key, "alarms", "enabled", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enabled)));
    g_key_file_set_double(key, "alarms", "watts", gtk_spin_button_get_value(GTK_SPIN_BUTTON(watts)));
    g_key_file_set_double(key, "alarms", "temperature", gtk_spin_button_get_value(GTK_SPIN_BUTTON(temperature)));
    gsize size;
    char *data = g_key_file_to_data(key, &size, NULL);
    char *dir = g_path_get_dirname(config_path);
    GError *error = NULL;
    if (g_mkdir_with_parents(dir, 0700) != 0 || !g_file_set_contents(config_path, data, (gssize)size, &error))
        g_warning("Cannot save alarm settings: %s", error ? error->message : "cannot create configuration directory");
    g_clear_error(&error);
    g_free(dir); g_free(data); g_key_file_unref(key);
    previous_alarm = 0;
}

static gboolean refresh(gpointer unused)
{
    (void)unused;
    /* Reinitialize to discover devices after unplug/replug. Never retain chip pointers. */
    if (sensors_ready) sensors_cleanup();
    sensors_ready = sensors_init(NULL) == 0;
    GString *text = g_string_new("");
    int index = 0, devices = 0, successful = 0, failed = 0;
    gboolean have_total = FALSE, hot = FALSE;
    double total = 0;
    const sensors_chip_name *chip;
    while (sensors_ready && (chip = sensors_get_detected_chips(NULL, &index))) {
        if (!chip->prefix || strcmp(chip->prefix, "corsairpsu") != 0) continue;
        ++devices;
        char name[256];
        if (sensors_snprintf_chip_name(name, sizeof name, chip) < 0)
            g_strlcpy(name, "Corsair PSU", sizeof name);
        g_string_append_printf(text, "%s\n", name);
        int fi = 0;
        const sensors_feature *feature;
        while ((feature = sensors_get_features(chip, &fi))) {
            sensors_subfeature_type input;
            const char *unit;
            switch (feature->type) {
            case SENSORS_FEATURE_IN: input = SENSORS_SUBFEATURE_IN_INPUT; unit = "V"; break;
            case SENSORS_FEATURE_FAN: input = SENSORS_SUBFEATURE_FAN_INPUT; unit = "RPM"; break;
            case SENSORS_FEATURE_TEMP: input = SENSORS_SUBFEATURE_TEMP_INPUT; unit = "°C"; break;
            case SENSORS_FEATURE_POWER: input = SENSORS_SUBFEATURE_POWER_INPUT; unit = "W"; break;
            case SENSORS_FEATURE_CURR: input = SENSORS_SUBFEATURE_CURR_INPUT; unit = "A"; break;
            default: continue;
            }
            const sensors_subfeature *sub = sensors_get_subfeature(chip, feature, input);
            if (!sub || !(sub->flags & SENSORS_MODE_R)) continue;
            char *allocated = sensors_get_label(chip, feature);
            const char *label = allocated ? allocated : feature->name;
            double value;
            if (sensors_get_value(chip, sub->number, &value) < 0 || !isfinite(value)) {
                g_string_append_printf(text, "%-22s Unavailable\n", label);
                ++failed;
            } else {
                ++successful;
                g_string_append_printf(text, "%-22s %8.2f %s\n", label, value, unit);
                /* corsair-psu power1 is the reported total, never sum the rails. */
                if (feature->type == SENSORS_FEATURE_POWER && strcmp(feature->name, "power1") == 0) {
                    total += value; have_total = TRUE;
                }
                if (feature->type == SENSORS_FEATURE_TEMP &&
                    value >= gtk_spin_button_get_value(GTK_SPIN_BUTTON(temperature)) - ((previous_alarm & 4) ? 2.0 : 0.0)) hot = TRUE;
            }
            free(allocated);
        }
        g_string_append_c(text, '\n');
    }
    const char *connection = !sensors_ready ? "Sensor initialization failed." :
        !devices ? "PSU not detected. Check USB cable, PSU power and corsair-psu driver." :
        !successful ? "PSU detected but unreadable. Check USB connection and driver." :
        failed ? "PSU detected • some readings unavailable" : "PSU connected • live readings";
    unsigned alarm = 0;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enabled))) {
        if (!devices || !successful || !sensors_ready) alarm |= 1;
        if (have_total && total >= gtk_spin_button_get_value(GTK_SPIN_BUTTON(watts)) - ((previous_alarm & 2) ? 5.0 : 0.0)) alarm |= 2;
        if (hot) alarm |= 4;
        if (failed) alarm |= 8;
    }
    GString *message = g_string_new(connection);
    if (alarm & 2) g_string_append(message, "\nALARM: total watts limit reached.");
    if (alarm & 4) g_string_append(message, "\nALARM: temperature limit reached.");
    if (alarm & 1) g_string_append(message, "\nALARM: PSU telemetry lost.");
    if (alarm & 8) g_string_append(message, "\nALARM: sensor read failure.");
    gtk_label_set_text(GTK_LABEL(status), message->str);
    if (alarm & ~previous_alarm) notify(message->str);
    if (!alarm && previous_alarm) g_application_withdraw_notification(G_APPLICATION(application), "psu-alarm");
    previous_alarm = alarm;
    char summary[160];
    if (have_total) g_snprintf(summary, sizeof summary, "Total watts: %.2f W%s", total, devices > 1 ? " (all detected PSUs)" : "");
    else g_strlcpy(summary, "Total watts: unavailable", sizeof summary);
    char *markup = g_markup_printf_escaped("<big><b>%s</b></big>", summary);
    gtk_label_set_markup(GTK_LABEL(total_label), markup);
    g_free(markup);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(details)), text->str, -1);
    set_tray_icon(alarm ? 1 : (!devices || !successful ? 2 : 0));
    char *tooltip = g_strdup_printf(APP_TITLE "\n%s\n%s", summary, message->str);
    if (tray) gtk_status_icon_set_tooltip_text(tray, tooltip);
    if (tray_summary) gtk_menu_item_set_label(GTK_MENU_ITEM(tray_summary), tooltip);
    g_free(tooltip);
    if (!waiting_for_tray && !gtk_widget_get_visible(window) && !tray_available())
        gtk_widget_show_all(window);
    g_string_free(message, TRUE); g_string_free(text, TRUE);
    return G_SOURCE_CONTINUE;
}

static void show_window(GtkWidget *widget, gpointer unused)
{
    (void)widget; (void)unused;
    gtk_widget_show_all(window);
    gtk_window_present(GTK_WINDOW(window));
}
static void quit_app(GtkWidget *widget, gpointer unused)
{
    (void)widget; (void)unused;
    g_application_quit(G_APPLICATION(application));
}
static gboolean close_window(GtkWidget *widget, GdkEvent *event, gpointer unused)
{
    (void)event; (void)unused;
    if (tray_available()) gtk_widget_hide(widget);
    else quit_app(NULL, NULL);
    return TRUE;
}
static void popup(GtkStatusIcon *icon, guint button, guint time, gpointer menu)
{
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, gtk_status_icon_position_menu, icon, button, time);
}
static void show_about(GtkWidget *widget, gpointer unused)
{
    (void)widget; (void)unused;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("About " APP_TITLE,
        GTK_WINDOW(window), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 560, 520);
    gtk_window_set_resizable(GTK_WINDOW(dialog), TRUE);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 16);
    gtk_box_set_spacing(GTK_BOX(content), 12);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<big><b>" APP_TITLE "</b></big>");
    gtk_box_pack_start(GTK_BOX(content), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), gtk_label_new(APP_SUBTITLE), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), gtk_label_new("Version " APP_VERSION), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), gtk_label_new("© mjd 2026"), FALSE, FALSE, 0);
    GtkWidget *disclaimer = gtk_label_new(
        APP_NOTICE);
    gtk_label_set_line_wrap(GTK_LABEL(disclaimer), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(disclaimer), 60);
    gtk_box_pack_start(GTK_BOX(content), disclaimer, FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new("MIT License");
    gtk_box_pack_start(GTK_BOX(content), frame, TRUE, TRUE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 240);
    gtk_container_add(GTK_CONTAINER(frame), scroll);
    GtkWidget *license = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(license), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(license), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(license), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(license), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(license), 10);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(license)), APP_LICENSE_TEXT, -1);
    gtk_container_add(GTK_CONTAINER(scroll), license);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void activate(GtkApplication *app, gpointer unused)
{
    (void)unused;
    if (window) return;
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), APP_TITLE);
    gtk_window_set_default_icon_name("mjd-psu-monitor");
    gtk_window_set_default_size(GTK_WINDOW(window), 570, 720);
    g_signal_connect(window, "delete-event", G_CALLBACK(close_window), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 18);
    gtk_container_add(GTK_CONTAINER(window), box);
    GtkWidget *subtitle = gtk_label_new(APP_SUBTITLE);
    gtk_box_pack_start(GTK_BOX(box), subtitle, FALSE, FALSE, 0);
    total_label = gtk_label_new("Total watts: unavailable");
    gtk_box_pack_start(GTK_BOX(box), total_label, FALSE, FALSE, 0);
    status = gtk_label_new("Detecting PSU…");
    gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
    gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    details = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(details), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(details), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(details), TRUE);
    gtk_container_add(GTK_CONTAINER(scroll), details);
    enabled = gtk_check_button_new_with_label("Enable alarms (power, temperature and connection)");
    gtk_box_pack_start(GTK_BOX(box), enabled, FALSE, FALSE, 0);
    GtkWidget *limits = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(limits), 12);
    gtk_grid_set_row_spacing(GTK_GRID(limits), 8);
    gtk_box_pack_start(GTK_BOX(box), limits, FALSE, FALSE, 0);
    watts = gtk_spin_button_new_with_range(1, 10000, 10);
    temperature = gtk_spin_button_new_with_range(1, 150, 1);
    gtk_grid_attach(GTK_GRID(limits), gtk_label_new("Total power limit (W)"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(limits), watts, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(limits), gtk_label_new("Any PSU temperature limit (°C)"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(limits), temperature, 1, 1, 1, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(watts), 800);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(temperature), 60);
    GKeyFile *key = g_key_file_new();
    if (g_key_file_load_from_file(key, config_path, G_KEY_FILE_NONE, NULL)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled), g_key_file_get_boolean(key, "alarms", "enabled", NULL));
        double w = g_key_file_get_double(key, "alarms", "watts", NULL);
        double t = g_key_file_get_double(key, "alarms", "temperature", NULL);
        if (isfinite(w) && w >= 1 && w <= 10000) gtk_spin_button_set_value(GTK_SPIN_BUTTON(watts), w);
        if (isfinite(t) && t >= 1 && t <= 150) gtk_spin_button_set_value(GTK_SPIN_BUTTON(temperature), t);
    }
    g_key_file_unref(key);
    g_signal_connect(enabled, "toggled", G_CALLBACK(save_settings), NULL);
    g_signal_connect(watts, "value-changed", G_CALLBACK(save_settings), NULL);
    g_signal_connect(temperature, "value-changed", G_CALLBACK(save_settings), NULL);
    GtkWidget *autostart = gtk_check_button_new_with_label("Start in system tray when I log in");
    gtk_widget_set_tooltip_text(autostart,
        "Starts after desktop login. Keep the executable or AppImage at its current location.");
    GKeyFile *startup_entry = g_key_file_new();
    gboolean startup_enabled = g_key_file_load_from_file(startup_entry, autostart_path, G_KEY_FILE_NONE, NULL) &&
        !g_key_file_get_boolean(startup_entry, "Desktop Entry", "Hidden", NULL);
    if (g_key_file_has_key(startup_entry, "Desktop Entry", "X-GNOME-Autostart-enabled", NULL))
        startup_enabled = startup_enabled && g_key_file_get_boolean(startup_entry, "Desktop Entry", "X-GNOME-Autostart-enabled", NULL);
    g_key_file_unref(startup_entry);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(autostart), startup_enabled);
    g_signal_connect(autostart, "toggled", G_CALLBACK(autostart_changed), NULL);
    gtk_box_pack_start(GTK_BOX(box), autostart, FALSE, FALSE, 0);
    GtkWidget *about = gtk_button_new_with_label("About");
    g_signal_connect(about, "clicked", G_CALLBACK(show_about), NULL);
    gtk_box_pack_start(GTK_BOX(box), about, FALSE, FALSE, 0);
    GtkWidget *quit = gtk_button_new_with_label("Quit");
    g_signal_connect(quit, "clicked", G_CALLBACK(quit_app), NULL);
    gtk_box_pack_start(GTK_BOX(box), quit, FALSE, FALSE, 0);
    tray_icons[0] = load_app_icon("");
    tray_icons[1] = load_app_icon("<circle cx='99' cy='99' r='25' fill='#ffcf40' stroke='#19384b' stroke-width='4'/><path d='M99 84v18' stroke='#19384b' stroke-width='7'/><circle cx='99' cy='111' r='4' fill='#19384b'/>");
    tray_icons[2] = load_app_icon("<circle cx='99' cy='99' r='25' fill='#e55353' stroke='#19384b' stroke-width='4'/><path d='M88 88l22 22m0-22l-22 22' stroke='white' stroke-width='6'/>");
    if (tray_icons[0]) gtk_window_set_icon(GTK_WINDOW(window), tray_icons[0]);
    GtkWidget *menu = gtk_menu_new();
    tray_summary = gtk_menu_item_new_with_label(APP_TITLE);
    gtk_widget_set_sensitive(tray_summary, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), tray_summary);
    GtkWidget *open_item = gtk_menu_item_new_with_label("Show monitor");
    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), open_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);
    gtk_widget_show_all(menu);
    g_signal_connect(open_item, "activate", G_CALLBACK(show_window), NULL);
    g_signal_connect(quit_item, "activate", G_CALLBACK(quit_app), NULL);
    gboolean wayland = FALSE;
#ifdef GDK_WINDOWING_WAYLAND
    wayland = GDK_IS_WAYLAND_DISPLAY(gdk_display_get_default());
#endif
    if (wayland) {
        indicator_icon_dir = g_dir_make_tmp("mjd-psu-icons-XXXXXX", NULL);
        if (indicator_icon_dir) {
            for (unsigned i = 0; i < G_N_ELEMENTS(tray_icons); ++i) {
                char *name = g_strdup_printf("mjd-psu-%u.png", i);
                char *path = g_build_filename(indicator_icon_dir, name, NULL);
                if (tray_icons[i] && gdk_pixbuf_save(tray_icons[i], path, "png", NULL, NULL))
                    indicator_icon_paths[i] = path;
                else g_free(path);
                g_free(name);
            }
        }
        indicator = app_indicator_new("mjd-psu-monitor", "mjd-psu-monitor",
            APP_INDICATOR_CATEGORY_HARDWARE);
        app_indicator_set_title(indicator, APP_TITLE);
        app_indicator_set_menu(indicator, GTK_MENU(menu));
        set_tray_icon(0);
        app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    } else {
        tray = gtk_status_icon_new();
        set_tray_icon(0);
        gtk_status_icon_set_visible(tray, TRUE);
        g_signal_connect(tray, "activate", G_CALLBACK(show_window), NULL);
        g_signal_connect(tray, "popup-menu", G_CALLBACK(popup), menu);
        g_object_set_data_full(G_OBJECT(tray), "menu", g_object_ref_sink(menu), g_object_unref);
    }
    waiting_for_tray = start_in_tray;
    gtk_widget_show_all(box);
    if (!start_in_tray) gtk_widget_show(window);
    else startup_timer = g_timeout_add(250, finish_tray_start, NULL);
    refresh(NULL);
    timer = g_timeout_add_seconds(2, refresh, NULL);
}
int main(int argc, char **argv)
{
    application = gtk_application_new("io.github.mjd.PsuMonitor", G_APPLICATION_FLAGS_NONE);
    g_application_add_main_option(G_APPLICATION(application), "start-in-tray", 0,
        G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, "Start hidden in the system tray", NULL);
    g_signal_connect(application, "handle-local-options", G_CALLBACK(local_options), NULL);
    GError *error = NULL;
    if (!g_application_register(G_APPLICATION(application), NULL, &error)) {
        g_printerr("Cannot register application: %s\n", error->message);
        g_clear_error(&error); g_object_unref(application); return EXIT_FAILURE;
    }
    if (g_application_get_is_remote(G_APPLICATION(application))) {
        g_print(APP_TITLE " is already running.\n");
        g_object_unref(application); return EXIT_SUCCESS;
    }
    config_path = g_build_filename(g_get_user_config_dir(), "mjd-psu-monitor", "settings.ini", NULL);
    autostart_path = g_build_filename(g_get_user_config_dir(), "autostart", "mjd-psu-monitor.desktop", NULL);
    g_signal_connect(application, "activate", G_CALLBACK(activate), NULL);
    int result = g_application_run(G_APPLICATION(application), argc, argv);
    if (timer) g_source_remove(timer);
    if (startup_timer) g_source_remove(startup_timer);
    if (tray) g_object_unref(tray);
    if (indicator) g_object_unref(indicator);
    for (unsigned i = 0; i < G_N_ELEMENTS(indicator_icon_paths); ++i) {
        if (indicator_icon_paths[i]) g_unlink(indicator_icon_paths[i]);
        g_free(indicator_icon_paths[i]);
    }
    if (indicator_icon_dir) g_rmdir(indicator_icon_dir);
    g_free(indicator_icon_dir);
    for (unsigned i = 0; i < G_N_ELEMENTS(tray_icons); ++i) g_clear_object(&tray_icons[i]);
    if (sensors_ready) sensors_cleanup();
    g_free(config_path);
    g_free(autostart_path);
    g_object_unref(application);
    return result;
}
