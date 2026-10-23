/* Connect page: choose the PS2 drive (or a disk image file) and open it. */
#include <unistd.h>

#include "app.h"

/* ---- connect ----------------------------------------------------------------------------------- */

/* does this folder look like the root of an OPL drive (an SD card in an MX4SIO adapter, a USB stick)? */
static gboolean looks_like_opl_drive(const char *path)
{
    static const char *marks[] = {"DVD", "CD", "POPS", "ART", "CFG", "VMC", "APPS", "CHT", "OPL"};
    guint i;

    for (i = 0; i < G_N_ELEMENTS(marks); i++) {
        char *p = g_build_filename(path, marks[i], NULL);
        gboolean yes = g_file_test(p, G_FILE_TEST_IS_DIR);

        g_free(p);
        if (yes)
            return TRUE;
    }
    return FALSE;
}

/* the mounted drives of the desktop (under /run/media/<user>) that hold an OPL layout */
static void add_mounted_drives(void)
{
    char *bases[2] = {g_build_filename("/run/media", g_get_user_name(), NULL), g_build_filename("/media", g_get_user_name(), NULL)};
    guint b;

    for (b = 0; b < 2; b++) {
        GDir *d = g_dir_open(bases[b], 0, NULL);
        const char *name;

        while (d && (name = g_dir_read_name(d)) != NULL) {
            char *path = g_build_filename(bases[b], name, NULL);

            if (g_file_test(path, G_FILE_TEST_IS_DIR) && looks_like_opl_drive(path)) {
                char *label = g_strdup_printf(_("SD card or USB drive: %s  (%s)"), name, path);

                gtk_string_list_append(A.disk_model, label);
                g_ptr_array_add(A.disk_paths, g_strdup(path));
                g_ptr_array_add(A.disk_names, g_strdup(name));
                g_free(label);
            }
            g_free(path);
        }
        if (d)
            g_dir_close(d);
        g_free(bases[b]);
    }
}

static void fill_disks(void)
{
    gchar *out = NULL;
    gchar **lines;
    GHashTable *busy = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    int i;
    GRegex *kv = g_regex_new("(\\w+)=\"([^\"]*)\"", 0, 0, NULL);
    guint n;

    n = g_list_model_get_n_items(G_LIST_MODEL(A.disk_model));
    if (n > 0)
        gtk_string_list_splice(A.disk_model, 0, n, NULL);
    g_ptr_array_set_size(A.disk_paths, 0);
    g_ptr_array_set_size(A.disk_names, 0);

    if (g_spawn_command_line_sync("lsblk -P -b -o PATH,PKNAME,SIZE,MODEL,TRAN,TYPE,MOUNTPOINT", &out, NULL, NULL, NULL) && out) {
        lines = g_strsplit(out, "\n", -1);
        /* first pass: disks with a mounted partition are in use (system disk) */
        for (i = 0; lines[i]; i++) {
            GMatchInfo *mi = NULL;
            char *pk = NULL, *mp = NULL;

            g_regex_match(kv, lines[i], 0, &mi);
            while (g_match_info_matches(mi)) {
                gchar *k = g_match_info_fetch(mi, 1), *v = g_match_info_fetch(mi, 2);

                if (!strcmp(k, "PKNAME"))
                    pk = g_strdup(v);
                else if (!strcmp(k, "MOUNTPOINT"))
                    mp = g_strdup(v);
                g_free(k);
                g_free(v);
                g_match_info_next(mi, NULL);
            }
            if (pk && *pk && mp && *mp)
                g_hash_table_add(busy, g_strdup(pk));
            g_free(pk);
            g_free(mp);
            g_match_info_free(mi);
        }
        for (i = 0; lines[i]; i++) {
            GMatchInfo *mi = NULL;
            char *path = NULL, *size = NULL, *model = NULL, *tran = NULL, *type = NULL;

            g_regex_match(kv, lines[i], 0, &mi);
            while (g_match_info_matches(mi)) {
                gchar *k = g_match_info_fetch(mi, 1), *v = g_match_info_fetch(mi, 2);

                if (!strcmp(k, "PATH"))
                    path = g_strdup(v);
                else if (!strcmp(k, "SIZE"))
                    size = g_strdup(v);
                else if (!strcmp(k, "MODEL"))
                    model = g_strdup(v);
                else if (!strcmp(k, "TRAN"))
                    tran = g_strdup(v);
                else if (!strcmp(k, "TYPE"))
                    type = g_strdup(v);
                g_free(k);
                g_free(v);
                g_match_info_next(mi, NULL);
            }
            if (path && type && !strcmp(type, "disk") && !g_str_has_prefix(path, "/dev/zram") &&
                !g_hash_table_contains(busy, path + 5)) {
                char *gb = g_strdup_printf("%.0f GB", g_ascii_strtod(size ? size : "0", NULL) / 1e9);
                char *label = g_strdup_printf("%s  (%s%s%s)  %s", model && *model ? model : _("Disk"), gb, tran && *tran ? ", " : "",
                                              tran ? tran : "", path);

                gtk_string_list_append(A.disk_model, label);
                g_ptr_array_add(A.disk_paths, g_strdup(path));
                g_ptr_array_add(A.disk_names, g_strdup(model && *model ? model : _("Disk")));
                g_free(label);
                g_free(gb);
            }
            g_free(path);
            g_free(size);
            g_free(model);
            g_free(tran);
            g_free(type);
            g_match_info_free(mi);
        }
        g_strfreev(lines);
    }
    g_free(out);
    g_hash_table_destroy(busy);
    g_regex_unref(kv);
    add_mounted_drives();
    adw_action_row_set_subtitle(ADW_ACTION_ROW(A.disk_row), A.disk_paths->len ? NULL : _("No disks found. Plug in the PS2 drive, then press Refresh"));
    if (A.image_path) {
        gboolean folder = g_file_test(A.image_path, G_FILE_TEST_IS_DIR); /* a folder chosen by hand, or a disk image file */
        char *label = g_strdup_printf(folder ? _("Folder: %s") : _("Disk image: %s"), A.image_path), *base = g_path_get_basename(A.image_path);

        gtk_string_list_append(A.disk_model, label);
        g_ptr_array_add(A.disk_paths, g_strdup(A.image_path));
        g_ptr_array_add(A.disk_names, folder ? g_strdup(base) : g_strdup(_("Disk image")));
        g_free(base);
        g_free(label);
        adw_combo_row_set_selected(A.disk_row, A.disk_paths->len - 1);
    }
}

/* the rows of facts about the connected disk */
static GtkWidget *details_group;
static GtkWidget *detail_rows[7];

static GtkWidget *fact_row(const char *title)
{
    GtkWidget *row = adw_action_row_new(), *val = gtk_label_new("");

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    gtk_widget_add_css_class(val, "detail-val");
    gtk_label_set_ellipsize(GTK_LABEL(val), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(val), 48);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), val);
    g_object_set_data(G_OBJECT(row), "value", val);
    return row;
}

static void set_fact(int i, const char *text)
{
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(detail_rows[i]), "value")), text);
}

static void fill_disk_details(void)
{
    char *t, *u;

    if (details_group == NULL || A.lib == NULL)
        return;
    set_fact(0, A.device_name && *A.device_name ? A.device_name : _("PS2 disk"));
    set_fact(1, A.device ? A.device : "");
    t = mb_text(A.lib->total_mb);
    set_fact(2, t);
    g_free(t);
    t = mb_text(A.lib->used_mb);
    u = mb_text(A.lib->free_mb);
    {
        char *both = g_strdup_printf(_("%s used, %s free"), t, u);

        set_fact(3, both);
        g_free(both);
    }
    g_free(t);
    g_free(u);
    t = g_strdup_printf("%u", A.lib->games->len);
    set_fact(4, t);
    g_free(t);
    t = g_strdup_printf("%u", A.lib->vcds->len);
    set_fact(5, t);
    g_free(t);
    t = g_strdup_printf("%u", A.lib->apps ? A.lib->apps->len : 0);
    set_fact(6, t);
    g_free(t);
}

/* the Devices page: the way to connect, or (connected) the disk that is open */
void update_devices_page(void)
{
    gboolean on = A.backend != NULL && A.lib != NULL;

    gtk_widget_set_visible(A.conn_connect_box, !on);
    gtk_widget_set_visible(A.conn_connected_box, on);
    if (on) {
        char *used = mb_text(A.lib->used_mb), *total = mb_text(A.lib->total_mb), *free_t = mb_text(A.lib->free_mb), *title, *desc;
        guint n = A.lib->games->len + A.lib->vcds->len;

        title = g_strdup_printf(_("Connected: %s"), A.device_name && *A.device_name ? A.device_name : _("PS2 disk"));
        desc = g_strdup_printf(g_dngettext(GETTEXT_PACKAGE, "%s used of %s, %s free. %u game on the disk.", "%s used of %s, %s free. %u games on the disk.", n),
                               used, total, free_t, n);
        adw_status_page_set_title(ADW_STATUS_PAGE(A.conn_status), title);
        adw_status_page_set_description(ADW_STATUS_PAGE(A.conn_status), desc);
        fill_disk_details();
        g_free(used);
        g_free(total);
        g_free(free_t);
        g_free(title);
        g_free(desc);
    } else {
        adw_status_page_set_title(ADW_STATUS_PAGE(A.conn_status), _("Connect your PS2 drive"));
        adw_status_page_set_description(ADW_STATUS_PAGE(A.conn_status),
                                        _("Plug the PS2 hard drive or SSD into this computer, or an SD card or USB drive with your games (MX4SIO, USB), choose it below "
                                        "and press Connect. A hard drive asks for your administrator password once; a card or USB drive does not."));
    }
}

void set_connected(gboolean on)
{
    tabs_set_connected(on);
    gtk_widget_set_visible(A.refresh_btn, on);
    update_devices_page();
    update_topbar();
    adw_view_stack_set_visible_child_name(A.stack, on ? "library" : "connect");
    update_iso_widgets();
    rebuild_disc_rows();
}

gboolean job_connect(gpointer data, Ps2gmProgress cb, gpointer pd, GPtrArray *notes, GError **error)
{
    const char *device = data;
    gboolean root = access(device, R_OK | W_OK) != 0;

    (void)notes;
    cb(0, 1, root ? _("Waiting for the administrator password") : _("Opening the disk"), pd);
    A.backend = backend_start(device, root, NULL, error);
    if (A.backend == NULL)
        return FALSE;
    g_free(A.device);
    A.device = g_strdup(device);
    if (!load_library(cb, pd, error)) {
        backend_stop(A.backend);
        A.backend = NULL;
        return FALSE;
    }
    ops_free(A.ops);
    A.ops = ops_new(A.backend, NULL);
    return TRUE;
}

void after_connect(gboolean ok, gpointer data)
{
    (void)data;
    if (ok) {
        set_connected(TRUE);
        rebuild_library();
    }
}

static void on_connect(GtkButton *b, gpointer d)
{
    guint sel = adw_combo_row_get_selected(A.disk_row);

    (void)b;
    (void)d;
    if (sel == GTK_INVALID_LIST_POSITION || sel >= A.disk_paths->len) {
        show_error(_("No disk selected"), _("Plug in the PS2 hard drive (or SSD) and press Refresh, or choose a disk image file."));
        return;
    }
    g_free(A.device_name);
    A.device_name = g_strdup(sel < A.disk_names->len ? g_ptr_array_index(A.disk_names, sel) : "");
    run_job(_("Connecting to the PS2 disk"), job_connect, g_strdup(g_ptr_array_index(A.disk_paths, sel)), g_free, after_connect);
}

void on_disconnect(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (transfers_busy()) {
        toast(_("Wait for the transfers to finish first"));
        return;
    }
    backend_stop(A.backend);
    A.backend = NULL;
    ops_free(A.ops);
    A.ops = NULL;
    library_free(A.lib);
    A.lib = NULL;
    set_connected(FALSE);
}

static void on_rescan(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    fill_disks();
}

static void on_image_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *f = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);

    (void)data;
    if (f == NULL)
        return;
    g_free(A.image_path);
    A.image_path = g_file_get_path(f);
    g_object_unref(f);
    fill_disks();
}

static void on_folder_chosen(GObject *src, GAsyncResult *res, gpointer data)
{
    GFile *f = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, NULL);

    (void)data;
    if (f == NULL)
        return;
    g_free(A.image_path);
    A.image_path = g_file_get_path(f);
    g_object_unref(f);
    fill_disks();
}

static void on_choose_folder(GtkButton *b, gpointer d)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();

    (void)b;
    (void)d;
    gtk_file_dialog_set_title(dlg, _("Choose the folder of an SD card or USB drive (the one with DVD, CD, ART...)"));
    gtk_file_dialog_select_folder(dlg, GTK_WINDOW(A.win), NULL, on_folder_chosen, NULL);
    g_object_unref(dlg);
}

static void on_choose_image(GtkButton *b, gpointer d)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();

    (void)b;
    (void)d;
    gtk_file_dialog_set_title(dlg, _("Choose a PS2 disk image file"));
    gtk_file_dialog_open(dlg, GTK_WINDOW(A.win), NULL, on_image_chosen, NULL);
    g_object_unref(dlg);
}

GtkWidget *build_connect_page(void)
{
    GtkWidget *status = adw_status_page_new(), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12), *group = adw_preferences_group_new();
    GtkWidget *go = gtk_button_new_with_label(_("Connect")), *row2;

    {
        GdkPaintable *art = pixel_art("ps2-console.png", 200);

        if (art != NULL) {
            adw_status_page_set_paintable(ADW_STATUS_PAGE(status), art);
            g_object_unref(art);
        } else
            adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), "drive-harddisk-symbolic");
    }
    adw_status_page_set_title(ADW_STATUS_PAGE(status), _("Connect your PS2 drive"));
    adw_status_page_set_description(ADW_STATUS_PAGE(status),
                                    _("Plug the PS2 hard drive or SSD into this computer, or an SD card or USB drive with your games (MX4SIO, USB), choose it below "
                                    "and press Connect. A hard drive asks for your administrator password once; a card or USB drive does not."));
    A.disk_model = gtk_string_list_new(NULL);
    A.disk_paths = g_ptr_array_new_with_free_func(g_free);
    A.disk_names = g_ptr_array_new_with_free_func(g_free);
    A.disk_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(A.disk_row), _("Disk"));
    adw_combo_row_set_model(A.disk_row, G_LIST_MODEL(A.disk_model));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), GTK_WIDGET(A.disk_row));
    row2 = button_row(_("Not listed?"), _("Look again, or choose the folder of an SD card or USB drive, or a disk image file (for tests and backups)"), _("Refresh"), G_CALLBACK(on_rescan), NULL);
    {
        GtkWidget *bf = gtk_button_new_with_label(_("Choose folder..."));

        gtk_widget_set_valign(bf, GTK_ALIGN_CENTER);
        g_signal_connect(bf, "clicked", G_CALLBACK(on_choose_folder), NULL);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row2), bf);
    }
    {
        GtkWidget *b = gtk_button_new_with_label(_("Choose image..."));

        gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
        g_signal_connect(b, "clicked", G_CALLBACK(on_choose_image), NULL);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row2), b);
    }
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row2);
    gtk_widget_add_css_class(go, "suggested-action");
    gtk_widget_add_css_class(go, "pill");
    gtk_widget_set_halign(go, GTK_ALIGN_CENTER);
    g_signal_connect(go, "clicked", G_CALLBACK(on_connect), NULL);
    gtk_box_append(GTK_BOX(box), group);
    gtk_box_append(GTK_BOX(box), go);
    gtk_widget_set_size_request(box, 460, -1);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    A.conn_connect_box = box;

    /* once connected the page shows the disk instead, with the way out */
    {
        GtkWidget *connected = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12), *again = gtk_button_new_with_label(_("Read the disk again"));

        A.disconnect_btn = gtk_button_new_with_label(_("Disconnect"));
        gtk_widget_add_css_class(A.disconnect_btn, "pill");
        gtk_widget_add_css_class(A.disconnect_btn, "btn-danger");
        gtk_widget_set_halign(A.disconnect_btn, GTK_ALIGN_CENTER);
        g_signal_connect(A.disconnect_btn, "clicked", G_CALLBACK(on_disconnect), NULL);
        gtk_widget_add_css_class(again, "pill");
        gtk_widget_add_css_class(again, "btn-secondary");
        gtk_widget_set_halign(again, GTK_ALIGN_CENTER);
        g_signal_connect(again, "clicked", G_CALLBACK(on_refresh), NULL);
        {
            static const char *keys[7] = {N_("Disk"), N_("Device"), N_("Capacity"), N_("Space"), N_("PS2 games"), N_("PS1 games"), N_("Programs")};
            guint k;

            details_group = adw_preferences_group_new();
            for (k = 0; k < 7; k++) {
                detail_rows[k] = fact_row(_(keys[k]));
                adw_preferences_group_add(ADW_PREFERENCES_GROUP(details_group), detail_rows[k]);
            }
            gtk_box_append(GTK_BOX(connected), details_group);
        }
        gtk_box_append(GTK_BOX(connected), again);
        gtk_box_append(GTK_BOX(connected), A.disconnect_btn);
        gtk_widget_set_size_request(connected, 460, -1);
        gtk_widget_set_halign(connected, GTK_ALIGN_CENTER);
        gtk_widget_set_visible(connected, FALSE);
        A.conn_connected_box = connected;
    }
    {
        GtkWidget *both = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        gtk_box_append(GTK_BOX(both), A.conn_connect_box);
        gtk_box_append(GTK_BOX(both), A.conn_connected_box);
        adw_status_page_set_child(ADW_STATUS_PAGE(status), both);
    }
    A.conn_status = status;
    fill_disks();
    return status;
}
