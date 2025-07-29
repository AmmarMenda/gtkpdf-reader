#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- Global State ---------- */
static fz_context   *ctx            = NULL;
static fz_document  *doc            = NULL;
static char         *current_path   = NULL;
static int           page_count     = 0;
static int           current_page   = 0;
static float         zoom_factor    = 1.0f;
static int           bookmark_page  = -1;

/* ---------- Drawing Globals ---------- */
static cairo_surface_t *page_surface = NULL;
static unsigned char   *page_pixels  = NULL;
static int              page_w, page_h;

/* ---------- Widgets ---------- */
static GtkWidget *drawing_area;
static GtkWidget *page_label;
static GtkWidget *bookmark_btn;

/* ---------- Command Line ---------- */
static char *initial_file = NULL;

/* ---------- Helpers ---------- */
static char *bookmark_path(const char *pdf) {
    return pdf ? g_strdup_printf("%s.bookmark", pdf) : NULL;
}

static void load_bookmark(void) {
    bookmark_page = -1;
    char *path = bookmark_path(current_path);
    if (!path) return;
    FILE *f = fopen(path, "r");
    if (f) {
        int p;
        if (fscanf(f, "%d", &p) == 1)
            bookmark_page = p - 1;
        fclose(f);
    }
    g_free(path);
}

static void save_bookmark(void) {
    char *path = bookmark_path(current_path);
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (f) {
        if (bookmark_page >= 0)
            fprintf(f, "%d\n", bookmark_page + 1);
        fclose(f);
    }
    g_free(path);
}

/* ---------- PDF Rendering ---------- */
static void free_page_surface(void) {
    if (page_surface) cairo_surface_destroy(page_surface);
    g_free(page_pixels);
    page_surface = NULL;
    page_pixels  = NULL;
}

static void render_current_page(void) {
    free_page_surface();
    if (!doc) return;

    fz_page *page = fz_load_page(ctx, doc, current_page);
    if (!page) return;

    fz_matrix ctm = fz_scale(zoom_factor, zoom_factor);
    fz_pixmap *pix = fz_new_pixmap_from_page(ctx, page, ctm, fz_device_rgb(ctx), 0);

    page_w = fz_pixmap_width(ctx, pix);
    page_h = fz_pixmap_height(ctx, pix);
    int in_stride = fz_pixmap_stride(ctx, pix);
    unsigned char *in = fz_pixmap_samples(ctx, pix);

    int out_stride = page_w * 4;
    page_pixels = g_malloc(out_stride * page_h);

    for (int y = 0; y < page_h; y++) {
        unsigned char *src = in + y * in_stride;
        unsigned char *dst = page_pixels + y * out_stride;
        for (int x = 0; x < page_w; x++) {
            dst[4*x + 0] = src[3*x + 2];
            dst[4*x + 1] = src[3*x + 1];
            dst[4*x + 2] = src[3*x + 0];
            dst[4*x + 3] = 255;
        }
    }

    page_surface = cairo_image_surface_create_for_data(
        page_pixels, CAIRO_FORMAT_ARGB32, page_w, page_h, out_stride);

    fz_drop_pixmap(ctx, pix);
    fz_drop_page(ctx, page);
}

/* ---------- UI Refresh ---------- */
static void update_ui(void) {
    char buf[128];
    snprintf(buf, sizeof(buf),
             "Page %d/%d   Zoom %.0f%%   %s",
             current_page+1, page_count,
             zoom_factor*100.0f,
             bookmark_page >= 0 ? "Bookmarked" : "");
    gtk_label_set_text(GTK_LABEL(page_label), buf);
    gtk_button_set_label(GTK_BUTTON(bookmark_btn),
        bookmark_page == current_page ? "Remove Bookmark" : "Set Bookmark");

    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(drawing_area), page_w);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(drawing_area), page_h);
    gtk_widget_queue_draw(drawing_area);
}

/* ---------- Actions ---------- */
static void go_to_page(int delta) {
    if (!doc) return;
    int np = current_page + delta;
    if (np < 0) np = 0;
    if (np >= page_count) np = page_count - 1;
    current_page = np;
    render_current_page();
    update_ui();
}

static void on_prev(GtkWidget*, gpointer) { go_to_page(-1); }
static void on_next(GtkWidget*, gpointer) { go_to_page(1); }

static void on_zoom_in(GtkWidget*, gpointer) {
    zoom_factor *= 1.2f;
    if (zoom_factor > 5.0f) zoom_factor = 5.0f;
    render_current_page();
    update_ui();
}

static void on_zoom_out(GtkWidget*, gpointer) {
    zoom_factor /= 1.2f;
    if (zoom_factor < 0.1f) zoom_factor = 0.1f;
    render_current_page();
    update_ui();
}

static void on_toggle_bookmark(GtkWidget*, gpointer) {
    if (!doc) return;
    bookmark_page = (bookmark_page == current_page) ? -1 : current_page;
    save_bookmark();
    update_ui();
}

/* ---------- File Handling ---------- */
static void open_pdf(const char *path) {
    if (doc) { fz_drop_document(ctx, doc); doc = NULL; }
    g_free(current_path); current_path = g_strdup(path);
    zoom_factor = 1.0f;
    bookmark_page = -1;

    fz_try(ctx) {
        doc = fz_open_document(ctx, current_path);
        page_count = fz_count_pages(ctx, doc);
        current_page = 0;
        load_bookmark();
    } fz_catch(ctx) {
        doc = NULL;
        page_count = 0;
    }
    render_current_page();
    update_ui();
}

static void on_open_response(GtkDialog *dlg, int resp, gpointer) {
    if (resp == GTK_RESPONSE_ACCEPT) {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dlg));
        char *path = g_file_get_path(file);
        open_pdf(path);
        g_free(path);
        g_object_unref(file);
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
}

static void on_open(GtkWidget*, gpointer) {
    GtkWindow *win = GTK_WINDOW(gtk_widget_get_ancestor(drawing_area, GTK_TYPE_WINDOW));
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open PDF", win, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    g_signal_connect(dlg, "response", G_CALLBACK(on_open_response), NULL);
    gtk_widget_show(dlg);
}

/* ---------- Drawing ---------- */
static void draw_cb(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer) {
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    if (!page_surface) return;
    double ox = (w > page_w) ? (w - page_w) / 2.0 : 0;
    double oy = (h > page_h) ? (h - page_h) / 2.0 : 0;
    cairo_set_source_surface(cr, page_surface, ox, oy);
    cairo_paint(cr);
}

/* ---------- Keyboard Shortcuts ---------- */
static gboolean on_key_pressed(GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer) {
    switch (keyval) {
        case GDK_KEY_Left:      on_prev(NULL, NULL); return TRUE;
        case GDK_KEY_Right:     on_next(NULL, NULL); return TRUE;
        case GDK_KEY_plus:
        case GDK_KEY_KP_Add:    on_zoom_in(NULL, NULL); return TRUE;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract: on_zoom_out(NULL, NULL); return TRUE;
        case GDK_KEY_b:         on_toggle_bookmark(NULL, NULL); return TRUE;
        default:                return FALSE;
    }
}

/* ---------- Command Line Handling ---------- */
static int on_command_line(GApplication *app, GApplicationCommandLine *cmdline, gpointer) {
    int argc;
    char **argv = g_application_command_line_get_arguments(cmdline, &argc);
    if (argc > 1)
        initial_file = g_strdup(argv[1]);
    g_strfreev(argv);
    g_application_activate(app);
    return 0;
}

/* ---------- CSS Load ---------- */
static void load_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    static const gchar *retro_css =
        "button {"
        "  border: 2px solid #808080;"
        "  border-top-color: #ffffff;"
        "  border-left-color: #ffffff;"
        "  border-bottom-color: #404040;"
        "  border-right-color: #404040;"
        "  background: #c0c0c0;"
        "  padding: 2px 6px;"
        "  box-shadow: none;"
        "  font-family: 'MS Sans Serif', 'Tahoma', Arial, sans-serif;"
        "  font-size: 10pt;"
        "  color: black;"
        "}"
        "button:hover {"
        "  background: #e0e0e0;"
        "}"
        "button:active {"
        "  border-top-color: #404040;"
        "  border-left-color: #404040;"
        "  border-bottom-color: #ffffff;"
        "  border-right-color: #ffffff;"
        "  background: #a0a0a0;"
        "}";
    gtk_css_provider_load_from_data(provider, retro_css, -1);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ---------- GTK Activate ---------- */
static void activate(GtkApplication *app, gpointer) {
    load_css();

    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "MuPDF Viewer");
    gtk_window_set_default_size(GTK_WINDOW(win), 1000, 700);

    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_widget_add_controller(win, kc);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key_pressed), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    GtkWidget *sc = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(sc, TRUE);
    gtk_widget_set_vexpand(sc, TRUE);
    gtk_box_append(GTK_BOX(vbox), sc);

    drawing_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area), draw_cb, NULL, NULL);
    gtk_widget_set_hexpand(drawing_area, TRUE);
    gtk_widget_set_vexpand(drawing_area, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), drawing_area);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_widget_set_margin_start(bar, 4);
    gtk_widget_set_margin_end(bar, 4);
    gtk_box_append(GTK_BOX(vbox), bar);

    GtkWidget *bopen = gtk_button_new_with_label("Open");
    GtkWidget *bprev = gtk_button_new_with_label("<");
    GtkWidget *bnext = gtk_button_new_with_label(">");
    GtkWidget *bzi   = gtk_button_new_with_label("+");
    GtkWidget *bzo   = gtk_button_new_with_label("−");
    bookmark_btn     = gtk_button_new_with_label("Set Bookmark");
    page_label       = gtk_label_new("No document");

    gtk_box_append(GTK_BOX(bar), bopen);
    gtk_box_append(GTK_BOX(bar), bprev);
    gtk_box_append(GTK_BOX(bar), bnext);
    gtk_box_append(GTK_BOX(bar), bzi);
    gtk_box_append(GTK_BOX(bar), bzo);
    gtk_box_append(GTK_BOX(bar), bookmark_btn);
    gtk_widget_set_hexpand(page_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(page_label), 1);
    gtk_box_append(GTK_BOX(bar), page_label);

    g_signal_connect(bopen, "clicked", G_CALLBACK(on_open), NULL);
    g_signal_connect(bprev, "clicked", G_CALLBACK(on_prev), NULL);
    g_signal_connect(bnext, "clicked", G_CALLBACK(on_next), NULL);
    g_signal_connect(bzi,   "clicked", G_CALLBACK(on_zoom_in), NULL);
    g_signal_connect(bzo,   "clicked", G_CALLBACK(on_zoom_out), NULL);
    g_signal_connect(bookmark_btn, "clicked", G_CALLBACK(on_toggle_bookmark), NULL);

    gtk_widget_show(win);

    if (initial_file) {
        open_pdf(initial_file);
        g_free(initial_file);
        initial_file = NULL;
    }
}

/* ---------- Main ---------- */
int main(int argc, char **argv) {
    ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
    if (!ctx) {
        fprintf(stderr, "Failed to create MuPDF context\n");
        return 1;
    }
    fz_register_document_handlers(ctx);

    GtkApplication *app = gtk_application_new(
        "com.example.mupdf", G_APPLICATION_HANDLES_COMMAND_LINE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    g_signal_connect(app, "command-line", G_CALLBACK(on_command_line), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    if (doc) fz_drop_document(ctx, doc);
    free_page_surface();
    fz_drop_context(ctx);
    g_free(current_path);
    g_object_unref(app);
    return status;
}
