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
        if (fscanf(f, "%d", &p) == 1) bookmark_page = p - 1;
        fclose(f);
    }
    g_free(path);
}

static void save_bookmark(void) {
    char *path = bookmark_path(current_path);
    if (!path) return;
    FILE *f = fopen(path, "w");
    if (f) {
        if (bookmark_page >= 0) fprintf(f, "%d\n", bookmark_page + 1);
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
            dst[x*4 + 0] = src[x*3 + 2]; // B
            dst[x*4 + 1] = src[x*3 + 1]; // G
            dst[x*4 + 2] = src[x*3 + 0]; // R
            dst[x*4 + 3] = 255;          // A
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
             current_page + 1,
             page_count,
             zoom_factor * 100.0,
             bookmark_page >= 0 ? "(Bookmarked)" : "");
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

static void on_prev(GtkWidget *w, gpointer d) { go_to_page(-1); }
static void on_next(GtkWidget *w, gpointer d) { go_to_page(+1); }

static void on_zoom_in(GtkWidget *w, gpointer d) {
    zoom_factor *= 1.2f;
    if (zoom_factor > 5.0f) zoom_factor = 5.0f;
    render_current_page();
    update_ui();
}

static void on_zoom_out(GtkWidget *w, gpointer d) {
    zoom_factor /= 1.2f;
    if (zoom_factor < 0.1f) zoom_factor = 0.1f;
    render_current_page();
    update_ui();
}

static void on_toggle_bookmark(GtkWidget *w, gpointer d) {
    if (!doc) return;
    if (bookmark_page == current_page)
        bookmark_page = -1;
    else
        bookmark_page = current_page;
    save_bookmark();
    update_ui();
}

/* ---------- File Handling ---------- */
static void open_pdf(const char *path) {
    if (doc) { fz_drop_document(ctx, doc); doc = NULL; }
    g_free(current_path);
    current_path = g_strdup(path);
    zoom_factor    = 1.0f;
    bookmark_page  = -1;

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

static void on_open_response(GtkDialog *dlg, int resp, gpointer d) {
    if (resp == GTK_RESPONSE_ACCEPT) {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dlg));
        char *path = g_file_get_path(file);
        open_pdf(path);
        g_free(path);
        g_object_unref(file);
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
}

static void on_open(GtkWidget *w, gpointer d) {
    GtkWindow *win = GTK_WINDOW(gtk_widget_get_ancestor(w, GTK_TYPE_WINDOW));
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Open PDF", win, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, NULL);
    g_signal_connect(dlg, "response", G_CALLBACK(on_open_response), NULL);
    gtk_widget_show(dlg);
}

/* ---------- Drawing ---------- */
static void draw_cb(GtkDrawingArea *area, cairo_t *cr, int w, int h, gpointer data) {
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    if (!page_surface) return;
    double ox = (w > page_w) ? (w - page_w) / 2.0 : 0;
    double oy = (h > page_h) ? (h - page_h) / 2.0 : 0;
    cairo_set_source_surface(cr, page_surface, ox, oy);
    cairo_paint(cr);
}

/* ---------- Keyboard Shortcuts ---------- */
static gboolean on_key_pressed(GtkEventControllerKey *controller,
                               guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data) {
    switch (keyval) {
        case GDK_KEY_Left:     go_to_page(-1); return TRUE;
        case GDK_KEY_Right:    go_to_page(+1); return TRUE;
        case GDK_KEY_plus:
        case GDK_KEY_KP_Add:   on_zoom_in(NULL, NULL); return TRUE;
        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract: on_zoom_out(NULL, NULL); return TRUE;
        case GDK_KEY_b:        on_toggle_bookmark(NULL, NULL); return TRUE;
        default:               return FALSE;
    }
}

/* ---------- GTK Activate ---------- */
static void activate(GtkApplication *app, gpointer user_data) {
    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "MuPDF GTK Viewer");
    gtk_window_set_default_size(GTK_WINDOW(win), 1000, 700);

    /* Key controller */
    GtkEventController *keyc = gtk_event_controller_key_new();
    gtk_widget_add_controller(win, keyc);
    g_signal_connect(keyc, "key-pressed", G_CALLBACK(on_key_pressed), NULL);

    /* Main layout */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    /* Scrolled drawing area */
    GtkWidget *sc = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(sc, TRUE);
    gtk_widget_set_vexpand(sc, TRUE);
    gtk_box_append(GTK_BOX(vbox), sc);

    drawing_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area),
                                   draw_cb, NULL, NULL);
    gtk_widget_set_hexpand(drawing_area, TRUE);
    gtk_widget_set_vexpand(drawing_area, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), drawing_area);

    /* Bottom control bar */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(bar, 4);
    gtk_widget_set_margin_bottom(bar, 4);
    gtk_widget_set_margin_start(bar, 4);
    gtk_widget_set_margin_end(bar, 4);
    gtk_box_append(GTK_BOX(vbox), bar);

    GtkWidget *b_open   = gtk_button_new_with_label("Open");
    GtkWidget *b_prev   = gtk_button_new_with_label("<");
    GtkWidget *b_next   = gtk_button_new_with_label(">");
    GtkWidget *b_zoom_in= gtk_button_new_with_label("+");
    GtkWidget *b_zoom_out=gtk_button_new_with_label("−");
    bookmark_btn        = gtk_button_new_with_label("Set Bookmark");
    page_label          = gtk_label_new("No document");

    gtk_box_append(GTK_BOX(bar), b_open);
    gtk_box_append(GTK_BOX(bar), b_prev);
    gtk_box_append(GTK_BOX(bar), b_next);
    gtk_box_append(GTK_BOX(bar), b_zoom_in);
    gtk_box_append(GTK_BOX(bar), b_zoom_out);
    gtk_box_append(GTK_BOX(bar), bookmark_btn);
    gtk_widget_set_hexpand(page_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(page_label), 1);
    gtk_box_append(GTK_BOX(bar), page_label);

    g_signal_connect(b_open,    "clicked", G_CALLBACK(on_open),               NULL);
    g_signal_connect(b_prev,    "clicked", G_CALLBACK(on_prev),               NULL);
    g_signal_connect(b_next,    "clicked", G_CALLBACK(on_next),               NULL);
    g_signal_connect(b_zoom_in, "clicked", G_CALLBACK(on_zoom_in),            NULL);
    g_signal_connect(b_zoom_out,"clicked", G_CALLBACK(on_zoom_out),           NULL);
    g_signal_connect(bookmark_btn,"clicked",G_CALLBACK(on_toggle_bookmark),   NULL);

    gtk_widget_show(win);
}

/* ---------- Main ---------- */
int main(int argc, char **argv) {
    ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
    if (!ctx) return 1;
    fz_register_document_handlers(ctx);

    GtkApplication *app = gtk_application_new("com.example.mupdf", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);

    if (doc) fz_drop_document(ctx, doc);
    free_page_surface();
    fz_drop_context(ctx);
    g_free(current_path);
    g_object_unref(app);
    return status;
}
