/* $Id$ */

/*
 *  (C) Copyright 2002 Wojtek Kaniewski <wojtekka@irc.pl>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License Version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * 21:56 <@Kooba> MASZ OKIENKA ALA IRSSI?
 * 21:56 <@Kooba> no to chlopie.
 * 21:56 <@Kooba> wywalam gaima.
 * 21:56 <@Kooba> bo taki ekg obsluguje dzwiek.
 * 21:56 <@Kooba> dcc
 * 21:56 <@bkU> JA CIE!
 * 21:56 <@Kooba> i lepiej wyszukuje.
 * 21:56 <@Kooba> i lepiej sie podrywa LASKI!
 *
 * 21:59 <@bkU> elluin, mo�esz napisa�, tak jak w �a�cuszkach szcz�cia
 * 21:59 <@bkU> Hosse Pedro Alvaros z po�udniowej Gwatemali
 * 21:59 <@bkU> przesta� u�ywa� ekg
 * 21:59 <@bkU> i w ci�gu 7 dni straci� wszystko
 *
 * 22:02 <@bkU> ekg mo�e wszystko!
 * 22:02 <@bkU> ostatnio s�ysza�em przecieki, �e w NASA u�ywaj� w�a�nie ekg.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ncurses.h>
#include <signal.h>
#ifndef _AIX
#  include <string.h>
#endif
#include <ctype.h>
#include "config.h"
#include "stuff.h"
#include "commands.h"
#include "userlist.h"
#include "xmalloc.h"
#include "themes.h"
#include "vars.h"
#include "ui.h"
#include "version.h"

static void ui_ncurses_loop();
static void ui_ncurses_print(const char *target, int separate, const char *line);
static void ui_ncurses_beep();
static int ui_ncurses_event(const char *event, ...);
static void ui_ncurses_deinit();

static void window_switch(int id);
static void update_statusbar();

struct window {
	WINDOW *window;		/* okno okna */
	char *target;		/* nick query albo inna nazwa albo NULL */
	int lines;		/* ilo�� linii okna */
	int y;			/* aktualna pozycja */
	int start;		/* od kt�rej linii zaczyna si� wy�wietlanie */
	int id;			/* numer okna */
	int act;		/* czy co� si� zmieni�o? */
	int more;		/* pojawi�o si� co� poza ekranem */
	char *prompt;		/* sformatowany prompt lub NULL */
	int prompt_len;		/* d�ugo�� prompta lub 0 */
};

static WINDOW *status = NULL;		/* okno stanu */
static WINDOW *input = NULL;		/* okno wpisywania tekstu */

#define HISTORY_MAX 1000		/* maksymalna ilo�� wpis�w historii */
static char *history[HISTORY_MAX];	/* zapami�tane linie */
static int history_index = 0;		/* offset w historii */

#define LINE_MAXLEN 1000		/* rozmiar linii */
static char *line = NULL;		/* wska�nik aktualnej linii */
static char *yanked = NULL;		/* bufor z ostatnio wyci�tym tekstem */
static char **lines = NULL;		/* linie wpisywania wielolinijkowego */
static int line_start = 0;		/* od kt�rego znaku wy�wietlamy? */
static int line_index = 0;		/* na kt�rym znaku jest kursor? */
static int lines_start = 0;		/* od kt�rej linii wy�wietlamy? */
static int lines_index = 0;		/* w kt�rej linii jeste�my? */
static char **completions = NULL;	/* lista dope�nie� */
static list_t windows = NULL;		/* lista okien */
static struct window *window_current;	/* wska�nik na aktualne okno */
static int input_size = 1;		/* rozmiar okna wpisywania tekstu */

/* rozmiar okna wy�wietlaj�cego tekst */
#define output_size (stdscr->_maxy - input_size)

/*
 * ui_debug()
 *
 * wy�wietla szybko sformatowany tekst w aktualnym oknie. do debugowania.
 */
#define ui_debug(x...) { \
	char *ui_debug_tmp = saprintf(x); \
	ui_ncurses_print(NULL, 0, ui_debug_tmp); \
	xfree(ui_debug_tmp); \
}

/*
 * set_cursor()
 *
 * ustawia kursor na pocz�ktu kolejnej linii okna.
 */
static void set_cursor(struct window *w)
{
	if (w->y == w->lines) {
		if (w->start == w->lines - output_size)
			w->start++;
		wresize(w->window, w->y + 1, w->window->_maxx + 1);
		w->lines++;
	}
	wmove(w->window, w->y, 0);
}

/*
 * window_find()
 *
 * szuka okna o podanym celu. zwraca struktur� opisuj�c� je.
 */
static struct window *window_find(const char *target)
{
	list_t l;

	if (!target || !strcasecmp(target, "__current"))
		return window_current;

	if (!strcasecmp(target, "__status"))
		return windows->data;
	
	for (l = windows; l; l = l->next) {
		struct window *w = l->data;

		if (w->target && !strcasecmp(target, w->target))
			return w;
	}

	return NULL;
}

/*
 * window_refresh()
 *
 * ncursesowo ustawia do wy�wietlenia aktualnie wybrane okienko, a reszt�
 * ucina, �eby si� schowa�y.
 */
static void window_refresh()
{
	list_t l;

	for (l = windows; l; l = l->next) {
		struct window *w = l->data;
		
		if (window_current->id == w->id)
			pnoutrefresh(w->window, w->start, 0, 0, 0, output_size - 1, stdscr->_maxx + 1);
		else
			pnoutrefresh(w->window, 0, 0, 0, 81, 0, 0);
	}
	
	mvwin(status, stdscr->_maxy - input_size, 0);
	wresize(input, input_size, input->_maxx + 1);
	mvwin(input, stdscr->_maxy - input_size + 1, 0);
}

/*
 * window_switch()
 *
 * prze��cza do podanego okna.
 */
static void window_switch(int id)
{
	list_t l;

	for (l = windows; l; l = l->next) {
		struct window *w = l->data;

		if (id == w->id) {
			window_current = w;

			w->act = 0;

			window_refresh();
			wnoutrefresh(status);
			wnoutrefresh(input);
			doupdate();
			update_statusbar();

			return;
		}
	}
}

/*
 * window_new_compare()
 *
 * do sortowania okienek.
 */
static int window_new_compare(void *data1, void *data2)
{
	struct window *a = data1, *b = data2;

	if (!a || !b)
		return 0;

	return a->id - b->id;
}

/*
 * window_new()
 *
 * tworzy nowe okno o podanej nazwie.
 */
static struct window *window_new(const char *target)
{
	struct window w;
	list_t l;
	int id = 1, done = 0;

	while (!done) {
		done = 1;

		for (l = windows; l; l = l->next) {
			struct window *w = l->data;

			if (w->id == id) {
				done = 0;
				id++;
				break;
			}
		}
	}
	
	memset(&w, 0, sizeof(w));

	w.id = id;
	w.target = xstrdup(target);
	if (target) {
		w.prompt = format_string(format_find("ncurses_prompt_query"), target);
		w.prompt_len = strlen(w.prompt);
	} else {
		const char *f = format_find("ncurses_prompt_none");

		if (strcmp(f, "")) {
			w.prompt = xstrdup(f);
			w.prompt_len = strlen(w.prompt);
		}
	}
	w.lines = stdscr->_maxy - 1;
	w.window = newpad(w.lines, stdscr->_maxx + 1);

	return list_add_sorted(&windows, &w, sizeof(w), window_new_compare);
}

/*
 * print_timestamp()
 *
 * wy�wietla timestamp na pocz�tku linii, je�li trzeba.
 */
static int print_timestamp(struct window *w)
{
	struct tm *tm;
	char buf[80];
	time_t t;
	int i, x = 0, attr, pair;

	if (!config_timestamp) {
		set_cursor(w);
		return 0;
	}

	t = time(NULL);
	tm = localtime(&t);
	strftime(buf, sizeof(buf), config_timestamp, tm);

	wattr_get(w->window, &attr, &pair, NULL);

	wattrset(w->window, A_NORMAL);

	set_cursor(w);

	for (i = 0; i < strlen(buf); i++) {
		waddch(w->window, buf[i]);
		x++;
	}

	wattrset(w->window, COLOR_PAIR(pair) | attr);

	return x;
}

/*
 * ui_ncurses_print()
 *
 * wy�wietla w podanym okienku, co trzeba.
 */
static void ui_ncurses_print(const char *target, int separate, const char *line)
{
	struct window *w;
	const char *p;
	int x = 0, count = 0;
	list_t l;

	switch (config_make_window) {
		case 1:
			if ((w = window_find(target)))
				break;

			if (!separate)
				w = windows->data;

			for (l = windows; l; l = l->next) {
				struct window *w = l->data;

				if (!w->target && w->id != 1) {
					w->target = xstrdup(target);
					xfree(w->prompt);
					w->prompt = format_string(format_find("ncurses_prompt_query"), target);
					w->prompt_len = strlen(w->prompt);
					print("window_id_query_started", itoa(w->id), target);
					break;
				}
			}

		case 2:
			if (!(w = window_find(target))) {
				if (!separate)
					w = windows->data;
				else {
					w = window_new(target);
					print("window_id_query_started", itoa(w->id), target);
				}
			}
			break;

		default:
			/* je�li nie ma okna, rzu� do statusowego. */
			if (!(w = window_find(target)))
				w = windows->data;
	}

	if (w != window_current) {
		w->act = 1;
		update_statusbar();
	}

	for (p = line; *p; p++) {
		if (*p == 27) {
			p++;
			if (*p == '[') {
				char *q;
				int a1, a2 = -1;
				p++;
				a1 = strtol(p, &q, 10);
				p = q;
				if (*p == ';') {
					p++;
					a2 = strtol(p, &q, 10);
					p = q;
				}
				if (a2 == 30)
					a2 += 16;
				if (*p == 'm' && config_display_color) {
					if (a1 == 0 && a2 == -1)
						wattrset(w->window, COLOR_PAIR(7));
					else if (a1 == 1 && a2 == -1)
						wattrset(w->window, COLOR_PAIR(7) | A_BOLD);
					else if (a2 == -1)
						wattrset(w->window, COLOR_PAIR(a1 - 30));
					else
						wattrset(w->window, COLOR_PAIR(a2 - 30) | ((a1) ? A_BOLD : A_NORMAL));
				}
				if (*p == 'm' && !config_display_color)
					wattrset(w->window, (a1 == 1) ? A_BOLD : A_NORMAL);
			} else {
				while (*p && ((*p >= '0' && *p <= '9') || *p == ';'))
					p++;
				p++;
			}
		} else if (*p == 10) {
			if (!*(p + 1))
				break;
			w->y++;
			x = print_timestamp(w);
		} else {
			if (!x)
				x += print_timestamp(w);
			waddch(w->window, (unsigned char) *p);
			count++;
			x++;
			if (x == stdscr->_maxx + 1) {
				if (*(p + 1) == 10)
					p++;
				else
					w->y++;
				set_cursor(w);
				x = 4;
				wmove(w->window, w->y, x);
			}
		}
	}

	if (!count) {
		print_timestamp(w);
		set_cursor(w);
	}

	w->y++;

	if (w->lines - w->start > output_size)
		w->more = 1;
	
	window_refresh();
	update_statusbar();
	wnoutrefresh(input);
	doupdate();
}

/*
 * update_statusbar()
 *
 * uaktualnia pasek stanu i wy�wietla go ponownie.
 */
static void update_statusbar()
{
	const char *p;
	int i, nested = 0;

	wmove(status, 0, 0);
	if (config_display_color)
		wattrset(status, COLOR_PAIR(15));
	else
		wattrset(status, A_REVERSE);

	for (i = 0; i <= status->_maxx; i++)
		waddch(status, ' ');
	
	wmove(status, 0, 0);

	for (p = format_find("statusbar"); *p; p++) {
		if (*p == '}' && nested) {
			nested--;
			continue;
		}

		if (*p != '%') {
			waddch(status, *p);
			continue;
		}
	
		p++;

		if (!*p)
			break;

#define __color(x,y,z) \
	case x: wattrset(status, COLOR_PAIR(8+z)); break; \
	case y: wattrset(status, COLOR_PAIR(8+z) | A_BOLD); break;

		if (*p != '{' && config_display_color) {
			switch (*p) {
				__color('k', 'K', 0);
				__color('r', 'R', 1);
				__color('g', 'G', 2);
				__color('y', 'Y', 3);
				__color('b', 'B', 4);
				__color('m', 'M', 5);
				__color('c', 'C', 6);
				__color('w', 'W', 7);
				case 'n':
					wattrset(status, (config_display_color) ? COLOR_PAIR(15) : A_NORMAL);
					break;
			}
			continue;
		}
#undef __color
		if (*p != '{' && !config_display_color)
			continue;

		p++;
		if (!*p)
			break;

		if (!strncmp(p, "time}", 5)) {
			struct tm *tm;
			time_t t = time(NULL);
			char tmp[16];

			tm = localtime(&t);

			strftime(tmp, sizeof(tmp), "%H:%M", tm);
			waddstr(status, tmp);
			p += 4;
		} else if (!strncmp(p, "time ", 5)) {	/* XXX naprawi� */
			struct tm *tm;
			time_t t = time(NULL);
			char tmp[100], *fmt;
			const char *q;

			tm = localtime(&t);

			p += 5;

			for (q = p; *q && *q != '}'; q++);

			fmt = xmalloc(q - p + 1);
			strncpy(fmt, p, q - p);
			fmt[q - p] = 0;

			strftime(tmp, sizeof(tmp), fmt, tm);

			xfree(fmt);
			p = q - 1;
		} else if (!strncmp(p, "window}", 7)) {
			waddstr(status, itoa(window_current->id));
			p += 6;
		} else if (!strncmp(p, "uin}", 4)) {
			waddstr(status, itoa(config_uin));
			p += 3;
		} else if (!strncmp(p, "nick}", 5)) {
			struct userlist *u = userlist_find(config_uin, NULL);
			if (u && u->display)
				waddstr(status, u->display);
			p += 4;
		} else if (!strncmp(p, "query}", 6)) {
			if (window_current->target)
				waddstr(status, window_current->target);
			p += 5;
		} else if (!strncmp(p, "activity}", 9)) {
			string_t s = string_init("");
			int first = 1;
			list_t l;

			for (l = windows; l; l = l->next) {
				struct window *w = l->data;

				if (w->act) {
					if (!first) 
						string_append_c(s, ',');
					string_append(s, itoa(w->id));
					first = 0;
				}
			}
			
			waddstr(status, s->str);

			string_free(s, 1);

			p += 8;
		} else if (*p == '?') {
			int matched = 0, neg = 0;

			p++;
			if (!*p)
				break;

			if (*p == '!') {
				neg = 1;
				p++;
				if (!*p)
					break;
			}
			
			if (!strncmp(p, "away ", 5)) {
				matched = (away == 1 || away == 3);
				p += 4;
			} else if (!strncmp(p, "avail ", 6)) {
				matched = (away == 0 || away == 4);
				p += 5;
			} else if (!strncmp(p, "invisible ", 10)) {
				matched = (away == 2 || away == 5);
				p += 9;
			} else if (!strncmp(p, "notavail ", 9)) {
				matched = (!sess || sess->state != GG_STATE_CONNECTED);
				p += 8;
			} else if (!strncmp(p, "query ", 6)) {
				matched = (window_current->target != NULL);
				p += 5;
			} else if (!strncmp(p, "activity ", 9)) {
				list_t l;

				for (l = windows; l; l = l->next) {
					struct window *w = l->data;

					if (w->act)
						matched = 1;
				}

				p += 8;
			} else if (!strncmp(p, "more ", 5)) {
				matched = (window_current->more);
				p += 4;
			} else if (!strncmp(p, "nick ", 5)) {
				struct userlist *u = userlist_find(config_uin, NULL);
				
				matched = (u && u->display);
				p += 4;
			}

			if (!matched) {
				while (*p && *p != '}')
					p++;
				if (!*p)
					break;
			} else 
				nested++;
		} else {
			while (*p && *p != '}')
				p++;
			if (!*p)
				break;
		}
	}
	
	wnoutrefresh(status);
	wnoutrefresh(input);
	doupdate();
}

/*
 * ui_ncurses_beep()
 *
 * ostentacyjnie wzywa u�ytkownika do reakcji.
 */
static void ui_ncurses_beep()
{
	beep();
}

/*
 * ui_ncurses_theme_init()
 *
 * inicjuje theme'y tego interfejsu.
 */
static void ui_ncurses_theme_init()
{
	format_add("statusbar", " %c(%w%{time}%c)%w %c(%wuin%c/%{?away %w}%{?avail %W}%{?invisible %K}%{?notavail %k}%{uin}%c) (%wwin%c/%w%{window}%{?query %c:%W}%{query}%c)%w%{?activity  %c(%wact%c/%w}%{activity}%{?activity %c)%w}%{?more  %c(%Gmore%c)}", 1);
	format_add("ncurses_prompt_none", "", 1);
	format_add("ncurses_prompt_query", "[%1] ", 1);
	format_add("no_prompt_cache", "", 1);
	format_add("prompt", "%K:%g:%G:%n", 1);
	format_add("prompt2", "%K:%c:%C:%n", 1);
	format_add("error", "%K:%r:%R:%n", 1);
}

/*
 * ui_ncurses_init()
 *
 * inicjalizuje ca�� zabaw� z ncurses.
 */
void ui_ncurses_init()
{
	struct timer *t;

	ui_print = ui_ncurses_print;
	ui_loop = ui_ncurses_loop;
	ui_beep = ui_ncurses_beep;
	ui_event = ui_ncurses_event;
	ui_deinit = ui_ncurses_deinit;
		
	initscr();
	cbreak();
	noecho();
	nonl();

	ui_screen_width = stdscr->_maxx + 1;
	ui_screen_height = stdscr->_maxy + 1;
	
	window_current = window_new(NULL);
	status = newwin(1, stdscr->_maxx + 1, stdscr->_maxy - 1, 0);
	input = newwin(1, stdscr->_maxx + 1, stdscr->_maxy, 0);
	keypad(input, TRUE);

	start_color();
	init_pair(16, COLOR_BLACK, COLOR_BLACK);
	init_pair(1, COLOR_RED, COLOR_BLACK);
	init_pair(2, COLOR_GREEN, COLOR_BLACK);
	init_pair(3, COLOR_YELLOW, COLOR_BLACK);
	init_pair(4, COLOR_BLUE, COLOR_BLACK);
	init_pair(5, COLOR_MAGENTA, COLOR_BLACK);
	init_pair(6, COLOR_CYAN, COLOR_BLACK);
	init_pair(7, COLOR_WHITE, COLOR_BLACK);

	init_pair(8, COLOR_BLACK, COLOR_BLUE);
	init_pair(9, COLOR_RED, COLOR_BLUE);
	init_pair(10, COLOR_GREEN, COLOR_BLUE);
	init_pair(11, COLOR_YELLOW, COLOR_BLUE);
	init_pair(12, COLOR_BLUE, COLOR_BLUE);
	init_pair(13, COLOR_MAGENTA, COLOR_BLUE);
	init_pair(14, COLOR_CYAN, COLOR_BLUE);
	init_pair(15, COLOR_WHITE, COLOR_BLUE);

	ui_ncurses_theme_init();
	
	wnoutrefresh(status);
	wnoutrefresh(input);
	doupdate();

	signal(SIGINT, SIG_IGN);
	
	memset(history, 0, sizeof(history));

	t = timer_add(1, "ui-ncurses-time", "refresh_time");
	t->ui = 1;
}

/*
 * ui_ncurses_deinit()
 *
 * zamyka, robi porz�dki.
 */
static void ui_ncurses_deinit()
{
	static int done = 0;
	list_t l;
	int i;

	if (done)
		return;

	for (l = windows; l; l = l->next) {
		struct window *w = l->data;

		xfree(w->target);
		xfree(w->prompt);
		delwin(w->window);
	}

	list_destroy(windows, 1);

	werase(input);
	wnoutrefresh(input);
	doupdate();
	delwin(input);
	delwin(status);
	endwin();

	for (i = 0; i < HISTORY_MAX; i++)
		if (history[i] != line)
			xfree(history[i]);

	if (lines) {
		for (i = 0; lines[i]; i++)
			xfree(lines[i]);

		xfree(lines);
	}

	xfree(line);
	xfree(yanked);

	done = 1;
}

/*
 * line_adjust()
 *
 * ustawia kursor w odpowiednim miejscu ekranu po zmianie tekstu w poziomie.
 */
static void line_adjust()
{
	int prompt_len = (lines) ? 0 : window_current->prompt_len;

	line_index = strlen(line);
	if (strlen(line) < input->_maxx - 9 - prompt_len)
		line_start = 0;
	else
		line_start = strlen(line) - strlen(line) % (input->_maxx - 9 - prompt_len);
}

/*
 * lines_adjust()
 *
 * poprawia kursor po przesuwaniu go w pionie.
 */
static void lines_adjust()
{
	if (lines_index < lines_start)
		lines_start = lines_index;

	if (lines_index - 4 > lines_start)
		lines_start = lines_index - 4;

	line = lines[lines_index];

	if (line_index > strlen(line))
		line_index = strlen(line);
}

void dcc_generator(const char *text, int len)
{
	char *words[] = { "close", "get", "send", "show", "voice", NULL };
	int i;

	for (i = 0; words[i]; i++)
		if (!strncasecmp(text, words[i], len))
			array_add(&completions, xstrdup(words[i]));
}

void command_generator(const char *text, int len)
{
	char *slash = "";
	list_t l;

	if (*text == '/') {
		slash = "/";
		text++;
		len--;
	}

	if (window_current->target)
		slash = "/";
			
	for (l = commands; l; l = l->next) {
		struct command *c = l->data;

		if (!strncasecmp(text, c->name, len))
			array_add(&completions, saprintf("%s%s", slash, c->name));
	}
}

void known_uin_generator(const char *text, int len)
{
	list_t l;

	for (l = userlist; l; l = l->next) {
		struct userlist *u = l->data;

		if (u->display && !strncasecmp(text, u->display, len))
			array_add(&completions, xstrdup(u->display));
	}

	for (l = conferences; l; l = l->next) {
		struct conference *c = l->data;

		if (!strncasecmp(text, c->name, len))
			array_add(&completions, xstrdup(c->name));
	}
}

void unknown_uin_generator(const char *text, int len)
{
	int i;

	for (i = 0; i < send_nicks_count; i++) {
		if (isdigit(send_nicks[i][0]) && !strncasecmp(text, send_nicks[i], len))
			array_add(&completions, send_nicks[i]);
	}
}

void variable_generator(const char *text, int len)
{
	list_t l;

	for (l = variables; l; l = l->next) {
		struct variable *v = l->data;

		if (v->type == VAR_FOREIGN)
			continue;

		if (*text == '-') {
			if (!strncasecmp(text + 1, v->name, len - 1))
				array_add(&completions, saprintf("-%s", v->name));
		} else {
			if (!strncasecmp(text, v->name, len))
				array_add(&completions, xstrdup(v->name));
		}
	}
}

void ignored_uin_generator(const char *text, int len)
{
	list_t l;

	for (l = ignored; l; l = l->next) {
		struct ignored *i = l->data;
		struct userlist *u;

		if (!(u = userlist_find(i->uin, NULL))) {
			if (!strncasecmp(text, itoa(i->uin), len))
				array_add(&completions, xstrdup(itoa(i->uin)));
		} else {
			if (u->display && !strncasecmp(text, u->display, len))
				array_add(&completions, xstrdup(u->display));
		}
	}
}

void empty_generator(const char *text, int len)
{

}

void file_generator(const char *text, int len)
{

}

void window_generator(const char *text, int len)
{
	char *words[] = { "new", "kill", "next", "prev", "switch", "clear", "refresh", "list", NULL };
	int i;

	for (i = 0; words[i]; i++)
		if (!strncasecmp(text, words[i], len))
			array_add(&completions, xstrdup(words[i]));
}

static struct {
	char ch;
	void (*generate)(const char *text, int len);
} generators[] = {
	{ 'u', known_uin_generator },
	{ 'U', unknown_uin_generator },
	{ 'c', command_generator },
	{ 's', empty_generator },
	{ 'i', ignored_uin_generator },
	{ 'v', variable_generator },
	{ 'd', dcc_generator },
	{ 'f', file_generator },
	{ 'w', window_generator },
	{ 0, NULL }
};

/*
 * complete()
 *
 * funkcja obs�uguj�ca dope�nianie klawiszem tab.
 */
static void complete(int *line_start, int *line_index)
{
	int i, blanks = 0, count;
	char *p, *start = line, *cmd;

	/* nie obs�ugujemy dope�niania w �rodku tekstu */
	if (*line_index != strlen(line))
		return;

	/* je�li uzbierano ju� co� */
	if (completions) {
		int maxlen = 0, cols, rows;
		char *tmp;

		for (i = 0; completions[i]; i++)
			if (strlen(completions[i]) + 2 > maxlen)
				maxlen = strlen(completions[i]) + 2;

		cols = (stdscr->_maxx - 5) / maxlen;
		if (cols == 0)
			cols = 1;

		rows = array_count(completions) / cols + 1;

		tmp = xmalloc(cols * maxlen + 2);

		for (i = 0; i < rows; i++) {
			int j;

			strcpy(tmp, "");

			for (j = 0; j < cols; j++) {
				int cell = j * rows + i;

				if (cell < array_count(completions)) {
					int k;

					strcat(tmp, completions[cell]); 

					for (k = 0; k < maxlen - strlen(completions[cell]); k++)
						strcat(tmp, " ");
				}
			}

			if (strcmp(tmp, "")) {
				strcat(tmp, "\n");
				ui_ncurses_print(NULL, 0, tmp);
			}
		}

		xfree(tmp);

		return;
	}
	
	/* policz spacje */
	for (p = line; *p; p++) {
		if (*p == ' ')
			blanks++;
	}

	/* nietypowe dope�nienie nick�w przy rozmowach */
	cmd = saprintf("%s%s ", (line[0] == '/') ? "/" : "", (config_tab_command) ? config_tab_command : "chat");

	if (!strcmp(line, "") || (!strncasecmp(line, cmd, strlen(cmd)) && blanks == 2 && send_nicks_count > 0) || !strcasecmp(line, cmd)) {
		if (send_nicks_count)
			snprintf(line, LINE_MAXLEN, (window_current->target && line[0] != '/') ? "/%s%s " : "%s%s ", cmd, send_nicks[send_nicks_index++]);
		else
			snprintf(line, LINE_MAXLEN, (window_current->target && line[0] != '/') ? "/%s" : "%s", cmd);
		*line_start = 0;
		*line_index = strlen(line);
		if (send_nicks_index >= send_nicks_count)
			send_nicks_index = 0;

		xfree(cmd);
		array_free(completions);
		completions = NULL;

		return;
	}

	xfree(cmd);

	/* pocz�tek komendy? */
	if (!blanks)
		command_generator(line, strlen(line));
	else {
		char *params = NULL;
		int abbrs = 0, word = 0;
		list_t l;

		start = line + strlen(line);

		while (start > line && *(start - 1) != ' ')
			start--;

		for (p = line + 1; *p; p++)
			if (isspace(*p) && !isspace(*(p - 1)))
				word++;
		word--;

		for (l = commands; l; l = l->next) {
			struct command *c = l->data;
			int len = strlen(c->name);
			char *cmd = (line[0] == '/') ? line + 1 : line;

			if (!strncasecmp(cmd, c->name, len) && isspace(cmd[len])) {
				params = c->params;
				abbrs = 1;
				break;
			}

			for (len = 0; cmd[len] && cmd[len] != ' '; len++);

			if (!strncasecmp(cmd, c->name, len)) {
				params = c->params;
				abbrs++;
			} else
				if (params && abbrs == 1)
					break;
		}

		if (params && abbrs == 1 && word < strlen(params)) {
			for (i = 0; generators[i].ch; i++)
				if (generators[i].ch == params[word]) {
					generators[i].generate(start, strlen(start));
					break;
				}
		}
	}

	count = array_count(completions);

	if (count == 1) {
		snprintf(start, LINE_MAXLEN - (start - line), "%s ", completions[0]);
		*line_index = strlen(line);
		array_free(completions);
		completions = NULL;
	}

	if (count > 1) {
		int common = 0, minlen = strlen(completions[0]);

		for (i = 1; i < count; i++) {
			if (strlen(completions[i]) < minlen)
				minlen = strlen(completions[i]);
		}

		for (i = 0; i < minlen; i++, common++) {
			char c = completions[0][i];
			int j, out = 0;

			for (j = 1; j < count; j++) {
				if (completions[j][i] != c) {
					out = 1;
					break;
				}
			}
			
			if (out)
				break;
		}

		if (common > strlen(start) && start - line + common < LINE_MAXLEN) {
			snprintf(start, common + 1, "%s", completions[0]);
			*line_index = strlen(line);
		}
	}

	return;
}

/*
 * update_input()
 *
 * uaktualnia zmian� rozmiaru pola wpisywania tekstu -- przesuwa okienka
 * itd. je�li zmieniono na pojedyncze, czy�ci dane wej�ciowe.
 */
static void update_input()
{
	list_t l;

	if (input_size == 1) {
		int i;
		
		for (i = 0; lines[i]; i++)
			xfree(lines[i]);
		xfree(lines);
		lines = NULL;

		line = xmalloc(LINE_MAXLEN);
		strcpy(line, "");

		history[0] = line;

		line_start = 0;
		line_index = 0; 
		lines_start = 0;
		lines_index = 0;
	} else {
		lines = xmalloc(2 * sizeof(char*));
		lines[0] = xmalloc(LINE_MAXLEN);
		lines[1] = NULL;
		strcpy(lines[0], line);
		xfree(line);
		line = lines[0];
		lines_start = 0;
		lines_index = 0;
	}
	
	/* przesu�/ods�o� okienka */
	for (l = windows; l; l = l->next) {
		struct window *w = l->data;

		if (input_size == 5 && w->lines - w->start == output_size + 4)
			w->start += 4;

		if (input_size == 1 && w->lines - w->start == output_size - 4)
			w->start -= 4;
	}


	window_refresh();
	wnoutrefresh(status);
	wnoutrefresh(input);
	doupdate();
}
	
/*
 * ui_ncurses_loop()
 *
 * g��wna p�tla interfejsu.
 */
static void ui_ncurses_loop()
{
	line = xmalloc(LINE_MAXLEN);
	strcpy(line, "");

	history[0] = line;

	for (;;) {
		int ch;

		ekg_wait_for_key();
		switch ((ch = wgetch(input))) {
			case KEY_RESIZE:  /* zmiana rozmiaru terminala */
				window_refresh();
				wnoutrefresh(status);
				wnoutrefresh(input);
				doupdate();
				wrefresh(curscr);
				break;

			case 27:
				ch = wgetch(input);

				/* obs�uga Ctrl-F1 - Ctrl-F12 na FreeBSD */
				if (ch == '[') {
					ch = wgetch(input);
					if (ch >= 107 && ch <= 118)
						window_switch(ch - 106);
					break;
				}
				
				if (ch >= '1' && ch <= '9')	/* Alt-cyfra */
					window_switch(ch - '1' + 1);
				else if (ch == '0')
					window_switch(10);

				if (ch == 'k' || ch == 'K')	/* Alt-K */
					ui_event("command", "window", "kill", NULL);

				if (ch == 'n' || ch == 'N')	/* Alt-N */
					ui_event("command", "window", "new");

				if (ch == 13) {		/* Ctrl-Enter */
					if (input_size == 1)
						input_size = 5;
					else {
						string_t s = string_init("");
						int i;
						
						for (i = 0; lines[i]; i++) {
							if (!strcmp(lines[i], "") && !lines[i + 1])
								break;

							string_append(s, lines[i]);
							string_append(s, "\r\n");
						}

						command_exec(window_current->target, s->str);

						string_free(s, 1);

						input_size = 1;
					}
					
					update_input();
				}

				if (ch == 27 && input_size == 5) {  /* Esc */
					input_size = 1;

					update_input();
				}

				break;
				
			case KEY_BACKSPACE:
			case 8:
			case 127:
				if (lines && line_index == 0 && lines_index > 0 && strlen(lines[lines_index]) + strlen(lines[lines_index - 1]) < LINE_MAXLEN) {
					int i;

					line_index = strlen(lines[lines_index - 1]);
					strcat(lines[lines_index - 1], lines[lines_index]);
					
					xfree(lines[lines_index]);

					for (i = lines_index; i < array_count(lines); i++)
						lines[i] = lines[i + 1];

					lines = xrealloc(lines, (array_count(lines) + 1) * sizeof(char*));

					lines_index--;
					lines_adjust();

					break;
				}

				if (strlen(line) > 0 && line_index > 0) {
					memmove(line + line_index - 1, line + line_index, LINE_MAXLEN - line_index);
					line[LINE_MAXLEN - 1] = 0;
					line_index--;
				}
				break;

			case 'Y' - 64:
				if (yanked && strlen(yanked) + strlen(line) + 1 < LINE_MAXLEN) {
					memmove(line + line_index + strlen(yanked), line + line_index, LINE_MAXLEN - line_index - strlen(yanked));
					memcpy(line + line_index, yanked, strlen(yanked));
					line_index += strlen(yanked);
				}
				break;

			case KEY_DC:	/* Ctrl-D, Delete */
			case 'D' - 64:
				if (line_index == strlen(line) && lines_index < array_count(lines) - 1 && strlen(line) + strlen(lines[lines_index + 1]) < LINE_MAXLEN) {
					int i;

					strcat(line, lines[lines_index + 1]);

					xfree(lines[lines_index + 1]);

					for (i = lines_index + 1; i < array_count(lines); i++)
						lines[i] = lines[i + 1];

					lines = xrealloc(lines, (array_count(lines) + 1) * sizeof(char*));

					lines_adjust();
					
					break;
				}
				
				if (line_index < strlen(line)) {
					memmove(line + line_index, line + line_index + 1, LINE_MAXLEN - line_index - 1);
					line[LINE_MAXLEN - 1] = 0;
				}
				break;	
				
			case KEY_ENTER:	/* Enter */
			case 13:
				if (lines) {
					int i;

					lines = xrealloc(lines, (array_count(lines) + 2) * sizeof(char*));

					for (i = array_count(lines); i > lines_index; i--)
						lines[i + 1] = lines[i];

					lines[lines_index + 1] = xmalloc(LINE_MAXLEN);
					strcpy(lines[lines_index + 1], line + line_index);
					line[line_index] = 0;
					
					line_index = 0;
					lines_index++;

					lines_adjust();
					
					break;
				}
				
				command_exec(window_current->target, line);

				if (strcmp(line, "")) {
					if (history[0] != line)
						xfree(history[0]);
					history[0] = xstrdup(line);
					xfree(history[HISTORY_MAX - 1]);
					memmove(&history[1], &history[0], sizeof(history) - sizeof(history[0]));
				}
				history[0] = line;
				history_index = 0;
				line[0] = 0;
				line_adjust();
				break;	

			case 'U' - 64:	/* Ctrl-U */
				xfree(yanked);
				yanked = strdup(line);
				line[0] = 0;
				line_adjust();

				if (lines && lines_index < array_count(lines) - 1) {
					int i;

					xfree(lines[lines_index]);

					for (i = lines_index; i < array_count(lines); i++)
						lines[i] = lines[i + 1];

					lines = xrealloc(lines, (array_count(lines) + 1) * sizeof(char*));

					lines_adjust();
				}

				break;

			case 'W' - 64:	/* Ctrl-W */
			{
				char *p;
				int eaten = 0;

				p = line + line_index;
				while (isspace(*(p-1)) && p > line) {
					p--;
					eaten++;
				}
				if (p > line) {
					while (!isspace(*(p-1)) && p > line) {
						p--;
						eaten++;
					}
				}
				memmove(p, line + line_index, strlen(line) - line_index + 1);
				line_index -= eaten;

				break;
			}

			case 'L' - 64:	/* Ctrl-L */
				ui_event("command", "window", "refresh");
				break;
				
			case 9:		/* Tab */
				if (!lines)
					complete(&line_start, &line_index);

				/* XXX tab w wielolinijkowym */

				break;
				
			case KEY_LEFT:	/* <-- */
				if (lines) {
					if (line_index > 0)
						line_index--;
					else {
						if (lines_index > 0)
							lines_index--;
						lines_adjust();
					}

					break;
				}

				if (line_index > 0)
					line_index--;
				break;
				
			case KEY_RIGHT:	/* --> */
				if (lines) {
					if (line_index < strlen(line))
						line_index++;
					else {
						if (lines_index < array_count(lines) - 1)
							lines_index++;
						lines_adjust();
					}

					break;
				}

				if (line_index < strlen(line))
					line_index++;
				break;
				
			case 'E' - 64:	/* Ctrl-E, End */
			case KEY_END:
			case KEY_SELECT:
				line_adjust();
				break;
				
			case 'A' - 64:	/* Ctrl-A, Home */
			case KEY_HOME:
			case KEY_FIND:
				line_index = 0;
				line_start = 0;
				break;
				
			case KEY_UP:	/* /\ */
				if (lines) {
					if (lines_index - lines_start == 0)
						if (lines_start)
							lines_start--;

					if (lines_index)
						lines_index--;

					lines_adjust();

					break;
				}
				
				if (history[history_index + 1]) {
					if (history_index == 0)
						history[0] = xstrdup(line);
					history_index++;
					strcpy(line, history[history_index]);
					line_adjust();
				}
				break;
				
			case KEY_DOWN:	/* \/ */
				if (lines) {
					if (lines_index - line_start == 4)
						if (lines_index < array_count(lines) - 1)
							lines_start++;

					if (lines_index < array_count(lines) - 1)
						lines_index++;

					lines_adjust();

					break;
				}

				if (history_index > 0) {
					history_index--;
					strcpy(line, history[history_index]);
					line_adjust();
					if (history_index == 0) {
						xfree(history[0]);
						history[0] = line;
					}
				}
				break;
				
			case KEY_PPAGE:	/* Page Up */
				window_current->start -= output_size;
				if (window_current->start < 0)
					window_current->start = 0;

				break;

			case KEY_NPAGE:	/* Page Down */
				window_current->start += output_size;
				if (window_current->start > window_current->lines - output_size)
					window_current->start = window_current->lines - output_size;

				if (window_current->start == window_current->lines - output_size) {
					window_current->more = 0;
					update_statusbar();
				}

				break;
				
			case KEY_F(1):	/* F1 */
				binding_help(0, 0);
				break;

			case KEY_F(2):	/* F2 */
				binding_quick_list(0, 0);
				break;

			case KEY_F(12):	/* F3 */
				binding_toggle_debug(0, 0);
				break;
				
			default:
				if (ch < 32)
					break;
				if (strlen(line) >= LINE_MAXLEN - 1)
					break;
				memmove(line + line_index + 1, line + line_index, LINE_MAXLEN - line_index - 1);

				line[line_index++] = ch;
		}

		/* je�li si� co� zmieni�o, wygeneruj dope�nienia na nowo */
		if (ch != 9) {
			array_free(completions);
			completions = NULL;
		}

		if (line_index - line_start > input->_maxx - 9 - window_current->prompt_len)
			line_start += input->_maxx - 19 - window_current->prompt_len;
		if (line_index - line_start < 10) {
			line_start -= input->_maxx - 19 - window_current->prompt_len;
			if (line_start < 0)
				line_start = 0;
		}
		window_refresh();
		werase(input);
		wattrset(input, COLOR_PAIR(7));

		if (lines) {
			int i;
			
			for (i = 0; i < 5; i++) {
				unsigned char *p;
				int j;

				if (!lines[lines_start + i])
					break;
				for (j = 0, p = lines[lines_start + i] + line_start; *p && j < input->_maxx + 1; p++, j++)
					mvwaddch(input, i, j, *p);
			}

			wmove(input, lines_index - lines_start, line_index - line_start);
		} else {
			if (window_current->prompt) {
				mvwaddstr(input, 0, 0, window_current->prompt);
				mvwaddstr(input, 0, window_current->prompt_len, line + line_start);
			} else
				mvwaddstr(input, 0, 0, line + line_start);
			wattrset(input, COLOR_PAIR(16) | A_BOLD);
			if (line_start > 0)
				mvwaddch(input, 0, window_current->prompt_len, '<');
			if (strlen(line) - line_start > input->_maxx + 1 - window_current->prompt_len)
				mvwaddch(input, 0, input->_maxx, '>');
			wattrset(input, COLOR_PAIR(7));
			wmove(input, 0, line_index - line_start + window_current->prompt_len);
		}
		wnoutrefresh(status);
		wnoutrefresh(input);
		doupdate();
	}
}

static void window_next()
{
	struct window *next = NULL;
	list_t l;

	for (l = windows; l; l = l->next) {
		if (l->data == window_current && l->next)
			next = l->next->data;
	}

	if (!next)
		next = windows->data;

	window_switch(next->id);
}

static void window_prev()
{
	struct window *prev = NULL;
	list_t l;

	for (l = windows; l; l = l->next) {
		if (l->data == window_current && l != windows)
			break;
		prev = l->data;
	}

	window_switch(prev->id);
}

/*
 * window_kill()
 *
 * usuwa podane okno.
 */
static void window_kill(struct window *w)
{
	int id = w->id;

	if (id == 1) {
		print("window_kill_status");
		return;
	}
	
	if (w == window_current)
		window_prev();

	if (config_sort_windows) {
		list_t l;

		for (l = windows; l; l = l->next) {
			struct window *w = l->data;

			if (w->id > id)
				w->id--;
		}
	}
		
	xfree(w->target);
	delwin(w->window);
	list_remove(&windows, w, 1);
}


/*
 * ui_ncurses_event()
 *
 * obs�uga zdarze�.
 */
static int ui_ncurses_event(const char *event, ...)
{
	va_list ap;

	va_start(ap, event);

	if (!event)
		return 0;

	if (!strcmp(event, "theme_init"))
		ui_ncurses_theme_init(); 

#if 0
	if (!strcmp(event, "xterm_update") && getenv("TERM") && !strncmp(getenv("TERM"), "xterm", 5)) {
		char *tmp = saprintf("\033]0;EKG (%d)\007", config_uin);
		tputs(tmp, 1, putchar);
		xfree(tmp);
	}
#endif

	if (!strcmp(event, "refresh_time")) {
		struct timer *t = timer_add(1, "ui-ncurses-time", "refresh_time");
		t->ui = 1;
	}

	if (!strcmp(event, "variable_changed")) {
		char *name = va_arg(ap, char*);

		if (!strcasecmp(name, "sort_windows") && config_sort_windows) {
			list_t l;
			int id = 1;

			for (l = windows; l; l = l->next) {
				struct window *w = l->data;
				
				w->id = id++;
			}
		}
	}

	if (!strcmp(event, "conference_rename")) {
		char *oldname = va_arg(ap, char*), *newname = va_arg(ap, char*);
		list_t l;

		for (l = windows; l; l = l->next) {
			struct window *w = l->data;

			if (w->target && !strcasecmp(w->target, oldname)) {
				xfree(w->target);
				xfree(w->prompt);
				w->target = xstrdup(newname);
				w->prompt = format_string(format_find("ncurses_prompt_query"), newname);
				w->prompt_len = strlen(w->prompt);
			}
		}
	}

	if (!strcmp(event, "command")) {
		char *command = va_arg(ap, char*);

		if (!strcasecmp(command, "bind")) {
			print("not_implemented");
			goto cleanup;
		}

		if (!strcasecmp(command, "query")) {
			char *param = va_arg(ap, char*);

			if (!param && !window_current->target)
				goto cleanup;

			if (param) {
				struct window *w;

				if ((w = window_find(param))) {
					window_switch(w->id);
					goto cleanup;
				}

				if (config_make_window == 2) {
					w = window_new(param);
					window_switch(w->id);
				}

				print("query_started", param);
				xfree(window_current->target);
				xfree(window_current->prompt);
				window_current->target = xstrdup(param);
				window_current->prompt = format_string(format_find("ncurses_prompt_query"), param);
				window_current->prompt_len = strlen(window_current->prompt);
			} else {
				const char *f = format_find("ncurses_prompt_none");

				print("query_finished", window_current->target);
				xfree(window_current->target);
				xfree(window_current->prompt);
				window_current->target = NULL;
				window_current->prompt = NULL;
				window_current->prompt_len = 0;
				
				if (strcmp(f, "")) {
					window_current->prompt = xstrdup(f);
					window_current->prompt_len = strlen(f);
				}
			}
			update_statusbar();
		}

		if (!strcasecmp(command, "window")) {
			char *p1 = va_arg(ap, char*), *p2 = va_arg(ap, char*);

			if (!p1 || !strcasecmp(p1, "list")) {
				list_t l;

				for (l = windows; l; l = l->next) {
					struct window *w = l->data;

					if (w->target)
						print("window_list_query", itoa(w->id), w->target);
					else
						print("window_list_nothing", itoa(w->id));
				}

				goto cleanup;
			}

			if (!strcasecmp(p1, "new")) {
				struct window *w = window_new(NULL);
				window_switch(w->id);
				goto cleanup;
			}

			if (!strcasecmp(p1, "switch")) {
				if (!p2) {
					print("not_enough_params", "window");
					goto cleanup;
				}
				window_switch(atoi(p2));
				goto cleanup;
			}			
			
			if (!strcasecmp(p1, "kill")) {
				struct window *w = window_current;

				if (p2) {
					list_t l;

					for (w = NULL, l = windows; l; l = l->next) {
						struct window *ww = l->data;

						if (ww->id == atoi(p2)) {
							w = ww;
							break;
						}
					}

					if (!w) {
						print("window_noexist");
						goto cleanup;
					}
				}

				window_kill(w);
				goto cleanup;
			}

			if (!strcasecmp(p1, "next")) {
				window_next();
				goto cleanup;
			}
			
			if (!strcasecmp(p1, "prev")) {
				window_prev();
				goto cleanup;
			}
			
			if (!strcasecmp(p1, "refresh")) {
				wrefresh(curscr);
				goto cleanup;
			}

			if (!strcasecmp(p1, "clear")) {
				struct window *w = window_current;
				int count = output_size - (w->lines - w->y);
				
				w->lines += count;
				w->start += count;
				
				wresize(w->window, w->lines, w->window->_maxx + 1);
				wmove(w->window, w->y, 0);

				window_refresh();
				wnoutrefresh(input);
				doupdate();

				goto cleanup;
			}
			
			print("window_invalid");
		}
	}

cleanup:
	va_end(ap);

	update_statusbar();
	
	return 0;
}

