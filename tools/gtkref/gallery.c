/*
 * GTK 2 reference gallery for the Murrine look-alike Panorama theme.
 *
 * Draws the same widgets at the same positions as layout/qt/gallery.xml
 * (and tools/qtref/gallery.py) with real GTK 2 and whatever engine the
 * gtkrc in GTK2_RC_FILES picks, then saves the window as a PNG and prints
 * the geometry of the parts a converter needs as JSON on stdout.
 * Black-box: it only uses GTK's public API; no engine code is read.
 *
 *   GTK2_RC_FILES=theme/gtk-2.0/gtkrc xvfb-run -a ./gtk2gallery out.png [probe.png]
 *
 * The window is 640x520: the top 640x400 is the gallery (out.png); the
 * strip below holds extra probe widgets (states the gallery doesn't show)
 * and is only saved in probe.png, the whole window.
 *
 * With GTKREF_BLANK=1 in the environment buttons, checks, entries and the
 * progress bar carry no text, so a converter can measure their faces.
 *
 * Build: cc -O2 gallery.c -o gtk2gallery $(pkg-config --cflags --libs gtk+-2.0)
 */
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GtkWidget *win, *fixed;
static int blank; /* GTKREF_BLANK=1: no text on buttons, checks and entries */

static const char *L( const char *s )
{
	return blank ? "" : s;
}
static const char *out_png, *probe_png;
static GString *json;

static GtkWidget *place( GtkWidget *w, int x, int y, int ww, int hh )
{
	gtk_fixed_put( GTK_FIXED( fixed ), w, x, y );
	gtk_widget_set_size_request( w, ww, hh );
	return w;
}

/* A widget that looks hovered: GTK draws PRELIGHT from the widget state. */
static void hover( GtkWidget *w )
{
	gtk_widget_set_state( w, GTK_STATE_PRELIGHT );
}

static void hover_after_realize( GtkWidget *w, gpointer d )
{
	(void)d;
	gtk_widget_set_state( w, GTK_STATE_PRELIGHT );
}

static struct { const char *name; GtkWidget *w; } named[64];
static int n_named;

static GtkWidget *name( const char *n, GtkWidget *w )
{
	named[n_named].name = n;
	named[n_named].w = w;
	n_named++;
	return w;
}

static void rect_json( const char *n, int x, int y, int w, int h )
{
	g_string_append_printf( json, "%s\"%s\": [%d, %d, %d, %d]", json->len > 2 ? ",\n " : "", n, x, y, w, h );
}

static void dump_geometry( void )
{
	int i;
	for ( i = 0; i < n_named; ++i )
	{
		GtkWidget *w = named[i].w;
		GtkAllocation *a = &w->allocation;
		rect_json( named[i].name, a->x, a->y, a->width, a->height );
		if ( GTK_IS_RANGE( w ) )
		{
			/* GtkRange keeps the slider's extent along the trough in public
			   (deprecated, but present) fields. */
			GtkRange *r = GTK_RANGE( w );
			char buf[96];
			snprintf( buf, sizeof buf, "%s.range", named[i].name );
			rect_json( buf, a->x + r->range_rect.x, a->y + r->range_rect.y, r->range_rect.width, r->range_rect.height );
			snprintf( buf, sizeof buf, "%s.slider", named[i].name );
			if ( gtk_orientable_get_orientation( GTK_ORIENTABLE( w ) ) == GTK_ORIENTATION_HORIZONTAL )
				rect_json( buf, a->x + r->slider_start, a->y + r->range_rect.y, r->slider_end - r->slider_start, r->range_rect.height );
			else
				rect_json( buf, a->x + r->range_rect.x, a->y + r->slider_start, r->range_rect.width, r->slider_end - r->slider_start );
		}
		if ( GTK_IS_TOGGLE_BUTTON( w ) && GTK_IS_CHECK_BUTTON( w ) )
		{
			int size = 0, spacing = 0;
			char buf[96];
			gtk_widget_style_get( w, "indicator-size", &size, "indicator-spacing", &spacing, NULL );
			snprintf( buf, sizeof buf, "%s.indicator", named[i].name );
			/* GTK centres the indicator vertically, spacing in from the left */
			rect_json( buf, a->x + GTK_CONTAINER( w )->border_width + spacing,
			           a->y + ( a->height - size ) / 2, size, size );
		}
		if ( GTK_IS_NOTEBOOK( w ) )
		{
			int p;
			GtkNotebook *nb = GTK_NOTEBOOK( w );
			for ( p = 0; p < gtk_notebook_get_n_pages( nb ); ++p )
			{
				GtkWidget *lab = gtk_notebook_get_tab_label( nb, gtk_notebook_get_nth_page( nb, p ) );
				char buf[96];
				snprintf( buf, sizeof buf, "%s.label%d", named[i].name, p );
				rect_json( buf, lab->allocation.x, lab->allocation.y, lab->allocation.width, lab->allocation.height );
			}
			{
				GtkWidget *page = gtk_notebook_get_nth_page( nb, 0 );
				rect_json( "tabs.page", page->allocation.x, page->allocation.y, page->allocation.width, page->allocation.height );
			}
		}
		if ( GTK_IS_FRAME( w ) )
		{
			GtkWidget *lab = gtk_frame_get_label_widget( GTK_FRAME( w ) );
			if ( lab )
				rect_json( "group.label", lab->allocation.x, lab->allocation.y, lab->allocation.width, lab->allocation.height );
		}
		if ( GTK_IS_BIN( w ) && gtk_bin_get_child( GTK_BIN( w ) ) && GTK_IS_LABEL( gtk_bin_get_child( GTK_BIN( w ) ) ) )
		{
			GtkWidget *lab = gtk_bin_get_child( GTK_BIN( w ) );
			char buf[96];
			snprintf( buf, sizeof buf, "%s.label", named[i].name );
			rect_json( buf, lab->allocation.x, lab->allocation.y, lab->allocation.width, lab->allocation.height );
		}
	}
	{
		GtkStyle *s = gtk_widget_get_style( win );
		static const char *states[] = { "normal", "active", "prelight", "selected", "insensitive" };
		int st;
		g_string_append( json, ",\n \"style\": {" );
		for ( st = 0; st < 5; ++st )
		{
#define C( c ) ( c ).red >> 8, ( c ).green >> 8, ( c ).blue >> 8
			g_string_append_printf( json, "%s\"%s\": {\"bg\": \"#%02x%02x%02x\", \"fg\": \"#%02x%02x%02x\", "
			                        "\"base\": \"#%02x%02x%02x\", \"text\": \"#%02x%02x%02x\"}",
			                        st ? ", " : "", states[st], C( s->bg[st] ), C( s->fg[st] ), C( s->base[st] ), C( s->text[st] ) );
		}
		g_string_append( json, "}" );
	}
}

static gboolean grab( gpointer d )
{
	GdkPixbuf *pb, *top;
	(void)d;
	while ( gtk_events_pending() )
		gtk_main_iteration();
	gdk_window_process_all_updates();
	gdk_flush();
	pb = gdk_pixbuf_get_from_drawable( NULL, win->window, NULL, 0, 0, 0, 0, 640, 520 );
	top = gdk_pixbuf_new_subpixbuf( pb, 0, 0, 640, 400 );
	gdk_pixbuf_save( top, out_png, "png", NULL, NULL );
	if ( probe_png )
		gdk_pixbuf_save( pb, probe_png, "png", NULL, NULL );
	json = g_string_new( "{" );
	dump_geometry();
	g_string_append( json, "\n}\n" );
	fputs( json->str, stdout );
	gtk_main_quit();
	return FALSE;
}

static GtkWidget *scrollbar( gboolean vertical )
{
	GtkObject *adj = gtk_adjustment_new( 0, 0, 100, 1, 30, 30 );
	return vertical ? gtk_vscrollbar_new( GTK_ADJUSTMENT( adj ) ) : gtk_hscrollbar_new( GTK_ADJUSTMENT( adj ) );
}

int main( int argc, char **argv )
{
	GtkWidget *w, *focused, *nb, *frame, *inner;
	int i;

	gtk_init( &argc, &argv );
	blank = getenv( "GTKREF_BLANK" ) != NULL;
	if ( argc < 2 )
	{
		fprintf( stderr, "usage: %s out.png [probe.png]\n", argv[0] );
		return 1;
	}
	out_png = argv[1];
	probe_png = argc > 2 ? argv[2] : NULL;

	win = gtk_window_new( GTK_WINDOW_TOPLEVEL );
	gtk_window_set_default_size( GTK_WINDOW( win ), 640, 520 );
	gtk_window_set_resizable( GTK_WINDOW( win ), FALSE );
	fixed = gtk_fixed_new();
	gtk_widget_set_size_request( fixed, 640, 520 );
	gtk_container_add( GTK_CONTAINER( win ), fixed );

	/* row 1: buttons */
	name( "btn-normal", place( gtk_button_new_with_label( L( "Button" ) ), 10, 10, 90, 28 ) );
	w = name( "btn-hover", place( gtk_button_new_with_label( L( "Hover" ) ), 110, 10, 90, 28 ) );
	g_signal_connect( w, "realize", G_CALLBACK( hover_after_realize ), NULL );
	w = name( "btn-pressed", place( gtk_button_new_with_label( L( "Pressed" ) ), 210, 10, 90, 28 ) );
	GTK_BUTTON( w )->depressed = TRUE;          /* drawn sunken */
	gtk_widget_set_state( w, GTK_STATE_ACTIVE );
	w = name( "btn-default", place( gtk_button_new_with_label( L( "Default" ) ), 310, 10, 90, 28 ) );
	GTK_WIDGET_SET_FLAGS( w, GTK_CAN_DEFAULT );
	gtk_window_set_default( GTK_WINDOW( win ), w );
	w = name( "btn-disabled", place( gtk_button_new_with_label( L( "Disabled" ) ), 410, 10, 90, 28 ) );
	gtk_widget_set_sensitive( w, FALSE );
	w = name( "btn-checked", place( gtk_toggle_button_new_with_label( L( "Checked" ) ), 510, 10, 90, 28 ) );
	gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON( w ), TRUE );

	/* row 2: check boxes and radios */
	name( "chk-normal", place( gtk_check_button_new_with_label( L( "Check" ) ), 10, 50, 90, 22 ) );
	w = name( "chk-checked", place( gtk_check_button_new_with_label( L( "Checked" ) ), 110, 50, 90, 22 ) );
	gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON( w ), TRUE );
	w = name( "chk-hover", place( gtk_check_button_new_with_label( L( "Hover" ) ), 210, 50, 90, 22 ) );
	g_signal_connect( w, "realize", G_CALLBACK( hover_after_realize ), NULL );
	w = name( "chk-disabled", place( gtk_check_button_new_with_label( L( "Disabled" ) ), 310, 50, 90, 22 ) );
	gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON( w ), TRUE );
	gtk_widget_set_sensitive( w, FALSE );
	{
		GtkWidget *r1 = name( "rad-normal", place( gtk_radio_button_new_with_label( NULL, L( "Radio" ) ), 410, 50, 90, 22 ) );
		GtkWidget *r2 = name( "rad-checked", place( gtk_radio_button_new_with_label_from_widget( GTK_RADIO_BUTTON( r1 ), L( "On" ) ), 510, 50, 90, 22 ) );
		gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON( r2 ), TRUE );
	}

	/* row 3: entries and combo */
	w = name( "entry-normal", place( gtk_entry_new(), 10, 90, 140, 26 ) );
	gtk_entry_set_text( GTK_ENTRY( w ), L( "Text entry" ) );
	focused = name( "entry-focus", place( gtk_entry_new(), 160, 90, 140, 26 ) );
	gtk_entry_set_text( GTK_ENTRY( focused ), L( "Focused" ) );
	w = name( "combo-normal", place( gtk_combo_box_new_text(), 310, 90, 140, 26 ) );
	gtk_combo_box_append_text( GTK_COMBO_BOX( w ), L( "Combo box" ) );
	gtk_combo_box_append_text( GTK_COMBO_BOX( w ), "Two" );
	gtk_combo_box_set_active( GTK_COMBO_BOX( w ), 0 );
	w = name( "entry-disabled", place( gtk_entry_new(), 460, 90, 140, 26 ) );
	gtk_entry_set_text( GTK_ENTRY( w ), L( "Disabled" ) );
	gtk_widget_set_sensitive( w, FALSE );

	/* row 4: slider and progress bars */
	w = name( "slider", place( gtk_hscale_new_with_range( 0, 100, 1 ), 10, 130, 200, 22 ) );
	gtk_scale_set_draw_value( GTK_SCALE( w ), FALSE );
	gtk_range_set_value( GTK_RANGE( w ), 40 );
	w = name( "progress", place( gtk_progress_bar_new(), 230, 130, 200, 22 ) );
	gtk_progress_bar_set_fraction( GTK_PROGRESS_BAR( w ), 0.4 );
	w = name( "progress-text", place( gtk_progress_bar_new(), 450, 130, 170, 22 ) );
	gtk_progress_bar_set_fraction( GTK_PROGRESS_BAR( w ), 0.7 );
	gtk_progress_bar_set_text( GTK_PROGRESS_BAR( w ), L( "70%" ) );

	/* row 5: scrollbars, group box (a GtkFrame), tabs */
	name( "scroll-v", place( scrollbar( TRUE ), 10, 170, 16, 140 ) );
	name( "scroll-h", place( scrollbar( FALSE ), 40, 170, 180, 16 ) );
	frame = name( "group", place( gtk_frame_new( "Group box" ), 40, 200, 180, 110 ) );
	inner = name( "group-child", gtk_fixed_new() );
	gtk_container_add( GTK_CONTAINER( frame ), inner );
	w = gtk_check_button_new_with_label( L( "Inside" ) );
	gtk_widget_set_size_request( w, 100, 22 );
	gtk_fixed_put( GTK_FIXED( inner ), w, 10, 6 );
	nb = name( "tabs", place( gtk_notebook_new(), 240, 170, 380, 140 ) );
	{
		static const char *tabs[] = { "First", "Second", "Third" };
		for ( i = 0; i < 3; ++i )
			gtk_notebook_append_page( GTK_NOTEBOOK( nb ), gtk_fixed_new(), gtk_label_new( tabs[i] ) );
	}

	/* probe strip, below the gallery */
	w = name( "combo-hover", place( gtk_combo_box_new_text(), 10, 410, 140, 26 ) );
	gtk_combo_box_append_text( GTK_COMBO_BOX( w ), L( "Combo box" ) );
	gtk_combo_box_set_active( GTK_COMBO_BOX( w ), 0 );
	g_signal_connect( w, "realize", G_CALLBACK( hover_after_realize ), NULL );
	w = name( "rad-hover", place( gtk_radio_button_new_with_label( NULL, L( "Radio" ) ), 160, 410, 90, 22 ) );
	g_signal_connect( w, "realize", G_CALLBACK( hover_after_realize ), NULL );
	w = name( "chk-disabled-off", place( gtk_check_button_new_with_label( L( "Off" ) ), 260, 410, 90, 22 ) );
	gtk_widget_set_sensitive( w, FALSE );
	w = name( "rad-disabled", place( gtk_radio_button_new_with_label( NULL, L( "Off" ) ), 360, 410, 90, 22 ) );
	gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON( w ), TRUE );
	gtk_widget_set_sensitive( w, FALSE );
	w = name( "progress-empty", place( gtk_progress_bar_new(), 10, 450, 200, 22 ) );
	gtk_progress_bar_set_fraction( GTK_PROGRESS_BAR( w ), 0.0 );
	w = name( "progress-full", place( gtk_progress_bar_new(), 230, 450, 200, 22 ) );
	gtk_progress_bar_set_fraction( GTK_PROGRESS_BAR( w ), 1.0 );
	w = name( "slider-hover", place( gtk_hscale_new_with_range( 0, 100, 1 ), 450, 450, 180, 22 ) );
	gtk_scale_set_draw_value( GTK_SCALE( w ), FALSE );
	gtk_range_set_value( GTK_RANGE( w ), 40 );
	g_signal_connect( w, "realize", G_CALLBACK( hover_after_realize ), NULL );
	name( "label", place( gtk_label_new( "Group box" ), 460, 410, 100, 22 ) );

	gtk_widget_show_all( win );
	gtk_window_move( GTK_WINDOW( win ), 0, 0 );
	/* No window manager under Xvfb: tell the window it has focus so the
	   entry draws its focused look. */
	gtk_widget_grab_focus( focused );
	{
		GdkEvent *fe = gdk_event_new( GDK_FOCUS_CHANGE );
		fe->focus_change.type = GDK_FOCUS_CHANGE;
		fe->focus_change.in = TRUE;
		fe->focus_change.window = g_object_ref( win->window );
		gtk_widget_send_focus_change( win, fe );
		gdk_event_free( fe );
	}
	g_timeout_add( 700, grab, NULL );
	gtk_main();
	return 0;
}
