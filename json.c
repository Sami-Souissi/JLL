/* vim: set et ts=3 sw=3 sts=3 ft=c: */

#include "json.h"

#ifdef _MSC_VER
   #ifndef _CRT_SECURE_NO_WARNINGS
      #define _CRT_SECURE_NO_WARNINGS
   #endif
#endif
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <errno.h>
#define FILENAME_SIZE 1024
#define MAX_LINE 2048
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_RESET   "\x1b[0m"
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_CYAN    "\x1b[36m"

#define BUFFER_SIZE 1000
#include <stdarg.h>


#ifdef _MSC_VER
#define inline _inline
#endif

#ifdef TRACING_ENABLE
#include <stdio.h>
#define TRACING(fmt, ...)	fprintf(stderr, "tracing: " fmt, ##__VA_ARGS__)
#else
#define TRACING(fmt, ...)	((void) 0)
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
   /* C99 might give us uintptr_t and UINTPTR_MAX but they also might not be provided */
   #include <stdint.h>
#endif

#ifndef JSON_INT_T_OVERRIDDEN
   #if defined(_MSC_VER)
      /* https://docs.microsoft.com/en-us/cpp/cpp/data-type-ranges */
      #define JSON_INT_MAX 9223372036854775807LL
   #elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
      /* C99 */
      #define JSON_INT_MAX INT_FAST64_MAX
   #else
      /* C89 */
      #include <limits.h>
      #define JSON_INT_MAX LONG_MAX
   #endif
#endif

#ifndef JSON_INT_MAX
#define JSON_INT_MAX (json_int_t)(((unsigned json_int_t)(-1)) / (unsigned json_int_t)2);
#endif
extern int errno ;
typedef unsigned int json_uchar;

const struct _json_value json_value_none;

static unsigned char hex_value (json_char c)
{
   if (isdigit((unsigned char)c))
      return c - '0';

   switch (c) {
      case 'a': case 'A': return 0x0A;
      case 'b': case 'B': return 0x0B;
      case 'c': case 'C': return 0x0C;
      case 'd': case 'D': return 0x0D;
      case 'e': case 'E': return 0x0E;
      case 'f': case 'F': return 0x0F;
      default: return 0xFF;
   }
}

static int would_overflow (json_int_t value, json_char b)
{
   return ((JSON_INT_MAX - (b - '0')) / 10 ) < value;
}

typedef struct
{
   size_t used_memory;

   json_settings settings;
   int first_pass;

   const json_char * ptr;
   unsigned int cur_line, cur_col;

} json_state;

static void * default_alloc (size_t size, int zero, void * user_data)
{
   (void)user_data; /* ignore unused-parameter warn */
   return zero ? calloc (1, size) : malloc (size);
}

static void default_free (void * ptr, void * user_data)
{
   (void)user_data; /* ignore unused-parameter warn */
   free (ptr);
}

static void * json_alloc (json_state * state, size_t size, int zero)
{
   if ((ULONG_MAX - 8 - state->used_memory) < size)
      return 0;

   if (state->settings.max_memory
         && (state->used_memory += size) > state->settings.max_memory)
   {
      return 0;
   }

   return state->settings.mem_alloc (size, zero, state->settings.user_data);
}

static int new_value (json_state * state,
                      json_value ** top, json_value ** root, json_value ** alloc,
                      json_type type)
{
   json_value * value;
   size_t values_size;

   if (!state->first_pass)
   {
      value = *top = *alloc;
      *alloc = (*alloc)->_reserved.next_alloc;

      if (!*root)
         *root = value;

      switch (value->type)
      {
         case json_array:

            if (value->u.array.length == 0)
               break;

            if (! (value->u.array.values = (json_value **) json_alloc
               (state, value->u.array.length * sizeof (json_value *), 0)) )
            {
               return 0;
            }

            value->u.array.length = 0;
            break;

         case json_object:

            if (value->u.object.length == 0)
               break;

            values_size = sizeof (*value->u.object.values) * value->u.object.length;

            if (! (value->u.object.values = (json_object_entry *) json_alloc
               #ifdef UINTPTR_MAX
                  (state, values_size + ((uintptr_t) value->u.object.values), 0)) )
               #else
                  (state, values_size + ((size_t) value->u.object.values), 0)) )
               #endif
            {
               return 0;
            }

            value->_reserved.object_mem = (void *) (((char *) value->u.object.values) + values_size);

            value->u.object.length = 0;
            break;

         case json_string:

            if (! (value->u.string.ptr = (json_char *) json_alloc
               (state, (value->u.string.length + 1) * sizeof (json_char), 0)) )
            {
               return 0;
            }

            value->u.string.length = 0;
            break;

         default:
            break;
      };

      return 1;
   }

   if (! (value = (json_value *) json_alloc
         (state, sizeof (json_value) + state->settings.value_extra, 1)))
   {
      return 0;
   }

   if (!*root)
      *root = value;

   value->type = type;
   value->parent = *top;

   #ifdef JSON_TRACK_SOURCE
      value->line = state->cur_line;
      value->col = state->cur_col;
   #endif

   if (*alloc)
      (*alloc)->_reserved.next_alloc = value;

   *alloc = *top = value;

   return 1;
}

#define whitespace \
   case '\n': ++ state.cur_line;  state.cur_col = 0; /* FALLTHRU */ \
   case ' ': /* FALLTHRU */ case '\t': /* FALLTHRU */ case '\r'

#define string_add(b)  \
   do { if (!state.first_pass) string [string_length] = b;  ++ string_length; } while (0);

#define line_and_col \
   state.cur_line, state.cur_col

static const long
   flag_next             = 1 << 0,
   flag_reproc           = 1 << 1,
   flag_need_comma       = 1 << 2,
   flag_seek_value       = 1 << 3,
   flag_escaped          = 1 << 4,
   flag_string           = 1 << 5,
   flag_need_colon       = 1 << 6,
   flag_done             = 1 << 7,
   flag_num_negative     = 1 << 8,
   flag_num_zero         = 1 << 9,
   flag_num_e            = 1 << 10,
   flag_num_e_got_sign   = 1 << 11,
   flag_num_e_negative   = 1 << 12,
   flag_line_comment     = 1 << 13,
   flag_block_comment    = 1 << 14,
   flag_num_got_decimal  = 1 << 15;

json_value * json_parse_ex (json_settings * settings,
                            const json_char * json,
                            size_t length,
                            char * error_buf)
{
   char error [json_error_max];
   const json_char * end;
   json_value * top, * root, * alloc = 0;
   json_state state = { 0 };
   long flags = 0;
   int num_digits = 0;
   double num_e = 0, num_fraction = 0;

   /* Skip UTF-8 BOM
    */
   if (length >= 3 && ((unsigned char) json [0]) == 0xEF
                   && ((unsigned char) json [1]) == 0xBB
                   && ((unsigned char) json [2]) == 0xBF)
   {
      json += 3;
      length -= 3;
   }

   error[0] = '\0';
   end = (json + length);

   memcpy (&state.settings, settings, sizeof (json_settings));

   if (!state.settings.mem_alloc)
      state.settings.mem_alloc = default_alloc;

   if (!state.settings.mem_free)
      state.settings.mem_free = default_free;

   for (state.first_pass = 1; state.first_pass >= 0; -- state.first_pass)
   {
      json_uchar uchar;
      unsigned char uc_b1, uc_b2, uc_b3, uc_b4;
      json_char * string = 0;
      unsigned int string_length = 0;

      top = root = 0;
      flags = flag_seek_value;

      state.cur_line = 1;

      for (state.ptr = json ;; ++ state.ptr)
      {
         json_char b = (state.ptr == end ? 0 : *state.ptr);

         if (flags & flag_string)
         {
            if (!b)
            {  sprintf (error, "%u:%u: Unexpected EOF in string", line_and_col);
               goto e_failed;
            }

            if (string_length > UINT_MAX - 8)
               goto e_overflow;

            if (flags & flag_escaped)
            {
               flags &= ~ flag_escaped;

               switch (b)
               {
                  case 'b':  string_add ('\b');  break;
                  case 'f':  string_add ('\f');  break;
                  case 'n':  string_add ('\n');  break;
                  case 'r':  string_add ('\r');  break;
                  case 't':  string_add ('\t');  break;
                  case 'u':

                    if (end - state.ptr <= 4 ||
                        (uc_b1 = hex_value (*++ state.ptr)) == 0xFF ||
                        (uc_b2 = hex_value (*++ state.ptr)) == 0xFF ||
                        (uc_b3 = hex_value (*++ state.ptr)) == 0xFF ||
                        (uc_b4 = hex_value (*++ state.ptr)) == 0xFF)
                    {
                        sprintf (error, "%u:%u: Invalid character value `%c`", line_and_col, b);
                        goto e_failed;
                    }

                    uc_b1 = (uc_b1 << 4) | uc_b2;
                    uc_b2 = (uc_b3 << 4) | uc_b4;
                    uchar = (uc_b1 << 8) | uc_b2;

                    if ((uchar & 0xF800) == 0xD800) {
                        json_uchar uchar2;

                        if (end - state.ptr <= 6 || (*++ state.ptr) != '\\' || (*++ state.ptr) != 'u' ||
                            (uc_b1 = hex_value (*++ state.ptr)) == 0xFF ||
                            (uc_b2 = hex_value (*++ state.ptr)) == 0xFF ||
                            (uc_b3 = hex_value (*++ state.ptr)) == 0xFF ||
                            (uc_b4 = hex_value (*++ state.ptr)) == 0xFF)
                        {
                            sprintf (error, "%u:%u: Invalid character value `%c`", line_and_col, b);
                            goto e_failed;
                        }

                        uc_b1 = (uc_b1 << 4) | uc_b2;
                        uc_b2 = (uc_b3 << 4) | uc_b4;
                        uchar2 = (uc_b1 << 8) | uc_b2;

                        uchar = 0x010000 | ((uchar & 0x3FF) << 10) | (uchar2 & 0x3FF);
                    }

                    if (sizeof (json_char) >= sizeof (json_uchar) || (uchar <= 0x7F))
                    {
                       string_add ((json_char) uchar);
                       break;
                    }

                    if (uchar <= 0x7FF)
                    {
                        if (state.first_pass)
                           string_length += 2;
                        else
                        {  string [string_length ++] = 0xC0 | (uchar >> 6);
                           string [string_length ++] = 0x80 | (uchar & 0x3F);
                        }

                        break;
                    }

                    if (uchar <= 0xFFFF) {
                        if (state.first_pass)
                           string_length += 3;
                        else
                        {  string [string_length ++] = 0xE0 | (uchar >> 12);
                           string [string_length ++] = 0x80 | ((uchar >> 6) & 0x3F);
                           string [string_length ++] = 0x80 | (uchar & 0x3F);
                        }

                        break;
                    }

                    if (state.first_pass)
                       string_length += 4;
                    else
                    {  string [string_length ++] = 0xF0 | (uchar >> 18);
                       string [string_length ++] = 0x80 | ((uchar >> 12) & 0x3F);
                       string [string_length ++] = 0x80 | ((uchar >> 6) & 0x3F);
                       string [string_length ++] = 0x80 | (uchar & 0x3F);
                    }

                    break;

                  default:
                     string_add (b);
               };

               continue;
            }

            if (b == '\\')
            {
               flags |= flag_escaped;
               continue;
            }

            if (b == '"')
            {
               if (!state.first_pass)
                  string [string_length] = 0;

               flags &= ~ flag_string;
               string = 0;

               switch (top->type)
               {
                  case json_string:

                     top->u.string.length = string_length;
                     flags |= flag_next;

                     break;

                  case json_object:

                     if (state.first_pass) {
                        json_char **chars = (json_char **) &top->u.object.values;
                        chars[0] += string_length + 1;
                     }
                     else
                     {
                        top->u.object.values [top->u.object.length].name
                           = (json_char *) top->_reserved.object_mem;

                        top->u.object.values [top->u.object.length].name_length
                           = string_length;

                        (*(json_char **) &top->_reserved.object_mem) += string_length + 1;
                     }

                     flags |= flag_seek_value | flag_need_colon;
                     continue;

                  default:
                     break;
               };
            }
            else
            {
               string_add (b);
               continue;
            }
         }

         if (state.settings.settings & json_enable_comments)
         {
            if (flags & (flag_line_comment | flag_block_comment))
            {
               if (flags & flag_line_comment)
               {
                  if (b == '\r' || b == '\n' || !b)
                  {
                     flags &= ~ flag_line_comment;
                     -- state.ptr;  /* so null can be reproc'd */
                  }

                  continue;
               }

               if (flags & flag_block_comment)
               {
                  if (!b)
                  {  sprintf (error, "%u:%u: Unexpected EOF in block comment", line_and_col);
                     goto e_failed;
                  }

                  if (b == '*' && state.ptr < (end - 1) && state.ptr [1] == '/')
                  {
                     flags &= ~ flag_block_comment;
                     ++ state.ptr;  /* skip closing sequence */
                  }

                  continue;
               }
            }
            else if (b == '/')
            {
               if (! (flags & (flag_seek_value | flag_done)) && top->type != json_object)
               {  sprintf (error, "%u:%u: Comment not allowed here", line_and_col);
                  goto e_failed;
               }

               if (++ state.ptr == end)
               {  sprintf (error, "%u:%u: EOF unexpected", line_and_col);
                  goto e_failed;
               }

               switch (b = *state.ptr)
               {
                  case '/':
                     flags |= flag_line_comment;
                     continue;

                  case '*':
                     flags |= flag_block_comment;
                     continue;

                  default:
                     sprintf (error, "%u:%u: Unexpected `%c` in comment opening sequence", line_and_col, b);
                     goto e_failed;
               };
            }
         }

         if (flags & flag_done)
         {
            if (!b)
               break;

            switch (b)
            {
               whitespace:
                  continue;

               default:

                  sprintf (error, "%u:%u: Trailing garbage: `%c`",
                           line_and_col, b);

                  goto e_failed;
            };
         }

         if (flags & flag_seek_value)
         {
            switch (b)
            {
               whitespace:
                  continue;

               case ']':

                  if (top && top->type == json_array)
                     flags = (flags & ~ (flag_need_comma | flag_seek_value)) | flag_next;
                  else
                  {  sprintf (error, "%u:%u: Unexpected `]`", line_and_col);
                     goto e_failed;
                  }

                  break;

               default:

                  if (flags & flag_need_comma)
                  {
                     if (b == ',')
                     {  flags &= ~ flag_need_comma;
                        continue;
                     }
                     else
                     {
                        sprintf (error, "%u:%u: Expected `,` before `%c`",
                                 line_and_col, b);

                        goto e_failed;
                     }
                  }

                  if (flags & flag_need_colon)
                  {
                     if (b == ':')
                     {  flags &= ~ flag_need_colon;
                        continue;
                     }
                     else
                     {
                        sprintf (error, "%u:%u: Expected `:` before `%c`",
                                 line_and_col, b);

                        goto e_failed;
                     }
                  }

                  flags &= ~ flag_seek_value;

                  switch (b)
                  {
                     case '{':

                        if (!new_value (&state, &top, &root, &alloc, json_object))
                           goto e_alloc_failure;

                        continue;

                     case '[':

                        if (!new_value (&state, &top, &root, &alloc, json_array))
                           goto e_alloc_failure;

                        flags |= flag_seek_value;
                        continue;

                     case '"':

                        if (!new_value (&state, &top, &root, &alloc, json_string))
                           goto e_alloc_failure;

                        flags |= flag_string;

                        string = top->u.string.ptr;
                        string_length = 0;

                        continue;

                     case 't':

                        if ((end - state.ptr) <= 3 || *(++ state.ptr) != 'r' ||
                            *(++ state.ptr) != 'u' || *(++ state.ptr) != 'e')
                        {
                           goto e_unknown_value;
                        }

                        if (!new_value (&state, &top, &root, &alloc, json_boolean))
                           goto e_alloc_failure;

                        top->u.boolean = 1;

                        flags |= flag_next;
                        break;

                     case 'f':

                        if ((end - state.ptr) <= 4 || *(++ state.ptr) != 'a' ||
                            *(++ state.ptr) != 'l' || *(++ state.ptr) != 's' ||
                            *(++ state.ptr) != 'e')
                        {
                           goto e_unknown_value;
                        }

                        if (!new_value (&state, &top, &root, &alloc, json_boolean))
                           goto e_alloc_failure;

                        flags |= flag_next;
                        break;

                     case 'n':

                        if ((end - state.ptr) <= 3 || *(++ state.ptr) != 'u' ||
                            *(++ state.ptr) != 'l' || *(++ state.ptr) != 'l')
                        {
                           goto e_unknown_value;
                        }

                        if (!new_value (&state, &top, &root, &alloc, json_null))
                           goto e_alloc_failure;

                        flags |= flag_next;
                        break;

                     default:

                        if (isdigit ((unsigned char) b) || b == '-')
                        {
                           if (!new_value (&state, &top, &root, &alloc, json_integer))
                              goto e_alloc_failure;

                           if (!state.first_pass)
                           {
                              while (isdigit ((unsigned char) b) || b == '+' || b == '-'
                                        || b == 'e' || b == 'E' || b == '.')
                              {
                                 if ( (++ state.ptr) == end)
                                 {
                                    b = 0;
                                    break;
                                 }

                                 b = *state.ptr;
                              }

                              flags |= flag_next | flag_reproc;
                              break;
                           }

                           flags &= ~ (flag_num_negative | flag_num_e |
                                        flag_num_e_got_sign | flag_num_e_negative |
                                           flag_num_zero);

                           num_digits = 0;
                           num_fraction = 0;
                           num_e = 0;

                           if (b != '-')
                           {
                              flags |= flag_reproc;
                              break;
                           }

                           flags |= flag_num_negative;
                           continue;
                        }
                        else
                        {  sprintf (error, "%u:%u: Unexpected `%c` when seeking value", line_and_col, b);
                           goto e_failed;
                        }
                  };
            };
         }
         else
         {
            switch (top->type)
            {
            case json_object:

               switch (b)
               {
                  whitespace:
                     continue;

                  case '"':

                     if (flags & flag_need_comma)
                     {  sprintf (error, "%u:%u: Expected `,` before `\"`", line_and_col);
                        goto e_failed;
                     }

                     flags |= flag_string;

                     string = (json_char *) top->_reserved.object_mem;
                     string_length = 0;

                     break;

                  case '}':

                     flags = (flags & ~ flag_need_comma) | flag_next;
                     break;

                  case ',':

                     if (flags & flag_need_comma)
                     {
                        flags &= ~ flag_need_comma;
                        break;
                     } /* FALLTHRU */

                  default:
                     sprintf (error, "%u:%u: Unexpected `%c` in object", line_and_col, b);
                     goto e_failed;
               };

               break;

            case json_integer:
            case json_double:

               if (isdigit ((unsigned char)b))
               {
                  ++ num_digits;

                  if (top->type == json_integer || flags & flag_num_e)
                  {
                     if (! (flags & flag_num_e))
                     {
                        if (flags & flag_num_zero)
                        {  sprintf (error, "%u:%u: Unexpected `0` before `%c`", line_and_col, b);
                           goto e_failed;
                        }

                        if (num_digits == 1 && b == '0')
                           flags |= flag_num_zero;
                     }
                     else
                     {
                        flags |= flag_num_e_got_sign;
                        num_e = (num_e * 10) + (b - '0');
                        continue;
                     }

                     if (would_overflow(top->u.integer, b))
                     {
                        json_int_t integer = top->u.integer;
                        -- num_digits;
                        -- state.ptr;
                        top->type = json_double;
                        top->u.dbl = (double)integer;
                        continue;
                     }

                     top->u.integer = (top->u.integer * 10) + (b - '0');
                     continue;
                  }

                  if (flags & flag_num_got_decimal)
                     num_fraction = (num_fraction * 10) + (b - '0');
                  else
                     top->u.dbl = (top->u.dbl * 10) + (b - '0');

                  continue;
               }

               if (b == '+' || b == '-')
               {
                  if ( (flags & flag_num_e) && !(flags & flag_num_e_got_sign))
                  {
                     flags |= flag_num_e_got_sign;

                     if (b == '-')
                        flags |= flag_num_e_negative;

                     continue;
                  }
               }
               else if (b == '.' && top->type == json_integer)
               {
                  json_int_t integer = top->u.integer;

                  if (!num_digits)
                  {  sprintf (error, "%u:%u: Expected digit before `.`", line_and_col);
                     goto e_failed;
                  }

                  top->type = json_double;
                  top->u.dbl = (double) integer;

                  flags |= flag_num_got_decimal;
                  num_digits = 0;
                  continue;
               }

               if (! (flags & flag_num_e))
               {
                  if (top->type == json_double)
                  {
                     if (!num_digits)
                     {  sprintf (error, "%u:%u: Expected digit after `.`", line_and_col);
                        goto e_failed;
                     }

                     top->u.dbl += num_fraction / pow (10.0, num_digits);
                  }

                  if (b == 'e' || b == 'E')
                  {
                     flags |= flag_num_e;

                     if (top->type == json_integer)
                     {
                        json_int_t integer = top->u.integer;
                        top->type = json_double;
                        top->u.dbl = (double) integer;
                     }

                     num_digits = 0;
                     flags &= ~ flag_num_zero;

                     continue;
                  }
               }
               else
               {
                  if (!num_digits)
                  {  sprintf (error, "%u:%u: Expected digit after `e`", line_and_col);
                     goto e_failed;
                  }

                  top->u.dbl *= pow (10.0, (flags & flag_num_e_negative ? - num_e : num_e));
               }

               if (flags & flag_num_negative)
               {
                  if (top->type == json_integer)
                     top->u.integer = - top->u.integer;
                  else
                     top->u.dbl = - top->u.dbl;
               }

               flags |= flag_next | flag_reproc;
               break;

            default:
               break;
            };
         }

         if (flags & flag_reproc)
         {
            flags &= ~ flag_reproc;
            -- state.ptr;
         }

         if (flags & flag_next)
         {
            flags = (flags & ~ flag_next) | flag_need_comma;

            if (!top->parent)
            {
               /* root value done */

               flags |= flag_done;
               continue;
            }

            if (top->parent->type == json_array)
               flags |= flag_seek_value;

            if (!state.first_pass)
            {
               json_value * parent = top->parent;

               switch (parent->type)
               {
                  case json_object:

                     parent->u.object.values
                        [parent->u.object.length].value = top;

                     break;

                  case json_array:

                     parent->u.array.values
                           [parent->u.array.length] = top;

                     break;

                  default:
                     break;
               };
            }

            if ( (++ top->parent->u.array.length) > UINT_MAX - 8)
               goto e_overflow;

            top = top->parent;

            continue;
         }
      }

      alloc = root;
   }

   return root;

e_unknown_value:

   sprintf (error, "%u:%u: Unknown value", line_and_col);
   goto e_failed;

e_alloc_failure:

   strcpy (error, "Memory allocation failure");
   goto e_failed;

e_overflow:

   sprintf (error, "%u:%u: Too long (caught overflow)", line_and_col);
   goto e_failed;

e_failed:

   if (error_buf)
   {
      if (*error)
         strcpy (error_buf, error);
      else
         strcpy (error_buf, "Unknown error");
   }

   if (state.first_pass)
      alloc = root;

   while (alloc)
   {
      top = alloc->_reserved.next_alloc;
      state.settings.mem_free (alloc, state.settings.user_data);
      alloc = top;
   }

   if (!state.first_pass)
      json_value_free_ex (&state.settings, root);

   return 0;
}

json_value * json_parse (const json_char * json, size_t length)
{
   json_settings settings = { 0 };
   return json_parse_ex (&settings, json, length, 0);
}

void json_value_free_ex (json_settings * settings, json_value * value)
{
   json_value * cur_value;

   if (!value)
      return;

   value->parent = 0;

   while (value)
   {
      switch (value->type)
      {
         case json_array:

            if (!value->u.array.length)
            {
               settings->mem_free (value->u.array.values, settings->user_data);
               break;
            }

            value = value->u.array.values [-- value->u.array.length];
            continue;

         case json_object:

            if (!value->u.object.length)
            {
               settings->mem_free (value->u.object.values, settings->user_data);
               break;
            }

            value = value->u.object.values [-- value->u.object.length].value;
            continue;

         case json_string:

            settings->mem_free (value->u.string.ptr, settings->user_data);
            break;

         default:
            break;
      };

      cur_value = value;
      value = value->parent;
      settings->mem_free (cur_value, settings->user_data);
   }
}

void json_value_free (json_value * value)
{
   json_settings settings = { 0 };
   settings.mem_free = default_free;
   json_value_free_ex (&settings, value);
}

 void print_depth_shift(int depth)
{
        int j;
        for (j=0; j < depth; j++) {
                printf(" ");
        }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////


void process_value(json_value* value, int depth);

 void process_object(json_value* value, int depth)
{
        int length, x;
        if (value == NULL) {
                return;
        }
        length = value->u.object.length;
        for (x = 0; x < length; x++) {
                print_depth_shift(depth);
                printf("object[%d].name = %s\n", x, value->u.object.values[x].name);
                process_value(value->u.object.values[x].value, depth+1);
                
        }
}

 void process_array(json_value* value, int depth)
{
        int length, x;
        if (value == NULL) {
                return;
        }
        length = value->u.array.length;
        printf("array\n");
        for (x = 0; x < length; x++) {
                process_value(value->u.array.values[x], depth);
        }
}

 void process_value(json_value* value, int depth)
{


        if (value == NULL) {
                return;
        }
        if (value->type != json_object) {
                print_depth_shift(depth);
        }
        switch (value->type) {
                case json_none:
                        printf("none\n");
                        
                        break;
                case json_null:
                        printf("null\n");
                                                
                        break;
                case json_object:
                        process_object(value, depth+1);
                                                
                        break;
                case json_array:
                        process_array(value, depth+1);
                                                
                        break;
                case json_integer:
                        printf("int: %10ld\n", (long)value->u.integer);
                                                
                        break;
                case json_double:
                        printf("double: %f\n", value->u.dbl);
                                                
                        break;
                case json_string:
                        printf("string: %s\n", value->u.string.ptr);
                                                
                        break;
                case json_boolean:
                        printf("bool: %d\n", value->u.boolean);
                                                
                        break;
        }

}

void getter(json_value* value)
{
int depth=0;
            int length,j;
      
        length = value->u.object.length;
       
        do {
        printf("give object index : ");
        scanf("%d", &j);}while(j>length);
        
        
              
                printf("\n");
                print_depth_shift(depth);
                printf("depth %d\n",depth);
                printf("key of object[%d] : %s\n", j, value->u.object.values[j].name);
                printf("value is ");
                process_value(value->u.object.values[j].value, depth);
                printf("\n");
}

void get_key(json_value* value)
{
int depth=0;
            int length,j;
      
        length = value->u.object.length;
       
        do {
        printf("give object index : ");
        scanf("%d", &j);}while(j>length);
        
        
              
                printf("\n");
                print_depth_shift(depth);
                printf("key of object[%d] : %s\n", j, value->u.object.values[j].name);
                printf("\n");
}

void get_value(json_value* value)
{
int depth=0;
            int length,j;
      
        length = value->u.object.length;
       
        do {
        printf("give object index : ");
        scanf("%d", &j);}while(j>length);
        
        
              
                printf("\n");
                print_depth_shift(depth);
                printf("value of object[%d] : ", j);
                process_value(value->u.object.values[j].value, depth+1);
                printf("\n");
}



int setter(int argc, char **argv)
{


// our program will work by writing all of the original file content to a 
// temp file EXCEPT for the line we want to delete, and then we'll delete the 
// original file and rename the temp file to the original file name
  FILE *file, *temp;
  file = fopen(argv[1], "r");
  if (file == NULL)
  {
    printf("Error opening file.\n");
    return 1;
  }
  int current_lines = 1;
  char c;
  do 
  {
    c = fgetc(file);
    if (c == '\n') current_lines++;
  } while (c != EOF);
  fclose(file);
  char temp_filename[FILENAME_SIZE];
  char buffer[MAX_LINE];
  int delete_line = current_lines-1;
  strcpy(temp_filename, "temp____");
  strcat(temp_filename, argv[1]);
  file = fopen(argv[1], "r");
  temp = fopen(temp_filename, "w");
  if (file == NULL || temp == NULL)
  {
    printf("Error opening file(s)\n");
    return 1;
  }
  bool keep_reading = true;
  int current_line = 1;
  do 
  {
    fgets(buffer, MAX_LINE, file);
    if (feof(file)) keep_reading = false;
    else if (current_line != delete_line)
      fputs(buffer, temp);
    // keeps track of the current line being read
    current_line++;
  } while (keep_reading);
  fclose(file);
  fclose(temp);
  remove(argv[1]);
  rename(temp_filename, argv[1]);
  
  FILE *pFile;

char str[256];

pFile=fopen(argv[1], "a");
char all[1000];
    char sep[1000];
    char middle[10];
    char end [10];
    char stopper[10];
    char elkey[256];
    char elval[256];
    printf("give key : ");
	scanf("%s", elkey);
	printf("give value : ");
	scanf("%s", elval);
    // Copy the first string into
    // the variable
    strcpy(all, ",\"");
    strcpy(sep, "\"");
    strcpy(middle, ":");
    strcpy(stopper, "\n}");
    // Concatenate this string
    // to the end of the first one
    strcat(elkey, sep);
    strcat(elkey, middle);
    strcat(elkey, sep);
    strcat(elkey, elval);
    strcat(elkey, sep);
    strcat(elkey, stopper);
    strcat(all, elkey);

if ( pFile )
   {
	           fputs(all, pFile);
    }
   else
      {
         printf("Failed to open the file\n");
        }
	fclose(pFile);
	printf(ANSI_COLOR_GREEN   "DONE"   ANSI_COLOR_RESET "\n");
	
	  
 
  return 0;
}


int display(int argc, char **argv)
{
  FILE *fh;
  fh = fopen(argv[1], "r");
  if (fh != NULL)
  {
    char c;
    while ( (c = fgetc(fh)) != EOF )
      putchar(c);
    printf("\n");
    fclose(fh);
  } else printf("Error opening file.\n");
  
  return 0;
}


/* Function declaration */
void replaceAll(char *str, const char *oldWord, const char *newWord);


int update(int argc, char **argv)
{
    /* File pointer to hold reference of input file */
    FILE * fPtr;
    FILE * fTemp;
    char path[100];
    
    char buffer[BUFFER_SIZE];
    char oldWord[100], newWord[100];



    printf("Enter key or value  to update : ");
    scanf("%s", oldWord);

    printf("new value / key  : ");
    scanf("%s", newWord);


    /*  Open all required files */
    fPtr  = fopen(argv[1], "r");
    fTemp = fopen("replace.tmp", "w"); 

    /* fopen() return NULL if unable to open file in given mode. */
    if (fPtr == NULL || fTemp == NULL)
    {
        /* Unable to open file hence exit */
        printf("\nUnable to open file.\n");
        printf("Please check whether file exists and you have read/write privilege.\n");
        exit(EXIT_SUCCESS);
    }


    /*
     * Read line from source file and write to destination 
     * file after replacing given word.
     */
    while ((fgets(buffer, BUFFER_SIZE, fPtr)) != NULL)
    {
        // Replace all occurrence of word from current line
        replaceAll(buffer, oldWord, newWord);

        // After replacing write it to temp file.
        fputs(buffer, fTemp);
    }


    /* Close all files to release resource */
    fclose(fPtr);
    fclose(fTemp);


    /* Delete original source file */
    remove(argv[1]);

    /* Rename temp file as original file */
    rename("replace.tmp", argv[1]);

    printf(ANSI_COLOR_GREEN   "DONE " ANSI_COLOR_RESET "\nSuccessfully replaced "ANSI_COLOR_RED" %s "ANSI_COLOR_RESET " with "ANSI_COLOR_CYAN " %s \n" ANSI_COLOR_RESET, oldWord, newWord);
    printf("\n");

    return 0;
}



/**
 * Replace all occurrences of a given a word in string.
 */
void replaceAll(char *str, const char *oldWord, const char *newWord)
{
    char *pos, temp[BUFFER_SIZE];
    int index = 0;
    int owlen;

    owlen = strlen(oldWord);

    // Fix: If oldWord and newWord are same it goes to infinite loop
    if (!strcmp(oldWord, newWord)) {
        return;
    }


    /*
     * Repeat till all occurrences are replaced. 
     */
    while ((pos = strstr(str, oldWord)) != NULL)
    {
        // Backup current line
        strcpy(temp, str);

        // Index of current found word
        index = pos - str;

        // Terminate str after word found index
        str[index] = '\0';

        // Concatenate str with new word 
        strcat(str, newWord);
        
        // Concatenate str with remaining words after 
        // oldword found index.
        strcat(str, temp + index + owlen);
    }
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////



enum classes {
	C_SPACE, /* space */
	C_NL,    /* newline */
	C_WHITE, /* tab, CR */
	C_LCURB, C_RCURB, /* object opening/closing */
	C_LSQRB, C_RSQRB, /* array opening/closing */
	/* syntax symbols */
	C_COLON,
	C_COMMA,
	C_QUOTE, /* " */
	C_BACKS, /* \ */
	C_SLASH, /* / */
	C_PLUS,
	C_MINUS,
	C_DOT,
	C_ZERO, C_DIGIT, /* digits */
	C_a, C_b, C_c, C_d, C_e, C_f, C_l, C_n, C_r, C_s, C_t, C_u, /* nocaps letters */
	C_ABCDF, C_E, /* caps letters */
	C_OTHER, /* all other */
	C_STAR, /* star in C style comment */
	C_HASH, /* # for YAML comment */
	C_ERROR = 0xfe,
};

/* map from character < 128 to classes. from 128 to 256 all C_OTHER */
static uint8_t character_class[128] = {
	C_ERROR, C_ERROR, C_ERROR, C_ERROR, C_ERROR, C_ERROR, C_ERROR, C_ERROR,
	C_ERROR, C_WHITE, C_NL,    C_ERROR, C_ERROR, C_WHITE, C_ERROR, C_ERROR,
	C_ERROR, C_ERROR, C_ERROR, C_ERROR, C_ERROR, C_ERROR, C_ERROR, C_ERROR,
	C_ERROR, C_ERROR, C_ERROR, C_ERROR, C_ERROR, C_ERROR, C_ERROR, C_ERROR,

	C_SPACE, C_OTHER, C_QUOTE, C_HASH,  C_OTHER, C_OTHER, C_OTHER, C_OTHER,
	C_OTHER, C_OTHER, C_STAR,  C_PLUS,  C_COMMA, C_MINUS, C_DOT,   C_SLASH,
	C_ZERO,  C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT,
	C_DIGIT, C_DIGIT, C_COLON, C_OTHER, C_OTHER, C_OTHER, C_OTHER, C_OTHER,

	C_OTHER, C_ABCDF, C_ABCDF, C_ABCDF, C_ABCDF, C_E,     C_ABCDF, C_OTHER,
	C_OTHER, C_OTHER, C_OTHER, C_OTHER, C_OTHER, C_OTHER, C_OTHER, C_OTHER,
	C_OTHER, C_OTHER, C_OTHER, C_OTHER, C_OTHER, C_OTHER, C_OTHER, C_OTHER,
	C_OTHER, C_OTHER, C_OTHER, C_LSQRB, C_BACKS, C_RSQRB, C_OTHER, C_OTHER,

	C_OTHER, C_a,     C_b,     C_c,     C_d,     C_e,     C_f,     C_OTHER,
	C_OTHER, C_OTHER, C_OTHER, C_OTHER, C_l,     C_OTHER, C_n,     C_OTHER,
	C_OTHER, C_OTHER, C_r,     C_s,     C_t,     C_u,     C_OTHER, C_OTHER,
	C_OTHER, C_OTHER, C_OTHER, C_LCURB, C_OTHER, C_RCURB, C_OTHER, C_OTHER
};

/* only the first 36 ascii characters need an escape */
static char const *character_escape[] = {
	"\\u0000", "\\u0001", "\\u0002", "\\u0003", "\\u0004", "\\u0005", "\\u0006", "\\u0007", /*  0-7  */
	"\\b"    ,     "\\t",     "\\n", "\\u000b",     "\\f",     "\\r", "\\u000e", "\\u000f", /*  8-f  */
	"\\u0010", "\\u0011", "\\u0012", "\\u0013", "\\u0014", "\\u0015", "\\u0016", "\\u0017", /* 10-17 */
	"\\u0018", "\\u0019", "\\u001a", "\\u001b", "\\u001c", "\\u001d", "\\u001e", "\\u001f", /* 18-1f */
	"\x20"   , "\x21"   , "\\\""   , "\x23"   , "\x24"   , "\x25"   , "\x26"   , "\x27"   , /* 20-27 */
	"\x28"   , "\x29"   , "\x2a"   , "\x2b"   , "\x2c"   , "\x2d"   , "\x2e"   , "\x2f"   , /* 28-2f */
	"\x30"   , "\x31"   , "\x32"   , "\x33"   , "\x34"   , "\x35"   , "\x36"   , "\x37"   , /* 30-37 */
	"\x38"   , "\x39"   , "\x3a"   , "\x3b"   , "\x3c"   , "\x3d"   , "\x3e"   , "\x3f"   , /* 38-3f */
	"\x40"   , "\x41"   , "\x42"   , "\x43"   , "\x44"   , "\x45"   , "\x46"   , "\x47"   , /* 40-47 */
	"\x48"   , "\x49"   , "\x4a"   , "\x4b"   , "\x4c"   , "\x4d"   , "\x4e"   , "\x4f"   , /* 48-4f */
	"\x50"   , "\x51"   , "\x52"   , "\x53"   , "\x54"   , "\x55"   , "\x56"   , "\x57"   , /* 50-57 */
	"\x58"   , "\x59"   , "\x5a"   , "\x5b"   , "\\\\"   , "\x5d"   , "\x5e"   , "\x5f"   , /* 58-5f */
	"\x60"   , "\x61"   , "\x62"   , "\x63"   , "\x64"   , "\x65"   , "\x66"   , "\x67"   , /* 60-67 */
	"\x68"   , "\x69"   , "\x6a"   , "\x6b"   , "\x6c"   , "\x6d"   , "\x6e"   , "\x6f"   , /* 68-6f */
	"\x70"   , "\x71"   , "\x72"   , "\x73"   , "\x74"   , "\x75"   , "\x76"   , "\x77"   , /* 70-77 */
	"\x78"   , "\x79"   , "\x7a"   , "\x7b"   , "\x7c"   , "\x7d"   , "\x7e"   , "\\u007f", /* 78-7f */
	"\\u0080", "\\u0081", "\\u0082", "\\u0083", "\\u0084", "\\u0085", "\\u0086", "\\u0087", /* 80-87 */
	"\\u0088", "\\u0089", "\\u008a", "\\u008b", "\\u008c", "\\u008d", "\\u008e", "\\u008f", /* 88-8f */
	"\\u0090", "\\u0091", "\\u0092", "\\u0093", "\\u0094", "\\u0095", "\\u0096", "\\u0097", /* 90-97 */
	"\\u0098", "\\u0099", "\\u009a", "\\u009b", "\\u009c", "\\u009d", "\\u009e", "\\u009f", /* 98-9f */
	"\\u00a0", "\\u00a1", "\\u00a2", "\\u00a3", "\\u00a4", "\\u00a5", "\\u00a6", "\\u00a7", /* a0-a7 */
	"\\u00a8", "\\u00a9", "\\u00aa", "\\u00ab", "\\u00ac", "\\u00ad", "\\u00ae", "\\u00af", /* a8-af */
	"\\u00b0", "\\u00b1", "\\u00b2", "\\u00b3", "\\u00b4", "\\u00b5", "\\u00b6", "\\u00b7", /* b0-b7 */
	"\\u00b8", "\\u00b9", "\\u00ba", "\\u00bb", "\\u00bc", "\\u00bd", "\\u00be", "\\u00bf", /* b8-bf */
	"\\u00c0", "\\u00c1", "\\u00c2", "\\u00c3", "\\u00c4", "\\u00c5", "\\u00c6", "\\u00c7", /* c0-c7 */
	"\\u00c8", "\\u00c9", "\\u00ca", "\\u00cb", "\\u00cc", "\\u00cd", "\\u00ce", "\\u00cf", /* c8-cf */
	"\\u00d0", "\\u00d1", "\\u00d2", "\\u00d3", "\\u00d4", "\\u00d5", "\\u00d6", "\\u00d7", /* d0-d7 */
	"\\u00d8", "\\u00d9", "\\u00da", "\\u00db", "\\u00dc", "\\u00dd", "\\u00de", "\\u00df", /* d8-df */
	"\\u00e0", "\\u00e1", "\\u00e2", "\\u00e3", "\\u00e4", "\\u00e5", "\\u00e6", "\\u00e7", /* e0-e7 */
	"\\u00e8", "\\u00e9", "\\u00ea", "\\u00eb", "\\u00ec", "\\u00ed", "\\u00ee", "\\u00ef", /* e8-ef */
	"\\u00f0", "\\u00f1", "\\u00f2", "\\u00f3", "\\u00f4", "\\u00f5", "\\u00f6", "\\u00f7", /* f0-f7 */
	"\\u00f8", "\\u00f9", "\\u00fa", "\\u00fb", "\\u00fc", "\\u00fd", "\\u00fe", "\\u00ff", /* f8-ff */
};

/* define all states and actions that will be taken on each transition.
 *
 * states are defined first because of the fact they are use as index in the
 * transitions table. they usually contains either a number or a prefix _
 * for simple state like string, object, value ...
 *
 * actions are defined starting from 0x80. state error is defined as 0xff
 */

enum states {
	STATE_GO, /* start  */
	STATE_OK, /* ok     */
	STATE__O, /* object */
	STATE__K, /* key    */
	STATE_CO, /* colon  */
	STATE__V, /* value  */
	STATE__A, /* array  */
	STATE__S, /* string */
	STATE_E0, /* escape */
	STATE_U1, STATE_U2, STATE_U3, STATE_U4, /* unicode states */
	STATE_M0, STATE_Z0, STATE_I0, /* number states */
	STATE_R1, STATE_R2, /* real states (after-dot digits) */
	STATE_X1, STATE_X2, STATE_X3, /* exponant states */
	STATE_T1, STATE_T2, STATE_T3, /* true constant states */
	STATE_F1, STATE_F2, STATE_F3, STATE_F4, /* false constant states */
	STATE_N1, STATE_N2, STATE_N3, /* null constant states */
	STATE_C1, STATE_C2, STATE_C3, /* C-comment states */
	STATE_Y1, /* YAML-comment state */
	STATE_D1, STATE_D2, /* multi unicode states */
};

/* the following are actions that need to be taken */
enum actions {
	STATE_KS = 0x80, /* key separator */
	STATE_SP, /* comma separator */
	STATE_AB, /* array begin */
	STATE_AE, /* array ending */
	STATE_OB, /* object begin */
	STATE_OE, /* object end */
	STATE_CB, /* C-comment begin */
	STATE_YB, /* YAML-comment begin */
	STATE_CE, /* YAML/C comment end */
	STATE_FA, /* false */
	STATE_TR, /* true */
	STATE_NU, /* null */
	STATE_DE, /* double detected by exponent */
	STATE_DF, /* double detected by . */
	STATE_SE, /* string end */
	STATE_MX, /* integer detected by minus */
	STATE_ZX, /* integer detected by zero */
	STATE_IX, /* integer detected by 1-9 */
	STATE_UC, /* Unicode character read */
};

/* error state */
#define STATE___ 	0xff

#define NR_STATES 	(STATE_D2 + 1)
#define NR_CLASSES	(C_HASH + 1)

#define IS_STATE_ACTION(s) ((s) & 0x80)
#define S(x) STATE_##x
#define PT_(a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q,r,s,t,u,v,w,x,y,z,a1,b1,c1,d1,e1,f1,g1,h1)	\
	{ S(a),S(b),S(c),S(d),S(e),S(f),S(g),S(h),S(i),S(j),S(k),S(l),S(m),S(n),		\
	  S(o),S(p),S(q),S(r),S(s),S(t),S(u),S(v),S(w),S(x),S(y),S(z),S(a1),S(b1),		\
	  S(c1),S(d1),S(e1),S(f1),S(g1),S(h1) }

/* map from the (previous state+new character class) to the next parser transition */
static const uint8_t state_transition_table[NR_STATES][NR_CLASSES] = {
/*             white                                                                            ABCDF  other    */
/*         sp nl |  {  }  [  ]  :  ,  "  \  /  +  -  .  0  19 a  b  c  d  e  f  l  n  r  s  t  u  |  E  |  *  # */
/*GO*/ PT_(GO,GO,GO,OB,__,AB,__,__,__,__,__,CB,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,YB),
/*OK*/ PT_(OK,OK,OK,__,OE,__,AE,__,SP,__,__,CB,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,YB),
/*_O*/ PT_(_O,_O,_O,__,OE,__,__,__,__,_S,__,CB,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,YB),
/*_K*/ PT_(_K,_K,_K,__,__,__,__,__,__,_S,__,CB,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,YB),
/*CO*/ PT_(CO,CO,CO,__,__,__,__,KS,__,__,__,CB,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,YB),
/*_V*/ PT_(_V,_V,_V,OB,__,AB,__,__,__,_S,__,CB,__,MX,__,ZX,IX,__,__,__,__,__,F1,__,N1,__,__,T1,__,__,__,__,__,YB),
/*_A*/ PT_(_A,_A,_A,OB,__,AB,AE,__,__,_S,__,CB,__,MX,__,ZX,IX,__,__,__,__,__,F1,__,N1,__,__,T1,__,__,__,__,__,YB),
/****************************************************************************************************************/
/*_S*/ PT_(_S,__,__,_S,_S,_S,_S,_S,_S,SE,E0,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S,_S),
/*E0*/ PT_(__,__,__,__,__,__,__,__,__,_S,_S,_S,__,__,__,__,__,__,_S,__,__,__,_S,__,_S,_S,__,_S,U1,__,__,__,__,__),
/*U1*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,U2,U2,U2,U2,U2,U2,U2,U2,__,__,__,__,__,__,U2,U2,__,__,__),
/*U2*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,U3,U3,U3,U3,U3,U3,U3,U3,__,__,__,__,__,__,U3,U3,__,__,__),
/*U3*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,U4,U4,U4,U4,U4,U4,U4,U4,__,__,__,__,__,__,U4,U4,__,__,__),
/*U4*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,UC,UC,UC,UC,UC,UC,UC,UC,__,__,__,__,__,__,UC,UC,__,__,__),
/****************************************************************************************************************/
/*M0*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,Z0,I0,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__),
/*Z0*/ PT_(OK,OK,OK,__,OE,__,AE,__,SP,__,__,CB,__,__,DF,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,YB),
/*I0*/ PT_(OK,OK,OK,__,OE,__,AE,__,SP,__,__,CB,__,__,DF,I0,I0,__,__,__,__,DE,__,__,__,__,__,__,__,__,DE,__,__,YB),
/*R1*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,R2,R2,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__),
/*R2*/ PT_(OK,OK,OK,__,OE,__,AE,__,SP,__,__,CB,__,__,__,R2,R2,__,__,__,__,X1,__,__,__,__,__,__,__,__,X1,__,__,YB),
/*X1*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,X2,X2,__,X3,X3,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__),
/*X2*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,X3,X3,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__),
/*X3*/ PT_(OK,OK,OK,__,OE,__,AE,__,SP,__,__,__,__,__,__,X3,X3,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__),
/****************************************************************************************************************/
/*T1*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,T2,__,__,__,__,__,__,__,__),
/*T2*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,T3,__,__,__,__,__),
/*T3*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,TR,__,__,__,__,__,__,__,__,__,__,__,__),
/*F1*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,F2,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__),
/*F2*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,F3,__,__,__,__,__,__,__,__,__,__),
/*F3*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,F4,__,__,__,__,__,__,__),
/*F4*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,FA,__,__,__,__,__,__,__,__,__,__,__,__),
/*N1*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,N2,__,__,__,__,__),
/*N2*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,N3,__,__,__,__,__,__,__,__,__,__),
/*N3*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,NU,__,__,__,__,__,__,__,__,__,__),
/****************************************************************************************************************/
/*C1*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,C2,__),
/*C2*/ PT_(C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C3,C2),
/*C3*/ PT_(C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,CE,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C2,C3,C2),
/*Y1*/ PT_(Y1,CE,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1,Y1),
/*D1*/ PT_(__,__,__,__,__,__,__,__,__,__,D2,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__),
/*D2*/ PT_(__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,U1,__,__,__,__,__),
};
#undef S
#undef PT_

/* map from (previous state+new character class) to the buffer policy. ignore=0/append=1/escape=2 */
static const uint8_t buffer_policy_table[NR_STATES][NR_CLASSES] = {
/*          white                                                                            ABCDF  other     */
/*      sp nl  |  {  }  [  ]  :  ,  "  \  /  +  -  .  0  19 a  b  c  d  e  f  l  n  r  s  t  u  |  E  |  *  # */
/*GO*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*OK*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*_O*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*_K*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*CO*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*_V*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*_A*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/**************************************************************************************************************/
/*_S*/ { 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
/*E0*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 2, 0, 2, 2, 0, 2, 0, 0, 0, 0, 0, 0 },
/*U1*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0 },
/*U2*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0 },
/*U3*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0 },
/*U4*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0 },
/**************************************************************************************************************/
/*M0*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*Z0*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*I0*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 },
/*R1*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*R2*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0 },
/*X1*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*X2*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*X3*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/**************************************************************************************************************/
/*T1*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*T2*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*T3*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*F1*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*F2*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*F3*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*F4*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*N1*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*N2*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*N3*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/**************************************************************************************************************/
/*C1*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*C2*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*C3*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*Y1*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*D1*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
/*D2*/ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

#define __ 0xff
static const uint8_t utf8_header_table[256] =
{
/* 00 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 10 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 20 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 30 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 40 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 50 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 60 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 70 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 80 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* 90 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* a0 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* b0 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* c0 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* d0 */ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
/* e0 */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
/* f0 */ 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5,__,__,
};

static const uint8_t utf8_continuation_table[256] =
{
/*__0 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* 10 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* 20 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* 30 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* 40 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* 50 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* 60 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* 70 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* 80 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* 90 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* a0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* b0 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* c0 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* d0 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* e0 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
/* f0 */__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,__,
};
#undef __

#define MODE_ARRAY 0
#define MODE_OBJECT 1

static inline void *memory_realloc(void *(*realloc_fct)(void *, size_t), void *ptr, size_t size)
{
	return (realloc_fct) ? realloc_fct(ptr, size) : realloc(ptr, size);
}

static inline void *memory_calloc(void *(*calloc_fct)(size_t, size_t), size_t nmemb, size_t size)
{
	return (calloc_fct) ? calloc_fct(nmemb, size) : calloc(nmemb, size);
}

#define parser_calloc(parser, n, s) memory_calloc(parser->config.user_calloc, n, s)
#define parser_realloc(parser, n, s) memory_realloc(parser->config.user_realloc, n, s)

static int state_grow(json_parser *parser)
{
	uint32_t newsize = parser->stack_size * 2;
	void *ptr;

	if (parser->config.max_nesting != 0)
		return JSON_ERROR_NESTING_LIMIT;

	ptr = parser_realloc(parser, parser->stack, newsize * sizeof(uint8_t));
	if (!ptr)
		return JSON_ERROR_NO_MEMORY;
	parser->stack = ptr;
	parser->stack_size = newsize;
	return 0;
}

static int state_push(json_parser *parser, int mode)
{
	if (parser->stack_offset >= parser->stack_size) {
		int ret = state_grow(parser);
		if (ret)
			return ret;
	}
	parser->stack[parser->stack_offset++] = mode;
	return 0;
}

static int state_pop(json_parser *parser, int mode)
{
	if (parser->stack_offset == 0)
		return JSON_ERROR_POP_EMPTY;
	parser->stack_offset--;
	if (parser->stack[parser->stack_offset] != mode)
		return JSON_ERROR_POP_UNEXPECTED_MODE;
	return 0;
}

static int buffer_grow(json_parser *parser)
{
	uint32_t newsize;
	void *ptr;
	uint32_t max = parser->config.max_data;

	if (max > 0 && parser->buffer_size == max)
		return JSON_ERROR_DATA_LIMIT;
	newsize = parser->buffer_size * 2;
	if (max > 0 && newsize > max)
		newsize = max;

	ptr = parser_realloc(parser, parser->buffer, newsize * sizeof(char));
	if (!ptr)
		return JSON_ERROR_NO_MEMORY;
	parser->buffer = ptr;
	parser->buffer_size = newsize;
	return 0;
}

static int buffer_push(json_parser *parser, unsigned char c)
{
	int ret;

	if (parser->buffer_offset + 1 >= parser->buffer_size) {
		ret = buffer_grow(parser);
		if (ret)
			return ret;
	}
	parser->buffer[parser->buffer_offset++] = c;
	return 0;
}

static int do_callback_withbuf(json_parser *parser, int type)
{
	if (!parser->callback)
		return 0;
	parser->buffer[parser->buffer_offset] = '\0';
	return (*parser->callback)(parser->userdata, type, parser->buffer, parser->buffer_offset);
}

static int do_callback(json_parser *parser, int type)
{
	if (!parser->callback)
		return 0;
	return (*parser->callback)(parser->userdata, type, NULL, 0);
}

static int do_buffer(json_parser *parser)
{
	int ret = 0;

	switch (parser->type) {
	case JSON_KEY: case JSON_STRING:
	case JSON_FLOAT: case JSON_INT:
	case JSON_NULL: case JSON_TRUE: case JSON_FALSE:
		ret = do_callback_withbuf(parser, parser->type);
		if (ret)
			return ret;
		break;
	default:
		break;
	}
	parser->buffer_offset = 0;
	return ret;
}

static const uint8_t hextable[] = {
	255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
	255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
	255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
	  0,  1,  2,  3,  4,  5,  6,  7,  8,  9,255,255,255,255,255,255,
	255, 10, 11, 12, 13, 14, 15,255,255,255,255,255,255,255,255,255,
	255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
	255, 10, 11, 12, 13, 14, 15,255,255,255,255,255,255,255,255,255,
	255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
};

#define hex(c) (hextable[(uint8_t) c])

/* high surrogate range from d800 to dbff */
/* low surrogate range dc00 to dfff */
#define IS_HIGH_SURROGATE(uc) (((uc) & 0xfc00) == 0xd800)
#define IS_LOW_SURROGATE(uc)  (((uc) & 0xfc00) == 0xdc00)

/* transform an unicode [0-9A-Fa-f]{4} sequence into a proper value */
static int decode_unicode_char(json_parser *parser)
{
	uint32_t uval;
	char *b = parser->buffer;
	int offset = parser->buffer_offset;

	uval = (hex(b[offset - 4]) << 12) | (hex(b[offset - 3]) << 8)
	     | (hex(b[offset - 2]) << 4) | hex(b[offset - 1]);

	parser->buffer_offset -= 4;

	/* fast case */
	if (!parser->unicode_multi && uval < 0x80) {
		b[parser->buffer_offset++] = (char) uval;
		return 0;
	}

	if (parser->unicode_multi) {
		if (!IS_LOW_SURROGATE(uval))
			return JSON_ERROR_UNICODE_MISSING_LOW_SURROGATE;

		uval = 0x10000 + ((parser->unicode_multi & 0x3ff) << 10) + (uval & 0x3ff);
		b[parser->buffer_offset++] = (char) ((uval >> 18) | 0xf0);
		b[parser->buffer_offset++] = (char) (((uval >> 12) & 0x3f) | 0x80);
		b[parser->buffer_offset++] = (char) (((uval >> 6) & 0x3f) | 0x80);
		b[parser->buffer_offset++] = (char) ((uval & 0x3f) | 0x80);
		parser->unicode_multi = 0;
		return 0;
	}

	if (IS_LOW_SURROGATE(uval))
		return JSON_ERROR_UNICODE_UNEXPECTED_LOW_SURROGATE;
	if (IS_HIGH_SURROGATE(uval)) {
		parser->unicode_multi = uval;
		return 0;
	}

	if (uval < 0x800) {
		b[parser->buffer_offset++] = (char) ((uval >> 6) | 0xc0);
		b[parser->buffer_offset++] = (char) ((uval & 0x3f) | 0x80);
	} else {
		b[parser->buffer_offset++] = (char) ((uval >> 12) | 0xe0);
		b[parser->buffer_offset++] = (char) (((uval >> 6) & 0x3f) | 0x80);
		b[parser->buffer_offset++] = (char) (((uval >> 0) & 0x3f) | 0x80);
	}
	return 0;
}

static int buffer_push_escape(json_parser *parser, unsigned char next)
{
	char c = '\0';

	switch (next) {
	case 'b': c = '\b'; break;
	case 'f': c = '\f'; break;
	case 'n': c = '\n'; break;
	case 'r': c = '\r'; break;
	case 't': c = '\t'; break;
	case '"': c = '"'; break;
	case '/': c = '/'; break;
	case '\\': c = '\\'; break;
	}
	/* push the escaped character */
	return buffer_push(parser, c);
}

#define CHK(f) do { ret = f; if (ret) return ret; } while(0)

static int act_uc(json_parser *parser)
{
	int ret;
	CHK(decode_unicode_char(parser));
	parser->state = (parser->unicode_multi) ? STATE_D1 : STATE__S;
	return 0;
}

static int act_yb(json_parser *parser)
{
	if (!parser->config.allow_yaml_comments)
		return JSON_ERROR_COMMENT_NOT_ALLOWED;
	parser->save_state = parser->state;
	return 0;
}

static int act_cb(json_parser *parser)
{
	if (!parser->config.allow_c_comments)
		return JSON_ERROR_COMMENT_NOT_ALLOWED;
	parser->save_state = parser->state;
	return 0;
}

static int act_ce(json_parser *parser)
{
	parser->state = (parser->save_state > STATE__A) ? STATE_OK : parser->save_state;
	return 0;
}

static int act_ob(json_parser *parser)
{
	int ret;
	CHK(do_callback(parser, JSON_OBJECT_BEGIN));
	CHK(state_push(parser, MODE_OBJECT));
	parser->expecting_key = 1;
	return 0;
}

static int act_oe(json_parser *parser)
{
	int ret;
	CHK(state_pop(parser, MODE_OBJECT));
	CHK(do_callback(parser, JSON_OBJECT_END));
	parser->expecting_key = 0;
	return 0;
}

static int act_ab(json_parser *parser)
{
	int ret;
	CHK(do_callback(parser, JSON_ARRAY_BEGIN));
	CHK(state_push(parser, MODE_ARRAY));
	return 0;
}

static int act_ae(json_parser *parser)
{
	int ret;
	CHK(state_pop(parser, MODE_ARRAY));
	CHK(do_callback(parser, JSON_ARRAY_END));
	return 0;
}

static int act_se(json_parser *parser)
{
	int ret;
	CHK(do_callback_withbuf(parser, (parser->expecting_key) ? JSON_KEY : JSON_STRING));
	parser->buffer_offset = 0;
	parser->state = (parser->expecting_key) ? STATE_CO : STATE_OK;
	parser->expecting_key = 0;
	return 0;
}

static int act_sp(json_parser *parser)
{
	if (parser->stack_offset == 0)
		return JSON_ERROR_COMMA_OUT_OF_STRUCTURE;
	if (parser->stack[parser->stack_offset - 1] == MODE_OBJECT) {
		parser->expecting_key = 1;
		parser->state = STATE__K;
	} else
		parser->state = STATE__V;
	return 0;
}

struct action_descr
{
	int (*call)(json_parser *parser);
	uint8_t type;
	uint8_t state; /* 0 if we let the callback set the value it want */
	uint8_t dobuffer;
};

static struct action_descr actions_map[] = {
	{ NULL,   JSON_NONE,  STATE__V, 0 }, /* KS */
	{ act_sp, JSON_NONE,  0,        1 }, /* SP */
	{ act_ab, JSON_NONE,  STATE__A, 0 }, /* AB */
	{ act_ae, JSON_NONE,  STATE_OK, 1 }, /* AE */
	{ act_ob, JSON_NONE,  STATE__O, 0 }, /* OB */
	{ act_oe, JSON_NONE,  STATE_OK, 1 }, /* OE */
	{ act_cb, JSON_NONE,  STATE_C1, 1 }, /* CB */
	{ act_yb, JSON_NONE,  STATE_Y1, 1 }, /* YB */
	{ act_ce, JSON_NONE,  0,        0 }, /* CE */
	{ NULL,   JSON_FALSE, STATE_OK, 0 }, /* FA */
	{ NULL,   JSON_TRUE,  STATE_OK, 0 }, /* TR */
	{ NULL,   JSON_NULL,  STATE_OK, 0 }, /* NU */
	{ NULL,   JSON_FLOAT, STATE_X1, 0 }, /* DE */
	{ NULL,   JSON_FLOAT, STATE_R1, 0 }, /* DF */
	{ act_se, JSON_NONE,  0,        0 }, /* SE */
	{ NULL,   JSON_INT,   STATE_M0, 0 }, /* MX */
	{ NULL,   JSON_INT,   STATE_Z0, 0 }, /* ZX */
	{ NULL,   JSON_INT,   STATE_I0, 0 }, /* IX */
	{ act_uc, JSON_NONE,  0,        0 }, /* UC */
};

static int do_action(json_parser *parser, int next_state)
{
	struct action_descr *descr = &actions_map[next_state & ~0x80];

	if (descr->call) {
		int ret;
		if (descr->dobuffer)
			CHK(do_buffer(parser));
		CHK((descr->call)(parser));
	}
	if (descr->state)
		parser->state = descr->state;
	parser->type = descr->type;
	return 0;
}

/** json_parser_init initialize a parser structure taking a config,
 * a config and its userdata.
 * return JSON_ERROR_NO_MEMORY if memory allocation failed or SUCCESS.
 */
int json_parser_init(json_parser *parser, json_config *config,
                     json_parser_callback callback, void *userdata)
{
	memset(parser, 0, sizeof(*parser));

	if (config)
		memcpy(&parser->config, config, sizeof(json_config));
	parser->callback = callback;
	parser->userdata = userdata;

	/* initialise parsing stack and state */
	parser->stack_offset = 0;
	parser->state = STATE_GO;

	/* initialize the parse stack */
	parser->stack_size = (parser->config.max_nesting > 0)
		? parser->config.max_nesting
		: LIBJSON_DEFAULT_STACK_SIZE;

	parser->stack = parser_calloc(parser, parser->stack_size, sizeof(parser->stack[0]));
	if (!parser->stack)
		return JSON_ERROR_NO_MEMORY;

	/* initialize the parse buffer */
	parser->buffer_size = (parser->config.buffer_initial_size > 0)
		? parser->config.buffer_initial_size
		: LIBJSON_DEFAULT_BUFFER_SIZE;

	if (parser->config.max_data > 0 && parser->buffer_size > parser->config.max_data)
		parser->buffer_size = parser->config.max_data;

	parser->buffer = parser_calloc(parser, parser->buffer_size, sizeof(char));
	if (!parser->buffer) {
		free(parser->stack);
		return JSON_ERROR_NO_MEMORY;
	}
	return 0;
}

/** json_parser_free freed memory structure allocated by the parser */
int json_parser_free(json_parser *parser)
{
	if (!parser)
		return 0;
	free(parser->stack);
	free(parser->buffer);
	parser->stack = NULL;
	parser->buffer = NULL;
	return 0;
}

/** json_parser_is_done return 0 is the parser isn't in a finish state. !0 if it is */
int json_parser_is_done(json_parser *parser)
{
	/* need to compare the state to !GO to not accept empty document */
	return parser->stack_offset == 0 && parser->state != STATE_GO;
}

/** json_parser_string append a string s with a specific length to the parser
 * return 0 if everything went ok, a JSON_ERROR_* otherwise.
 * the user can supplied a valid processed pointer that will
 * be fill with the number of processed characters before returning */
int json_parser_string(json_parser *parser, const char *s,
                       uint32_t length, uint32_t *processed)
{
	int ret;
	int next_class, next_state;
	int buffer_policy;
	uint32_t i;

	ret = 0;
	for (i = 0; i < length; i++) {
		unsigned char ch = s[i];

		ret = 0;
		if (parser->utf8_multibyte_left > 0) {
			if (utf8_continuation_table[ch] != 0) {
				ret = JSON_ERROR_UTF8;
				break;
			}
			next_class = C_OTHER;
			parser->utf8_multibyte_left--;
		} else {
			parser->utf8_multibyte_left = utf8_header_table[ch];
			if (parser->utf8_multibyte_left == 0xff) {
				ret = JSON_ERROR_UTF8;
				break;
			}
			next_class = (parser->utf8_multibyte_left > 0) ? C_OTHER : character_class[ch];
			if (next_class == C_ERROR) {
				ret = JSON_ERROR_BAD_CHAR;
				break;
			}
		}

		next_state = state_transition_table[parser->state][next_class];
		buffer_policy = buffer_policy_table[parser->state][next_class];
		TRACING("addchar %d (current-state=%d, next-state=%d, buf-policy=%d)\n",
			ch, parser->state, next_state, buffer_policy);
		if (next_state == STATE___) {
			ret = JSON_ERROR_UNEXPECTED_CHAR;
			break;
		}

		/* add char to buffer */
		if (buffer_policy) {
			ret = (buffer_policy == 2)
				? buffer_push_escape(parser, ch)
				: buffer_push(parser, ch);
			if (ret)
				break;
		}

		/* move to the next level */
		if (IS_STATE_ACTION(next_state))
			ret = do_action(parser, next_state);
		else
			parser->state = next_state;
		if (ret)
			break;
	}
	if (processed)
		*processed = i;
	return ret;
}

/** json_parser_char append one single char to the parser
 * return 0 if everything went ok, a JSON_ERROR_* otherwise */
int json_parser_char(json_parser *parser, unsigned char ch)
{
	return json_parser_string(parser, (char *) &ch, 1, NULL);
}

/** json_print_init initialize a printer context. always succeed */
int json_print_init(json_printer *printer, json_printer_callback callback, void *userdata)
{
	memset(printer, '\0', sizeof(*printer));
	printer->callback = callback;
	printer->userdata = userdata;

	printer->indentstr = "\t";
	printer->indentlevel = 0;
	printer->enter_object = 1;
	printer->first = 1;
	return 0;
}

/** json_print_free free a printer context
 * doesn't do anything now, but in future print_init could allocate memory */
int json_print_free(json_printer *printer)
{
	memset(printer, '\0', sizeof(*printer));
	return 0;
}

/* escape a C string to be a JSON valid string on the wire.
 * : it doesn't do unicode verification. yet?. */
static int print_string(json_printer *printer, const char *data, uint32_t length)
{
	uint32_t i;

	printer->callback(printer->userdata, "\"", 1);
	for (i = 0; i < length; i++) {
		unsigned char c = data[i];
		if (c < 36) {
			char const *esc = character_escape[c];
			printer->callback(printer->userdata, esc, strlen(esc));
		} else if (c == '\\') {
			printer->callback(printer->userdata, "\\\\", 2);
		} else
			printer->callback(printer->userdata, data + i, 1);
	}
	printer->callback(printer->userdata, "\"", 1);
	return 0;
}

static int print_binary_string(json_printer *printer, const char *data, uint32_t length)
{
	uint32_t i;

	printer->callback(printer->userdata, "\"", 1);
	for (i = 0; i < length; i++) {
		unsigned char c = data[i];
		char const *esc = character_escape[c];
		printer->callback(printer->userdata, esc, strlen(esc));
	}
	printer->callback(printer->userdata, "\"", 1);
	return 0;
}


static int print_indent(json_printer *printer)
{
	int i;
	printer->callback(printer->userdata, "\n", 1);
	for (i = 0; i < printer->indentlevel; i++)
		printer->callback(printer->userdata, printer->indentstr, strlen(printer->indentstr));
	return 0;
}

static int json_print_mode(json_printer *printer, int type, const char *data, uint32_t length, int pretty)
{
	int enterobj = printer->enter_object;

	if (!enterobj && !printer->afterkey && (type != JSON_ARRAY_END && type != JSON_OBJECT_END)) {
		printer->callback(printer->userdata, ",", 1);
		if (pretty) print_indent(printer);
	}

	if (pretty && (enterobj && !printer->first && (type != JSON_ARRAY_END && type != JSON_OBJECT_END))) {
		print_indent(printer);
	}

	printer->first = 0;
	printer->enter_object = 0;
	printer->afterkey = 0;
	switch (type) {
	case JSON_ARRAY_BEGIN:
		printer->callback(printer->userdata, "[", 1);
		printer->indentlevel++;
		printer->enter_object = 1;
		break;
	case JSON_OBJECT_BEGIN:
		printer->callback(printer->userdata, "{", 1);
		printer->indentlevel++;
		printer->enter_object = 1;
		break;
	case JSON_ARRAY_END:
	case JSON_OBJECT_END:
		printer->indentlevel--;
		if (pretty && !enterobj) print_indent(printer);
		printer->callback(printer->userdata, (type == JSON_OBJECT_END) ? "}" : "]", 1);
		break;
	case JSON_INT: printer->callback(printer->userdata, data, length); break;
	case JSON_FLOAT: printer->callback(printer->userdata, data, length); break;
	case JSON_NULL: printer->callback(printer->userdata, "null", 4); break;
	case JSON_TRUE: printer->callback(printer->userdata, "true", 4); break;
	case JSON_FALSE: printer->callback(printer->userdata, "false", 5); break;
	case JSON_KEY:
		print_string(printer, data, length);
		printer->callback(printer->userdata, ": ", (pretty) ? 2 : 1);
		printer->afterkey = 1;
		break;
	case JSON_STRING:
		print_string(printer, data, length);
		break;
	case JSON_BSTRING:
		print_binary_string(printer, data, length);
		break;
	default:
		break;
	}

	return 0;
}

/** json_print_pretty pretty print the passed argument (type/data/length). */
int json_print_pretty(json_printer *printer, int type, const char *data, uint32_t length)
{
	return json_print_mode(printer, type, data, length, 1);
}

/** json_print_raw prints without eye candy the passed argument (type/data/length). */
int json_print_raw(json_printer *printer, int type, const char *data, uint32_t length)
{
	return json_print_mode(printer, type, data, length, 0);
}

/** json_print_args takes multiple types and pass them to the printer function */
int json_print_args(json_printer *printer,
                    int (*f)(json_printer *, int, const char *, uint32_t),
                    ...)
{
	va_list ap;
	char *data;
	uint32_t length;
	int type, ret;

	ret = 0;
	va_start(ap, f);
	while ((type = va_arg(ap, int)) != -1) {
		switch (type) {
		case JSON_ARRAY_BEGIN:
		case JSON_ARRAY_END:
		case JSON_OBJECT_BEGIN:
		case JSON_OBJECT_END:
		case JSON_NULL:
		case JSON_TRUE:
		case JSON_FALSE:
			ret = (*f)(printer, type, NULL, 0);
			break;
		case JSON_INT:
		case JSON_FLOAT:
		case JSON_KEY:
		case JSON_STRING:
			data = va_arg(ap, char *);
			length = va_arg(ap, uint32_t);
			if (length == -1)
				length = strlen(data);
			ret = (*f)(printer, type, data, length);
			break;
		}
		if (ret)
			break;
	}
	va_end(ap);
	return ret;
}

static int dom_push(struct json_parser_dom *ctx, void *val)
{
	if (ctx->stack_offset == ctx->stack_size) {
		void *ptr;
		uint32_t newsize = ctx->stack_size * 2;
		ptr = memory_realloc(ctx->user_realloc, ctx->stack, newsize);
		if (!ptr)
			return JSON_ERROR_NO_MEMORY;
		ctx->stack = ptr;
		ctx->stack_size = newsize;
	}
	ctx->stack[ctx->stack_offset].val = val;
	ctx->stack[ctx->stack_offset].key = NULL;
	ctx->stack[ctx->stack_offset].key_length = 0;
	ctx->stack_offset++;
	return 0;
}

static int dom_pop(struct json_parser_dom *ctx, void **val)
{
	ctx->stack_offset--;
	*val = ctx->stack[ctx->stack_offset].val;
	return 0;
}

int json_parser_dom_init(json_parser_dom *dom,
                         json_parser_dom_create_structure create_structure,
                         json_parser_dom_create_data create_data,
                         json_parser_dom_append append)
{
	memset(dom, 0, sizeof(*dom));
	dom->stack_size = 1024;
	dom->stack_offset = 0;
	dom->stack = memory_calloc(dom->user_calloc, dom->stack_size, sizeof(*(dom->stack)));
	if (!dom->stack)
		return JSON_ERROR_NO_MEMORY;
	dom->append = append;
	dom->create_structure = create_structure;
	dom->create_data = create_data;
	return 0;
}

int json_parser_dom_free(json_parser_dom *dom)
{
	free(dom->stack);
	return 0;
}

int json_parser_dom_callback(void *userdata, int type, const char *data, uint32_t length)
{
	struct json_parser_dom *ctx = userdata;
	void *v;
	struct stack_elem *stack = NULL;

	switch (type) {
	case JSON_ARRAY_BEGIN:
	case JSON_OBJECT_BEGIN:
		v = ctx->create_structure(ctx->stack_offset, type == JSON_OBJECT_BEGIN);
		if (!v)
			return JSON_ERROR_CALLBACK;
		dom_push(ctx, v);
		break;
	case JSON_OBJECT_END:
	case JSON_ARRAY_END:
		dom_pop(ctx, &v);
		if (ctx->stack_offset > 0) {
			stack = &(ctx->stack[ctx->stack_offset - 1]);
			ctx->append(stack->val, stack->key, stack->key_length, v);
			free(stack->key);
		} else
			ctx->root_structure = v;
		break;
	case JSON_KEY:
		stack = &(ctx->stack[ctx->stack_offset - 1]);
		stack->key = memory_calloc(ctx->user_calloc, length + 1, sizeof(char));
		stack->key_length = length;
		if (!stack->key)
			return JSON_ERROR_NO_MEMORY;
		memcpy(stack->key, data, length);
		break;
	case JSON_STRING:
	case JSON_INT:
	case JSON_FLOAT:
	case JSON_NULL:
	case JSON_TRUE:
	case JSON_FALSE:
		stack = &(ctx->stack[ctx->stack_offset - 1]);
		v = ctx->create_data(type, data, length);
		if (!v)
			return JSON_ERROR_CALLBACK;
		if (ctx->append(stack->val, stack->key, stack->key_length, v))
			return JSON_ERROR_CALLBACK;
		free(stack->key);
		break;
	}
	return 0;
}


//////////////////////////////////////////////////////////////////////////////


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <locale.h>
#include <getopt.h>
#include <errno.h>


char *indent_string = NULL;

char *string_of_errors[] =
{
	[JSON_ERROR_NO_MEMORY] = "out of memory",
	[JSON_ERROR_BAD_CHAR] = "bad character",
	[JSON_ERROR_POP_EMPTY] = "stack empty",
	[JSON_ERROR_POP_UNEXPECTED_MODE] = "pop unexpected mode",
	[JSON_ERROR_NESTING_LIMIT] = "nesting limit",
	[JSON_ERROR_DATA_LIMIT] = "data limit",
	[JSON_ERROR_COMMENT_NOT_ALLOWED] = "comment not allowed by config",
	[JSON_ERROR_UNEXPECTED_CHAR] = "unexpected char",
	[JSON_ERROR_UNICODE_MISSING_LOW_SURROGATE] = "missing unicode low surrogate",
	[JSON_ERROR_UNICODE_UNEXPECTED_LOW_SURROGATE] = "unexpected unicode low surrogate",
	[JSON_ERROR_COMMA_OUT_OF_STRUCTURE] = "error comma out of structure",
	[JSON_ERROR_CALLBACK] = "error in a callback",
	[JSON_ERROR_UTF8]     = "utf8 validation error"
};

static int printchannel(void *userdata, const char *data, uint32_t length)
{
	FILE *channel = userdata;
	int ret;
	ret = fwrite(data, length, 1, channel);
	if (ret != length)
		return 1;
	return 0;
}

static int prettyprint(void *userdata, int type, const char *data, uint32_t length)
{
	json_printer *printer = userdata;
	
	return json_print_pretty(printer, type, data, length);
}

FILE *open_filename(const char *filename, const char *opt, int is_input)
{
	FILE *input;
	if (strcmp(filename, "-") == 0)
		input = (is_input) ? stdin : stdout;
	else {
		input = fopen(filename, opt);
		if (!input) {
			fprintf(stderr, "error: cannot open %s: %s", filename, strerror(errno));
			return NULL;
		}
	}
	return input;
}

void close_filename(const char *filename, FILE *file)
{
	if (strcmp(filename, "-") != 0)
		fclose(file);
}

int process_file(json_parser *parser, FILE *input, int *retlines, int *retcols)
{
	char buffer[4096];
	int ret = 0;
	int32_t read;
	int lines, col, i;

	lines = 1;
	col = 0;
	while (1) {
		uint32_t processed;
		read = fread(buffer, 1, 4096, input);
		if (read <= 0)
			break;
		ret = json_parser_string(parser, buffer, read, &processed);
		for (i = 0; i < processed; i++) {
			if (buffer[i] == '\n') { col = 0; lines++; } else col++;
		}
		if (ret)
			break;
	}
	if (retlines) *retlines = lines;
	if (retcols) *retcols = col;
	return ret;
}

static int do_verify(json_config *config, const char *filename)
{
	FILE *input;
	json_parser parser;
	int ret;

	input = open_filename(filename, "r", 1);
	if (!input)
		return 2;

	/* initialize the parser structure. we don't need a callback in verify */
	ret = json_parser_init(&parser, config, NULL, NULL);
	if (ret) {
		fprintf(stderr, "error: initializing parser failed (code=%d): %s\n", ret, string_of_errors[ret]);
		return ret;
	}

	ret = process_file(&parser, input, NULL, NULL);
	if (ret)
		return 1;

	ret = json_parser_is_done(&parser);
	if (!ret)
		return 1;
	
	close_filename(filename, input);
	return 0;
}

static int do_parse(json_config *config, const char *filename)
{
	FILE *input;
	json_parser parser;
	int ret;
	int col, lines;

	input = open_filename(filename, "r", 1);
	if (!input)
		return 2;

	/* initialize the parser structure. we don't need a callback in verify */
	ret = json_parser_init(&parser, config, NULL, NULL);
	if (ret) {
		fprintf(stderr, "error: initializing parser failed (code=%d): %s\n", ret, string_of_errors[ret]);
		return ret;
	}

	ret = process_file(&parser, input, &lines, &col);
	if (ret) {
		fprintf(stderr, "line %d, col %d: [code=%d] %s\n",
		        lines, col, ret, string_of_errors[ret]);
		return 1;
	}

	ret = json_parser_is_done(&parser);
	if (!ret) {
		fprintf(stderr, "syntax error\n");
		return 1;
	}
	
	close_filename(filename, input);
	return 0;
}

static int do_format(json_config *config, const char *filename)
{  
   char ext [10];
   char outputfile[1000];
	FILE *input, *output;
	json_parser parser;
	json_printer printer;
	int ret;
	int col, lines;
	strcpy(ext, ".json");

	input = open_filename(filename, "r", 1);
	if (!input)
		return 2;

	/* initialize printer and parser structures */
	ret = json_print_init(&printer, printchannel, stdout);
	if (ret) {
		fprintf(stderr, "error: initializing printer failed: [code=%d] %s\n", ret, string_of_errors[ret]);
		return ret;
	}
	if (indent_string)
		printer.indentstr = indent_string;

	ret = json_parser_init(&parser, config, &prettyprint, &printer);
	if (ret) {
		fprintf(stderr, "error: initializing parser failed: [code=%d] %s\n", ret, string_of_errors[ret]);
		return ret;
	}

	ret = process_file(&parser, input, &lines, &col);
	if (ret) {
		fprintf(stderr, "line %d, col %d: [code=%d] %s\n",
		        lines, col, ret, string_of_errors[ret]);
		return 1;
	}

	ret = json_parser_is_done(&parser);
	if (!ret) {
		fprintf(stderr, "syntax error\n");
		return 1;
	}

	/* cleanup */
	json_parser_free(&parser);
	json_print_free(&printer);
	fwrite("\n", 1, 1, stdout);
	
	close_filename(filename, input);
	return 0;
}


struct json_val_elem {
	char *key;
	uint32_t key_length;
	struct json_val *val;
};

typedef struct json_val {
	int type;
	int length;
	union {
		char *data;
		struct json_val **array;
		struct json_val_elem **object;
	} u;
} json_val_t;

static void *tree_create_structure(int nesting, int is_object)
{
	json_val_t *v = malloc(sizeof(json_val_t));
	if (v) {
		/* instead of defining a new enum type, we abuse the
		 * meaning of the json enum type for array and object */
		if (is_object) {
			v->type = JSON_OBJECT_BEGIN;
			v->u.object = NULL;
		} else {
			v->type = JSON_ARRAY_BEGIN;
			v->u.array = NULL;
		}
		v->length = 0;
	}
	return v;
}

static char *memalloc_copy_length(const char *src, uint32_t n)
{
	char *dest;

	dest = calloc(n + 1, sizeof(char));
	if (dest)
		memcpy(dest, src, n);
	return dest;
}

static void *tree_create_data(int type, const char *data, uint32_t length)
{
	json_val_t *v;

	v = malloc(sizeof(json_val_t));
	if (v) {
		v->type = type;
		v->length = length;
		v->u.data = memalloc_copy_length(data, length);
		if (!v->u.data) {
			free(v);
			return NULL;
		}
	}
	return v;
}

static int tree_append(void *structure, char *key, uint32_t key_length, void *obj)
{
	json_val_t *parent = structure;
	if (key) {
		struct json_val_elem *objelem;

		if (parent->length == 0) {
			parent->u.object = calloc(1 + 1, sizeof(json_val_t *)); /* +1 for null */
			if (!parent->u.object)
				return 1;
		} else {
			uint32_t newsize = parent->length + 1 + 1; /* +1 for null */
			void *newptr;

			newptr = realloc(parent->u.object, newsize * sizeof(json_val_t *));
			if (!newptr)
				return -1;
			parent->u.object = newptr;
		}

		objelem = malloc(sizeof(struct json_val_elem));
		if (!objelem)
			return -1;

		objelem->key = memalloc_copy_length(key, key_length);
		objelem->key_length = key_length;
		objelem->val = obj;
		parent->u.object[parent->length++] = objelem;
		parent->u.object[parent->length] = NULL;
	} else {
		if (parent->length == 0) {
			parent->u.array = calloc(1 + 1, sizeof(json_val_t *)); /* +1 for null */
			if (!parent->u.array)
				return 1;
		} else {
			uint32_t newsize = parent->length + 1 + 1; /* +1 for null */
			void *newptr;

			newptr = realloc(parent->u.object, newsize * sizeof(json_val_t *));
			if (!newptr)
				return -1;
			parent->u.array = newptr;
		}
		parent->u.array[parent->length++] = obj;
		parent->u.array[parent->length] = NULL;
	}
	return 0;
}

static int do_tree(json_config *config, const char *filename, json_val_t **root_structure)
{
	FILE *input;
	json_parser parser;
	json_parser_dom dom;
	int ret;
	int col, lines;

	input = open_filename(filename, "r", 1);
	if (!input)
		return 2;

	ret = json_parser_dom_init(&dom, tree_create_structure, tree_create_data, tree_append);
	if (ret) {
		fprintf(stderr, "error: initializing helper failed: [code=%d] %s\n", ret, string_of_errors[ret]);
		return ret;
	}

	ret = json_parser_init(&parser, config, json_parser_dom_callback, &dom);
	if (ret) {
		fprintf(stderr, "error: initializing parser failed: [code=%d] %s\n", ret, string_of_errors[ret]);
		return ret;
	}

	ret = process_file(&parser, input, &lines, &col);
	if (ret) {
		fprintf(stderr, "line %d, col %d: [code=%d] %s\n",
		        lines, col, ret, string_of_errors[ret]);

		return 1;
	}

	ret = json_parser_is_done(&parser);
	if (!ret) {
		fprintf(stderr, "syntax error\n");
		return 1;
	}

	if (root_structure)
		*root_structure = dom.root_structure;

	/* cleanup */
	json_parser_free(&parser);
	close_filename(filename, input);
	return 0;
}
int last_line_del(char *fm)
{


// our program will work by writing all of the original file content to a 
// temp file EXCEPT for the line we want to delete, and then we'll delete the 
// original file and rename the temp file to the original file name
  FILE *file, *temp;
  file = fopen(fm, "r");
  if (file == NULL)
  {
    printf("Error opening file.\n");
    return 1;
  }
  int current_lines = 1;
  char c;
  do 
  {
    c = fgetc(file);
    if (c == '\n') current_lines++;
  } while (c != EOF);
  fclose(file);
  char temp_filename[FILENAME_SIZE];
  char buffer[MAX_LINE];
  int delete_line = current_lines-1;
  strcpy(temp_filename, "temp____");
  strcat(temp_filename, fm);
  file = fopen(fm, "r");
  temp = fopen(temp_filename, "w");
  if (file == NULL || temp == NULL)
  {
    printf("Error opening file(s)\n");
    return 1;
  }
  bool keep_reading = true;
  int current_line = 1;
  do 
  {
    fgets(buffer, MAX_LINE, file);
    if (feof(file)) keep_reading = false;
    else if (current_line != delete_line)
      fputs(buffer, temp);
    // keeps track of the current line being read
    current_line++;
  } while (keep_reading);
  fclose(file);
  fclose(temp);
  remove(fm);
  rename(temp_filename, fm);
  
  return 0;
}

////////////////////////////////////////////////////////////////////////////////////
static int print_tree_json(json_val_t *element, FILE *output)
{
	int i;
	if (!element) {
		fprintf(stderr, "error: no element in print tree\n");
		return -1;
	}

	switch (element->type) {
	case JSON_OBJECT_BEGIN:
		fprintf(output, "{");
		for (i = 0; i < element->length; i++) {
			fprintf(output, "\"%s\"", element->u.object[i]->key);
			fprintf(output,":"); 
			print_tree_json(element->u.object[i]->val, output);
			 
			if (element->length > 1)
			   fprintf(output,",");
		}
		fprintf(output, "}\n,\n ");
		break;
	case JSON_ARRAY_BEGIN:
		fprintf(output, "[\n");
		for (i = 0; i < element->length; i++) {
			print_tree_json(element->u.array[i], output);
		}
		fprintf(output, "]\n");
		break;
	case JSON_FALSE:
	case JSON_TRUE:
	case JSON_NULL:
		fprintf(output, "constant\n");
		break;
	case JSON_INT:
		fprintf(output, "\"%s\"", element->u.data);
		break;
	case JSON_STRING:
		fprintf(output, "\"%s\"", element->u.data);
		break;
	case JSON_FLOAT:
		fprintf(output, "\"%s\"\n", element->u.data);
		break;
	default:
		break;
	}
	
	return 0;
}
//////////////////////////////////////////////////////////////////////
static int print_tree_iter(json_val_t *element, FILE *output)
{
	int i;
	if (!element) {
		fprintf(stderr, "error: no element in print tree\n");
		return -1;
	}

	switch (element->type) {
	case JSON_OBJECT_BEGIN:
		fprintf(output, "object begin (%d element)\n", element->length);
		for (i = 0; i < element->length; i++) {
			fprintf(output, "key: %s\n", element->u.object[i]->key);
			print_tree_iter(element->u.object[i]->val, output);
		}
		fprintf(output, "object end\n");
		break;
	case JSON_ARRAY_BEGIN:
		fprintf(output, "array begin\n");
		for (i = 0; i < element->length; i++) {
			print_tree_iter(element->u.array[i], output);
		}
		fprintf(output, "array end\n");
		break;
	case JSON_FALSE:
	case JSON_TRUE:
	case JSON_NULL:
		fprintf(output, "constant\n");
		break;
	case JSON_INT:
		fprintf(output, "integer: %s\n", element->u.data);
		break;
	case JSON_STRING:
		fprintf(output, "string: %s\n", element->u.data);
		break;
	case JSON_FLOAT:
		fprintf(output, "float: %s\n", element->u.data);
		break;
	default:
		break;
	}
	return 0;
}

//////////////////////////////////////////////////////////////////////
static int print_tree(json_val_t *root_structure)
{
	FILE *output;
	    char *outputfile = malloc(sizeof(char) * (1000));
	 //char ext[1000];
	 //strcpy(ext, ".json");
	 
	printf("Enter output file name : ");
   scanf("%s", outputfile);
   //strcat(outputfile, ext);

	output = open_filename(outputfile, "a+", 0);
	
	if (!output)
		return 2;
	print_tree_iter(root_structure, output);
	
	close_filename(outputfile, output);
	last_line_del(outputfile);
	return 0;
}
//////////////////////////////////////////////////////////////////////
static int print_json(json_val_t *root_structure)
{
	FILE *output;
	    char *outputfile = malloc(sizeof(char) * (1000));
	 char ext[1000];
	 strcpy(ext, ".json");
	 
	printf("Enter output file name : ");
   scanf("%s", outputfile);
   strcat(outputfile, ext);

	output = open_filename(outputfile, "a+", 0);
	
	if (!output)
		return 2;
	print_tree_json(root_structure, output);
	
	close_filename(outputfile, output);
	last_line_del(outputfile);
	return 0;
}
/////////////////////////////////////////////////////
static char *output_tree(json_val_t *root_structure)
{
	FILE *output;
	    char *outputfile = malloc(sizeof(char) * (10));
	 char ext[1000];
	 strcpy(ext, "temp.json");
	 strcpy(outputfile, "");
	 
   strcat(outputfile, ext);

	output = open_filename(outputfile, "a+", 0);
	
	if (!output)
		exit;
	print_tree_json(root_structure, output);
	
	close_filename(outputfile, output);
	last_line_del(outputfile);
	return outputfile;
}


int Export(int argc, char **argv)
{
int format = 0, verify = 0, use_tree = 0, benchmarks = 0;
	int ret = 0, i;
	json_config config;
	char *output = "-";

	memset(&config, 0, sizeof(json_config));
	config.max_nesting = 0;
	config.max_data = 0;
	config.allow_c_comments = 1;
	config.allow_yaml_comments = 1;
	
	ret = do_verify(&config, argv[1]);
	ret = do_parse(&config, argv[1]);
	ret = do_format(&config, argv[1]);
	//////////////////////////////////
			json_val_t *root_structure;
			ret = do_tree(&config, argv[1], &root_structure);
			print_tree(root_structure);
			
	
	printf(ANSI_COLOR_GREEN   "DONE"   ANSI_COLOR_RESET "\n");
	
			
				
		
	if (ret)
		exit(ret);
	
	return ret;

}

static int do_errdet(json_config *config, const char *filename, const char *outputfile)
{
	FILE *input, *output;
	json_parser parser;
	json_printer printer;
	int ret;
	int col, lines;

	input = open_filename(filename, "r", 1);
	if (!input)
		return 2;

	output = open_filename(outputfile, "a+", 0);
	if (!output)
		return 2;

	/* initialize printer and parser structures */
	ret = json_print_init(&printer, printchannel, stdout);
	if (ret) {
		fprintf(stderr, "error: initializing printer failed: [code=%d] %s\n", ret, string_of_errors[ret]);
		return ret;
	}
	if (indent_string)
		printer.indentstr = indent_string;

	ret = json_parser_init(&parser, config, &prettyprint, &printer);
	if (ret) {
		fprintf(stderr, "error: initializing parser failed: [code=%d] %s\n", ret, string_of_errors[ret]);
		return ret;
	}

	ret = process_file(&parser, input, &lines, &col);
	if (ret) {
		fprintf(stderr, "line %d, col %d: [code=%d] %s\n",
		        lines, col, ret, string_of_errors[ret]);
		return 1;
	}

	ret = json_parser_is_done(&parser);
	if (!ret) {
		fprintf(stderr, "syntax error\n");
		return 1;
	}

	/* cleanup */
	json_parser_free(&parser);
	json_print_free(&printer);
	fwrite("\n", 1, 1, stdout);
	close_filename(filename, input);
	return 0;
}

int error_det (int argc, char **argv)

{
int format = 1, verify = 0, use_tree = 0, benchmarks = 0;
	int ret = 0, i;
	json_config config;
	char *output = "-";

	memset(&config, 0, sizeof(json_config));
	config.max_nesting = 0;
	config.max_data = 0;
	config.allow_c_comments = 1;
	config.allow_yaml_comments = 1;

		
	if (!output)
		output = "-";


	for (i = optind; i < argc; i++) {
		
			if (format)
				{ret = do_errdet(&config, argv[i], output);
				printf("\n");
				printf("_________________________________\n\n");}
			else if (verify)
				ret = do_verify(&config, argv[i]);
			else
				ret = do_parse(&config, argv[i]);
		}
		if (ret)
		printf("\n");
			exit(ret);
			
	
	return ret;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////

#define FILENAME_SIZE 1024


#define MAX_SIZE 100

//da main function
int new_getter(int argc, char **argv)
{
int ret = 0, i;
	json_config config;
	char *output = "-";
  // prompt the user for filename of the file to read, store it into filename
  json_val_t *root_structure;
			ret = do_tree(&config, argv[1], &root_structure);
  char *outpt= malloc(sizeof(char) * (1000));
	outpt=output_tree(root_structure);
  int count;
  // read the file contents of the file
  char *file_contents = read_file(outpt);
  char toSearch[MAX_SIZE];
  printf("Enter key : ");
    scanf("%s",toSearch);

  // if there was an error reading the file, file_contents will be set to NULL,
  // handle the error gracefully with an error message and error return status
  if (file_contents == NULL)
  {
    printf("Error reading file.\n");
    return 1;
  }
  
  
  // output the file contents to verify they were read into the string correctly
  //printf("File Contents:\n\n%s\n", file_contents);
  count = countOccurrences(file_contents, toSearch);

    printf("Total occurrences of '%s': %d \n", toSearch, count);
    
    if(count>1)
    {printf("be more specific !!!!\n");
    num_line(outpt);
    char dummy[1000];
	strcpy(dummy,specline(outpt));
	if (strlen(dummy)>2)
	hunter(toSearch,dummy);
	else 
	printf("error the line you picked is invalid \n");
	return 1;
	}
	else 
	hunter(toSearch,file_contents);
	
	
  
  // free the dynamically allocated memory as a best practice to prevent a 
  // memory leak from occurring
  free(file_contents);
  remove(outpt);

  return 0;
}

// Reads and stores the whole contents of the file with filename into a 
// dynamically allocated char array on the heap, returns a pointer to this char
// array (or NULL if there was an error reading the file contents)/
char *read_file(char *filename)
{
  // file pointer variable used to access the file
  FILE *file;
  
  // attempt to open the file in read mode
  file = fopen(filename, "r");
  
  // if the file fails to open, return NULL as an error return value
  if (file == NULL) return NULL;
  
  // move the file pointer to the end of the file
  fseek(file, 0, SEEK_END); 

  // fseek(file) will return the current value of the position indicator, 
  // which will give us the number of characters in the file
  int length = ftell(file);

  // move file pointer back to start of file so we can read each character
  fseek(file, 0, SEEK_SET);
  
  // dynamically allocate a char array to store the file contents, we add 1 to 
  // length for the null terminator we will need to add to terminate the string
  char *string = malloc(sizeof(char) * (length+1));
  
  // c will store each char we read from the string
  char c;

  // i will be an index into the char array string as we read each char
  int i = 0;

  // keep reading each char from the file until we reach the end of the file
  while ( (c = fgetc(file)) != EOF)
  {
    // store char into the char array string
    string[i] = c;

    // increment i so we store the next char in the next index in the char array
    i++;
  }

  // put a null terminator as the final char in the char array to properly 
  // terminate the string
  string[i] = '\0';
  
  // close the file as we are now done with it
  fclose(file);
  
  // return a pointer to the dynamically allocated string on the heap
  return string;
}

int countOccurrences(char * str, char * toSearch)
{
    int i, j, found, count;
    int stringLen, searchLen;

    stringLen = strlen(str);      // length of string
    searchLen = strlen(toSearch); // length of word to be searched

    count = 0;

    for(i=0; i <= stringLen-searchLen; i++)
    {
        /* Match word with string */
        found = 1;
        for(j=0; j<searchLen; j++)
        {
            if(str[i + j] != toSearch[j])
            {
                found = 0;
                break;
            }
        }

        if(found == 1)
        {
            count++;
        }
    }

    return count;
}
char *specline(char *arg)
{
  // file pointer will be used to open/read the file
  FILE *file, *fp;

  // used to store the filename and each line from the file
 
  //char buffer[MAX_LINE];
   

  // stores the line number of the line the user wants to read from the file
  int read_line = 0;
  

  // prompt the user for the filename, store it into filename


  // prompt the user for the line to read, store it into read_line
  printf("Line N°: ");
  scanf("%d", &read_line);

  // open the the file in read mode
  file = fopen(arg, "r");
  char *buffer = malloc(sizeof(char) * (3000));
	
	
	
    int count = 0;  // Line counter (result)
    char c;  // To store a character read from file
 
    // Get file name from user. The file should be
    // either in current folder or complete path should be provided
    
 
    // Open the file
    fp = fopen(arg, "r");
 
    // Check if file exists
    if (fp == NULL)
    {
        printf("Could not open file %s", arg);
  	exit;
    }
 
    // Extract characters from file and store in character c
    for (c = getc(fp); c != EOF; c = getc(fp))
        if (c == '\n') // Increment count if this character is newline
            count = count + 1;
 
    // Close the file
    fclose(fp);
  // if the file failed to open, exit with an error message and status
  if (file == NULL)
  {
    printf("Error opening file.\n");
    exit;
  }

  // we'll keep reading the file so long as keep_reading is true, and we'll 
  // keep track of the current line of the file using current_line
  bool keep_reading = true;
  int current_line = 1;
  do 
  {
    // read the next line from the file, store it into buffer
    fgets(buffer, MAX_LINE, file);

    // if we've reached the end of the file, we didn't find the line
    if (feof(file))
    {
      // stop reading from the file, and tell the user the number of lines in 
      // the file as well as the line number they were trying to read as the 
      // file is not large enough
      keep_reading = false;
      printf("File %d lines.\n", current_line-1);
      printf("Couldn't find line %d.\n", read_line);
    }
    // if we've found the line the user is looking for, print it out
    else if (current_line == read_line)
    {
    	
      keep_reading = false;
      
     
      
     int buffersz;
  buffersz=strlen(buffer);
  buffer[buffersz]='\0';
     
      
    }

    // continue to keep track of the current line we are reading
    current_line++;

  } while (keep_reading);
 // for (current_line=read_line ;current_line < count ; current_line ++)
 // {fgets(buffer, MAX_LINE, file);  
 // printf("\n%s", buffer);}
  
  
  

  // close our access to the file
  fclose(file);

  // notably at this point in the code, buffer will contain the line of the 
  // file we were looking for if it was found successfully, so it could be 
  // used for other purposes at this point as well...

  return buffer;
}



int num_line(  char *filename)
{
  // file pointers to access the original file and temp file
  FILE *file, *temp;

  // used to point to the filename command-line argument


  // stores the temporary filename
  char temp_filename[FILENAME_SIZE];

  // stores each line of the original file
  char buffer[MAX_LINE];

  // check to make sure the correct number of command-line arguments is 
  // provided, if it is not then exit with an error message and status
  // otherwise set filename to point to the 2nd command line argument string

  
  // build a temporary filename by first copying the string "temp____" into the 
  // temp_filename char array, and then appending on the original filename, so
  // if the name of the original file was file.txt we'll have "temp____file.txt"
  strcpy(temp_filename, "templine____");
  strcat(temp_filename, filename);
  
  // open the original file for reading, and the temp file for writing
  file = fopen(filename, "r");
  temp = fopen(temp_filename, "w");
  
  // if either or both files failed to open, exit with an error message & status
  if (file == NULL || temp == NULL)
  {
    printf("Error opening file.\n");
    return 1;
  }
  
  // keep track of the current line number to print on each line of temp file
  int current_line = 1;
  
  // read each line of the original file into the buffer until we reach the end
  // of the file
  while (fgets(buffer, MAX_LINE, file) != NULL)
  {
    // write the int value of current_line to the temp file, followed by a 
    // space, followed by the line in the original file
    fprintf(temp, "%d %s", current_line, buffer);

    // increment the current line so that it is the correct value for the next
    // line in the file
    current_line++;
  }
  
  // close both files
  fclose(temp);
  fclose(file);
  
  // delete the original file, and rename the temporary file to the original
  // file's name
  //remove(filename);
  //rename(temp_filename, filename);
  
  //display 
  FILE *fh;
  fh = fopen(temp_filename, "r");
  if (fh != NULL)
  {
    char c;
    while ( (c = fgetc(fh)) != EOF )
      putchar(c);
    printf("\n");
    fclose(fh);
  } else printf("Error opening file.\n");
  
	remove(temp_filename);
  return 0;
} 


//this shit will output shit that comes after some shit


int hunter(char key[3000],char content[3000]){
    
 
    int index = -1;
    for (int i = 0; content[i] != '\0'; i++) {
        index = -1;
        for (int j = 0; key[j] != '\0'; j++) {
            if (content[i + j] != key[j]) {
                index = -1;
                break;
            }
            index = i;
        }
        if (index != -1) {
            break;
        }
    }
    char *chk = malloc(1);
    char subchk [sizeof(":")];
    
    strncpy(chk,&content[index-2],1);
    strncpy(subchk,chk,1);
  
    //printf("chk is  %s \n",subchk);
    if (strcmp(subchk,":")==0)
    {printf(ANSI_COLOR_RED   "FATAL ERROR !!!"   ANSI_COLOR_RESET "\n THE DATAT YOU ENTERED IS A " ANSI_COLOR_RED "VALUE" ANSI_COLOR_RESET);
    return 1;}
    if (index == -1 )
    {printf("key not found \n");
    return 1;}
    
    
    
    char val[3000];
    strcpy(val,&content[index+strlen(key)+2]);
    char sep[1000];
    strcpy(sep,"\",");
    for (int i = 0; val[i] != '\0'; i++) {
        index = -1;
        for (int j = 0; sep[j] != '\0'; j++) {
            if (val[i + j] != sep[j]) {
                index = -1;
                break;
            }
            index = i;
          
        }
        if (index != -1) {
        
            break;
        }
    }
   
    if (index>0)
    
    {char res[(strlen(val)-strlen(&val[index]))];
    strncpy(res,val,(strlen(val)-strlen(&val[index])));
    printf("value is %s \n",&res[1]);}
    else {
    strcpy(sep,"\"}");
    for (int i = 0; val[i] != '\0'; i++) {
        index = -1;
        for (int j = 0; sep[j] != '\0'; j++) {
            if (val[i + j] != sep[j]) {
                index = -1;
                break;
            }
            index = i;
          
        }
        if (index != -1) {
        
            break;
        }
    }
    
    if (index>0)
    
    {char res[(strlen(val)-strlen(&val[index]))];
    strncpy(res,val,(strlen(val)-strlen(&val[index])));
    
    printf("value is %s \n",&res[1]);}
    else 
    {strcpy(sep,"[");
    for (int i = 0; content[i] != '\0'; i++) {
        index = -1;
        for (int j = 0; sep[j] != '\0'; j++) {
            if (content[i + j] != sep[j]) {
                index = -1;
                break;
            }
            index = i;
          
        }
        if (index != -1) {
        
            break;
        }
    }
    if (index>0)
    printf("array \n");}
    }
    
    
    
   
    
    
     

    return 0;
}


/////////////////////////////////////////////////////////

int add (int argc, char **argv)
{


// our program will work by writing all of the original file content to a 
// temp file EXCEPT for the line we want to delete, and then we'll delete the 
// original file and rename the temp file to the original file name
  FILE *file, *temp;
  file = fopen(argv[1], "r");
  if (file == NULL)
  {
    printf("Error opening file.\n");
    return 1;
  }
  int current_lines = 1;
  char c;
  do 
  {
    c = fgetc(file);
    if (c == '\n') current_lines++;
  } while (c != EOF);
  fclose(file);
  char temp_filename[FILENAME_SIZE];
  char buffer[MAX_LINE];
  int delete_line = current_lines-1;
  strcpy(temp_filename, "temp____");
  strcat(temp_filename, argv[1]);
  file = fopen(argv[1], "r");
  temp = fopen(temp_filename, "w");
  if (file == NULL || temp == NULL)
  {
    printf("Error opening file(s)\n");
    return 1;
  }
  bool keep_reading = true;
  int current_line = 1;
  do 
  {
    fgets(buffer, MAX_LINE, file);
    if (feof(file)) keep_reading = false;
    else if (current_line != delete_line)
      fputs(buffer, temp);
    // keeps track of the current line being read
    current_line++;
  } while (keep_reading);
  fclose(file);
  fclose(temp);
  remove(argv[1]);
  rename(temp_filename, argv[1]);
  
  FILE *pFile;

char str[256];

pFile=fopen(argv[1], "a");
char all[1000];
    char sep[1000];
    char middle[10];
    char end [10];
    char stopper[10];
    char elkey[256];
    char elval[256];
    char blanc_space[10];
     int choice = 0;
 
    printf("give key : ");
	scanf("%s", elkey);
	printf("sprecify the type of data of your value \n");
	printf(ANSI_COLOR_GREEN"------->1) sring \n" ANSI_COLOR_RESET );
   printf(ANSI_COLOR_GREEN"------->2) integer \n" ANSI_COLOR_RESET );
    //printf("3) object\n");
    //printf("4) array\n");
	scanf("%d", &choice);
	if (choice != 1 && choice != 2)
	{printf(ANSI_COLOR_RED   "ERROR !!!!\n Unlisted choice "   ANSI_COLOR_RESET "\n");
	pFile=fopen(argv[1], "a");
	strcpy(stopper, "\n \n}");
            fputs(stopper, pFile);
            fclose(pFile);
	return 1;}
    
    
    switch (choice)
    {
      
      case 1:
        printf("give value (string): ");
	scanf("%s", elval);
    // Copy the first string into
    // the variable
    strcpy(all, "\n,\"");
    strcpy(sep, "\"");
    strcpy(middle, ":");
    strcpy(stopper, "\n \n}");
   
    // Concatenate this string
    // to the end of the first one
    strcat(elkey, sep);
    strcat(elkey, middle);
    strcat(elkey, sep);
    strcat(elkey, elval);
    strcat(elkey, sep);
    strcat(elkey, stopper);
    strcat(all, elkey);
        break;
        
        
        case 2:
        	printf("\n");
        
        printf("give value (integer): ");
	scanf("%s", elval);

	int length=strlen(elval);
	int i;
	for (i=0;i<length; i++)
       { if(!isdigit(elval[i]))
            {printf(ANSI_COLOR_RED   "FATAL ERROR !!! \n user entered wrong type of data "   ANSI_COLOR_RESET "\n");
            strcpy(stopper, "\n \n}");
            fputs(stopper, pFile);
            exit(1);}
            
            }
            
	
    // Copy the first string into
    // the variable
    strcpy(all, "\n,\"");
    strcpy(sep, "\"");
    strcpy(middle, ":");
    strcpy(stopper, "\n \n}");
     strcpy(blanc_space, " ");
    // Concatenate this string
    // to the end of the first one
    strcat(elkey, sep);
    strcat(elkey, middle);
    strcat(elkey, blanc_space);
    strcat(elkey, elval);
    strcat(elkey, blanc_space);
    strcat(elkey, stopper);
    strcat(all, elkey); 
        break;
      
    }
	

if ( pFile )
   {
	           fputs(all, pFile);
    }
   else
      {
         printf("Failed to open the file\n");
        }
	fclose(pFile);
	printf(ANSI_COLOR_GREEN   "DONE"   ANSI_COLOR_RESET "\n");
	
	  
 
  return 0;
}

int Export_to_json(int argc, char **argv)
{
int format = 0, verify = 0, use_tree = 0, benchmarks = 0;
	int ret = 0, i;
	json_config config;
	char *output = "-";

	memset(&config, 0, sizeof(json_config));
	config.max_nesting = 0;
	config.max_data = 0;
	config.allow_c_comments = 1;
	config.allow_yaml_comments = 1;
	
	ret = do_verify(&config, argv[1]);
	ret = do_parse(&config, argv[1]);
	ret = do_format(&config, argv[1]);
	//////////////////////////////////
			json_val_t *root_structure;
			ret = do_tree(&config, argv[1], &root_structure);
			print_json(root_structure);
			
	
	printf(ANSI_COLOR_CYAN   "DONE"   ANSI_COLOR_RESET "\n");
	
			
				
		
	if (ret)
		exit(ret);
	
	return ret;

}

////////////////////////////////////////////////////////////////////////////////////////////
int subupdate(char *argy,char oldWord[2000],char newWord[2000]);
int updatev2(int argc, char **argv)
{
int ret = 0, i;
	json_config config;
	char *output = "-";
  // prompt the user for filename of the file to read, store it into filename
  json_val_t *root_structure;
			ret = do_tree(&config, argv[1], &root_structure);
  char *outpt= malloc(sizeof(char) * (1000));
	outpt=output_tree(root_structure);
  int count;
  // read the file contents of the file
  char *file_contents = read_file(outpt);
  char oldWord[100], newWord[100];



    printf("Enter key or value  to update : ");
    scanf("%s", oldWord);

    printf("new value / key  : ");
    scanf("%s", newWord);

  // if there was an error reading the file, file_contents will be set to NULL,
  // handle the error gracefully with an error message and error return status
  if (file_contents == NULL)
  {
    printf("Error reading file.\n");
    return 1;
  }
  
  
  // output the file contents to verify they were read into the string correctly
  //printf("File Contents:\n\n%s\n", file_contents);
  count = countOccurrences(file_contents, oldWord);

    printf("Total occurrences of '%s': %d \n", oldWord, count);
    
    if(count>1)
    {printf("be more specific !!!!\n");
    num_line(outpt);
    char dummy[1000];
	strcpy(dummy,specline(outpt));
	if (strlen(dummy)>2)
	{
	printf("%s \n ", dummy);
	FILE * fTempo;
	fTempo = fopen("tempo.txt", "w"); 
	fputs(dummy, fTempo);
	fclose(fTempo);
	subupdate("tempo.txt",oldWord,newWord);
	char *tempo_contents = read_file("tempo.txt");
	subupdate(argv[1],dummy,tempo_contents);
	remove("tempo.txt");
	 
	


   }
	else 
	printf("error the line you picked is invalid \n");
	return 1;
	}
	else 
	printf("\n");
	
	
  
  // free the dynamically allocated memory as a best practice to prevent a 
  // memory leak from occurring
  free(file_contents);
  remove(outpt);

  return 0;
}

int subupdate(char *argy,char oldWord[2000],char newWord[2000])
{
    /* File pointer to hold reference of input file */
    FILE * fPtr;
    FILE * fTemp;
    char buffer[2000];
    


    /*  Open all required files */
    fPtr  = fopen(argy, "r");
    fTemp = fopen("replace.tmp", "w"); 

    /* fopen() return NULL if unable to open file in given mode. */
    if (fPtr == NULL || fTemp == NULL)
    {
        /* Unable to open file hence exit */
        printf("\nUnable to open file.\n");
        printf("Please check whether file exists and you have read/write privilege.\n");
        exit(EXIT_SUCCESS);
    }


    /*
     * Read line from source file and write to destination 
     * file after replacing given word.
     */
    while ((fgets(buffer, 2000, fPtr)) != NULL)
    {
        // Replace all occurrence of word from current line
        replaceAll(buffer, oldWord, newWord);

        // After replacing write it to temp file.
        fputs(buffer, fTemp);
    }


    /* Close all files to release resource */
    fclose(fPtr);
    fclose(fTemp);


    /* Delete original source file */
    remove(argy);

    /* Rename temp file as original file */
    rename("replace.tmp", argy);

    printf(ANSI_COLOR_GREEN   "DONE " ANSI_COLOR_RESET "\nSuccessfully replaced "ANSI_COLOR_RED" %s "ANSI_COLOR_RESET " with "ANSI_COLOR_CYAN " %s \n" ANSI_COLOR_RESET, oldWord, newWord);
    printf("\n");

    return 0;
}

///////////////////////////////////////////////////
static int print_tree_gv2(json_val_t *root_structure)
{
	FILE *output;
	    char outputfile[7];
	 //char ext[1000];
	 //strcpy(ext, ".json");
	 
	strcpy(outputfile,"test");
   //strcat(outputfile, ext);

	output = open_filename(outputfile, "a+", 0);
	
	if (!output)
		return 2;
	print_tree_iter(root_structure, output);
	
	close_filename(outputfile, output);
	//last_line_del(outputfile);
	return 0;
}
int Export_gv2(int argc, char **argv)
{
int format = 0, verify = 0, use_tree = 0, benchmarks = 0;
	int ret = 0, i;
	json_config config;
	char *output = "-";

	memset(&config, 0, sizeof(json_config));
	config.max_nesting = 0;
	config.max_data = 0;
	config.allow_c_comments = 1;
	config.allow_yaml_comments = 1;
	
	ret = do_verify(&config, argv[1]);
	ret = do_parse(&config, argv[1]);
	ret = do_format(&config, argv[1]);
	//////////////////////////////////
			json_val_t *root_structure;
			ret = do_tree(&config, argv[1], &root_structure);
			print_tree_gv2(root_structure);
			
	
			
				
		printf(ANSI_COLOR_CYAN   "#####################################"   ANSI_COLOR_RESET "\n");
	if (ret)
		exit(ret);
	
	return ret;

}

int getterv2(int argc, char **argv)
{
Export_gv2(argc,argv);
  // file pointers to access the original file and temp file
  FILE *file, *temp;

  // used to point to the filename command-line argument
  char filename[7];
  strcpy(filename,"test");

  // stores the temporary filename
  char temp_filename[FILENAME_SIZE];

  // stores each line of the original file
  char buffer[MAX_LINE];

  // check to make sure the correct number of command-line arguments is 
  // provided, if it is not then exit with an error message and status
  // otherwise set filename to point to the 2nd command line argument string

  
  // build a temporary filename by first copying the string "temp____" into the 
  // temp_filename char array, and then appending on the original filename, so
  // if the name of the original file was file.txt we'll have "temp____file.txt"
  strcpy(temp_filename, "temp____");
  strcat(temp_filename, filename);
  
  // open the original file for reading, and the temp file for writing
  file = fopen(filename, "r");
  temp = fopen(temp_filename, "w");
  
  // if either or both files failed to open, exit with an error message & status
  if (file == NULL || temp == NULL)
  {
    printf("Error opening file.\n");
    return 1;
  }
  
  // keep track of the current line number to print on each line of temp file
  int current_line = 1;
  
  // read each line of the original file into the buffer until we reach the end
  // of the file
  while (fgets(buffer, MAX_LINE, file) != NULL)
  {
    // write the int value of current_line to the temp file, followed by a 
    // space, followed by the line in the original file
    fprintf(temp, "%d %s", current_line, buffer);

    // increment the current line so that it is the correct value for the next
    // line in the file
    current_line++;
  }
  
  // close both files
  fclose(temp);
  fclose(file);
  
  // delete the original file, and rename the temporary file to the original
  // file's name
  //remove(filename);
  //rename(temp_filename, filename);
  
  
  //display
  
  // fh is the file handle we use to access the file
  FILE *fh;
  
  // open the file in "read mode"
  fh = fopen(temp_filename, "r");
  
  // fopen will return NULL if the file wasn't opened successfully, so we 
  // make sure it has opened OK before accessing the file
  if (fh != NULL)
  {
    // read each character of the file one at a time until end of file (EOF) is 
    // returned to signify the end of the file, output each char to the console
    char c;
    while ( (c = fgetc(fh)) != EOF )
      putchar(c);
    
    // close the file handle as we are done with the file
    fclose(fh);
  
  // if there was a problem opening the file, output an error message
  } else printf("Error opening file.\n");
  
  
  //read line 
    int read_line = 0;
   printf("indicate key line : ");
  scanf("%d", &read_line);

  // open the the file in read mode
  file = fopen(filename, "r");

  // if the file failed to open, exit with an error message and status
  if (file == NULL)
  {
    printf("Error opening file.\n");
    return 1;
  }
printf("<========================> \n");
  // we'll keep reading the file so long as keep_reading is true, and we'll 
  // keep track of the current line of the file using current_line
  bool keep_reading = true;
  int cur_line = 1;
  do 
  {
    // read the next line from the file, store it into buffer
    fgets(buffer, MAX_LINE, file);

    // if we've reached the end of the file, we didn't find the line
    if (feof(file))
    {
      // stop reading from the file, and tell the user the number of lines in 
      // the file as well as the line number they were trying to read as the 
      // file is not large enough
      keep_reading = false;
      printf("File %d lines.\n", cur_line-1);
      printf("Couldn't find line %d.\n", read_line);
    }
    // if we've found the line the user is looking for, print it out
    else if (cur_line == read_line)
    {
    	if (buffer[0]!='k')
    	{printf("the line you chose doesn't contain a key  \n");
    	printf("<========================> \n");
    	remove(temp_filename);
  remove("test");
    	return 1;
    	}
    	else
      {keep_reading = false;
     
      printf("\n%s", buffer);
       fgets(buffer, MAX_LINE, file);
       int ts1 ,ts2 ,tsy;
       
       char array[13];
       strcpy(array , "object begin");
       //array test
       ts1 =strncmp(buffer,"array begin",11);
       if (ts1==0)
       {printf("\n%s\n", buffer);}
       //object test
       ts2 =strncmp(buffer,"object begin",12);
       if (ts2==0)
       {printf("\n%s\n", buffer);}
	
       //array exec
       if (ts1==0)
       {do 
       {	
       	fgets(buffer, MAX_LINE, file);
       	printf("\n%s\n", buffer);
       	tsy =strncmp(buffer,"array end",9);} while(tsy!=0)
       ;}
       //object exec
       else if (ts2==0)
       {do 
       {	
       	fgets(buffer, MAX_LINE, file);
       	printf("\n%s\n", buffer);
       	tsy =strncmp(buffer,"object end",10);} while(tsy!=0)
       ;}
       //string ,integer or float exec
       else 
       {printf("\n%s\n", buffer);}
	
	
	}     
    }

    // continue to keep track of the current line we are reading
    cur_line++;
     

  } while (keep_reading);
  
printf("<========================> \n");
  // close our access to the file
  fclose(file);
remove(temp_filename);
  remove("test");

  return 0;
} 


