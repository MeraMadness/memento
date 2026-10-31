#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "udisks.h"

#define UDISKS_NAME "org.freedesktop.UDisks2"

int udisks_open_device(const char *dev_path, GError **error)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
    GVariantBuilder spec, none, opts;
    GVariant *res, *objs;
    GUnixFDList *fds = NULL;
    gchar *object = NULL;
    gint32 handle = 0;
    int fd = -1;

    if (bus == NULL)
        return -1;
    /* which UDisks object is /dev/sdb? */
    g_variant_builder_init(&spec, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&spec, "{sv}", "path", g_variant_new_string(dev_path));
    g_variant_builder_init(&none, G_VARIANT_TYPE("a{sv}"));
    res = g_dbus_connection_call_sync(bus, UDISKS_NAME, "/org/freedesktop/UDisks2/Manager", UDISKS_NAME ".Manager", "ResolveDevice",
                                      g_variant_new("(a{sv}a{sv})", &spec, &none), G_VARIANT_TYPE("(ao)"), G_DBUS_CALL_FLAGS_NONE, -1,
                                      NULL, error);
    if (res == NULL)
        goto out;
    objs = g_variant_get_child_value(res, 0);
    if (g_variant_n_children(objs) == 0) {
        g_set_error(error, PS2GM_ERROR, PS2GM_ERROR_IO, "The system does not know the device %s", dev_path);
        g_variant_unref(objs);
        g_variant_unref(res);
        goto out;
    }
    g_variant_get_child(objs, 0, "o", &object);
    g_variant_unref(objs);
    g_variant_unref(res);

    /* open it read/write; O_DSYNC like the tools do when they open the disk themselves. The password window may appear here. */
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "flags", g_variant_new_int32(O_DSYNC));
    res = g_dbus_connection_call_with_unix_fd_list_sync(bus, UDISKS_NAME, object, UDISKS_NAME ".Block", "OpenDevice",
                                                        g_variant_new("(sa{sv})", "rw", &opts), G_VARIANT_TYPE("(h)"),
                                                        G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, -1, NULL, &fds, NULL, error);
    if (res == NULL)
        goto out;
    g_variant_get(res, "(h)", &handle);
    fd = g_unix_fd_list_get(fds, handle, error);
    g_variant_unref(res);
out:
    g_free(object);
    g_clear_object(&fds);
    g_object_unref(bus);
    return fd;
}
