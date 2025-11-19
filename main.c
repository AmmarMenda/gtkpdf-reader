#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mupdf/pdf.h>
#include <math.h>

// MuPDF internal helper for text selection
extern char *fz_copy_selection_from_stext_page(fz_context *ctx, fz_stext_page *page, fz_rect rect);

/* ---------- Global State ---------- */
static fz_context  *ctx              = NULL;
static fz_document *doc              = NULL;
static char        *current_path     = NULL;
static int          page_count       = 0;
static int          current_page     = 0;
static float        zoom_factor      = 1.0f;
static int          bookmark_page    = -1;

/* ---------- Drawing Globals ---------- */
static cairo_surface_t *page_surface = NULL;
static unsigned char   *page_pixels  = NULL;
static int              page_w, page_h;

/* ---------- Text Selection Globals ---------- */
static int              selecting = 0;
static double           initial_drag_x = 0.0;
static double           initial_drag_y = 0.0;
static fz_point         selection_start_pt = { 0, 0 };
static fz_point         selection_end_pt = { 0, 0 };
static fz_rect          selection_rect = { 0, 0, 0, 0 };

/* ---------- Widgets ---------- */
static GtkWidget *drawing_area;
static GtkWidget *page_label;
static GtkWidget *bookmark_btn;
static char *initial_file = NULL;

/* ---------- Forward Declarations ---------- */
void on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data);
void on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer data);
void on_drag_end(GtkGestureDrag *gesture, double velocity_x, double velocity_y, gpointer data);

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

static fz_point screen_to_pdf(double screen_x, double screen_y) {
    fz_point pt = {0.0f, 0.0f};
    if (!drawing_area || !doc) return pt;

    double drawing_w = gtk_widget_get_width(drawing_area);
    double drawing_h = gtk_widget_get_height(drawing_area);
    double ox = (drawing_w > page_w) ? (drawing_w - page_w) / 2.0 : 0.0;
    double oy = (drawing_h > page_h) ? (drawing_h - page_h) / 2.0 : 0.0;

    double page_x = screen_x - ox;
    double page_y = screen_y - oy;

    page_x = fmax(0, fmin(page_w, page_x));
    page_y = fmax(0, fmin(page_h, page_y));

    if (zoom_factor > 0) {
        pt.x = (float)(page_x / zoom_factor);
        pt.y = (float)(page_y / zoom_factor);
    }
    return pt;
}

// --- NEW HELPER: Reset Scroll to Top-Left ---
static void reset_scroll_view(void) {
    if (!drawing_area) return;
    GtkWidget *sc = gtk_widget_get_ancestor(drawing_area, GTK_TYPE_SCROLLED_WINDOW);
    if (!sc) return;

    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sc));
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(sc));

    if (vadj) gtk_adjustment_set_value(vadj, 0.0);
    if (hadj) gtk_adjustment_set_value(hadj, 0.0);
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

    fz_page *page = NULL;
    fz_pixmap *pix = NULL;

    fz_try(ctx) {
        page = fz_load_page(ctx, doc, current_page);
        if (!page) fz_throw(ctx, FZ_ERROR_GENERIC, "Failed to load page %d", current_page);

        fz_matrix ctm = fz_scale(zoom_factor, zoom_factor);
        pix = fz_new_pixmap_from_page(ctx, page, ctm, fz_device_rgb(ctx), 0);

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
                dst[4*x + 0] = src[3*x + 2]; // B
                dst[4*x + 1] = src[3*x + 1]; // G
                dst[4*x + 2] = src[3*x + 0]; // R
                dst[4*x + 3] = 255;          // A
            }
        }

        page_surface = cairo_image_surface_create_for_data(
            page_pixels, CAIRO_FORMAT_ARGB32, page_w, page_h, out_stride);

    } fz_always(ctx) {
        fz_drop_pixmap(ctx, pix);
        fz_drop_page(ctx, page);
    } fz_catch(ctx) {
        fprintf(stderr, "Error rendering page: %s\n", fz_caught_message(ctx));
        free_page_surface();
    }
}

/* ---------- UI Refresh ---------- */
static void update_ui(void) {
    char buf[128];
    if (doc) {
        snprintf(buf, sizeof(buf),
                 "PAGE %d/%d  [ZOOM: %.0f%%]  %s",
                 current_page + 1, page_count,
                 zoom_factor * 100.0f,
                 bookmark_page >= 0 ? "*SAVED*" : "");
    } else {
        snprintf(buf, sizeof(buf), "NO DATA LOADED");
    }

    gtk_label_set_text(GTK_LABEL(page_label), buf);

    gtk_button_set_label(GTK_BUTTON(bookmark_btn),
        (doc && bookmark_page == current_page) ? "UN-MARK (B)" : "MARK (B)");

    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(drawing_area), page_w);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(drawing_area), page_h);
    gtk_widget_queue_draw(drawing_area);
}

/* ---------- Selection & Clipboard Logic ---------- */
static int rect_intersects_selection(fz_rect r1, fz_rect r2) {
    if (r1.x1 <= r2.x0 || r1.x0 >= r2.x1) return 0;
    if (r1.y1 <= r2.y0 || r1.y0 >= r2.y1) return 0;
    return 1;
}

static void copy_selection_to_clipboard(void) {
    if (!doc || selection_rect.x0 >= selection_rect.x1 || selection_rect.y0 >= selection_rect.y1) return;

    fz_page *page = NULL;
    fz_stext_page *stext_page = NULL;
    GString *text_buffer = g_string_new("");

    fz_try(ctx) {
        page = fz_load_page(ctx, doc, current_page);
        stext_page = fz_new_stext_page_from_page(ctx, page, NULL);

        for (fz_stext_block *block = stext_page->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line *line = block->u.t.first_line; line; line = line->next) {
                if (!rect_intersects_selection(line->bbox, selection_rect)) continue;
                for (fz_stext_char *ch = line->first_char; ch; ch = ch->next) {
                    g_string_append_unichar(text_buffer, ch->c);
                }
                g_string_append_c(text_buffer, '\n');
            }
        }

        if (text_buffer->len > 0) {
            GdkDisplay *display = gdk_display_get_default();
            GdkClipboard *clipboard = gdk_display_get_clipboard(display);
            char *final_text = g_string_free(text_buffer, FALSE);
            size_t len = strlen(final_text);
            if (len > 0 && final_text[len - 1] == '\n') final_text[len - 1] = '\0';
            gdk_clipboard_set_text(clipboard, final_text);
            g_free(final_text);
        } else {
            g_string_free(text_buffer, TRUE);
        }
    } fz_always(ctx) {
        fz_drop_stext_page(ctx, stext_page);
        fz_drop_page(ctx, page);
        selection_rect = (fz_rect){0, 0, 0, 0};
    } fz_catch(ctx) {
        g_string_free(text_buffer, TRUE);
    }
}

void on_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer data) {
    if (!selecting || !doc) return;
    double current_x = initial_drag_x + offset_x;
    double current_y = initial_drag_y + offset_y;
    selection_end_pt = screen_to_pdf(current_x, current_y);
    selection_rect.x0 = fminf(selection_start_pt.x, selection_end_pt.x);
    selection_rect.y0 = fminf(selection_start_pt.y, selection_end_pt.y);
    selection_rect.x1 = fmaxf(selection_start_pt.x, selection_end_pt.x);
    selection_rect.y1 = fmaxf(selection_start_pt.y, selection_end_pt.y);
    gtk_widget_queue_draw(drawing_area);
}

void on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer data) {
    if (!doc) return;
    selecting = 1;
    initial_drag_x = x;
    initial_drag_y = y;
    selection_start_pt = screen_to_pdf(x, y);
    selection_end_pt = selection_start_pt;
    selection_rect = (fz_rect){0, 0, 0, 0};
    gtk_widget_queue_draw(drawing_area);
}

void on_drag_end(GtkGestureDrag *gesture, double velocity_x, double velocity_y, gpointer data) {
    if (!selecting) return;
    selecting = 0;
    double offset_x, offset_y;
    gtk_gesture_drag_get_offset(gesture, &offset_x, &offset_y);
    double final_x = initial_drag_x + offset_x;
    double final_y = initial_drag_y + offset_y;
    selection_end_pt = screen_to_pdf(final_x, final_y);

    selection_rect.x0 = fminf(selection_start_pt.x, selection_end_pt.x);
    selection_rect.y0 = fminf(selection_start_pt.y, selection_end_pt.y);
    selection_rect.x1 = fmaxf(selection_start_pt.x, selection_end_pt.x);
    selection_rect.y1 = fmaxf(selection_start_pt.y, selection_end_pt.y);

    float width = selection_rect.x1 - selection_rect.x0;
    float height = selection_rect.y1 - selection_rect.y0;

    if (width > 1.0f || height > 1.0f) {
        copy_selection_to_clipboard();
    } else {
        selection_rect = (fz_rect){0, 0, 0, 0};
    }
    gtk_widget_queue_draw(drawing_area);
}

/* ---------- Actions ---------- */
static void go_to_page(int delta) {
    if (!doc) return;
    int np = current_page + delta;
    if (np < 0) np = 0;
    if (np >= page_count) np = page_count - 1;
    if (np == current_page) return;

    current_page = np;
    selection_rect = (fz_rect){0, 0, 0, 0};

    render_current_page();
    reset_scroll_view(); // <--- RESET SCROLL ON PAGE CHANGE
    update_ui();
}

static void on_prev(GtkWidget *w, gpointer data) { go_to_page(-1); }
static void on_next(GtkWidget *w, gpointer data) { go_to_page(1); }
static void on_zoom_in(GtkWidget *w, gpointer data) {
    if (!doc) return;
    zoom_factor *= 1.2f;
    if (zoom_factor > 5.0f) zoom_factor = 5.0f;
    selection_rect = (fz_rect){0, 0, 0, 0};
    render_current_page();
    update_ui();
}
static void on_zoom_out(GtkWidget *w, gpointer data) {
    if (!doc) return;
    zoom_factor /= 1.2f;
    if (zoom_factor < 0.1f) zoom_factor = 0.1f;
    selection_rect = (fz_rect){0, 0, 0, 0};
    render_current_page();
    update_ui();
}
static void on_toggle_bookmark(GtkWidget *w, gpointer data) {
    if (!doc) return;
    bookmark_page = (bookmark_page == current_page) ? -1 : current_page;
    save_bookmark();
    update_ui();
}
static void on_go_to_bookmark(GtkWidget *w, gpointer data) {
    if (!doc || bookmark_page < 0) return;
    current_page = bookmark_page;
    selection_rect = (fz_rect){0, 0, 0, 0};
    render_current_page();
    reset_scroll_view(); // <--- RESET SCROLL ON JUMP TO BOOKMARK
    update_ui();
}

/* ---------- File Handling ---------- */
static void open_pdf(const char *path) {
    if (doc) { fz_drop_document(ctx, doc); doc = NULL; }
    free_page_surface();
    g_free(current_path); current_path = g_strdup(path);
    zoom_factor = 1.0f;
    page_w = 0; page_h = 0;
    selection_rect = (fz_rect){0, 0, 0, 0};

    fz_try(ctx) {
        doc = fz_open_document(ctx, current_path);
        page_count = fz_count_pages(ctx, doc);
        current_page = 0;
        load_bookmark();
        if (bookmark_page >= 0 && bookmark_page < page_count) {
            current_page = bookmark_page;
        } else {
            bookmark_page = -1;
        }
    } fz_catch(ctx) {
        fprintf(stderr, "Error opening document: %s\n", fz_caught_message(ctx));
        doc = NULL;
        page_count = 0;
    }
    render_current_page();
    reset_scroll_view(); // <--- RESET SCROLL ON OPEN
    update_ui();
}

static void on_open_response(GtkDialog *dlg, int resp, gpointer data) {
    if (resp == GTK_RESPONSE_ACCEPT) {
        GFile *file = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dlg));
        char *path = g_file_get_path(file);
        open_pdf(path);
        g_free(path);
        g_object_unref(file);
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
}

static void on_open(GtkWidget *w, gpointer data) {
    GtkWindow *win = GTK_WINDOW(gtk_widget_get_ancestor(drawing_area, GTK_TYPE_WINDOW));
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "OPEN FILE", win, GTK_FILE_CHOOSER_ACTION_OPEN,
        "_CANCEL", GTK_RESPONSE_CANCEL, "_OPEN", GTK_RESPONSE_ACCEPT, NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "PDF Documents");
    gtk_file_filter_add_pattern(filter, "*.pdf");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);

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

    // --- NEO-BRUTALIST SELECTION DRAWING ---
    if (doc && (selection_rect.x1 > selection_rect.x0 || selection_rect.y1 > selection_rect.y0)) {
        double sx0 = selection_rect.x0 * zoom_factor + ox;
        double sy0 = selection_rect.y0 * zoom_factor + oy;
        double sx1 = selection_rect.x1 * zoom_factor + ox;
        double sy1 = selection_rect.y1 * zoom_factor + oy;

        double sel_w = sx1 - sx0;
        double sel_h = sy1 - sy0;

        cairo_rectangle(cr, sx0, sy0, sel_w, sel_h);

        // High Contrast Neon Pink Fill
        cairo_set_source_rgba(cr, 1.0, 0.0, 0.8, 0.4);
        cairo_fill_preserve(cr);

        // Thick Black Border
        cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
        cairo_set_line_width(cr, 3.0);
        cairo_stroke(cr);
    }
}

/* ---------- KEYBOARD SHORTCUTS & SCROLLING ---------- */

static void scroll_view(double x_dir, double y_dir) {
    if (!drawing_area) return;
    GtkWidget *sc = gtk_widget_get_ancestor(drawing_area, GTK_TYPE_SCROLLED_WINDOW);
    if (!sc) return;

    // --- Handle Vertical Scroll (W/S) ---
    if (y_dir != 0.0) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sc));
        double val = gtk_adjustment_get_value(adj);
        double page_size = gtk_adjustment_get_page_size(adj);
        double upper = gtk_adjustment_get_upper(adj);

        double step = page_size * 0.15;
        double new_val = val + (y_dir * step);

        if (new_val < 0) new_val = 0;
        if (new_val > upper - page_size) new_val = upper - page_size;
        gtk_adjustment_set_value(adj, new_val);
    }

    // --- Handle Horizontal Scroll (A/D) ---
    if (x_dir != 0.0) {
        GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(sc));
        double val = gtk_adjustment_get_value(adj);
        double page_size = gtk_adjustment_get_page_size(adj);
        double upper = gtk_adjustment_get_upper(adj);

        double step = page_size * 0.15;
        double new_val = val + (x_dir * step);

        if (new_val < 0) new_val = 0;
        if (new_val > upper - page_size) new_val = upper - page_size;
        gtk_adjustment_set_value(adj, new_val);
    }
}

static gboolean on_key_pressed(GtkEventControllerKey *kc, guint keyval, guint keycode, GdkModifierType state, gpointer data) {
    if (keyval == GDK_KEY_o && (state & GDK_CONTROL_MASK)) {
        on_open(NULL, NULL); return TRUE;
    }
    switch (keyval) {
        /* WASD PANNING */
        case GDK_KEY_w:
        case GDK_KEY_W: scroll_view(0.0, -1.0); return TRUE; // Pan Up

        case GDK_KEY_s:
        case GDK_KEY_S: scroll_view(0.0, 1.0); return TRUE;  // Pan Down

        case GDK_KEY_a:
        case GDK_KEY_A: scroll_view(-1.0, 0.0); return TRUE; // Pan Left

        case GDK_KEY_d:
        case GDK_KEY_D: scroll_view(1.0, 0.0); return TRUE;  // Pan Right

        /* PAGE TURNING (ARROWS) */
        case GDK_KEY_Left:       on_prev(NULL, NULL); return TRUE;
        case GDK_KEY_Right:      on_next(NULL, NULL); return TRUE;

        /* Alternate Standard Controls */
        case GDK_KEY_Up:         scroll_view(0.0, -1.0); return TRUE;
        case GDK_KEY_Down:       scroll_view(0.0, 1.0); return TRUE;

        case GDK_KEY_plus:
        case GDK_KEY_equal:
        case GDK_KEY_KP_Add:     on_zoom_in(NULL, NULL); return TRUE;

        case GDK_KEY_minus:
        case GDK_KEY_KP_Subtract: on_zoom_out(NULL, NULL); return TRUE;

        case GDK_KEY_b:          on_toggle_bookmark(NULL, NULL); return TRUE;
        case GDK_KEY_g:          if (state & GDK_CONTROL_MASK) { on_go_to_bookmark(NULL, NULL); return TRUE; } return FALSE;
        default:                 return FALSE;
    }
}

static int on_command_line(GApplication *app, GApplicationCommandLine *cmdline, gpointer data) {
    int argc;
    char **argv = g_application_command_line_get_arguments(cmdline, &argc);
    if (argc > 1) initial_file = g_strdup(argv[1]);
    g_strfreev(argv);
    g_application_activate(app);
    return 0;
}

/* ---------- NEO-BRUTALIST CSS LOADER ---------- */
static void load_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    static const gchar *neo_css =
        "window {"
        "  background-color: #f5f5dc;"
        "  color: black;"
        "  font-family: 'Monospace', 'Courier New';"
        "  font-weight: 800;"
        "}"
        "scrolledwindow, viewport {"
        "  border: 3px solid black;"
        "  background: #ffffff;"
        "  border-radius: 0px;"
        "}"
        "button {"
        "  background-color: #ffffff;"
        "  color: black;"
        "  border: 3px solid black;"
        "  border-radius: 0px;"
        "  padding: 8px 16px;"
        "  margin-right: 10px;"
        "  margin-bottom: 6px;"
        "  box-shadow: 6px 6px 0px black;"
        "  transition: all 50ms ease;"
        "  font-weight: 900;"
        "  letter-spacing: 1px;"
        "}"
        "button:hover {"
        "  background-color: #FFF700;"
        "  transform: translate(-1px, -1px);"
        "  box-shadow: 7px 7px 0px black;"
        "}"
        "button:active {"
        "  box-shadow: 0px 0px 0px black;"
        "  transform: translate(6px, 6px);"
        "  background-color: #FF6B6B;"
        "}"
        "label {"
        "  font-family: 'Monospace';"
        "  font-size: 14px;"
        "  background-color: black;"
        "  color: white;"
        "  padding: 6px 12px;"
        "  border-radius: 0px;"
        "}";

    gtk_css_provider_load_from_data(provider, neo_css, -1);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ---------- GTK Activate ---------- */
static void activate(GtkApplication *app, gpointer data) {
    load_css();

    GtkWidget *win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "NEO_READER_V2 [SCROLL_RESET]");
    gtk_window_set_default_size(GTK_WINDOW(win), 1100, 800);

    GtkEventController *kc = gtk_event_controller_key_new();
    gtk_widget_add_controller(win, kc);
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_key_pressed), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    GtkWidget *sc = gtk_scrolled_window_new();
    gtk_widget_set_hexpand(sc, TRUE);
    gtk_widget_set_vexpand(sc, TRUE);

    gtk_widget_set_margin_start(sc, 20);
    gtk_widget_set_margin_end(sc, 20);
    gtk_widget_set_margin_top(sc, 20);
    gtk_box_append(GTK_BOX(vbox), sc);

    drawing_area = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(drawing_area), draw_cb, NULL, NULL);
    gtk_widget_set_size_request(drawing_area, 1, 1);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), drawing_area);

    GtkGesture *drag_gesture = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag_gesture), GDK_BUTTON_PRIMARY);
    gtk_widget_add_controller(drawing_area, GTK_EVENT_CONTROLLER(drag_gesture));
    g_signal_connect(drag_gesture, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    g_signal_connect(drag_gesture, "drag-update", G_CALLBACK(on_drag_update), NULL);
    g_signal_connect(drag_gesture, "drag-end", G_CALLBACK(on_drag_end), NULL);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 15);
    gtk_widget_set_margin_top(bar, 20);
    gtk_widget_set_margin_bottom(bar, 20);
    gtk_widget_set_margin_start(bar, 20);
    gtk_widget_set_margin_end(bar, 20);
    gtk_box_append(GTK_BOX(vbox), bar);

    GtkWidget *bopen = gtk_button_new_with_label("OPEN");
    GtkWidget *bprev = gtk_button_new_with_label("PREV (\u2190)");
    GtkWidget *bnext = gtk_button_new_with_label("NEXT (\u2192)");
    GtkWidget *bzi   = gtk_button_new_with_label("ZOOM +");
    GtkWidget *bzo   = gtk_button_new_with_label("ZOOM -");
    bookmark_btn     = gtk_button_new_with_label("MARK (B)");
    page_label       = gtk_label_new("NO DATA");

    gtk_box_append(GTK_BOX(bar), bopen);
    gtk_box_append(GTK_BOX(bar), bprev);
    gtk_box_append(GTK_BOX(bar), bnext);
    gtk_box_append(GTK_BOX(bar), bzi);
    gtk_box_append(GTK_BOX(bar), bzo);
    gtk_box_append(GTK_BOX(bar), bookmark_btn);

    gtk_widget_set_hexpand(page_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(page_label), 1.0f);
    gtk_box_append(GTK_BOX(bar), page_label);

    g_signal_connect(bopen, "clicked", G_CALLBACK(on_open), NULL);
    g_signal_connect(bprev, "clicked", G_CALLBACK(on_prev), NULL);
    g_signal_connect(bnext, "clicked", G_CALLBACK(on_next), NULL);
    g_signal_connect(bzi,   "clicked", G_CALLBACK(on_zoom_in), NULL);
    g_signal_connect(bzo,   "clicked", G_CALLBACK(on_zoom_out), NULL);
    g_signal_connect(bookmark_btn, "clicked", G_CALLBACK(on_toggle_bookmark), NULL);

    gtk_widget_show(win);

    if (initial_file && ctx) {
        open_pdf(initial_file);
        g_free(initial_file);
        initial_file = NULL;
    } else {
        update_ui();
    }
}

int main(int argc, char **argv) {
    ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
    if (!ctx) return 1;
    fz_register_document_handlers(ctx);

    GtkApplication *app = gtk_application_new("com.neo.pdf", G_APPLICATION_HANDLES_COMMAND_LINE);
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
