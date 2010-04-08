/*
 *      editor.c - this file is part of Geany, a fast and lightweight IDE
 *
 *      Copyright 2005-2010 Enrico Tröger <enrico(dot)troeger(at)uvena(dot)de>
 *      Copyright 2006-2010 Nick Treleaven <nick(dot)treleaven(at)btinternet(dot)com>
 *      Copyright 2009-2010 Frank Lanitz <frank(at)frank(dot)uvena(dot)de>
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
 *      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id$
 */

/**
 * @file editor.h
 * Editor-related functions for @ref GeanyEditor.
 * Geany uses the Scintilla editing widget, and this file is mostly built around
 * Scintilla's functionality.
 * @see sciwrappers.h.
 */
/* Callbacks for the Scintilla widget (ScintillaObject).
 * Most important is the sci-notify callback, handled in on_editor_notification().
 * This includes auto-indentation, comments, auto-completion, calltips, etc.
 * Also some general Scintilla-related functions.
 */


#include <ctype.h>
#include <string.h>

#include <gdk/gdkkeysyms.h>

#include "SciLexer.h"
#include "geany.h"

#include "support.h"
#include "editor.h"
#include "document.h"
#include "documentprivate.h"
#include "filetypes.h"
#include "sciwrappers.h"
#include "ui_utils.h"
#include "utils.h"
#include "dialogs.h"
#include "symbols.h"
#include "callbacks.h"
#include "templates.h"
#include "keybindings.h"
#include "project.h"
#include "projectprivate.h"


/* Note: use sciwrappers.h instead where possible.
 * Do not use SSM in files unrelated to scintilla. */
#define SSM(s, m, w, l) scintilla_send_message(s, m, w, l)


static GHashTable *snippet_hash = NULL;
static GQueue *snippet_offsets = NULL;
static gint snippet_cursor_insert_pos;

/* holds word under the mouse or keyboard cursor */
static gchar current_word[GEANY_MAX_WORD_LENGTH];

/* Initialised in keyfile.c. */
GeanyEditorPrefs editor_prefs;

EditorInfo editor_info = {current_word, -1};

static struct
{
	gchar *text;
	gboolean set;
	gchar *last_word;
	guint tag_index;
	gint pos;
	ScintillaObject *sci;
} calltip = {NULL, FALSE, NULL, 0, 0, NULL};

static gchar indent[100];


static void on_new_line_added(GeanyEditor *editor);
static gboolean handle_xml(GeanyEditor *editor, gint pos, gchar ch);
static void insert_indent_after_line(GeanyEditor *editor, gint line);
static void auto_multiline(GeanyEditor *editor, gint pos);
static gboolean is_code_style(gint lexer, gint style);
static gboolean is_string_style(gint lexer, gint style);
static void auto_close_chars(ScintillaObject *sci, gint pos, gchar c);
static void auto_table(GeanyEditor *editor, gint pos);
static void close_block(GeanyEditor *editor, gint pos);
static void editor_highlight_braces(GeanyEditor *editor, gint cur_pos);
static void read_current_word(GeanyEditor *editor, gint pos, gchar *word, size_t wordlen,
		const gchar *wc, gboolean stem);


void editor_snippets_free(void)
{
	g_hash_table_destroy(snippet_hash);
	g_queue_free(snippet_offsets);
}


void editor_snippets_init(void)
{
	gsize i, j, len = 0, len_keys = 0;
	gchar *sysconfigfile, *userconfigfile;
	gchar **groups_user, **groups_sys;
	gchar **keys_user, **keys_sys;
	gchar *value;
	GKeyFile *sysconfig = g_key_file_new();
	GKeyFile *userconfig = g_key_file_new();
	GHashTable *tmp;

	snippet_offsets = g_queue_new();

	sysconfigfile = g_strconcat(app->datadir, G_DIR_SEPARATOR_S, "snippets.conf", NULL);
	userconfigfile = g_strconcat(app->configdir, G_DIR_SEPARATOR_S, "snippets.conf", NULL);

	/* check for old autocomplete.conf files (backwards compatibility) */
	if (! g_file_test(userconfigfile, G_FILE_TEST_IS_REGULAR | G_FILE_TEST_IS_SYMLINK))
		setptr(userconfigfile,
			g_strconcat(app->configdir, G_DIR_SEPARATOR_S, "autocomplete.conf", NULL));

	/* load the actual config files */
	g_key_file_load_from_file(sysconfig, sysconfigfile, G_KEY_FILE_NONE, NULL);
	g_key_file_load_from_file(userconfig, userconfigfile, G_KEY_FILE_NONE, NULL);

	/* keys are strings, values are GHashTables, so use g_free and g_hash_table_destroy */
	snippet_hash =
		g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify) g_hash_table_destroy);

	/* first read all globally defined auto completions */
	groups_sys = g_key_file_get_groups(sysconfig, &len);
	for (i = 0; i < len; i++)
	{
		keys_sys = g_key_file_get_keys(sysconfig, groups_sys[i], &len_keys, NULL);
		/* create new hash table for the read section (=> filetype) */
		tmp = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
		g_hash_table_insert(snippet_hash, g_strdup(groups_sys[i]), tmp);

		for (j = 0; j < len_keys; j++)
		{
			g_hash_table_insert(tmp, g_strdup(keys_sys[j]),
						utils_get_setting_string(sysconfig, groups_sys[i], keys_sys[j], ""));
		}
		g_strfreev(keys_sys);
	}

	/* now read defined completions in user's configuration directory and add / replace them */
	groups_user = g_key_file_get_groups(userconfig, &len);
	for (i = 0; i < len; i++)
	{
		keys_user = g_key_file_get_keys(userconfig, groups_user[i], &len_keys, NULL);

		tmp = g_hash_table_lookup(snippet_hash, groups_user[i]);
		if (tmp == NULL)
		{	/* new key found, create hash table */
			tmp = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
			g_hash_table_insert(snippet_hash, g_strdup(groups_user[i]), tmp);
		}
		for (j = 0; j < len_keys; j++)
		{
			value = g_hash_table_lookup(tmp, keys_user[j]);
			if (value == NULL)
			{	/* value = NULL means the key doesn't yet exist, so insert */
				g_hash_table_insert(tmp, g_strdup(keys_user[j]),
						utils_get_setting_string(userconfig, groups_user[i], keys_user[j], ""));
			}
			else
			{	/* old key and value will be freed by destroy function (g_free) */
				g_hash_table_replace(tmp, g_strdup(keys_user[j]),
						utils_get_setting_string(userconfig, groups_user[i], keys_user[j], ""));
			}
		}
		g_strfreev(keys_user);
	}

	g_free(sysconfigfile);
	g_free(userconfigfile);
	g_strfreev(groups_sys);
	g_strfreev(groups_user);
	g_key_file_free(sysconfig);
	g_key_file_free(userconfig);
}


static gboolean on_editor_button_press_event(GtkWidget *widget, GdkEventButton *event,
											 gpointer data)
{
	GeanyEditor *editor = data;
	GeanyDocument *doc = editor->document;

	/* it's very unlikely we got a 'real' click even on 0, 0, so assume it is a
	 * fake event to show the editor menu triggered by a key event where we want to use the
	 * text cursor position. */
	if (event->x > 0.0 && event->y > 0.0)
		editor_info.click_pos = sci_get_position_from_xy(editor->sci,
			(gint)event->x, (gint)event->y, FALSE);
	else
		editor_info.click_pos = sci_get_current_position(editor->sci);

	if (event->button == 1)
	{
		guint state = event->state & gtk_accelerator_get_default_mod_mask();

		if (event->type == GDK_BUTTON_PRESS && editor_prefs.disable_dnd)
		{
			gint ss = sci_get_selection_start(editor->sci);
			sci_set_selection_end(editor->sci, ss);
		}
		if (event->type == GDK_BUTTON_PRESS && state == GDK_CONTROL_MASK)
		{
			sci_set_current_position(editor->sci, editor_info.click_pos, FALSE);

			editor_find_current_word(editor, editor_info.click_pos,
				current_word, sizeof current_word, NULL);
			if (*current_word)
				return symbols_goto_tag(current_word, TRUE);
			else
				keybindings_send_command(GEANY_KEY_GROUP_GOTO, GEANY_KEYS_GOTO_MATCHINGBRACE);
			return TRUE;
		}
		return document_check_disk_status(doc, FALSE);
	}

	/* calls the edit popup menu in the editor */
	if (event->button == 3)
	{
		gboolean can_goto;

		editor_find_current_word(editor, editor_info.click_pos,
			current_word, sizeof current_word, NULL);

		can_goto = sci_has_selection(editor->sci) || current_word[0] != '\0';
		ui_update_popup_goto_items(can_goto);
		ui_update_popup_copy_items(doc);
		ui_update_insert_include_item(doc, 0);

		g_signal_emit_by_name(geany_object, "update-editor-menu",
			current_word, editor_info.click_pos, doc);

		gtk_menu_popup(GTK_MENU(main_widgets.editor_menu),
			NULL, NULL, NULL, NULL, event->button, event->time);

		return TRUE;
	}
	return FALSE;
}


static gboolean is_style_php(gint style)
{
	if ((style >= SCE_HPHP_DEFAULT && style <= SCE_HPHP_OPERATOR) ||
		style == SCE_HPHP_COMPLEX_VARIABLE)
	{
		return TRUE;
	}

	return FALSE;
}


gint editor_get_long_line_type(void)
{
	if (app->project)
		switch (app->project->long_line_behaviour)
		{
			case 0: /* marker disabled */
				return 2;
			case 1: /* use global settings */
				break;
			case 2: /* custom (enabled) */
				return editor_prefs.long_line_global_type;
		}

	if (!editor_prefs.long_line_global_enabled)
		return 2;
	else
		return editor_prefs.long_line_global_type;
}


gint editor_get_long_line_column(void)
{
	if (app->project && app->project->long_line_behaviour != 1 /* use global settings */)
		return app->project->long_line_column;
	else
		return editor_prefs.long_line_global_column;
}


void editor_toggle_fold(GeanyEditor *editor, gint line, gint modifiers)
{
	ScintillaObject *sci;

	g_return_if_fail(editor != NULL);

	sci = editor->sci;

	sci_toggle_fold(sci, line);

	/* extra toggling of child fold points
	 * use when editor_prefs.unfold_all_children is set and Shift is NOT pressed or when
	 * editor_prefs.unfold_all_children is NOT set but Shift is pressed */
	if ((editor_prefs.unfold_all_children && ! (modifiers & SCMOD_SHIFT)) ||
		(! editor_prefs.unfold_all_children && (modifiers & SCMOD_SHIFT)))
	{
		gint last_line = SSM(sci, SCI_GETLASTCHILD, line, -1);
		gint i;

		if (sci_get_line_is_visible(sci, line + 1))
		{	/* unfold all children of the current fold point */
			for (i = line; i < last_line; i++)
			{
				if (! sci_get_line_is_visible(sci, i))
				{
					sci_toggle_fold(sci, sci_get_fold_parent(sci, i));
				}
			}
		}
		else
		{	/* fold all children of the current fold point */
			for (i = line; i < last_line; i++)
			{
				gint level = sci_get_fold_level(sci, i);
				if (level & SC_FOLDLEVELHEADERFLAG)
				{
					if (sci_get_fold_expanded(sci, i))
						sci_toggle_fold(sci, i);
				}
			}
		}
	}
}


static void on_margin_click(GeanyEditor *editor, SCNotification *nt)
{
	/* left click to marker margin marks the line */
	if (nt->margin == 1)
	{
		gint line = sci_get_line_from_position(editor->sci, nt->position);

		/*sci_marker_delete_all(editor->sci, 1);*/
		sci_toggle_marker_at_line(editor->sci, line, 1);	/* toggle the marker */
	}
	/* left click on the folding margin to toggle folding state of current line */
	else if (nt->margin == 2 && editor_prefs.folding)
	{
		gint line = sci_get_line_from_position(editor->sci, nt->position);
		editor_toggle_fold(editor, line, nt->modifiers);
	}
}


static void on_update_ui(GeanyEditor *editor, G_GNUC_UNUSED SCNotification *nt)
{
	ScintillaObject *sci = editor->sci;
	gint pos = sci_get_current_position(sci);

	/* undo / redo menu update */
	ui_update_popup_reundo_items(editor->document);

	/* brace highlighting */
	editor_highlight_braces(editor, pos);

	ui_update_statusbar(editor->document, pos);

	/* Visible lines are only laid out accurately once [SCN_UPDATEUI] is sent,
	 * so we need to only call sci_scroll_to_line here, because the document
	 * may have line wrapping and folding enabled.
	 * http://scintilla.sourceforge.net/ScintillaDoc.html#LineWrapping */
	if (editor->scroll_percent > 0.0F)
	{
		editor_scroll_to_line(editor, -1, editor->scroll_percent);
		editor->scroll_percent = -1.0F;	/* disable further scrolling */
	}
#if 0
	/** experimental code for inverting selections */
	{
	gint i;
	for (i = SSM(sci, SCI_GETSELECTIONSTART, 0, 0); i < SSM(sci, SCI_GETSELECTIONEND, 0, 0); i++)
	{
		/* need to get colour from getstyleat(), but how? */
		SSM(sci, SCI_STYLESETFORE, STYLE_DEFAULT, 0);
		SSM(sci, SCI_STYLESETBACK, STYLE_DEFAULT, 0);
	}

	sci_get_style_at(sci, pos);
	}
#endif
}


static void check_line_breaking(GeanyEditor *editor, gint pos, gchar c)
{
	ScintillaObject *sci = editor->sci;
	gint line, lstart, col;

	if (!editor->line_breaking)
		return;

	col = sci_get_col_from_position(sci, pos);

	if (c == GDK_space)
		pos--;	/* Look for previous space, not the new one */

	line = sci_get_current_line(sci);

	lstart = sci_get_position_from_line(sci, line);

	/* use column instead of position which might be different with multibyte characters */
	if (col < editor_prefs.line_break_column)
		return;

	/* look for the last space before line_break_column */
	pos = MIN(pos, lstart + editor_prefs.line_break_column);

	while (pos > lstart)
	{
		c = sci_get_char_at(sci, --pos);
		if (c == GDK_space)
		{
			gint diff, last_pos, last_col;
			const gchar *eol = editor_get_eol_char(editor);

			/* remember the distance between the current column and the last column on the line
			 * (we use column position in case the previous line gets altered, such as removing
			 * trailing spaces or in case it contains multibyte characters) */
			last_pos = sci_get_line_end_position(sci, line);
			last_col = sci_get_col_from_position(sci, last_pos);
			diff = last_col - col;

			/* break the line after the space */
			sci_insert_text(sci, pos + 1, eol);
			line++;

			/* set position as if user had pressed return */
			pos = sci_get_position_from_line(sci, line);
			sci_set_current_position(sci, pos, FALSE);
			/* add indentation, comment multilines, etc */
			on_new_line_added(editor);

			/* correct cursor position (might not be at line end) */
			last_pos = sci_get_line_end_position(sci, line);
			last_col = sci_get_col_from_position(sci, last_pos); /* get last column on line */
			/* last column - distance is the desired column, then retrieve its document position */
			pos = SSM(sci, SCI_FINDCOLUMN, line, last_col - diff);
			sci_set_current_position(sci, pos, FALSE);

			return;
		}
	}
}


static void show_autocomplete(ScintillaObject *sci, gint rootlen, const gchar *words)
{
	/* store whether a calltip is showing, so we can reshow it after autocompletion */
	calltip.set = SSM(sci, SCI_CALLTIPACTIVE, 0, 0);
	SSM(sci, SCI_AUTOCSHOW, rootlen, (sptr_t) words);
}


static void show_tags_list(GeanyEditor *editor, const GPtrArray *tags, gsize rootlen)
{
	ScintillaObject *sci = editor->sci;

	g_return_if_fail(tags);

	if (tags->len > 0)
	{
		GString *words = g_string_sized_new(150);
		guint j;

		for (j = 0; j < tags->len; ++j)
		{
			TMTag *tag = tags->pdata[j];

			if (j > 0)
				g_string_append_c(words, '\n');

			if (j == editor_prefs.autocompletion_max_entries)
			{
				g_string_append(words, "...");
				break;
			}
			g_string_append(words, tag->name);

			/* for now, tag types don't all follow C, so just look at arglist */
			if (NZV(tag->atts.entry.arglist))
				g_string_append(words, "?2");
			else
				g_string_append(words, "?1");
		}
		show_autocomplete(sci, rootlen, words->str);
		g_string_free(words, TRUE);
	}
}


/* do not use with long strings */
static gboolean match_last_chars(ScintillaObject *sci, gint pos, const gchar *str)
{
	gsize len = strlen(str);
	gchar *buf;

	g_return_val_if_fail(len < 100, FALSE);

	buf = g_alloca(len + 1);
	sci_get_text_range(sci, pos - len, pos, buf);
	return strcmp(str, buf) == 0;
}


static gboolean reshow_calltip(gpointer data)
{
	GeanyDocument *doc;

	g_return_val_if_fail(calltip.sci != NULL, FALSE);

	SSM(calltip.sci, SCI_CALLTIPCANCEL, 0, 0);
	doc = document_get_current();

	if (doc && doc->editor->sci == calltip.sci)
	{
		/* we use the position where the calltip was previously started as SCI_GETCURRENTPOS
		 * may be completely wrong in case the user cancelled the auto completion with the mouse */
		SSM(calltip.sci, SCI_CALLTIPSHOW, calltip.pos, (sptr_t) calltip.text);
	}
	return FALSE;
}


static void request_reshowing_calltip(SCNotification *nt)
{
	if (calltip.set)
	{
		/* delay the reshow of the calltip window to make sure it is actually displayed,
		 * without it might be not visible on SCN_AUTOCCANCEL */
		g_idle_add(reshow_calltip, NULL);
	}
}


static void autocomplete_scope(GeanyEditor *editor)
{
	ScintillaObject *sci = editor->sci;
	gint pos = sci_get_current_position(editor->sci);
	gchar typed = sci_get_char_at(sci, pos - 1);
	gchar *name;
	const GPtrArray *tags = NULL;
	const TMTag *tag;
	GeanyFiletype *ft = editor->document->file_type;

	if (ft->id == GEANY_FILETYPES_C || ft->id == GEANY_FILETYPES_CPP)
	{
		if (match_last_chars(sci, pos, "->") || match_last_chars(sci, pos, "::"))
			pos--;
		else if (typed != '.')
			return;
	}
	else if (typed != '.')
		return;

	/* allow for a space between word and operator */
	if (isspace(sci_get_char_at(sci, pos - 2)))
		pos--;
	name = editor_get_word_at_pos(editor, pos - 1, NULL);
	if (!name)
		return;

	tags = tm_workspace_find(name, tm_tag_max_t, NULL, FALSE, ft->lang);
	g_free(name);
	if (!tags || tags->len == 0)
		return;

	tag = g_ptr_array_index(tags, 0);
	name = tag->atts.entry.var_type;
	if (name)
	{
		TMWorkObject *obj = editor->document->tm_file;

		tags = tm_workspace_find_scope_members(obj ? obj->tags_array : NULL,
			name, TRUE, FALSE);
		if (tags)
			show_tags_list(editor, tags, 0);
	}
}


static void on_char_added(GeanyEditor *editor, SCNotification *nt)
{
	ScintillaObject *sci = editor->sci;
	gint pos = sci_get_current_position(sci);

	switch (nt->ch)
	{
		case '\r':
		{	/* simple indentation (only for CR format) */
			if (sci_get_eol_mode(sci) == SC_EOL_CR)
				on_new_line_added(editor);
			break;
		}
		case '\n':
		{	/* simple indentation (for CR/LF and LF format) */
			on_new_line_added(editor);
			break;
		}
		case '>':
			editor_start_auto_complete(editor, pos, FALSE);	/* C/C++ ptr-> scope completion */
			/* fall through */
		case '/':
		{	/* close xml-tags */
			handle_xml(editor, pos, nt->ch);
			break;
		}
		case '(':
		{
			auto_close_chars(sci, pos, nt->ch);
			/* show calltips */
			editor_show_calltip(editor, --pos);
			break;
		}
		case ')':
		{	/* hide calltips */
			if (SSM(sci, SCI_CALLTIPACTIVE, 0, 0))
			{
				SSM(sci, SCI_CALLTIPCANCEL, 0, 0);
			}
			g_free(calltip.text);
			calltip.text = NULL;
			calltip.pos = 0;
			calltip.sci = NULL;
			calltip.set = FALSE;
			break;
		}
		case '{':
		case '[':
		case '"':
		case '\'':
		{
			auto_close_chars(sci, pos, nt->ch);
			break;
		}
		case '}':
		{	/* closing bracket handling */
			if (editor->auto_indent)
				close_block(editor, pos - 1);
			break;
		}
		/* scope autocompletion */
		case '.':
		case ':':	/* C/C++ class:: syntax */
		/* tag autocompletion */
		default:
#if 0
			if (! editor_start_auto_complete(editor, pos, FALSE))
				request_reshowing_calltip(nt);
#else
			editor_start_auto_complete(editor, pos, FALSE);
#endif
	}
	check_line_breaking(editor, pos, nt->ch);
}


/* expand() and fold_changed() are copied from SciTE (thanks) to fix #1923350. */
static void expand(ScintillaObject *sci, gint *line, gboolean doExpand,
		gboolean force, gint visLevels, gint level)
{
	gint lineMaxSubord = SSM(sci, SCI_GETLASTCHILD, *line, level & SC_FOLDLEVELNUMBERMASK);
	gint levelLine = level;
	(*line)++;
	while (*line <= lineMaxSubord)
	{
		if (G_UNLIKELY(force))
		{
			if (visLevels > 0)
				SSM(sci, SCI_SHOWLINES, *line, *line);
			else
				SSM(sci, SCI_HIDELINES, *line, *line);
		}
		else
		{
			if (doExpand)
				SSM(sci, SCI_SHOWLINES, *line, *line);
		}
		if (levelLine == -1)
			levelLine = SSM(sci, SCI_GETFOLDLEVEL, *line, 0);
		if (levelLine & SC_FOLDLEVELHEADERFLAG)
		{
			if (G_UNLIKELY(force))
			{
				if (visLevels > 1)
					SSM(sci, SCI_SETFOLDEXPANDED, *line, 1);
				else
					SSM(sci, SCI_SETFOLDEXPANDED, *line, 0);
				expand(sci, line, doExpand, force, visLevels - 1, -1);
			}
			else
			{
				if (doExpand)
				{
					if (!sci_get_fold_expanded(sci, *line))
						SSM(sci, SCI_SETFOLDEXPANDED, *line, 1);
					expand(sci, line, TRUE, force, visLevels - 1, -1);
				}
				else
				{
					expand(sci, line, FALSE, force, visLevels - 1, -1);
				}
			}
		}
		else
		{
			(*line)++;
		}
	}
}


static void fold_changed(ScintillaObject *sci, gint line, gint levelNow, gint levelPrev)
{
	if (levelNow & SC_FOLDLEVELHEADERFLAG)
	{
		if (! (levelPrev & SC_FOLDLEVELHEADERFLAG))
		{
			/* Adding a fold point */
			SSM(sci, SCI_SETFOLDEXPANDED, line, 1);
			expand(sci, &line, TRUE, FALSE, 0, levelPrev);
		}
	}
	else if (levelPrev & SC_FOLDLEVELHEADERFLAG)
	{
		if (! sci_get_fold_expanded(sci, line))
		{	/* Removing the fold from one that has been contracted so should expand
			 * otherwise lines are left invisible with no way to make them visible */
			SSM(sci, SCI_SETFOLDEXPANDED, line, 1);
			expand(sci, &line, TRUE, FALSE, 0, levelPrev);
		}
	}
	if (! (levelNow & SC_FOLDLEVELWHITEFLAG) &&
			((levelPrev & SC_FOLDLEVELNUMBERMASK) > (levelNow & SC_FOLDLEVELNUMBERMASK)))
	{
		/* See if should still be hidden */
		gint parentLine = sci_get_fold_parent(sci, line);
		if (parentLine < 0)
		{
			SSM(sci, SCI_SHOWLINES, line, line);
		}
		else if (sci_get_fold_expanded(sci, parentLine) &&
				sci_get_line_is_visible(sci, parentLine))
		{
			SSM(sci, SCI_SHOWLINES, line, line);
		}
	}
}


static void ensure_range_visible(ScintillaObject *sci, gint posStart, gint posEnd,
		gboolean enforcePolicy)
{
	gint lineStart = sci_get_line_from_position(sci, MIN(posStart, posEnd));
	gint lineEnd = sci_get_line_from_position(sci, MAX(posStart, posEnd));
	gint line;

	for (line = lineStart; line <= lineEnd; line++)
	{
		SSM(sci, enforcePolicy ? SCI_ENSUREVISIBLEENFORCEPOLICY : SCI_ENSUREVISIBLE, line, 0);
	}
}


static void auto_update_margin_width(GeanyEditor *editor)
{
	gint next_linecount = 1;
	gint linecount = sci_get_line_count(editor->sci);
	GeanyDocument *doc = editor->document;

	while (next_linecount <= linecount)
		next_linecount *= 10;

	if (editor->document->priv->line_count != next_linecount)
	{
		doc->priv->line_count = next_linecount;
		sci_set_line_numbers(editor->sci, TRUE, 0);
	}
}


static void partial_complete(ScintillaObject *sci, const gchar *text)
{
	gint pos = sci_get_current_position(sci);

	sci_insert_text(sci, pos, text);
	sci_set_current_position(sci, pos + strlen(text), TRUE);
}


/* Complete the next word part from @a entry */
static gboolean check_partial_completion(GeanyEditor *editor, const gchar *entry)
{
	gchar *stem, *ptr, *text = utils_strdupa(entry);

	read_current_word(editor, -1, current_word, sizeof current_word, NULL, TRUE);
	stem = current_word;
	if (strstr(text, stem) != text)
		return FALSE;	/* shouldn't happen */
	if (strlen(text) <= strlen(stem))
		return FALSE;

	text += strlen(stem); /* skip stem */
	ptr = strstr(text + 1, "_");
	if (ptr)
	{
		ptr[1] = '\0';
		partial_complete(editor->sci, text);
		return TRUE;
	}
	else
	{
		/* CamelCase */
		foreach_str(ptr, text + 1)
		{
			if (!ptr[0])
				break;
			if (g_ascii_isupper(*ptr) && g_ascii_islower(ptr[1]))
			{
				ptr[0] = '\0';
				partial_complete(editor->sci, text);
				return TRUE;
			}
		}
	}
	return FALSE;
}


/* Callback for the "sci-notify" signal to emit a "editor-notify" signal.
 * Plugins can connect to the "editor-notify" signal. */
void editor_sci_notify_cb(G_GNUC_UNUSED GtkWidget *widget, G_GNUC_UNUSED gint scn,
						  gpointer scnt, gpointer data)
{
	GeanyEditor *editor = data;
	gboolean retval;

	g_return_if_fail(editor != NULL);

	g_signal_emit_by_name(geany_object, "editor-notify", editor, scnt, &retval);
}


static gboolean on_editor_notify(G_GNUC_UNUSED GObject *object, GeanyEditor *editor,
								 SCNotification *nt, G_GNUC_UNUSED gpointer data)
{
	ScintillaObject *sci = editor->sci;
	GeanyDocument *doc = editor->document;

	switch (nt->nmhdr.code)
	{
		case SCN_SAVEPOINTLEFT:
			document_set_text_changed(doc, TRUE);
			break;

		case SCN_SAVEPOINTREACHED:
			document_set_text_changed(doc, FALSE);
			break;

		case SCN_MODIFYATTEMPTRO:
			utils_beep();
			break;

		case SCN_MARGINCLICK:
			on_margin_click(editor, nt);
			break;

		case SCN_UPDATEUI:
			on_update_ui(editor, nt);
			break;

 		case SCN_MODIFIED:
			if (editor_prefs.show_linenumber_margin && (nt->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) && nt->linesAdded)
			{
				/* automatically adjust Scintilla's line numbers margin width */
				auto_update_margin_width(editor);
			}
			if (nt->modificationType & SC_STARTACTION && ! ignore_callback)
			{
				/* get notified about undo changes */
				document_undo_add(doc, UNDO_SCINTILLA, NULL);
			}
			if (editor_prefs.folding && (nt->modificationType & SC_MOD_CHANGEFOLD) != 0)
			{
				/* handle special fold cases, e.g. #1923350 */
				fold_changed(sci, nt->line, nt->foldLevelNow, nt->foldLevelPrev);
			}
			break;

		case SCN_CHARADDED:
			on_char_added(editor, nt);
			break;

		case SCN_USERLISTSELECTION:
			if (nt->listType == 1)
			{
				sci_add_text(sci, nt->text);
			}
			break;

		case SCN_AUTOCSELECTION:
			if (g_str_equal(nt->text, "..."))
			{
				sci_cancel(sci);
				utils_beep();
				break;
			}
			/* fall through */
		case SCN_AUTOCCANCELLED:
			/* now that autocomplete is finishing or was cancelled, reshow calltips
			 * if they were showing */
			request_reshowing_calltip(nt);
			break;

#ifdef GEANY_DEBUG
		case SCN_STYLENEEDED:
			geany_debug("style");
			break;
#endif
		case SCN_NEEDSHOWN:
			ensure_range_visible(sci, nt->position, nt->position + nt->length, FALSE);
			break;

		case SCN_URIDROPPED:
			if (nt->text != NULL)
			{
				document_open_file_list(nt->text, -1);
			}
			break;

		case SCN_CALLTIPCLICK:
			if (nt->position > 0)
			{
				switch (nt->position)
				{
					case 1:	/* up arrow */
						if (calltip.tag_index > 0)
							calltip.tag_index--;
						break;

					case 2: calltip.tag_index++; break;	/* down arrow */
				}
				editor_show_calltip(editor, -1);
			}
			break;
	}
	/* we always return FALSE here to let plugins handle the event too */
	return FALSE;
}


/* Note: this is the same as sci_get_tab_width(), but is still useful when you don't have
 * a scintilla pointer. */
static gint get_tab_width(const GeanyIndentPrefs *indent_prefs)
{
	if (indent_prefs->type == GEANY_INDENT_TYPE_BOTH)
		return indent_prefs->hard_tab_width;

	return indent_prefs->width;	/* tab width = indent width */
}


/* Returns a string containing width chars of whitespace, filled with simple space
 * characters or with the right number of tab characters, according to the indent prefs.
 * (Result is filled with tabs *and* spaces if width isn't a multiple of
 * the tab width). */
static gchar *
get_whitespace(const GeanyIndentPrefs *iprefs, gint width)
{
	g_return_val_if_fail(width >= 0, NULL);

	if (width == 0)
		return g_strdup("");

	if (iprefs->type == GEANY_INDENT_TYPE_SPACES)
	{
		return g_strnfill(width, ' ');
	}
	else
	{	/* first fill text with tabs and fill the rest with spaces */
		const gint tab_width = get_tab_width(iprefs);
		gint tabs = width / tab_width;
		gint spaces = width % tab_width;
		gint len = tabs + spaces;
		gchar *str;

		str = g_malloc(len + 1);

		memset(str, '\t', tabs);
		memset(str + tabs, ' ', spaces);
		str[len] = '\0';
		return str;
 	}
}


static const GeanyIndentPrefs *
get_default_indent_prefs(void)
{
	static GeanyIndentPrefs iprefs;

	iprefs = app->project ? *app->project->priv->indentation : *editor_prefs.indentation;
	return &iprefs;
}


/** Gets the indentation prefs for the editor.
 * In future, the prefs might be different according to project or filetype.
 * @warning Always get a fresh result instead of keeping a pointer to it if the editor
 * settings may have changed, or if this function has been called for a different @a editor.
 * @param editor The editor, or @c NULL to get the default indent prefs.
 * @return The indent prefs. */
const GeanyIndentPrefs *
editor_get_indent_prefs(GeanyEditor *editor)
{
	static GeanyIndentPrefs iprefs;

	iprefs = *get_default_indent_prefs();

	if (editor == NULL)
		return &iprefs;

	iprefs.type = editor->indent_type;

	/* if per-document auto-indent is enabled, but we don't have a global mode set,
	 * just use basic auto-indenting */
	if (editor->auto_indent && iprefs.auto_indent_mode == GEANY_AUTOINDENT_NONE)
		iprefs.auto_indent_mode = GEANY_AUTOINDENT_BASIC;

	if (!editor->auto_indent)
		iprefs.auto_indent_mode = GEANY_AUTOINDENT_NONE;

	return &iprefs;
}


static void on_new_line_added(GeanyEditor *editor)
{
	ScintillaObject *sci = editor->sci;
	gint line = sci_get_current_line(sci);

	/* simple indentation */
	if (editor->auto_indent)
	{
		insert_indent_after_line(editor, line - 1);
	}

	if (editor_prefs.auto_continue_multiline)
	{	/* " * " auto completion in multiline C/C++/D/Java comments */
		auto_multiline(editor, line);
	}

	if (editor_prefs.newline_strip)
	{	/* strip the trailing spaces on the previous line */
		editor_strip_line_trailing_spaces(editor, line - 1);
	}
}


static gboolean lexer_has_braces(ScintillaObject *sci)
{
	gint lexer = sci_get_lexer(sci);

	switch (lexer)
	{
		case SCLEX_CPP:
		case SCLEX_D:
		case SCLEX_HTML:	/* for PHP & JS */
		case SCLEX_PASCAL:	/* for multiline comments? */
		case SCLEX_BASH:
		case SCLEX_PERL:
		case SCLEX_TCL:
			return TRUE;
		default:
			return FALSE;
	}
}


/* Read indent chars for the line that pos is on into indent global variable.
 * Note: Use sci_get_line_indentation() and get_whitespace()/editor_insert_text_block()
 * instead in any new code.  */
static void read_indent(GeanyEditor *editor, gint pos)
{
	ScintillaObject *sci = editor->sci;
	guint i, len, j = 0;
	gint line;
	gchar *linebuf;

	line = sci_get_line_from_position(sci, pos);

	len = sci_get_line_length(sci, line);
	linebuf = sci_get_line(sci, line);

	for (i = 0; i < len && j <= (sizeof(indent) - 1); i++)
	{
		if (linebuf[i] == ' ' || linebuf[i] == '\t')	/* simple indentation */
			indent[j++] = linebuf[i];
		else
			break;
	}
	indent[j] = '\0';
	g_free(linebuf);
}


static gint get_brace_indent(ScintillaObject *sci, gint line)
{
	guint i, len;
	gint ret = 0;
	gchar *linebuf;

	len = sci_get_line_length(sci, line);
	linebuf = sci_get_line(sci, line);

	for (i = 0; i < len; i++)
	{
		/* i == (len - 1) prevents wrong indentation after lines like
		 * "	{ return bless({}, shift); }" (Perl) */
		if (linebuf[i] == '{' && i == (len - 1))
		{
			ret++;
			break;
		}
		else
		{
			gint k = len - 1;

			while (k > 0 && isspace(linebuf[k])) k--;

			/* if last non-whitespace character is a { increase indentation by a tab
			 * e.g. for (...) { */
			if (linebuf[k] == '{')
			{
				ret++;
			}
			break;
		}
	}
	g_free(linebuf);
	return ret;
}


static gint get_python_indent(ScintillaObject *sci, gint line)
{
	gint last_char = sci_get_line_end_position(sci, line) - 1;

	/* add extra indentation for Python after colon */
	if (sci_get_char_at(sci, last_char) == ':' &&
		sci_get_style_at(sci, last_char) == SCE_P_OPERATOR)
	{
		return 1;
	}
	return 0;
}


static gint get_indent_size_after_line(GeanyEditor *editor, gint line)
{
	ScintillaObject *sci = editor->sci;
	gint size;
	const GeanyIndentPrefs *iprefs = editor_get_indent_prefs(editor);

	g_return_val_if_fail(line >= 0, 0);

	size = sci_get_line_indentation(sci, line);

	if (iprefs->auto_indent_mode > GEANY_AUTOINDENT_BASIC)
	{
		if (lexer_has_braces(sci))
			size += iprefs->width * get_brace_indent(sci, line);
		else
		if (FILETYPE_ID(editor->document->file_type) == GEANY_FILETYPES_PYTHON)
			size += iprefs->width * get_python_indent(sci, line);
	}
	return size;
}


static void insert_indent_after_line(GeanyEditor *editor, gint line)
{
	ScintillaObject *sci = editor->sci;
	gint line_indent = sci_get_line_indentation(sci, line);
	gint size = get_indent_size_after_line(editor, line);
	const GeanyIndentPrefs *iprefs = editor_get_indent_prefs(editor);
	gchar *text;

	if (size == 0)
		return;

	if (iprefs->type == GEANY_INDENT_TYPE_TABS && size == line_indent)
	{
		/* support tab indents, space aligns style - copy last line 'indent' exactly */
		gint start = sci_get_position_from_line(sci, line);
		gint end = sci_get_line_indent_position(sci, line);

		text = sci_get_contents_range(sci, start, end);
	}
	else
	{
		text = get_whitespace(iprefs, size);
	}
	sci_add_text(sci, text);
	g_free(text);
}


static void auto_close_chars(ScintillaObject *sci, gint pos, gchar c)
{
	const gchar *closing_char = NULL;
	gint end_pos = -1;

	if (utils_isbrace(c, 0))
		end_pos = sci_find_matching_brace(sci, pos - 1);

	switch (c)
	{
		case '(':
			if ((editor_prefs.autoclose_chars & GEANY_AC_PARENTHESIS) && end_pos == -1)
				closing_char = ")";
			break;
		case '{':
			if ((editor_prefs.autoclose_chars & GEANY_AC_CBRACKET) && end_pos == -1)
				closing_char = "}";
			break;
		case '[':
			if ((editor_prefs.autoclose_chars & GEANY_AC_SBRACKET) && end_pos == -1)
				closing_char = "]";
			break;
		case '\'':
			if (editor_prefs.autoclose_chars & GEANY_AC_SQUOTE)
				closing_char = "'";
			break;
		case '"':
			if (editor_prefs.autoclose_chars & GEANY_AC_DQUOTE)
				closing_char = "\"";
			break;
	}

	if (closing_char != NULL)
	{
		sci_add_text(sci, closing_char);
		sci_set_current_position(sci, pos, TRUE);
	}
}


/* Finds a corresponding matching brace to the given pos
 * (this is taken from Scintilla Editor.cxx,
 * fit to work with close_block) */
static gint brace_match(ScintillaObject *sci, gint pos)
{
	gchar chBrace = sci_get_char_at(sci, pos);
	gchar chSeek = utils_brace_opposite(chBrace);
	gchar chAtPos;
	gint direction = -1;
	gint styBrace;
	gint depth = 1;
	gint styAtPos;

	styBrace = sci_get_style_at(sci, pos);

	if (utils_is_opening_brace(chBrace, editor_prefs.brace_match_ltgt))
		direction = 1;

	pos = pos + direction;
	while ((pos >= 0) && (pos < sci_get_length(sci)))
	{
		chAtPos = sci_get_char_at(sci, pos - 1);
		styAtPos = sci_get_style_at(sci, pos);

		if ((pos > sci_get_end_styled(sci)) || (styAtPos == styBrace))
		{
			if (chAtPos == chBrace)
				depth++;
			if (chAtPos == chSeek)
				depth--;
			if (depth == 0)
				return pos;
		}
		pos = pos + direction;
	}
	return -1;
}


/* Called after typing '}'. */
static void close_block(GeanyEditor *editor, gint pos)
{
	GeanyDocument *doc;
	const GeanyIndentPrefs *iprefs = editor_get_indent_prefs(editor);
	gint x = 0, cnt = 0;
	gint line, line_len, eol_char_len;
	gchar *text, *line_buf;
	ScintillaObject *sci;
	gint line_indent, last_indent;

	if (iprefs->auto_indent_mode < GEANY_AUTOINDENT_CURRENTCHARS)
		return;
	g_return_if_fail(editor != NULL && editor->document->file_type != NULL);

	sci = editor->sci;
	doc = editor->document;

	if (! lexer_has_braces(sci))
		return;

	line = sci_get_line_from_position(sci, pos);
	line_len = sci_get_line_length(sci, line);
	/* set eol_char_len to 0 if on last line, because there is no EOL char */
	eol_char_len = (line == (sci_get_line_count(sci) - 1)) ? 0 :
								editor_get_eol_char_len(editor);

	/* check that the line is empty, to not kill text in the line */
	line_buf = sci_get_line(sci, line);
	line_buf[line_len - eol_char_len] = '\0';
	while (x < (line_len - eol_char_len))
	{
		if (isspace(line_buf[x]))
			cnt++;
		x++;
	}
	g_free(line_buf);

	if ((line_len - eol_char_len - 1) != cnt)
		return;

	if (iprefs->auto_indent_mode == GEANY_AUTOINDENT_MATCHBRACES)
	{
		gint start_brace = brace_match(sci, pos);

		if (start_brace >= 0)
		{
			gint line_start;
			gint brace_line = sci_get_line_from_position(sci, start_brace);
			gint size = sci_get_line_indentation(sci, brace_line);
			gchar *ind = get_whitespace(iprefs, size);

			text = g_strconcat(ind, "}", NULL);
			line_start = sci_get_position_from_line(sci, line);
			sci_set_anchor(sci, line_start);
			sci_replace_sel(sci, text);
			g_free(text);
			g_free(ind);
			return;
		}
		/* fall through - unmatched brace (possibly because of TCL, PHP lexer bugs) */
	}

	/* GEANY_AUTOINDENT_CURRENTCHARS */
	line_indent = sci_get_line_indentation(sci, line);
	last_indent = sci_get_line_indentation(sci, line - 1);

	if (line_indent < last_indent)
		return;
	line_indent -= iprefs->width;
	line_indent = MAX(0, line_indent);
	sci_set_line_indentation(sci, line, line_indent);
}


/* Reads the word at given cursor position and writes it into the given buffer. The buffer will be
 * NULL terminated in any case, even when the word is truncated because wordlen is too small.
 * position can be -1, then the current position is used.
 * wc are the wordchars to use, if NULL, GEANY_WORDCHARS will be used */
static void read_current_word(GeanyEditor *editor, gint pos, gchar *word, size_t wordlen,
		const gchar *wc, gboolean stem)
{
	gint line, line_start, startword, endword;
	gchar *chunk;
	ScintillaObject *sci;

	g_return_if_fail(editor != NULL);
	sci = editor->sci;

	if (pos == -1)
		pos = sci_get_current_position(sci);

	line = sci_get_line_from_position(sci, pos);
	line_start = sci_get_position_from_line(sci, line);
	startword = pos - line_start;
	endword = pos - line_start;

	word[0] = '\0';
	chunk = sci_get_line(sci, line);

	if (wc == NULL)
		wc = GEANY_WORDCHARS;

	/* the checks for "c < 0" are to allow any Unicode character which should make the code
	 * a little bit more Unicode safe, anyway, this allows also any Unicode punctuation,
	 * TODO: improve this code */
	while (startword > 0 && (strchr(wc, chunk[startword - 1]) || chunk[startword - 1] < 0))
		startword--;
	if (!stem)
	{
		while (chunk[endword] != 0 && (strchr(wc, chunk[endword]) || chunk[endword] < 0))
			endword++;
	}

	if (startword != endword)
	{
		chunk[endword] = '\0';

		g_strlcpy(word, chunk + startword, wordlen); /* ensure null terminated */
	}
	else
		g_strlcpy(word, "", wordlen);

	g_free(chunk);
}


/* Reads the word at given cursor position and writes it into the given buffer. The buffer will be
 * NULL terminated in any case, even when the word is truncated because wordlen is too small.
 * position can be -1, then the current position is used.
 * wc are the wordchars to use, if NULL, GEANY_WORDCHARS will be used */
void editor_find_current_word(GeanyEditor *editor, gint pos, gchar *word, size_t wordlen,
							  const gchar *wc)
{
	read_current_word(editor, pos, word, wordlen, wc, FALSE);
}


/**
 *  Finds the word at the position specified by @a pos. If any word is found, it is returned.
 *  Otherwise NULL is returned.
 *  Additional wordchars can be specified to define what to consider as a word.
 *
 *  @param editor The editor to operate on.
 *  @param pos The position where the word should be read from.
 *             Maybe @c -1 to use the current position.
 *  @param wordchars The wordchars to separate words. wordchars mean all characters to count
 *                   as part of a word. Maybe @c NULL to use the default wordchars,
 *                   see @ref GEANY_WORDCHARS.
 *
 *  @return A newly-allocated string containing the word at the given @a pos or @c NULL.
 *          Should be freed when no longer needed.
 *
 *  @since 0.16
 */
gchar *editor_get_word_at_pos(GeanyEditor *editor, gint pos, const gchar *wordchars)
{
	static gchar cword[GEANY_MAX_WORD_LENGTH];

	g_return_val_if_fail(editor != NULL, FALSE);

	read_current_word(editor, pos, cword, sizeof(cword), wordchars, FALSE);

	return (*cword == '\0') ? NULL : g_strdup(cword);
}


/* Read the word up to position @a pos. */
static const gchar *
editor_read_word_stem(GeanyEditor *editor, gint pos, const gchar *wordchars)
{
	static gchar word[GEANY_MAX_WORD_LENGTH];

	read_current_word(editor, pos, word, sizeof word, wordchars, TRUE);

	return (*word) ? word : NULL;
}


static gint find_previous_brace(ScintillaObject *sci, gint pos)
{
	gchar c;
	gint orig_pos = pos;

	c = sci_get_char_at(sci, pos);
	while (pos >= 0 && pos > orig_pos - 300)
	{
		c = sci_get_char_at(sci, pos);
		pos--;
		if (utils_is_opening_brace(c, editor_prefs.brace_match_ltgt))
			return pos;
	}
	return -1;
}


static gint find_start_bracket(ScintillaObject *sci, gint pos)
{
	gchar c;
	gint brackets = 0;
	gint orig_pos = pos;

	c = sci_get_char_at(sci, pos);
	while (pos > 0 && pos > orig_pos - 300)
	{
		c = sci_get_char_at(sci, pos);
		if (c == ')') brackets++;
		else if (c == '(') brackets--;
		pos--;
		if (brackets < 0) return pos;	/* found start bracket */
	}
	return -1;
}


static gboolean append_calltip(GString *str, const TMTag *tag, filetype_id ft_id)
{
	if (! tag->atts.entry.arglist)
		return FALSE;

	if (ft_id != GEANY_FILETYPES_PASCAL)
	{	/* usual calltips: "retval tagname (arglist)" */
		if (tag->atts.entry.var_type)
		{
			guint i;

			g_string_append(str, tag->atts.entry.var_type);
			for (i = 0; i < tag->atts.entry.pointerOrder; i++)
			{
				g_string_append_c(str, '*');
			}
			g_string_append_c(str, ' ');
		}
		if (tag->atts.entry.scope)
		{
			const gchar *cosep = symbols_get_context_separator(ft_id);

			g_string_append(str, tag->atts.entry.scope);
			g_string_append(str, cosep);
		}
		g_string_append(str, tag->name);
		g_string_append_c(str, ' ');
		g_string_append(str, tag->atts.entry.arglist);
	}
	else
	{	/* special case Pascal calltips: "tagname (arglist) : retval" */
		g_string_append(str, tag->name);
		g_string_append_c(str, ' ');
		g_string_append(str, tag->atts.entry.arglist);

		if (NZV(tag->atts.entry.var_type))
		{
			g_string_append(str, " : ");
			g_string_append(str, tag->atts.entry.var_type);
		}
	}

	return TRUE;
}


static gchar *find_calltip(const gchar *word, GeanyFiletype *ft)
{
	const GPtrArray *tags;
	const gint arg_types = tm_tag_function_t | tm_tag_prototype_t |
		tm_tag_method_t | tm_tag_macro_with_arg_t;
	TMTagAttrType *attrs = NULL;
	TMTag *tag;
	GString *str = NULL;
	guint i;

	g_return_val_if_fail(ft && word && *word, NULL);

	/* use all types in case language uses wrong tag type e.g. python "members" instead of "methods" */
	tags = tm_workspace_find(word, tm_tag_max_t, attrs, FALSE, ft->lang);
	if (tags->len == 0)
		return NULL;

	tag = TM_TAG(tags->pdata[0]);

	if (tag->type == tm_tag_class_t && FILETYPE_ID(ft) == GEANY_FILETYPES_D)
	{
		/* user typed e.g. 'new Classname(' so lookup D constructor Classname::this() */
		tags = tm_workspace_find_scoped("this", tag->name,
			arg_types, attrs, FALSE, ft->lang, TRUE);
		if (tags->len == 0)
			return NULL;
	}

	/* remove tags with no argument list */
	for (i = 0; i < tags->len; i++)
	{
		tag = TM_TAG(tags->pdata[i]);

		if (! tag->atts.entry.arglist)
			tags->pdata[i] = NULL;
	}
	tm_tags_prune((GPtrArray *) tags);
	if (tags->len == 0)
		return NULL;
	else
	{	/* remove duplicate calltips */
		TMTagAttrType sort_attr[] = {tm_tag_attr_name_t, tm_tag_attr_scope_t,
			tm_tag_attr_arglist_t, 0};

		tm_tags_sort((GPtrArray *) tags, sort_attr, TRUE);
	}

	/* if the current word has changed since last time, start with the first tag match */
	if (! utils_str_equal(word, calltip.last_word))
		calltip.tag_index = 0;
	/* cache the current word for next time */
	g_free(calltip.last_word);
	calltip.last_word = g_strdup(word);
	calltip.tag_index = MIN(calltip.tag_index, tags->len - 1);	/* ensure tag_index is in range */

	for (i = calltip.tag_index; i < tags->len; i++)
	{
		tag = TM_TAG(tags->pdata[i]);

		if (str == NULL)
		{
			str = g_string_new(NULL);
			if (calltip.tag_index > 0)
				g_string_prepend(str, "\001 ");	/* up arrow */
			append_calltip(str, tag, FILETYPE_ID(ft));
		}
		else /* add a down arrow */
		{
			if (calltip.tag_index > 0)	/* already have an up arrow */
				g_string_insert_c(str, 1, '\002');
			else
				g_string_prepend(str, "\002 ");
			break;
		}
	}
	if (str)
	{
		gchar *result = str->str;

		g_string_free(str, FALSE);
		return result;
	}
	return NULL;
}


/* use pos = -1 to search for the previous unmatched open bracket. */
gboolean editor_show_calltip(GeanyEditor *editor, gint pos)
{
	gint orig_pos = pos; /* the position for the calltip */
	gint lexer;
	gint style;
	gchar word[GEANY_MAX_WORD_LENGTH];
	gchar *str;
	ScintillaObject *sci;

	g_return_val_if_fail(editor != NULL, FALSE);
	g_return_val_if_fail(editor->document->file_type != NULL, FALSE);

	sci = editor->sci;

	lexer = sci_get_lexer(sci);

	if (pos == -1)
	{
		/* position of '(' is unknown, so go backwards from current position to find it */
		pos = sci_get_current_position(sci);
		pos--;
		orig_pos = pos;
		pos = (lexer == SCLEX_LATEX) ? find_previous_brace(sci, pos) :
			find_start_bracket(sci, pos);
		if (pos == -1)
			return FALSE;
	}

	/* the style 1 before the brace (which may be highlighted) */
	style = sci_get_style_at(sci, pos - 1);
	if (! is_code_style(lexer, style))
		return FALSE;

	word[0] = '\0';
	editor_find_current_word(editor, pos - 1, word, sizeof word, NULL);
	if (word[0] == '\0')
		return FALSE;

	str = find_calltip(word, editor->document->file_type);
	if (str)
	{
		g_free(calltip.text);	/* free the old calltip */
		calltip.text = str;
		calltip.pos = orig_pos;
		calltip.sci = sci;
		calltip.set = TRUE;
		utils_wrap_string(calltip.text, -1);
		SSM(sci, SCI_CALLTIPSHOW, orig_pos, (sptr_t) calltip.text);
		return TRUE;
	}
	return FALSE;
}


gchar *editor_get_calltip_text(GeanyEditor *editor, const TMTag *tag)
{
	GString *str;

	g_return_val_if_fail(editor != NULL, NULL);

	str = g_string_new(NULL);
	if (append_calltip(str, tag, FILETYPE_ID(editor->document->file_type)))
		return g_string_free(str, FALSE);
	else
		return g_string_free(str, TRUE);
}


/* HTML entities auto completion from html_entities.tags text file */
static gboolean
autocomplete_html(ScintillaObject *sci, const gchar *root, gsize rootlen)
{
	guint i, j = 0;
	gboolean found = FALSE;
	GString *words;
	const gchar **entities = symbols_get_html_entities();

	if (*root != '&' || G_UNLIKELY(entities == NULL))
		return FALSE;

	words = g_string_sized_new(500);
	for (i = 0; ; i++)
	{
		if (entities[i] == NULL)
			break;
		else if (entities[i][0] == '#')
			continue;

		if (! strncmp(entities[i], root, rootlen))
		{
			if (j++ > 0)
				g_string_append_c(words, '\n');
			g_string_append(words, entities[i]);
			found = TRUE;
		}
	}
	if (found)
		show_autocomplete(sci, rootlen, words->str);

	g_string_free(words, TRUE);
	return found;
}


/* Current document & global tags autocompletion */
static gboolean
autocomplete_tags(GeanyEditor *editor, const gchar *root, gsize rootlen)
{
	TMTagAttrType attrs[] = { tm_tag_attr_name_t, 0 };
	const GPtrArray *tags;
	GeanyDocument *doc;

	g_return_val_if_fail(editor, FALSE);

	doc = editor->document;

	tags = tm_workspace_find(root, tm_tag_max_t, attrs, TRUE, doc->file_type->lang);
	if (tags)
	{
		show_tags_list(editor, tags, rootlen);
		return tags->len > 0;
	}
	return FALSE;
}


/* Check whether to use entity autocompletion:
 * - always in a HTML file except when inside embedded JavaScript, Python, ASP, ...
 * - in a PHP file only when we are outside of <? ?> */
static gboolean autocomplete_check_for_html(gint ft_id, gint style)
{
	/* use entity completion when style is not JavaScript, ASP, Python, PHP, ...
	 * (everything after SCE_HJ_START is for embedded scripting languages) */
	if (ft_id == GEANY_FILETYPES_HTML && style < SCE_HJ_START)
		return TRUE;

	if (ft_id == GEANY_FILETYPES_PHP)
	{
		/* use entity completion when style is outside of PHP styles */
		if (! is_style_php(style))
			return TRUE;
	}

	return FALSE;
}


/* Algorithm based on based on Scite's StartAutoCompleteWord() */
static GString *get_doc_words(ScintillaObject *sci, gchar *root, gsize rootlen)
{
	gchar *word;
	gint len, current, word_end;
	gint pos_find, flags;
	guint word_length;
	gsize nmatches = 0;
	GString *words;
	struct Sci_TextToFind ttf;

	len = sci_get_length(sci);
	current = sci_get_current_position(sci) - rootlen;

	ttf.lpstrText = root;
	ttf.chrg.cpMin = 0;
	ttf.chrg.cpMax = len;
	ttf.chrgText.cpMin = 0;
	ttf.chrgText.cpMax = 0;
	flags = SCFIND_WORDSTART | SCFIND_MATCHCASE;

	words = g_string_sized_new(256);
	/* put space before first entry to make searching with strstr easy */
	g_string_append_c(words, ' ');

	/* search the whole document for the word root and collect results */
	pos_find = scintilla_send_message(sci, SCI_FINDTEXT, flags, (uptr_t) &ttf);
	while (pos_find >= 0 && pos_find < len)
	{
		word_end = pos_find + rootlen;
		if (pos_find != current)
		{
			while (word_end < len && strchr(GEANY_WORDCHARS, sci_get_char_at(sci, word_end)) != NULL)
				word_end++;

			word_length = word_end - pos_find;
			if (word_length > rootlen)
			{
				word = g_malloc0(word_length + 3);
				sci_get_text_range(sci, pos_find, word_end, word + 1);
				word[0] = ' ';
				word[word_length + 1] = ' ';
				/* search the words string whether we already have the word in, otherwise add it */
				if (strstr(words->str, word) == NULL)
				{
					g_string_append(words, word + 1);
					nmatches++;
				}
				g_free(word);

				if (nmatches == editor_prefs.autocompletion_max_entries)
					break;
			}
		}
		ttf.chrg.cpMin = word_end;
		pos_find = scintilla_send_message(sci, SCI_FINDTEXT, flags, (uptr_t) &ttf);
	}

	if (words->len > 1)
	{
		g_strdelimit(words->str, " ", '\n');
		words->str[words->len - 1] = '\0'; /* remove the trailing '\n' */
		return words;
	}
	g_string_free(words, TRUE);
	return NULL;
}


static gboolean autocomplete_doc_word(GeanyEditor *editor, gchar *root, gsize rootlen)
{
	ScintillaObject *sci = editor->sci;
	GString *words;
	GString *str;
	gchar *ptr;
	GSList *node, *list = NULL;

	words = get_doc_words(sci, root, rootlen);
	if (!words)
	{
		scintilla_send_message(sci, SCI_AUTOCCANCEL, 0, 0);
		return FALSE;
	}

	/* words are unsorted, make list of words */
	foreach_str(ptr, words->str)
	{
		if (*ptr == '\n')
		{
			list = g_slist_prepend(list, ptr + 1);
			/* terminate previous string in list */
			ptr[0] = 0x0;
			ptr++;
		}
	}
	list = g_slist_sort(list, (GCompareFunc)utils_str_casecmp);

	str = g_string_sized_new(words->len);
	foreach_slist(node, list)
	{
		g_string_append(str, node->data);
		if (node->next)
			g_string_append_c(str, '\n');
	}
	if (g_slist_length(list) >= editor_prefs.autocompletion_max_entries)
		g_string_append(str, "\n...");

	g_slist_free(list);
	g_string_free(words, TRUE);

	show_autocomplete(sci, rootlen, str->str);
	g_string_free(str, TRUE);
	return TRUE;
}


gboolean editor_start_auto_complete(GeanyEditor *editor, gint pos, gboolean force)
{
	gint line, line_start, line_len, line_pos, current, rootlen, startword, lexer, style;
	gchar *linebuf, *root;
	ScintillaObject *sci;
	gboolean ret = FALSE;
	gchar *wordchars;
	GeanyFiletype *ft;

	if (! editor_prefs.auto_complete_symbols && ! force)
		return FALSE;

	g_return_val_if_fail(editor != NULL, FALSE);

	/* If we are at the beginning of the document, we skip autocompletion as we can't determine the
	 * necessary styling information */
	if (G_UNLIKELY(pos < 2))
		return FALSE;

	sci = editor->sci;
	ft = editor->document->file_type;

	line = sci_get_line_from_position(sci, pos);
	line_start = sci_get_position_from_line(sci, line);
	line_len = sci_get_line_length(sci, line);
	line_pos = pos - line_start - 1;
	current = pos - line_start;
	startword = current;
	lexer = sci_get_lexer(sci);
	style = sci_get_style_at(sci, pos - 2);

	/* don't autocomplete in comments and strings */
	if (!force && !is_code_style(lexer, style))
		return FALSE;

	autocomplete_scope(editor);

	linebuf = sci_get_line(sci, line);

	if (ft->id == GEANY_FILETYPES_LATEX)
		wordchars = GEANY_WORDCHARS"\\"; /* add \ to word chars if we are in a LaTeX file */
	else if (ft->id == GEANY_FILETYPES_HTML || ft->id == GEANY_FILETYPES_PHP)
		wordchars = GEANY_WORDCHARS"&"; /* add & to word chars if we are in a PHP or HTML file */
	else
		wordchars = GEANY_WORDCHARS;

	/* find the start of the current word */
	while ((startword > 0) && (strchr(wordchars, linebuf[startword - 1])))
		startword--;
	linebuf[current] = '\0';
	root = linebuf + startword;
	rootlen = current - startword;

	if (rootlen > 0)
	{
		if (autocomplete_check_for_html(ft->id, style))
		{
			/* Allow something like "&quot;some text&quot;". The above startword calculation
			 * only works on words but for HTML entity completion we also want to have completion
			 * based on '&' within words. */
			gchar *tmp = strchr(root, '&');
			if (tmp != NULL)
			{
				root = tmp;
				rootlen = strlen(tmp);
			}
			ret = autocomplete_html(sci, root, rootlen);
		}
		else
		{
			/* force is set when called by keyboard shortcut, otherwise start at the
			 * editor_prefs.symbolcompletion_min_chars'th char */
			if (force || rootlen >= editor_prefs.symbolcompletion_min_chars)
			{
				/* complete tags, except if forcing when completion is already visible */
				if (!(force && SSM(sci, SCI_AUTOCACTIVE, 0, 0)))
					ret = autocomplete_tags(editor, root, rootlen);

				/* If forcing and there's nothing else to show, complete from words in document */
				if (!ret && (force || editor_prefs.autocomplete_doc_words))
					ret = autocomplete_doc_word(editor, root, rootlen);
			}
		}
	}
	if (!ret && force)
		utils_beep();

	g_free(linebuf);
	return ret;
}


static const gchar *snippets_find_completion_by_name(const gchar *type, const gchar *name)
{
	gchar *result = NULL;
	GHashTable *tmp;

	g_return_val_if_fail(type != NULL && name != NULL, NULL);

	tmp = g_hash_table_lookup(snippet_hash, type);
	if (tmp != NULL)
	{
		result = g_hash_table_lookup(tmp, name);
	}
	/* whether nothing is set for the current filetype(tmp is NULL) or
	 * the particular completion for this filetype is not set (result is NULL) */
	if (tmp == NULL || result == NULL)
	{
		tmp = g_hash_table_lookup(snippet_hash, "Default");
		if (tmp != NULL)
		{
			result = g_hash_table_lookup(tmp, name);
		}
	}
	/* if result is still NULL here, no completion could be found */

	/* result is owned by the hash table and will be freed when the table will destroyed */
	return result;
}


/* This is very ugly but passing the pattern to ac_replace_specials() doesn't work because it is
 * modified when replacing a completion but the foreach function still passes the old pointer
 * to ac_replace_specials, so we use a global pointer outside of ac_replace_specials and
 * ac_complete_constructs. Any hints to improve this are welcome. */
static GString *snippets_global_pattern = NULL;

static void snippets_replace_specials(gpointer key, gpointer value, gpointer user_data)
{
	gchar *needle;

	g_return_if_fail(key != NULL);
	g_return_if_fail(value != NULL);

	needle = g_strconcat("%", (gchar*) key, "%", NULL);

	utils_string_replace_all(snippets_global_pattern, needle, (gchar*) value);
	g_free(needle);
}


static void snippets_replace_wildcards(GeanyEditor *editor, GString *text)
{
	const gchar *file_name = DOC_FILENAME(editor->document);
	gchar *basename = g_path_get_basename(file_name);

	templates_replace_default_dates(text);
	templates_replace_valist(text, "{filename}", basename, NULL);
	templates_replace_command(text, file_name, editor->document->file_type->name, NULL);

	g_free(basename);
}


/* this only works with spaces only indentation on the lines */
static void fix_line_indents(GeanyEditor *editor, gint line_start, gint line_end)
{
	ScintillaObject *sci = editor->sci;
	gint line, cur_line, cur_col, pos;

	/* get the line, col position as fixing indentation will move cursor to start of line */
	pos = sci_get_current_position(sci);
	cur_col = sci_get_col_from_position(sci, pos);
	cur_line = sci_get_current_line(sci);

	for (line = line_start; line <= line_end; line++)
	{
		gint size = sci_get_line_indentation(sci, line);

		/* set to 0 first to trigger proper indent creation */
		sci_set_line_indentation(sci, line, 0);
		sci_set_line_indentation(sci, line, size);
	}
	pos = scintilla_send_message(sci, SCI_FINDCOLUMN, cur_line, cur_col);
	sci_set_current_position(sci, pos, FALSE);
}


/** Inserts text, replacing \\t tab chars with the correct indent width, and \\n newline
 * chars with the correct line ending string.
 * @param editor The editor to operate on.
 * @param text Intended as e.g. "if (1)\n\tdo_something();"
 * @param insert_pos Position, where to start with inserting text block.
 * @param cursor_index If >= 0, the index into @a text to place the cursor.
 * @param newline_indent_size Indentation size (in spaces) to insert for each newline; use
 * -1 to read the indent size from the line with @a insert_pos on it.
 * @param replace_newlines Whether to replace newlines in text or not. If
 * newlines have been replaced before, this should be false, to avoid multiple
 * replacements of newlines, which is error prone on Windows.
 * @warning Make sure all \\t tab chars in @a text are intended as indent widths,
 * NOT any hard tabs (you get those when copying document text with the Tabs
 * & Spaces indent mode set).
 * @note This doesn't scroll the cursor in view afterwards. **/
void editor_insert_text_block(GeanyEditor *editor, const gchar *text, gint insert_pos,
		gint cursor_index, gint newline_indent_size, gboolean replace_newlines)
{
	ScintillaObject *sci = editor->sci;
	gint line_start = sci_get_line_from_position(sci, insert_pos);
	gint line_end;
	gchar *whitespace;
	GString *buf;
	const gchar cur_marker[] = "__GEANY_CURSOR_MARKER__";

	g_return_if_fail(text);
	g_return_if_fail(editor != NULL);

	buf = g_string_new(text);

	if (cursor_index >= 0)
		g_string_insert(buf, cursor_index, cur_marker);	/* remember cursor pos */

	/* Add line indents (in spaces) */

	if (newline_indent_size == -1)
		newline_indent_size = sci_get_line_indentation(sci, line_start);

	if (newline_indent_size > 0)
	{
		whitespace = g_strnfill(newline_indent_size, ' ');
		setptr(whitespace, g_strconcat("\n", whitespace, NULL));
		utils_string_replace_all(buf, "\n", whitespace);
		g_free(whitespace);
	}

	/* transform line endings */
	if (replace_newlines)
		utils_string_replace_all(buf, "\n", editor_get_eol_char(editor));

	/* transform tabs into indent widths (in spaces) */
	whitespace = g_strnfill(editor_get_indent_prefs(editor)->width, ' ');
	utils_string_replace_all(buf, "\t", whitespace);
	g_free(whitespace);

	if (cursor_index >= 0)
	{
		gint idx = utils_strpos(buf->str, cur_marker);

		g_string_erase(buf, idx, strlen(cur_marker));

		sci_insert_text(sci, insert_pos, buf->str);
		sci_set_current_position(sci, insert_pos + idx, FALSE);
	}
	else
		sci_insert_text(sci, insert_pos, buf->str);

	/* fixup indentation (very useful for Tabs & Spaces indent type) */
	line_end = sci_get_line_from_position(sci, insert_pos + buf->len);
	fix_line_indents(editor, line_start, line_end);
	snippet_cursor_insert_pos = sci_get_current_position(sci);

	g_string_free(buf, TRUE);
}


/* Move the cursor to the next specified cursor position in an inserted snippet.
 * Can, and should, be optimized to give better results */
void editor_goto_next_snippet_cursor(GeanyEditor *editor)
{
	ScintillaObject *sci = editor->sci;
	gint current_pos = sci_get_current_position(sci);

	if (snippet_offsets && !g_queue_is_empty(snippet_offsets))
	{
		gint offset;

		offset = GPOINTER_TO_INT(g_queue_pop_head(snippet_offsets));
		if (current_pos > snippet_cursor_insert_pos)
			snippet_cursor_insert_pos = offset + current_pos;
		else
			snippet_cursor_insert_pos += offset;

		sci_set_current_position(sci, snippet_cursor_insert_pos, FALSE);
	}
	else
	{
		utils_beep();
	}
}


static gssize snippets_make_replacements(GeanyEditor *editor, GString *pattern,
		gsize indent_size)
{
	gssize cur_index = -1;
	gchar *whitespace;
	gint i, tmp_pos, whitespace_len, nl_count = 0;
	GHashTable *specials;
	GList *temp_list = NULL;
	const GeanyIndentPrefs *iprefs = editor_get_indent_prefs(editor);
	gint cursor_steps, old_cursor = 0;

	/* replace 'special' completions */
	specials = g_hash_table_lookup(snippet_hash, "Special");
	if (G_LIKELY(specials != NULL))
	{
		/* ugly hack using global_pattern */
		snippets_global_pattern = pattern;
		g_hash_table_foreach(specials, snippets_replace_specials, NULL);
	}

	/* replace any %template% wildcards */
	snippets_replace_wildcards(editor, pattern);

	/* transform other wildcards */
	/* convert to %newlines%, else we get endless loops */
	utils_string_replace_all(pattern, "\n", "%newline%");

	/* if spaces are used, replaces all tabs with %ws%, which is later replaced
	 * by real whitespace characters
	 * otherwise replace all %ws% by \t, which will be replaced later by tab
	 * characters,
	 * this makes seperating between tab and spaces intentation pretty easy */
	if (iprefs->type == GEANY_INDENT_TYPE_SPACES)
		utils_string_replace_all(pattern, "\t", "%ws%");
	else
		utils_string_replace_all(pattern, "%ws%", "\t");

	whitespace = g_strnfill(iprefs->width, ' '); /* use spaces for indentation, will be fixed up */
	whitespace_len = strlen(whitespace);
	i = 0;
	while ((cursor_steps = utils_strpos(pattern->str, "%cursor%")) >= 0)
	{
		/* replace every %newline% (up to next %cursor%) with EOL,
		 * and update cursor_steps after */
		while ((tmp_pos = utils_strpos(pattern->str, "%newline%")) < cursor_steps && tmp_pos != -1)
		{
			nl_count++;
			utils_string_replace_first(pattern, "%newline%", editor_get_eol_char(editor));
			cursor_steps = utils_strpos(pattern->str, "%cursor%");
		}
		/* replace every %ws% (up to next %cursor%) with whitespaces,
		 * and update cursor_steps after */
		while ((tmp_pos = utils_strpos(pattern->str, "%ws%")) < cursor_steps && tmp_pos != -1)
		{
			utils_string_replace_first(pattern, "%ws%", whitespace);
			cursor_steps = utils_strpos(pattern->str, "%cursor%");
		}
		/* finally replace the next %cursor% */
		utils_string_replace_first(pattern, "%cursor%", "");

		/* modify cursor_steps to take indentation count and type into account */

		/* We're saving the relative offset to each cursor position in a simple
		 * linked list, including indentations between them. */
		if (i++ > 0)
		{
			cursor_steps += (nl_count * indent_size);
			temp_list = g_list_append(temp_list, GINT_TO_POINTER(cursor_steps - old_cursor));
		}
		else
		{
			nl_count = 0;
			cur_index = cursor_steps;
		}
		old_cursor = cursor_steps;
	}
	/* replace remaining %ws% and %newline% which may occur after the last %cursor% */
	utils_string_replace_all(pattern, "%newline%", editor_get_eol_char(editor));
	utils_string_replace_all(pattern, "%ws%", whitespace);
	g_free(whitespace);

	/* We put the cursor positions for the most recent
	 * parsed snippet first, followed by any remaining positions */
	i = 0;
	if (temp_list)
	{
		GList *node;

		foreach_list(node, temp_list)
			g_queue_push_nth(snippet_offsets, node->data, i++);

		/* limit length of queue */
		while (g_queue_get_length(snippet_offsets) > 20)
			g_queue_pop_tail(snippet_offsets);

		g_list_free(temp_list);
	}
	if (cur_index < 0)
		cur_index = pattern->len;

	return cur_index;
}


static gboolean snippets_complete_constructs(GeanyEditor *editor, gint pos, const gchar *word)
{
	ScintillaObject *sci = editor->sci;
	gchar *str;
	GString *pattern;
	gssize cur_index = -1;
	gint str_len;
	gint ft_id = FILETYPE_ID(editor->document->file_type);

	str = g_strdup(word);
	g_strstrip(str);
	pattern = g_string_new(snippets_find_completion_by_name(filetypes[ft_id]->name, str));
	if (pattern == NULL || pattern->len == 0)
	{
		g_free(str);
		g_string_free(pattern, TRUE);
		return FALSE;
	}

	read_indent(editor, pos);

	/* remove the typed word, it will be added again by the used auto completion
	 * (not really necessary but this makes the auto completion more flexible,
	 *  e.g. with a completion like hi=hello, so typing "hi<TAB>" will result in "hello") */
	str_len = strlen(str);
	sci_set_selection_start(sci, pos - str_len);
	sci_set_selection_end(sci, pos);
	sci_replace_sel(sci, "");
	pos -= str_len; /* pos has changed while deleting */

	cur_index = snippets_make_replacements(editor, pattern, strlen(indent));

	/* finally insert the text and set the cursor */
	editor_insert_text_block(editor, pattern->str, pos, cur_index, -1, FALSE);
	sci_scroll_caret(sci);

	g_free(str);
	g_string_free(pattern, TRUE);
 	return TRUE;
}


static gboolean at_eol(ScintillaObject *sci, gint pos)
{
	gint line = sci_get_line_from_position(sci, pos);
	gchar c;

	/* skip any trailing spaces */
	while (TRUE)
	{
		c = sci_get_char_at(sci, pos);
		if (c == ' ' || c == '\t')
			pos++;
		else
			break;
	}

	return (pos == sci_get_line_end_position(sci, line));
}


gboolean editor_complete_snippet(GeanyEditor *editor, gint pos)
{
	gboolean result = FALSE;
	gint lexer, style;
	const gchar *wc;
	const gchar *word;
	ScintillaObject *sci;

	g_return_val_if_fail(editor != NULL, FALSE);

	sci = editor->sci;
	if (sci_has_selection(sci))
		return FALSE;
	/* return if we are editing an existing line (chars on right of cursor) */
	if (keybindings_lookup_item(GEANY_KEY_GROUP_EDITOR,
			GEANY_KEYS_EDITOR_COMPLETESNIPPET)->key == GDK_space &&
		! editor_prefs.complete_snippets_whilst_editing && ! at_eol(sci, pos))
		return FALSE;

	lexer = sci_get_lexer(sci);
	style = sci_get_style_at(sci, pos - 2);

	wc = snippets_find_completion_by_name("Special", "wordchars");
	word = editor_read_word_stem(editor, pos, wc);

	/* prevent completion of "for " */
	if (NZV(word) &&
		! isspace(sci_get_char_at(sci, pos - 1))) /* pos points to the line end char so use pos -1 */
	{
		sci_start_undo_action(sci);	/* needed because we insert a space separately from construct */
		result = snippets_complete_constructs(editor, pos, word);
		sci_end_undo_action(sci);
		if (result)
			sci_cancel(sci);	/* cancel any autocompletion list, etc */
	}
	return result;
}


void editor_show_macro_list(GeanyEditor *editor)
{
	GString *words;

	if (editor == NULL || editor->document->file_type == NULL)
		return;

	words = symbols_get_macro_list(editor->document->file_type->lang);
	if (words == NULL)
		return;

	SSM(editor->sci, SCI_USERLISTSHOW, 1, (sptr_t) words->str);
	g_string_free(words, TRUE);
}


static void insert_closing_tag(GeanyEditor *editor, gint pos, gchar ch, const gchar *tag_name)
{
	ScintillaObject *sci = editor->sci;
	gchar *to_insert = NULL;

	if (ch == '/')
	{
		const gchar *gt = ">";
		/* if there is already a '>' behind the cursor, don't add it */
		if (sci_get_char_at(sci, pos) == '>')
			gt = "";

		to_insert = g_strconcat(tag_name, gt, NULL);
	}
	else
		to_insert = g_strconcat("</", tag_name, ">", NULL);

	sci_start_undo_action(sci);
	sci_replace_sel(sci, to_insert);
	if (ch == '>')
	{
		sci_set_selection(sci, pos, pos);
		if (utils_str_equal(tag_name, "table"))
			auto_table(editor, pos);
	}
	sci_end_undo_action(sci);
	g_free(to_insert);
}


/*
 * (stolen from anjuta and heavily modified)
 * This routine will auto complete XML or HTML tags that are still open by closing them
 * @param ch The character we are dealing with, currently only works with the '>' character
 * @return True if handled, false otherwise
 */
static gboolean handle_xml(GeanyEditor *editor, gint pos, gchar ch)
{
	ScintillaObject *sci = editor->sci;
	gint lexer = sci_get_lexer(sci);
	gint min, style;
	gchar *str_found, sel[512];
	gboolean result = FALSE;

	/* If the user has turned us off, quit now.
	 * This may make sense only in certain languages */
	if (! editor_prefs.auto_close_xml_tags || (lexer != SCLEX_HTML && lexer != SCLEX_XML))
		return FALSE;

	/* return if we are inside any embedded script */
	style = sci_get_style_at(sci, pos);
	if (style > SCE_H_XCCOMMENT && ! is_string_style(lexer, style))
		return FALSE;

	/* if ch is /, check for </, else quit */
	if (ch == '/' && sci_get_char_at(sci, pos - 2) != '<')
		return FALSE;

	/* Grab the last 512 characters or so */
	min = pos - (sizeof(sel) - 1);
	if (min < 0) min = 0;

	if (pos - min < 3)
		return FALSE; /* Smallest tag is 3 characters e.g. <p> */

	sci_get_text_range(sci, min, pos, sel);
	sel[sizeof(sel) - 1] = '\0';

	if (ch == '>' && sel[pos - min - 2] == '/')
		/* User typed something like "<br/>" */
		return FALSE;

	str_found = utils_find_open_xml_tag(sel, pos - min, (ch == '/'));

	/* when found string is something like br, img or another short tag, quit */
	if (utils_str_equal(str_found, "br")
	 || utils_str_equal(str_found, "hr")
	 || utils_str_equal(str_found, "img")
	 || utils_str_equal(str_found, "base")
	 || utils_str_equal(str_found, "basefont")	/* < or not < */
	 || utils_str_equal(str_found, "frame")
	 || utils_str_equal(str_found, "input")
	 || utils_str_equal(str_found, "link")
	 || utils_str_equal(str_found, "area")
	 || utils_str_equal(str_found, "meta"))
	{
		/* ignore tag */
	}
	else if (NZV(str_found))
	{
		insert_closing_tag(editor, pos, ch, str_found);
		result = TRUE;
	}
	g_free(str_found);
	return result;
}


/* like sci_get_line_indentation(), but for a string. */
static gsize count_indent_size(GeanyEditor *editor, const gchar *base_indent)
{
	const gchar *ptr;
	gsize tab_size = sci_get_tab_width(editor->sci);
	gsize count = 0;

	g_return_val_if_fail(base_indent, 0);

	for (ptr = base_indent; *ptr != 0; ptr++)
	{
		switch (*ptr)
		{
			case ' ':
				count++;
				break;
			case '\t':
				count += tab_size;
				break;
		}
	}
	return count;
}


static void auto_table(GeanyEditor *editor, gint pos)
{
	ScintillaObject *sci = editor->sci;
	gchar *table;
	gint indent_pos;
	const gchar *indent_str;

	if (sci_get_lexer(sci) != SCLEX_HTML) return;

	read_indent(editor, pos);
	indent_pos = sci_get_line_indent_position(sci, sci_get_line_from_position(sci, pos));
	if ((pos - 7) != indent_pos) /* 7 == strlen("<table>") */
	{
		gint i;
		guint x;

		x = strlen(indent);
		/* find the start of the <table tag */
		i = 1;
		while (i <= pos && sci_get_char_at(sci, pos - i) != '<') i++;
		/* add all non whitespace before the tag to the indent string */
		while ((pos - i) != indent_pos && x < sizeof(indent) - 1)
		{
			indent[x++] = ' ';
			i++;
		}
		indent[x] = '\0';
	}

	if (! editor->auto_indent)
		indent_str = "";
	else
		indent_str = "\t";

	table = g_strconcat("\n", indent_str, "<tr>\n",
						indent_str, indent_str, "<td> </td>\n",
						indent_str, "</tr>\n",
						NULL);
	editor_insert_text_block(editor, table, pos, -1,
		count_indent_size(editor, indent), TRUE);
	g_free(table);
}


static void real_comment_multiline(GeanyEditor *editor, gint line_start, gint last_line)
{
	const gchar *eol;
	gchar *str_begin, *str_end, *co, *cc;
	gint line_len;

	g_return_if_fail(editor != NULL && editor->document->file_type != NULL);

	eol = editor_get_eol_char(editor);
	co = editor->document->file_type->comment_open;
	cc = editor->document->file_type->comment_close;
	str_begin = g_strdup_printf("%s%s", (co != NULL) ? co : "", eol);
	str_end = g_strdup_printf("%s%s", (cc != NULL) ? cc : "", eol);

	/* insert the comment strings */
	sci_insert_text(editor->sci, line_start, str_begin);
	line_len = sci_get_position_from_line(editor->sci, last_line + 2);
	sci_insert_text(editor->sci, line_len, str_end);

	g_free(str_begin);
	g_free(str_end);
}


static void real_uncomment_multiline(GeanyEditor *editor)
{
	/* find the beginning of the multi line comment */
	gint pos, line, len, x;
	gchar *linebuf;
	GeanyDocument *doc;

	g_return_if_fail(editor != NULL && editor->document->file_type != NULL);
	doc = editor->document;

	/* remove comment open chars */
	pos = document_find_text(doc, doc->file_type->comment_open, 0, TRUE, FALSE, NULL);
	SSM(editor->sci, SCI_DELETEBACK, 0, 0);

	/* check whether the line is empty and can be deleted */
	line = sci_get_line_from_position(editor->sci, pos);
	len = sci_get_line_length(editor->sci, line);
	linebuf = sci_get_line(editor->sci, line);
	x = 0;
	while (linebuf[x] != '\0' && isspace(linebuf[x])) x++;
	if (x == len) SSM(editor->sci, SCI_LINEDELETE, 0, 0);
	g_free(linebuf);

	/* remove comment close chars */
	pos = document_find_text(doc, doc->file_type->comment_close, 0, FALSE, FALSE, NULL);
	SSM(editor->sci, SCI_DELETEBACK, 0, 0);

	/* check whether the line is empty and can be deleted */
	line = sci_get_line_from_position(editor->sci, pos);
	len = sci_get_line_length(editor->sci, line);
	linebuf = sci_get_line(editor->sci, line);
	x = 0;
	while (linebuf[x] != '\0' && isspace(linebuf[x])) x++;
	if (x == len) SSM(editor->sci, SCI_LINEDELETE, 0, 0);
	g_free(linebuf);
}


static gint get_multiline_comment_style(GeanyEditor *editor, gint line_start)
{
	gint lexer = sci_get_lexer(editor->sci);
	gint style_comment;

	/* List only those lexers which support multi line comments */
	switch (lexer)
	{
		case SCLEX_XML:
		case SCLEX_HTML:
		{
			if (is_style_php(sci_get_style_at(editor->sci, line_start)))
				style_comment = SCE_HPHP_COMMENT;
			else
				style_comment = SCE_H_COMMENT;
			break;
		}
		case SCLEX_HASKELL: style_comment = SCE_HA_COMMENTBLOCK; break;
		case SCLEX_LUA: style_comment = SCE_LUA_COMMENT; break;
		case SCLEX_CSS: style_comment = SCE_CSS_COMMENT; break;
		case SCLEX_SQL: style_comment = SCE_SQL_COMMENT; break;
		case SCLEX_CAML: style_comment = SCE_CAML_COMMENT; break;
		case SCLEX_D: style_comment = SCE_D_COMMENT; break;
		case SCLEX_PASCAL: style_comment = SCE_PAS_COMMENT; break;
		default: style_comment = SCE_C_COMMENT;
	}

	return style_comment;
}


/* set toggle to TRUE if the caller is the toggle function, FALSE otherwise
 * returns the amount of uncommented single comment lines, in case of multi line uncomment
 * it returns just 1 */
gint editor_do_uncomment(GeanyEditor *editor, gint line, gboolean toggle)
{
	gint first_line, last_line;
	gint x, i, line_start, line_len;
	gint sel_start, sel_end;
	gint count = 0;
	gsize co_len;
	gchar sel[256], *co, *cc;
	gboolean break_loop = FALSE, single_line = FALSE;
	GeanyFiletype *ft;

	g_return_val_if_fail(editor != NULL && editor->document->file_type != NULL, 0);

	if (line < 0)
	{	/* use selection or current line */
		sel_start = sci_get_selection_start(editor->sci);
		sel_end = sci_get_selection_end(editor->sci);

		first_line = sci_get_line_from_position(editor->sci, sel_start);
		/* Find the last line with chars selected (not EOL char) */
		last_line = sci_get_line_from_position(editor->sci,
			sel_end - editor_get_eol_char_len(editor));
		last_line = MAX(first_line, last_line);
	}
	else
	{
		first_line = last_line = line;
		sel_start = sel_end = sci_get_position_from_line(editor->sci, line);
	}

	ft = editor->document->file_type;

	/* detection of HTML vs PHP code, if non-PHP set filetype to XML */
	line_start = sci_get_position_from_line(editor->sci, first_line);
	if (ft->id == GEANY_FILETYPES_PHP)
	{
		if (! is_style_php(sci_get_style_at(editor->sci, line_start)))
			ft = filetypes[GEANY_FILETYPES_XML];
	}

	co = ft->comment_open;
	cc = ft->comment_close;
	if (co == NULL)
		return 0;

	co_len = strlen(co);
	if (co_len == 0)
		return 0;

	sci_start_undo_action(editor->sci);

	for (i = first_line; (i <= last_line) && (! break_loop); i++)
	{
		gint buf_len;

		line_start = sci_get_position_from_line(editor->sci, i);
		line_len = sci_get_line_length(editor->sci, i);
		x = 0;

		buf_len = MIN((gint)sizeof(sel) - 1, line_len - 1);
		if (buf_len <= 0)
			continue;
		sci_get_text_range(editor->sci, line_start, line_start + buf_len, sel);
		sel[buf_len] = '\0';

		while (isspace(sel[x])) x++;

		/* to skip blank lines */
		if (x < line_len && sel[x] != '\0')
		{
			/* use single line comment */
			if (cc == NULL || strlen(cc) == 0)
			{
				single_line = TRUE;

				if (toggle)
				{
					gsize tm_len = strlen(editor_prefs.comment_toggle_mark);
					if (strncmp(sel + x, co, co_len) != 0 ||
						strncmp(sel + x + co_len, editor_prefs.comment_toggle_mark, tm_len) != 0)
						continue;

					co_len += tm_len;
				}
				else
				{
					if (strncmp(sel + x, co, co_len) != 0)
						continue;
				}

				sci_set_selection(editor->sci, line_start + x, line_start + x + co_len);
				sci_replace_sel(editor->sci, "");
				count++;
			}
			/* use multi line comment */
			else
			{
				gint style_comment;

				/* skip lines which are already comments */
				style_comment = get_multiline_comment_style(editor, line_start);
				if (sci_get_style_at(editor->sci, line_start + x) == style_comment)
				{
					real_uncomment_multiline(editor);
					count = 1;
				}

				/* break because we are already on the last line */
				break_loop = TRUE;
				break;
			}
		}
	}
	sci_end_undo_action(editor->sci);

	/* restore selection if there is one
	 * but don't touch the selection if caller is editor_do_comment_toggle */
	if (! toggle && sel_start < sel_end)
	{
		if (single_line)
		{
			sci_set_selection_start(editor->sci, sel_start - co_len);
			sci_set_selection_end(editor->sci, sel_end - (count * co_len));
		}
		else
		{
			gint eol_len = editor_get_eol_char_len(editor);
			sci_set_selection_start(editor->sci, sel_start - co_len - eol_len);
			sci_set_selection_end(editor->sci, sel_end - co_len - eol_len);
		}
	}

	return count;
}


void editor_do_comment_toggle(GeanyEditor *editor)
{
	gint first_line, last_line;
	gint x, i, line_start, line_len, first_line_start;
	gint sel_start, sel_end;
	gint count_commented = 0, count_uncommented = 0;
	gchar sel[256], *co, *cc;
	gboolean break_loop = FALSE, single_line = FALSE;
	gboolean first_line_was_comment = FALSE;
	gsize co_len;
	gsize tm_len = strlen(editor_prefs.comment_toggle_mark);
	GeanyFiletype *ft;

	g_return_if_fail(editor != NULL && editor->document->file_type != NULL);

	sel_start = sci_get_selection_start(editor->sci);
	sel_end = sci_get_selection_end(editor->sci);

	ft = editor->document->file_type;

	first_line = sci_get_line_from_position(editor->sci,
		sci_get_selection_start(editor->sci));
	/* Find the last line with chars selected (not EOL char) */
	last_line = sci_get_line_from_position(editor->sci,
		sci_get_selection_end(editor->sci) - editor_get_eol_char_len(editor));
	last_line = MAX(first_line, last_line);

	/* detection of HTML vs PHP code, if non-PHP set filetype to XML */
	first_line_start = sci_get_position_from_line(editor->sci, first_line);
	if (ft->id == GEANY_FILETYPES_PHP)
	{
		if (! is_style_php(sci_get_style_at(editor->sci, first_line_start)))
			ft = filetypes[GEANY_FILETYPES_XML];
	}

	co = ft->comment_open;
	cc = ft->comment_close;
	if (co == NULL)
		return;

	co_len = strlen(co);
	if (co_len == 0)
		return;

	sci_start_undo_action(editor->sci);

	for (i = first_line; (i <= last_line) && (! break_loop); i++)
	{
		gint buf_len;

		line_start = sci_get_position_from_line(editor->sci, i);
		line_len = sci_get_line_length(editor->sci, i);
		x = 0;

		buf_len = MIN((gint)sizeof(sel) - 1, line_len - 1);
		if (buf_len < 0)
			continue;
		sci_get_text_range(editor->sci, line_start, line_start + buf_len, sel);
		sel[buf_len] = '\0';

		while (isspace(sel[x])) x++;

		/* use single line comment */
		if (cc == NULL || strlen(cc) == 0)
		{
			gboolean do_continue = FALSE;
			single_line = TRUE;

			if (strncmp(sel + x, co, co_len) == 0 &&
				strncmp(sel + x + co_len, editor_prefs.comment_toggle_mark, tm_len) == 0)
			{
				do_continue = TRUE;
			}

			if (do_continue && i == first_line)
				first_line_was_comment = TRUE;

			if (do_continue)
			{
				count_uncommented += editor_do_uncomment(editor, i, TRUE);
				continue;
			}

			/* we are still here, so the above lines were not already comments, so comment it */
			editor_do_comment(editor, i, TRUE, TRUE);
			count_commented++;
		}
		/* use multi line comment */
		else
		{
			gint style_comment;

			/* skip lines which are already comments */
			style_comment = get_multiline_comment_style(editor, line_start);
			if (sci_get_style_at(editor->sci, line_start + x) == style_comment)
			{
				real_uncomment_multiline(editor);
				count_uncommented++;
			}
			else
			{
				real_comment_multiline(editor, line_start, last_line);
				count_commented++;
			}

			/* break because we are already on the last line */
			break_loop = TRUE;
			break;
		}
	}

	sci_end_undo_action(editor->sci);

	co_len += tm_len;

	/* restore selection if there is one */
	if (sel_start < sel_end)
	{
		if (single_line)
		{
			gint a = (first_line_was_comment) ? - co_len : co_len;

			/* don't modify sel_start when the selection starts within indentation */
			read_indent(editor, sel_start);
			if ((sel_start - first_line_start) <= (gint) strlen(indent))
				a = 0;

			sci_set_selection_start(editor->sci, sel_start + a);
			sci_set_selection_end(editor->sci, sel_end +
								(count_commented * co_len) - (count_uncommented * co_len));
		}
		else
		{
			gint eol_len = editor_get_eol_char_len(editor);
			if (count_uncommented > 0)
			{
				sci_set_selection_start(editor->sci, sel_start - co_len + eol_len);
				sci_set_selection_end(editor->sci, sel_end - co_len + eol_len);
			}
			else if (count_commented > 0)
			{
				sci_set_selection_start(editor->sci, sel_start + co_len - eol_len);
				sci_set_selection_end(editor->sci, sel_end + co_len - eol_len);
			}
		}
	}
	else if (count_uncommented > 0)
	{
		gint eol_len = single_line ? 0: editor_get_eol_char_len(editor);
		sci_set_current_position(editor->sci, sel_start - co_len + eol_len, TRUE);
	}
	else if (count_commented > 0)
	{
		gint eol_len = single_line ? 0: editor_get_eol_char_len(editor);
		sci_set_current_position(editor->sci, sel_start + co_len - eol_len, TRUE);
	}
}


/* set toggle to TRUE if the caller is the toggle function, FALSE otherwise */
void editor_do_comment(GeanyEditor *editor, gint line, gboolean allow_empty_lines, gboolean toggle)
{
	gint first_line, last_line;
	gint x, i, line_start, line_len;
	gint sel_start, sel_end, co_len;
	gchar sel[256], *co, *cc;
	gboolean break_loop = FALSE, single_line = FALSE;
	GeanyFiletype *ft;

	g_return_if_fail(editor != NULL && editor->document->file_type != NULL);

	if (line < 0)
	{	/* use selection or current line */
		sel_start = sci_get_selection_start(editor->sci);
		sel_end = sci_get_selection_end(editor->sci);

		first_line = sci_get_line_from_position(editor->sci, sel_start);
		/* Find the last line with chars selected (not EOL char) */
		last_line = sci_get_line_from_position(editor->sci,
			sel_end - editor_get_eol_char_len(editor));
		last_line = MAX(first_line, last_line);
	}
	else
	{
		first_line = last_line = line;
		sel_start = sel_end = sci_get_position_from_line(editor->sci, line);
	}

	ft = editor->document->file_type;

	/* detection of HTML vs PHP code, if non-PHP set filetype to XML */
	line_start = sci_get_position_from_line(editor->sci, first_line);
	if (ft->id == GEANY_FILETYPES_PHP)
	{
		if (! is_style_php(sci_get_style_at(editor->sci, line_start)))
			ft = filetypes[GEANY_FILETYPES_XML];
	}

	co = ft->comment_open;
	cc = ft->comment_close;
	if (co == NULL)
		return;

	co_len = strlen(co);
	if (co_len == 0)
		return;

	sci_start_undo_action(editor->sci);

	for (i = first_line; (i <= last_line) && (! break_loop); i++)
	{
		gint buf_len;

		line_start = sci_get_position_from_line(editor->sci, i);
		line_len = sci_get_line_length(editor->sci, i);
		x = 0;

		buf_len = MIN((gint)sizeof(sel) - 1, line_len - 1);
		if (buf_len < 0)
			continue;
		sci_get_text_range(editor->sci, line_start, line_start + buf_len, sel);
		sel[buf_len] = '\0';

		while (isspace(sel[x])) x++;

		/* to skip blank lines */
		if (allow_empty_lines || (x < line_len && sel[x] != '\0'))
		{
			/* use single line comment */
			if (cc == NULL || strlen(cc) == 0)
			{
				gint start = line_start;
				single_line = TRUE;

				if (ft->comment_use_indent)
					start = line_start + x;

				if (toggle)
				{
					gchar *text = g_strconcat(co, editor_prefs.comment_toggle_mark, NULL);
					sci_insert_text(editor->sci, start, text);
					g_free(text);
				}
				else
					sci_insert_text(editor->sci, start, co);
			}
			/* use multi line comment */
			else
			{
				gint style_comment;

				/* skip lines which are already comments */
				style_comment = get_multiline_comment_style(editor, line_start);
				if (sci_get_style_at(editor->sci, line_start + x) == style_comment)
					continue;

				real_comment_multiline(editor, line_start, last_line);

				/* break because we are already on the last line */
				break_loop = TRUE;
				break;
			}
		}
	}
	sci_end_undo_action(editor->sci);

	/* restore selection if there is one
	 * but don't touch the selection if caller is editor_do_comment_toggle */
	if (! toggle && sel_start < sel_end)
	{
		if (single_line)
		{
			sci_set_selection_start(editor->sci, sel_start + co_len);
			sci_set_selection_end(editor->sci, sel_end + ((i - first_line) * co_len));
		}
		else
		{
			gint eol_len = editor_get_eol_char_len(editor);
			sci_set_selection_start(editor->sci, sel_start + co_len + eol_len);
			sci_set_selection_end(editor->sci, sel_end + co_len + eol_len);
		}
	}
}


static gboolean brace_timeout_active = FALSE;

static gboolean delay_match_brace(G_GNUC_UNUSED gpointer user_data)
{
	GeanyDocument *doc = document_get_current();
	GeanyEditor *editor;
	gint brace_pos = GPOINTER_TO_INT(user_data);
	gint end_pos, cur_pos;

	brace_timeout_active = FALSE;
	if (!doc)
		return FALSE;

	editor = doc->editor;
	cur_pos = sci_get_current_position(editor->sci) - 1;

	if (cur_pos != brace_pos)
	{
		cur_pos++;
		if (cur_pos != brace_pos)
		{
			/* we have moved past the original brace_pos, but after the timeout
			 * we may now be on a new brace, so check again */
			editor_highlight_braces(editor, cur_pos);
			return FALSE;
		}
	}
	if (!utils_isbrace(sci_get_char_at(editor->sci, brace_pos), editor_prefs.brace_match_ltgt))
	{
		editor_highlight_braces(editor, cur_pos);
		return FALSE;
	}
	end_pos = sci_find_matching_brace(editor->sci, brace_pos);

	if (end_pos >= 0)
	{
		gint col = MIN(sci_get_col_from_position(editor->sci, brace_pos),
			sci_get_col_from_position(editor->sci, end_pos));
		SSM(editor->sci, SCI_SETHIGHLIGHTGUIDE, col, 0);
		SSM(editor->sci, SCI_BRACEHIGHLIGHT, brace_pos, end_pos);
	}
	else
	{
		SSM(editor->sci, SCI_SETHIGHLIGHTGUIDE, 0, 0);
		SSM(editor->sci, SCI_BRACEBADLIGHT, brace_pos, 0);
	}
	return FALSE;
}


static void editor_highlight_braces(GeanyEditor *editor, gint cur_pos)
{
	gint brace_pos = cur_pos - 1;

	SSM(editor->sci, SCI_SETHIGHLIGHTGUIDE, 0, 0);
	SSM(editor->sci, SCI_BRACEBADLIGHT, (uptr_t)-1, 0);

	if (! utils_isbrace(sci_get_char_at(editor->sci, brace_pos), editor_prefs.brace_match_ltgt))
	{
		brace_pos++;
		if (! utils_isbrace(sci_get_char_at(editor->sci, brace_pos), editor_prefs.brace_match_ltgt))
		{
			return;
		}
	}
	if (!brace_timeout_active)
	{
		brace_timeout_active = TRUE;
		/* delaying matching makes scrolling faster e.g. holding down arrow keys */
		g_timeout_add(100, delay_match_brace, GINT_TO_POINTER(brace_pos));
	}
}


static gboolean in_block_comment(gint lexer, gint style)
{
	switch (lexer)
	{
		case SCLEX_CPP:
			return (style == SCE_C_COMMENT ||
				style == SCE_C_COMMENTDOC);

		case SCLEX_PASCAL:
			return (style == SCE_PAS_COMMENT ||
				style == SCE_PAS_COMMENT2);

		case SCLEX_D:
			return (style == SCE_D_COMMENT ||
				style == SCE_D_COMMENTDOC ||
				style == SCE_D_COMMENTNESTED);

		case SCLEX_HTML:
			return (style == SCE_HPHP_COMMENT);

		case SCLEX_CSS:
			return (style == SCE_CSS_COMMENT);

		default:
			return FALSE;
	}
}


static gboolean is_comment_char(gchar c, gint lexer)
{
	if ((c == '*' || c == '+') && lexer == SCLEX_D)
		return TRUE;
	else
	if (c == '*')
		return TRUE;

	return FALSE;
}


static void auto_multiline(GeanyEditor *editor, gint cur_line)
{
	ScintillaObject *sci = editor->sci;
	gint indent_pos, style;
	gint lexer = sci_get_lexer(sci);

	/* Use the start of the line enter was pressed on, to avoid any doc keyword styles */
	indent_pos = sci_get_line_indent_position(sci, cur_line - 1);
	style = sci_get_style_at(sci, indent_pos);
	if (!in_block_comment(lexer, style))
		return;

	/* Check whether the comment block continues on this line */
	indent_pos = sci_get_line_indent_position(sci, cur_line);
	if (sci_get_style_at(sci, indent_pos) == style)
	{
		gchar *previous_line = sci_get_line(sci, cur_line - 1);
		/* the type of comment, '*' (C/C++/Java), '+' and the others (D) */
		gchar *continuation = "*";
		gchar *whitespace = ""; /* to hold whitespace if needed */
		gchar *result;
		gint len = strlen(previous_line);
		gint i;

		/* find and stop at end of multi line comment */
		i = len - 1;
		while (i >= 0 && isspace(previous_line[i])) i--;
		if (i >= 1 && is_comment_char(previous_line[i - 1], lexer) && previous_line[i] == '/')
		{
			gint indent_len, indent_width;

			indent_pos = sci_get_line_indent_position(sci, cur_line);
			indent_len = sci_get_col_from_position(sci, indent_pos);
			indent_width = editor_get_indent_prefs(editor)->width;

			/* if there is one too many spaces, delete the last space,
			 * to return to the indent used before the multiline comment was started. */
			if (indent_len % indent_width == 1)
				SSM(sci, SCI_DELETEBACKNOTLINE, 0, 0);	/* remove whitespace indent */
			g_free(previous_line);
			return;
		}
		/* check whether we are on the second line of multi line comment */
		i = 0;
		while (i < len && isspace(previous_line[i])) i++; /* get to start of the line */

		if (i + 1 < len &&
			previous_line[i] == '/' && is_comment_char(previous_line[i + 1], lexer))
		{ /* we are on the second line of a multi line comment, so we have to insert white space */
			whitespace = " ";
		}

		if (G_UNLIKELY(style == SCE_D_COMMENTNESTED))
			continuation = "+"; /* for nested comments in D */

		result = g_strconcat(whitespace, continuation, " ", NULL);
		sci_add_text(sci, result);
		g_free(result);

		g_free(previous_line);
	}
}


/* Checks whether the given style is a string for the given lexer.
 * It doesn't handle LEX_HTML, this should be done by the caller.
 * Returns true if the style is a string, FALSE otherwise.
 *
 * Don't forget STRINGEOL, to prevent completion whilst typing a string with no closing char.
 */
static gboolean is_string_style(gint lexer, gint style)
{
	switch (lexer)
	{
		case SCLEX_CPP:
			return (style == SCE_C_CHARACTER ||
				style == SCE_C_STRING ||
				style == SCE_C_STRINGEOL);

		case SCLEX_PASCAL:
			return (style == SCE_PAS_CHARACTER ||
				style == SCE_PAS_STRING ||
				style == SCE_PAS_STRINGEOL);

		case SCLEX_D:
			return (style == SCE_D_STRING ||
				style == SCE_D_STRINGEOL ||
				style == SCE_D_CHARACTER ||
				style == SCE_D_STRINGB ||
				style == SCE_D_STRINGR);

		case SCLEX_PYTHON:
			return (style == SCE_P_STRING ||
				style == SCE_P_TRIPLE ||
				style == SCE_P_TRIPLEDOUBLE ||
				style == SCE_P_CHARACTER ||
				style == SCE_P_STRINGEOL);

		case SCLEX_F77:
		case SCLEX_FORTRAN:
			return (style == SCE_F_STRING1 ||
				style == SCE_F_STRING2 ||
				style == SCE_F_STRINGEOL);

		case SCLEX_PERL:
			return (/*style == SCE_PL_STRING ||*/ /* may want variable autocompletion "$(foo)" */
				style == SCE_PL_CHARACTER ||
				style == SCE_PL_HERE_DELIM ||
				style == SCE_PL_HERE_Q ||
				style == SCE_PL_HERE_QQ ||
				style == SCE_PL_HERE_QX ||
				style == SCE_PL_POD ||
				style == SCE_PL_STRING_Q ||
				style == SCE_PL_STRING_QQ ||
				style == SCE_PL_STRING_QX ||
				style == SCE_PL_STRING_QR ||
				style == SCE_PL_STRING_QW ||
				style == SCE_PL_POD_VERB);

		case SCLEX_R:
			return (style == SCE_R_STRING);

		case SCLEX_RUBY:
			return (style == SCE_RB_CHARACTER ||
				style == SCE_RB_STRING ||
				style == SCE_RB_HERE_DELIM ||
				style == SCE_RB_HERE_Q ||
				style == SCE_RB_HERE_QQ ||
				style == SCE_RB_HERE_QX ||
				style == SCE_RB_POD);

		case SCLEX_BASH:
			return (style == SCE_SH_STRING);

		case SCLEX_SQL:
			return (style == SCE_SQL_STRING);

		case SCLEX_TCL:
			return (style == SCE_TCL_IN_QUOTE);

		case SCLEX_LUA:
			return (style == SCE_LUA_LITERALSTRING ||
				style == SCE_LUA_CHARACTER ||
				style == SCE_LUA_STRINGEOL ||
				style == SCE_LUA_STRING);

		case SCLEX_HASKELL:
			return (style == SCE_HA_CHARACTER ||
				style == SCE_HA_STRING);

		case SCLEX_FREEBASIC:
			return (style == SCE_B_STRING ||
				style == SCE_B_STRINGEOL);

		case SCLEX_MATLAB:
			return (style == SCE_MATLAB_STRING ||
				style == SCE_MATLAB_DOUBLEQUOTESTRING);

		case SCLEX_HTML:
			return (
				style == SCE_HBA_STRING ||
				style == SCE_HBA_STRINGEOL ||
				style == SCE_HB_STRING ||
				style == SCE_HB_STRINGEOL ||
				style == SCE_H_CDATA ||
				style == SCE_H_DOUBLESTRING ||
				style == SCE_HJA_DOUBLESTRING ||
				style == SCE_HJA_SINGLESTRING ||
				style == SCE_HJA_STRINGEOL ||
				style == SCE_HJ_DOUBLESTRING ||
				style == SCE_HJ_SINGLESTRING ||
				style == SCE_HJ_STRINGEOL ||
				style == SCE_HPA_CHARACTER ||
				style == SCE_HPA_STRING ||
				style == SCE_HPA_TRIPLE ||
				style == SCE_HPA_TRIPLEDOUBLE ||
				style == SCE_HP_CHARACTER ||
				style == SCE_HPHP_HSTRING ||
				style == SCE_HPHP_HSTRING ||  /* HSTRING is a heredoc */
				style == SCE_HPHP_HSTRING_VARIABLE ||
				style == SCE_HPHP_SIMPLESTRING ||
				style == SCE_HPHP_SIMPLESTRING ||
				style == SCE_HP_STRING ||
				style == SCE_HP_TRIPLE ||
				style == SCE_HP_TRIPLEDOUBLE ||
				style == SCE_H_SGML_DOUBLESTRING ||
				style == SCE_H_SGML_SIMPLESTRING ||
				style == SCE_H_SINGLESTRING);

		case SCLEX_CMAKE:
			return (style == SCE_CMAKE_STRINGDQ ||
				style == SCE_CMAKE_STRINGLQ ||
				style == SCE_CMAKE_STRINGRQ ||
				style == SCE_CMAKE_STRINGVAR);

		case SCLEX_NSIS:
			return (style == SCE_NSIS_STRINGDQ ||
				style == SCE_NSIS_STRINGLQ ||
				style == SCE_NSIS_STRINGRQ ||
				style == SCE_NSIS_STRINGVAR);

		case SCLEX_ADA:
			return (style == SCE_ADA_CHARACTER ||
				style == SCE_ADA_STRING ||
				style == SCE_ADA_CHARACTEREOL ||
				style == SCE_ADA_STRINGEOL);
	}
	return FALSE;
}


/* Checks whether the given style is a comment for the given lexer.
 * It doesn't handle LEX_HTML, this should be done by the caller.
 * Returns true if the style is a comment, FALSE otherwise.
 */
static gboolean is_comment_style(gint lexer, gint style)
{
	switch (lexer)
	{
		case SCLEX_CPP:
			return (style == SCE_C_COMMENT ||
				style == SCE_C_COMMENTLINE ||
				style == SCE_C_COMMENTDOC ||
				style == SCE_C_COMMENTLINEDOC ||
				style == SCE_C_COMMENTDOCKEYWORD ||
				style == SCE_C_COMMENTDOCKEYWORDERROR);

		case SCLEX_PASCAL:
			return (style == SCE_PAS_COMMENT ||
				style == SCE_PAS_COMMENT2 ||
				style == SCE_PAS_COMMENTLINE);

		case SCLEX_D:
			return (style == SCE_D_COMMENT ||
				style == SCE_D_COMMENTLINE ||
				style == SCE_D_COMMENTDOC ||
				style == SCE_D_COMMENTNESTED ||
				style == SCE_D_COMMENTLINEDOC ||
				style == SCE_D_COMMENTDOCKEYWORD ||
				style == SCE_D_COMMENTDOCKEYWORDERROR);

		case SCLEX_PYTHON:
			return (style == SCE_P_COMMENTLINE ||
				style == SCE_P_COMMENTBLOCK);

		case SCLEX_F77:
		case SCLEX_FORTRAN:
			return (style == SCE_F_COMMENT);

		case SCLEX_PERL:
			return (style == SCE_PL_COMMENTLINE);

		case SCLEX_PROPERTIES:
			return (style == SCE_PROPS_COMMENT);

		case SCLEX_PO:
			return (style == SCE_PO_COMMENT);

		case SCLEX_LATEX:
			return (style == SCE_L_COMMENT);

		case SCLEX_MAKEFILE:
			return (style == SCE_MAKE_COMMENT);

		case SCLEX_RUBY:
			return (style == SCE_RB_COMMENTLINE);

		case SCLEX_BASH:
			return (style == SCE_SH_COMMENTLINE);

		case SCLEX_R:
			return (style == SCE_R_COMMENT);

		case SCLEX_SQL:
			return (style == SCE_SQL_COMMENT ||
				style == SCE_SQL_COMMENTLINE ||
				style == SCE_SQL_COMMENTDOC);

		case SCLEX_TCL:
			return (style == SCE_TCL_COMMENT ||
				style == SCE_TCL_COMMENTLINE ||
				style == SCE_TCL_COMMENT_BOX ||
				style == SCE_TCL_BLOCK_COMMENT);

		case SCLEX_MATLAB:
			return (style == SCE_MATLAB_COMMENT);

		case SCLEX_LUA:
			return (style == SCE_LUA_COMMENT ||
				style == SCE_LUA_COMMENTLINE ||
				style == SCE_LUA_COMMENTDOC);

		case SCLEX_HASKELL:
			return (style == SCE_HA_COMMENTLINE ||
				style == SCE_HA_COMMENTBLOCK ||
				style == SCE_HA_COMMENTBLOCK2 ||
				style == SCE_HA_COMMENTBLOCK3);

		case SCLEX_FREEBASIC:
			return (style == SCE_B_COMMENT);

		case SCLEX_YAML:
			return (style == SCE_YAML_COMMENT);

		case SCLEX_HTML:
			return (
				style == SCE_HBA_COMMENTLINE ||
				style == SCE_HB_COMMENTLINE ||
				style == SCE_H_COMMENT ||
				style == SCE_HJA_COMMENT ||
				style == SCE_HJA_COMMENTDOC ||
				style == SCE_HJA_COMMENTLINE ||
				style == SCE_HJ_COMMENT ||
				style == SCE_HJ_COMMENTDOC ||
				style == SCE_HJ_COMMENTLINE ||
				style == SCE_HPA_COMMENTLINE ||
				style == SCE_HP_COMMENTLINE ||
				style == SCE_HPHP_COMMENT ||
				style == SCE_HPHP_COMMENTLINE ||
				style == SCE_H_SGML_COMMENT);

		case SCLEX_CMAKE:
			return (style == SCE_CMAKE_COMMENT);

		case SCLEX_NSIS:
			return (style == SCE_NSIS_COMMENT ||
				style == SCE_NSIS_COMMENTBOX);

		case SCLEX_ADA:
			return (style == SCE_ADA_COMMENTLINE ||
				style == SCE_NSIS_COMMENTBOX);
	}
	return FALSE;
}


/* Checks whether the given style is normal code (not string, comment, preprocessor, etc).
 * It doesn't handle LEX_HTML, this should be done by the caller.
 */
static gboolean is_code_style(gint lexer, gint style)
{
	switch (lexer)
	{
		case SCLEX_CPP:
			if (style == SCE_C_PREPROCESSOR)
				return FALSE;
			break;
	}
	return !(is_comment_style(lexer, style) ||
		is_string_style(lexer, style));
}


#if 0
static gboolean editor_lexer_is_c_like(gint lexer)
{
	switch (lexer)
	{
		case SCLEX_CPP:
		case SCLEX_D:
		return TRUE;

		default:
		return FALSE;
	}
}
#endif


/* Returns: -1 if lexer doesn't support type keywords */
gint editor_lexer_get_type_keyword_idx(gint lexer)
{
	switch (lexer)
	{
		case SCLEX_CPP:
		case SCLEX_D:
		return 3;

		default:
		return -1;
	}
}


/* inserts a three-line comment at one line above current cursor position */
void editor_insert_multiline_comment(GeanyEditor *editor)
{
	gchar *text;
	gint text_len;
	gint line;
	gint pos;
	gboolean have_multiline_comment = FALSE;
	GeanyDocument *doc;

	g_return_if_fail(editor != NULL &&
		editor->document->file_type != NULL && editor->document->file_type->comment_open != NULL);

	doc = editor->document;

	if (doc->file_type->comment_close != NULL && strlen(doc->file_type->comment_close) > 0)
		have_multiline_comment = TRUE;

	/* insert three lines one line above of the current position */
	line = sci_get_line_from_position(editor->sci, editor_info.click_pos);
	pos = sci_get_position_from_line(editor->sci, line);

	/* use the indent on the current line but only when comment indentation is used
	 * and we don't have multi line comment characters */
	if (editor->auto_indent &&
		! have_multiline_comment &&	doc->file_type->comment_use_indent)
	{
		read_indent(editor, editor_info.click_pos);
		text = g_strdup_printf("%s\n%s\n%s\n", indent, indent, indent);
		text_len = strlen(text);
	}
	else
	{
		text = g_strdup("\n\n\n");
		text_len = 3;
	}
	sci_insert_text(editor->sci, pos, text);
	g_free(text);


	/* select the inserted lines for commenting */
	sci_set_selection_start(editor->sci, pos);
	sci_set_selection_end(editor->sci, pos + text_len);

	editor_do_comment(editor, -1, TRUE, FALSE);

	/* set the current position to the start of the first inserted line */
	pos += strlen(doc->file_type->comment_open);

	/* on multi line comment jump to the next line, otherwise add the length of added indentation */
	if (have_multiline_comment)
		pos += 1;
	else
		pos += strlen(indent);

	sci_set_current_position(editor->sci, pos, TRUE);
	/* reset the selection */
	sci_set_anchor(editor->sci, pos);
}


/* Note: If the editor is pending a redraw, set document::scroll_percent instead.
 * Scroll the view to make line appear at percent_of_view.
 * line can be -1 to use the current position. */
void editor_scroll_to_line(GeanyEditor *editor, gint line, gfloat percent_of_view)
{
	gint vis1, los, delta;
	GtkWidget *wid;

	g_return_if_fail(editor != NULL);

	wid = GTK_WIDGET(editor->sci);

	if (! wid->window || ! gdk_window_is_viewable(wid->window))
		return;	/* prevent gdk_window_scroll warning */

	if (line == -1)
		line = sci_get_current_line(editor->sci);

	/* sci 'visible line' != doc line number because of folding and line wrapping */
	/* calling SCI_VISIBLEFROMDOCLINE for line is more accurate than calling
	 * SCI_DOCLINEFROMVISIBLE for vis1. */
	line = SSM(editor->sci, SCI_VISIBLEFROMDOCLINE, line, 0);
	vis1 = SSM(editor->sci, SCI_GETFIRSTVISIBLELINE, 0, 0);
	los = SSM(editor->sci, SCI_LINESONSCREEN, 0, 0);
	delta = (line - vis1) - los * percent_of_view;
	sci_scroll_lines(editor->sci, delta);
	sci_scroll_caret(editor->sci); /* needed for horizontal scrolling */
}


/* creates and inserts one tab or whitespace of the amount of the tab width */
void editor_insert_alternative_whitespace(GeanyEditor *editor)
{
	gchar *text;
	GeanyIndentPrefs iprefs = *editor_get_indent_prefs(editor);

	g_return_if_fail(editor != NULL);

	switch (iprefs.type)
	{
		case GEANY_INDENT_TYPE_TABS:
			iprefs.type = GEANY_INDENT_TYPE_SPACES;
			break;
		case GEANY_INDENT_TYPE_SPACES:
		case GEANY_INDENT_TYPE_BOTH:	/* most likely we want a tab */
			iprefs.type = GEANY_INDENT_TYPE_TABS;
			break;
	}
	text = get_whitespace(&iprefs, iprefs.width);
	sci_add_text(editor->sci, text);
	g_free(text);
}


void editor_select_word(GeanyEditor *editor)
{
	gint pos;
	gint start;
	gint end;

	g_return_if_fail(editor != NULL);

	pos = SSM(editor->sci, SCI_GETCURRENTPOS, 0, 0);
	start = SSM(editor->sci, SCI_WORDSTARTPOSITION, pos, TRUE);
	end = SSM(editor->sci, SCI_WORDENDPOSITION, pos, TRUE);

	if (start == end) /* caret in whitespaces sequence */
	{
		/* look forward but reverse the selection direction,
		 * so the caret end up stay as near as the original position. */
		end = SSM(editor->sci, SCI_WORDENDPOSITION, pos, FALSE);
		start = SSM(editor->sci, SCI_WORDENDPOSITION, end, TRUE);
		if (start == end)
			return;
	}

	sci_set_selection(editor->sci, start, end);
}


/* extra_line is for selecting the cursor line (or anchor line) at the bottom of a selection,
 * when those lines have no selection (cursor at start of line). */
void editor_select_lines(GeanyEditor *editor, gboolean extra_line)
{
	gint start, end, line;

	g_return_if_fail(editor != NULL);

	start = sci_get_selection_start(editor->sci);
	end = sci_get_selection_end(editor->sci);

	/* check if whole lines are already selected */
	if (! extra_line && start != end &&
		sci_get_col_from_position(editor->sci, start) == 0 &&
		sci_get_col_from_position(editor->sci, end) == 0)
			return;

	line = sci_get_line_from_position(editor->sci, start);
	start = sci_get_position_from_line(editor->sci, line);

	line = sci_get_line_from_position(editor->sci, end);
	end = sci_get_position_from_line(editor->sci, line + 1);

	sci_set_selection(editor->sci, start, end);
}


/* find the start or end of a paragraph by searching all lines in direction (UP or DOWN)
 * starting at the given line and return the found line or return -1 if called on an empty line */
static gint find_paragraph_stop(GeanyEditor *editor, gint line, gint direction)
{
	gboolean found_end = FALSE;
	gint step;
	gchar *line_buf, *x;
	ScintillaObject *sci = editor->sci;

	/* first check current line and return -1 if it is empty to skip creating of a selection */
	line_buf = x = sci_get_line(sci, line);
	while (isspace(*x))
		x++;
	if (*x == '\0')
	{
		g_free(line_buf);
		return -1;
	}

	if (direction == GTK_DIR_UP)
		step = -1;
	else
		step = 1;

	while (! found_end)
	{
		line += step;

		/* sci_get_line checks for sanity of the given line, sci_get_line always return a string
		 * containing at least '\0' so no need to check for NULL */
		line_buf = x = sci_get_line(sci, line);

		/* check whether after skipping all whitespace we are at end of line and if so, assume
		 * this line as end of paragraph */
		while (isspace(*x))
			x++;
		if (*x == '\0')
		{
			found_end = TRUE;
			if (line == -1)
				/* called on the first line but there is no previous line so return line 0 */
				line = 0;
		}
		g_free(line_buf);
	}
	return line;
}


void editor_select_paragraph(GeanyEditor *editor)
{
	gint pos_start, pos_end, line_start, line_found;

	g_return_if_fail(editor != NULL);

	line_start = SSM(editor->sci, SCI_LINEFROMPOSITION,
						SSM(editor->sci, SCI_GETCURRENTPOS, 0, 0), 0);

	line_found = find_paragraph_stop(editor, line_start, GTK_DIR_UP);
	if (line_found == -1)
		return;

	/* find_paragraph_stop returns the emtpy line(previous to the real start of the paragraph),
	 * so use the next line for selection start */
	if (line_found > 0)
		line_found++;

	pos_start = SSM(editor->sci, SCI_POSITIONFROMLINE, line_found, 0);

	line_found = find_paragraph_stop(editor, line_start, GTK_DIR_DOWN);
	pos_end = SSM(editor->sci, SCI_POSITIONFROMLINE, line_found, 0);

	sci_set_selection(editor->sci, pos_start, pos_end);
}


/* simple indentation to indent the current line with the same indent as the previous one */
static void smart_line_indentation(GeanyEditor *editor, gint first_line, gint last_line)
{
	gint i, sel_start = 0, sel_end = 0;

	/* get previous line and use it for read_indent to use that line
	 * (otherwise it would fail on a line only containing "{" in advanced indentation mode) */
	read_indent(editor, sci_get_position_from_line(editor->sci, first_line - 1));

	for (i = first_line; i <= last_line; i++)
	{
		/* skip the first line or if the indentation of the previous and current line are equal */
		if (i == 0 ||
			SSM(editor->sci, SCI_GETLINEINDENTATION, i - 1, 0) ==
			SSM(editor->sci, SCI_GETLINEINDENTATION, i, 0))
			continue;

		sel_start = SSM(editor->sci, SCI_POSITIONFROMLINE, i, 0);
		sel_end = SSM(editor->sci, SCI_GETLINEINDENTPOSITION, i, 0);
		if (sel_start < sel_end)
		{
			sci_set_selection(editor->sci, sel_start, sel_end);
			sci_replace_sel(editor->sci, "");
		}
		sci_insert_text(editor->sci, sel_start, indent);
	}
}


/* simple indentation to indent the current line with the same indent as the previous one */
void editor_smart_line_indentation(GeanyEditor *editor, gint pos)
{
	gint first_line, last_line;
	gint first_sel_start, first_sel_end;
	ScintillaObject *sci;

	g_return_if_fail(editor != NULL);

	sci = editor->sci;

	first_sel_start = sci_get_selection_start(sci);
	first_sel_end = sci_get_selection_end(sci);

	first_line = sci_get_line_from_position(sci, first_sel_start);
	/* Find the last line with chars selected (not EOL char) */
	last_line = sci_get_line_from_position(sci, first_sel_end - editor_get_eol_char_len(editor));
	last_line = MAX(first_line, last_line);

	if (pos == -1)
		pos = first_sel_start;

	sci_start_undo_action(sci);

	smart_line_indentation(editor, first_line, last_line);

	/* set cursor position if there was no selection */
	if (first_sel_start == first_sel_end)
	{
		gint indent_pos = SSM(sci, SCI_GETLINEINDENTPOSITION, first_line, 0);

		/* use indent position as user may wish to change indentation afterwards */
		sci_set_current_position(sci, indent_pos, FALSE);
	}
	else
	{
		/* fully select all the lines affected */
		sci_set_selection_start(sci, sci_get_position_from_line(sci, first_line));
		sci_set_selection_end(sci, sci_get_position_from_line(sci, last_line + 1));
	}

	sci_end_undo_action(sci);
}


/* increase / decrease current line or selection by one space */
void editor_indentation_by_one_space(GeanyEditor *editor, gint pos, gboolean decrease)
{
	gint i, first_line, last_line, line_start, indentation_end, count = 0;
	gint sel_start, sel_end, first_line_offset = 0;

	g_return_if_fail(editor != NULL);

	sel_start = sci_get_selection_start(editor->sci);
	sel_end = sci_get_selection_end(editor->sci);

	first_line = sci_get_line_from_position(editor->sci, sel_start);
	/* Find the last line with chars selected (not EOL char) */
	last_line = sci_get_line_from_position(editor->sci, sel_end - editor_get_eol_char_len(editor));
	last_line = MAX(first_line, last_line);

	if (pos == -1)
		pos = sel_start;

	sci_start_undo_action(editor->sci);

	for (i = first_line; i <= last_line; i++)
	{
		indentation_end = SSM(editor->sci, SCI_GETLINEINDENTPOSITION, i, 0);
		if (decrease)
		{
			line_start = SSM(editor->sci, SCI_POSITIONFROMLINE, i, 0);
			/* searching backwards for a space to remove */
			while (sci_get_char_at(editor->sci, indentation_end) != ' ' && indentation_end > line_start)
				indentation_end--;

			if (sci_get_char_at(editor->sci, indentation_end) == ' ')
			{
				sci_set_selection(editor->sci, indentation_end, indentation_end + 1);
				sci_replace_sel(editor->sci, "");
				count--;
				if (i == first_line)
					first_line_offset = -1;
			}
		}
		else
		{
			sci_insert_text(editor->sci, indentation_end, " ");
			count++;
			if (i == first_line)
				first_line_offset = 1;
		}
	}

	/* set cursor position */
	if (sel_start < sel_end)
	{
		gint start = sel_start + first_line_offset;
		if (first_line_offset < 0)
			start = MAX(sel_start + first_line_offset,
						SSM(editor->sci, SCI_POSITIONFROMLINE, first_line, 0));

		sci_set_selection_start(editor->sci, start);
		sci_set_selection_end(editor->sci, sel_end + count);
	}
	else
		sci_set_current_position(editor->sci, pos + count, FALSE);

	sci_end_undo_action(editor->sci);
}


void editor_finalize()
{
	scintilla_release_resources();
}


/* wordchars: NULL or a string containing characters to match a word.
 * Returns: the current selection or the current word. */
gchar *editor_get_default_selection(GeanyEditor *editor, gboolean use_current_word,
									const gchar *wordchars)
{
	gchar *s = NULL;

	g_return_val_if_fail(editor != NULL, NULL);

	if (sci_get_lines_selected(editor->sci) == 1)
	{
		gint len = sci_get_selected_text_length(editor->sci);

		s = g_malloc(len + 1);
		sci_get_selected_text(editor->sci, s);
	}
	else if (sci_get_lines_selected(editor->sci) == 0 && use_current_word)
	{	/* use the word at current cursor position */
		gchar word[GEANY_MAX_WORD_LENGTH];

		editor_find_current_word(editor, -1, word, sizeof(word), wordchars);
		if (word[0] != '\0')
			s = g_strdup(word);
	}
	return s;
}


/* Note: Usually the line should be made visible (not folded) before calling this.
 * Returns: TRUE if line is/will be displayed to the user, or FALSE if it is
 * outside the *vertical* view.
 * Warning: You may need horizontal scrolling to make the cursor visible - so always call
 * sci_scroll_caret() when this returns TRUE. */
gboolean editor_line_in_view(GeanyEditor *editor, gint line)
{
	gint vis1, los;

	g_return_val_if_fail(editor != NULL, FALSE);

	/* If line is wrapped the result may occur on another virtual line than the first and may be
	 * still hidden, so increase the line number to check for the next document line */
	if (SSM(editor->sci, SCI_WRAPCOUNT, line, 0) > 1)
		line++;

	line = SSM(editor->sci, SCI_VISIBLEFROMDOCLINE, line, 0);	/* convert to visible line number */
	vis1 = SSM(editor->sci, SCI_GETFIRSTVISIBLELINE, 0, 0);
	los = SSM(editor->sci, SCI_LINESONSCREEN, 0, 0);

	return (line >= vis1 && line < vis1 + los);
}


/* If the current line is outside the current view window, scroll the line
 * so it appears at percent_of_view. */
void editor_display_current_line(GeanyEditor *editor, gfloat percent_of_view)
{
	gint line;

	g_return_if_fail(editor != NULL);

	line = sci_get_current_line(editor->sci);

	/* unfold maybe folded results */
	sci_ensure_line_is_visible(editor->sci, line);

	/* scroll the line if it's off screen */
	if (! editor_line_in_view(editor, line))
		editor->scroll_percent = percent_of_view;
	else
		sci_scroll_caret(editor->sci); /* may need horizontal scrolling */
}


 /*
 *  Deletes all currently set indicators in the @a editor window.
 *  Error indicators (red squiggly underlines) and usual line markers are removed.
 *
 *  @param editor The editor to operate on.
 */
void editor_indicator_clear_errors(GeanyEditor *editor)
{
	editor_indicator_clear(editor, GEANY_INDICATOR_ERROR);
	sci_marker_delete_all(editor->sci, 0);	/* remove the yellow error line marker */
}


/**
 *  Deletes all currently set indicators matching @a indic in the @a editor window.
 *
 *  @param editor The editor to operate on.
 *  @param indic The indicator number to clear, this is a value of @ref GeanyIndicator.
 *
 *  @since 0.16
 */
void editor_indicator_clear(GeanyEditor *editor, gint indic)
{
	glong last_pos;

	g_return_if_fail(editor != NULL);

	last_pos = sci_get_length(editor->sci);
	if (last_pos > 0)
	{
		sci_indicator_set(editor->sci, indic);
		sci_indicator_clear(editor->sci, 0, last_pos);
	}
}


/**
 *  Sets an indicator @a indic on @a line.
 *  Whitespace at the start and the end of the line is not marked.
 *
 *  @param editor The editor to operate on.
 *  @param indic The indicator number to use, this is a value of @ref GeanyIndicator.
 *  @param line The line number which should be marked.
 *
 *  @since 0.16
 */
void editor_indicator_set_on_line(GeanyEditor *editor, gint indic, gint line)
{
	gint start, end;
	guint i = 0, len;
	gchar *linebuf;

	g_return_if_fail(editor != NULL);
	g_return_if_fail(line >= 0);

	start = sci_get_position_from_line(editor->sci, line);
	end = sci_get_position_from_line(editor->sci, line + 1);

	/* skip blank lines */
	if ((start + 1) == end ||
		start > end ||
		sci_get_line_length(editor->sci, line) == editor_get_eol_char_len(editor))
	{
		return;
	}

	len = end - start;
	linebuf = sci_get_line(editor->sci, line);

	/* don't set the indicator on whitespace */
	while (isspace(linebuf[i]))
		i++;
	while (len > 1 && len > i && isspace(linebuf[len - 1]))
	{
		len--;
		end--;
	}
	g_free(linebuf);

	editor_indicator_set_on_range(editor, indic, start + i, end);
}


/**
 *  Sets an indicator on the range specified by @a start and @a end.
 *  No error checking or whitespace removal is performed, this should be done by the calling
 *  function if necessary.
 *
 *  @param editor The editor to operate on.
 *  @param indic The indicator number to use, this is a value of @ref GeanyIndicator.
 *  @param start The starting position for the marker.
 *  @param end The ending position for the marker.
 *
 *  @since 0.16
 */
void editor_indicator_set_on_range(GeanyEditor *editor, gint indic, gint start, gint end)
{
	g_return_if_fail(editor != NULL);
	if (start >= end)
		return;

	sci_indicator_set(editor->sci, indic);
	sci_indicator_fill(editor->sci, start, end - start);
}


/* Inserts the given colour (format should be #...), if there is a selection starting with 0x...
 * the replacement will also start with 0x... */
void editor_insert_color(GeanyEditor *editor, const gchar *colour)
{
	g_return_if_fail(editor != NULL);

	if (sci_has_selection(editor->sci))
	{
		gint start = sci_get_selection_start(editor->sci);
		const gchar *replacement = colour;

		if (sci_get_char_at(editor->sci, start) == '0' &&
			sci_get_char_at(editor->sci, start + 1) == 'x')
		{
			sci_set_selection_start(editor->sci, start + 2);
			sci_set_selection_end(editor->sci, start + 8);
			replacement++; /* skip the leading "0x" */
		}
		else if (sci_get_char_at(editor->sci, start - 1) == '#')
		{	/* double clicking something like #00ffff may only select 00ffff because of wordchars */
			replacement++; /* so skip the '#' to only replace the colour value */
		}
		sci_replace_sel(editor->sci, replacement);
	}
	else
		sci_add_text(editor->sci, colour);
}


/**
 *  Retrieves the localized name (for displaying) of the used end of line characters
 *  (LF, CR/LF, CR) in the given editor.
 *  If @a editor is @c NULL, the default end of line characters are used.
 *
 *  @param editor The editor to operate on, or @c NULL to query the default value.
 *  @return The name of the end of line characters.
 *
 *  @since 0.19
 */
const gchar *editor_get_eol_char_name(GeanyEditor *editor)
{
	gint mode = file_prefs.default_eol_character;

	if (editor != NULL)
		mode = sci_get_eol_mode(editor->sci);

	return utils_get_eol_name(mode);
}


/**
 *  Retrieves the length of the used end of line characters (LF, CR/LF, CR) in the given editor.
 *  If @a editor is @c NULL, the default end of line characters are used.
 *  The returned value is 1 for CR and LF and 2 for CR/LF.
 *
 *  @param editor The editor to operate on, or @c NULL to query the default value.
 *  @return The length of the end of line characters.
 *
 *  @since 0.19
 */
gint editor_get_eol_char_len(GeanyEditor *editor)
{
	gint mode = file_prefs.default_eol_character;

	if (editor != NULL)
		mode = sci_get_eol_mode(editor->sci);

	switch (mode)
	{
		case SC_EOL_CRLF: return 2; break;
		default: return 1; break;
	}
}


/**
 *  Retrieves the used end of line characters (LF, CR/LF, CR) in the given editor.
 *  If @a editor is @c NULL, the default end of line characters are used.
 *  The returned value is either "\n", "\r\n" or "\r".
 *
 *  @param editor The editor to operate on, or @c NULL to query the default value.
 *  @return The end of line characters.
 *
 *  @since 0.19
 */
const gchar *editor_get_eol_char(GeanyEditor *editor)
{
	gint mode = file_prefs.default_eol_character;

	if (editor != NULL)
		mode = sci_get_eol_mode(editor->sci);

	switch (mode)
	{
		case SC_EOL_CRLF: return "\r\n"; break;
		case SC_EOL_CR: return "\r"; break;
		default: return "\n"; break;
	}
}


static void fold_all(GeanyEditor *editor, gboolean want_fold)
{
	gint lines, first, i;

	if (editor == NULL || ! editor_prefs.folding)
		return;

	lines = sci_get_line_count(editor->sci);
	first = sci_get_first_visible_line(editor->sci);

	for (i = 0; i < lines; i++)
	{
		gint level = sci_get_fold_level(editor->sci, i);

		if (level & SC_FOLDLEVELHEADERFLAG)
		{
			if (sci_get_fold_expanded(editor->sci, i) == want_fold)
					sci_toggle_fold(editor->sci, i);
		}
	}
	editor_scroll_to_line(editor, first, 0.0F);
}


void editor_unfold_all(GeanyEditor *editor)
{
	fold_all(editor, FALSE);
}


void editor_fold_all(GeanyEditor *editor)
{
	fold_all(editor, TRUE);
}


void editor_replace_tabs(GeanyEditor *editor)
{
	gint search_pos, pos_in_line, current_tab_true_length;
	gint tab_len;
	gchar *tab_str;
	struct Sci_TextToFind ttf;

	g_return_if_fail(editor != NULL);

	sci_start_undo_action(editor->sci);
	tab_len = sci_get_tab_width(editor->sci);
	ttf.chrg.cpMin = 0;
	ttf.chrg.cpMax = sci_get_length(editor->sci);
	ttf.lpstrText = (gchar*) "\t";

	while (TRUE)
	{
		search_pos = sci_find_text(editor->sci, SCFIND_MATCHCASE, &ttf);
		if (search_pos == -1)
			break;

		pos_in_line = sci_get_col_from_position(editor->sci, search_pos);
		current_tab_true_length = tab_len - (pos_in_line % tab_len);
		tab_str = g_strnfill(current_tab_true_length, ' ');
		sci_set_target_start(editor->sci, search_pos);
		sci_set_target_end(editor->sci, search_pos + 1);
		sci_replace_target(editor->sci, tab_str, FALSE);
		/* next search starts after replacement */
		ttf.chrg.cpMin = search_pos + current_tab_true_length - 1;
		/* update end of range now text has changed */
		ttf.chrg.cpMax += current_tab_true_length - 1;
		g_free(tab_str);
	}
	sci_end_undo_action(editor->sci);
}


/* Replaces all occurrences all spaces of the length of a given tab_width. */
void editor_replace_spaces(GeanyEditor *editor)
{
	gint search_pos;
	static gdouble tab_len_f = -1.0; /* keep the last used value */
	gint tab_len;
	struct Sci_TextToFind ttf;

	g_return_if_fail(editor != NULL);

	if (tab_len_f < 0.0)
		tab_len_f = sci_get_tab_width(editor->sci);

	if (! dialogs_show_input_numeric(
		_("Enter Tab Width"),
		_("Enter the amount of spaces which should be replaced by a tab character."),
		&tab_len_f, 1, 100, 1))
	{
		return;
	}
	tab_len = (gint) tab_len_f;

	sci_start_undo_action(editor->sci);
	ttf.chrg.cpMin = 0;
	ttf.chrg.cpMax = sci_get_length(editor->sci);
	ttf.lpstrText = g_strnfill(tab_len, ' ');

	while (TRUE)
	{
		search_pos = sci_find_text(editor->sci, SCFIND_MATCHCASE, &ttf);
		if (search_pos == -1)
			break;

		sci_set_target_start(editor->sci, search_pos);
		sci_set_target_end(editor->sci, search_pos + tab_len);
		sci_replace_target(editor->sci, "\t", FALSE);
		ttf.chrg.cpMin = search_pos;
		/* update end of range now text has changed */
		ttf.chrg.cpMax -= tab_len - 1;
	}
	sci_end_undo_action(editor->sci);
	g_free(ttf.lpstrText);
}


void editor_strip_line_trailing_spaces(GeanyEditor *editor, gint line)
{
	gint line_start = sci_get_position_from_line(editor->sci, line);
	gint line_end = sci_get_line_end_position(editor->sci, line);
	gint i = line_end - 1;
	gchar ch = sci_get_char_at(editor->sci, i);

	while ((i >= line_start) && ((ch == ' ') || (ch == '\t')))
	{
		i--;
		ch = sci_get_char_at(editor->sci, i);
	}
	if (i < (line_end - 1))
	{
		sci_set_target_start(editor->sci, i + 1);
		sci_set_target_end(editor->sci, line_end);
		sci_replace_target(editor->sci, "", FALSE);
	}
}


void editor_strip_trailing_spaces(GeanyEditor *editor)
{
	gint max_lines = sci_get_line_count(editor->sci);
	gint line;

	sci_start_undo_action(editor->sci);

	for (line = 0; line < max_lines; line++)
	{
		editor_strip_line_trailing_spaces(editor, line);
	}
	sci_end_undo_action(editor->sci);
}


void editor_ensure_final_newline(GeanyEditor *editor)
{
	gint max_lines = sci_get_line_count(editor->sci);
	gboolean append_newline = (max_lines == 1);
	gint end_document = sci_get_position_from_line(editor->sci, max_lines);

	if (max_lines > 1)
	{
		append_newline = end_document > sci_get_position_from_line(editor->sci, max_lines - 1);
	}
	if (append_newline)
	{
		const gchar *eol = "\n";
		switch (sci_get_eol_mode(editor->sci))
		{
			case SC_EOL_CRLF:
				eol = "\r\n";
				break;
			case SC_EOL_CR:
				eol = "\r";
				break;
		}
		sci_insert_text(editor->sci, end_document, eol);
	}
}


void editor_set_font(GeanyEditor *editor, const gchar *font)
{
	gint style, size;
	gchar *font_name;
	PangoFontDescription *pfd;

	g_return_if_fail(editor);

	pfd = pango_font_description_from_string(font);
	size = pango_font_description_get_size(pfd) / PANGO_SCALE;
	font_name = g_strdup_printf("!%s", pango_font_description_get_family(pfd));
	pango_font_description_free(pfd);

	for (style = 0; style <= 127; style++)
		sci_set_font(editor->sci, style, font_name, size);

	/* line number and braces */
	sci_set_font(editor->sci, STYLE_LINENUMBER, font_name, size);
	sci_set_font(editor->sci, STYLE_BRACELIGHT, font_name, size);
	sci_set_font(editor->sci, STYLE_BRACEBAD, font_name, size);
	g_free(font_name);

	/* zoom to 100% to prevent confusion */
	sci_zoom_off(editor->sci);
}


void editor_set_line_wrapping(GeanyEditor *editor, gboolean wrap)
{
	g_return_if_fail(editor != NULL);

	editor->line_wrapping = wrap;
	sci_set_lines_wrapped(editor->sci, wrap);
}


/** Sets the indent type for @a editor.
 * @param editor Editor.
 * @param type Indent type.
 *
 *  @since 0.16
 */
void editor_set_indent_type(GeanyEditor *editor, GeanyIndentType type)
{
	const GeanyIndentPrefs *iprefs = editor_get_indent_prefs(editor);
	ScintillaObject *sci = editor->sci;
	gboolean use_tabs = type != GEANY_INDENT_TYPE_SPACES;

	editor->indent_type = type;
	sci_set_use_tabs(sci, use_tabs);

	if (type == GEANY_INDENT_TYPE_BOTH)
		sci_set_tab_width(sci, iprefs->hard_tab_width);
	else
		sci_set_tab_width(sci, iprefs->width);
	SSM(sci, SCI_SETINDENT, iprefs->width, 0);

	/* remove indent spaces on backspace, if using any spaces to indent */
	SSM(sci, SCI_SETBACKSPACEUNINDENTS, type != GEANY_INDENT_TYPE_TABS, 0);
}


/* Convenience function for editor_goto_pos() to pass in a line number. */
gboolean editor_goto_line(GeanyEditor *editor, gint line)
{
	gint pos;

	g_return_val_if_fail(editor, FALSE);
	if (line < 0 || line >= sci_get_line_count(editor->sci))
		return FALSE;

	pos = sci_get_position_from_line(editor->sci, line);
	return editor_goto_pos(editor, pos, TRUE);
}


/* Move to position @a pos, switching to the document if necessary,
 * setting a marker if @a mark is set. */
gboolean editor_goto_pos(GeanyEditor *editor, gint pos, gboolean mark)
{
	gint page_num;

	g_return_val_if_fail(editor, FALSE);
	if (G_UNLIKELY(pos < 0))
		return FALSE;

	if (mark)
	{
		gint line = sci_get_line_from_position(editor->sci, pos);

		/* mark the tag with the yellow arrow */
		sci_marker_delete_all(editor->sci, 0);
		sci_set_marker_at_line(editor->sci, line, 0);
	}

	sci_goto_pos(editor->sci, pos, TRUE);
	editor->scroll_percent = 0.25F;

	/* finally switch to the page */
	page_num = gtk_notebook_page_num(GTK_NOTEBOOK(main_widgets.notebook), GTK_WIDGET(editor->sci));
	gtk_notebook_set_current_page(GTK_NOTEBOOK(main_widgets.notebook), page_num);

	return TRUE;
}


static gboolean
on_editor_scroll_event(GtkWidget *widget, GdkEventScroll *event, gpointer user_data)
{
	GeanyEditor *editor = user_data;

	/* Handle scroll events if Alt is pressed and scroll whole pages instead of a
	 * few lines only, maybe this could/should be done in Scintilla directly */
	if (event->state & GDK_MOD1_MASK)
	{
		sci_send_command(editor->sci, (event->direction == GDK_SCROLL_DOWN) ? SCI_PAGEDOWN : SCI_PAGEUP);
		return TRUE;
	}
	else if (event->state & GDK_SHIFT_MASK)
	{
		gint amount = (event->direction == GDK_SCROLL_DOWN) ? 8 : -8;

		sci_scroll_columns(editor->sci, amount);
		return TRUE;
	}

	return FALSE; /* let Scintilla handle all other cases */
}


static gboolean editor_check_colourise(GeanyEditor *editor)
{
	GeanyDocument *doc = editor->document;

	if (!doc->priv->colourise_needed)
		return FALSE;

	doc->priv->colourise_needed = FALSE;
	sci_colourise(editor->sci, 0, -1);

	/* now that the current document is colourised, fold points are now accurate,
	 * so force an update of the current function/tag. */
	symbols_get_current_function(NULL, NULL);
	ui_update_statusbar(NULL, -1);

	return TRUE;
}


/* We only want to colourise just before drawing, to save startup time and
 * prevent unnecessary recolouring other documents after one is saved.
 * Really we want a "draw" signal but there doesn't seem to be one (expose is too late,
 * and "show" doesn't work). */
static gboolean on_editor_focus_in(GtkWidget *widget, GdkEventFocus *event, gpointer user_data)
{
	GeanyEditor *editor = user_data;

	editor_check_colourise(editor);
	return FALSE;
}


/* This is just to catch any uncolourised documents being drawn that didn't receive focus
 * for some reason, maybe it's not necessary but just in case. */
static gboolean on_editor_expose_event(GtkWidget *widget, GdkEventExpose *event,
		gpointer user_data)
{
	GeanyEditor *editor = user_data;

	editor_check_colourise(editor);
	return FALSE;
}


static void setup_sci_keys(ScintillaObject *sci)
{
	/* disable some Scintilla keybindings to be able to redefine them cleanly */
	sci_clear_cmdkey(sci, 'A' | (SCMOD_CTRL << 16)); /* select all */
	sci_clear_cmdkey(sci, 'D' | (SCMOD_CTRL << 16)); /* duplicate */
	sci_clear_cmdkey(sci, 'T' | (SCMOD_CTRL << 16)); /* line transpose */
	sci_clear_cmdkey(sci, 'T' | (SCMOD_CTRL << 16) | (SCMOD_SHIFT << 16)); /* line copy */
	sci_clear_cmdkey(sci, 'L' | (SCMOD_CTRL << 16)); /* line cut */
	sci_clear_cmdkey(sci, 'L' | (SCMOD_CTRL << 16) | (SCMOD_SHIFT << 16)); /* line delete */
	sci_clear_cmdkey(sci, SCK_DELETE | (SCMOD_CTRL << 16) | (SCMOD_SHIFT << 16)); /* line to end delete */
	sci_clear_cmdkey(sci, '/' | (SCMOD_CTRL << 16)); /* Previous word part */
	sci_clear_cmdkey(sci, '\\' | (SCMOD_CTRL << 16)); /* Next word part */
	sci_clear_cmdkey(sci, SCK_UP | (SCMOD_CTRL << 16)); /* scroll line up */
	sci_clear_cmdkey(sci, SCK_DOWN | (SCMOD_CTRL << 16)); /* scroll line down */
	sci_clear_cmdkey(sci, SCK_HOME); /* line start */
	sci_clear_cmdkey(sci, SCK_END);	/* line end */
	sci_clear_cmdkey(sci, SCK_END | (SCMOD_ALT << 16));	/* visual line end */

	if (editor_prefs.use_gtk_word_boundaries)
	{
		/* use GtkEntry-like word boundaries */
		sci_assign_cmdkey(sci, SCK_RIGHT | (SCMOD_CTRL << 16), SCI_WORDRIGHTEND);
		sci_assign_cmdkey(sci, SCK_RIGHT | (SCMOD_CTRL << 16) | (SCMOD_SHIFT << 16), SCI_WORDRIGHTENDEXTEND);
		sci_assign_cmdkey(sci, SCK_DELETE | (SCMOD_CTRL << 16), SCI_DELWORDRIGHTEND);
	}
	sci_assign_cmdkey(sci, SCK_UP | (SCMOD_ALT << 16), SCI_LINESCROLLUP);
	sci_assign_cmdkey(sci, SCK_DOWN | (SCMOD_ALT << 16), SCI_LINESCROLLDOWN);
	sci_assign_cmdkey(sci, SCK_UP | (SCMOD_CTRL << 16), SCI_PARAUP);
	sci_assign_cmdkey(sci, SCK_UP | (SCMOD_CTRL << 16) | (SCMOD_SHIFT << 16), SCI_PARAUPEXTEND);
	sci_assign_cmdkey(sci, SCK_DOWN | (SCMOD_CTRL << 16), SCI_PARADOWN);
	sci_assign_cmdkey(sci, SCK_DOWN | (SCMOD_CTRL << 16) | (SCMOD_SHIFT << 16), SCI_PARADOWNEXTEND);

	sci_clear_cmdkey(sci, SCK_BACK | (SCMOD_ALT << 16)); /* clear Alt-Backspace (Undo) */
}


#include "icons/16x16/classviewer-var.xpm"
#include "icons/16x16/classviewer-method.xpm"

/* Create new editor widget (scintilla).
 * @note The @c "sci-notify" signal is connected separately. */
static ScintillaObject *create_new_sci(GeanyEditor *editor)
{
	ScintillaObject	*sci;

	sci = SCINTILLA(scintilla_new());

	gtk_widget_show(GTK_WIDGET(sci));

	sci_set_codepage(sci, SC_CP_UTF8);
	/*SSM(sci, SCI_SETWRAPSTARTINDENT, 4, 0);*/
	/* disable scintilla provided popup menu */
	sci_use_popup(sci, FALSE);

	setup_sci_keys(sci);

	sci_set_symbol_margin(sci, editor_prefs.show_markers_margin);
	sci_set_lines_wrapped(sci, editor_prefs.line_wrapping);
	sci_set_scrollbar_mode(sci, editor_prefs.show_scrollbars);
	sci_set_caret_policy_x(sci, CARET_JUMPS | CARET_EVEN, 0);
	/*sci_set_caret_policy_y(sci, CARET_JUMPS | CARET_EVEN, 0);*/
	SSM(sci, SCI_AUTOCSETSEPARATOR, '\n', 0);
	SSM(sci, SCI_SETSCROLLWIDTHTRACKING, 1, 0);

	/* tag autocompletion images */
	SSM(sci, SCI_REGISTERIMAGE, 1, (sptr_t)classviewer_var);
	SSM(sci, SCI_REGISTERIMAGE, 2, (sptr_t)classviewer_method);

	/* necessary for column mode editing, implemented in Scintilla since 2.0 */
	SSM(sci, SCI_SETADDITIONALSELECTIONTYPING, 1, 0);

	/* virtual space */
	SSM(sci, SCI_SETVIRTUALSPACEOPTIONS, editor_prefs.show_virtual_space, 0);

	/* only connect signals if this is for the document notebook, not split window */
	if (editor->sci == NULL)
	{
		g_signal_connect(sci, "button-press-event", G_CALLBACK(on_editor_button_press_event), editor);
		g_signal_connect(sci, "scroll-event", G_CALLBACK(on_editor_scroll_event), editor);
		g_signal_connect(sci, "motion-notify-event", G_CALLBACK(on_motion_event), NULL);
		g_signal_connect(sci, "focus-in-event", G_CALLBACK(on_editor_focus_in), editor);
		g_signal_connect(sci, "expose-event", G_CALLBACK(on_editor_expose_event), editor);
	}
	return sci;
}


/** Creates a new Scintilla @c GtkWidget based on the settings for @a editor.
 * @param editor Editor settings.
 * @return The new widget.
 *
 * @since 0.15
 **/
ScintillaObject *editor_create_widget(GeanyEditor *editor)
{
	const GeanyIndentPrefs *iprefs = get_default_indent_prefs();
	ScintillaObject *old, *sci;

	/* temporarily change editor to use the new sci widget */
	old = editor->sci;
	sci = create_new_sci(editor);
	editor->sci = sci;

	editor_set_indent_type(editor, iprefs->type);
	editor_set_font(editor, interface_prefs.editor_font);
	editor_apply_update_prefs(editor);

	/* if editor already had a widget, restore it */
	if (old)
		editor->sci = old;
	return sci;
}


GeanyEditor *editor_create(GeanyDocument *doc)
{
	const GeanyIndentPrefs *iprefs = get_default_indent_prefs();
	GeanyEditor *editor = g_new0(GeanyEditor, 1);

	editor->document = doc;
	doc->editor = editor;	/* needed in case some editor functions/callbacks expect it */

	editor->auto_indent = (iprefs->auto_indent_mode != GEANY_AUTOINDENT_NONE);
	editor->line_wrapping = editor_prefs.line_wrapping;
	editor->scroll_percent = -1.0F;
	editor->line_breaking = FALSE;

	editor->sci = editor_create_widget(editor);
	return editor;
}


/* in case we need to free some fields in future */
void editor_destroy(GeanyEditor *editor)
{
	g_free(editor);
}


static void on_document_save(GObject *obj, GeanyDocument *doc)
{
	g_return_if_fail(NZV(doc->real_path));

	if (utils_str_equal(doc->real_path,
		utils_build_path(app->configdir, "snippets.conf", NULL)))
	{
		/* reload snippets */
		editor_snippets_free();
		editor_snippets_init();
	}
}


gboolean editor_complete_word_part(GeanyEditor *editor)
{
	gchar *entry;

	g_return_val_if_fail(editor, FALSE);

	if (!SSM(editor->sci, SCI_AUTOCACTIVE, 0, 0))
		return FALSE;

	entry = sci_get_string(editor->sci, SCI_AUTOCGETCURRENTTEXT, 0);

	/* if no word part, complete normally */
	if (!check_partial_completion(editor, entry))
		SSM(editor->sci, SCI_AUTOCCOMPLETE, 0, 0);

	g_free(entry);
	return TRUE;
}


void editor_init(void)
{
	static GeanyIndentPrefs indent_prefs;

	memset(&editor_prefs, 0, sizeof(GeanyEditorPrefs));
	memset(&indent_prefs, 0, sizeof(GeanyIndentPrefs));
	editor_prefs.indentation = &indent_prefs;

	/* use g_signal_connect_after() to allow plugins connecting to the signal before the default
	 * handler (on_editor_notify) is called */
	g_signal_connect_after(geany_object, "editor-notify", G_CALLBACK(on_editor_notify), NULL);

	ui_add_config_file_menu_item(utils_build_path(app->configdir, "snippets.conf", NULL),
		NULL, NULL);
	g_signal_connect(geany_object, "document-save", G_CALLBACK(on_document_save), NULL);
}


/* TODO: Should these be user-defined instead of hard-coded? */
void editor_set_indentation_guides(GeanyEditor *editor)
{
	gint mode;
	gint lexer;

	g_return_if_fail(editor != NULL);

	if (! editor_prefs.show_indent_guide)
	{
		sci_set_indentation_guides(editor->sci, SC_IV_NONE);
		return;
	}

	lexer = sci_get_lexer(editor->sci);
	switch (lexer)
	{
		/* Lines added/removed are prefixed with +/- characters, so
		 * those lines will not be shown with any indentation guides.
		 * It can be distracting that only a few of lines in a diff/patch
		 * file will show the guides. */
		case SCLEX_DIFF:
			mode = SC_IV_NONE;
			break;

		/* These languages use indentation for control blocks; the "look forward" method works
		 * best here */
		case SCLEX_PYTHON:
		case SCLEX_HASKELL:
		case SCLEX_MAKEFILE:
		case SCLEX_ASM:
		case SCLEX_SQL:
		case SCLEX_PROPERTIES:
		case SCLEX_FORTRAN: /* Is this the best option for Fortran? */
		case SCLEX_CAML:
			mode = SC_IV_LOOKFORWARD;
			break;

		/* C-like (structured) languages benefit from the "look both" method */
		case SCLEX_CPP:
		case SCLEX_HTML:
		case SCLEX_XML:
		case SCLEX_PERL:
		case SCLEX_LATEX:
		case SCLEX_LUA:
		case SCLEX_PASCAL:
		case SCLEX_RUBY:
		case SCLEX_TCL:
		case SCLEX_F77:
		case SCLEX_CSS:
		case SCLEX_BASH:
		case SCLEX_VHDL:
		case SCLEX_FREEBASIC:
		case SCLEX_D:
		case SCLEX_MATLAB:
			mode = SC_IV_LOOKBOTH;
			break;

		default:
			mode = SC_IV_REAL;
			break;
	}

	sci_set_indentation_guides(editor->sci, mode);
}


/* Apply just the prefs that can change in the Preferences dialog */
void editor_apply_update_prefs(GeanyEditor *editor)
{
	ScintillaObject *sci;

	g_return_if_fail(editor != NULL);

	sci = editor->sci;

	sci_set_mark_long_lines(sci, editor_get_long_line_type(),
		editor_get_long_line_column(), editor_prefs.long_line_color);

	/* update indent width, tab width */
	editor_set_indent_type(editor, editor->indent_type);
	sci_set_tab_indents(sci, editor_prefs.use_tab_to_indent);

	sci_set_autoc_max_height(sci, editor_prefs.symbolcompletion_max_height);
	SSM(sci, SCI_AUTOCSETDROPRESTOFWORD, editor_prefs.completion_drops_rest_of_word, 0);

	editor_set_indentation_guides(editor);

	sci_set_visible_white_spaces(sci, editor_prefs.show_white_space);
	sci_set_visible_eols(sci, editor_prefs.show_line_endings);
	sci_set_symbol_margin(sci, editor_prefs.show_markers_margin);
	sci_set_line_numbers(sci, editor_prefs.show_linenumber_margin, 0);

	sci_set_folding_margin_visible(sci, editor_prefs.folding);

	/* virtual space */
	SSM(sci, SCI_SETVIRTUALSPACEOPTIONS, editor_prefs.show_virtual_space, 0);

	/* (dis)allow scrolling past end of document */
	sci_set_scroll_stop_at_last_line(sci, editor_prefs.scroll_stop_at_last_line);
}


/* This is for tab-indents, space aligns formatted code. Spaces should be preserved. */
static void change_tab_indentation(GeanyEditor *editor, gint line, gboolean increase)
{
	ScintillaObject *sci = editor->sci;
	gint pos = sci_get_position_from_line(sci, line);

	if (increase)
	{
		sci_insert_text(sci, pos, "\t");
	}
	else
	{
		if (sci_get_char_at(sci, pos) == '\t')
		{
			sci_set_selection(sci, pos, pos + 1);
			sci_replace_sel(sci, "");
		}
		else /* remove spaces only if no tabs */
		{
			gint width = sci_get_line_indentation(sci, line);

			width -= editor_get_indent_prefs(editor)->width;
			sci_set_line_indentation(sci, line, width);
		}
	}
}


static void editor_change_line_indent(GeanyEditor *editor, gint line, gboolean increase)
{
	const GeanyIndentPrefs *iprefs = editor_get_indent_prefs(editor);
	ScintillaObject *sci = editor->sci;

	if (iprefs->type == GEANY_INDENT_TYPE_TABS)
		change_tab_indentation(editor, line, increase);
	else
	{
		gint width = sci_get_line_indentation(sci, line);

		width += increase ? iprefs->width : -iprefs->width;
		sci_set_line_indentation(sci, line, width);
	}
}


void editor_indent(GeanyEditor *editor, gboolean increase)
{
	ScintillaObject *sci = editor->sci;
	gint start, end;
	gint line, lstart, lend;

	if (sci_get_lines_selected(sci) <= 1)
	{
		line = sci_get_current_line(sci);
		editor_change_line_indent(editor, line, increase);
		return;
	}
	editor_select_lines(editor, FALSE);
	start = sci_get_selection_start(sci);
	end = sci_get_selection_end(sci);
	lstart = sci_get_line_from_position(sci, start);
	lend = sci_get_line_from_position(sci, end);
	if (end == sci_get_length(sci))
		lend++;	/* for last line with text on it */

	sci_start_undo_action(sci);
	for (line = lstart; line < lend; line++)
	{
		editor_change_line_indent(editor, line, increase);
	}
	sci_end_undo_action(sci);

	/* set cursor/selection */
	if (lend > lstart)
	{
		sci_set_selection_start(sci, start);
		end = sci_get_position_from_line(sci, lend);
		sci_set_selection_end(sci, end);
		editor_select_lines(editor, FALSE);
	}
	else
	{
		sci_set_current_line(sci, lstart);
	}
}
