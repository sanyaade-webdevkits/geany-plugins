/*
 *		stree.c
 *      
 *      Copyright 2010 Alexander Petukhov <devel(at)apetukhov.ru>
 *      
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *      
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *      
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *      MA 02110-1301, USA.
 */

/*
 *		Contains function to manipulate stack trace tree view.
 */

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
	#include "config.h"
#endif
#include <geanyplugin.h>

#include "breakpoints.h"
#include "utils.h"
#include "debug_module.h"
#include "pixbuf.h"

/* Tree view columns */
enum
{
   S_ADRESS,
   S_FUNCTION,
   S_FILEPATH,
   S_LINE,
   S_LAST_VISIBLE,
   S_HAVE_SOURCE,
   S_THREAD_ID,
   S_N_COLUMNS
};

/* hash table to keep thread nodes in the tree */
static GHashTable *threads;

/* current_thread_id */
static glong current_thread_id = 0;

/* double click callback pointer */
static move_to_line_cb callback = NULL;

/* tree view, model and store handles */
static GtkWidget* tree = NULL;
static GtkTreeModel* model = NULL;
static GtkTreeStore* store = NULL;

/* selection callback id */
static gulong selection_callback = 0;

/* 
 * shows a tooltip for a file name
 */
static gboolean on_query_tooltip(GtkWidget *widget, gint x, gint y, gboolean keyboard_mode, GtkTooltip *tooltip, gpointer user_data)
{
	gboolean show = FALSE;
	int bx, by;
	gtk_tree_view_convert_widget_to_bin_window_coords(GTK_TREE_VIEW(widget), x, y, &bx, &by);

	GtkTreePath *tpath = NULL;
	GtkTreeViewColumn *column = NULL;
	if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), bx, by, &tpath, &column, NULL, NULL))
	{
		if (2 == gtk_tree_path_get_depth(tpath) && column == gtk_tree_view_get_column(GTK_TREE_VIEW(widget), S_FILEPATH))
		{
			GtkTreeIter iter;
			gtk_tree_model_get_iter(model, &iter, tpath);
			
			gchar *path = NULL;
			gtk_tree_model_get(model, &iter, S_FILEPATH, &path, -1);

			gtk_tooltip_set_text(tooltip, path);
			
			gtk_tree_view_set_tooltip_row(GTK_TREE_VIEW(widget), tooltip, tpath);

			show = TRUE;

			g_free(path);
		}
		gtk_tree_path_free(tpath);
	}
	
	return show;
}

/* 
 * shows arrow icon for the current frame, hides renderer for a thread row
 */
static void on_render_icon(GtkTreeViewColumn *tree_column, GtkCellRenderer *cell, GtkTreeModel *tree_model,
	GtkTreeIter *iter, gpointer data)
{
	GtkTreePath *tpath = gtk_tree_model_get_path(model, iter);
	gboolean frame_row = 1 != gtk_tree_path_get_depth(tpath);

	g_object_set(cell, "visible", frame_row, NULL);
	g_object_set(cell, "pixbuf", (frame_row && 0 == gtk_tree_path_get_indices(tpath)[1]) ? (gpointer)frame_current_pixbuf : NULL, NULL);

	gtk_tree_path_free(tpath);
}

/* 
 * empty line renderer text for thread row
 */
static void on_render_line(GtkTreeViewColumn *tree_column, GtkCellRenderer *cell, GtkTreeModel *tree_model,
	GtkTreeIter *iter, gpointer data)
{
	GtkTreePath *tpath = gtk_tree_model_get_path(model, iter);
	
	if (1 == gtk_tree_path_get_depth(tpath))
	{
		g_object_set(cell, "text", "", NULL);
	}

	gtk_tree_path_free(tpath);
}

/* 
 * shows only the file name instead of a full path
 */
static void on_render_filename(GtkTreeViewColumn *tree_column, GtkCellRenderer *cell, GtkTreeModel *tree_model,
	GtkTreeIter *iter, gpointer data)
{
	GtkTreePath *tpath = gtk_tree_model_get_path(model, iter);
	
	if (1 != gtk_tree_path_get_depth(tpath))
	{
		gchar *path = NULL;
		gtk_tree_model_get(model, iter, S_FILEPATH, &path, -1);

		const gchar *name = path ? g_basename(path) : NULL;
		g_object_set(cell, "text", name ? name : path, NULL);

		if (path)
		{
			g_free(path);
		}
	}
	else
	{
		g_object_set(cell, "text", "", NULL);
	}

	gtk_tree_path_free(tpath);
}

/*
 *  Handles same tree row click to open frame position
 */
static gboolean on_msgwin_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	if (event->type == GDK_BUTTON_PRESS)
	{
		GtkTreePath *pressed_path = NULL;
		if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tree), (int)event->x, (int)event->y, &pressed_path, NULL, NULL, NULL))
		{
			if (2 == gtk_tree_path_get_depth(pressed_path))
			{
				GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
				GList *rows = gtk_tree_selection_get_selected_rows(selection, &model);
				GtkTreePath *selected_path = (GtkTreePath*)rows->data;

				if (!gtk_tree_path_compare(pressed_path, selected_path))
				{
					GtkTreeIter iter;
					gtk_tree_model_get_iter (
						 model,
						 &iter,
						 pressed_path);

					gboolean have_source;
					gtk_tree_model_get (
						model,
						&iter,
						S_HAVE_SOURCE, &have_source,
						-1);
					
					/* check if file name is not empty and we have source files for the frame */
					if (have_source)
					{
						gchar *file;
						gint line;
						gtk_tree_model_get (
							model,
							&iter,
							S_FILEPATH, &file,
							S_LINE, &line,
							-1);
						callback(file, line);

						g_free(file);
					}
				}

				g_list_foreach (rows, (GFunc) gtk_tree_path_free, NULL);
				g_list_free (rows);
			}

			gtk_tree_path_free(pressed_path);
		}
	}

	return FALSE;
}

/*
 *  Tree view selection changed callback
 */
void on_selection_changed(GtkTreeSelection *treeselection, gpointer user_data)
{
	GList *rows = gtk_tree_selection_get_selected_rows(treeselection, &model);
	GtkTreePath *path = (GtkTreePath*)rows->data;

	if (2 == gtk_tree_path_get_depth(path))
	{
		GtkTreeIter iter;
		gtk_tree_model_get_iter (
			 gtk_tree_view_get_model(GTK_TREE_VIEW(tree)),
			 &iter,
			 path);
		gboolean have_source;
		gtk_tree_model_get (
			gtk_tree_view_get_model(GTK_TREE_VIEW(tree)),
			&iter,
			S_HAVE_SOURCE, &have_source,
			-1);
		
		/* check if file name is not empty and we have source files for the frame */
		if (have_source)
		{
			gchar *file;
			gint line;
			gtk_tree_model_get (
				model,
				&iter,
				S_FILEPATH, &file,
				S_LINE, &line,
				-1);
			callback(file, line);
	
			g_free(file);
		}
	}

	g_list_foreach (rows, (GFunc)gtk_tree_path_free, NULL);
	g_list_free(rows);
}

/*
 *	inits stack trace tree
 */
GtkWidget* stree_init(move_to_line_cb cb)
{
	callback = cb;

	/* create tree view */
	store = gtk_tree_store_new (
		S_N_COLUMNS,
		G_TYPE_STRING,
		G_TYPE_STRING,
		G_TYPE_STRING,
		G_TYPE_INT,
		G_TYPE_STRING,
		G_TYPE_INT,
		G_TYPE_INT);
		
	model = GTK_TREE_MODEL(store);
	tree = gtk_tree_view_new_with_model (GTK_TREE_MODEL(store));
	
	/* set tree view properties */
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), 1);
	gtk_widget_set_has_tooltip(tree, TRUE);
	gtk_tree_view_set_show_expanders(GTK_TREE_VIEW(tree), FALSE);
	
	/* connect signals */

	selection_callback = g_signal_connect(G_OBJECT(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree))), "changed", G_CALLBACK (on_selection_changed), NULL);

	/* for clicking on already selected frame */
	g_signal_connect(G_OBJECT(tree), "button-press-event", G_CALLBACK(on_msgwin_button_press), NULL);

	g_signal_connect(G_OBJECT(tree), "query-tooltip", G_CALLBACK (on_query_tooltip), NULL);

	/* creating columns */
	GtkTreeViewColumn	*column;

	/* address */
	column = gtk_tree_view_column_new();
	gtk_tree_view_column_set_title(column, _("Address"));

	GtkCellRenderer *renderer_arrow = gtk_cell_renderer_pixbuf_new ();
	gtk_tree_view_column_pack_start(column, renderer_arrow, TRUE);
	gtk_tree_view_column_set_cell_data_func(column, renderer_arrow, on_render_icon, NULL, NULL);

	GtkCellRenderer *renderer_address = gtk_cell_renderer_text_new ();
	gtk_tree_view_column_pack_start(column, renderer_address, TRUE);
	gtk_tree_view_column_set_attributes(column, renderer_address, "text", S_ADRESS, NULL);

	gtk_tree_view_column_set_resizable (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	/* function */
	GtkCellRenderer *renderer_function = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Function"), renderer_function, "text", S_FUNCTION, NULL);
	gtk_tree_view_column_set_resizable (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);
	
	/* file */
	GtkCellRenderer *renderer_file = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("File"), renderer_file, NULL);
	gtk_tree_view_column_set_resizable (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);
	gtk_tree_view_column_set_cell_data_func(column, renderer_file, on_render_filename, NULL, NULL);
	
	/* line */
	GtkCellRenderer *renderer_line = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes (_("Line"), renderer_line, "text", S_LINE, NULL);
	gtk_tree_view_column_set_cell_data_func(column, renderer_line, on_render_line, NULL, NULL);
	gtk_tree_view_column_set_resizable (column, TRUE);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	/* Last invisible column */
	GtkCellRenderer *renderer_last = gtk_cell_renderer_text_new ();
	column = gtk_tree_view_column_new_with_attributes ("", renderer_last, "text", S_LAST_VISIBLE, NULL);
	gtk_tree_view_append_column (GTK_TREE_VIEW (tree), column);

	/* create threads hash table */
	threads =  g_hash_table_new_full(
		g_direct_hash,
		g_direct_equal,
		NULL,
		(GDestroyNotify)gtk_tree_row_reference_free
	);
		
	return tree;
}

/*
 *	add frame to the tree view
 */
void stree_add(frame *f)
{
	GtkTreeRowReference *reference = (GtkTreeRowReference*)g_hash_table_lookup(threads, (gpointer)current_thread_id);
	GtkTreeIter thread_iter;
	gtk_tree_model_get_iter(model, &thread_iter, gtk_tree_row_reference_get_path(reference));

	GtkTreeIter frame_iter;
	gtk_tree_store_insert_before(store, &frame_iter, &thread_iter, 0);

	gtk_tree_store_set (store, &frame_iter,
                    S_ADRESS, f->address,
                    S_FUNCTION, f->function,
                    S_FILEPATH, f->file,
                    S_LINE, f->line,
                    S_HAVE_SOURCE, f->have_source,
                    -1);
}

/*
 *	clear tree view completely
 */
void stree_clear()
{
	g_signal_handler_disconnect(G_OBJECT(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree))), selection_callback);
	
	gtk_tree_store_clear(store);
	g_hash_table_remove_all(threads);

	selection_callback = g_signal_connect(G_OBJECT(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree))), "changed", G_CALLBACK (on_selection_changed), NULL);
}

/*
 *	select first frame in the stack
 */
void stree_select_first_frame()
{
	gtk_tree_view_expand_all(GTK_TREE_VIEW(tree));
	
	GtkTreeRowReference *reference = (GtkTreeRowReference*)g_hash_table_lookup(threads, (gpointer)current_thread_id);
	GtkTreeIter thread_iter, frame_iter;
	gtk_tree_model_get_iter(model, &thread_iter, gtk_tree_row_reference_get_path(reference));
	if(gtk_tree_model_iter_children(model, &frame_iter, &thread_iter))
	{
		GtkTreePath* path = gtk_tree_model_get_path(model, &frame_iter);
		
		gtk_tree_selection_select_path (
			gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)),
			path);
		
		gtk_tree_path_free(path);
	}
}

/*
 *	called on plugin exit to free module data
 */
void stree_destroy()
{
	g_hash_table_destroy(threads);
	threads = NULL;
}

/*
 *	add new thread to the tree view
 */
void stree_add_thread(int thread_id)
{
	GtkTreeIter thread_iter, new_thread_iter;
	if (gtk_tree_model_get_iter_first(model, &thread_iter))
	{
		GtkTreeIter *consecutive = NULL;
		do
		{
			int existing_thread_id;
			gtk_tree_model_get(model, &thread_iter, S_THREAD_ID, &existing_thread_id);
			if (existing_thread_id > thread_id)
			{
				consecutive = &thread_iter;
				break;
			}
		}
		while(gtk_tree_model_iter_next(model, &thread_iter));

		if(consecutive)
		{
			gtk_tree_store_prepend(store, &new_thread_iter, consecutive);
		}
		else
		{
			gtk_tree_store_append(store, &new_thread_iter, NULL);
		}
	}
	else
	{
		gtk_tree_store_append (store, &new_thread_iter, NULL);
	}

	gchar *thread_label = g_strdup_printf(_("Thread %i"), thread_id);
	gtk_tree_store_set (store, &new_thread_iter,
					S_ADRESS, thread_label,
					S_THREAD_ID, thread_id,
					-1);
	g_free(thread_label);

	GtkTreePath *tpath = gtk_tree_model_get_path(model, &new_thread_iter);
	GtkTreeRowReference *reference = gtk_tree_row_reference_new(model, tpath);
	g_hash_table_insert(threads, (gpointer)(long)thread_id,(gpointer)reference);
	gtk_tree_path_free(tpath);
}

/*
 *	remove a thread from the tree view
 */
void stree_remove_thread(int thread_id)
{
	GtkTreeRowReference *reference = (GtkTreeRowReference*)g_hash_table_lookup(threads, (gpointer)(glong)thread_id);
	GtkTreePath *tpath = gtk_tree_row_reference_get_path(reference);

	GtkTreeIter iter;
	gtk_tree_model_get_iter(model, &iter, tpath);

	gtk_tree_store_remove(store, &iter);
	g_hash_table_remove(threads, (gpointer)(glong)thread_id);
}

/*
 *	remove all frames
 */
void stree_remove_frames()
{
	GtkTreeRowReference *reference = (GtkTreeRowReference*)g_hash_table_lookup(threads, (gpointer)current_thread_id);
	GtkTreeIter thread_iter;
	gtk_tree_model_get_iter(model, &thread_iter, gtk_tree_row_reference_get_path(reference));

	g_signal_handler_disconnect(G_OBJECT(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree))), selection_callback);
	
	GtkTreeIter child;
	if (gtk_tree_model_iter_children(model, &child, &thread_iter))
	{
		while(gtk_tree_store_remove(GTK_TREE_STORE(model), &child))
			;
	}

	selection_callback = g_signal_connect(G_OBJECT(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree))), "changed", G_CALLBACK (on_selection_changed), NULL);
}

/*
 *	set current thread id
 */
void stree_set_current_thread_id(int thread_id)
{
	current_thread_id = thread_id;
}
