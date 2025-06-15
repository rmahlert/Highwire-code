/* form.c
 ** Form handler
 **
 ** Changes
 ** Author         Date           Desription
 ** P Slegg        14-Aug-2009    input_keybrd: Utilise NKCC from cflib to handle the various control keys in text fields.
 ** P Slegg        18-Mar-2010    input_keybrd: Add Ctrl-Delete and Ctrl-Backspace edit functions
 ** P Slegg        29-May-2010    Add Ctrl-C to copy textarea to clipboard
 **
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <wctype.h>
#include <stdio.h> // Required for snprintf and printf

#ifndef __PUREC__
#  include <osbind.h>
#else
#  include <tos.h>
#endif

#include <cflib.h>

#include "token.h"
#include <gemx.h>

#ifdef __PUREC__
#define          FORM    FORM
typedef struct s_form  * FORM;
#define          INPUT   INPUT
typedef struct s_input * INPUT;
#endif

#include "global.h"
#include "fontbase.h"
#include "scanner.h"
#include "Containr.h"
#include "Loader.h"
#include "Location.h"
#include "parser.h"
#include "Form.h"
#include "hwWind.h"

typedef struct s_select   * SELECT;
typedef struct s_slctitem * SLCTITEM;

struct s_form {
	FRAME       Frame;
	FORM        Next;
	char      * Target;
	char      * Action;
	char      * Enctype;
	INPUT       TextActive;
	WORD        TextCursrX;
	WORD        TextCursrY;
	WORD        TextShiftX;
	WORD        TextShiftY;
	INPUT       InputList, Last;
	FORM_METHOD Method;
};

typedef enum {
	IT_HIDDN = 1,
	IT_GROUP,  /* node for a radio button group */
	IT_RADIO,
	IT_CHECK,
	IT_BUTTN,
	IT_SELECT,
	IT_TEXT,
	IT_TAREA,
	IT_FILE
} INP_TYPE;

struct s_input {
	INPUT	Next;
	union {
		void * Void;
		INPUT  Group;  /* radio button: points to the group node             */
		INPUT  FileEd; /* file upload button: points to its text input field */
		SELECT Select; /* selection menue                                    */
	}        u;
	WORDITEM Word;
	PARAGRPH Paragraph;
	FORM     Form;
	char   * Value;
	INP_TYPE Type;
	char     SubType;  /* [S]ubmit, [R]eset, [F]ile, \0 */
	BOOL     checked;
	BOOL     disabled;
	BOOL     readonly;
	UWORD    VisibleX;
	UWORD    VisibleY;
	short    CursorH;
	UWORD    TextMax;  /* either 0 for infinite long or restricted to n char */
	UWORD    TextRows;
	WCHAR ** TextArray; /* possibly rows of text */
	char     Name[1];
};

struct s_select {
	SLCTITEM ItemList;
	WCHAR  * SaveWchr;
	WORD     NumItems;
	char   * Array[1];
};
struct s_slctitem {
	SLCTITEM Next;
	char   * Value;
	WORD     Width;
	WORD     Length;
	WCHAR    Text[80];
	char     Strng[82];
};

// Forward declarations for functions defined later
static WCHAR * edit_init (INPUT, TEXTBUFF, UWORD cols, UWORD rows, size_t size);
static BOOL    edit_zero (INPUT);
static void    edit_feed (INPUT, ENCODING, const char * beg, const char * end);
static BOOL    edit_crlf (INPUT, WORD col, WORD row);
static BOOL    edit_char (INPUT, WORD col, WORD row, WORD chr);
static BOOL    edit_delc (INPUT, WORD col, WORD row);
static void    form_activate_multipart (FORM form);
static int     ctrl_left  (WCHAR * beg, WCHAR * end);
static int     ctrl_right (WCHAR * beg, WCHAR * end);
static void    del_chars (INPUT input, WORD col, WORD row, int numChars);
static void    finish_selct (INPUT input);
static char  * base64enc (const char * src, long len);
static void    form_activate (FORM form);
static void    input_file_handler (INPUT input); // Declaration for the handler

#define        __edit_len(inp,a,b)   ((inp)->TextArray[b]-(inp)->TextArray[a]-1)
#define        edit_rowln(inp,row)   __edit_len (inp, row, row +1)
#define        __edit_r_0(inp)       ((inp)->TextArray[0])
#define        __edit_r_r(inp)       ((inp)->TextArray[(inp)->TextRows])
#define        edit_space(inp)       ((WCHAR*)&__edit_r_0(inp)-__edit_r_r(inp))

/*------------------------------------------------------------------------------*/
#ifdef __PUREC__
int iswspace(wint_t wc)
{
	static const wchar_t spaces[] = {
		' ', '\t', '\n', '\r', 11, 12,  0x0085,
		0x2000, 0x2001, 0x2002, 0x2003, 0x2004, 0x2005,
		0x2006, 0x2008, 0x2009, 0x200a,
		0x2028, 0x2029, 0x205f, 0x3000, 0
	};
	int i;

	for (i = 0; spaces[i] != 0; i++)
		if (spaces[i] == wc)
			return 1;
	return 0;
}
#endif

/*------------------------------------------------------------------------------
 * Memo: UTF-8 ranges
00-7F : 1 byte
	( 0bbbaaaa = 7 bits )
80-1FFF   : 2 bytes
	( 10dccccb 0bbbaaaa = 13 bits)
2000-3FFFF : 3 bytes
	( 110eeddd 10dccccb 0bbbaaaa = 18 bits)
40000-7FFFFF : 4 bytes
	( 1110fffe 10eeeddd 10dccccb 0bbbaaaa = 23 bits)
800000-FFFFFFF : 5 bytes
	( 11110ggg 10gffffe 10eeeddd 10dccccb 0bbbaaaa = 28 bits)
10000000-1FFFFFFFF : 6 bytes
	( 111110ih 10hhhggg 10gffffe 10eeeddd 10dccccb 0bbbaaaa = 33 bits)
*/
/* here's a little helper for multipart POST
 only supporting unicode values on 16bits, thus 3 bytes utf8 sequence */
static char *
unicode_to_utf8 (WCHAR *src)
{
	int len = 0;
	char *ret;
	char *outptr;
	char out;
	WCHAR *inptr = src;
	WCHAR in;
	if (src == NULL)
	{
		return NULL;
	}
	while((in = *inptr++) != 0)
	{
		if (in < 128)	{ len++; }
		else if (in < 0x1fff) { len += 2; }
		else { len += 3; }
	}
	ret = (char*) malloc(len+1);
	if (ret != NULL)
	{
		outptr = ret;
		inptr = src;
		while((in = *inptr++) != 0)
		{
			if (in < 128)
			{
				*outptr++ = (char)in;
			}
			else if (in < 0x1fff)
			{
				out = (char)(((in >> 6) & 0x1F) | 0xC0); /* Corrected: Shift by 6 bits for first byte */
				*outptr++ = out;
				out = (char)((in & 0x3F) | 0x80); /* Corrected: Mask with 0x3F for second byte */
				*outptr++ = out;
			}
			else
			{
				out = (char)(((in >> 12) & 0x0F) | 0xE0); /* Corrected: Shift by 12 bits for first byte */
				*outptr++ = out;
				out = (char)(((in >> 6) & 0x3F) | 0x80); /* Corrected: Shift by 6 bits for second byte */
				*outptr++ = out;
				out = (char)((in & 0x3F) | 0x80); /* Corrected: Mask with 0x3F for third byte */
				*outptr++ = out;
			}
		}
		*outptr = 0;
	}
	return ret;
}


/*============================================================================*/
void *
new_form (FRAME frame, char * target, char * action, const char * method, char *enctype)
{
	char *ptr;
		FORM form = malloc (sizeof (struct s_form));

	if (!form) {
		return NULL;
	}

	if (!action) {
		const char * q = strchr (frame->BaseHref->FullName, '?');
		size_t       n = (q ? q - frame->BaseHref->FullName
		                    : strlen (frame->BaseHref->FullName));
		if ((action = malloc (n +1)) != NULL) {
			strncpy (action, frame->BaseHref->FullName, n);
			action[n] = '\0';
		} else {
			free(form); /* Allocation failed, clean up */
			return NULL;
		}
	}

	form->Frame     = frame;
	form->Next      = frame->FormList;
	frame->FormList = form;

	/* clean up action */
	if (action != NULL)
	{
		ptr = strstr(action,"&amp;");
		while(ptr != NULL)
		{
			ptr++;
			memmove(ptr, ptr + 4, strlen(ptr + 4) + 1);
			ptr = strstr(ptr,"&amp;");
		}
	}

	form->Target = target;
	form->Action = action;
	form->Method = (method && strcmp  (method, "AUTH") == 0 ? METH_AUTH :
	                method && stricmp (method, "post") == 0 ? METH_POST :
	                                                          METH_GET);
	if (enctype != NULL)
	{
		form->Enctype = enctype;
	}
	else
	{
		form->Enctype = strdup("application/x-www-form-urlencoded");
	}
	form->TextActive = NULL;
	form->TextCursrX = 0;
	form->TextCursrY = 0;
	form->TextShiftX = 0;
	form->TextShiftY = 0;
	form->InputList = NULL;
	form->Last      = NULL;

	return form;
}

/*============================================================================*/
void
form_finish (TEXTBUFF current_tb) // Renamed parameter to avoid shadowing future 'current'
{
	INPUT input = (current_tb->form ? ((FORM)current_tb->form)->InputList : NULL);
	while (input) {
		if (input->Type == IT_SELECT) {
			finish_selct (input);
		}
		input = input->Next;
	}
	current_tb->form = NULL;
}

/*============================================================================*/
void
destroy_form (FORM form, BOOL all)
{
	while (form) {
		FORM  next  = form->Next;
		INPUT input = form->InputList;
		while (input) {
			INPUT inxt = input->Next;
			if (input->Type == IT_SELECT) {
				SELECT sel = input->u.Select;
				if (sel) {
					while (sel->ItemList) {
						SLCTITEM item = sel->ItemList;
						sel->ItemList = item->Next;
						if (item->Value && item->Value != item->Strng +1) {
							free (item->Value);
						}
						free (item);
					}
					input->Value      = NULL;
					input->Word->item = sel->SaveWchr;
					free (sel);
				}
			}
			if (input->Type != IT_GROUP && input->Value) {
				free (input->Value);
			}
			if (input->Word) {
				input->Word->input = NULL;
			}
			free (input);
			input = inxt;
		}
		if (form->Enctype) {
			free (form->Enctype);
		}
		if (form->Target) {
			free (form->Target);
		}
		if (form->Action) {
			free (form->Action);
		}
		free (form);
		if (!all) break;
		else      form = next;
	}
}


/*----------------------------------------------------------------------------*/
static INPUT
_alloc (INP_TYPE type, TEXTBUFF current, const char * name)
{
	FORM   form  = current->form;
	size_t n_len = (name && *name ? strlen (name) : 0);
	INPUT  input = malloc (sizeof (struct s_input) + n_len);
	if (!input) {
		return NULL;
	}
	input->Next      = NULL;
	input->u.Void    = NULL;
	input->Paragraph = current->paragraph;
	input->Form      = form;
	input->Value     = NULL;
	input->Type      = type;
	input->SubType   = '\0';
	input->disabled  = FALSE;
	input->readonly  = TRUE;
	input->VisibleX  = 0;
	input->VisibleY  = 0;
	input->TextMax   = 0;
	input->TextRows  = 0;
	input->TextArray = NULL;

	if (type <= IT_GROUP) {
		input->Word    = NULL;
		input->checked = (type == IT_HIDDN);
	} else {
		input->Word    = current->word;
		input->checked = (type >= IT_SELECT/*IT_TEXT*/);
		current->word->input = input;
	}

	if (n_len) {
		memcpy (input->Name, name, n_len);
	}
	input->Name[n_len] = '\0';

	if (type != IT_RADIO) {
		if (form->Last) {
			form->Last->Next = input;
		} else {
			form->InputList  = input;
		}
		form->Last          = input;
	}
	return input;
}

/*----------------------------------------------------------------------------*/
static void
set_word (TEXTBUFF current, WORD asc, WORD dsc, WORD wid)
{
	WORDITEM word = current->word;

	if (current->text == current->buffer) {
		*(current->text++) = font_Nobrk(current->word->font);
	}
	new_word (current, TRUE);
	if (wid <= 0) {
		wid = word->word_width - wid;
	}
	word->word_width     = wid +4;
	word->word_height    = asc +2;
	word->word_tail_drop = dsc +2;
	word->space_width    = 0;
	TA_Color(word->attr) = G_BLACK;
}

/*============================================================================*/
INPUT
form_check (TEXTBUFF current, const char * name, char * value, BOOL checked)
{
	WORD  asc = current->word->word_height - (current->word->word_tail_drop +2);
	INPUT input = _alloc (IT_CHECK, current, name);

	if (value) {
	input->Value   = value;
	} else {
		char *val = malloc (3);
		if (val) {
			memcpy (val, "on", 3);
		}
		input->Value = val;
	}
	input->checked = checked;
	if (asc < 2) asc = 2;
	set_word (current, asc, -1, asc -1);
	return input;
}

/*============================================================================*/
INPUT
form_radio (TEXTBUFF current,
            const char * name, const char * value, BOOL checked)
{
	WORD  asc   = current->word->word_height - current->word->word_tail_drop;
	FORM  form  = current->form;
	INPUT input = _alloc (IT_RADIO, current, value);
	INPUT group = form->InputList;

	while (group) {
		if (group->Type == IT_GROUP && strcmp (group->Name, name) == 0) break;
		group = group->Next;
	}
	if (!group) {
		group = _alloc (IT_GROUP, current, name);
	}
	if (checked && !group->checked) {
		group->Value   = input->Name;
		group->checked = TRUE;
		input->checked = TRUE;
	}
	input->u.Group = group;
	input->Next    = group->u.Group;
	group->u.Group = input;

	if (asc < 1) asc =  1;
	else         asc |= 1;
	set_word (current, asc, 0, asc);

	return input;
}

/*============================================================================*/
INPUT
form_buttn (TEXTBUFF current, const char * name, const char * value,
            ENCODING encoding, char sub_type)
{
	WORDITEM word  = current->word;
	TEXTATTR attr  = word->attr;
	INPUT    input = _alloc (IT_BUTTN, current, name);

	if (sub_type == 'I') {
		FRAME frame          = ((FORM)current->form)->Frame;
		short word_height    = current->word->word_height;
		short word_tail_drop = current->word->word_tail_drop;
		short word_v_align   = current->word->vertical_align;

		input->SubType = 'S';

		new_image (frame, current, value, frame->BaseHref, 0,0, 0,0,FALSE);
		font_switch (current->word->font, NULL);

		new_word (current, TRUE);
		current->word->word_height    = word_height;
		current->word->word_tail_drop = word_tail_drop;
		current->word->vertical_align = word_v_align;

	} else {
		input->SubType = sub_type;
		input->Value   = strdup (value);
		if (!input->Value) {
			/* Fail gracefully by disabling the input if memory allocation fails */
			input->disabled = TRUE;
		}

		font_byType (-1, -1, -1, word);
		scan_string_to_16bit (value, encoding, &current->text,
		                      word->font->Base->Mapping);
		if (current->text == current->buffer) {
			*(current->text++) = font_Nobrk (word->font);
		}
		set_word (current, word->word_height, word->word_tail_drop +1, -4);
	}
	current->word->attr = attr;

	return input;
}

/*============================================================================*/
INPUT
form_text (TEXTBUFF current, const char * name, char * value, UWORD maxlen,
          ENCODING encoding, UWORD cols, BOOL readonly, BOOL is_pwd)
{
	INPUT  input = _alloc (IT_TEXT, current, name);
	size_t v_len = (value  ? strlen (value) : 0);
	size_t size  = (maxlen ? maxlen : v_len);

	input->TextMax  = maxlen;
	input->readonly = readonly;

	if (edit_init (input, current, cols, 1, size)) {
		if (value && v_len < maxlen) {
			char * mem = malloc (maxlen +1);
			if (mem) {
				memcpy (mem, value, v_len +1);
				free (value);
				value = mem;
			}
			/* If mem is NULL, we just continue with the old, smaller 'value' buffer */
		} else if (!value && is_pwd) {
			value = malloc (maxlen +1);
			if (!value) {
				input->readonly = TRUE; /* Fail gracefully by making the field readonly */
				return input;
			}
		}
		if (is_pwd) { /* == "PASSWORD" */
			input->Value = value;
		}
		if (value) {
			edit_feed (input, encoding, value, value + v_len);
			if (value != input->Value) {
				free (value);
			}
		}
	} else {
		input->Value = value;
	}

	return input;
}

/*============================================================================*/
INPUT
new_input (PARSER parser, WORD width)
{
	INPUT    input_new   = NULL; // Renamed to avoid shadowing
	FRAME    frame   = parser->Frame;
	TEXTBUFF current = &parser->Current;
	const char * val = "T"; /* default type is TEXT */
	char output[100], name[100];

	if (!current->form) {
		current->form = new_form (frame, NULL, NULL, NULL, NULL);
	}

	get_value (parser, KEY_NAME, name, sizeof(name));

	if (!get_value (parser, KEY_TYPE, output, sizeof(output))
	    || stricmp (output, val = "TEXT")     == 0
	    || stricmp (output, val = "FILE")     == 0
	    || stricmp (output, val = "PASSWORD") == 0) {
		UWORD  mlen = get_value_unum (parser, KEY_MAXLENGTH, 0);
		UWORD  cols = get_value_unum (parser, KEY_SIZE, 0);
		if (!mlen) {
			if (cols > 500)  cols = 500;
		} else {
			if (mlen > 500)  mlen = 500;
			if (cols > mlen) cols = mlen;
		}

		if (*val == 'F') {
			INPUT bttn = form_buttn (current, name, "...", frame->Encoding, 'F');

			input_new = form_text (current, name,
					   	get_value_exists (parser, KEY_READONLY) ? get_value_str (parser, KEY_VALUE) : strdup(""),
		                   mlen, frame->Encoding, (cols ? cols : 20),
		                   1, (*val == 'P'));

			/* Add the browse button */
			bttn->u.FileEd = input_new;
			input_new->Type = IT_FILE;
			if (width > 0) {
				width = (width > bttn->Word->word_width
				         ? width - bttn->Word->word_width : 1);
			}
		} else {
			input_new = form_text (current, name, get_value_str (parser, KEY_VALUE),
		                   mlen, frame->Encoding, (cols ? cols : 20),
		                   get_value_exists (parser, KEY_READONLY),
		                   (*val == 'P'));
		}

		if (width > 0) {
			WORD cw = (input_new->Word->word_width -1 -4) / input_new->VisibleX;
			input_new->Word->word_width = max (width, input_new->Word->word_height *2);
			input_new->VisibleX = (input_new->Word->word_width -1 -4) / cw;
		}
	} else if (stricmp (output, "HIDDEN") == 0) {
		input_new = _alloc (IT_HIDDN, current, name);
		input_new->Value = get_value_str (parser, KEY_VALUE);

	} else if (stricmp (output, "RADIO") == 0) {
		get_value (parser, KEY_VALUE, output, sizeof(output));
		input_new = form_radio (current, name, output,
		                    get_value (parser, KEY_CHECKED, NULL,0));

	} else if (stricmp (output, "CHECKBOX") == 0) {
		input_new = form_check (current, name, get_value_str (parser, KEY_VALUE),
		                    get_value (parser, KEY_CHECKED, NULL,0));

	} else if (stricmp (output, val = "Submit") == 0 ||
	           stricmp (output, val = "Reset")  == 0 ||
	           stricmp (output, val = "Button") == 0 ||
	           stricmp (output, val = "Image")  == 0) {
		char sub_type = (*val == 'B' ? '\0' : *val);

		if (sub_type == 'I') {
			if (get_value (parser, KEY_SRC, output, sizeof(output))) {
				val = output;
			} else {
				sub_type = 'S';
			}
		} else if (get_value (parser, KEY_VALUE, output, sizeof(output))) {
			char * p = output;
			while (isspace (*p)) p++;
			if (*p) {
				p = strchr (val = p, '\0');
				while (isspace (*(--p))) *p = '\0';
			}
		}
		input_new = form_buttn (current, name, val, frame->Encoding, sub_type);

	} else if (stricmp (output, "debug") == 0) {
		FORM  form_debug = current->form; // Renamed to avoid shadowing
		INPUT inp_debug  = form_debug->InputList; // Renamed to avoid shadowing
		printf ("%s: %s%s", (form_debug->Method == METH_POST ? "POST" : "GET"),
		        (form_debug->Action ? form_debug->Action : "<->"),
		        (strchr (form_debug->Action, '?') ? "&" : "?"));
		while (inp_debug) {
			if (inp_debug->checked) {
				printf ("%s=%s&", inp_debug->Name, (inp_debug->Value ? inp_debug->Value : ""));
			}
			inp_debug = inp_debug->Next;
		}
		printf ("\n");
	}

	if (input_new && get_value (parser, KEY_DISABLED, NULL,0)) {
		input_disable (input_new, TRUE);
	}

	return input_new;
}

/*============================================================================*/
INPUT
new_tarea (PARSER parser, const char * beg, const char * end, UWORD nlines)
{
	INPUT    input   = NULL;
	FRAME    frame   = parser->Frame;
	TEXTBUFF current = &parser->Current;

	size_t size = (end - beg) + nlines * (sizeof(void*) / sizeof(WCHAR));
	UWORD  rows = get_value_unum (parser, KEY_ROWS, 0);
	UWORD  cols = get_value_unum (parser, KEY_COLS, 0);
	char   name[100];

	if (!current->form) {
		current->form = new_form (frame, NULL, NULL, NULL, NULL);
	}
	get_value (parser, KEY_NAME, name, sizeof(name));
	input = _alloc (IT_TAREA, current, name);

	input->disabled = get_value_exists (parser, KEY_DISABLED);
	input->readonly = get_value_exists (parser, KEY_READONLY);

	if (!rows) rows = 5;
	if (!cols) cols = 40;

	if (edit_init (input, current, cols, rows, size)) {
		edit_feed (input, frame->Encoding, beg, end);
	}

	return input;
}


/*============================================================================*/
INPUT
form_selct (TEXTBUFF current, const char * name, UWORD size, BOOL disabled)
{
	WORDITEM word = current->word;
	WORD     asc  = word->word_height -1;
	WORD     dsc  = word->word_tail_drop;
	INPUT    input_sel; // Renamed to avoid shadowing

	(void)size;

	if (!current->form /*&&
		 (current->form = new_form (frame, NULL, NULL, NULL)) == NULL*/) {
		input_sel = NULL;

	} else if ((input_sel = _alloc (IT_SELECT, current, name)) != NULL) {
		SELECT sel = malloc (sizeof (struct s_select));
		if (!sel) {
			input_sel->disabled = TRUE; /* Fail gracefully by disabling the element */
			return input_sel;
		}
		if ((input_sel->u.Select = sel) != NULL) {
			sel->ItemList = NULL;
			sel->NumItems = 0;
			sel->Array[0] = NULL;
		} else {
			disabled = TRUE;
		}
		input_sel->disabled = disabled;
		set_word (current, asc, dsc, asc + dsc);
		sel->SaveWchr = input_sel->Word->item;
	}

	return input_sel;
}

/*============================================================================*/
INPUT
selct_option (TEXTBUFF current, const char * text,
              BOOL disabled, ENCODING encoding, char * value, BOOL selected)
{
	INPUT    input_opt = (current->form ? ((FORM)current->form)->Last : NULL); // Renamed input
	SELECT   sel;
	SLCTITEM item;

	if (((!input_opt || input_opt->Type != IT_SELECT) &&
	     (input_opt = form_selct (current, "", 1, FALSE)) == NULL) ||
	    (sel = input_opt->u.Select) == NULL) {
		return NULL;
	}

	if (*text) {
		item = malloc (sizeof(struct s_slctitem));
		if (item) {
		size_t tlen = strlen (text);
		WORD  pts[8];
		if ((text[0] == '-' && (!text[1] || (text[1] == '-' && (!text[2] ||
		    (text[2] == '-' && (!text[3] || (text[3] == '-'))))))) ||
		    (tlen > 3 && strncmp (text + tlen -3, "---", 3) == 0)) {
			item->Strng[0] = '-';
			disabled = TRUE;
		} else if (disabled) {
			item->Strng[0] = '!';
		} else {
			item->Strng[0] = ' ';
		}
		if (tlen >= sizeof(item->Strng) -1) {
			tlen = sizeof(item->Strng) -2;
		}
		memcpy (item->Strng +1, text, tlen);
		item->Strng[tlen +1] = '\0';

		encoding = ENCODING_ATARIST;
		current->text = current->buffer;
		scan_string_to_16bit (text, encoding, &current->text,
		                      current->word->font->Base->Mapping);
		tlen = current->text - current->buffer;
		if (tlen >= numberof(item->Text)) {
			tlen = numberof(item->Text) -1;
		}
		current->text = current->buffer;
		memcpy (item->Text, current->buffer, tlen *2);
		item->Text[tlen] = '\0';
		item->Length = (WORD)tlen;
		vqt_f_extent16n (vdi_handle, item->Text, item->Length, pts);
		item->Width = pts[2] - pts[0];

		item->Value = (disabled ? NULL : value ? value : item->Strng +1);

		if (!sel->ItemList || selected) {
			input_opt->Word->item   = item->Text;
			input_opt->Word->length = item->Length;
			input_opt->Value        = item->Value;
		}
		item->Next    = sel->ItemList;
		sel->ItemList = item;
		sel->NumItems--;
	}
    } // End of if (*text) block. It was missing a closing brace.

	return input_opt;
}

/*============================================================================*/
void
selct_finish (TEXTBUFF current_tb_finish) // Renamed parameter
{
	INPUT input = (current_tb_finish->form ? ((FORM)current_tb_finish->form)->Last : NULL);
	if (input && input->Type == IT_SELECT) {
		finish_selct (input);
	}
}

/*----------------------------------------------------------------------------*/
static void
finish_selct (INPUT input_fs) // Renamed parameter
{
	SELECT sel = input_fs->u.Select;
	WORD   num;

	if (!sel || !sel->NumItems) {
		input_fs->disabled = TRUE;

	} else if ((num = -sel->NumItems) > 0) {
		SELECT rdy = malloc (sizeof (struct s_select) + num * sizeof(char*));
		if (rdy) {
			SLCTITEM item_fs = sel->ItemList; // Renamed variable
			short    wdth = 0;
			rdy->ItemList = sel->ItemList;
			rdy->SaveWchr = sel->SaveWchr;
			rdy->NumItems = num;
			rdy->Array[0] = rdy->Array[num] = NULL;
			do {
				if (wdth < item_fs->Width) { // Using item_fs
					wdth = item_fs->Width;   // Using item_fs
				}
				rdy->Array[--num] = item_fs->Strng; // Using item_fs
			} while ((item_fs = item_fs->Next) != NULL && num); // Using item_fs
			input_fs->Word->word_width += wdth +2;
			free (sel);
			input_fs->u.Select = rdy;
		}
	}
}


/*============================================================================*/
void
input_draw (INPUT input_id, WORD x, WORD y) // Renamed parameter
{
	WORDITEM word = input_id->Word;
	short c_lu, c_rd;
	PXY p[6];
	p[2].p_x = (p[0].p_x = x) + word->word_width -1;
	p[1].p_y = y - word->word_height;
	p[3].p_y = y + word->word_tail_drop -1;

	if (input_id->Type >= IT_TEXT) {
		vsf_color (vdi_handle, G_WHITE);
		vsl_color (vdi_handle, G_LBLACK);
		c_lu = G_BLACK;
		c_rd = G_LWHITE;
	} else if (input_id->checked && input_id->Type != IT_SELECT) {
		vsf_color (vdi_handle, G_LBLACK);
		vsl_color (vdi_handle, G_BLACK);
		c_lu = G_BLACK;
		c_rd = G_WHITE;
	} else {
		vsf_color (vdi_handle, (ignore_colours ? G_WHITE : G_LWHITE));
		vsl_color (vdi_handle, G_BLACK);
		c_lu = G_WHITE;
		c_rd = G_LBLACK;
	}
	if (input_id->Type == IT_RADIO) {
		p[1].p_x = p[3].p_x = (p[0].p_x + p[2].p_x) /2;
		p[0].p_y = p[2].p_y = (p[1].p_y + p[3].p_y) /2;
	} else {
		p[0].p_y = p[3].p_y;
		p[1].p_x = p[0].p_x;
		p[2].p_y = p[1].p_y;
		p[3].p_x = p[2].p_x;
	}
	p[4]     = p[0];
	v_fillarea (vdi_handle, 4, &p[0].p_x);
	v_pline    (vdi_handle, 5, &p[0].p_x);
	if (input_id->Type == IT_RADIO) {
		vsl_color (vdi_handle, c_lu);
		p[0].p_x++;
		p[1].p_y++;
		v_pline (vdi_handle, 2, &p[0].p_x);
		p[0].p_x++;
		p[1].p_y++;
		v_pline (vdi_handle, 2, &p[0].p_x);
		vsl_color (vdi_handle, c_rd);
		p[1].p_x++;   p[2].p_x -= 2;   p[4].p_x = p[0].p_x +1;
		p[1].p_y++;   p[3].p_y -= 2;   p[4].p_y = p[0].p_y +1;
		v_pline (vdi_handle, 4, &p[1].p_x);
		p[2].p_x++;
		p[3].p_y++;
		v_pline (vdi_handle, 2, &p[2].p_x);

	} else {
		vsl_color (vdi_handle, c_lu);
		p[1].p_x = ++p[0].p_x;   p[2].p_x -= 2;
		p[1].p_y = ++p[2].p_y;   p[0].p_y -= 2;
		v_pline (vdi_handle, 3, &p[0].p_x);
		if (input_id->Type == IT_BUTTN) {
			p[1].p_x = ++p[0].p_x;  --p[2].p_x;
			p[1].p_y = ++p[2].p_y;  --p[0].p_y;
			v_pline (vdi_handle, 3, &p[0].p_x);
			vsl_color (vdi_handle, c_rd);
			p[0].p_x++;   p[1].p_x = ++p[2].p_x;
			p[2].p_y++;   p[1].p_y = ++p[0].p_y;
			v_pline (vdi_handle, 3, &p[0].p_x);
			p[0].p_x--;
			p[2].p_y--;
		} else if (input_id->Type == IT_SELECT) {
			short w;
			p[1].p_x = p[2].p_x; /* save this */
			p[3].p_y = p[4].p_y = p[2].p_y +3;
			p[2].p_y            = p[0].p_y -2;
			w = (p[2].p_y - p[3].p_y +1) & ~1;
			p[4].p_x = p[2].p_x -2;
			p[3].p_x = p[4].p_x - w;
			p[2].p_x = p[3].p_x + w /2;
			p[5]     = p[2];
			if (ignore_colours) {
				vsl_color (vdi_handle, c_rd);
				v_pline (vdi_handle, 4, &p[2].p_x);
			} else {
				v_pline (vdi_handle, 3, &p[2].p_x);
				vsl_color (vdi_handle, c_rd);
				v_pline (vdi_handle, 2, &p[4].p_x);
			}
			p[2] = p[1];
			p[0].p_x++;
			p[2].p_y++;
		} else {
			vsl_color (vdi_handle, c_rd);
			p[0].p_x++;
			p[2].p_y++;
		}
		p[1].p_x = ++p[2].p_x;
		p[1].p_y = ++p[0].p_y;
		v_pline (vdi_handle, 3, &p[0].p_x);
	}
	if (input_id->Type >= IT_TEXT) {
		BOOL     fmap = (word->font->Base->Mapping != MAP_UNICODE);
		FORM     form = input_id->Form;
		WCHAR ** wptr = input_id->TextArray;
		short    rows = min (input_id->TextRows, input_id->VisibleY);
		WORD     shft;
		PXY      pos;
		pos.p_x = x + 3;
		pos.p_y = y - input_id->CursorH * input_id->VisibleY;
		if (input_id == form->TextActive) {
			shft =  form->TextShiftX;
			wptr += form->TextShiftY;
		} else {
			shft = 0;
		}
		if (fmap) {
			vst_map_mode (vdi_handle, MAP_UNICODE);
		}
		while (rows--) {
			WCHAR * ptr = wptr[0] + shft;
			WORD    len = min ((UWORD)(wptr[1] -1 - ptr), input_id->VisibleX);
			pos.p_y += input_id->CursorH;
			if (len > 0)
#ifdef __PUREC__
				v_ftext16n (vdi_handle, pos.p_x, pos.p_y, ptr, len);
#else
				v_ftext16n (vdi_handle, pos, ptr, len);
#endif
			wptr++;
		}
		if (input_id == form->TextActive) {
			WCHAR * ptr = input_id->TextArray[form->TextShiftY]   + shft;
			vqt_f_extent16n (vdi_handle, ptr, form->TextCursrX - shft, &p[0].p_x);
			p[0].p_x = x +2 + p[1].p_x - p[0].p_x;
			p[1].p_x = p[0].p_x +1;
			p[0].p_y = y - word->word_height +2
			         + (form->TextCursrY - form->TextShiftY) * input_id->CursorH;
			p[1].p_y = p[0].p_y                              + input_id->CursorH;
			vsf_color (vdi_handle, G_WHITE);
			vswr_mode (vdi_handle, MD_XOR);
			v_bar     (vdi_handle, &p[0].p_x);
			vswr_mode (vdi_handle, MD_TRANS);
		}
		if (fmap) {
			vst_map_mode (vdi_handle, word->font->Base->Mapping);
		}
	} else if (input_id->Type >= IT_BUTTN) {
		v_ftext16 (vdi_handle,
		           x + (input_id->Type == IT_BUTTN ? 4 : 3), y, word->item);
	}
	if (input_id->disabled) {
		p[1].p_x = (p[0].p_x = x) + word->word_width -1;
		p[0].p_y = y - word->word_height;
		p[1].p_y = y + word->word_tail_drop -1;
		vsf_interior (vdi_handle, FIS_PATTERN);
		if (ignore_colours) {
			vsf_style (vdi_handle, 3);
			vsf_color (vdi_handle, G_WHITE);
		} else {
			vsf_style (vdi_handle, 4);
			vsf_color (vdi_handle, G_LWHITE);
		}
		v_bar (vdi_handle, &p[0].p_x);
		vsf_interior (vdi_handle, FIS_SOLID);
	}
}


/*============================================================================*/
void
input_disable (INPUT input_id, BOOL onNoff) // Renamed parameter
{
	input_id->disabled = onNoff;
	/* check for file uploade element to set its correspondind part also */
	if (input_id->Type == IT_BUTTN) {
		INPUT field = (input_id->SubType == 'F' ? input_id->u.FileEd : NULL);
		if (field && field->Next == input_id) {
			field->disabled = onNoff;
		}
	} else if (input_id->Type == IT_TEXT) {
		INPUT buttn = (input_id->Next->Type == IT_BUTTN ? input_id->Next : NULL);
		if (buttn->SubType == 'F' && buttn->u.FileEd == input_id) {
			buttn->disabled = onNoff;
		}
	}
}


/*============================================================================*/
BOOL
input_isEdit (INPUT input_ie) // Renamed parameter
{
	return (input_ie->Type >= IT_TEXT);
}


/*----------------------------------------------------------------------------*/
static void
coord_diff (INPUT check, INPUT input_cd,  GRECT * rect) // Renamed input parameter
{
	WORDITEM c_w = check->Word, i_w = input_cd->Word;
	rect->g_x = c_w->h_offset
	           - i_w->h_offset;
	rect->g_y = (WORD)((c_w->line->OffsetY - c_w->word_height)
	           - (i_w->line->OffsetY - i_w->word_height));
	rect->g_w = c_w->word_width;
	rect->g_h = c_w->word_height + c_w->word_tail_drop;
	if (check->Paragraph != input_cd->Paragraph) {
		long c_x, c_y, i_x, i_y;
		dombox_Offset (&check->Paragraph->Box, &c_x, &c_y);
		dombox_Offset (&input_cd->Paragraph->Box, &i_x, &i_y);
		rect->g_x += (WORD)(c_x - i_x);
		rect->g_y += (WORD)(c_y - i_y);
	}
}

/*============================================================================*/
WORD
input_handle (INPUT input_ih, PXY mxy, GRECT * radio, char *** popup) // Renamed input parameter
{
	WORD rtn = 0;

	if (!input_ih->disabled) switch (input_ih->Type) {

		case IT_RADIO:
			if (!input_ih->checked) {
				INPUT group = input_ih->u.Group;
				rtn = 1;
				if (group->checked) {
					INPUT check = group->u.Group;
					do if (check->checked) {
						coord_diff (check, input_ih, radio);
						check->checked = FALSE;
						rtn = 2;
						break;
					} while ((check = check->Next) != NULL);
				} else {
					group->checked = TRUE;
				}
				group->Value = input_ih->Name;
				input_ih->checked = TRUE;
			}
			break;

		case IT_FILE:
		case IT_TAREA:
		case IT_TEXT: {
			FORM form = input_ih->Form;
			if (form->TextActive && form->TextActive != input_ih) {
				coord_diff (form->TextActive, input_ih, radio);
				rtn = 2;
			} else {
				rtn = 1;
			}
			form->TextActive = input_ih;
			form->TextShiftX = 0;
			form->TextShiftY = 0;
			if (mxy.p_y > 0 && input_ih->TextRows > 1) {
				form->TextCursrY = mxy.p_y / input_ih->CursorH;
				if (form->TextCursrY > input_ih->VisibleY) {
					 form->TextCursrY = input_ih->VisibleY;
				}
				if (form->TextCursrY > input_ih->TextRows -1) {
					 form->TextCursrY = input_ih->TextRows -1;
				}
			} else {
				form->TextCursrY = 0;
			}
			if (mxy.p_x > 0 && edit_rowln (input_ih, form->TextCursrY) > 0) {
				form->TextCursrX = mxy.p_x / (input_ih->Word->font->SpaceWidth -1);
				if (form->TextCursrX > input_ih->VisibleX) {
					 form->TextCursrX = input_ih->VisibleX;
				}
				if (form->TextCursrX > edit_rowln (input_ih, form->TextCursrY)) {
					 form->TextCursrX = (WORD)edit_rowln (input_ih, form->TextCursrY);
				}
			} else {
				form->TextCursrX = 0;
			}
		}	break;

		case IT_CHECK:
			input_ih->checked = !input_ih->checked;
			rtn = 1;
			break;

		case IT_SELECT: {
			SELECT sel_ih = input_ih->u.Select; // Renamed variable
			if (sel_ih && sel_ih->NumItems > 0) {
				WORDITEM word = input_ih->Word;
				long   * x    = &((long*)radio)[0];
				long   * y    = &((long*)radio)[1];
				dombox_Offset (&input_ih->Paragraph->Box, x, y);
				*x += word->h_offset;
				*y += word->line->OffsetY + word->word_tail_drop -1;
				*popup = sel_ih->Array;
				rtn = 1;
			}
		}	break;

		case IT_BUTTN:
			if (!input_ih->checked) {
				input_ih->checked = TRUE;
				rtn = 1;
			}
			break;

		default: ;
	}
	return rtn;
}


/*----------------------------------------------------------------------------*/
static char *
base64enc (const char * src, long len)
{
	char * mem = (src && len > 0 ? malloc (((len +2) /3) *4 +1) : NULL);
	if (mem) {
		const char trans[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		char * dst = mem;
		do {
			if (len >= 3) {
				long val = ((((long)src[0] <<8) | src[1]) <<8) | src[2];
				dst[3] = trans[val & 0x3F]; val >>= 6;
				dst[2] = trans[val & 0x3F]; val >>= 6;
				dst[1] = trans[val & 0x3F]; val >>= 6;
				dst[0] = trans[val & 0x3F];
			} else if (len == 2) {
				long val = (((long)src[0] <<8) | src[1]) <<2;
				dst[3] = '=';
				dst[2] = trans[val & 0x3F]; val >>= 6;
				dst[1] = trans[val & 0x3F]; val >>= 6;
				dst[0] = trans[val & 0x3F];
			} else /* len == 1 */ {
				long val = (long)src[0] <<4;
				dst[3] = '=';
				dst[2] = '=';
				dst[1] = trans[val & 0x3F]; val >>= 6;
				dst[0] = trans[val & 0x3F];
			}
			src += 3;
			dst += 4;
		} while ((len -= 3) > 0);
		dst[0] = '\0';
	}
	return mem;
}

/*----------------------------------------------------------------------------*/
static void
form_activate (FORM form_fa) // Renamed parameter
{
	FRAME    frame = form_fa->Frame;
	INPUT    elem_fa  = form_fa->InputList; // Renamed to avoid shadowing
	LOCATION loc   = frame->Location;
	LOADER   ldr   = NULL;

    // Declare variables for METH_AUTH path at the top of the function if they are used within it.
    const char * current_realm = NULL;
    char buf_fa[100];
    char * p_fa = buf_fa;
    WCHAR * beg_fa = NULL, * end_fa = NULL;

	if (form_fa->Method == METH_AUTH) { /* special case, internal created *
	                                  * for HTTP Authentication        */
		current_realm = (frame->AuthRealm && *frame->AuthRealm
		                      ? frame->AuthRealm : NULL);
		ldr = start_page_load (frame->Container, NULL,loc, TRUE, NULL);
		if (!ldr) {
			current_realm = NULL;
		}
		if (current_realm) {
			if (elem_fa && elem_fa->TextArray) {
				beg_fa = elem_fa->TextArray[0];
				end_fa = elem_fa->TextArray[1] -1;
			} else {
				beg_fa = end_fa = NULL;
			}
			if (beg_fa < end_fa) {
				do {
					*(p_fa++)= *(beg_fa++);
				} while (beg_fa < end_fa);
				*(p_fa++) = ':';
				*(p_fa)   = '\0';
				elem_fa = elem_fa->Next;
			} else {
				current_realm = NULL;
			}
		}
		if (current_realm && elem_fa && elem_fa->Value) {
				snprintf(p_fa, sizeof(buf_fa) - (p_fa - buf_fa), "%s", elem_fa->Value);
				elem_fa = elem_fa->Next;
			} else {
				current_realm = NULL;
			}

		if (current_realm && elem_fa && !elem_fa->Next
		          && elem_fa->Type == IT_BUTTN && elem_fa->SubType == 'S') {
			ldr->AuthRealm = strdup (current_realm);
			ldr->AuthBasic = base64enc (buf_fa, strlen(buf_fa));
		}

		return; // This return is crucial for the METH_AUTH path.
	}

    // Variables for non-METH_AUTH path (POST/GET) declared here
    size_t	size  = 0;
    size_t	len;
    char 	*data;
    char	*url_fa;

	if (elem_fa) // Using elem_fa
	{
		int nbvar = 0;
		int nbfile = 0;
		/* pre-check list of inputs for a type IT_FILE */
		INPUT temp_elem = elem_fa; // Use a temporary variable for iteration
		do
		{
			if (temp_elem->checked && *temp_elem->Name)
			{
				nbvar++;
				if (temp_elem->Type == IT_FILE)
				{
					nbfile++;
				}
			}
			temp_elem = temp_elem->Next;
		} while(temp_elem != NULL);

		if (nbfile > 0)
		{
			/* PUT only possible if single file and single item */
			if (form_fa->Method == METH_PUT && ((nbvar > 1) || (nbfile > 1)))
			{
				form_fa->Method = METH_POST;
			}
			if (form_fa->Method == METH_POST)
			{
				/* if file input used, switch to "multipart/form-data" enc */
				form_activate_multipart(form_fa);
				return;
			}
		}
		if (form_fa->Enctype && stricmp(form_fa->Enctype, "multipart/form-data")==0)
		{
			form_fa->Method = METH_POST;
			form_activate_multipart(form_fa);
			return;
		}
		/* go back to 1st input */
		elem_fa = form_fa->InputList;

		do if (elem_fa->checked && *elem_fa->Name) {
			size += 2 + strlen (elem_fa->Name);
			if (elem_fa->Value) {
				char * v = elem_fa->Value, current_char; // Declare current_char for this block
				while ((current_char = *(v++)) != '\0') {
					size += (current_char == ' ' || isalnum (current_char) ? 1 : 3);
				}
			} else if (elem_fa->TextArray) {
				WCHAR * beg = elem_fa->TextArray[0];
				WCHAR * end = elem_fa->TextArray[elem_fa->TextRows] -1;
				while (beg < end) {
					char current_char_text = (char)(*(beg++)); // Declare current_char_text for this block
					size += (current_char_text == ' ' || isalnum (current_char_text) ? 1 : 3);
				}
			}
		} while ((elem_fa = elem_fa->Next) != NULL);
	}

	if (form_fa->Method == METH_POST) {
		len  = 0;
		url_fa  = form_fa->Action;
		data = malloc (size +1);
		if (size) size--;
    } else {
		len  = strlen (form_fa->Action);
		url_fa = malloc (len + size + 1);
		if (!url_fa) {
			/* Do not proceed if memory allocation fails */
			return;
		}
		strcpy (url_fa, form_fa->Action);
		data = url_fa;
		data[len] = (strchr (url_fa, '?') ? '&' : '?');
	}
	if (size) {
		char * p = data + (len > 0 ? len +1 : 0);
		size += len;
		elem_fa = form_fa->InputList;
		do if (elem_fa->checked && *elem_fa->Name) {
			size_t remaining_space = (data + size + len) - p;
			int written;

			/* This logic to remove previous values of the same name is complex */
			/* and itself a potential risk. For now, we focus on safe writing. */

			written = snprintf(p, remaining_space, "%s=", elem_fa->Name);
			if (written < 0 || (size_t)written >= remaining_space) break;
			p += written;

			if (elem_fa->Value) {
				const char * v = elem_fa->Value;
				while (*v) {
					remaining_space = (data + size + len) - p;
					if (remaining_space <= 3) break; /* Need up to 3 chars for encoding */
					if (*v == ' ') {
						*p++ = '+';
					} else if (isalnum((unsigned char)*v)) {
						*p++ = *v;
					} else {
						p += snprintf(p, remaining_space, "%%%02X", (unsigned char)*v); // Use *v directly
					}
					v++;
				}
			} else if (elem_fa->TextArray) {
				WCHAR * beg = elem_fa->TextArray[0];
				WCHAR * end = elem_fa->TextArray[elem_fa->TextRows] -1;
				while (beg < end) {
					char c = (char)(*(beg++));
					size_t remaining_space_inner = (data + size + len) - p; // Renamed to avoid shadowing
					if (remaining_space_inner <= 3) break;
					if (c == ' ') {
						*p++ = '+';
					} else if (isalnum((unsigned char)c)) {
						*p++ = c;
					} else {
						p += snprintf(p, remaining_space_inner, "%%%02X", (unsigned char)c);
					}
				}
			}

			size_t remaining_space_after_value = (data + size + len) - p; // Renamed to avoid shadowing
			if (remaining_space_after_value > 1) {
				*(p++) = '&';
			} else {
				/* Not enough space for the '&', so we must stop */
				break;
			}
		} while ((elem_fa = elem_fa->Next) != NULL);
		len = size;
	}
	data[len] = '\0';

	if (form_fa->Method != METH_POST) {
		CONTAINR target = NULL;
		CONTAINR cont = NULL;

		if (form_fa->Target && stricmp(form_fa->Target, "_hw_top") == 0) {
			HwWIND this = hwWind_byType (0);

			if (this != NULL) {
				hwWind_raise (this, TRUE);

				cont = this->Pane;
				target = containr_byName (cont, "_top");
			} else {
				target = NULL;
			}
		} else {
			target = (form_fa->Target &&
		                   stricmp (form_fa->Target, "_blank") != 0
		                   ? containr_byName (frame->Container, form_fa->Target) : NULL);
		}

		if (target) {
			cont = target;
		} else {
			cont = frame->Container;
		}

		ldr = start_page_load (cont, url_fa,loc, TRUE, NULL);
		free (url_fa);
	} else {
		POSTDATA post = new_post(data, strlen(data), strdup("application/x-www-form-urlencoded"));
		if (post)
		{
			ldr = start_page_load (frame->Container, url_fa,loc, TRUE, post);
			if (!ldr) delete_post (post);
		}
		/* TODO: add an else with an error message for user */
	}
	if (ldr) {
		if (location_equalHost (loc, ldr->Location)
	       && frame->AuthRealm && frame->AuthBasic) {
			ldr->AuthRealm = strdup (frame->AuthRealm);
			ldr->AuthBasic = strdup (frame->AuthBasic);
		}
		ldr->Referer = location_share (loc);
	}
}

/*============================================================================*/
static void
form_activate_multipart (FORM form_fam) // Renamed parameter
{
	FRAME  frame = form_fam->Frame;
	INPUT  elem_fam  = form_fam->InputList; // Renamed to avoid shadowing
	LOADER   ldr   = NULL;
	POSTDATA post  = NULL;
	FILE *current_file = NULL; // Renamed to avoid shadowing
	size_t size  = 0;
	size_t len;
	size_t boundlen;
	size_t flen;
	char *data;
	char *url_fam; // Renamed to avoid shadowing
	char boundary[39];
	char minihex[16] = "0123456789ABCDEF";
	ULONG randomized;
	WORD i;
	char *ptr;
	char *cnvstr;
	char *atari;
	WCHAR *wptr;
	char *type;

	/* build our own multipart boundary for this submitting */
	randomized = (ULONG)Random();	/* Xbios(17) -> 24bit random value */
	memset(boundary, '-', 38);
	boundary[38] = 0;
	for (i=0; i<6; i++)
	{
		boundary[32+i] = minihex[randomized & 0x0F];
		randomized >>= 4;
	}
	boundary[30] = 'H';
	boundary[31] = 'W';
	boundlen = 38;

	/* multipart posting is MIME-like formatted */
	/* it's a list of bodies containing the value of each variable */
	/* contents are not encoded, and charset of texts should be same as page */
	// Reset elem_fam to the beginning of the list
	elem_fam = form_fam->InputList;
	i = 0; // Reset i for this loop
	do
	{
		if (elem_fam->checked && *elem_fam->Name)
		{
			if ((elem_fam->Type == IT_FILE) && (elem_fam->TextArray[0]))
			{
				cnvstr = unicode_to_utf8(elem_fam->TextArray[0]);
				if (cnvstr)
				{
					if (elem_fam->Value) free(elem_fam->Value);
					elem_fam->Value = cnvstr;
				}
				/* quick hack to get filename */
				len = 0;
				wptr = elem_fam->TextArray[0];
				while(*wptr++) len++;
				atari = malloc(len+1);
				if (!atari) {
					/* If we can't allocate memory, we can't get the file size.
					 * Skip this part of the size calculation.
					 */
					size += 2; /* for CRLF */
					continue;
				}
				len = 0;
				wptr = elem_fam->TextArray[0];
				while(*wptr) { atari[len] = (char)(*wptr++); len++; }
				atari[len] = 0;
				/* "--" + boundary + CRLF */
				size += 2 + boundlen + 2;
				/* "Content-Disposition: form-data; name=\"" + Name + "\"" */
				size += 38 + strlen(elem_fam->Name) + 1;
				/* "; filename=\"" + value + "\"" + CRLF */
				size += 12 + strlen(elem_fam->Value) + 1 + 2;
				/* guess content-type, use "application/octet-stream" (24) now... */
				/* then add line "Content-Type: " (14) + strlen(cnttype) + CRLF + CRLF */
				size += 14 + 24 + 2 + 2;
				/* then add file content (raw) + CRLF */
				current_file = fopen(atari, "rb"); // Using renamed variable
				flen = 0;
				if (current_file) // Using renamed variable
				{
					fseek (current_file, 0, SEEK_END); // Using renamed variable
					flen = ftell(current_file); // Using renamed variable
					fclose(current_file); // Using renamed variable
				}
				size += flen + 2;
				free(atari);
			}
			else
			{
				/* "--" + boundary + CRLF */
				size += 2 + boundlen + 2;
				/* "Content-Disposition: form-data; name=\"" + Name + "\"" */
				size += 38 + strlen(elem_fam->Name) + 1;
				/* CRLF + CRLF + value + CRLF */
				size += 4;
				/* body is the value if not a file */
				if (elem_fam->Value)
				{
					size += strlen(elem_fam->Value);
				}
				else if (elem_fam->TextArray)
				{
					WCHAR * beg = elem_fam->TextArray[0];
					WCHAR * end = elem_fam->TextArray[elem_fam->TextRows] -1;

					size += (end-beg);	/* the - gives a number of entry, not byte */
					/*while (beg < end) {
						char c = *(beg++);
						size += (c == ' ' || isalnum (c) ? 1 : 3);
					}*/
				}
				size += 2;
			}
		}
		elem_fam = elem_fam->Next;
	}
	while (elem_fam != NULL);
	/* "--" + boundary + "--" */
	size += 2 + boundlen + 2;
	/* end of post data size computing */

	/* now malloc the buffer */
	len  = 0;
	url_fam  = form_fam->Action;
	data = malloc (size +1);
	if (!data)
	{
		/* bad luck, cannot post because not enough ram */
		return;
	}

	/* now fill that post buffer */
	elem_fam = form_fam->InputList; // Reset elem_fam to the beginning of the list for filling
	ptr = data;
	do {
		if (elem_fam->checked && *elem_fam->Name) {
			size_t remaining = (data + size) - ptr;
			int written;

			/* Part 1: Write boundary and Content-Disposition header */
			written = snprintf(ptr, remaining, "--%s\r\nContent-Disposition: form-data; name=\"%s\"",
			                 boundary, elem_fam->Name);
			if (written < 0 || (size_t)written >= remaining) break;
			ptr += written;
			remaining -= written;

			if ((elem_fam->Type == IT_FILE) && (elem_fam->Value)) {
				/* Part 2a: Write filename and Content-Type for file uploads */
				written = snprintf(ptr, remaining, "; filename=\"%s\"\r\nContent-Type: application/octet-stream\r\n\r\n", elem_fam->Value);
				if (written < 0 || (size_t)written >= remaining) break;
				ptr += written;
				remaining -= written;

				/* Part 3a: Safely read and append file content */
				current_file = fopen(elem_fam->Value, "rb"); // Using renamed variable
				if (current_file) {
					fseek(current_file, 0, SEEK_END); // Using renamed variable
					flen = ftell(current_file); // Using renamed variable
					fseek(current_file, 0, SEEK_SET); // Using renamed variable
					if (flen > 0 && flen < remaining - 2) {
						fread(ptr, 1, flen, current_file); // Using renamed variable
						ptr += flen;
						remaining -= flen;
					}
					fclose(current_file); // Using renamed variable
				}
				if (remaining > 2) {
					*ptr++ = '\r';
					*ptr++ = '\n';
				}

			} else {
				/* Part 2b: Write separator for regular form fields */
				if (remaining > 4) {
					memcpy(ptr, "\r\n\r\n", 4);
					ptr += 4;
					remaining -= 4;
				} else {
					break;
				}

				/* Part 3b: Append the value of the form field */
				if (elem_fam->Value) {
					len = strlen(elem_fam->Value);
					if (len < remaining - 2) {
						memcpy(ptr, elem_fam->Value, len);
						ptr += len;
					}
				} else if (elem_fam->TextArray) {
					WCHAR *beg = elem_fam->TextArray[0];
					WCHAR *end = elem_fam->TextArray[elem_fam->TextRows] - 1;
					while (beg < end && (remaining > 1)) {
						*ptr++ = (char)*beg++;
						remaining--;
					}
				}
				if (remaining > 2) {
					*ptr++ = '\r';
					*ptr++ = '\n';
				}
			}
		}
		elem_fam = elem_fam->Next;
	} while (elem_fam != NULL);
	/* "--" + boundary + "--" */
	snprintf(ptr, (data + size) - ptr, "--%s--", boundary);
	ptr += 4 + boundlen;
	size = ptr - data;
	/* new send request with Content-Length: %ld\r\nContent-Type: multipart/form-data; boundary=%s\r\n */
	form_fam->Method = METH_POST; /* with file, nothing else possible */

	type = malloc(50+strlen(boundary));
	if (!type) {
		/* Handle memory allocation failure */
		delete_post(new_post(data, size, NULL)); /* Free data since we can't proceed */
		return;
	}
	if (type)
	{
		sprintf(type, "multipart/form-data; boundary=%s", boundary);
	}
	post = new_post(data, size, type);
	ldr = start_page_load (frame->Container, url_fam, frame->Location, TRUE, post);
	if (!ldr)
	{
		/* post already deleted in start_page_load */
	}
	return;
}
/*============================================================================*/
/* A routine that could be broken into a wrapper for 2 or 3 small util routines */
static void
input_file_handler (INPUT input_ifh) { // Renamed parameter
	char fsel_file[HW_PATH_MAX] = "";
	FORM   form = input_ifh->Form;
	FRAME frame = form->Frame;
	HwWIND wind = hwWind_byContainr(frame->Container);

	if (file_selector ("HighWire: Select File to Upload", NULL,
	                   fsel_file, fsel_file,sizeof(fsel_file))) {
		INPUT field = input_ifh->u.FileEd;

		form->TextActive = field;

		edit_feed (field, frame->Encoding, fsel_file, strchr(fsel_file, '\0'));

		/* to be overworked, probably the same scheme as for radio buttons */
		{
			WORDITEM word = field->Word;
			GRECT clip;
			long  x, y;
			clip.g_x = 0;
			clip.g_w = word->word_width;
			clip.g_y = -word->word_height;
			clip.g_h = word->word_height + word->word_tail_drop;

			dombox_Offset (&word->line->Paragraph->Box, &x, &y);
			x += (long)clip.g_x + word->h_offset
			     + frame->clip.g_x - frame->h_bar.scroll;
			y += (long)clip.g_y + word->line->OffsetY
			     + frame->clip.g_y - frame->v_bar.scroll;

			clip.g_x = (WORD)x;
			clip.g_y = (WORD)y;
			hwWind_redraw (wind, &clip);
		}
	}
}

/*============================================================================*/
WORDITEM
input_activate (INPUT input_ia, WORD slct) // Renamed parameter
{
	FORM form = input_ia->Form;

	if (input_ia->Type >= IT_TEXT) {
		WORDITEM word;
		if (slct >= 0) {
			word = NULL;
		} else {
			word = input_ia->Word;
			if (input_ia == form->TextActive) form->TextActive = NULL;
		}
		return word;
	}

	if (input_ia->Type == IT_SELECT) {
		if (slct >= 0) {
			SELECT   sel  = input_ia->u.Select;
			SLCTITEM item = sel->ItemList;
			while (++slct < sel->NumItems && item->Next) {
				item = item->Next;
			}
			input_ia->Word->item   = item->Text;
			input_ia->Word->length = item->Length;
			input_ia->Value        = item->Value;
		}
		return input_ia->Word;
	}

	if (input_ia->Type != IT_BUTTN) {
		return NULL;
	}

	if (input_ia->SubType == 'F') {
		input_file_handler (input_ia);
	}

	if (input_ia->SubType == 'S' && form->Action) {
		form_activate (form);
	}

	input_ia->checked = FALSE;

	return input_ia->Word;
}


/*============================================================================
 ** Changes
 ** Author         Date           Desription
 ** P Slegg        14-Aug-2009    Utilise NKCC from cflib to handle the various control keys in text fields.
 ** P Slegg        18-Mar-2010    input_keybrd: Add Ctrl-Delete and Ctrl-Backspace edit functions
 **
 */
WORDITEM
input_keybrd (INPUT input_ik, WORD key, UWORD kstate, GRECT * rect, INPUT * next) // Renamed parameter
{
	FORM     form = input_ik->Form;
	WORDITEM word = input_ik->Word;
	WCHAR ** text = input_ik->TextArray;
	WORD     ascii_code;
	WORD     scrl = 0;
	WORD     lift = 0;
	WORD     line = 0;
	UWORD    nkey;
	BOOL     shift, ctrl;

	/* Convert the GEM key code to the "standard" */
	nkey = gem_to_norm ((short)kstate, (short)key);

	/* Remove the unwanted flags */
	nkey &= ~(NKF_RESVD|NKF_SHIFT|NKF_CTRL|NKF_CAPS);

	ascii_code =  nkey & 0x00FF;

	shift = (kstate & (K_RSHIFT|K_LSHIFT)) != 0;
	ctrl  = (kstate & K_CTRL) != 0;

	if (input_ik != (*next = form->TextActive))
	{
		return NULL;   /* shouldn't happen but who knows... */
	}

	if (!(nkey & NKF_FUNC))
	{
		if (!input_ik->readonly
				&& edit_char (input_ik, form->TextCursrX, form->TextCursrY, ascii_code))
		{
			scrl = +1;
			line = 1;
		}
		else
		{
			word = NULL;
		}
	}
	else
	{
		nkey &= ~NKF_FUNC;

		switch (nkey)
		{
			case NK_UP:
				if (form->TextCursrY)
				{
					WORD curPos;
					lift = -1;
					line = +2;
					curPos = form->TextCursrX;  /* current cursor position */

					if (curPos > edit_rowln (input_ik, form->TextCursrY -1) )
					{  /* cursor pos is greater than length of previous line */
						scrl = (WORD)(-(curPos - edit_rowln (input_ik, form->TextCursrY -1) ));  /* move cursor left, to end of line */
					}
				}
				else
				{
					word = NULL;
				}
				break;

			case NK_DOWN:
				if (form->TextCursrY < input_ik->TextRows -1)
				{
					lift = +1;
					line = -2;
				}
				else
				{
					word = NULL;
				}
				break;

			case NK_LEFT:  /* cursor-left */
				if (shift)  /* shift cursor-left: 52 */
				{
					/* Move cursor to the left by scrl places */
					scrl = -form->TextCursrX;
					if (scrl >= 0)
					{
						word = NULL;
					}
					else
					{
						line = 1;
					}
				}
				else if (ctrl)  /* control left */
				{								/* move cursor one word to the left */
					WCHAR * beg_ik; // Renamed
					WCHAR * end_ik; // Renamed
					int      numChars;

					beg_ik = input_ik->TextArray[form->TextCursrY];         /* beginning of line    */
					end_ik = beg_ik + form->TextCursrX;                     /* cursor point in line */

					scrl = 0;
					numChars = ctrl_left (beg_ik, end_ik);

					end_ik = end_ik - numChars;
					scrl = scrl - numChars;

					line = 1;
				}
				else  /* cursor-left */
				{
					if (form->TextCursrX)
					{
						scrl = -1;
						line = 1;
					}
					else if (form->TextCursrY)
					{
						scrl = (WORD)edit_rowln (input_ik, form->TextCursrY -1);
						lift = -1;
						line = +2;
					}
					else
					{
						word = NULL;
					}
				}
				break;

			case NK_RIGHT:  /*cursor-right */
				if (shift)  /* shift cursor-right: 54 */
				{
					/* Move cursor to the right by scrl places */
					scrl = (WORD)edit_rowln (input_ik, form->TextCursrY) - form->TextCursrX;
					if (scrl <= 0)
					{
						word = NULL;
					}
					else
					{
						line = 1;
					}
				}
				else if (ctrl)  /* control cursor-right */
				{               /* move cursor one word to the right */
					WCHAR * beg_ik; // Renamed
					WCHAR * end_ik; // Renamed
					int      numChars;

					beg_ik = input_ik->TextArray[form->TextCursrY] + form->TextCursrX;  /* find cursor position */
					end_ik = input_ik->TextArray[form->TextCursrY+1] - 1;               /* find end of row */

					scrl = 0;
					numChars = ctrl_right (beg_ik, end_ik);

					end_ik = end_ik + numChars;
					scrl = scrl + numChars;

					if (scrl <= 0)
					{
						word = NULL;
					}
					else
					{
						line = 1;
					}

				}
				else  /* cursor-right */
				{
					if (form->TextCursrX < edit_rowln (input_ik, form->TextCursrY))
					{
						scrl = +1;
						line = 1;
					}
					else if (form->TextCursrY < input_ik->TextRows -1)
					{
						scrl = -form->TextCursrX;
						lift = +1;
						line = -2;
					}
					else
					{
						word = NULL;
					}
				}
				break;

			case NK_ESC:  /* Escape:  27 */
			{
				WCHAR  * last = text[input_ik->TextRows];
				if (!input_ik->readonly && text[0] < last -1 && edit_zero (input_ik))
				{
					form->TextCursrX = 0;
					form->TextCursrY = 0;
					form->TextShiftX = 0;
					form->TextShiftY = 0;
				}
				else
				{
					word = NULL;
				}
			}	break;

			case NK_RET:  /* Enter/Return: 13 */
			case (NK_ENTER|NKF_NUM):
				if (input_ik->Type == IT_TAREA)
				{
					if (edit_crlf (input_ik, form->TextCursrX, form->TextCursrY))
					{
						scrl = -form->TextCursrX;
						lift = +1;
					}
					else
					{
						word = NULL;
					}
				}
				else if (edit_rowln (input_ik, form->TextCursrY) && (key & 0xFF00))
				{
					form_activate (form);
					form->TextActive = NULL;
				}
				else
				{
					word = NULL;
				}
				break;

			case NK_CLRHOME:  /* Clr/Home: 55 */
				if (shift)  /* Clr-Home shifted */
				{
					scrl = (WORD)edit_rowln (input_ik, input_ik->TextRows -1) - form->TextCursrX;
					lift =                    input_ik->TextRows -1  - form->TextCursrY;
					if (!scrl && !lift)
					{
						word = NULL;
					}
				}
				else  /* Clr-Home unshifted */
				{
					if (form->TextCursrX || form->TextCursrY)
					{
						scrl = -form->TextCursrX;
						lift = -form->TextCursrY;
					}
					else
					{
						word = NULL;
					}
				}
				break;

			case NK_M_END:  /* Mac END key */
				scrl = (WORD)edit_rowln (input_ik, input_ik->TextRows -1) - form->TextCursrX;
				lift =                    input_ik->TextRows -1  - form->TextCursrY;
				if (!scrl && !lift)
				{
					word = NULL;
				}
				break;

			case NK_TAB:  /* Tab: 9 */
				if (ctrl)  /* Tab control */
				{
					form->TextActive = NULL;
					*next            = NULL;
				}
				else if (shift)
				{
					INPUT srch = form->InputList;
					INPUT last = NULL;

					if (srch)
					{
						while (1)
						{
							while ((srch->disabled) && (srch = srch->Next) != NULL) ;

							if (srch->Next == input_ik)
							{
								if (srch->Type == IT_TEXT)
								{
									last = srch;
								}
								break;
							}
							else
							{
								if (srch->Type == IT_TEXT)
								{
									last = srch;
								}
								srch = srch->Next;
							}
						}
					}

					if (!last)
					{
						srch = input_ik->Next;

						while ((srch = srch->Next) != NULL)
						{
							if (!srch->disabled && srch->Type == IT_TEXT)
							{
								last = srch;
							}
						}
					}

					/* on google it would move out of the form */
					if (!last) last = input_ik;

					*next = last;
				}
				else
				{
					INPUT srch = input_ik->Next;
					if (srch)
					{
						while ((srch->disabled || srch->Type < IT_TEXT)
									 && (srch = srch->Next) != NULL);
					}
					if (!srch)
					{
						srch = form->InputList;
						while ((srch->disabled || srch->Type < IT_TEXT)
									 && (srch = srch->Next) != NULL);
					}

					*next = srch;
				}
				break;

			case NK_BS:  /* Backspace: 8 */
				if (input_ik->readonly)
				{
					word = NULL;
				}
				else if (form->TextCursrX)
				{
					if (shift)
					{
						int col = form->TextCursrX;

						if (col > 0)
						{
							scrl = scrl - col;
							del_chars (input_ik, col, form->TextCursrY, -col);
							line = 1;
						}
					}
					else if (ctrl)  /* Ctrl Backspace */
					{
						WCHAR * beg_ik; // Renamed
						WCHAR * end_ik; // Renamed
						int col = form->TextCursrX;
						int      numChars;

						beg_ik = input_ik->TextArray[form->TextCursrY];         /* beginning of line    */
						end_ik = beg_ik + form->TextCursrX;                     /* cursor point in line */

						numChars = ctrl_left (beg_ik, end_ik);
						del_chars (input_ik, col, form->TextCursrY, -numChars);

						end_ik = end_ik - numChars;
						scrl = scrl - numChars;
						line = 1;
					}
					else
					{
						edit_delc (input_ik, form->TextCursrX -1, form->TextCursrY);
						scrl = -1;
						line = 1;
					}
				}
				else if (form->TextCursrY)
				{
					WORD col = (WORD)edit_rowln (input_ik, form->TextCursrY -1);
					edit_delc (input_ik, col, form->TextCursrY -1);
					scrl = col;
					lift = -1;
				}
				else
				{
					word = NULL;
				}
				break;

			case NK_DEL:  /* Delete: 127 */
				if (input_ik->readonly)
				{
					word = NULL;
				}
				else
				{
					if (shift)  /* Shift Delete */
					{
					int col = form->TextCursrX;
					int numChars = (int)edit_rowln (input_ik, form->TextCursrY) - col - 1;

						if (numChars > 0)
						{
							del_chars (input_ik, col, form->TextCursrY, numChars);
							line = 1;
						}
						else if (!edit_delc (input_ik, form->TextCursrX, form->TextCursrY))
						{
							word = NULL;
						}
					}
					else if (ctrl)  /* Ctrl Delete */
					{
						WCHAR * beg_ik; // Renamed
						WCHAR * end_ik; // Renamed
						int col = form->TextCursrX;
						int      numChars;

						beg_ik = input_ik->TextArray[form->TextCursrY] + form->TextCursrX;       /* cursor point in line */
						end_ik = input_ik->TextArray[form->TextCursrY+1] - 1;                    /* end of line    */

						numChars = ctrl_right (beg_ik, end_ik);
						del_chars (input_ik, col, form->TextCursrY, numChars);

						end_ik = end_ik - numChars;
						line = 1;
					}


					else
					{
						if (form->TextCursrX < edit_rowln (input_ik, form->TextCursrY))
						{
							edit_delc (input_ik, form->TextCursrX, form->TextCursrY);
							line = 1;
						}
						else if (!edit_delc (input_ik, form->TextCursrX, form->TextCursrY))
						{
							word = NULL;
						}
					}
				}
				break;

			case 'C':
				if (ctrl)
				{
					int m,n;
					/* printf ("ctrl-c\n"); */
					for (n = 0; n <= input_ik->TextRows; n++)
					{
						for (m = 0; m <= edit_rowln (input_ik, n); m++)
						{
							// This block was empty in the original code,
							// but the printf was commented out.
							// Re-adding a printf for illustration purposes,
							// though it might not be what the original intent was.
							// Ensure 'edit_rowln' correctly handles the size of the array.
							// if (input_ik->TextArray[n] && m < (int)(input_ik->TextArray[n+1] - input_ik->TextArray[n] - 1)) {
							// 	printf("%wc", input_ik->TextArray[n][m]);
							// }
						}
					}
				}
				break;

			default:
				word = NULL;

		}  /* switch */
	}  /* if  */


	if (word)
	{
		form->TextCursrY += lift;
		if (!scrl && edit_rowln (input_ik, form->TextCursrY) < form->TextCursrX)
		{
			scrl = (WORD)edit_rowln (input_ik, form->TextCursrY) - form->TextCursrX;
		}
		if (lift > 0)
		{  /* cursor down */
			if (form->TextShiftY < form->TextCursrY - (WORD)(input_ik->VisibleY -1))
			{
				form->TextShiftY = form->TextCursrY - (WORD)input_ik->VisibleY -1; // Corrected: removed extra parenthesis
				line = 0;
			}
		}
		else
		{
			if (form->TextShiftY > form->TextCursrY)
			{
				form->TextShiftY = form->TextCursrY;
				line = 0;
			}
			else if (form->TextShiftY &&
								 input_ik->TextRows < form->TextShiftY + input_ik->VisibleY)
			{
				form->TextShiftY = input_ik->TextRows - (WORD)input_ik->VisibleY;
				line = 0;
			}
		}
		form->TextCursrX += scrl;
		if (scrl > 0)
		{
			if (form->TextShiftX < form->TextCursrX - (WORD)input_ik->VisibleX)
			{
				form->TextShiftX = form->TextCursrX - (WORD)input_ik->VisibleX;
				line = 0;
			}
		}
		else
		{  /* cursor up, cursor down */
			if (form->TextShiftX > form->TextCursrX)
			{
				form->TextShiftX = form->TextCursrX;
				line = 0;
			}
			else if (form->TextShiftX)
			{
				WORD n = (WORD)edit_rowln (input_ik, form->TextCursrY);
				if (n < form->TextShiftX + input_ik->VisibleX)
				{
					form->TextShiftX = n - (WORD)input_ik->VisibleX;
					line = 0;
				}
			}
		}
/*printf ("%i %i %i\n", form->TextShiftX, form->TextCursrX, input_ik->VisibleX);*/
		rect->g_x = 2;
		rect->g_y = 2 - word->word_height;
		rect->g_w = word->word_width -4;
		if (!line)
		{
			rect->g_h = word->word_height + word->word_tail_drop -4;
		}
		else
		{  /* cursor up, cursor down */
			WORD row = form->TextCursrY - form->TextShiftY;
			if (line < 0)
			{  /* cursor down */
				row -= 1;
				line = 2;
			}
			rect->g_y += input_ik->CursorH * row;
			rect->g_h =  input_ik->CursorH * line +1;
		}
	}
	return word;
}


/*******************************************************************************
 *
 * Edit field functions
 *
 * ||W0|W1|W2|...|\0||...(free)...|| L0 | L1 | L2 |...|Ln-1| Ln ||
*/

/*----------------------------------------------------------------------------*/
static BOOL
edit_grow (INPUT input)
{
	size_t  o_num = (WCHAR*)&input->TextArray[0] - (WCHAR*)input->Word->item;
	size_t  n_num = o_num +500;
	WORD    rows  = input->TextRows +1;
	size_t  size  = (n_num * sizeof(WCHAR) + rows * sizeof(WCHAR*) +3) & ~3uL;
	WCHAR * buff;

	if (input->TextMax) {
		return FALSE;
	}
	if (input->Value) {
		char * mem = malloc (n_num +1);
		if (!mem) {
			return FALSE;
		}
		memcpy (mem, input->Value, o_num +1);
		free (input->Value);
		input->Value = mem;
	}
	if ((buff = malloc (size)) != NULL) {
		WCHAR ** arr = (WCHAR**)((char*)buff + size) - rows;
		WORD     n   = 0;
		while (n < rows) {
			arr[n] = &buff[input->TextArray[n] - input->Word->item];
			n++;
		}
		memcpy (buff, input->Word->item, o_num * sizeof(WCHAR));
		free (input->Word->item);
		input->Word->item = buff;
		input->TextArray  = arr;
	}
	return (buff != NULL);
}

/*----------------------------------------------------------------------------*/
static WCHAR *
edit_init (INPUT input, TEXTBUFF current, UWORD cols, UWORD rows, size_t size)
{
	WORDITEM word = current->word;
	TEXTATTR attr = word->attr;
	void   * buff;
	WORD     p[8], n;

	font_byType (pre_font, -1, -1, word);
	if (word->font->Base->Mapping != MAP_UNICODE) {
		vst_map_mode (vdi_handle, MAP_UNICODE);
	}
	for (n = 0; n < cols; current->text[n++] = NOBRK_UNI);
	vqt_f_extent16n (vdi_handle, word->item, cols, p);
	*(current->text++) = word->font->Base->SpaceCode;
	set_word (current, word->word_height, word->word_tail_drop, p[2] - p[0] +2);
	if (word->font->Base->Mapping != MAP_UNICODE) {
		vst_map_mode (vdi_handle, word->font->Base->Mapping);
	}
	current->word->attr = attr;

	size += 1;                 /* space for the trailing 0 */
	size *= sizeof(WCHAR);
	size =  (size +1) & ~1uL;  /* aligned to (WCHAR*) boundary */
	size += sizeof(WCHAR*) *2;
	if ((buff = malloc (size)) != NULL) {
		word->item       = buff;
		input->TextArray = (WCHAR**)((char*)buff + size) -1;

	} else { /* memory exhausted */
		input->TextMax  = 1;
		input->readonly = TRUE;
	}
	edit_zero (input);

	input->VisibleX = cols;
	input->VisibleY = rows;
	input->CursorH  = p[7] + p[1] -1;

	if (rows > 1) {
		word->word_height += (rows-1) * input->CursorH;
	}

	return buff;
}

/*----------------------------------------------------------------------------*/
static BOOL
edit_zero (INPUT input)
{
	BOOL ok;
	if (input->TextArray) {
		input->TextArray    += (WORD)input->TextRows -1;
		input->TextRows      = 1;
		input->TextArray[0]  = input->Word->item;
		input->TextArray[1]  = input->Word->item +1;
		input->Word->item[0] = '\0';
		ok = TRUE;
	} else {
		ok = FALSE;
	}
	if (input->Value) {
		input->Value[0] = '\0';
	}
	return ok;
}

/*----------------------------------------------------------------------------*/
static void
edit_feed (INPUT input, ENCODING encoding, const char * beg, const char * end)
{
	WCHAR ** line = input->TextArray +1;
	WCHAR * ptr  = input->TextArray[0];
	BOOL     crlf = (input->Type == IT_TAREA);
	*line = ptr;
	while (beg < end) {
		if (ptr >= (WCHAR*)&input->TextArray[0] -1) {
			*line = ptr;
			if (!edit_grow (input)) {
				beg = end;
				break;
			}
			line = &input->TextArray[input->TextRows];
			ptr  = *line;
		}
		if (*beg == '\n' && crlf) {
			WORD col = (WORD)(ptr - *line);
			*line    = ptr;
			if (!edit_crlf (input, col, input->TextRows -1)) {
				beg = end;
				break;
			}
			line = &input->TextArray[input->TextRows];
			ptr  = *line;
			beg++;
		} else if (*beg < ' ') {
			beg++;
		} else if (*beg == '&') {
			WCHAR tmp[5];
			scan_namedchar (&beg, tmp, TRUE, MAP_UNICODE);
			*(ptr++) = *tmp;
		} else {
			*(ptr++) = *(beg++);
		}
	}
	*(ptr++) = '\0';
	*(line)  = ptr; /* behind the last line */

	if (input->Value) { /* password */
		char * val = input->Value;
		ptr = input->Word->item;
		while ((*(val++) = *(ptr)) != '\0') {
			*(ptr++) = '*';
		}
	}

	(void)encoding;
}

/*----------------------------------------------------------------------------*/
static BOOL
edit_crlf (INPUT input, WORD col, WORD row)
{
	WCHAR * dst, * end;
	WORD    n;

	if (edit_space (input) < (sizeof(WCHAR) + sizeof(WCHAR*)) / sizeof(WCHAR)
	    && !edit_grow (input)) {
		return FALSE;
	}

	input->TextArray--;
	input->TextRows++;
	for (n = 0; n <= row; n++) {
		input->TextArray[n] = input->TextArray[n+1];
	}
	input->TextArray[++row] += col;
	for (n = row; n <= input->TextRows; n++) {
		input->TextArray[n]++;
	}
	dst = input->TextArray[row] -1;
	end = input->TextArray[input->TextRows];
	while (--end > dst) {
		*(end) = *(end -1);
	}
	*(dst) = '\n';

	return TRUE;
}

/*----------------------------------------------------------------------------*/
static BOOL
edit_char (INPUT input, WORD col, WORD row, WORD chr)
{
	ENCODER_W encoder = encoder_word (ENCODING_ATARIST, MAP_UNICODE);
	const char  * ptr = (const char *)&chr; // Corrected: chr is a WORD, not char array
	BOOL ok;
	WCHAR uni_char[2]; // Renamed to avoid shadowing 'uni' in nested block

	if (edit_space (input) > 0 || edit_grow (input)) {
		WCHAR ** line = input->TextArray;
		WORD  n;
		// WCHAR uni[5]; // This was shadowing. Changed to uni_char above.
		(*encoder)((const char**)&ptr, uni_char); // Cast to const char**

		if (input->Value) { /*password */
			UWORD  len = edit_rowln (input, row);
			char * end_val = input->Value + len; // Renamed to avoid shadowing 'end' from above
			char * dst_val = input->Value + col; // Renamed to avoid shadowing 'dst' from above
			do {
				*(end_val +1) = *(end_val);
			} while (--end_val >= dst_val);
			*dst_val = (char)*uni_char; // Cast to char for input->Value
			input->Word->item[len]    = '*';
			input->Word->item[len +1] = '\0';

		} else {
			WCHAR * end_char = line[input->TextRows]; // Renamed to avoid shadowing 'end'
			WCHAR * dst_char = line[row] + col; // Renamed to avoid shadowing 'dst'
			while (--end_char >= dst_char) {
				end_char[1] = end_char[0];
			}
			*dst_char = *uni_char;
		}
		for (n = row +1; n <= input->TextRows; line[n++]++);
		ok = TRUE;

	} else { /* buffer full */
		ok = FALSE;
	}
	return ok;
}

/*----------------------------------------------------------------------------*/
static BOOL
edit_delc (INPUT input, WORD col, WORD row)
{
	WCHAR ** text = input->TextArray;
	WCHAR  * beg  = text[row] + col;
	WCHAR  * end  = text[input->TextRows];
	BOOL ok;

	if (beg < end -1) {
		WORD n;

		if (input->Value) { /*password */
			char * ptr = input->Value + col;
			do {
				*(ptr) = *(ptr +1);
			} while (*(ptr++));
			*(--end) = '\0';

		} else {
			if (beg >= text[row +1] -1) { /* at the end of this row, merge */
				n    =  row;               /* with the following one        */
				WCHAR **temp_text = text; // Temporary pointer for iteration
				temp_text += row +2;
				for (n = row; n >= 0; *(--temp_text) = input->TextArray[n--]);
				input->TextArray++;
				input->TextRows--;
			}
			do {
				*(beg) = *(beg +1);
			} while (*(beg++));
		}
		for (n = row; n < input->TextRows; text[++n]--);
		ok = TRUE;

	} else {
		ok = FALSE;
	}
	return ok;
}

/*----------------------------------------------------------------------------*/
static void
del_chars (INPUT input, WORD col, WORD row, int numChars)
{
  /* Delete characters to left (-ve numChars) or right (+ve numChars) of position col */
	if (numChars < 0)
	{
		numChars = abs(numChars);

		while (numChars > 0)
		{
			edit_delc (input, col-1, row);
			col--;
			numChars--;
		}  /* while */
	}
	else
	{
		// Corrected: loop should run exactly numChars times.
		while (numChars > 0)
		{
			edit_delc (input, col, row);
			numChars--;
		}  /* while */
	}
}

/*----------------------------------------------------------------------------*/
static int
ctrl_left (WCHAR * beg, WCHAR * end)
{
	int numChars = 0;

	/* Ignore the leading spaces to the left of the cursor */
	while (beg < end
	       && iswspace(*(end-1)) )
	{
		end--;
		numChars++;
	}

	/* Find the next space to the left of the cursor or word */
	while (beg < end
	       &&  ! iswspace(*(end-1)) )
	{
		end--;
		numChars++;
	}
	return numChars;
}

/*----------------------------------------------------------------------------*/
static int
ctrl_right (WCHAR * beg, WCHAR * end)
{
	int numChars = 0;

	/* ignore the leading spaces to the right of the cursr */
	while (beg < end
	       &&   iswspace(*beg) )
	{
		beg++;
		numChars++;
	}

	/* Find the next space to the right of the cursor or word */
	while (beg < end
	       &&  (! iswspace(*beg)) )
	{
		beg++;
		numChars++;
	}
	return numChars;
}
