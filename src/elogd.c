/********************************************************************
 Name:         elogd.c
 Created by:   Stefan Ritt

 Contents:     Web server program for Electronic Logbook ELOG

 $Id$

 \********************************************************************/

/* Version of ELOG */
#define VERSION "2.7.5"
char svn_revision[] = "$Id$";

/* ELOG identification */
static const char ELOGID[] = "elogd " VERSION " built " __DATE__ ", " __TIME__;

#define _GNU_SOURCE
#include <stdio.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <assert.h>
#include <locale.h>

/* Default name of the configuration file. */
#ifndef CFGFILE
#define CFGFILE "elogd.cfg"
#endif

/* Default TCP port for server. */
#ifndef DEFAULT_PORT
#define DEFAULT_PORT 80
#endif

#ifdef _MSC_VER

#define OS_WINNT

#define DIR_SEPARATOR '\\'
#define DIR_SEPARATOR_STR "\\"

#define snprintf _snprintf

#include <windows.h>
#include <io.h>
#include <conio.h>
#include <time.h>
#include <direct.h>
#include <sys/stat.h>

#else

#define OS_UNIX

#ifdef __APPLE__
#define OS_MACOSX
#endif

#define TRUE 1
#define FALSE 0

#ifndef O_TEXT
#define O_TEXT 0
#define O_BINARY 0
#endif

#define DIR_SEPARATOR '/'
#define DIR_SEPARATOR_STR "/"

#ifndef DEFAULT_USER
#define DEFAULT_USER "nobody"
#endif

#ifndef DEFAULT_GROUP
#define DEFAULT_GROUP "nogroup"
#endif

#ifndef PIDFILE
#define PIDFILE "/var/run/elogd.pid"
#endif

#ifndef __USE_XOPEN
#define __USE_XOPEN             /* needed for crypt() */
#endif

typedef int BOOL;

#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include <syslog.h>
#include <termios.h>

#define closesocket(s) close(s)

#ifndef stricmp
#define stricmp(s1, s2) strcasecmp(s1, s2)
#endif

gid_t orig_gid;                 /* Original effective GID before dropping privilege */
uid_t orig_uid;                 /* Original effective UID before dropping privilege */
char pidfile[256];              /* Pidfile name                                     */

#endif                          /* OS_UNIX */

#ifdef __CYGWIN__               /* bug in cygwin, 'tmezone' not linked automatically */
long _timezone;
#endif

/* SSL includes */
#ifdef HAVE_SSL
#include <openssl/ssl.h>
#endif

/* local includes */
#include "regex.h"
#include "mxml.h"
#include "strlcpy.h"

BOOL running_as_daemon;         /* Running as a daemon/service? */
int elog_tcp_port;              /* Server's TCP port            */

static void (*printf_handler) (const char *);   /* Handler to printf for logging */
static void (*fputs_handler) (const char *);    /* Handler to fputs for logging  */
static FILE *current_output_stream = NULL;      /* Currently used output stream  */

#define SYSLOG_PRIORITY LOG_NOTICE      /* Default priority for syslog facility */

#define TELL(fh) lseek(fh, 0, SEEK_CUR)

#ifdef OS_WINNT
#define TRUNCATE(fh) chsize(fh, TELL(fh))
#else
#define TRUNCATE(fh) ftruncate(fh, TELL(fh))
#endif

#define NAME_LENGTH  1500

#define DEFAULT_TIME_FORMAT "%c"
#define DEFAULT_DATE_FORMAT "%x"

#define DEFAULT_HTTP_CHARSET "ISO-8859-1"

#define SUCCESS        1
#define FAILURE        0

#define EL_SUCCESS     1
#define EL_FIRST_MSG   2
#define EL_LAST_MSG    3
#define EL_NO_MSG      4
#define EL_FILE_ERROR  5
#define EL_UPGRADE     6
#define EL_EMPTY       7
#define EL_MEM_ERROR   8
#define EL_DUPLICATE   9
#define EL_INVAL_FILE 10

#define EL_FIRST       1
#define EL_LAST        2
#define EL_NEXT        3
#define EL_PREV        4

char *return_buffer;
int return_buffer_size;
int strlen_retbuf;
int keep_alive;
char header_buffer[20000];
int return_length;
char host_name[256];
char referer[256];
char browser[256];
char config_file[256];
char resource_dir[256];
char logbook_dir[256];
char listen_interface[256];
char theme_name[80];
char http_host[256];

#define MAX_GROUPS       32
#define MAX_PARAM       120
#define MAX_ATTACHMENTS  50
#define MAX_N_LIST      100
#define MAX_N_ATTR      100
#define MAX_REPLY_TO    100
#define CMD_SIZE      10000
#define TEXT_SIZE    250000
#define MAX_PATH_LENGTH 256

#define MAX_CONTENT_LENGTH 10*1024*1024

char _param[MAX_PARAM][NAME_LENGTH];
char _value[MAX_PARAM][NAME_LENGTH];
char _mtext[TEXT_SIZE];
char _cmdline[CMD_SIZE];
char *_attachment_buffer;
int _attachment_size;
int _max_content_length = MAX_CONTENT_LENGTH;
struct in_addr rem_addr;
char rem_host[256];
char rem_host_ip[256];
int _sock;
BOOL use_keepalive, enable_execute = FALSE;
BOOL fckedit_exist, image_magick_exist;
int verbose, _current_message_id;
int _logging_level, _ssl_flag;

char *mname[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September",
   "October", "November", "December"
};

char type_list[MAX_N_LIST][NAME_LENGTH] = { "Routine", "Other" };

char category_list[MAX_N_LIST][NAME_LENGTH] = { "General", "Other",
};

char author_list[MAX_N_LIST][NAME_LENGTH] = { "" };

/* attribute flags */
#define AF_REQUIRED           (1<<0)
#define AF_LOCKED             (1<<1)
#define AF_MULTI              (1<<2)
#define AF_FIXED_EDIT         (1<<3)
#define AF_FIXED_REPLY        (1<<4)
#define AF_ICON               (1<<5)
#define AF_RADIO              (1<<6)
#define AF_EXTENDABLE         (1<<7)
#define AF_DATE               (1<<8)
#define AF_DATETIME           (1<<9)
#define AF_TIME              (1<<10)
#define AF_NUMERIC           (1<<11)
#define AF_USERLIST          (1<<12)
#define AF_MUSERLIST         (1<<13)
#define AF_USEREMAIL         (1<<14)
#define AF_MUSEREMAIL        (1<<15)

/* attribute format flags */
#define AFF_SAME_LINE              1
#define AFF_MULTI_LINE             2
#define AFF_DATE                   4
#define AFF_EXTENDABLE             8

char attr_list[MAX_N_ATTR][NAME_LENGTH];
char attr_options[MAX_N_ATTR][MAX_N_LIST][NAME_LENGTH];
int attr_flags[MAX_N_ATTR];

char attr_list_default[][NAME_LENGTH] = { "Author", "Type", "Category", "Subject", "" };

char attr_options_default[][MAX_N_LIST][NAME_LENGTH] = { {""}, {"Routine", "Other"},
{"General", "Other"}, {""}
};

int attr_flags_default[] = { AF_REQUIRED, 0, 0, 0 };

struct {
   char ext[32];
   char type[32];
} filetype[] = {

   {
   ".AI", "application/postscript"}, {
   ".ASC", "text/plain"}, {
   ".BZ2", "application/x-bzip2"}, {
   ".CFG", "text/plain"}, {
   ".CHRT", "application/x-kchart"}, {
   ".CONF", "text/plain"}, {
   ".CSH", "application/x-csh"}, {
   ".CSS", "text/css"}, {
   ".DOC", "application/msword"}, {
   ".DVI", "application/x-dvi"}, {
   ".EPS", "application/postscript"}, {
   ".GIF", "image/gif"}, {
   ".GZ", "application/x-gzip"}, {
   ".HTM", "text/html"}, {
   ".HTML", "text/html"}, {
   ".ICO", "image/x-icon"}, {
   ".JPEG", "image/jpeg"}, {
   ".JPG", "image/jpeg"}, {
   ".JS", "application/x-javascript"}, {
   ".KPR", "application/x-kpresenter"}, {
   ".KSP", "application/x-kspread"}, {
   ".KWD", "application/x-kword"}, {
   ".MP3", "audio/mpeg"}, {
   ".OGG", "application/x-ogg"}, {
   ".PDF", "application/pdf"}, {
   ".PNG", "image/png"}, {
   ".PS", "application/postscript"}, {
   ".RAM", "audio/x-pn-realaudio"}, {
   ".RM", "audio/x-pn-realaudio"}, {
   ".RM", "audio/x-pn-realaudio"}, {
   ".RM", "audio/x-pn-realaudio"}, {
   ".RPM", "application/x-rpm"}, {
   ".RTF", "application/rtf"}, {
   ".SH", "application/x-sh"}, {
   ".TAR", "application/x-tar"}, {
   ".TCL", "application/x-tcl"}, {
   ".TEX", "application/x-tex"}, {
   ".TGZ", "application/x-gzip"}, {
   ".TIF", "image/tiff"}, {
   ".TIFF", "image/tiff"}, {
   ".TXT", "text/plain"}, {
   ".WAV", "audio/x-wav"}, {
   ".XLS", "application/x-msexcel"}, {
   ".XML", "text/xml"}, {
   ".XSL", "text/xml"}, {
   ".ZIP", "application/x-zip-compressed"}, {

"", ""},};

typedef struct {
   int message_id;
   char file_name[32];
   time_t file_time;
   int offset;
   int in_reply_to;
   unsigned char md5_digest[16];
} EL_INDEX;

typedef struct {
   char name[256];
   char name_enc[256];
   char data_dir[256];
   char top_group[256];
   EL_INDEX *el_index;
   int *n_el_index;
   int n_attr;
   PMXML_NODE pwd_xml_tree;
} LOGBOOK;

typedef struct {
   int message_id;
   unsigned char md5_digest[16];
} MD5_INDEX;

typedef struct LBNODE *LBLIST;

struct LBNODE {
   char name[256];
   LBLIST *member;
   int n_members;
   int is_top;
} LBNODE;

typedef struct {
   LOGBOOK *lbs;
   int index;
   char string[256];
   int number;
   int in_reply_to;
} MSG_LIST;

LOGBOOK *lb_list = NULL;

#ifdef OS_WINNT
int run_service(void);
#endif

void show_error(char *error);
BOOL enum_user_line(LOGBOOK * lbs, int n, char *user, int size);
int get_user_line(LOGBOOK * lbs, char *user, char *password, char *full_name, char *email,
                  BOOL email_notify[1000], time_t * last_access);
int strbreak(char *str, char list[][NAME_LENGTH], int size, char *brk, BOOL ignore_quotes);
int execute_shell(LOGBOOK * lbs, int message_id, char attrib[MAX_N_ATTR][NAME_LENGTH],
                  char att_file[MAX_ATTACHMENTS][256], char *sh_cmd);
BOOL isparam(char *param);
char *getparam(char *param);
void write_logfile(LOGBOOK * lbs, const char *str);
BOOL check_login_user(LOGBOOK * lbs, char *user);
LBLIST get_logbook_hierarchy(void);
BOOL is_logbook_in_group(LBLIST pgrp, char *logbook);
BOOL is_admin_user(char *logbook, char *user);
BOOL is_admin_user_global(char *user);
void free_logbook_hierarchy(LBLIST root);
void show_top_text(LOGBOOK * lbs);
void show_bottom_text(LOGBOOK * lbs);
int set_attributes(LOGBOOK * lbs, char attributes[][NAME_LENGTH], int n);
void show_elog_list(LOGBOOK * lbs, int past_n, int last_n, int page_n, BOOL default_page, char *info);
int change_config_line(LOGBOOK * lbs, char *option, char *old_value, char *new_value);
int read_password(char *pwd, int size);
int getcfg(char *group, char *param, char *value, int vsize);
int build_subst_list(LOGBOOK * lbs, char list[][NAME_LENGTH], char value[][NAME_LENGTH],
                     char attrib[][NAME_LENGTH], BOOL format_date);
void highlight_searchtext(regex_t * re_buf, char *src, char *dst, BOOL hidden);
int parse_config_file(char *config_file);
PMXML_NODE load_password_file(LOGBOOK * lbs, char *error, int error_size);
int load_password_files();
void compose_base_url(LOGBOOK * lbs, char *base_url, int size, BOOL email_notify);
void show_elog_entry(LOGBOOK * lbs, char *dec_path, char *command);
char *loc(char *orig);
void strencode(char *text);
void strencode_nouml(char *text);
int scan_attributes(char *logbook);
int is_inline_attachment(char *encoding, int message_id, char *text, int i, char *att);
int setgroup(char *str);
int setuser(char *str);
int setegroup(char *str);
int seteuser(char *str);
void strencode2(char *b, const char *text, int size);
void load_config_section(char *section, char **buffer, char *error);
void remove_crlf(char *buffer);
time_t convert_date(char *date_string);
time_t convert_datetime(char *date_string);
int get_thumb_name(const char *file_name, char *thumb_name, int size, int index);
int create_thumbnail(LOGBOOK * lbs, char *file_name);
int ascii_compare(const void *s1, const void *s2);

/*---- Funcions from the MIDAS library -----------------------------*/

#define my_toupper(_c)    ( ((_c)>='a' && (_c)<='z') ? ((_c)-'a'+'A') : (_c) )
#define my_tolower(_c)    ( ((_c)>='A' && (_c)<='Z') ? ((_c)-'A'+'a') : (_c) )

BOOL strieq(const char *str1, const char *str2)
{
   char c1, c2;

   if (str1 == NULL && str2 == NULL)
      return TRUE;
   if (str1 == NULL || str2 == NULL)
      return FALSE;
   if (strlen(str1) != strlen(str2))
      return FALSE;

   while (*str1) {
      c1 = *str1++;
      c2 = *str2++;
      if (my_toupper(c1) != my_toupper(c2))
         return FALSE;
   }

   if (*str2)
      return FALSE;

   return TRUE;
}

BOOL strnieq(const char *str1, const char *str2, int n)
{
   char c1, c2;
   int i;

   if (str1 == NULL && str2 == NULL && n == 0)
      return TRUE;
   if (str1 == NULL || str2 == NULL)
      return FALSE;
   if ((int) strlen(str1) < n || (int) strlen(str2) < n)
      return FALSE;

   for (i = 0; i < n && *str1; i++) {
      c1 = *str1++;
      c2 = *str2++;
      if (my_toupper(c1) != my_toupper(c2))
         return FALSE;
   }

   if (i < n)
      return FALSE;

   return TRUE;
}

char *stristr(const char *str, const char *pattern)
{
   char c1, c2, *ps, *pp;

   if (str == NULL || pattern == NULL)
      return NULL;

   while (*str) {
      ps = (char *) str;
      pp = (char *) pattern;
      c1 = *ps;
      c2 = *pp;
      if (my_toupper(c1) == my_toupper(c2)) {
         while (*pp) {
            c1 = *ps;
            c2 = *pp;

            if (my_toupper(c1) != my_toupper(c2))
               break;

            ps++;
            pp++;
         }

         if (!*pp)
            return (char *) str;
      }
      str++;
   }

   return NULL;
}

void strextract(const char *str, char delim, char *extr, int size)
/* extract a substinr "extr" from "str" until "delim" is found */
{
   int i;

   for (i = 0; str[i] != delim && i < size - 1; i++)
      extr[i] = str[i];
   extr[i] = 0;
}

static BOOL chkext(const char *str, const char *ext)
{
   int extl, strl;
   char c1, c2;

   if (ext == NULL || str == NULL)
      return FALSE;

   extl = strlen(ext);
   strl = strlen(str);
   if (extl >= strl)
      return FALSE;
   str = str + strl - extl;
   while (*str) {
      c1 = *str++;
      c2 = *ext++;
      if (my_toupper(c1) != my_toupper(c2))
         return FALSE;
   }
   return TRUE;
}

/* workaround for some gcc versions bug for "%c" format (see strftime(3) */
size_t my_strftime(char *s, size_t max, const char *fmt, const struct tm * tm)
{
   return strftime(s, max, fmt, tm);
}

/* signal save read function */
int my_read(int fh, void *buffer, unsigned int bytes)
{
#ifdef OS_UNIX
   int i, n = 0;

   do {
      i = read(fh, (char *) buffer + n, bytes - n);

      /* don't return if an alarm signal was cought */
      if (i == -1 && errno == EINTR)
         continue;

      if (i == -1)
         return -1;

      if (i == 0)
         return n;

      n += i;

   } while (n < (int) bytes);

   return n;
#else
   return read(fh, buffer, bytes);
#endif

   return 0;
}

/* workaround for wong timezone under MAX OSX */
long my_timezone()
{
#if defined(OS_MACOSX) || defined(__FreeBSD__)
   time_t tp;
   time(&tp);
   return -localtime(&tp)->tm_gmtoff;
#else
   return timezone;
#endif
}

/*---- Compose RFC2822 compliant date ---*/

void get_rfc2822_date(char *date, int size, time_t ltime)
{
   time_t now;
   char buf[256];
   int offset;
   struct tm *ts;

   /* switch locale temporarily back to english to comply with RFC2822 date format */
   setlocale(LC_ALL, "C");

   if (ltime == 0)
      time(&now);
   else
      now = ltime;
   ts = localtime(&now);
   assert(ts);
   strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S", ts);
   offset = (-(int) my_timezone());
   if (ts->tm_isdst)
      offset += 3600;
   snprintf(date, size - 1, "%s %+03d%02d", buf, (int) (offset / 3600),
            (int) ((abs((int) offset) / 60) % 60));
}

/*---- Safe malloc wrappers with out of memory checking from GNU ---*/

static void memory_error_and_abort(char *func)
{
   extern void eprintf(const char *, ...);

   eprintf("%s: not enough memory\n", func);
   exit(EXIT_FAILURE);
}

/* Return a pointer to free()able block of memory large enough
 to hold BYTES number of bytes.  If the memory cannot be allocated,
 print an error message and abort. */
void *xmalloc(size_t bytes)
{
   char *temp;

   /* Align buffer on 4 byte boundery for HP UX and other 64 bit systems to prevent Bus error (core dump) */
   if (bytes & 3)
      bytes += 4 - (bytes & 3);

   temp = (char *) malloc(bytes + 12);
   if (temp == 0)
      memory_error_and_abort("xmalloc");

   /* put magic number around array and put array size */
   *(unsigned int *) (temp + 0) = bytes;
   *(unsigned int *) (temp + 4) = 0xdeadc0de;
   *(unsigned int *) (temp + bytes + 8) = 0xdeadc0de;

   return (temp + 8);
}

void *xcalloc(size_t count, size_t bytes)
{
   char *temp;

   /* Align buffer on 4 byte boundery for HP UX and other 64 bit systems to prevent Bus error (core dump) */
   if (bytes & 3)
      bytes += 4 - (bytes & 3);

   temp = (char *) malloc(count * bytes + 12);
   if (temp == 0)
      memory_error_and_abort("xcalloc");
   memset(temp, 0, count * bytes + 12);

   /* put magic number around array */
   *(unsigned int *) (temp + 0) = count * bytes;
   *(unsigned int *) (temp + 4) = 0xdeadc0de;
   *(unsigned int *) (temp + count * bytes + 8) = 0xdeadc0de;

   return (temp + 8);
}

void *xrealloc(void *pointer, size_t bytes)
{
   char *temp;
   int old_size;

   /* Align buffer on 4 byte boundery for HP UX and other 64 bit systems to prevent Bus error (core dump) */
   if (bytes & 3)
      bytes += 4 - (bytes & 3);

   if (pointer == NULL)
      return xmalloc(bytes);

   /* check old magic number */
   temp = pointer;
   assert(*((unsigned int *) (temp - 4)) == 0xdeadc0de);
   old_size = *((unsigned int *) (temp - 8));
   assert(*((unsigned int *) (temp + old_size)) == 0xdeadc0de);

   temp = (char *) realloc(temp - 8, bytes + 12);

   if (temp == 0)
      memory_error_and_abort("xrealloc");

   /* put magic number around array */
   *(unsigned int *) (temp + 0) = bytes;
   *(unsigned int *) (temp + 4) = 0xdeadc0de;
   *(unsigned int *) (temp + bytes + 8) = 0xdeadc0de;

   return (temp + 8);
}

void xfree(void *pointer)
{
   char *temp;
   int old_size;

   if (!pointer)
      return;

   /* check for magic byte */
   temp = pointer;
   assert(*((unsigned int *) (temp - 4)) == 0xdeadc0de);
   old_size = *((unsigned int *) (temp - 8));
   assert(*((unsigned int *) (temp + old_size)) == 0xdeadc0de);

   free(temp - 8);
}

char *xstrdup(const char *string)
{
   char *s;

   s = (char *) xmalloc(strlen(string) + 1);
   strcpy(s, string);
   return s;
}

/*----------------------- Message handling -------------------------*/

/* Have vasprintf? (seems that only libc6 based Linux has this) */
#ifdef __linux__
#  define HAVE_VASPRintF
#endif

#ifndef HAVE_VASPRintF
/* vasprintf implementation taken (and adapted) from GNU libiberty */

static int int_vasprintf(char **result, const char *format, va_list args)
{
   const char *p = format;
   /* Add one to make sure that it is never zero, which might cause malloc
      to return NULL.  */
   int total_width = strlen(format) + 1;
   va_list ap;

#ifdef va_copy
   va_copy(ap, args);
#else
   memcpy(&ap, &args, sizeof(va_list));
#endif

   while (*p != '\0') {
      if (*p++ == '%') {
         while (strchr("-+ #0", *p))
            ++p;
         if (*p == '*') {
            ++p;
            total_width += abs(va_arg(ap, int));
         } else
            total_width += strtoul(p, (char **) &p, 10);
         if (*p == '.') {
            ++p;
            if (*p == '*') {
               ++p;
               total_width += abs(va_arg(ap, int));
            } else
               total_width += strtoul(p, (char **) &p, 10);
         }
         while (strchr("hlL", *p))
            ++p;
         /*
          * Should be big enough for any format specifier
          * except %s and floats.
          */
         total_width += 30;
         switch (*p) {
         case 'd':
         case 'i':
         case 'o':
         case 'u':
         case 'x':
         case 'X':
         case 'c':
            (void) va_arg(ap, int);
            break;
         case 'f':
         case 'e':
         case 'E':
         case 'g':
         case 'G':
            (void) va_arg(ap, double);
            /*
             * Since an ieee double can have an exponent of 307, we'll
             * make the buffer wide enough to cover the gross case.
             */
            total_width += 307;
            break;
         case 's':
            total_width += strlen(va_arg(ap, char *));
            break;
         case 'p':
         case 'n':
            (void) va_arg(ap, char *);
            break;
         }
         p++;
      }
   }
#ifdef va_copy
   va_end(ap);
#endif
   *result = (char *) malloc(total_width);
   if (*result != NULL)
      return vsprintf(*result, format, args);
   else
      return -1;
}

#if defined (_BSD_VA_LIST_) && defined (__FreeBSD__)
int vasprintf(char **result, const char *format, _BSD_VA_LIST_ args)
#else
int vasprintf(char **result, const char *format, va_list args)
#endif
{
   return int_vasprintf(result, format, args);
}
#endif                          /* ! HAVE_VASPRintF */

/* Safe replacement for vasprintf (adapted code from Samba) */
int xvasprintf(char **ptr, const char *format, va_list ap)
{
   int n;
   va_list save;

#ifdef va_copy
   va_copy(save, ap);
#else
# ifdef __va_copy
   __va_copy(save, ap);
# else
   save = ap;
# endif
#endif

   n = vasprintf(ptr, format, save);

   if (n == -1 || !*ptr) {
      printf("Not enough memory");
      exit(EXIT_FAILURE);
   }

   return n;
}

/* Driver for printf_handler, drop-in replacement for printf */
void eprintf(const char *format, ...)
{
   va_list ap;
   char *msg;

   va_start(ap, format);
   xvasprintf(&msg, format, ap);
   va_end(ap);

   (*printf_handler) (msg);

   free(msg);
}

/* Driver for fputs_handler, drop-in replacement for fputs(buf, fd) */
void efputs(const char *buf)
{
   (*fputs_handler) (buf);
}

/* Dump with the newline, drop-in replacement for puts(buf) */
void eputs(const char *buf)
{
   char *p;

   p = xmalloc(strlen(buf) + 2);
   strcpy(p, buf);
   strcat(p, "\n");

   (*fputs_handler) (p);

   xfree(p);
}

/* Flush the current output stream */
void eflush(void)
{
   /* Do this only for non-NULL streams (uninitiated stream or a syslog) */
   if (current_output_stream != NULL)
      fflush(current_output_stream);
}

#ifdef OS_WINNT
HANDLE hEventLog;
#endif

/* Print MSG to syslog */
void print_syslog(const char *msg)
{
   char *p;

   /* strip trailing \r and \n */
   p = xstrdup(msg);
   while (p[strlen(p) - 1] == '\r' || p[strlen(p) - 1] == '\n')
      p[strlen(p) - 1] = 0;

#ifdef OS_UNIX
   syslog(SYSLOG_PRIORITY, "%s", p);
#else
   ReportEvent(hEventLog, EVENTLOG_INFORMATION_TYPE, 0, 0, NULL, 1, 0, &p, NULL);
#endif

   xfree(p);
}

/* Print MSG to stderr */
void print_stderr(const char *msg)
{
   fprintf(stderr, "%s", msg);
}

/* Dump BUF to syslog */
void fputs_syslog(const char *buf)
{
   char *p;

   /* strip trailing \r and \n */
   p = xstrdup(buf);
   while (p[strlen(p) - 1] == '\r' || p[strlen(p) - 1] == '\n')
      p[strlen(p) - 1] = 0;

#ifdef OS_UNIX
   syslog(SYSLOG_PRIORITY, "%s", p);
#else
   ReportEvent(hEventLog, EVENTLOG_INFORMATION_TYPE, 0, 0, NULL, 1, 0, &p, NULL);
#endif

   xfree(p);
}

/* Dump BUF to stderr */
void fputs_stderr(const char *buf)
{
   fputs(buf, stderr);
}

/* Redirect all messages handled with eprintf/efputs
 to syslog (Unix) or event log (Windows) */
void redirect_to_syslog(void)
{
   static int has_inited = 0;

   /* initiate syslog */
   if (!has_inited) {
#ifdef OS_UNIX
      setlogmask(LOG_UPTO(SYSLOG_PRIORITY));
      openlog("elogd", LOG_CONS | LOG_PID | LOG_NDELAY, LOG_LOCAL1);
#else
      hEventLog = RegisterEventSource(NULL, "ELOG");
#endif
   }
   has_inited = 1;

   printf_handler = print_syslog;
   fputs_handler = fputs_syslog;

   /* tells that the syslog facility is currently used as output */
   current_output_stream = NULL;
}

/* Redirect all messages handled with eprintf/efputs to stderr */
void redirect_to_stderr(void)
{
   printf_handler = print_stderr;
   fputs_handler = fputs_stderr;

   current_output_stream = stderr;
}

/*------------------------------------------------------------------*/

int my_shell(char *cmd, char *result, int size)
{
#ifdef OS_WINNT

   HANDLE hChildStdinRd, hChildStdinWr, hChildStdinWrDup,
       hChildStdoutRd, hChildStdoutWr, hChildStderrRd, hChildStderrWr, hSaveStdin, hSaveStdout, hSaveStderr;

   SECURITY_ATTRIBUTES saAttr;
   PROCESS_INFORMATION piProcInfo;
   STARTUPINFO siStartInfo;
   char buffer[10000];
   DWORD dwRead, dwAvail, i;

   /* Set the bInheritHandle flag so pipe handles are inherited. */
   saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
   saAttr.bInheritHandle = TRUE;
   saAttr.lpSecurityDescriptor = NULL;

   /* Save the handle to the current STDOUT. */
   hSaveStdout = GetStdHandle(STD_OUTPUT_HANDLE);

   /* Create a pipe for the child's STDOUT. */
   if (!CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &saAttr, 0))
      return 0;

   /* Set a write handle to the pipe to be STDOUT. */
   if (!SetStdHandle(STD_OUTPUT_HANDLE, hChildStdoutWr))
      return 0;

   /* Save the handle to the current STDERR. */
   hSaveStderr = GetStdHandle(STD_ERROR_HANDLE);

   /* Create a pipe for the child's STDERR. */
   if (!CreatePipe(&hChildStderrRd, &hChildStderrWr, &saAttr, 0))
      return 0;

   /* Set a read handle to the pipe to be STDERR. */
   if (!SetStdHandle(STD_ERROR_HANDLE, hChildStderrWr))
      return 0;

   /* Save the handle to the current STDIN. */
   hSaveStdin = GetStdHandle(STD_INPUT_HANDLE);

   /* Create a pipe for the child's STDIN. */
   if (!CreatePipe(&hChildStdinRd, &hChildStdinWr, &saAttr, 0))
      return 0;

   /* Set a read handle to the pipe to be STDIN. */
   if (!SetStdHandle(STD_INPUT_HANDLE, hChildStdinRd))
      return 0;

   /* Duplicate the write handle to the pipe so it is not inherited. */
   if (!DuplicateHandle(GetCurrentProcess(), hChildStdinWr, GetCurrentProcess(), &hChildStdinWrDup, 0, FALSE,   /* not inherited */
                        DUPLICATE_SAME_ACCESS))
      return 0;

   CloseHandle(hChildStdinWr);

   /* Now create the child process. */
   memset(&siStartInfo, 0, sizeof(siStartInfo));
   siStartInfo.cb = sizeof(STARTUPINFO);
   siStartInfo.lpReserved = NULL;
   siStartInfo.lpReserved2 = NULL;
   siStartInfo.cbReserved2 = 0;
   siStartInfo.lpDesktop = NULL;
   siStartInfo.dwFlags = 0;

   /* command to execute */
   sprintf(buffer, "cmd /q /c %s", cmd);

   if (!CreateProcess(NULL, buffer,     /* command line */
                      NULL,     /* process security attributes */
                      NULL,     /* primary thread security attributes */
                      TRUE,     /* handles are inherited */
                      0,        /* creation flags */
                      NULL,     /* use parent's environment */
                      NULL,     /* use parent's current directory */
                      &siStartInfo,     /* STARTUPINFO pointer */
                      &piProcInfo))     /* receives PROCESS_INFORMATION */
      return 0;

   /* After process creation, restore the saved STDIN and STDOUT. */
   SetStdHandle(STD_INPUT_HANDLE, hSaveStdin);
   SetStdHandle(STD_OUTPUT_HANDLE, hSaveStdout);
   SetStdHandle(STD_ERROR_HANDLE, hSaveStderr);

   memset(result, 0, size);

   do {
      /* query stdout */
      do {
         if (!PeekNamedPipe(hChildStdoutRd, buffer, 256, &dwRead, &dwAvail, NULL))
            break;
         if (dwRead > 0) {
            ReadFile(hChildStdoutRd, buffer, 256, &dwRead, NULL);
            buffer[dwRead] = 0;
            strlcat(result, buffer, size);
         }
      } while (dwAvail > 0);

      /* query stderr */
      do {
         if (!PeekNamedPipe(hChildStderrRd, buffer, 256, &dwRead, &dwAvail, NULL))
            break;
         if (dwRead > 0) {
            ReadFile(hChildStderrRd, buffer, 256, &dwRead, NULL);
            buffer[dwRead] = 0;
            strlcat(result, buffer, size);
         }
      } while (dwAvail > 0);

      /* check if subprocess still alive */
      if (!GetExitCodeProcess(piProcInfo.hProcess, &i))
         break;
      if (i != STILL_ACTIVE)
         break;

      /* give some CPU to subprocess */
      Sleep(10);

   } while (TRUE);

   CloseHandle(hChildStdinWrDup);
   CloseHandle(hChildStdinRd);
   CloseHandle(hChildStderrRd);
   CloseHandle(hChildStdoutRd);

   /* strip trailing CR/LF */
   while (strlen(result) > 0 && (result[strlen(result) - 1] == '\r' || result[strlen(result) - 1] == '\n'))
      result[strlen(result) - 1] = 0;

   return 1;

#endif                          /* OS_WINNT */

#ifdef OS_UNIX
   pid_t child_pid;
   int fh, status, i;
   char str[256];

   if ((child_pid = fork()) < 0)
      return 0;
   else if (child_pid > 0) {
      /* parent process waits for child */
      waitpid(child_pid, &status, 0);

      /* read back result */
      memset(result, 0, size);
      fh = open("/tmp/elog-shell", O_RDONLY);
      if (fh > 0) {
         i = read(fh, result, size);
         close(fh);
      }

      /* remove temporary file */
      remove("/tmp/elog-shell");

      /* strip trailing CR/LF */
      while (strlen(result) > 0 && (result[strlen(result) - 1] == '\r' || result[strlen(result) - 1] == '\n'))
         result[strlen(result) - 1] = 0;

   } else {
      /* child process */

      /* restore original UID/GID */
      if (setregid(-1, orig_gid) < 0 || setreuid(-1, orig_uid) < 0)
         eprintf("Cannot restore original GID/UID.\n");

      /* give up root privilege permanently */
      if (geteuid() == 0) {
         if (!getcfg("global", "Grp", str, sizeof(str)) || setgroup(str) < 0) {
            eprintf("Falling back to default group \"elog\"\n");
            if (setgroup("elog") < 0) {
               eprintf("Falling back to default group \"%s\"\n", DEFAULT_GROUP);
               if (setgroup(DEFAULT_GROUP) < 0) {
                  eprintf("Refuse to run as setgid root.\n");
                  eprintf("Please consider to define a Grp statement in configuration file\n");
                  exit(EXIT_FAILURE);
               }
            }
         } else if (verbose)
            eprintf("Falling back to group \"%s\"\n", str);

         if (!getcfg("global", "Usr", str, sizeof(str)) || setuser(str) < 0) {
            eprintf("Falling back to default user \"elog\"\n");
            if (setuser("elog") < 0) {
               eprintf("Falling back to default user \"%s\"\n", DEFAULT_USER);
               if (setuser(DEFAULT_USER) < 0) {
                  eprintf("Refuse to run as setuid root.\n");
                  eprintf("Please consider to define a Usr statement in configuration file\n");
                  exit(EXIT_FAILURE);
               }
            }
         } else if (verbose)
            eprintf("Falling back to user \"%s\"\n", str);
      }

      /* execute shell with redirection to /tmp/elog-shell */
      sprintf(str, "/bin/sh -c \"%s\" > /tmp/elog-shell 2>&1", cmd);

      if (verbose) {
         efputs("Going to execute: ");
         efputs(str);
         efputs("\n");
      }

      system(str);
      _exit(0);
   }

   return 1;

#endif                          /* OS_UNIX */
}

/*------------------------------------------------------------------*/

void strsubst_list(char *string, int size, char name[][NAME_LENGTH], char value[][NAME_LENGTH], int n)
/* subsitute "$name" with value corresponding to name */
{
   int i, j;
   char tmp[2 * NAME_LENGTH], str[2 * NAME_LENGTH], uattr[2 * NAME_LENGTH], *ps, *pt, *p, result[10000];

   pt = tmp;
   ps = string;
   for (p = strchr(ps, '$'); p != NULL; p = strchr(ps, '$')) {

      /* copy leading characters */
      j = (int) (p - ps);
      if (j >= (int) sizeof(tmp))
         return;
      memcpy(pt, ps, j);
      pt += j;
      p++;

      /* extract name */
      strlcpy(str, p, sizeof(str));
      for (j = 0; j < (int) strlen(str); j++)
         str[j] = toupper(str[j]);
      /* do shell substituion at the end, so that shell parameter can
         contain substituted attributes */
      if (strncmp(str, "SHELL(", 6) == 0) {
         strlcpy(pt, "$shell(", sizeof(tmp) - (pt - tmp));
         ps += 7;
         pt += 7;
         continue;
      }

      /* search name */
      for (i = 0; i < n; i++) {
         strlcpy(uattr, name[i], sizeof(uattr));
         for (j = 0; j < (int) strlen(uattr); j++)
            uattr[j] = toupper(uattr[j]);

         if (strncmp(str, uattr, strlen(uattr)) == 0)
            break;
      }

      /* copy value */
      if (i < n) {
         strlcpy(pt, value[i], sizeof(tmp) - (pt - tmp));
         pt += strlen(pt);
         ps = p + strlen(uattr);
      } else {
         *pt++ = '$';
         ps = p;
      }
   }

   /* copy remainder */
   strlcpy(pt, ps, sizeof(tmp) - (pt - tmp));
   strlcpy(string, tmp, size);

   /* check for $shell() subsitution */
   pt = tmp;
   ps = string;
   p = strchr(ps, '$');
   if (p != NULL) {

      /* copy leading characters */
      j = (int) (p - ps);
      if (j >= (int) sizeof(tmp))
         return;
      memcpy(pt, ps, j);
      pt += j;
      p++;

      /* extract name */
      strlcpy(str, p, sizeof(str));
      for (j = 0; j < (int) strlen(str); j++)
         str[j] = toupper(str[j]);

      if (strncmp(str, "SHELL(", 6) == 0) {
         ps += 7;
         if (strrchr(p, '\"')) {
            ps += strrchr(p, '\"') - p - 5;
            if (strchr(ps, ')'))
               ps = strchr(ps, ')') + 1;
         } else {
            if (strchr(ps, ')'))
               ps = strchr(ps, ')') + 1;
         }

         if (str[6] == '"') {
            strcpy(str, p + 7);
            if (strrchr(str, '\"'))
               *strrchr(str, '\"') = 0;
         } else {
            strcpy(str, p + 6);
            if (strrchr(str, ')'))
               *strrchr(str, ')') = 0;
         }

         if (!enable_execute) {
            strlcpy(result, loc("Shell execution not enabled via -x flag"), sizeof(result));
            eprintf("Shell execution not enabled via -x flag.\n");
         } else
            my_shell(str, result, sizeof(result));

         strlcpy(pt, result, sizeof(tmp) - (pt - tmp));
         pt += strlen(pt);
      }
   }

   /* copy remainder */
   strlcpy(pt, ps, sizeof(tmp) - (pt - tmp));

   /* return result */
   strlcpy(string, tmp, size);
}

/*------------------------------------------------------------------*/

void strsubst(char *string, int size, char *pattern, char *subst)
/* subsitute "pattern" with "subst" in "string" */
{
   char *tail, *p;
   int s;

   p = string;
   for (p = stristr(p, pattern); p != NULL; p = stristr(p, pattern)) {

      if (strlen(pattern) == strlen(subst)) {
         memcpy(p, subst, strlen(subst));
      } else if (strlen(pattern) > strlen(subst)) {
         memcpy(p, subst, strlen(subst));
         strcpy(p + strlen(subst), p + strlen(pattern));
      } else {
         tail = xmalloc(strlen(p) - strlen(pattern) + 1);
         strcpy(tail, p + strlen(pattern));
         s = size - (p - string);
         strlcpy(p, subst, s);
         strlcat(p, tail, s);
         xfree(tail);
      }

      p += strlen(subst);
   }
}

/*------------------------------------------------------------------*/

void url_decode(char *p)
/********************************************************************\
Decode the given string in-place by expanding %XX escapes
 \********************************************************************/
{
   char *pD, str[3];
   int i;

   pD = p;
   while (*p) {
      if (*p == '%') {
         /* Escape: next 2 chars are hex representation of the actual character */
         p++;
         if (isxdigit(p[0]) && isxdigit(p[1])) {
            str[0] = p[0];
            str[1] = p[1];
            str[2] = 0;
            sscanf(p, "%02X", &i);

            *pD++ = (char) i;
            p += 2;
         } else
            *pD++ = '%';
      } else if (*p == '+') {
         /* convert '+' to ' ' */
         *pD++ = ' ';
         p++;
      } else {
         *pD++ = *p++;
      }
   }
   *pD = '\0';
}

void url_encode(char *ps, int size)
/********************************************************************\
Encode the given string in-place by adding %XX escapes
 \********************************************************************/
{
   unsigned char *pd, *p;
   unsigned char str[NAME_LENGTH];

   pd = str;
   p = (unsigned char *) ps;
   while (*p && pd < str + 250) {
      if (strchr("%&=#?+<>", *p) || *p > 127) {
         sprintf((char *) pd, "%%%02X", *p);
         pd += 3;
         p++;
      } else if (*p == ' ') {
         *pd++ = '+';
         p++;
      } else {
         *pd++ = *p++;
      }
   }
   *pd = '\0';
   strlcpy(ps, (char *) str, size);
}

void url_slash_encode(char *ps, int size)
/********************************************************************\
Do the same including '/' characters
 \********************************************************************/
{
   unsigned char *pd, *p;
   unsigned char str[NAME_LENGTH];

   pd = str;
   p = (unsigned char *) ps;
   while (*p && pd < str + 250) {
      if (strchr("%&=#?+<>/", *p) || *p > 127) {
         sprintf((char *) pd, "%%%02X", *p);
         pd += 3;
         p++;
      } else if (*p == ' ') {
         *pd++ = '+';
         p++;
      } else {
         *pd++ = *p++;
      }
   }
   *pd = '\0';
   strlcpy(ps, (char *) str, size);
}

/*-------------------------------------------------------------------*/

void str_escape(char *ps, int size)
/********************************************************************\
Encode the given string in-place by adding \\ escapes for `$"\
\********************************************************************/
{
   unsigned char *pd, *p;
   unsigned char str[NAME_LENGTH];

   pd = str;
   p = (unsigned char *) ps;
   while (*p && pd < str + 250) {
      if (strchr("'$\"\\", *p)) {
         *pd++ = '\\';
         *pd++ = *p++;
      } else {
         *pd++ = *p++;
      }
   }
   *pd = '\0';
   strlcpy(ps, (char *) str, size);
}

void btou(char *str)
/* convert all blanks to underscores in a string */
{
   int i;

   for (i = 0; i < (int) strlen(str); i++)
      if (str[i] == ' ')
         str[i] = '_';
}

/*-------------------------------------------------------------------*/

void stou(char *str)
/* convert all special characters to underscores in a string */
{
   int i;

   for (i = 0; i < (int) strlen(str); i++)
      if (str[i] == ' ' || str[i] == '.' || str[i] == '/' ||
          str[i] == '\\' || str[i] == '-' || str[i] == '(' || str[i] == ')')
         str[i] = '_';
}

/*-------------------------------------------------------------------*/

char *map = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int cind(char c)
{
   int i;

   if (c == '=')
      return 0;

   for (i = 0; i < 64; i++)
      if (map[i] == c)
         return i;

   return -1;
}

void base64_decode(char *s, char *d)
{
   unsigned int t;

   while (s && *s) {
      t = cind(*s) << 18;
      s++;
      t |= cind(*s) << 12;
      s++;
      t |= cind(*s) << 6;
      s++;
      t |= cind(*s) << 0;
      s++;

      *(d + 2) = (char) (t & 0xFF);
      t >>= 8;
      *(d + 1) = (char) (t & 0xFF);
      t >>= 8;
      *d = (char) (t & 0xFF);

      d += 3;
   }
   *d = 0;
}

void base64_encode(unsigned char *s, unsigned char *d, int size)
{
   unsigned int t, pad;
   unsigned char *p;

   pad = 3 - strlen((char *) s) % 3;
   if (pad == 3)
      pad = 0;
   p = d;
   while (*s) {
      t = (*s++) << 16;
      if (*s)
         t |= (*s++) << 8;
      if (*s)
         t |= (*s++) << 0;

      *(d + 3) = map[t & 63];
      t >>= 6;
      *(d + 2) = map[t & 63];
      t >>= 6;
      *(d + 1) = map[t & 63];
      t >>= 6;
      *(d + 0) = map[t & 63];

      d += 4;
      if (d - p >= size - 3)
         return;
   }
   *d = 0;
   while (pad--)
      *(--d) = '=';
}

void base64_bufenc(unsigned char *s, int len, char *d)
{
   unsigned int t, pad;
   int i;

   pad = 3 - len % 3;
   if (pad == 3)
      pad = 0;
   for (i = 0; i < len;) {
      t = s[i++] << 16;
      if (i < len)
         t |= s[i++] << 8;
      if (i < len)
         t |= s[i++] << 0;

      *(d + 3) = map[t & 63];
      t >>= 6;
      *(d + 2) = map[t & 63];
      t >>= 6;
      *(d + 1) = map[t & 63];
      t >>= 6;
      *(d + 0) = map[t & 63];

      d += 4;
   }
   *d = 0;
   while (pad--)
      *(--d) = '=';
}

void do_crypt(char *s, char *d, int size)
{
#ifdef HAVE_CRYPT
   strlcpy(d, crypt(s, "el"), size);
#else
   base64_encode((unsigned char *) s, (unsigned char *) d, size);
#endif
}

/*------------------------------------------------------------------*
 MD5 Checksum Routines

 \*------------------------------------------------------------------*/

typedef struct {
   unsigned int state[4];       // state (ABCD)
   unsigned int count[2];       // number of bits, modulo 2^64 (lsb first)
   unsigned char buffer[64];    // input buffer
} MD5_CONTEXT;

/*------------------------------------------------------------------*/

/* prototypes of the support routines */
void _MD5_update(MD5_CONTEXT *, const void *, unsigned int);
void _MD5_transform(unsigned int[4], unsigned char[64]);
void _MD5_encode(unsigned char *, unsigned int *, unsigned int);
void _MD5_decode(unsigned int *, unsigned char *, unsigned int);

/* F, G, H and I are basic MD5 functions */
#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (z)) | ((y) & (~z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))
#define I(x, y, z) ((y) ^ ((x) | (~z)))

/* ROTATE_LEFT rotates x left n bits */
#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32-(n))))

/* FF, GG, HH, and II transformations for rounds 1, 2, 3, and 4 */
/* Rotation is separate from addition to prevent recomputation */
#define FF(a, b, c, d, x, s, ac) {                  \
   (a) += F ((b), (c), (d)) + (x) + (unsigned int)(ac);  \
   (a) = ROTATE_LEFT ((a), (s));                   \
   (a) += (b);                                     \
                                                   }
#define GG(a, b, c, d, x, s, ac) {                  \
   (a) += G ((b), (c), (d)) + (x) + (unsigned int)(ac);  \
   (a) = ROTATE_LEFT ((a), (s));                   \
   (a) += (b);                                     \
                                                   }
#define HH(a, b, c, d, x, s, ac) {                  \
   (a) += H ((b), (c), (d)) + (x) + (unsigned int)(ac);  \
   (a) = ROTATE_LEFT ((a), (s));                   \
   (a) += (b);                                     \
                                                   }
#define II(a, b, c, d, x, s, ac) {                  \
   (a) += I ((b), (c), (d)) + (x) + (unsigned int)(ac);  \
   (a) = ROTATE_LEFT ((a), (s));                   \
   (a) += (b);                                     \
                                                   }

/*------------------------------------------------------------------*/

/* main MD5 checksum routine, returns digest from pdata buffer */

void MD5_checksum(const void *pdata, unsigned int len, unsigned char digest[16])
{
   MD5_CONTEXT ctx;
   unsigned char bits[8];
   unsigned int i, padlen;

   /* to allow multithreading we have to locate the padding memory here */
   unsigned char PADDING[64] = { 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0
   };

   memset(&ctx, 0, sizeof(MD5_CONTEXT));
   ctx.count[0] = ctx.count[1] = 0;

   /* load magic initialization constants */
   ctx.state[0] = 0x67452301;
   ctx.state[1] = 0xefcdab89;
   ctx.state[2] = 0x98badcfe;
   ctx.state[3] = 0x10325476;

   _MD5_update(&ctx, pdata, len);

   // save number of bits
   _MD5_encode(bits, ctx.count, 8);

   // pad out to 56 mod 64
   i = (unsigned int) ((ctx.count[0] >> 3) & 0x3f);
   padlen = (i < 56) ? (56 - i) : (120 - i);
   _MD5_update(&ctx, PADDING, padlen);

   // append length (before padding)
   _MD5_update(&ctx, bits, 8);

   // store state in digest
   _MD5_encode(digest, ctx.state, 16);
}

/*------------------------------------------------------------------*/

void _MD5_update(MD5_CONTEXT * pctx, const void *pdata, unsigned int len)
{
   unsigned char *pin;
   unsigned int i, index, partlen;

   pin = (unsigned char *) pdata;

   // compute number of bytes mod 64
   index = (unsigned int) ((pctx->count[0] >> 3) & 0x3F);

   // update number of bits
   if ((pctx->count[0] += ((unsigned int) len << 3)) < ((unsigned int) len << 3))
      pctx->count[1]++;
   pctx->count[1] += ((unsigned int) len >> 29);

   partlen = 64 - index;

   // transform as many times as possible.
   if (len >= partlen) {
      memcpy(&pctx->buffer[index], pin, partlen);
      _MD5_transform(pctx->state, pctx->buffer);

      for (i = partlen; i + 63 < len; i += 64)
         _MD5_transform(pctx->state, &pin[i]);

      index = 0;
   } else
      i = 0;

   /* buffer remaining input */
   memcpy(&pctx->buffer[index], &pin[i], len - i);
}

/*------------------------------------------------------------------*/

/* basic transformation, transforms state based on block */
void _MD5_transform(unsigned int state[4], unsigned char block[64])
{
   unsigned int lA = state[0], lB = state[1], lC = state[2], lD = state[3];
   unsigned int x[16];

   _MD5_decode(x, block, 64);

   /* round 1 */
   FF(lA, lB, lC, lD, x[0], 7, 0xd76aa478);     // 1
   FF(lD, lA, lB, lC, x[1], 12, 0xe8c7b756);    // 2
   FF(lC, lD, lA, lB, x[2], 17, 0x242070db);    // 3
   FF(lB, lC, lD, lA, x[3], 22, 0xc1bdceee);    // 4
   FF(lA, lB, lC, lD, x[4], 7, 0xf57c0faf);     // 5
   FF(lD, lA, lB, lC, x[5], 12, 0x4787c62a);    // 6
   FF(lC, lD, lA, lB, x[6], 17, 0xa8304613);    // 7
   FF(lB, lC, lD, lA, x[7], 22, 0xfd469501);    // 8
   FF(lA, lB, lC, lD, x[8], 7, 0x698098d8);     // 9
   FF(lD, lA, lB, lC, x[9], 12, 0x8b44f7af);    // 10
   FF(lC, lD, lA, lB, x[10], 17, 0xffff5bb1);   // 11
   FF(lB, lC, lD, lA, x[11], 22, 0x895cd7be);   // 12
   FF(lA, lB, lC, lD, x[12], 7, 0x6b901122);    // 13
   FF(lD, lA, lB, lC, x[13], 12, 0xfd987193);   // 14
   FF(lC, lD, lA, lB, x[14], 17, 0xa679438e);   // 15
   FF(lB, lC, lD, lA, x[15], 22, 0x49b40821);   // 16

   /* round 2 */
   GG(lA, lB, lC, lD, x[1], 5, 0xf61e2562);     // 17
   GG(lD, lA, lB, lC, x[6], 9, 0xc040b340);     // 18
   GG(lC, lD, lA, lB, x[11], 14, 0x265e5a51);   // 19
   GG(lB, lC, lD, lA, x[0], 20, 0xe9b6c7aa);    // 20
   GG(lA, lB, lC, lD, x[5], 5, 0xd62f105d);     // 21
   GG(lD, lA, lB, lC, x[10], 9, 0x2441453);     // 22
   GG(lC, lD, lA, lB, x[15], 14, 0xd8a1e681);   // 23
   GG(lB, lC, lD, lA, x[4], 20, 0xe7d3fbc8);    // 24
   GG(lA, lB, lC, lD, x[9], 5, 0x21e1cde6);     // 25
   GG(lD, lA, lB, lC, x[14], 9, 0xc33707d6);    // 26
   GG(lC, lD, lA, lB, x[3], 14, 0xf4d50d87);    // 27
   GG(lB, lC, lD, lA, x[8], 20, 0x455a14ed);    // 28
   GG(lA, lB, lC, lD, x[13], 5, 0xa9e3e905);    // 29
   GG(lD, lA, lB, lC, x[2], 9, 0xfcefa3f8);     // 30
   GG(lC, lD, lA, lB, x[7], 14, 0x676f02d9);    // 31
   GG(lB, lC, lD, lA, x[12], 20, 0x8d2a4c8a);   // 32

   /* round 3 */
   HH(lA, lB, lC, lD, x[5], 4, 0xfffa3942);     // 33
   HH(lD, lA, lB, lC, x[8], 11, 0x8771f681);    // 34
   HH(lC, lD, lA, lB, x[11], 16, 0x6d9d6122);   // 35
   HH(lB, lC, lD, lA, x[14], 23, 0xfde5380c);   // 36
   HH(lA, lB, lC, lD, x[1], 4, 0xa4beea44);     // 37
   HH(lD, lA, lB, lC, x[4], 11, 0x4bdecfa9);    // 38
   HH(lC, lD, lA, lB, x[7], 16, 0xf6bb4b60);    // 39
   HH(lB, lC, lD, lA, x[10], 23, 0xbebfbc70);   // 40
   HH(lA, lB, lC, lD, x[13], 4, 0x289b7ec6);    // 41
   HH(lD, lA, lB, lC, x[0], 11, 0xeaa127fa);    // 42
   HH(lC, lD, lA, lB, x[3], 16, 0xd4ef3085);    // 43
   HH(lB, lC, lD, lA, x[6], 23, 0x4881d05);     // 44
   HH(lA, lB, lC, lD, x[9], 4, 0xd9d4d039);     // 45
   HH(lD, lA, lB, lC, x[12], 11, 0xe6db99e5);   // 46
   HH(lC, lD, lA, lB, x[15], 16, 0x1fa27cf8);   // 47
   HH(lB, lC, lD, lA, x[2], 23, 0xc4ac5665);    // 48

   /* round 4 */
   II(lA, lB, lC, lD, x[0], 6, 0xf4292244);     // 49
   II(lD, lA, lB, lC, x[7], 10, 0x432aff97);    // 50
   II(lC, lD, lA, lB, x[14], 15, 0xab9423a7);   // 51
   II(lB, lC, lD, lA, x[5], 21, 0xfc93a039);    // 52
   II(lA, lB, lC, lD, x[12], 6, 0x655b59c3);    // 53
   II(lD, lA, lB, lC, x[3], 10, 0x8f0ccc92);    // 54
   II(lC, lD, lA, lB, x[10], 15, 0xffeff47d);   // 55
   II(lB, lC, lD, lA, x[1], 21, 0x85845dd1);    // 56
   II(lA, lB, lC, lD, x[8], 6, 0x6fa87e4f);     // 57
   II(lD, lA, lB, lC, x[15], 10, 0xfe2ce6e0);   // 58
   II(lC, lD, lA, lB, x[6], 15, 0xa3014314);    // 59
   II(lB, lC, lD, lA, x[13], 21, 0x4e0811a1);   // 60
   II(lA, lB, lC, lD, x[4], 6, 0xf7537e82);     // 61
   II(lD, lA, lB, lC, x[11], 10, 0xbd3af235);   // 62
   II(lC, lD, lA, lB, x[2], 15, 0x2ad7d2bb);    // 63
   II(lB, lC, lD, lA, x[9], 21, 0xeb86d391);    // 64

   state[0] += lA;
   state[1] += lB;
   state[2] += lC;
   state[3] += lD;

   /* lClear sensitive information */
   memset(x, 0, sizeof(x));
}

/*------------------------------------------------------------------*/

/* encodes input (unsigned int) into output (unsigned char),
 assumes that lLen is a multiple of 4 */
void _MD5_encode(unsigned char *pout, unsigned int *pin, unsigned int len)
{
   unsigned int i, j;

   for (i = 0, j = 0; j < len; i++, j += 4) {
      pout[j] = (unsigned char) (pin[i] & 0x0ff);
      pout[j + 1] = (unsigned char) ((pin[i] >> 8) & 0x0ff);
      pout[j + 2] = (unsigned char) ((pin[i] >> 16) & 0x0ff);
      pout[j + 3] = (unsigned char) ((pin[i] >> 24) & 0x0ff);
   }
}

/*------------------------------------------------------------------*/

/* encodes input (unsigned char) into output (unsigned int),
 assumes that lLen is a multiple of 4 */
void _MD5_decode(unsigned int *pout, unsigned char *pin, unsigned int len)
{
   unsigned int i, j;

   for (i = 0, j = 0; j < len; i++, j += 4)
      pout[i] = ((unsigned int) pin[j]) | (((unsigned int) pin[j + 1]) << 8) | (((unsigned int) pin[j + 2])
                                                                                << 16) | (((unsigned int)
                                                                                           pin[j + 3]) << 24);
}

/*------------------------------------------------------------------*/

BOOL file_exist(char *file_name)
{
   int fh;

   fh = open(file_name, O_RDONLY);
   if (fh < 0)
      return FALSE;

   close(fh);
   return TRUE;
}

/*------------------------------------------------------------------*/

void serialdate2date(double days, int *day, int *month, int *year)
/* convert days since 1.1.1900 to date */
{
   int i, j, l, n;

   l = (int) days + 68569 + 2415019;
   n = (int) ((4 * l) / 146097);
   l = l - (int) ((146097 * n + 3) / 4);
   i = (int) ((4000 * (l + 1)) / 1461001);
   l = l - (int) ((1461 * i) / 4) + 31;
   j = (int) ((80 * l) / 2447);
   *day = l - (int) ((2447 * j) / 80);
   l = (int) (j / 11);
   *month = j + 2 - (12 * l);
   *year = 100 * (n - 49) + i + l;
}

double date2serialdate(int day, int month, int year)
/* convert date to days since 1.1.1900 */
{
   int serialdate;

   serialdate = (int) ((1461 * (year + 4800 + (int) ((month - 14) / 12))) / 4) + (int) ((367 * (month - 2
                                                                                                -
                                                                                                12 *
                                                                                                ((month -
                                                                                                  14) /
                                                                                                 12))) / 12) -
       (int) ((3 * ((int) ((year + 4900 + (int) ((month - 14) / 12))
                           / 100))) / 4) + day - 2415019 - 32075;

   return serialdate;
}

/*------------------------------------------------------------------*/

/* Wrapper for setegid. */
int setegroup(char *str)
{
#ifdef OS_UNIX
   struct group *gr;

   gr = getgrnam(str);

   if (gr != NULL)
      if (setregid(-1, gr->gr_gid) >= 0 && initgroups(gr->gr_name, gr->gr_gid) >= 0)
         return 0;
      else {
         eprintf("Cannot set effective GID to group \"%s\"\n", gr->gr_name);
         eprintf("setgroup: %s\n", strerror(errno));
   } else
      eprintf("Group \"%s\" not found\n", str);

   return -1;
#else
   return 0;
#endif
}

/* Wrapper for seteuid. */
int seteuser(char *str)
{
#ifdef OS_UNIX
   struct passwd *pw;

   pw = getpwnam(str);

   if (pw != NULL)
      if (setreuid(-1, pw->pw_uid) >= 0) {
         return 0;
      } else {
         eprintf("Cannot set effective UID to user \"%s\"\n", str);
         eprintf("setuser: %s\n", strerror(errno));
   } else
      eprintf("User \"%s\" not found\n", str);

   return -1;
#else
   return 0;
#endif
}

/* Wrapper for setgid. */
int setgroup(char *str)
{
#ifdef OS_UNIX
   struct group *gr;

   gr = getgrnam(str);

   if (gr != NULL)
      if (setgid(gr->gr_gid) >= 0 && initgroups(gr->gr_name, gr->gr_gid) >= 0)
         return 0;
      else {
         eprintf("Cannot set effective GID to group \"%s\"\n", gr->gr_name);
         eprintf("setgroup: %s\n", strerror(errno));
   } else
      eprintf("Group \"%s\" not found\n", str);

   return -1;
#else
   return 0;
#endif
}

/* Wrapper for setuid. */
int setuser(char *str)
{
#ifdef OS_UNIX
   struct passwd *pw;

   pw = getpwnam(str);

   if (pw != NULL)
      if (setuid(pw->pw_uid) >= 0)
         return 0;
      else {
         eprintf("Cannot set effective UID to user \"%s\"\n", str);
         eprintf("setuser: %s\n", strerror(errno));
   } else
      eprintf("User \"%s\" not found\n", str);

   return -1;
#else
   return 0;
#endif
}

/*-------------------------------------------------------------------*/

int recv_string(int sock, char *buffer, int buffer_size, int millisec)
{
   int i, n;
   fd_set readfds;
   struct timeval timeout;

   n = 0;
   memset(buffer, 0, buffer_size);

   do {
      if (millisec > 0) {
         FD_ZERO(&readfds);
         FD_SET(sock, &readfds);

         timeout.tv_sec = millisec / 1000;
         timeout.tv_usec = (millisec % 1000) * 1000;

         select(FD_SETSIZE, (void *) &readfds, NULL, NULL, (void *) &timeout);

         if (!FD_ISSET(sock, &readfds))
            break;
      }

      i = recv(sock, buffer + n, 1, 0);

      if (i <= 0)
         break;

      n++;

      if (n >= buffer_size)
         break;

   } while (buffer[n - 1] && buffer[n - 1] != 10);

   return n - 1;
}

/*-------------------------------------------------------------------*/

void compose_email_header(LOGBOOK * lbs, char *subject, char *from, char *to, char *url, char *mail_text,
                          int size, int mail_encoding, int n_attachments, char *multipart_boundary,
                          int message_id, int reply_id)
{
   char buffer[256], charset[256], subject_enc[5000];
   char buf[80], str[256];
   int i, offset, multipart;
   time_t now;
   struct tm *ts;

   i = 0;
   if (mail_encoding & 1)
      i++;
   if (mail_encoding & 2)
      i++;
   if (mail_encoding & 4)
      i++;
   multipart = i > 1;

   if (!getcfg("global", "charset", charset, sizeof(charset)))
      strcpy(charset, DEFAULT_HTTP_CHARSET);

   /* switch locale temporarily back to english to comply with RFC2822 date format */
   setlocale(LC_ALL, "C");

   time(&now);
   ts = localtime(&now);
   assert(ts);
   strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S", ts);
   offset = (-(int) my_timezone());
   if (ts->tm_isdst)
      offset += 3600;
   if (verbose) {
      sprintf(str, "timezone: %d, offset: %d\n", (int) my_timezone(), (int) offset);
      efputs(str);
   }
   snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1, "Date: %s %+03d%02d\r\n", buf,
            (int) (offset / 3600), (int) ((abs((int) offset) / 60) % 60));

   getcfg("global", "Language", str, sizeof(str));
   if (str[0])
      setlocale(LC_ALL, str);

   strlcat(mail_text, "To: ", size);

   if (getcfg(lbs->name, "Omit Email to", str, sizeof(str)) && atoi(str) == 1)
      strlcat(mail_text, "ELOG", size);
   else
      strlcat(mail_text, to, size);

   strlcat(mail_text, "\r\n", size);

   snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1, "From: %s\r\n", from);
   snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1, "User-Agent: Elog Version %s\r\n",
            VERSION);

   strlcat(mail_text, "MIME-Version: 1.0\r\n", size);

   memset(subject_enc, 0, sizeof(subject_enc));

   for (i = 0; i < (int) strlen(subject); i++)
      if (subject[i] < 0)
         break;

   if (i < (int) strlen(subject)) {
      /* subject contains local characters, so encode it using charset */
      for (i = 0; i < (int) strlen(subject); i += 40) {
         strlcpy(buffer, subject + i, sizeof(buffer));
         buffer[40] = 0;
         strlcat(subject_enc, "=?", sizeof(subject_enc));
         strlcat(subject_enc, charset, sizeof(subject_enc));
         strlcat(subject_enc, "?B?", sizeof(subject_enc));
         base64_encode((unsigned char *) buffer, (unsigned char *) (subject_enc + strlen(subject_enc)),
                       sizeof(subject_enc) - strlen(subject_enc));
         strlcat(subject_enc, "?=", sizeof(subject_enc));
         if (strlen(subject + i) < 40)
            break;

         strlcat(subject_enc, "\r\n ", sizeof(subject_enc));    // another encoded-word
      }
   } else
      strlcpy(subject_enc, subject, sizeof(subject_enc));

   snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1, "Subject: %s\r\n", subject_enc);

   if (strchr(from, '@')) {
      strlcpy(str, strchr(from, '@') + 1, sizeof(str));
      if (strchr(str, '>'))
         *strchr(str, '>') = 0;
   } else
      strlcpy(str, "elog.org", sizeof(str));

   if (message_id)
      snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1, "Message-ID: <%s-%d@%s>\r\n",
               lbs->name_enc, message_id, str);
   if (reply_id)
      snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1, "In-Reply-To: <%s-%d@%s>\r\n",
               lbs->name_enc, reply_id, str);

   if (url)
      snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1, "X-Elog-URL: %s\r\n", url);

   strlcat(mail_text, "X-Elog-submit-type: web|elog\r\n", size);

   if (multipart) {

      sprintf(multipart_boundary, "------------%04X%04X%04X", rand(), rand(), rand());
      snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1,
               "Content-Type: multipart/alternative;\r\n boundary=\"%s\"\r\n\r\n", multipart_boundary);

      strlcat(mail_text, "This is a multi-part message in MIME format.\r\n", size);
   } else {
      if (n_attachments) {
         sprintf(multipart_boundary, "------------%04X%04X%04X", rand(), rand(), rand());
         snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1,
                  "Content-Type: multipart/mixed;\r\n boundary=\"%s\"\r\n\r\n", multipart_boundary);

         strlcat(mail_text, "This is a multi-part message in MIME format.\r\n", size);
      } else {
         if (multipart_boundary)
            multipart_boundary[0] = 0;
      }
   }
}

/*-------------------------------------------------------------------*/

int check_smtp_error(char *str, int expected, char *error, int error_size)
{
   if (atoi(str) != expected) {
      if (error)
         strlcpy(error, str + 4, error_size);
      return 0;
   }

   return 1;
}

/*-------------------------------------------------------------------*/

int sendmail(LOGBOOK * lbs, char *smtp_host, char *from, char *to, char *text, char *error, int error_size)
{
   struct sockaddr_in bind_addr;
   struct hostent *phe;
   int i, n, s, strsize;
   char *str, *p;
   char list[1024][NAME_LENGTH], buffer[10000], decoded[256];

   memset(error, 0, error_size);

   if (verbose)
      eprintf("\n\nEmail from %s to %s, SMTP host %s:\n", from, to, smtp_host);
   sprintf(buffer, "Email from %s to ", from);
   strlcat(buffer, to, sizeof(buffer));
   strlcat(buffer, ", SMTP host ", sizeof(buffer));
   strlcat(buffer, smtp_host, sizeof(buffer));
   strlcat(buffer, ":\n", sizeof(buffer));
   write_logfile(lbs, buffer);

   /* create a new socket for connecting to remote server */
   s = socket(AF_INET, SOCK_STREAM, 0);
   if (s == -1)
      return -1;

   /* connect to remote node port 25 */
   memset(&bind_addr, 0, sizeof(bind_addr));
   bind_addr.sin_family = AF_INET;
   bind_addr.sin_port = htons((short) 25);

   phe = gethostbyname(smtp_host);
   if (phe == NULL) {
      if (error)
         strlcpy(error, loc("Cannot lookup server name"), error_size);
      return -1;
   }
   memcpy((char *) &(bind_addr.sin_addr), phe->h_addr, phe->h_length);

   if (connect(s, (void *) &bind_addr, sizeof(bind_addr)) < 0) {
      closesocket(s);
      if (error)
         strlcpy(error, loc("Cannot connect to server"), error_size);
      return -1;
   }

   strsize = MAX_CONTENT_LENGTH + 1000;
   str = xmalloc(strsize);

   recv_string(s, str, strsize, 10000);
   if (verbose)
      efputs(str);
   write_logfile(lbs, str);

   /* drain server messages */
   do {
      str[0] = 0;
      recv_string(s, str, strsize, 300);
      if (verbose)
         efputs(str);
      write_logfile(lbs, str);
   } while (str[0]);

   if (getcfg(lbs->name, "SMTP username", str, strsize)) {

      snprintf(str, strsize - 1, "EHLO %s\r\n", host_name);
      send(s, str, strlen(str), 0);
      if (verbose)
         efputs(str);
      write_logfile(lbs, str);

      do {
         recv_string(s, str, strsize, 3000);
         if (verbose)
            efputs(str);
         write_logfile(lbs, str);
         if (!check_smtp_error(str, 250, error, error_size))
            goto smtp_error;
      } while (stristr(str, "250 ") == NULL);

   } else {

      snprintf(str, strsize - 1, "HELO %s\r\n", host_name);

      send(s, str, strlen(str), 0);
      if (verbose)
         efputs(str);
      write_logfile(lbs, str);
      recv_string(s, str, strsize, 3000);
      if (verbose)
         efputs(str);
      write_logfile(lbs, str);
      if (!check_smtp_error(str, 250, error, error_size))
         goto smtp_error;
   }

   /* optional authentication */
   if (getcfg(lbs->name, "SMTP username", str, strsize)) {

      snprintf(str, strsize - 1, "AUTH LOGIN\r\n");
      send(s, str, strlen(str), 0);
      if (verbose)
         efputs(str);
      write_logfile(lbs, str);
      recv_string(s, str, strsize, 3000);
      if (strchr(str, '\r'))
         *strchr(str, '\r') = 0;
      if (verbose)
         efputs(decoded);
      write_logfile(lbs, str);
      base64_decode(str + 4, decoded);
      strcat(decoded, "\n");
      if (verbose)
         efputs(decoded);
      write_logfile(lbs, decoded);
      if (!check_smtp_error(str, 334, error, error_size))
         goto smtp_error;

      getcfg(lbs->name, "SMTP username", decoded, sizeof(decoded));
      base64_encode((unsigned char *) decoded, (unsigned char *) str, strsize);
      strcat(str, "\r\n");
      send(s, str, strlen(str), 0);
      if (verbose)
         efputs(str);
      write_logfile(lbs, str);
      recv_string(s, str, strsize, 3000);
      if (strchr(str, '\r'))
         *strchr(str, '\r') = 0;
      base64_decode(str + 4, decoded);
      strcat(decoded, "\n");
      if (verbose)
         efputs(decoded);
      write_logfile(lbs, decoded);
      if (!check_smtp_error(str, 334, error, error_size))
         goto smtp_error;

      getcfg(lbs->name, "SMTP password", str, strsize);
      strcat(str, "\r\n");
      send(s, str, strlen(str), 0);
      if (verbose)
         efputs(str);
      write_logfile(lbs, str);
      recv_string(s, str, strsize, 3000);
      if (verbose)
         efputs(str);
      write_logfile(lbs, str);
      if (!check_smtp_error(str, 235, error, error_size))
         goto smtp_error;
   }

   snprintf(str, strsize - 1, "MAIL FROM: %s\r\n", from);
   send(s, str, strlen(str), 0);
   if (verbose)
      efputs(str);
   write_logfile(lbs, str);
   recv_string(s, str, strsize, 3000);
   if (verbose)
      efputs(str);
   write_logfile(lbs, str);

   if (!check_smtp_error(str, 250, error, error_size))
      goto smtp_error;

   /* break recipients into list */
   n = strbreak(to, list, 1024, ",", FALSE);

   for (i = 0; i < n; i++) {
      if (list[i] == 0 || strchr(list[i], '@') == NULL)
         continue;

      snprintf(str, strsize - 1, "RCPT TO: <%s>\r\n", list[i]);
      send(s, str, strlen(str), 0);
      if (verbose)
         efputs(str);
      write_logfile(lbs, str);

      /* increased timeout for SMTP servers with long alias lists */
      recv_string(s, str, strsize, 30000);
      if (verbose)
         efputs(str);
      write_logfile(lbs, str);

      if (!check_smtp_error(str, 250, error, error_size))
         goto smtp_error;
   }

   snprintf(str, strsize - 1, "DATA\r\n");
   send(s, str, strlen(str), 0);
   if (verbose)
      efputs(str);
   write_logfile(lbs, str);
   recv_string(s, str, strsize, 3000);
   if (verbose)
      efputs(str);
   write_logfile(lbs, str);
   if (!check_smtp_error(str, 354, error, error_size))
      goto smtp_error;

   /* analyze text for "." at beginning of line */
   p = text;
   str[0] = 0;
   while (strstr(p, "\r\n.\r\n")) {
      i = strstr(p, "\r\n.\r\n") - p + 1;
      strlcat(str, p, i);
      p += i + 4;
      strlcat(str, "\r\n..\r\n", strsize);
   }
   strlcat(str, p, strsize);
   /* add ".<CR>" to signal end of message */
   strlcat(str, ".\r\n", strsize);

   /* check if buffer exceeded */
   if ((int) strlen(str) == strsize - 1) {
      strlcpy(error, loc("Entry size too large for email notification"), error_size);
      goto smtp_error;
   }

   send(s, str, strlen(str), 0);
   if (verbose)
      efputs(str);
   write_logfile(lbs, str);
   recv_string(s, str, strsize, 10000);
   if (verbose)
      efputs(str);
   write_logfile(lbs, str);
   if (!check_smtp_error(str, 250, error, error_size))
      goto smtp_error;

   snprintf(str, strsize - 1, "QUIT\r\n");
   send(s, str, strlen(str), 0);
   if (verbose)
      efputs(str);
   write_logfile(lbs, str);
   recv_string(s, str, strsize, 3000);
   if (verbose)
      efputs(str);
   write_logfile(lbs, str);
   if (!check_smtp_error(str, 221, error, error_size))
      goto smtp_error;

   closesocket(s);
   xfree(str);
   return 1;

 smtp_error:

   closesocket(s);
   xfree(str);

   return -1;
}

/*-------------------------------------------------------------------*/

void split_url(const char *url, char *host, int *port, char *subdir, char *param)
{
   const char *p;
   char str[256];

   if (host)
      host[0] = 0;
   if (port)
      *port = 80;
   if (subdir)
      subdir[0] = 0;
   if (param)
      param[0] = 0;

   p = url;
   if (strncmp(url, "http://", 7) == 0)
      p += 7;
   if (strncmp(url, "https://", 8) == 0)
      p += 8;

   strncpy(str, p, sizeof(str));
   if (strchr(str, '/')) {
      if (subdir)
         strncpy(subdir, strchr(str, '/'), 256);
      *strchr(str, '/') = 0;
   }

   if (strchr(str, '?')) {
      if (subdir)
         strncpy(subdir, strchr(str, '?'), 256);
      *strchr(str, '?') = 0;
   }

   if (strchr(str, ':')) {
      if (port)
         *port = atoi(strchr(str, ':') + 1);
      *strchr(str, ':') = 0;
   }

   if (host)
      strcpy(host, str);

   if (subdir) {
      if (strchr(subdir, '?')) {
         strncpy(param, strchr(subdir, '?'), 256);
         *strchr(subdir, '?') = 0;
      }

      if (subdir[0] == 0)
         strcpy(subdir, "/");
   }
}

/*-------------------------------------------------------------------*/

int retrieve_url(const char *url, char **buffer, char *rpwd)
{
   struct sockaddr_in bind_addr;
   struct hostent *phe;
   char str[1000], unm[256], upwd[256], host[256], subdir[256], param[256], auth[256], pwd_enc[256];
   int port, bufsize;
   int i, n;
   fd_set readfds;
   struct timeval timeout;

   static int sock, last_port;
   static char last_host[256];

   *buffer = NULL;
   split_url(url, host, &port, subdir, param);

   if (sock && (strcmp(host, last_host) != 0 || port != last_port)) {
      closesocket(sock);
      sock = 0;
   }

   if (sock) {                  // keep-alive does not yet work, requires evaluation of Content-Length !!!
      closesocket(sock);
      sock = 0;
   }

   /* create a new socket for connecting to remote server */
   if (!sock) {

      sock = socket(AF_INET, SOCK_STREAM, 0);
      if (sock == -1)
         return -1;

      /* connect to remote node */
      memset(&bind_addr, 0, sizeof(bind_addr));
      bind_addr.sin_family = AF_INET;
      bind_addr.sin_port = htons((short) port);

      phe = gethostbyname(host);
      if (phe == NULL)
         return -1;
      memcpy((char *) &(bind_addr.sin_addr), phe->h_addr, phe->h_length);

      if (connect(sock, (void *) &bind_addr, sizeof(bind_addr)) < 0) {
         closesocket(sock);
         return -1;
      }
   }

   last_port = port;
   strcpy(last_host, host);

   /* compose GET request, avoid chunked data in HTTP/1.1 protocol */
   sprintf(str, "GET %s%s HTTP/1.0\r\nConnection: Close\r\n", subdir, param);

   /* add local username/password */
   if (isparam("unm") && isparam("upwd")) {
      strlcpy(unm, getparam("unm"), sizeof(unm));
      strlcpy(upwd, getparam("upwd"), sizeof(upwd));
      sprintf(str + strlen(str), "Cookie: unm=%s; upwd=%s\r\n", getparam("unm"), getparam("upwd"));
   }

   if (rpwd && rpwd[0]) {
      sprintf(auth, "anybody:%s", rpwd);
      base64_encode((unsigned char *) auth, (unsigned char *) pwd_enc, sizeof(pwd_enc));
      sprintf(str + strlen(str), "Authorization: Basic %s\r\n", pwd_enc);
   }

   /* add host (RFC2616, Sec. 14) */
   sprintf(str + strlen(str), "Host: %s:%d\r\n", host, port);

   /* add keepalive */
   //sprintf(str + strlen(str), "Keep-Alive: 600\r\n");
   //sprintf(str + strlen(str), "Connection: keep-alive\r\n");

   strcat(str, "\r\n");

   send(sock, str, strlen(str), 0);

   bufsize = TEXT_SIZE + 1000;
   *buffer = xmalloc(bufsize);
   memset(*buffer, 0, bufsize);

   n = 0;

   do {
      FD_ZERO(&readfds);
      FD_SET(sock, &readfds);

      timeout.tv_sec = 30;      /* 30 sec. timeout */
      timeout.tv_usec = 0;

      select(FD_SETSIZE, (void *) &readfds, NULL, NULL, (void *) &timeout);

      if (!FD_ISSET(sock, &readfds)) {
         closesocket(sock);
         sock = 0;
         xfree(*buffer);
         *buffer = NULL;
         return -1;
      }

      i = recv(sock, *buffer + n, bufsize - n, 0);

      if (i <= 0)
         break;

      n += i;

      if (n >= bufsize) {
         /* increase buffer size */
         bufsize += 10000;
         *buffer = xrealloc(*buffer, bufsize);
         if (*buffer == NULL) {
            closesocket(sock);
            return -1;
         }
      }

   } while (1);

   return n;
}

/*-------------------------------------------------------------------*/

int ss_daemon_init()
{
#ifdef OS_UNIX

   /* only implemented for UNIX */
   int i, fd, pid;

   if ((pid = fork()) < 0)
      return FAILURE;
   else if (pid != 0)
      _exit(EXIT_SUCCESS);      /* parent finished, exit without atexit hook */

   /* child continues here */

   /* try and use up stdin, stdout and stderr, so other
      routines writing to stdout etc won't cause havoc. Copied from smbd */
   for (i = 0; i < 3; i++) {
      close(i);
      fd = open("/dev/null", O_RDWR, 0);
      if (fd < 0)
         fd = open("/dev/null", O_WRONLY, 0);
      if (fd < 0) {
         eprintf("Can't open /dev/null");
         return FAILURE;
      }
      if (fd != i) {
         eprintf("Did not get file descriptor");
         return FAILURE;
      }
   }

   setsid();                    /* become session leader */
   chdir("/");                  /* change working direcotry (not on NFS!) */
   umask(0);                    /* clear our file mode creation mask */

#endif

   return SUCCESS;
}

/*------------------------------------------------------------------*/

/* Parameter extraction from configuration file similar to win.ini */

typedef struct {
   char *param;
   char *uparam;
   char *value;
} CONFIG_PARAM;

typedef struct {
   char *section_name;
   int n_params;
   CONFIG_PARAM *config_param;
} LB_CONFIG;

LB_CONFIG *lb_config = NULL;
int n_lb_config = 0;

char _topgroup[256];
char _condition[256];
time_t cfgfile_mtime = 0;

/*-------------------------------------------------------------------*/

void check_config_file(BOOL force)
{
   struct stat cfg_stat;

   if (force) {
      parse_config_file(config_file);
      return;
   }

   /* force re-read configuration file if changed */
   if (stat(config_file, &cfg_stat) == 0) {
      if (cfgfile_mtime < cfg_stat.st_mtime) {
         cfgfile_mtime = cfg_stat.st_mtime;
         parse_config_file(config_file);
      }
   } else
      eprintf("Cannot stat() config file \"%s\": %s\n", config_file, strerror(errno));
}

/*-------------------------------------------------------------------*/

void setcfg_topgroup(char *topgroup)
{
   strcpy(_topgroup, topgroup);
}

char *getcfg_topgroup()
{
   if (_topgroup[0])
      return _topgroup;

   return NULL;
}

/*------------------------------------------------------------------*/

int is_logbook(char *logbook)
{
   char str[256];

   if (strieq(logbook, "global"))
      return 0;

   /* check for 'global group' or 'global_xxx' */
   strlcpy(str, logbook, sizeof(str));
   str[7] = 0;
   return !strieq(str, "global ");
}

/*-------------------------------------------------------------------*/

void set_condition(char *c)
{
   strlcpy(_condition, c, sizeof(_condition));
}

/*-------------------------------------------------------------------*/

void evaluate_conditions(LOGBOOK * lbs, char attrib[MAX_N_ATTR][NAME_LENGTH])
{
   char condition[256], str[256];
   int index, i;

   condition[0] = 0;
   set_condition("");
   for (index = 0; index < lbs->n_attr; index++) {
      for (i = 0; i < MAX_N_LIST && attr_options[index][i][0]; i++) {

         if (strchr(attr_options[index][i], '{') && strchr(attr_options[index][i], '}')) {

            strlcpy(str, attr_options[index][i], sizeof(str));
            *strchr(str, '{') = 0;

            if (strieq(str, attrib[index])) {
               strlcpy(str, strchr(attr_options[index][i], '{') + 1, sizeof(str));
               if (*strchr(str, '}'))
                  *strchr(str, '}') = 0;

               if (condition[0] == 0)
                  strlcpy(condition, str, sizeof(condition));
               else {
                  strlcat(condition, ",", sizeof(condition));
                  strlcat(condition, str, sizeof(condition));
               }

               set_condition(condition);
               scan_attributes(lbs->name);
            }
         }
      }
   }
}

/*-------------------------------------------------------------------*/

BOOL match_param(char *str, char *param, int conditional_only)
{
   int ncl, npl, nand, i, j, k;
   char *p, pcond[256], clist[10][NAME_LENGTH], plist[10][NAME_LENGTH], alist[10][NAME_LENGTH];

   if (conditional_only && str[0] != '{')
      return FALSE;

   if (!_condition[0] || str[0] != '{')
      return (stricmp(str, param) == 0);

   p = str;
   if (strchr(p, '}'))
      p = strchr(p, '}') + 1;
   while (*p == ' ')
      p++;

   strlcpy(pcond, str, sizeof(pcond));
   if (strchr(pcond, '}'))
      *strchr(pcond, '}') = 0;
   if (strchr(pcond, '{'))
      *strchr(pcond, '{') = ' ';

   npl = strbreak(pcond, plist, 10, ",", FALSE);
   ncl = strbreak(_condition, clist, 10, ",", FALSE);

   for (i = 0; i < ncl; i++)
      for (j = 0; j < npl; j++)
         if (stricmp(clist[i], plist[j]) == 0) {
            /* condition matches */
            return stricmp(p, param) == 0;
         }

   /* check and'ed conditions */
   for (i = 0; i < npl; i++)
      if (strchr(plist[i], '&')) {
         nand = strbreak(plist[i], alist, 10, "&", FALSE);
         for (j = 0; j < nand; j++) {
            for (k = 0; k < ncl; k++)
               if (stricmp(clist[k], alist[j]) == 0)
                  break;

            if (k == ncl)
               return FALSE;
         }

         if (j == nand)
            return stricmp(p, param) == 0;
      }

   return 0;
}

/*-------------------------------------------------------------------*/

int param_compare(const void *p1, const void *p2)
{
   return stricmp(((CONFIG_PARAM *) p1)->uparam, ((CONFIG_PARAM *) p2)->uparam);
}

/*------------------------------------------------------------------*/

void free_config()
{
   int i, j;

   for (i = 0; i < n_lb_config; i++) {
      for (j = 0; j < lb_config[i].n_params; j++) {
         xfree(lb_config[i].config_param[j].param);
         xfree(lb_config[i].config_param[j].uparam);
         xfree(lb_config[i].config_param[j].value);
      }
      if (lb_config[i].config_param)
         xfree(lb_config[i].config_param);
      xfree(lb_config[i].section_name);
   }
   xfree(lb_config);
   lb_config = NULL;
   n_lb_config = 0;
}

/*------------------------------------------------------------------*/

int parse_config_file(char *file_name)
/* parse whole config file and store options in sorted list */
{
   char *str, *buffer, *p, *pstr;
   int index, i, j, fh, length;

   str = xmalloc(20000);

   /* open configuration file */
   fh = open(file_name, O_RDONLY | O_BINARY);
   if (fh < 0)
      return 0;
   length = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);
   buffer = xmalloc(length + 1);
   read(fh, buffer, length);
   buffer[length] = 0;
   close(fh);

   /* release previously allocated memory */
   if (lb_config)
      free_config();

   /* search group */
   p = buffer;
   index = 0;
   do {
      if (*p == '#' || *p == ';') {
         /* skip comment */
         while (*p && *p != '\n' && *p != '\r')
            p++;
      } else if (*p == '[') {
         p++;
         pstr = str;
         while (*p && *p != ']' && *p != '\n' && *p != '\r' && pstr - str < 10000)
            *pstr++ = *p++;
         *pstr = 0;

         /* allocate new group */
         if (!lb_config)
            lb_config = xmalloc(sizeof(LB_CONFIG));
         else
            lb_config = xrealloc(lb_config, sizeof(LB_CONFIG) * (n_lb_config + 1));
         lb_config[n_lb_config].section_name = xmalloc(strlen(str) + 1);
         lb_config[n_lb_config].n_params = 0;
         lb_config[n_lb_config].config_param = NULL;
         strcpy(lb_config[n_lb_config].section_name, str);

         /* enumerate parameters */
         i = 0;
         p = strchr(p, '\n');
         if (p)
            p++;
         while (p && *p && *p != '[') {
            pstr = str;
            while (*p && *p != '=' && *p != '\n' && *p != '\r' && pstr - str < 10000)
               *pstr++ = *p++;
            *pstr-- = 0;
            while (pstr > str && (*pstr == ' ' || *pstr == '\t' || *pstr == '='))
               *pstr-- = 0;

            if (*p == '=') {

               if (lb_config[n_lb_config].n_params == 0)
                  lb_config[n_lb_config].config_param = xmalloc(sizeof(CONFIG_PARAM));
               else
                  lb_config[n_lb_config].config_param = xrealloc(lb_config[n_lb_config].config_param,
                                                                 sizeof(CONFIG_PARAM) *
                                                                 (lb_config[n_lb_config].n_params + 1));
               lb_config[n_lb_config].config_param[i].param = xmalloc(strlen(str) + 1);
               lb_config[n_lb_config].config_param[i].uparam = xmalloc(strlen(str) + 1);

               strcpy(lb_config[n_lb_config].config_param[i].param, str);
               for (j = 0; j < (int) strlen(str); j++)
                  lb_config[n_lb_config].config_param[i].uparam[j] = toupper(str[j]);
               lb_config[n_lb_config].config_param[i].uparam[j] = 0;

               p++;
               while (*p == ' ' || *p == '\t')
                  p++;
               pstr = str;
               while (*p && *p != '\n' && *p != '\r' && pstr - str < 10000)
                  *pstr++ = *p++;
               *pstr-- = 0;
               while (*pstr == ' ' || *pstr == '\t')
                  *pstr-- = 0;

               lb_config[n_lb_config].config_param[i].value = xmalloc(strlen(str) + 1);
               strcpy(lb_config[n_lb_config].config_param[i].value, str);

               i++;
               lb_config[n_lb_config].n_params = i;
            }

            /* search for next line beginning */
            while (*p && *p != '\r' && *p != '\n')
               p++;
            while (*p && (*p == '\r' || *p == '\n'))
               p++;
         }

         /* sort parameter */
         // outcommented: not needed, might screw up group ordering
         //qsort(lb_config[n_lb_config].config_param, lb_config[n_lb_config].n_params, sizeof(CONFIG_PARAM),
         //      param_compare);

         n_lb_config++;
         index++;
      }

      /* search for next line beginning */
      while (*p && *p != '\r' && *p != '\n' && *p != '[')
         p++;
      while (*p && (*p == '\r' || *p == '\n'))
         p++;

   } while (*p);

   xfree(str);
   xfree(buffer);
   return 0;
}

/*-------------------------------------------------------------------*/

int getcfg_simple(char *group, char *param, char *value, int vsize, int conditional)
{
   int i, j, status;
   char uparam[256];

   if (strlen(param) >= sizeof(uparam))
      return 0;

   for (i = 0; i < (int) strlen(param); i++)
      uparam[i] = toupper(param[i]);
   uparam[i] = 0;
   value[0] = 0;

   for (i = 0; i < n_lb_config; i++)
      if (strieq(lb_config[i].section_name, group))
         break;

   if (i == n_lb_config)
      return 0;

   for (j = 0; j < lb_config[i].n_params; j++)
      if (match_param(lb_config[i].config_param[j].uparam, uparam, conditional)) {
         status = strchr(lb_config[i].config_param[j].uparam, '{') ? 2 : 1;
         strlcpy(value, lb_config[i].config_param[j].value, vsize);
         return status;
      }

   return 0;
}

/*-------------------------------------------------------------------*/

int enumgrp(int index, char *group)
{
   if (index < n_lb_config) {
      strcpy(group, lb_config[index].section_name);
      return 1;
   }

   return 0;
}

/*-------------------------------------------------------------------*/

int getcfg(char *group, char *param, char *value, int vsize)
/*
 Read parameter from configuration file.

 - if group == [global] and top group exists, read
 from [global <top group>]

 - if parameter not in [global <top group>], read from [global]

 - if group is logbook, read from logbook section

 - if parameter not in [<logbook>], read from [global <top group>]
 or [global]
 */
{
   char str[256];
   int status;

   /* if group is [global] and top group exists, read from there */
   if (strieq(group, "global") && getcfg_topgroup()) {
      sprintf(str, "global %s", getcfg_topgroup());
      status = getcfg(str, param, value, vsize);
      if (status)
         return status;
   }

   /* first check if parameter is under condition */
   if (_condition[0]) {
      status = getcfg_simple(group, param, value, vsize, TRUE);
      if (status)
         return status;
   }

   status = getcfg_simple(group, param, value, vsize, FALSE);
   if (status)
      return status;

   /* if parameter not found in logbook, look in [global] section */
   if (!group || is_logbook(group))
      return getcfg("global", param, value, vsize);

   return 0;
}

/*-------------------------------------------------------------------*/

char *find_param(char *buf, char *group, char *param)
{
   char *str, *p, *pstr, *pstart;

   /* search group */
   str = xmalloc(10000);
   p = buf;
   do {
      if (*p == '[') {
         p++;
         pstr = str;
         while (*p && *p != ']' && *p != '\n')
            *pstr++ = *p++;
         *pstr = 0;
         if (strieq(str, group)) {
            /* search parameter */
            p = strchr(p, '\n');
            if (p)
               p++;
            while (p && *p && *p != '[') {
               pstr = str;
               pstart = p;
               while (*p && *p != '=' && *p != '\n')
                  *pstr++ = *p++;
               *pstr-- = 0;
               while (pstr > str && (*pstr == ' ' || *pstr == '=' || *pstr == '\t'))
                  *pstr-- = 0;

               if (match_param(str, param, FALSE)) {
                  xfree(str);
                  return pstart;
               }

               if (p)
                  p = strchr(p, '\n');
               if (p)
                  p++;
            }
         }
      }
      if (p)
         p = strchr(p, '\n');
      if (p)
         p++;
   } while (p);

   xfree(str);

   /* now search if in [global] section */
   if (!strieq(group, "global"))
      return find_param(buf, "global", param);

   return NULL;
}

/*-------------------------------------------------------------------*/

int is_group(char *group)
{
   int i;

   for (i = 0; i < n_lb_config; i++)
      if (strieq(group, lb_config[i].section_name))
         return 1;
   return 0;
}

/*------------------------------------------------------------------*/

int enumcfg(char *group, char *param, int psize, char *value, int vsize, int index)
{
   int i;

   for (i = 0; i < n_lb_config; i++)
      if (strieq(group, lb_config[i].section_name)) {

         if (index < lb_config[i].n_params) {
            strlcpy(param, lb_config[i].config_param[index].param, psize);
            strlcpy(value, lb_config[i].config_param[index].value, vsize);
            return 1;
         }

         return 0;

      }

   return 0;
}

/*-------------------------------------------------------------------*/

int exist_top_group()
{
   int i;
   char str[256];

   for (i = 0;; i++) {
      if (!enumcfg("global", str, sizeof(str), NULL, 0, i))
         break;
      str[9] = 0;
      if (strieq(str, "top group"))
         return 1;
   }

   return 0;
}

/*-------------------------------------------------------------------*/

char *_locbuffer = NULL;
char **_porig, **_ptrans;
time_t _locfile_mtime = 0;

/* check if language file changed and if so reload it */
int check_language()
{
   char language[256], file_name[256], *p;
   int fh, length, n;
   struct stat cfg_stat;

   getcfg("global", "Language", language, sizeof(language));

   /* set locale for strftime */
   if (language[0])
      setlocale(LC_ALL, language);
   else
      setlocale(LC_ALL, "english");

   /* force re-read configuration file if changed */
   strlcpy(file_name, resource_dir, sizeof(file_name));
   strlcat(file_name, "resources", sizeof(file_name));
   strlcat(file_name, DIR_SEPARATOR_STR, sizeof(file_name));
   strlcat(file_name, "eloglang.", sizeof(file_name));
   strlcat(file_name, language, sizeof(file_name));

   if (stat(file_name, &cfg_stat) == 0) {
      if (_locfile_mtime != cfg_stat.st_mtime) {
         _locfile_mtime = cfg_stat.st_mtime;

         if (_locbuffer) {
            xfree(_locbuffer);
            _locbuffer = NULL;
         }
      }
   }

   if (strieq(language, "english") || language[0] == 0) {
      if (_locbuffer) {
         xfree(_locbuffer);
         _locbuffer = NULL;
      }
   } else {
      if (_locbuffer == NULL) {
         fh = open(file_name, O_RDONLY | O_BINARY);
         if (fh < 0)
            return -1;

         length = lseek(fh, 0, SEEK_END);
         lseek(fh, 0, SEEK_SET);
         _locbuffer = xmalloc(length + 1);
         read(fh, _locbuffer, length);
         _locbuffer[length] = 0;
         close(fh);

         /* scan lines, setup orig-translated pointers */
         p = _locbuffer;
         n = 0;
         do {
            while (*p && (*p == '\r' || *p == '\n'))
               p++;

            if (*p && (*p == ';' || *p == '#' || *p == ' ' || *p == '\t')) {
               while (*p && *p != '\n' && *p != '\r')
                  p++;
               continue;
            }

            if (n == 0) {
               _porig = xmalloc(sizeof(char *) * 2);
               _ptrans = xmalloc(sizeof(char *) * 2);
            } else {
               _porig = xrealloc(_porig, sizeof(char *) * (n + 2));
               _ptrans = xrealloc(_ptrans, sizeof(char *) * (n + 2));
            }

            _porig[n] = p;
            while (*p && (*p != '=' && *p != '\r' && *p != '\n'))
               p++;

            if (*p && *p != '=')
               continue;

            _ptrans[n] = p + 1;
            while (*_ptrans[n] == ' ' || *_ptrans[n] == '\t')
               _ptrans[n]++;

            /* remove '=' and surrounding blanks */
            while (*p == '=' || *p == ' ' || *p == '\t')
               *p-- = 0;

            p = _ptrans[n];
            while (*p && *p != '\n' && *p != '\r')
               p++;

            if (p)
               *p++ = 0;

            n++;
         } while (p && *p);

         _porig[n] = NULL;
         _ptrans[n] = NULL;
      }
   }

   return 0;
}

/*-------------------------------------------------------------------*/

/* localization support */
char *loc(char *orig)
{
   int n;
   char language[256];
   static char result[256];

   if (!_locbuffer)
      return orig;

   /* search string and return translation */
   for (n = 0; _porig[n]; n++)
      if (strcmp(orig, _porig[n]) == 0) {
         if (*_ptrans[n])
            return _ptrans[n];
         return orig;
      }

   /* special case: "Change %s" */
   if (strstr(orig, "Change ") && strcmp(orig, "Change %s") != 0) {
      sprintf(result, loc("Change %s"), orig + 7);
      return result;
   }

   /* special case: some intrinsic commands */
   if (strstr(orig, "GetPwdFile")) {
      strcpy(result, orig);
      return result;
   }

   getcfg("global", "Language", language, sizeof(language));
   eprintf("Language error: string \"%s\" not found for language \"%s\"\n", orig, language);

   return orig;
}

/*-------------------------------------------------------------------*/

/* translate back from localized string to english */

char *unloc(char *orig)
{
   int n;

   if (!_locbuffer)
      return orig;

   /* search string and return translation */
   for (n = 0; _ptrans[n]; n++)
      if (strcmp(orig, _ptrans[n]) == 0) {
         if (*_porig[n])
            return _porig[n];
         return orig;
      }

   eprintf("Language error: string \"%s\" not found in English\n", orig);

   return orig;
}

/*-------------------------------------------------------------------*/

char *month_name(int m)
/* return name of month in current locale, m=0..11 */
{
   struct tm ts;
   static char name[32];

   memset(&ts, 0, sizeof(ts));
   ts.tm_mon = m;
   ts.tm_mday = 15;
   ts.tm_year = 2000;

   mktime(&ts);
   strftime(name, sizeof(name), "%B", &ts);
   return name;
}

/*-------------------------------------------------------------------*/

time_t date_to_ltime(char *date)
{
   struct tm tms;
   int i, date_zone, local_zone;
   time_t ltime;

   memset(&tms, 0, sizeof(struct tm));

   if (strlen(date) > 25) {

      /* RFC2822 compliant date */
      for (i = 0; i < 12; i++)
         if (strncmp(date + 8, mname[i], 3) == 0)
            break;
      tms.tm_mon = i;

      tms.tm_mday = atoi(date + 5);
      tms.tm_hour = atoi(date + 17);
      tms.tm_min = atoi(date + 20);
      tms.tm_sec = atoi(date + 23);
      tms.tm_year = atoi(date + 12) - 1900;
      tms.tm_isdst = -1;

      if (tms.tm_year < 90)
         tms.tm_year += 100;

      ltime = mktime(&tms);

      /* correct for difference between local time zone (used by mktime) and time zone of date */
      date_zone = atoi(date + 26);

      /* correct for incorrect date_zone */
      if (date_zone > 2400 || date_zone < -2400)
         date_zone = 0;
      date_zone = (abs(date_zone) % 100) * 60 + (date_zone) / 100 * 3600;

      local_zone = my_timezone();
      if (tms.tm_isdst)
         local_zone -= 3600;

      ltime = ltime - local_zone - date_zone;

   } else {

      /* ctime() complient date */
      for (i = 0; i < 12; i++)
         if (strncmp(date + 4, mname[i], 3) == 0)
            break;
      tms.tm_mon = i;

      tms.tm_mday = atoi(date + 8);
      tms.tm_hour = atoi(date + 11);
      tms.tm_min = atoi(date + 14);
      tms.tm_sec = atoi(date + 17);
      tms.tm_year = atoi(date + 20) - 1900;
      tms.tm_isdst = -1;

      if (tms.tm_year < 90)
         tms.tm_year += 100;

      ltime = mktime(&tms);
   }

   return ltime;
}

/*-------------------------------------------------------------------*/

void check_config()
{
   check_config_file(FALSE);
   check_language();
}

/*-------------------------------------------------------------------*/

void retrieve_email_from(LOGBOOK * lbs, char *ret, char *ret_name, char attrib[MAX_N_ATTR][NAME_LENGTH])
{
   char email_from[256], email_from_name[256], str[256], *p, login_name[256];
   char slist[MAX_N_ATTR + 10][NAME_LENGTH], svalue[MAX_N_ATTR + 10][NAME_LENGTH];
   int i;

   if (getcfg(lbs->name, "Use Email from", str, sizeof(str))) {
      strlcpy(email_from, str, sizeof(email_from));
      strlcpy(email_from_name, str, sizeof(email_from));
   } else if (isparam("full_name") && isparam("user_email")) {
      strlcpy(email_from_name, getparam("full_name"), sizeof(email_from_name));
      strlcat(email_from_name, " <", sizeof(email_from_name));
      strlcat(email_from_name, getparam("user_email"), sizeof(email_from_name));
      strlcat(email_from_name, ">", sizeof(email_from_name));
      strlcpy(email_from, getparam("user_email"), sizeof(email_from));
   } else if (getcfg(lbs->name, "Default Email from", str, sizeof(str))) {
      strlcpy(email_from, str, sizeof(email_from));
      strlcpy(email_from_name, str, sizeof(email_from));
   } else {
      sprintf(email_from_name, "ELog <ELog@%s>", host_name);
      sprintf(email_from, "ELog@%s", host_name);
   }

   if (attrib) {
      i = build_subst_list(lbs, slist, svalue, attrib, TRUE);
      strsubst_list(email_from_name, sizeof(email_from_name), slist, svalue, i);
      strsubst_list(email_from, sizeof(email_from), slist, svalue, i);

      /* remove possible 'mailto:' */
      if ((p = strstr(email_from_name, "mailto:")) != NULL)
         strcpy(p, p + 7);
      if ((p = strstr(email_from, "mailto:")) != NULL)
         strcpy(p, p + 7);
   }

   /* if nothing available, figure out email from an administrator */
   if (strchr(email_from, '@') == NULL) {
      for (i = 0;; i++) {
         if (!enum_user_line(lbs, i, login_name, sizeof(login_name)))
            break;
         get_user_line(lbs, login_name, NULL, NULL, email_from, NULL, NULL);
         sprintf(email_from_name, "%s <%s>", login_name, email_from);
         if (is_admin_user(lbs->name, login_name) && strchr(email_from, '@'))
            break;
      }
   }

   if (ret)
      strcpy(ret, email_from);
   if (ret_name)
      strcpy(ret_name, email_from_name);
}

/*------------------------------------------------------------------*/

void el_decode(char *message, char *key, char *result)
{
   char *pc, *ph;

   if (result == NULL)
      return;

   *result = 0;

   ph = strstr(message, "========================================");
   if (ph == NULL)
      return;

   do {
      if (ph[40] == '\r' || ph[40] == '\n')
         break;
      ph = strstr(ph + 40, "========================================");
      if (ph == NULL)
         return;

   } while (1);

   /* go through all lines */
   for (pc = message; pc < ph;) {

      if (strncmp(pc, key, strlen(key)) == 0) {
         pc += strlen(key);
         while (*pc != '\n' && *pc != '\r')
            *result++ = *pc++;
         *result = 0;
         return;
      }

      pc = strchr(pc, '\n');
      if (pc == NULL)
         return;
      while (*pc && (*pc == '\n' || *pc == '\r'))
         pc++;
   }
}

/*------------------------------------------------------------------*/

void el_enum_attr(char *message, int n, char *attr_name, char *attr_value)
{
   char *p, str[NAME_LENGTH], tmp[NAME_LENGTH];
   int i;

   p = message;
   for (i = 0; i <= n; i++) {
      strlcpy(str, p, sizeof(str));
      if (strchr(str, '\n'))
         *strchr(str, '\n') = 0;
      if (strchr(str, '\r'))
         *strchr(str, '\r') = 0;

      if (strcmp(str, "========================================") == 0)
         break;

      p = strchr(p, '\n');
      if (!p) {
         str[0] = 0;            /* not a valid line */
         break;
      }
      while (*p == '\n' || *p == '\r')
         p++;

      if (strchr(str, ':')) {
         strcpy(tmp, str);
         *strchr(tmp, ':') = 0;

         if (strieq(tmp, "$@MID@$") || strieq(tmp, "Date") || strieq(tmp, "Attachment") || strieq(tmp,
                                                                                                  "Reply To")
             || strieq(tmp, "In Reply To") || strieq(tmp, "Encoding") || strieq(tmp, "Locked by"))
            i--;
      }
   }

   attr_name[0] = 0;
   attr_value[0] = 0;
   if (strchr(str, ':')) {
      strlcpy(attr_name, str, NAME_LENGTH);
      *strchr(attr_name, ':') = 0;
      strlcpy(attr_value, strchr(str, ':') + 2, NAME_LENGTH);
   }

}

/*------------------------------------------------------------------*/

/* Simplified copy of fnmatch() for Cygwin where fnmatch is not defined */

#define EOS '\0'

int fnmatch1(const char *pattern, const char *string)
{
   const char *stringstart;
   char c, test;

   for (stringstart = string;;)
      switch (c = *pattern++) {
      case EOS:
         return (*string == EOS ? 0 : 1);
      case '?':
         if (*string == EOS)
            return (1);
         ++string;
         break;
      case '*':
         c = *pattern;
         /* Collapse multiple stars. */
         while (c == '*')
            c = *++pattern;

         /* Optimize for pattern with * at end or before /. */
         if (c == EOS)
            return (0);

         /* General case, use recursion. */
         while ((test = *string) != EOS) {
            if (!fnmatch1(pattern, string))
               return (0);
            ++string;
         }
         return (1);
         /* FALLTHROUGH */
      default:
         if (c != *string)
            return (1);
         string++;
         break;
      }
}

/*------------------------------------------------------------------*/

int ss_file_find(char *path, char *pattern, char **plist)
/********************************************************************
 Routine: ss_file_find

 Purpose: Return list of files matching 'pattern' from the 'path' location

 Input:
 char  *path             Name of a file in file system to check
 char  *pattern          pattern string (wildcard allowed)

 Output:
 char  **plist           pointer to the file list

 Function value:
 int                     Number of files matching request

 \********************************************************************/
{
#ifdef OS_UNIX
   DIR *dir_pointer;
   struct dirent *dp;
   int i;

   if ((dir_pointer = opendir(path)) == NULL)
      return 0;
   *plist = (char *) xmalloc(MAX_PATH_LENGTH);
   i = 0;
   for (dp = readdir(dir_pointer); dp != NULL; dp = readdir(dir_pointer)) {
      if (fnmatch1(pattern, dp->d_name) == 0) {
         *plist = (char *) xrealloc(*plist, (i + 1) * MAX_PATH_LENGTH);
         strncpy(*plist + (i * MAX_PATH_LENGTH), dp->d_name, strlen(dp->d_name));
         *(*plist + (i * MAX_PATH_LENGTH) + strlen(dp->d_name)) = '\0';
         i++;
      }
   }
   closedir(dir_pointer);
   return i;
#endif

#ifdef OS_WINNT
   HANDLE pffile;
   LPWIN32_FIND_DATA lpfdata;
   char str[255];
   int i, first;

   strlcpy(str, path, sizeof(str));
   strlcat(str, "\\", sizeof(str));
   strlcat(str, pattern, sizeof(str));
   first = 1;
   i = 0;
   lpfdata = xmalloc(sizeof(WIN32_FIND_DATA));
   *plist = (char *) xmalloc(MAX_PATH_LENGTH);
   pffile = FindFirstFile(str, lpfdata);
   if (pffile == INVALID_HANDLE_VALUE)
      return 0;
   first = 0;
   *plist = (char *) xrealloc(*plist, (i + 1) * MAX_PATH_LENGTH);
   strncpy(*plist + (i * MAX_PATH_LENGTH), lpfdata->cFileName, strlen(lpfdata->cFileName));
   *(*plist + (i * MAX_PATH_LENGTH) + strlen(lpfdata->cFileName)) = '\0';
   i++;
   while (FindNextFile(pffile, lpfdata)) {
      *plist = (char *) xrealloc(*plist, (i + 1) * MAX_PATH_LENGTH);
      strncpy(*plist + (i * MAX_PATH_LENGTH), lpfdata->cFileName, strlen(lpfdata->cFileName));
      *(*plist + (i * MAX_PATH_LENGTH) + strlen(lpfdata->cFileName)) = '\0';
      i++;
   }
   xfree(lpfdata);
   return i;
#endif
}

/*------------------------------------------------------------------*/

int eli_compare(const void *e1, const void *e2)
{

   if (((EL_INDEX *) e1)->file_time < ((EL_INDEX *) e2)->file_time)
      return -1;
   if (((EL_INDEX *) e1)->file_time >= ((EL_INDEX *) e2)->file_time)
      return 1;
   return 0;
}

/*------------------------------------------------------------------*/

int el_build_index(LOGBOOK * lbs, BOOL rebuild)
/* scan all ??????a.log files and build an index table in eli[] */
{
   char *file_list, str[256], error_str[256], date[256], dir[256], file_name[MAX_PATH_LENGTH], *buffer, *p,
       *pn, in_reply_to[80];
   int index, n, length;
   int i, fh, len;
   unsigned char digest[16];

   if (rebuild) {
      xfree(lbs->el_index);
      xfree(lbs->n_el_index);
   }

   /* check if this logbook has same data dir as previous */
   for (i = 0; lb_list[i].name[0] && &lb_list[i] != lbs; i++)
      if (strieq(lb_list[i].data_dir, lbs->data_dir))
         break;

   if (strieq(lb_list[i].data_dir, lbs->data_dir) && &lb_list[i] != lbs) {
      if (verbose)
         eprintf("\n  Same index as logbook %s\n", lb_list[i].name);

      lbs->el_index = lb_list[i].el_index;
      lbs->n_el_index = lb_list[i].n_el_index;
      return EL_SUCCESS;
   }

   lbs->n_el_index = xmalloc(sizeof(int));
   *lbs->n_el_index = 0;
   lbs->el_index = xmalloc(0);

   /* get data directory */
   strcpy(dir, lbs->data_dir);

   if (verbose > 1) {
      /* show MD5 from config file */

      load_config_section(lbs->name, &buffer, error_str);
      if (error_str[0])
         eprintf(error_str);
      else {
         remove_crlf(buffer);
         MD5_checksum(buffer, strlen(buffer), digest);
         eprintf("\n\nConfig [%s],                           MD5=", lbs->name);

         for (i = 0; i < 16; i++)
            eprintf("%02X", digest[i]);
         eprintf("\n\n");
      }
      if (buffer)
         xfree(buffer);
   }

   file_list = NULL;
   n = ss_file_find(dir, "??????a.log", &file_list);
   if (n == 0) {
      if (file_list)
         xfree(file_list);
      file_list = NULL;

      n = ss_file_find(dir, "??????.log", &file_list);
      if (n > 0)
         return EL_UPGRADE;

      return EL_EMPTY;
   }

   if (verbose > 1)
      eprintf("Entries:\n");

   /* go through all files */
   for (index = 0; index < n; index++) {
      strlcpy(file_name, dir, sizeof(file_name));
      strlcat(file_name, file_list + index * MAX_PATH_LENGTH, sizeof(file_name));

      fh = open(file_name, O_RDONLY | O_BINARY, 0644);

      if (fh < 0) {
         sprintf(str, "Cannot open file \"%s\"", file_name);
         eprintf("%s; %s\n", str, strerror(errno));
         return EL_FILE_ERROR;
      }

      /* read file into buffer */
      length = lseek(fh, 0, SEEK_END);

      if (length > 0) {
         buffer = xmalloc(length + 1);
         lseek(fh, 0, SEEK_SET);
         read(fh, buffer, length);
         buffer[length] = 0;
         close(fh);

         /* go through buffer */
         p = buffer;

         do {
            p = strstr(p, "$@MID@$:");

            if (p) {
               lbs->el_index = xrealloc(lbs->el_index, sizeof(EL_INDEX) * (*lbs->n_el_index + 1));
               if (lbs->el_index == NULL) {
                  eprintf("Not enough memory to allocate entry index\n");
                  return EL_MEM_ERROR;
               }

               strcpy(str, file_list + index * MAX_PATH_LENGTH);
               strcpy(lbs->el_index[*lbs->n_el_index].file_name, str);

               el_decode(p, "Date: ", date);
               el_decode(p, "In reply to: ", in_reply_to);

               lbs->el_index[*lbs->n_el_index].file_time = date_to_ltime(date);

               lbs->el_index[*lbs->n_el_index].message_id = atoi(p + 8);
               lbs->el_index[*lbs->n_el_index].offset = p - buffer;
               lbs->el_index[*lbs->n_el_index].in_reply_to = atoi(in_reply_to);

               pn = strstr(p + 8, "$@MID@$:");
               if (pn)
                  len = pn - p;
               else
                  len = strlen(p);

               MD5_checksum(p, len, lbs->el_index[*lbs->n_el_index].md5_digest);

               if (lbs->el_index[*lbs->n_el_index].message_id > 0) {
                  if (verbose > 1) {
                     eprintf("  ID %3d, %s, ofs %5d, %s, MD5=", lbs->el_index[*lbs->n_el_index].message_id,
                             str, lbs->el_index[*lbs->n_el_index].offset,
                             lbs->el_index[*lbs->n_el_index].in_reply_to ? "reply" : "thead");

                     for (i = 0; i < 16; i++)
                        eprintf("%02X", lbs->el_index[*lbs->n_el_index].md5_digest[i]);
                     eprintf("\n");
                  }

                  /* valid ID */
                  (*lbs->n_el_index)++;
               }

               p += 8;
            }

         } while (p);

         xfree(buffer);
      }

   }

   xfree(file_list);

   /* sort entries according to date */
   qsort(lbs->el_index, *lbs->n_el_index, sizeof(EL_INDEX), eli_compare);

   if (verbose > 1) {
      eprintf("After sort:\n");
      for (i = 0; i < *lbs->n_el_index; i++)
         eprintf("  ID %3d, %s, ofs %5d\n", lbs->el_index[i].message_id, lbs->el_index[i].file_name,
                 lbs->el_index[i].offset);
   }

   return EL_SUCCESS;
}

/*------------------------------------------------------------------*/

int el_index_logbooks()
{
   char str[256], data_dir[256], logbook[256], cwd[256], *p;
   int i, j, n, status;

   if (lb_list) {
      for (i = 0; lb_list[i].name[0]; i++) {
         if (lb_list[i].el_index != NULL) {
            xfree(lb_list[i].el_index);
            xfree(lb_list[i].n_el_index);

            /* check if other logbook uses same index */
            for (j = i + 1; lb_list[j].name[0]; j++) {
               /* mark that logbook already freed */
               if (lb_list[j].el_index == lb_list[i].el_index)
                  lb_list[j].el_index = NULL;
            }
         }
      }
      xfree(lb_list);
   }

   /* count logbooks */
   for (i = n = 0;; i++) {
      if (!enumgrp(i, str))
         break;

      if (!is_logbook(str))
         continue;

      n++;
   }

   lb_list = xcalloc(sizeof(LOGBOOK), n + 1);
   for (i = n = 0;; i++) {
      if (!enumgrp(i, logbook))
         break;

      if (!is_logbook(logbook))
         continue;

      /* check for duplicate name */
      for (j = 0; j < i && lb_list[j].name[0]; j++)
         if (strieq(lb_list[j].name, logbook)) {
            eprintf("Error in configuration file: Duplicate logbook \"%s\"\n", logbook);
            return EL_DUPLICATE;
         }

      /* store logbook in list */
      strcpy(lb_list[n].name, logbook);
      strcpy(lb_list[n].name_enc, logbook);
      url_encode(lb_list[n].name_enc, sizeof(lb_list[n].name_enc));

      /* get data dir from configuration file (old method) */
      if (getcfg(logbook, "Data dir", str, sizeof(str))) {
         if (str[0] == DIR_SEPARATOR || str[1] == ':')
            strlcpy(data_dir, str, sizeof(data_dir));
         else {
            strlcpy(data_dir, resource_dir, sizeof(data_dir));
            strlcat(data_dir, str, sizeof(data_dir));
         }
      } else {
         /* use logbook_dir + "Subdir" (new method) */
         strlcpy(data_dir, logbook_dir, sizeof(data_dir));
         if (data_dir[strlen(data_dir) - 1] != DIR_SEPARATOR)
            strlcat(data_dir, DIR_SEPARATOR_STR, sizeof(data_dir));

         if (getcfg(logbook, "Subdir", str, sizeof(str))) {
            if (str[0] == DIR_SEPARATOR)
               strlcpy(data_dir, str, sizeof(data_dir));
            else
               strlcat(data_dir, str, sizeof(data_dir));
         } else
            strlcat(data_dir, logbook, sizeof(data_dir));       /* use logbook name as default */
      }

      if (data_dir[strlen(data_dir) - 1] != DIR_SEPARATOR)
         strlcat(data_dir, DIR_SEPARATOR_STR, sizeof(data_dir));

      /* create data directory if not existing */
      getcwd(cwd, sizeof(cwd));

      j = chdir(data_dir);
      if (j < 0) {
         p = data_dir;
         if (*p == DIR_SEPARATOR) {
            chdir(DIR_SEPARATOR_STR);
            p++;
         }
         if (p[1] == ':') {
            strcpy(str, p);
            if (str[2] == DIR_SEPARATOR)
               str[3] = 0;
            else
               str[2] = 0;

            chdir(str);
            p += strlen(str);
         }

         do {
            if (strchr(p, DIR_SEPARATOR)) {
               strlcpy(str, p, sizeof(str));
               *strchr(str, DIR_SEPARATOR) = 0;
               p = strchr(p, DIR_SEPARATOR) + 1;
            } else {
               strlcpy(str, p, sizeof(str));
               p = NULL;
            }

            j = chdir(str);
            if (j < 0) {

#ifdef OS_WINNT
               j = mkdir(str);
#else
               j = mkdir(str, 0755);
#endif

               if (j == 0) {
                  if (verbose)
                     eprintf("Created directory \"%s\"\n", str);
               } else {
                  eprintf("el_index_logbooks: %s\n", strerror(errno));
                  eprintf("Cannot create directory \"%s\"\n", str);
               }

               chdir(str);
            }

         } while (p && *p);
      }

      chdir(cwd);
      strcpy(lb_list[n].data_dir, data_dir);
      lb_list[n].el_index = NULL;

      if (verbose)
         eprintf("Indexing logbook \"%s\" in \"%s\" ... ", logbook, lb_list[n].data_dir);
      eflush();
      status = el_build_index(&lb_list[n], FALSE);
      if (verbose)
         if (status == EL_SUCCESS)
            eprintf("ok\n");

      if (status == EL_EMPTY) {
         if (verbose)
            eprintf("Found empty logbook \"%s\"\n", logbook);
      } else if (status == EL_UPGRADE) {
         eprintf("Please upgrade data files in \"%s\" with the elconv program.\n", data_dir);
         return EL_UPGRADE;
      } else if (status != EL_SUCCESS) {
         eprintf("Error generating index.\n");
         return status;
      }

      n++;
   }

   /* if top groups defined, set top group in logbook */
   if (exist_top_group()) {
      LBLIST phier;

      phier = get_logbook_hierarchy();
      for (i = 0; i < phier->n_members; i++)
         if (phier->member[i]->is_top)
            for (j = 0; lb_list[j].name[0]; j++)
               if (is_logbook_in_group(phier->member[i], lb_list[j].name))
                  strcpy(lb_list[j].top_group, phier->member[i]->name);

      free_logbook_hierarchy(phier);
   }

   if (!load_password_files())
      return EL_INVAL_FILE;

   return EL_SUCCESS;
}

/*------------------------------------------------------------------*/

int el_search_message(LOGBOOK * lbs, int mode, int message_id, BOOL head_only)
/********************************************************************
 Routine: el_search_message

 Purpose: Search for a specific message in a logbook

 Input:
 int   mode              Search mode, EL_FIRST, EL_LAST, EL_NEXT, EL_PREV
 int   message_id        Message id for EL_NEXT and EL_PREV

 Function value:
 int                     New message id

 \********************************************************************/
{
   int i;

   if (lbs->n_el_index == 0)
      return 0;

   if (mode == EL_FIRST) {
      if (head_only) {
         for (i = 0; i < *lbs->n_el_index; i++)
            if (lbs->el_index[i].in_reply_to == 0)
               return lbs->el_index[i].message_id;

         return 0;
      }
      if (*lbs->n_el_index == 0)
         return 0;
      return lbs->el_index[0].message_id;
   }

   if (mode == EL_LAST) {
      if (head_only) {
         for (i = *lbs->n_el_index - 1; i >= 0; i--)
            if (lbs->el_index[i].in_reply_to == 0)
               return lbs->el_index[i].message_id;

         return 0;
      }
      if (*lbs->n_el_index == 0)
         return 0;
      return lbs->el_index[*lbs->n_el_index - 1].message_id;
   }

   if (mode == EL_NEXT) {
      for (i = 0; i < *lbs->n_el_index; i++)
         if (lbs->el_index[i].message_id == message_id)
            break;

      if (i == *lbs->n_el_index)
         return 0;              // message not found

      if (i == *lbs->n_el_index - 1)
         return 0;              // last message

      if (head_only) {
         for (i++; i < *lbs->n_el_index; i++)
            if (lbs->el_index[i].in_reply_to == 0)
               return lbs->el_index[i].message_id;

         return 0;
      }

      return lbs->el_index[i + 1].message_id;
   }

   if (mode == EL_PREV) {
      for (i = 0; i < *lbs->n_el_index; i++)
         if (lbs->el_index[i].message_id == message_id)
            break;

      if (i == *lbs->n_el_index)
         return 0;              // message not found

      if (i == 0)
         return 0;              // first message

      if (head_only) {
         for (i--; i >= 0; i--)
            if (lbs->el_index[i].in_reply_to == 0)
               return lbs->el_index[i].message_id;

         return 0;
      }

      return lbs->el_index[i - 1].message_id;
   }

   return 0;
}

/*------------------------------------------------------------------*/

int el_retrieve(LOGBOOK * lbs, int message_id, char *date, char attr_list[MAX_N_ATTR][NAME_LENGTH],
                char attrib[MAX_N_ATTR][NAME_LENGTH], int n_attr, char *text, int *textsize,
                char *in_reply_to, char *reply_to, char attachment[MAX_ATTACHMENTS][MAX_PATH_LENGTH],
                char *encoding, char *locked_by)
/********************************************************************
 Routine: el_retrieve

 Purpose: Retrieve an ELog entry by its message tab

 Input:
 LOGBOOK lbs             Logbook structure
 int    message_id       Message ID to retrieve
 int    *size            Size of text buffer

 Output:
 char   *tag             tag of retrieved message
 char   *date            Date/time of message recording
 char   attr_list        Names of attributes
 char   attrib           Values of attributes
 int    n_attr           Number of attributes
 char   *text            Message text
 char   *in_reply_to     Original message if this one is a reply
 char   *reply_to        Replies for current message
 char   *attachment[]    File attachments
 char   *encoding        Encoding of message
 char   *locked_by       User/Host if locked for editing
 int    *size            Actual message text size

 Function value:
 EL_SUCCESS              Successful completion
 EL_EMPTY                Logbook is empty
 EL_NO_MSG               Message doesn't exist
 EL_FILE_ERROR           Internal error

 \********************************************************************/
{
   int i, index, size, fh;
   char str[NAME_LENGTH], file_name[256], *p;
   char *message, attachment_all[64 * MAX_ATTACHMENTS];

   if (message_id == 0)
      /* open most recent message */
      message_id = el_search_message(lbs, EL_LAST, 0, FALSE);

   if (message_id == 0)
      return EL_EMPTY;

   for (index = 0; index < *lbs->n_el_index; index++)
      if (lbs->el_index[index].message_id == message_id)
         break;

   if (index == *lbs->n_el_index)
      return EL_NO_MSG;

   sprintf(file_name, "%s%s", lbs->data_dir, lbs->el_index[index].file_name);
   fh = open(file_name, O_RDONLY | O_BINARY, 0644);
   if (fh < 0) {
      /* file might have been deleted, rebuild index */
      el_build_index(lbs, TRUE);
      return el_retrieve(lbs, message_id, date, attr_list, attrib, n_attr, text, textsize, in_reply_to,
                         reply_to, attachment, encoding, locked_by);
   }

   message = xmalloc(TEXT_SIZE + 1000);

   lseek(fh, lbs->el_index[index].offset, SEEK_SET);
   i = my_read(fh, message, TEXT_SIZE + 1000 - 1);
   if (i <= 0) {
      xfree(message);
      close(fh);
      return EL_FILE_ERROR;
   }

   message[i] = 0;
   close(fh);

   if (strncmp(message, "$@MID@$:", 8) != 0) {
      xfree(message);
      /* file might have been edited, rebuild index */
      el_build_index(lbs, TRUE);
      return el_retrieve(lbs, message_id, date, attr_list, attrib, n_attr, text, textsize, in_reply_to,
                         reply_to, attachment, encoding, locked_by);
   }

   /* check for correct ID */
   if (atoi(message + 8) != message_id) {
      xfree(message);
      return EL_FILE_ERROR;
   }

   /* decode message size */
   p = strstr(message + 8, "$@MID@$:");
   if (p == NULL)
      size = strlen(message);
   else
      size = p - message;

   message[size] = 0;

   /* decode message */
   if (date)
      el_decode(message, "Date: ", date);
   if (reply_to)
      el_decode(message, "Reply to: ", reply_to);
   if (in_reply_to)
      el_decode(message, "In reply to: ", in_reply_to);

   if (n_attr == -1) {
      /* derive attribute names from message */
      for (i = 0;; i++) {
         el_enum_attr(message, i, attr_list[i], attrib[i]);
         if (!attr_list[i][0])
            break;
      }
      n_attr = i;

   } else {
      if (attrib)
         for (i = 0; i < n_attr; i++) {
            sprintf(str, "%s: ", attr_list[i]);
            el_decode(message, str, attrib[i]);
         }
   }

   el_decode(message, "Attachment: ", attachment_all);
   if (encoding)
      el_decode(message, "Encoding: ", encoding);

   if (attachment) {
      /* break apart attachements */
      for (i = 0; i < MAX_ATTACHMENTS; i++)
         if (attachment[i] != NULL)
            attachment[i][0] = 0;

      for (i = 0; i < MAX_ATTACHMENTS; i++) {
         if (attachment[i] != NULL) {
            if (i == 0)
               p = strtok(attachment_all, ",");
            else
               p = strtok(NULL, ",");

            if (p != NULL)
               strcpy(attachment[i], p);
            else
               break;
         }
      }
   }

   if (locked_by)
      el_decode(message, "Locked by: ", locked_by);

   p = strstr(message, "========================================\n");

   /* check for \n -> \r conversion (e.g. zipping/unzipping) */
   if (p == NULL)
      p = strstr(message, "========================================\r");

   if (text) {
      if (p != NULL) {
         p += 41;
         if ((int) strlen(p) >= *textsize) {
            strlcpy(text, p, *textsize);
            show_error("Entry too long to display. Please increase TEXT_SIZE and recompile elogd.");
            xfree(message);
            return EL_FILE_ERROR;
         } else {
            strlcpy(text, p, *textsize);

            /* strip CR at end */
            if (text[strlen(text) - 1] == '\n') {
               text[strlen(text) - 1] = 0;
               if (text[strlen(text) - 1] == '\r')
                  text[strlen(text) - 1] = 0;
            }

            *textsize = strlen(text);
         }
      } else {
         text[0] = 0;
         *textsize = 0;
      }
   }

   xfree(message);
   return EL_SUCCESS;
}

/*------------------------------------------------------------------*/

int el_submit_attachment(LOGBOOK * lbs, const char *afilename, const char *buffer, int buffer_size,
                         char *full_name)
{
   char file_name[MAX_PATH_LENGTH], ext_file_name[MAX_PATH_LENGTH + 100], str[MAX_PATH_LENGTH], *p;
   int fh;
   time_t now;
   struct tm tms;

   /* strip directory, add date and time to filename */
   strlcpy(str, afilename, sizeof(str));
   p = str;
   while (strchr(p, ':'))
      p = strchr(p, ':') + 1;
   while (strchr(p, '\\'))
      p = strchr(p, '\\') + 1;  /* NT */
   while (strchr(p, '/'))
      p = strchr(p, '/') + 1;   /* Unix */
   strlcpy(file_name, p, sizeof(file_name));

   /* assemble ELog filename */
   if (file_name[0]) {

      if (file_name[6] == '_' && file_name[13] == '_' && isdigit(file_name[0]) && isdigit(file_name[1]))
         strlcpy(ext_file_name, file_name, sizeof(ext_file_name));
      else {
         time(&now);
         memcpy(&tms, localtime(&now), sizeof(struct tm));

         sprintf(ext_file_name, "%02d%02d%02d_%02d%02d%02d_%s", tms.tm_year % 100, tms.tm_mon + 1,
                 tms.tm_mday, tms.tm_hour, tms.tm_min, tms.tm_sec, file_name);
      }

      if (full_name)
         strlcpy(full_name, ext_file_name, MAX_PATH_LENGTH);

      strlcpy(str, lbs->data_dir, sizeof(str));
      strlcat(str, ext_file_name, sizeof(str));

      /* save attachment */
      fh = open(str, O_CREAT | O_RDWR | O_BINARY, 0644);
      if (fh < 0) {
         strencode2(file_name, str, sizeof(file_name));
         sprintf(str, "Cannot write attachment file \"%s\"", file_name);
         show_error(str);
         return -1;
      } else {
         write(fh, buffer, buffer_size);
         close(fh);
      }
   }

   return 0;
}

/*------------------------------------------------------------------*/

void el_delete_attachment(LOGBOOK * lbs, char *file_name)
{
   int i;
   char str[MAX_PATH_LENGTH];

   strlcpy(str, lbs->data_dir, sizeof(str));
   strlcat(str, file_name, sizeof(str));
   remove(str);
   strlcat(str, ".png", sizeof(str));
   remove(str);
   for (i = 0;; i++) {
      strlcpy(str, lbs->data_dir, sizeof(str));
      strlcat(str, file_name, sizeof(str));
      sprintf(str + strlen(str), "-%d.png", i);
      if (file_exist(str)) {
         remove(str);
         continue;
      }

      strlcpy(str, lbs->data_dir, sizeof(str));
      strlcat(str, file_name, sizeof(str));
      if (strrchr(str, '.'))
         *strrchr(str, '.') = 0;
      sprintf(str + strlen(str), "-%d.png", i);
      if (file_exist(str)) {
         remove(str);
         continue;
      }
      break;
   }
}

/*------------------------------------------------------------------*/

int el_retrieve_attachment(LOGBOOK * lbs, int message_id, int n, char name[MAX_PATH_LENGTH])
{
   int i, index, size, fh;
   char file_name[256], *p;
   char message[TEXT_SIZE + 1000], attachment_all[64 * MAX_ATTACHMENTS];

   if (message_id == 0)
      return EL_EMPTY;

   for (index = 0; index < *lbs->n_el_index; index++)
      if (lbs->el_index[index].message_id == message_id)
         break;

   if (index == *lbs->n_el_index)
      return EL_NO_MSG;

   sprintf(file_name, "%s%s", lbs->data_dir, lbs->el_index[index].file_name);
   fh = open(file_name, O_RDONLY | O_BINARY, 0644);
   if (fh < 0) {
      /* file might have been deleted, rebuild index */
      el_build_index(lbs, TRUE);
      return el_retrieve_attachment(lbs, message_id, n, name);
   }

   lseek(fh, lbs->el_index[index].offset, SEEK_SET);
   i = my_read(fh, message, sizeof(message) - 1);
   if (i <= 0) {
      close(fh);
      return EL_FILE_ERROR;
   }

   message[i] = 0;
   close(fh);

   if (strncmp(message, "$@MID@$:", 8) != 0) {
      /* file might have been edited, rebuild index */
      el_build_index(lbs, TRUE);
      return el_retrieve_attachment(lbs, message_id, n, name);
   }

   /* check for correct ID */
   if (atoi(message + 8) != message_id)
      return EL_FILE_ERROR;

   /* decode message size */
   p = strstr(message + 8, "$@MID@$:");
   if (p == NULL)
      size = strlen(message);
   else
      size = p - message;

   message[size] = 0;

   el_decode(message, "Attachment: ", attachment_all);

   name[0] = 0;

   for (i = 0; i <= n; i++) {
      if (i == 0)
         p = strtok(attachment_all, ",");
      else
         p = strtok(NULL, ",");

      if (p == NULL)
         break;
   }

   if (p)
      strlcpy(name, p, MAX_PATH_LENGTH);

   return EL_SUCCESS;
}

/*------------------------------------------------------------------*/

int el_submit(LOGBOOK * lbs, int message_id, BOOL bedit, char *date, char attr_name[MAX_N_ATTR][NAME_LENGTH],
              char attr_value[MAX_N_ATTR][NAME_LENGTH], int n_attr, char *text, char *in_reply_to,
              char *reply_to, char *encoding, char afilename[MAX_ATTACHMENTS][256], BOOL mark_original,
              char *locked_by)
/********************************************************************
 Routine: el_submit

 Purpose: Submit an ELog entry

 Input:
 LOGBOOK lbs             Logbook structure
 int    message_id       Message id
 BOOL   bedit            TRUE for existing message, FALSE for new message
 char   *date            Message date
 char   attr_name[][]    Name of attributes
 char   attr_value[][]   Value of attributes
 int    n_attr           Number of attributes

 char   *text            Message text
 char   *in_reply_to     In reply to this message
 char   *reply_to        Replie(s) to this message
 char   *encoding        Text encoding, either HTML or plain

 char   *afilename[]     File name of attachments
 char   *tag             If given, edit existing message
 int    *tag_size        Maximum size of tag
 BOOL   mark_original    Tag original message for replies
 char   *locked_by       User/Host which locked message for edit

 Function value:
 int                     New message ID

 \********************************************************************/
{
   int n, i, j, size, fh, index, tail_size, orig_size, delta, reply_id;
   char file_name[256], dir[256], str[NAME_LENGTH], date1[256], attrib[MAX_N_ATTR][NAME_LENGTH],
       reply_to1[MAX_REPLY_TO * 10], in_reply_to1[MAX_REPLY_TO * 10], encoding1[80], *message, *p,
       *old_text, *buffer;
   char attachment_all[64 * MAX_ATTACHMENTS];
   time_t ltime;

   tail_size = orig_size = 0;

   buffer = NULL;
   message = xmalloc(TEXT_SIZE + 100);
   old_text = NULL;
   memcpy(attrib, attr_value, sizeof(attrib));
   strlcpy(reply_to1, reply_to, sizeof(reply_to1));
   strlcpy(in_reply_to1, in_reply_to, sizeof(in_reply_to1));
   strlcpy(encoding1, encoding, sizeof(encoding1));
   strlcpy(date1, date, sizeof(date1));

   /* generate new file name YYMMDD.log in data directory */
   strcpy(dir, lbs->data_dir);

   if (bedit) {
      /* edit existing message */
      for (index = 0; index < *lbs->n_el_index; index++)
         if (lbs->el_index[index].message_id == message_id)
            break;

      if (index == *lbs->n_el_index) {
         xfree(message);
         return -1;
      }

      sprintf(file_name, "%s%s", lbs->data_dir, lbs->el_index[index].file_name);
      fh = open(file_name, O_CREAT | O_RDWR | O_BINARY, 0644);
      if (fh < 0) {
         xfree(message);
         return -1;
      }

      lseek(fh, lbs->el_index[index].offset, SEEK_SET);
      i = my_read(fh, message, TEXT_SIZE + 100 - 1);
      if (i >= 0)
         message[i] = 0;
      else
         message[0] = 0;

      /* check for valid message */
      if (strncmp(message, "$@MID@$:", 8) != 0) {
         close(fh);
         xfree(message);

         /* file might have been edited, rebuild index */
         el_build_index(lbs, TRUE);
         return el_submit(lbs, message_id, bedit, date, attr_name, attrib, n_attr, text, in_reply_to,
                          reply_to, encoding, afilename, mark_original, locked_by);
      }

      /* check for correct ID */
      if (atoi(message + 8) != message_id) {
         close(fh);
         xfree(message);
         return -1;
      }

      /* decode message size */
      p = strstr(message + 8, "$@MID@$:");
      if (p == NULL)
         size = strlen(message);
      else
         size = p - message;

      message[size] = 0;

      if (strcmp(text, "<keep>") == 0) {

         p = strstr(message, "========================================\n");
         /* check for \n -> \r conversion (e.g. zipping/unzipping) */
         if (p == NULL)
            p = strstr(message, "========================================\r");
         if (p) {
            p += 41;
            old_text = xmalloc(size + 1);
            strlcpy(old_text, p, size);
            if (old_text[strlen(old_text)-1] == '\n' ||
                old_text[strlen(old_text)-1] == '\r')
               old_text[strlen(old_text)-1] = 0;
         }
      }

      if (strieq(date1, "<keep>"))
         el_decode(message, "Date: ", date1);
      else
         strlcpy(date1, date, sizeof(date1));
      if (strieq(reply_to1, "<keep>"))
         el_decode(message, "Reply to: ", reply_to1);
      if (strieq(in_reply_to1, "<keep>"))
         el_decode(message, "In reply to: ", in_reply_to1);
      if (strieq(encoding1, "<keep>"))
         el_decode(message, "Encoding: ", encoding1);
      el_decode(message, "Attachment: ", attachment_all);

      for (i = 0; i < n_attr; i++) {
         sprintf(str, "%s: ", attr_name[i]);
         if (strieq(attrib[i], "<keep>"))
            el_decode(message, str, attrib[i]);
      }

      /* buffer tail of logfile */
      lseek(fh, 0, SEEK_END);
      orig_size = size;
      tail_size = TELL(fh) - (lbs->el_index[index].offset + size);

      if (tail_size > 0) {
         buffer = xmalloc(tail_size);

         lseek(fh, lbs->el_index[index].offset + size, SEEK_SET);
         n = my_read(fh, buffer, tail_size);
      }
      lseek(fh, lbs->el_index[index].offset, SEEK_SET);
   } else {
      /* create new message */
      if (!date[0])
         get_rfc2822_date(date1, sizeof(date1), 0);
      else
         strlcpy(date1, date, sizeof(date1));

      for (i = 0; i < 12; i++)
         if (strncmp(date1 + 8, mname[i], 3) == 0)
            break;

      ltime = date_to_ltime(date1);

      sprintf(file_name, "%c%c%02d%c%ca.log", date1[14], date1[15], i + 1, date1[5], date1[6]);

      sprintf(str, "%s%s", dir, file_name);
      fh = open(str, O_CREAT | O_RDWR | O_BINARY, 0644);
      if (fh < 0) {
         xfree(message);
         if (old_text)
            xfree(old_text);
         return -1;
      }

      lseek(fh, 0, SEEK_END);

      /* new message id is old plus one */
      if (message_id == 0) {
         message_id = 1;
         for (i = 0; i < *lbs->n_el_index; i++)
            if (lbs->el_index[i].message_id >= message_id)
               message_id = lbs->el_index[i].message_id + 1;
      }

      /* enter message in index */
      index = *lbs->n_el_index;

      (*lbs->n_el_index)++;
      lbs->el_index = xrealloc(lbs->el_index, sizeof(EL_INDEX) * (*lbs->n_el_index));
      lbs->el_index[index].message_id = message_id;
      strcpy(lbs->el_index[index].file_name, file_name);
      lbs->el_index[index].file_time = ltime;
      lbs->el_index[index].offset = TELL(fh);
      lbs->el_index[index].in_reply_to = atoi(in_reply_to1);

      /* if index not ordered, sort it */
      i = *lbs->n_el_index;
      if (i > 1 && lbs->el_index[i - 1].file_time < lbs->el_index[i - 2].file_time) {
         qsort(lbs->el_index, i, sizeof(EL_INDEX), eli_compare);

         /* search message again, index could have been changed by sorting */
         for (index = 0; index < *lbs->n_el_index; index++)
            if (lbs->el_index[index].message_id == message_id)
               break;
      }

      /* if other logbook has same index, update pointers */
      for (i = 0; lb_list[i].name[0]; i++)
         if (&lb_list[i] != lbs && lb_list[i].n_el_index == lbs->n_el_index)
            lb_list[i].el_index = lbs->el_index;
   }

   /* compose message */

   sprintf(message, "$@MID@$: %d\n", message_id);
   sprintf(message + strlen(message), "Date: %s\n", date1);

   if (reply_to1[0])
      sprintf(message + strlen(message), "Reply to: %s\n", reply_to1);

   if (in_reply_to1[0])
      sprintf(message + strlen(message), "In reply to: %s\n", in_reply_to1);

   for (i = 0; i < n_attr; i++)
      sprintf(message + strlen(message), "%s: %s\n", attr_name[i], attrib[i]);

   sprintf(message + strlen(message), "Attachment: ");

   if (afilename) {
      sprintf(message + strlen(message), "%s", afilename[0]);
      for (i = 1; i < MAX_ATTACHMENTS; i++)
         if (afilename[i][0])
            sprintf(message + strlen(message), ",%s", afilename[i]);
   }
   sprintf(message + strlen(message), "\n");

   sprintf(message + strlen(message), "Encoding: %s\n", encoding1);
   if (locked_by && locked_by[0])
      sprintf(message + strlen(message), "Locked by: %s\n", locked_by);

   sprintf(message + strlen(message), "========================================\n");

   if (strieq(text, "<keep>") && old_text)
      strlcat(message, old_text, TEXT_SIZE + 100);
   else
      strlcat(message, text, TEXT_SIZE + 100);
   strlcat(message, "\n", TEXT_SIZE + 100);

   if (old_text)
      xfree(old_text);

   n = write(fh, message, strlen(message));
   if (n != (int) strlen(message)) {
      if (tail_size > 0)
         xfree(buffer);
      close(fh);
      return -1;
   }

   /* update MD5 checksum */
   MD5_checksum(message, strlen(message), lbs->el_index[index].md5_digest);

   if (bedit) {
      if (tail_size > 0) {
         n = write(fh, buffer, tail_size);
         xfree(buffer);

         /* correct offsets for remaining messages in same file */
         delta = strlen(message) - orig_size;

         for (i = 0; i < *lbs->n_el_index; i++)
            if (lbs->el_index[i].message_id == message_id)
               break;

         for (j = i + 1; j < *lbs->n_el_index && strieq(lbs->el_index[i].file_name,
                                                        lbs->el_index[j].file_name); j++)
            lbs->el_index[j].offset += delta;
      }

      /* truncate file here */
      TRUNCATE(fh);
   }

   close(fh);

   /* if reply, mark original message */
   reply_id = atoi(in_reply_to);

   if (mark_original && in_reply_to[0] && !bedit && atoi(in_reply_to) > 0) {
      char date[80], attr[MAX_N_ATTR][NAME_LENGTH], enc[80], att[MAX_ATTACHMENTS][256],
          reply_to[MAX_REPLY_TO * 10], in_reply_to[MAX_REPLY_TO * 10], lock[256];

      /* retrieve original message */
      size = TEXT_SIZE + 100;
      el_retrieve(lbs, reply_id, date, attr_list, attr, n_attr, message, &size, in_reply_to, reply_to, att,
                  enc, lock);

      if (reply_to[0])
         strcat(reply_to, ", ");
      sprintf(reply_to + strlen(reply_to), "%d", message_id);

      /* write modified message */
      el_submit(lbs, reply_id, TRUE, date, attr_list, attr, n_attr, message, in_reply_to, reply_to, enc, att,
                TRUE, lock);
   }

   xfree(message);
   return message_id;
}

/*------------------------------------------------------------------*/

void remove_reference(LOGBOOK * lbs, int message_id, int remove_id, BOOL reply_to_flag)
{
   char date[80], attr[MAX_N_ATTR][NAME_LENGTH], enc[80], in_reply_to[80], reply_to[MAX_REPLY_TO * 10],
       att[MAX_ATTACHMENTS][256], lock[256], *p, *ps, *message;
   int size, status;

   /* retrieve original message */
   size = TEXT_SIZE + 1000;
   message = xmalloc(size);
   status = el_retrieve(lbs, message_id, date, attr_list, attr, lbs->n_attr, message, &size, in_reply_to,
                        reply_to, att, enc, lock);
   if (status != EL_SUCCESS)
      return;

   if (reply_to_flag)
      p = reply_to;
   else
      p = in_reply_to;

   while (*p) {
      while (*p && (*p == ',' || *p == ' '))
         p++;

      ps = p;
      while (isdigit(*ps))
         ps++;

      while (*ps && (*ps == ',' || *ps == ' '))
         ps++;

      if (atoi(p) == remove_id)
         strcpy(p, ps);
      else
         while (isdigit(*p))
            p++;
   }

   /* write modified message */
   el_submit(lbs, message_id, TRUE, date, attr_list, attr, lbs->n_attr, message, in_reply_to, reply_to, enc,
             att, TRUE, lock);

   xfree(message);
}

/*------------------------------------------------------------------*/

int el_delete_message(LOGBOOK * lbs, int message_id, BOOL delete_attachments,
                      char attachment[MAX_ATTACHMENTS][MAX_PATH_LENGTH], BOOL delete_bw_ref,
                      BOOL delete_reply_to)
/********************************************************************
 Routine: el_delete_message

 Purpose: Delete an ELog entry including attachments

 Input:
 LOGBOOK *lbs          Pointer to logbook structure
 int     message_id    Message ID
 BOOL    delete_attachments   Delete attachments if TRUE
 char    attachment    Used to return attachments (on move)
 BOOL    delete_bw_ref If true, delete backward references
 BOOL    delete_reply_to If true, delete replies to this message

 Output:
 <none>

 Function value:
 EL_SUCCESS              Successful completion

 \********************************************************************/
{
   int i, index, n, size, fh, tail_size, old_offset;
   char str[MAX_PATH_LENGTH], file_name[MAX_PATH_LENGTH], reply_to[MAX_REPLY_TO * 10], in_reply_to[256];
   char *buffer, *p;
   char *message, attachment_all[64 * MAX_ATTACHMENTS];

   for (index = 0; index < *lbs->n_el_index; index++)
      if (lbs->el_index[index].message_id == message_id)
         break;

   if (index == *lbs->n_el_index)
      return -1;

   sprintf(file_name, "%s%s", lbs->data_dir, lbs->el_index[index].file_name);
   fh = open(file_name, O_RDWR | O_BINARY, 0644);
   if (fh < 0)
      return EL_FILE_ERROR;

   message = xmalloc(TEXT_SIZE + 1000);

   lseek(fh, lbs->el_index[index].offset, SEEK_SET);
   i = my_read(fh, message, TEXT_SIZE + 1000 - 1);
   if (i <= 0) {
      xfree(message);
      close(fh);
      return EL_FILE_ERROR;
   }

   if (_logging_level > 1) {
      sprintf(str, "DELETE entry #%d", message_id);
      write_logfile(lbs, str);
   }

   message[i] = 0;

   if (strncmp(message, "$@MID@$:", 8) != 0) {
      close(fh);
      xfree(message);

      /* file might have been edited, rebuild index */
      el_build_index(lbs, TRUE);
      return el_delete_message(lbs, message_id, delete_attachments, attachment, delete_bw_ref,
                               delete_reply_to);
   }

   /* check for correct ID */
   if (atoi(message + 8) != message_id) {
      close(fh);
      xfree(message);
      return EL_FILE_ERROR;
   }

   /* decode message size */
   p = strstr(message + 8, "$@MID@$:");
   if (p == NULL)
      size = strlen(message);
   else
      size = p - message;

   message[size] = 0;

   /* delete attachments */
   el_decode(message, "Attachment: ", attachment_all);

   for (i = 0; i < MAX_ATTACHMENTS; i++) {
      if (i == 0)
         p = strtok(attachment_all, ",");
      else
         p = strtok(NULL, ",");

      if (attachment != NULL) {
         if (attachment[i][0] && p) {
            /* delete old attachment if new one exists */
            el_delete_attachment(lbs, p);
         }

         /* return old attachment if no new one */
         if (!attachment[i][0] && p)
            strcpy(attachment[i], p);
      }

      if (delete_attachments && p)
         el_delete_attachment(lbs, p);
   }

   /* decode references */
   el_decode(message, "Reply to: ", reply_to);
   el_decode(message, "In reply to: ", in_reply_to);

   /* buffer tail of logfile */
   lseek(fh, 0, SEEK_END);
   tail_size = TELL(fh) - (lbs->el_index[index].offset + size);

   buffer = NULL;
   if (tail_size > 0) {
      buffer = xmalloc(tail_size);

      lseek(fh, lbs->el_index[index].offset + size, SEEK_SET);
      n = my_read(fh, buffer, tail_size);
   }
   lseek(fh, lbs->el_index[index].offset, SEEK_SET);

   if (tail_size > 0) {
      n = write(fh, buffer, tail_size);
      xfree(buffer);
   }

   /* truncate file here */
   TRUNCATE(fh);

   /* if file length gets zero, delete file */
   tail_size = lseek(fh, 0, SEEK_END);
   close(fh);
   xfree(message);

   if (tail_size == 0)
      remove(file_name);

   /* remove message from index */
   strcpy(str, lbs->el_index[index].file_name);
   old_offset = lbs->el_index[index].offset;
   for (i = index; i < *lbs->n_el_index - 1; i++)
      memcpy(&lbs->el_index[i], &lbs->el_index[i + 1], sizeof(EL_INDEX));

   (*lbs->n_el_index)--;
   if (*lbs->n_el_index > 0)
      lbs->el_index = xrealloc(lbs->el_index, sizeof(EL_INDEX) * (*lbs->n_el_index));

   /* correct all offsets after deleted message */
   for (i = 0; i < *lbs->n_el_index; i++)
      if (strieq(lbs->el_index[i].file_name, str) && lbs->el_index[i].offset > old_offset)
         lbs->el_index[i].offset -= size;

   /* if other logbook has same index, update pointers */
   for (i = 0; lb_list[i].name[0]; i++)
      if (&lb_list[i] != lbs && lb_list[i].n_el_index == lbs->n_el_index)
         lb_list[i].el_index = lbs->el_index;

   /* delete also replies to this message */
   if (delete_reply_to && reply_to[0]) {
      p = reply_to;
      if (isdigit(*p))
         do {
            if (atoi(p))
               el_delete_message(lbs, atoi(p), TRUE, NULL, FALSE, TRUE);

            while (*p && isdigit(*p))
               p++;
            while (*p && (*p == ',' || *p == ' '))
               p++;
         } while (*p);
   }

   /* delete backward references */
   if (in_reply_to[0] && delete_bw_ref) {
      p = in_reply_to;
      do {
         if (atoi(p))
            remove_reference(lbs, atoi(p), message_id, TRUE);

         while (*p && isdigit(*p))
            p++;
         while (*p && (*p == ',' || *p == ' '))
            p++;
      } while (*p);
   }

   /* execute shell if requested */
   if (getcfg(lbs->name, "Execute delete", str, sizeof(str)))
      execute_shell(lbs, message_id, NULL, NULL, str);

   return EL_SUCCESS;
}

/*------------------------------------------------------------------*/

int el_correct_links(LOGBOOK * lbs, int old_id, int new_id)
/* If a message gets resubmitted, the links to that message are wrong.
 This routine corrects that. */
{
   int i, i1, n, n1, size;
   char date[80], *attrib, *text, in_reply_to[80], reply_to[MAX_REPLY_TO * 10], encoding[80], locked_by[256];
   char list[MAX_N_ATTR][NAME_LENGTH], list1[MAX_N_ATTR][NAME_LENGTH];
   char *att_file;

   attrib = xmalloc(MAX_N_ATTR * NAME_LENGTH);
   text = xmalloc(TEXT_SIZE);
   att_file = xmalloc(MAX_ATTACHMENTS * 256);

   el_retrieve(lbs, new_id, date, attr_list, (void *) attrib, lbs->n_attr, NULL, 0, in_reply_to, reply_to,
               (void *) att_file, encoding, locked_by);

   /* go through in_reply_to list */
   n = strbreak(in_reply_to, list, MAX_N_ATTR, ",", FALSE);
   for (i = 0; i < n; i++) {
      size = TEXT_SIZE;
      el_retrieve(lbs, atoi(list[i]), date, attr_list, (void *) attrib, lbs->n_attr, text, &size,
                  in_reply_to, reply_to, (void *) att_file, encoding, locked_by);

      n1 = strbreak(reply_to, list1, MAX_N_ATTR, ",", FALSE);
      reply_to[0] = 0;
      for (i1 = 0; i1 < n1; i1++) {
         /* replace old ID by new ID */
         if (atoi(list1[i1]) == old_id)
            sprintf(reply_to + strlen(reply_to), "%d", new_id);
         else
            strcat(reply_to, list1[i1]);

         if (i1 < n1 - 1)
            strcat(reply_to, ", ");
      }

      el_submit(lbs, atoi(list[i]), TRUE, date, attr_list, (void *) attrib, lbs->n_attr, text, in_reply_to,
                reply_to, encoding, (void *) att_file, TRUE, locked_by);
   }

   el_retrieve(lbs, new_id, date, attr_list, (void *) attrib, lbs->n_attr, NULL, 0, in_reply_to, reply_to,
               (void *) att_file, encoding, locked_by);

   /* go through reply_to list */
   n = strbreak(reply_to, list, MAX_N_ATTR, ",", FALSE);
   for (i = 0; i < n; i++) {
      size = sizeof(text);
      el_retrieve(lbs, atoi(list[i]), date, attr_list, (void *) attrib, lbs->n_attr, text, &size,
                  in_reply_to, reply_to, (void *) att_file, encoding, locked_by);

      n1 = strbreak(in_reply_to, list1, MAX_N_ATTR, ",", FALSE);
      in_reply_to[0] = 0;
      for (i1 = 0; i1 < n1; i1++) {
         /* replace old ID by new ID */
         if (atoi(list1[i1]) == old_id)
            sprintf(in_reply_to + strlen(in_reply_to), "%d", new_id);
         else
            strcat(in_reply_to, list1[i1]);

         if (i1 < n1 - 1)
            strcat(in_reply_to, ", ");
      }

      el_submit(lbs, atoi(list[i]), TRUE, date, attr_list, (void *) attrib, lbs->n_attr, text, in_reply_to,
                reply_to, encoding, (void *) att_file, TRUE, locked_by);
   }

   xfree(text);
   xfree(attrib);
   xfree(att_file);

   return EL_SUCCESS;
}

/*------------------------------------------------------------------*/

int el_move_message_thread(LOGBOOK * lbs, int message_id)
{
   int i, n, size, new_id;
   char date[80], attrib[MAX_N_ATTR][NAME_LENGTH], *text, in_reply_to[80], reply_to[MAX_REPLY_TO * 10],
       encoding[80], locked_by[256];
   char list[MAX_N_ATTR][NAME_LENGTH], str[256];
   char att_file[MAX_ATTACHMENTS][256];

   /* retrieve message */
   text = xmalloc(TEXT_SIZE);
   size = TEXT_SIZE;
   el_retrieve(lbs, message_id, date, attr_list, attrib, lbs->n_attr, text, &size, in_reply_to, reply_to,
               att_file, encoding, locked_by);

   /* submit as new message */
   date[0] = 0;
   new_id = el_submit(lbs, 0, FALSE, date, attr_list, attrib, lbs->n_attr, text, in_reply_to, reply_to,
                      encoding, att_file, FALSE, locked_by);

   xfree(text);

   /* correct links */
   el_correct_links(lbs, message_id, new_id);

   /* delete original message */
   el_delete_message(lbs, message_id, FALSE, NULL, FALSE, FALSE);

   /* move all replies recursively */
   if (getcfg(lbs->name, "Resubmit replies", str, sizeof(str)) && atoi(str) == 1) {
      if (reply_to[0]) {
         n = strbreak(reply_to, list, MAX_N_ATTR, ",", FALSE);
         for (i = 0; i < n; i++)
            el_move_message_thread(lbs, atoi(list[i]));
      }
   }

   return new_id;
}

/*------------------------------------------------------------------*/

int el_move_message(LOGBOOK * lbs, int old_id, int new_id)
{
   int status, size;
   char date[80], attrib[MAX_N_ATTR][NAME_LENGTH], *text, in_reply_to[80], reply_to[MAX_REPLY_TO * 10],
       encoding[80], locked_by[256], att_file[MAX_ATTACHMENTS][256];

   /* retrieve message */
   text = xmalloc(TEXT_SIZE);
   size = TEXT_SIZE;
   status = el_retrieve(lbs, old_id, date, attr_list, attrib, lbs->n_attr, text, &size, in_reply_to,
                        reply_to, att_file, encoding, locked_by);
   if (status != EL_SUCCESS)
      return 0;

   /* submit as new message */
   status = el_submit(lbs, new_id, FALSE, date, attr_list, attrib, lbs->n_attr, text, in_reply_to, reply_to,
                      encoding, att_file, FALSE, locked_by);

   xfree(text);

   if (status != new_id)
      return 0;

   /* correct links */
   el_correct_links(lbs, old_id, new_id);

   /* delete original message */
   el_delete_message(lbs, old_id, FALSE, NULL, FALSE, FALSE);

   return 1;
}

/*------------------------------------------------------------------*/

int el_lock_message(LOGBOOK * lbs, int message_id, char *user)
/* lock message for editing */
{
   int size;
   char date[80], attrib[MAX_N_ATTR][NAME_LENGTH], text[TEXT_SIZE], in_reply_to[80],
       reply_to[MAX_REPLY_TO * 10], encoding[80], locked_by[256];
   char att_file[MAX_ATTACHMENTS][256];

   /* retrieve message */
   size = sizeof(text);
   el_retrieve(lbs, message_id, date, attr_list, attrib, lbs->n_attr, text, &size, in_reply_to, reply_to,
               att_file, encoding, locked_by);

   /* submit message, unlocked if user==NULL */
   el_submit(lbs, message_id, TRUE, date, attr_list, attrib, lbs->n_attr, text, in_reply_to, reply_to,
             encoding, att_file, FALSE, user);

   return EL_SUCCESS;
}

/*------------------------------------------------------------------*/

void write_logfile(LOGBOOK * lbs, const char *text)
{
   char file_name[MAX_PATH_LENGTH];
   char str[MAX_PATH_LENGTH], unm[256];
   int fh;
   time_t now;
   char buf[10000];

   if (lbs == NULL) {
      if (!getcfg("global", "logfile", str, sizeof(str)))
         return;
   } else if (!getcfg(lbs->name, "logfile", str, sizeof(str)))
      return;

   if (str[0] == DIR_SEPARATOR || str[1] == ':')
      strlcpy(file_name, str, sizeof(file_name));
   else {
      strlcpy(file_name, logbook_dir, sizeof(file_name));
      strlcat(file_name, str, sizeof(file_name));
   }

   fh = open(file_name, O_RDWR | O_BINARY | O_CREAT | O_APPEND, 0644);
   if (fh < 0)
      return;

   now = time(0);
   strftime(buf, sizeof(buf), "%d-%b-%Y %H:%M:%S", localtime(&now));
   strcat(buf, " ");

   if (isparam("unm") && rem_host[0]) {
      strlcpy(unm, getparam("unm"), sizeof(unm));
      if (rem_host_ip[0])
         sprintf(buf + strlen(buf), "[%s@%s(%s)] ", unm, rem_host, rem_host_ip);
      else
         sprintf(buf + strlen(buf), "[%s@%s] ", unm, rem_host);
   } else if (rem_host[0]) {
      if (rem_host_ip[0])
         sprintf(buf + strlen(buf), "[%s(%s)] ", rem_host, rem_host_ip);
      else
         sprintf(buf + strlen(buf), "[%s] ", rem_host);
   } else
      sprintf(buf + strlen(buf), "[%s] ", rem_host_ip);

   if (lbs)
      sprintf(buf + strlen(buf), "{%s} ", lbs->name);

   strlcat(buf, text, sizeof(buf) - 1);
#ifdef OS_WINNT
   if (strlen(buf) > 0 && buf[strlen(buf) - 1] != '\n')
      strlcat(buf, "\r\n", sizeof(buf));
   else if (strlen(buf) > 1 && buf[strlen(buf) - 2] != '\r')
      strlcpy(buf + strlen(buf) - 2, "\r\n", sizeof(buf) - (strlen(buf) - 2));

#else
   if (strlen(buf) > 1 && buf[strlen(buf) - 1] != '\n')
      strlcat(buf, "\n", sizeof(buf));
#endif

   write(fh, buf, strlen(buf));

   close(fh);
}

/*------------------------------------------------------------------*/

/*
 void logd(const char *format, ...)
 {
 va_list argptr;
 char str[10000];
 FILE *f;
 time_t now;
 char buf[1000];

 va_start(argptr, format);
 vsprintf(str, (char *) format, argptr);
 va_end(argptr);

 f = fopen("c:\\tmp\\elogd.log", "a");
 if (!f)
 return;

 now = time(0);
 strftime(buf, sizeof(buf), "%d-%b-%Y %H:%M:%S", localtime(&now));
 strcat(buf, " ");

 strlcat(buf, str, sizeof(buf));
 if (buf[strlen(buf) - 1] != '\n')
 strlcat(buf, "\n", sizeof(buf));

 fprintf(f, buf);

 fclose(f);
 }
 */

/*------------------------------------------------------------------*/

char *html_tags[] = { "<A HREF=", "<IMG ", "<B>", "<I>", "<P>", "<HR>", "" };

int is_html(char *s)
{
   char *str, *p;
   int i;

   str = xstrdup(s);

   for (i = 0; i < (int) strlen(s); i++)
      str[i] = toupper(s[i]);
   str[i] = 0;

   for (i = 0; html_tags[i][0]; i++) {
      p = strstr(str, html_tags[i]);
      if (p && strchr(p, '>') && (p == str || (p > str && *(p - 1) != '\\'))) {
         xfree(str);
         return TRUE;
      }
   }

   xfree(str);
   return FALSE;
}

/*------------------------------------------------------------------*/

char *full_html_tags[] = { "<HTML>", "<BODY>", "<HEAD>", "" };

int is_full_html(char *file_name)
{
   char *str, *p;
   int i, fh, length;
   unsigned char *buf;

   fh = open(file_name, O_RDONLY | O_BINARY);
   if (fh < 0)
      return FALSE;
   lseek(fh, 0, SEEK_END);
   length = TELL(fh);
   lseek(fh, 0, SEEK_SET);
   if (length > 1000)
      length = 1000;
   buf = xmalloc(length);
   read(fh, buf, length);
   close(fh);

   str = xstrdup((char *) buf);

   for (i = 0; i < (int) strlen((char *) buf); i++)
      str[i] = toupper(buf[i]);
   str[i] = 0;

   xfree(buf);

   for (i = 0; full_html_tags[i][0]; i++) {
      p = strstr(str, full_html_tags[i]);
      if (p && strchr(p, '>') && (p == str || (p > str && *(p - 1) != '\\'))) {
         xfree(str);
         return TRUE;
      }
   }

   xfree(str);
   return FALSE;
}

/*------------------------------------------------------------------*/

int is_ascii(char *file_name)
{
   int i, fh, length;
   unsigned char *buf;

   fh = open(file_name, O_RDONLY | O_BINARY);
   if (fh < 0)
      return FALSE;
   lseek(fh, 0, SEEK_END);
   length = TELL(fh);
   lseek(fh, 0, SEEK_SET);
   if (length > 1000)
      length = 1000;
   buf = xmalloc(length);
   read(fh, buf, length);
   close(fh);

   for (i = 0; i < length; i++) {
      if (buf[i] < 32 && buf[i] != '\r' && buf[i] != '\n' && buf[i] != '\t') {
         xfree(buf);
         return FALSE;
      }
   }

   xfree(buf);
   return TRUE;
}

/*------------------------------------------------------------------*/

int is_image(char *att)
{
   return (stristr(att, ".GIF") != NULL) || (stristr(att, ".JPG") != NULL) || (stristr(att, ".JPEG") != NULL)
       || (stristr(att, ".PNG") != NULL);
}

/*------------------------------------------------------------------*/

void strip_html(char *s)
{
   char *p;

   while ((p = strchr(s, '<')) != NULL) {
      if (strchr(p, '>'))
         strcpy(p, strchr(p, '>') + 1);
      else
         *p = 0;
   }
}

/*------------------------------------------------------------------*/

int line_break(char *str, char *encoding)
{
   if (strieq(encoding, "plain") || strieq(encoding, "ELCode")) {
      return str[0] == '\n';
   }

   // HTML encoding
   if (strncmp(str, "</p>", 4) == 0 ||
       strncmp(str, "<br>", 4) == 0 ||
       strncmp(str, "<br />", 4) == 0)
      return 1;

   return 0;
}

/*------------------------------------------------------------------*/

void insert_breaks(char *str, int n, int size)
{
   int i, j, i_last;

   i_last = 0;
   for (i = 0; i < (int) strlen(str); i++) {
      if (str[i] == '\r')
         i_last = i;

      /* if more than n chars without return, insert one */
      if (i - i_last >= n && (int) strlen(str) + 3 < size) {

         /* find previous blank */
         while (i > i_last && str[i] != ' ')
            i--;
         if (str[i] == ' ')
            i++;

         /* move trailing string one char further */
         for (j = strlen(str) + 2; j > i; j--)
            str[j] = str[j - 2];

         /* set CR */
         str[i++] = '\r';
         str[i++] = '\n';

         i_last = i;
      }
   }
}

/*------------------------------------------------------------------*/

void replace_inline_img(LOGBOOK * lbs, char *str)
{
   char *p, *pn, *pa, old[256], link[256], base_url[256];
   int index;

   p = str;
   do {
      p = strstr(p, "<img ");
      if (p) {
         pn = strstr(p, "name=");
         if (pn) {
            pn += 9;
            index = atoi(pn);
            while (*pn && *pn != '>')
               pn++;
            if (*pn == '>')
               pn++;
            sprintf(p, "<img src=\"cid:att%d@psi.ch\">", index);
            memmove(p + strlen(p), pn, strlen(pn) + 1);

            /* now change href to absolute link */
            pa = p - 1;
            while (pa > str && *pa != '<')
               // search '<a href=...>'
               pa--;
            pn = strstr(pa, "href=");
            if (pn && pn - pa < 10) {
               strlcpy(old, pn + 6, sizeof(old));
               if (strchr(old, '\"'))
                  *strchr(old, '\"') = 0;
               compose_base_url(lbs, base_url, sizeof(base_url), FALSE);
               strlcpy(link, base_url, sizeof(link));
               strlcat(link, old, sizeof(link));
               if (strchr(link, '?'))
                  *strchr(link, '?') = 0;
               strsubst(pn + 6, TEXT_SIZE, old, link);
               if (strlen(link) > strlen(old))
                  p += strlen(link) - strlen(old);
            }

            p++;
         } else
            p++;
      }
   } while (p != NULL);
}

/*------------------------------------------------------------------*/

void convert_elog_link(LOGBOOK * lbs, char *link, char *link_text, char *result, int absolute_link,
                       int message_id)
{
   char str[256], base_url[256];
   int i;

   strlcpy(str, link, sizeof(str));
   if (strchr(str, '/'))
      *strchr(str, '/') = 0;

   for (i = 0; i < (int) strlen(str); i++)
      if (!isdigit(str[i]))
         break;

   if (i < (int) strlen(str)) {
      /* if link contains reference to other logbook, put logbook explicitly */
      if (absolute_link)
         compose_base_url(NULL, base_url, sizeof(base_url), FALSE);
      else
         strcpy(base_url, "../");
      sprintf(result, "<a href=\"%s%s\">elog:%s</a>", base_url, link, link_text);
   } else if (link[0] == '/') {
      if (absolute_link)
         compose_base_url(NULL, base_url, sizeof(base_url), FALSE);
      else
         base_url[0] = 0;
      sprintf(result, "<a href=\"%s%s/%d%s\">elog:%s</a>", base_url, lbs->name_enc, message_id, link,
              link_text);
   } else {
      if (absolute_link)
         compose_base_url(lbs, base_url, sizeof(base_url), FALSE);
      else
         base_url[0] = 0;
      sprintf(result, "<a href=\"%s%s\">elog:%s</a>", base_url, link, link_text);
   }
}

/*------------------------------------------------------------------*/

void rsputs(const char *str)
{
   if (strlen_retbuf + (int) strlen(str) + 1 >= return_buffer_size) {
      return_buffer = xrealloc(return_buffer, return_buffer_size + (int) strlen(str) + 100000);
      memset(return_buffer + return_buffer_size, 0, (int) strlen(str) + 100000);
      return_buffer_size += (int) strlen(str)+100000;
   }

   strcpy(return_buffer + strlen_retbuf, str);
   strlen_retbuf += strlen(str);
}

/*------------------------------------------------------------------*/

char *key_list[] = { "http://", "https://", "ftp://", "mailto:", "elog:", "file://", "" };

void rsputs2(LOGBOOK * lbs, int absolute_link, const char *str)
{
   int i, j, k, l, n;
   char *p, *pd, link[1000], link_text[1000];

   if (strlen_retbuf + (int) (2 * strlen(str) + 1000) >= return_buffer_size) {
      return_buffer = xrealloc(return_buffer, return_buffer_size + 100000);
      memset(return_buffer + return_buffer_size, 0, 100000);
      return_buffer_size += 100000;
   }

   j = strlen_retbuf;
   for (i = 0; i < (int) strlen(str); i++) {
      for (l = 0; key_list[l][0]; l++) {
         if (strncmp(str + i, key_list[l], strlen(key_list[l])) == 0) {

            /* check for escape character */
            if (i > 0 && *(str + i - 1) == '\\') {
               j--;
               *(return_buffer + j) = 0;
               continue;
            }

            p = (char *) (str + i + strlen(key_list[l]));
            i += strlen(key_list[l]);
            for (k = 0; *p && strcspn(p, " \t\n\r({[)}]\"") && k < (int) sizeof(link); k++, i++)
               link[k] = *p++;
            link[k] = 0;
            i--;

            /* link may not end with a '.'/',' (like in a sentence) */
            if (link[k - 1] == '.' || link[k - 1] == ',') {
               link[k - 1] = 0;
               k--;
               i--;
            }

            /* check if link contains coloring */
            p = strchr(link, '\001');
            if (p != NULL) {
               strlcpy(link_text, link, sizeof(link_text));

               /* skip everything between '<' and '>' */
               pd = p;
               while (*pd && *pd != '\002')
                  *p = *pd++;

               strcpy(p, pd + 1);

               /* skip '</B>' */
               p = strchr(link, '\001');
               if (p != NULL) {
                  pd = p;

                  while (*pd && *pd != '\002')
                     *p = *pd++;

                  strcpy(p, pd + 1);
               }

               /* correct link text */
               for (n = 0; n < (int) strlen(link_text); n++) {
                  switch (link_text[n]) {
                     /* the translation for the search highliting */
                  case '\001':
                     link_text[n] = '<';
                     break;
                  case '\002':
                     link_text[n] = '>';
                     break;
                  case '\003':
                     link_text[n] = '\"';
                     break;
                  case '\004':
                     link_text[n] = ' ';
                     break;
                  }
               }

            } else
               strlcpy(link_text, link, sizeof(link_text));

            if (strcmp(key_list[l], "elog:") == 0) {
               convert_elog_link(lbs, link, link_text, return_buffer + j, absolute_link, _current_message_id);
            } else if (strcmp(key_list[l], "mailto:") == 0) {
               sprintf(return_buffer + j, "<a href=\"mailto:%s\">%s</a>", link, link_text);
            } else {
               sprintf(return_buffer + j, "<a href=\"%s", key_list[l]);
               j += strlen(return_buffer + j);
               strlen_retbuf = j;

               /* link can contain special characters */
               rsputs2(lbs, absolute_link, link);
               j = strlen_retbuf;

               sprintf(return_buffer + j, "\">%s", key_list[l]);
               j += strlen(return_buffer + j);
               strlen_retbuf = j;

               /* link_text can contain special characters */
               rsputs2(lbs, absolute_link, link_text);
               j = strlen_retbuf;
               sprintf(return_buffer + j, "</a>");
            }

            j += strlen(return_buffer + j);
            break;
         }
      }

      if (!key_list[l][0]) {
         if (strncmp(str + i, "<br>", 4) == 0) {
            strcpy(return_buffer + j, "<br>");
            j += 4;
            i += 3;
         } else
            switch (str[i]) {
            case '&':
               strcat(return_buffer, "&amp;");
               j += 5;
               break;
            case '<':
               strcat(return_buffer, "&lt;");
               j += 4;
               break;
            case '>':
               strcat(return_buffer, "&gt;");
               j += 4;
               break;

               /* suppress escape character '\' in front of HTML or ELCode tag */
            case '\\':
               if (str[i + 1] != '<' && str[i + 1] != '[')
                  return_buffer[j++] = str[i];
               break;

               /* the translation for the search highliting */
            case '\001':
               strcat(return_buffer, "<");
               j++;
               break;
            case '\002':
               strcat(return_buffer, ">");
               j++;
               break;
            case '\003':
               strcat(return_buffer, "\"");
               j++;
               break;
            case '\004':
               strcat(return_buffer, " ");
               j++;
               break;

            default:
               return_buffer[j++] = str[i];
            }
      }
   }

   return_buffer[j] = 0;
   strlen_retbuf = j;
}

/*------------------------------------------------------------------*/

void rsputs3(const char *text)
{
   int i;
   char str[2];

   str[1] = 0;
   for (i = 0; i < (int) strlen(text); i++) {
      switch (text[i]) {
      case '<':
         rsputs("&lt;");
         break;
      case '>':
         rsputs("&gt;");
         break;
      case '&':
         rsputs("&amp;");
         break;
      case '\"':
         rsputs("&quot;");
         break;

      default:
         str[0] = text[i];
         rsputs(str);
      }
   }
}

/*------------------------------------------------------------------*/

typedef struct {
   char *pattern;
   char *subst;
} PATTERN_LIST;

PATTERN_LIST pattern_list[] = {

   /* smileys */
   {
    ":))", "<img alt=\"Happy\" title=\"Happy\" src=\"%sicons/happy.png\">"}, {
                                                                              ":-))",
                                                                              "<img alt=\"Happy\" title=\"Happy\" src=\"%sicons/happy.png\">"},
   {
    ":)", "<img alt=\"Smile\" title=\"Smile\" src=\"%sicons/smile.png\">"}, {
                                                                             ":-)",
                                                                             "<img alt=\"Smile\" title=\"Smile\" src=\"%sicons/smile.png\">"},
   {
    ":(", "<img alt=\"Frown\" title=\"Frown\" src=\"%sicons/frown.png\">"}, {
                                                                             ":-(",
                                                                             "<img alt=\"Frown\" title=\"Frown\" src=\"%sicons/frown.png\">"},
   {
    ";)", "<img alt=\"Wink\" title=\"Wink\" src=\"%sicons/wink.png\">"}, {
                                                                          ";-)",
                                                                          "<img alt=\"Wink\" title=\"Wink\" src=\"%sicons/wink.png\">"},
   {
    ":d", "<img alt=\"Big grin\" title=\"Big grin\" src=\"%sicons/biggrin.png\">"}, {
                                                                                     "?-)",
                                                                                     "<img alt=\"Confused\" title=\"Confused\" src=\"%sicons/confused.png\">"},
   {
    ";(", "<img alt=\"Crying\" title=\"Crying\" src=\"%sicons/crying.png\">"}, {
                                                                                ";-(",
                                                                                "<img alt=\"Crying\" title=\"Crying\" src=\"%sicons/crying.png\">"},
   {
    ":]", "<img alt=\"Pleased\" title=\"Pleased\" src=\"%sicons/pleased.png\">"}, {
                                                                                   ":-]",
                                                                                   "<img alt=\"Pleased\" title=\"Pleased\" src=\"%sicons/pleased.png\">"},
   {
    ":o", "<img alt=\"Yawn\" title=\"Yawn\" src=\"%sicons/yawn.png\">"}, {
                                                                          ":-o",
                                                                          "<img alt=\"Yawn\" title=\"Yawn\" src=\"%sicons/yawn.png\">"},
   {
    "8-)", "<img alt=\"Cool\" title=\"Cool\" src=\"%sicons/cool.png\">"}, {
                                                                           "8o",
                                                                           "<img alt=\"Astonished\" title=\"Astonished\" src=\"%sicons/astonished.png\">"},
   {
    "x-(", "<img alt=\"Mad\" title=\"Mad\" src=\"%sicons/mad.png\">"}, {
                                                                        ":p",
                                                                        "<img alt=\"Tongue\" title=\"Tongue\" src=\"%sicons/tongue.png\">"},
   {
    ":-p", "<img alt=\"Tongue\" title=\"Tongue\" src=\"%sicons/tongue.png\">"},
   /* formatting */
   {
    "[b]", "<b>"}, {
                    "[/b]", "</b>"}, {
                                      "[u]", "<u>"}, {
                                                      "[/u]", "</u>"}, {
                                                                        "[i]", "<i>"}, {
                                                                                        "[/i]", "</i>"}, {
                                                                                                          "[center]",
                                                                                                          "<center>"},
   {
    "[/center]", "</center>"}, {
                                "[color=", "<font color=\"%s\">"}, {
                                                                    "[/color]", "</font>"}, {
                                                                                             "[size=",
                                                                                             "<font size=\"%s\">"},
   {
    "[/size]", "</font>"}, {
                            "[font=", "<font face=\"%s\">"}, {
                                                              "[/font]", "</font>"}, {
                                                                                      "\r\n[code]", "<pre>"}, {
                                                                                                               "[code]",
                                                                                                               "<pre>"},
   {
    "[/code]\r\n", "</pre>"}, {
                               "[/code]", "</pre>"}, {
                                                      "\r\n[code1]", "<pre>"}, {
                                                                                "[code1]", "<pre>"}, {
                                                                                                      "[/code1]\r\n",
                                                                                                      "</pre>"},
   {
    "[/code1]", "</pre>"},
   /* lists */
   {
    "[list]\r", "<ul>"}, {
                          "[list]", "<ul>"}, {
                                              "[*]", "<li>"}, {
                                                               "[/list]\r", "</#>"},    // either </ul> or </ol>
   {
    "[/list]", "</#>"}, {
                         "[list=", "<ol type=\"%s\">"},
   /* headings */
   {
    "[h1]", "<h1>"}, {
                      "[/h1]", "</h1>"}, {
                                          "[h2]", "<h2>"}, {
                                                            "[/h2]", "</h2>"}, {
                                                                                "[h3]", "<h3>"}, {
                                                                                                  "[/h3]",
                                                                                                  "</h3>"},
   /* URLs */
   {
    "[url=", "<a href=\"%#\">%s</a>"}, {
                                        "[url]", "<a href=\"%#\">%s</a>"}, {
                                                                            "[/url]", ""}, {
                                                                                            "[email]",
                                                                                            "<a href=\"mailto:%#\">%s</a>"},
   {
    "[/email]", ""}, {
                      "[img]", "<a href=\"%#\"><img border=0 src=\"%#?thumb=1\"></a>"}, {
                                                                                         "[/img]", ""},
   /* quote */
   {
    "[quote=",
    "<br /><table class=\"quotetable\" align=\"center\" cellspacing=\"1\"><tr><td class=\"quotetitle\">%s:</td></tr><tr><td class=\"quote\">"},
   {
    "[quote]",
    "<br /><table class=\"quotetable\" align=\"center\" cellspacing=\"1\"><tr><td class=\"quotetitle\">%s:</td></tr><tr><td class=\"quote\">"},
   {
    "[/quote]\r", "</td></tr></table><br />\r\n"}, {
                                                    "[/quote]", "</td></tr></table>\r\n"},
   /* table */
   {
    "[table]", "<table><tr><td>"}, {
                                    "[table ", "<table %s><tr><td>"}, {
                                                                       "|-", "</td></tr><tr><td>"}, {
                                                                                                     "|",
                                                                                                     "</td><td>"},
   {
    "[/table]", "</td></tr></table>"},
   /* horizontal line */
   {
    "[line]", "<hr />"},
   /* anchor */
   {
    "[anchor]", "<a name=\"%#\">"}, {
                                     "[/anchor]", "</a>"}, {
                                                            "", ""}
};

char
*email_quote_table =
    "<table width=100%% border=1 align=\"left\" cellpadding=\"3\"><tr><td bgcolor=#486090><font color=white>%s:</font></td></tr><tr><td bgcolor=#FFFFB0>";

void rsputs_elcode(LOGBOOK * lbs, BOOL email_notify, const char *str)
{
   int i, j, k, l, m, elcode_disabled, elcode_disabled1, escape_char, ordered_list, substituted, inside_table;
   char *p, *pd, link[1000], link_text[1000], tmp[1000], attrib[1000], hattrib[1000], value[1000],
       subst[1000], base_url[256], param[256], *lstr;

   if (strlen_retbuf + (int) (2 * strlen(str) + 1000) >= return_buffer_size) {
      return_buffer = xrealloc(return_buffer, return_buffer_size + 100000);
      memset(return_buffer + return_buffer_size, 0, 100000);
      return_buffer_size += 100000;
   }

   elcode_disabled = FALSE;
   elcode_disabled1 = FALSE;
   ordered_list = FALSE;
   escape_char = FALSE;
   inside_table = 0;
   j = strlen_retbuf;
   m = 0;

   /* make lower case copy of str */
   lstr = xmalloc(strlen(str) + 1);
   for (pd = lstr, p = (char *) str; *p; p++, pd++)
      *pd = tolower(*p);
   *pd = 0;

   for (i = 0; i < (int) strlen(str); i++) {

      for (l = 0; key_list[l][0]; l++) {
         if (strncmp(lstr + i, key_list[l], strlen(key_list[l])) == 0) {

            /* check for escape character */
            if (i > 0 && *(str + i - 1) == '\\') {
               j--;
               *(return_buffer + j) = 0;
               continue;
            }

            p = (char *) (str + i + strlen(key_list[l]));
            i += strlen(key_list[l]);
            for (k = 0; *p && strcspn(p, " \t\n\r({[)}]\"") && k < (int) sizeof(link); k++, i++)
               link[k] = *p++;
            link[k] = 0;
            i--;

            /* link may not end with a '.'/',' (like in a sentence) */
            if (link[k - 1] == '.' || link[k - 1] == ',') {
               link[k - 1] = 0;
               k--;
               i--;
            }

            strlcpy(link_text, link, sizeof(link_text));

            /* check if link contains coloring */
            while ((p = strchr(link, '\001')) != NULL) {

               /* skip everything between '<' and '>' */
               pd = p;
               while (*pd && *pd != '\002')
                  *p = *pd++;

               strcpy(p, pd + 1);

               /* skip '</B>' */
               p = strchr(link, '\001');
               if (p != NULL) {
                  pd = p;

                  while (*pd && *pd != '\002')
                     *p = *pd++;

                  strcpy(p, pd + 1);
               }
            }

            if (strcmp(key_list[l], "elog:") == 0) {
               strlcpy(tmp, link, sizeof(tmp));
               if (strchr(tmp, '/'))
                  *strchr(tmp, '/') = 0;

               for (m = 0; m < (int) strlen(tmp); m++)
                  if (!isdigit(tmp[m]))
                     break;

               if (m < (int) strlen(tmp) && tmp[m] != '#') {
                  /* if link contains reference to other logbook, put logbook explicitly */
                  if (email_notify)
                     compose_base_url(NULL, base_url, sizeof(base_url), TRUE);
                  else
                     strcpy(base_url, "../");
                  sprintf(return_buffer + j, "<a href=\"%s%s\">elog:%s</a>", base_url, link, link_text);
               } else if (link[0] == '/') {
                  if (email_notify)
                     compose_base_url(NULL, base_url, sizeof(base_url), TRUE);
                  else
                     base_url[0] = 0;
                  sprintf(return_buffer + j, "<a href=\"%s%s/%d%s\">elog:%s</a>", base_url, lbs->name_enc,
                          _current_message_id, link, link_text);
               } else {
                  if (email_notify)
                     compose_base_url(lbs, base_url, sizeof(base_url), TRUE);
                  else
                     base_url[0] = 0;
                  sprintf(return_buffer + j, "<a href=\"%s%s\">elog:%s</a>", base_url, link, link_text);
               }
            } else if (strcmp(key_list[l], "mailto:") == 0) {
               sprintf(return_buffer + j, "<a href=\"mailto:%s\">%s</a>", link, link_text);
            } else {
               sprintf(return_buffer + j, "<a href=\"%s", key_list[l]);
               j += strlen(return_buffer + j);
               strlen_retbuf = j;

               /* link can contain special characters */
               rsputs2(lbs, email_notify, link);
               j = strlen_retbuf;

               sprintf(return_buffer + j, "\">%s", key_list[l]);
               j += strlen(return_buffer + j);
               strlen_retbuf = j;

               /* link_text can contain special characters */
               rsputs2(lbs, email_notify, link_text);
               j = strlen_retbuf;
               sprintf(return_buffer + j, "</a>");
            }

            j += strlen(return_buffer + j);
            break;
         }
      }
      if (key_list[l][0])
         continue;

      substituted = FALSE;

      for (l = 0; pattern_list[l].pattern[0]; l++) {
         if (strncmp(lstr + i, pattern_list[l].pattern, strlen(pattern_list[l].pattern)) == 0) {

            if (stristr(pattern_list[l].pattern, "[/code]"))
               elcode_disabled = FALSE;
            if (stristr(pattern_list[l].pattern, "[/code1]"))
               elcode_disabled1 = FALSE;

            /* check for escape character */
            if (i > 0 && str[i - 1] == '\\') {

               j--;
               strcpy(return_buffer + j, pattern_list[l].pattern);
               j += strlen(pattern_list[l].pattern);
               i += strlen(pattern_list[l].pattern) - 1;        // 1 gets added in for loop...
               substituted = TRUE;

               break;
            }

            if (!elcode_disabled && !elcode_disabled1) {

               substituted = TRUE;

               if (stristr(pattern_list[l].pattern, "[list="))
                  ordered_list = TRUE;

               if (stristr(pattern_list[l].pattern, "[table"))
                  inside_table++;
               if (stristr(pattern_list[l].pattern, "[/table]"))
                  inside_table--;

               if (stristr(pattern_list[l].pattern, "[quote")) {
                  if (pattern_list[l].pattern[strlen(pattern_list[l].pattern) - 1] == '=') {
                     i += strlen(pattern_list[l].pattern);
                     strextract(str + i, ']', attrib, sizeof(attrib));
                     i += strlen(attrib);

                     if (attrib[0] == '\"')
                        strcpy(attrib, attrib + 1);
                     if (attrib[strlen(attrib) - 1] == '\"')
                        attrib[strlen(attrib) - 1] = 0;

                     sprintf(value, loc("%s wrote"), attrib);
                     if (email_notify)
                        sprintf(return_buffer + j, email_quote_table, value);
                     else
                        sprintf(return_buffer + j, pattern_list[l].subst, value);
                     j += strlen(return_buffer + j);
                  } else {
                     if (email_notify)
                        sprintf(return_buffer + j, email_quote_table, loc("Quote"));
                     else
                        sprintf(return_buffer + j, pattern_list[l].subst, loc("Quote"));
                     j += strlen(return_buffer + j);
                     i += strlen(pattern_list[l].pattern) - 1;  // 1 gets added in for loop...
                  }
               }

               else if (strstr(pattern_list[l].subst, "%#")) {

                  /* special substitutions */
                  if (pattern_list[l].pattern[strlen(pattern_list[l].pattern) - 1] == '=') {

                     i += strlen(pattern_list[l].pattern);
                     strextract(str + i, ']', attrib, sizeof(attrib));
                     i += strlen(attrib) + 1;

                     if (strncmp(attrib, "elog:", 5) == 0) {    /* eval elog: */
                        strlcpy(tmp, attrib + 5, sizeof(tmp));
                        if (strchr(tmp, '/'))
                           *strchr(tmp, '/') = 0;

                        for (m = 0; m < (int) strlen(tmp); m++)
                           if (!isdigit(tmp[m]))
                              break;

                        if (m < (int) strlen(tmp))
                           /* if link contains reference to other logbook, add ".." in front */
                           sprintf(hattrib, "../%s", attrib + 5);
                        else if (attrib[5] == '/')
                           sprintf(hattrib, "%d%s", _current_message_id, attrib + 5);
                        else
                           sprintf(hattrib, "%s", attrib + 5);

                     } else if (strstr(attrib, "://") == 0 && attrib[0] != '#') {       /* add http:// if missing */
                        if (_ssl_flag)
                           sprintf(hattrib, "https://%s", attrib);
                        else
                           sprintf(hattrib, "http://%s", attrib);
                     } else
                        strlcpy(hattrib, attrib, sizeof(hattrib));

                     strextract(str + i, '[', value, sizeof(value));
                     i += strlen(value) - 1;
                     strlcpy(subst, pattern_list[l].subst, sizeof(subst));
                     *strchr(subst, '#') = 's';
                     sprintf(return_buffer + j, subst, hattrib, value);

                     j += strlen(return_buffer + j);

                  } else if (pattern_list[l].pattern[strlen(pattern_list[l].pattern) - 1] != '=') {

                     i += strlen(pattern_list[l].pattern);
                     strextract(str + i, '[', attrib, sizeof(attrib));
                     i += strlen(attrib) - 1;
                     strlcpy(hattrib, attrib, sizeof(hattrib));

                     /* replace elog:x/x for images */
                     if (strnieq(attrib, "elog:", 5)) {
                        if (email_notify) {
                           retrieve_email_from(lbs, link, NULL, NULL);
                           p = strchr(attrib, '/');
                           if (p)
                              m = atoi(p + 1) - 1;
                           if (strchr(link, '@'))
                              p = strchr(link, '@') + 1;
                           else
                              p = link;
                           sprintf(hattrib, "cid:att%d@%s", m, p);
                        } else {
                           if (email_notify)
                              compose_base_url(lbs, hattrib, sizeof(hattrib), TRUE);
                           else
                              hattrib[0] = 0;
                           if (attrib[5] == '/') {
                              if (_current_message_id == 0) {
                                 sprintf(param, "attachment%d", atoi(attrib + 6) - 1);
                                 if (isparam(param))
                                    strlcat(hattrib, getparam(param), sizeof(hattrib));
                              } else
                                 sprintf(hattrib + strlen(hattrib), "%d%s", _current_message_id, attrib + 5);
                           } else {
                              strlcat(hattrib, attrib + 5, sizeof(hattrib));
                           }
                        }
                     }

                     /* add http:// if missing */
                     else if ((!strnieq(attrib, "http://", 7) && !strnieq(attrib, "https://", 8)) &&
                              strstr(pattern_list[l].subst, "mailto") == NULL &&
                              strstr(pattern_list[l].subst, "img") == NULL &&
                              strncmp(pattern_list[l].subst, "<a", 2) != 0) {
                        if (_ssl_flag)
                           sprintf(hattrib, "https://%s", attrib);
                        else
                           sprintf(hattrib, "http://%s", attrib);
                     }

                     strlcpy(subst, pattern_list[l].subst, sizeof(subst));
                     strsubst(subst, sizeof(subst), "%#", hattrib);
                     sprintf(return_buffer + j, subst, attrib);
                     j += strlen(return_buffer + j);
                  }

               } else if (pattern_list[l].pattern[strlen(pattern_list[l].pattern) - 1] == '=') {

                  /* extract sting after '=' and put it into '%s' of subst */
                  i += strlen(pattern_list[l].pattern);
                  strextract(str + i, ']', attrib, sizeof(attrib));
                  i += strlen(attrib);
                  sprintf(return_buffer + j, pattern_list[l].subst, attrib);
                  j += strlen(return_buffer + j);

               } else if (pattern_list[l].pattern[strlen(pattern_list[l].pattern) - 1] == ' ') {

                  /* extract sting after ' ' and put it into '%s' of subst */
                  i += strlen(pattern_list[l].pattern);
                  strextract(str + i, ']', attrib, sizeof(attrib));
                  i += strlen(attrib);
                  sprintf(return_buffer + j, pattern_list[l].subst, attrib);
                  j += strlen(return_buffer + j);

               } else if (strncmp(pattern_list[l].pattern, "[/list]", 7) == 0) {

                  if (ordered_list)
                     strcpy(subst, "</ol>");
                  else
                     strcpy(subst, "</ul>");
                  ordered_list = FALSE;

                  strcpy(return_buffer + j, subst);
                  j += strlen(subst);
                  i += strlen(pattern_list[l].pattern) - 1;     // 1 gets added in for loop...

               } else if (strncmp(pattern_list[l].pattern, "|", 1) == 0) {

                  if (inside_table) {
                     strcpy(link, pattern_list[l].subst);
                     if (strstr(link, "%s")) {
                        strcpy(tmp, link);
                        if (email_notify)
                           compose_base_url(lbs, base_url, sizeof(base_url), TRUE);
                        else
                           base_url[0] = 0;
                        sprintf(link, tmp, base_url);
                     }

                     strcpy(return_buffer + j, link);
                     j += strlen(link);
                     i += strlen(pattern_list[l].pattern) - 1;  // 1 gets added in for loop...
                  } else {
                     strcpy(return_buffer + j, pattern_list[l].pattern);
                     j += strlen(pattern_list[l].pattern);
                     i += strlen(pattern_list[l].pattern) - 1;  // 1 gets added in for loop...
                  }

               } else {

                  /* simple substitution */
                  strcpy(link, pattern_list[l].subst);
                  if (strstr(link, "%s")) {
                     strcpy(tmp, link);
                     if (email_notify)
                        compose_base_url(lbs, base_url, sizeof(base_url), TRUE);
                     else
                        base_url[0] = 0;
                     sprintf(link, tmp, base_url);
                  }

                  strcpy(return_buffer + j, link);
                  j += strlen(link);
                  i += strlen(pattern_list[l].pattern) - 1;     // 1 gets added in for loop...
               }
            }                   // !elcode_disabled && !elcode_disabled1

            else if (!elcode_disabled) {

               substituted = TRUE;

               /* simple substitution */
               strcpy(link, pattern_list[l].subst);
               if (strstr(link, "%s")) {
                  strcpy(tmp, link);
                  if (email_notify)
                     compose_base_url(lbs, base_url, sizeof(base_url), TRUE);
                  else
                     base_url[0] = 0;
                  sprintf(link, tmp, base_url);
               }

               strcpy(return_buffer + j, link);
               j += strlen(link);
               i += strlen(pattern_list[l].pattern) - 1;        // 1 gets added in for loop...
            }

            if (stristr(pattern_list[l].pattern, "[code]"))
               elcode_disabled = TRUE;
            if (stristr(pattern_list[l].pattern, "[code1]"))
               elcode_disabled1 = TRUE;

            break;
         }
      }

      /* don't output tags if already subsituted by HTML code */
      if (substituted)
         continue;

      if (strnieq(str + i, "<br>", 4)) {
         strcpy(return_buffer + j, "<br />");
         j += 6;
         i += 3;
      } else
         switch (str[i]) {
         case '\r':
            if (!elcode_disabled && !elcode_disabled1 && !inside_table) {
               strcat(return_buffer, "<br />\r\n");
               j += 8;
            } else {
               strcat(return_buffer, "\r\n");
               j += 2;
            }
            break;
         case '\n':
            break;
         case '&':
            strcat(return_buffer, "&amp;");
            j += 5;
            break;
         case '<':
            strcat(return_buffer, "&lt;");
            j += 4;
            break;
         case '>':
            strcat(return_buffer, "&gt;");
            j += 4;
            break;

            /* the translation for the search highliting */
         case '\001':
            strcat(return_buffer, "<");
            j++;
            break;
         case '\002':
            strcat(return_buffer, ">");
            j++;
            break;
         case '\003':
            strcat(return_buffer, "\"");
            j++;
            break;
         case '\004':
            strcat(return_buffer, " ");
            j++;
            break;

         default:
            return_buffer[j++] = str[i];
         }
   }

   xfree(lstr);
   return_buffer[j] = 0;
   strlen_retbuf = j;
}

/*------------------------------------------------------------------*/

void rsprintf(const char *format, ...)
{
   va_list argptr;
   char str[10000];

   va_start(argptr, format);
   vsprintf(str, (char *) format, argptr);
   va_end(argptr);

   if (strlen_retbuf + (int) strlen(str) + 1 >= return_buffer_size) {
      return_buffer = xrealloc(return_buffer, return_buffer_size + 100000);
      memset(return_buffer + return_buffer_size, 0, 100000);
      return_buffer_size += 100000;
   }

   strcpy(return_buffer + strlen_retbuf, str);

   strlen_retbuf += strlen(str);
}

/*------------------------------------------------------------------*/

void flush_return_buffer()
{
   send(_sock, return_buffer, strlen_retbuf, 0);
   memset(return_buffer, 0, return_buffer_size);
   strlen_retbuf = 0;
}

/*------------------------------------------------------------------*/

/* Parameter handling functions similar to setenv/getenv */

void initparam()
{
   memset(_param, 0, sizeof(_param));
   memset(_value, 0, sizeof(_value));
   _mtext[0] = 0;
   _cmdline[0] = 0;
}

int setparam(char *param, char *value)
{
   int i;
   char str[10000];

   if (strieq(param, "text")) {
      if (strlen(value) >= TEXT_SIZE) {
         sprintf(str,
                 "Error: Entry text too big (%lu bytes). Please increase TEXT_SIZE and recompile elogd\n",
                 (unsigned long) strlen(value));
         show_error(str);
         return 0;
      }

      strlcpy(_mtext, value, TEXT_SIZE);
      return 1;
   }

   if (strieq(param, "cmdline")) {
      if (strlen(value) >= CMD_SIZE) {
         sprintf(str,
                 "Error: Command line too big (%lu bytes). Please increase CMD_SIZE and recompile elogd\n",
                 (unsigned long) strlen(value));
         show_error(str);
         return 0;
      }

      strlcpy(_cmdline, value, CMD_SIZE);
      return 1;
   }

   /* paremeters can be superseeded */
   for (i = 0; i < MAX_PARAM; i++)
      if (_param[i][0] == 0 || strieq(param, _param[i]))
         break;

   if (i < MAX_PARAM) {
      if (strlen(param) >= NAME_LENGTH) {
         sprintf(str, "Error: Parameter name too big (%lu bytes).\n", (unsigned long) strlen(param));
         show_error(str);
         return 0;
      }

      strlcpy(_param[i], param, NAME_LENGTH);

      if (strlen(value) >= NAME_LENGTH) {
         sprintf(str,
                 "Error: Parameter value for parameter <b>%s</b> too big (%lu bytes). Please increase NAME_LENGTH and recompile elogd\n",
                 param, (unsigned long) strlen(value));
         show_error(str);
         return 0;
      }

      strlcpy(_value[i], value, NAME_LENGTH);
   } else {
      sprintf(str, "Error: Too many parameters (> %d). Cannot perform operation.\n", MAX_PARAM);
      show_error(str);
      return 0;
   }

   return 1;
}

char *getparam(char *param)
{
   int i;

   if (strieq(param, "text"))
      return _mtext;

   if (strieq(param, "cmdline"))
      return _cmdline;

   for (i = 0; i < MAX_PARAM && _param[i][0]; i++)
      if (strieq(param, _param[i]))
         break;

   if (i < MAX_PARAM && _param[i][0])
      return _value[i];

   return NULL;
}

BOOL enumparam(int n, char *param, char *value)
{
   param[0] = value[0] = 0;

   if (n >= MAX_PARAM)
      return FALSE;

   if (_param[n][0] == 0)
      return FALSE;

   strcpy(param, _param[n]);
   strcpy(value, _value[n]);

   return TRUE;
}

BOOL isparam(char *param)
{
   int i;

   if (strieq(param, "text"))
      return _mtext[0] != 0;

   for (i = 0; i < MAX_PARAM && _param[i][0]; i++)
      if (strieq(param, _param[i]))
         break;

   if (i < MAX_PARAM && _param[i][0])
      return TRUE;

   return FALSE;
}

void unsetparam(char *param)
{
   int i;

   for (i = 0; i < MAX_PARAM; i++)
      if (strieq(param, _param[i]))
         break;

   if (i < MAX_PARAM) {
      for (; i < MAX_PARAM - 1; i++) {
         strlcpy(_param[i], _param[i + 1], NAME_LENGTH);
         strlcpy(_value[i], _value[i + 1], NAME_LENGTH);
      }
      _param[MAX_PARAM - 1][0] = 0;
      _value[MAX_PARAM - 1][0] = 0;
   }
}

/*------------------------------------------------------------------*/

void extract_path(char *str)
{
   char *p, str2[256];

   p = NULL;

   if (strstr(str, "http://"))
      p = str + 7;
   if (strstr(str, "https://"))
      p = str + 8;

   if (p) {
      while (*p && *p != '/')
         p++;
      if (*p == '/')
         p++;

      strcpy(str2, p);
      strcpy(str, str2);
      if (str[strlen(str) - 1] == '/')
         str[strlen(str) - 1] = 0;
   }
}

/*------------------------------------------------------------------*/

void extract_host(char *str)
{
   char *p, *ph, str2[256];

   p = NULL;

   if (strstr(str, "http://"))
      p = str + 7;
   else if (strstr(str, "https://"))
      p = str + 8;

   if (p) {
      ph = p;
      while (*p && *p != '/' && *p != ':')
         p++;
      *p = 0;

      strcpy(str2, ph);
      strcpy(str, str2);
   }
}

/*------------------------------------------------------------------*/

void compose_base_url(LOGBOOK * lbs, char *base_url, int size, BOOL email_notify)
/* return URL for elogd with optional logbook subdirectory */
{
   if (email_notify)
      if (getcfg(lbs->name, "Use Email URL", base_url, size))
         return;

   if (!getcfg("global", "URL", base_url, size)) {

      if (referer[0]) {
         /* get URL from referer */
         strlcpy(base_url, referer, size);
         if (strchr(base_url, '?'))
            *strchr(base_url, '?') = 0;
         if (strrchr(base_url, '/'))
            *(strrchr(base_url, '/') + 1) = 0;
      } else {
         if (_ssl_flag)
            strcpy(base_url, "https://");
         else
            strcpy(base_url, "http://");

         if (elog_tcp_port == 80)
            sprintf(base_url + strlen(base_url), "%s/", host_name);
         else
            sprintf(base_url + strlen(base_url), "%s:%d/", host_name, elog_tcp_port);

         if (lbs) {
            strlcat(base_url, lbs->name_enc, size);
            strlcat(base_url, "/", size);
         }
      }
   } else {
      if (base_url[strlen(base_url) - 1] != '/')
         strlcat(base_url, "/", size);
      if (lbs) {
         strlcat(base_url, lbs->name_enc, size);
         strlcat(base_url, "/", size);
      }
   }
}

/*------------------------------------------------------------------*/

void set_location(LOGBOOK * lbs, char *rel_path)
{
   char str[NAME_LENGTH], group[NAME_LENGTH], list[NAME_LENGTH], *p;
   int i;

   if (getcfg(lbs->name, "Relative redirect", str, sizeof(str)) && atoi(str) == 1) {

      if (rel_path[0])
         strlcpy(str, rel_path, sizeof(str));
      else
         strlcpy(str, ".", sizeof(str));

      rsputs("Location: ");
      rsputs(str);

   } else {
      /* Absolute redirect */

      /* if path contains http(s), go directly there */
      if (strncmp(rel_path, "http://", 7) == 0) {
         rsputs("Location: ");
         rsputs(rel_path);
      } else if (strncmp(rel_path, "https://", 8) == 0) {
         rsputs("Location: ");
         rsputs(rel_path);
      } else {

         /* check for URL options */
         str[0] = 0;
         if (lbs)
            getcfg(lbs->name, "URL", str, sizeof(str));
         else
            getcfg("global", "URL", str, sizeof(str));

         if (str[0] == 0) {

            /* get redirection from referer and host */

            if (referer[0]) {
               strlcpy(str, referer, sizeof(str));

               /* strip any parameter */
               if (strchr(str, '?'))
                  *strchr(str, '?') = 0;

               /* strip rightmost '/' */
               if (str[strlen(str) - 1] == '/')
                  str[strlen(str) - 1] = 0;

               /* extract last subdir */
               p = str + strlen(str);
               while (p > str && *p != '/')
                  p--;
               if (*p == '/')
                  p++;

               /* if last subdir equals any logbook name, strip it */
               for (i = 0; lb_list[i].name[0]; i++)
                  if (stricmp(p, lb_list[i].name_enc) == 0) {
                     *p = 0;
                     break;
                  }

               /* if last subdir equals any group, strip it */
               sprintf(group, "Group %s", p);
               if (getcfg("global", group, list, sizeof(list)))
                  *p = 0;

               /* if last subdir equals any top group, strip it */
               sprintf(group, "Top group %s", p);
               if (getcfg("global", group, list, sizeof(list)))
                  *p = 0;

            } else {
               /* assemble absolute path from host name and port */
               if (_ssl_flag)
                  sprintf(str, "https://%s", http_host);
               else
                  sprintf(str, "http://%s", http_host);
               if (elog_tcp_port != 80 && strchr(str, ':') == NULL)
                  sprintf(str + strlen(str), ":%d", elog_tcp_port);
               strlcat(str, "/", sizeof(str));
            }

            /* add trailing '/' if not present */
            if (str[strlen(str) - 1] != '/')
               strlcat(str, "/", sizeof(str));

            /* add top group if existing and not logbook */
            if (!lbs && getcfg_topgroup()) {
               strlcat(str, getcfg_topgroup(), sizeof(str));
               strlcat(str, "/", sizeof(str));
            }

            if (strncmp(rel_path, "../", 3) == 0)
               strlcat(str, rel_path + 3, sizeof(str));
            else if (strcmp(rel_path, ".") == 0) {
               if (lbs)
                  strlcat(str, lbs->name_enc, sizeof(str));
            } else if (rel_path[0] == '/')
               strlcat(str, rel_path + 1, sizeof(str));
            else {
               if (lbs) {
                  strlcat(str, lbs->name_enc, sizeof(str));
                  strlcat(str, "/", sizeof(str));
                  strlcat(str, rel_path, sizeof(str));
               } else
                  strlcat(str, rel_path, sizeof(str));
            }

            rsputs("Location: ");
            rsputs(str);

         } else {

            /* use redirection via URL */

            /* if HTTP request comes from localhost, use localhost as
               absolute link (needed if running on DSL at home) */
            if (!str[0] && strstr(http_host, "localhost")) {
               if (_ssl_flag)
                  strlcpy(str, "https://localhost", sizeof(str));
               else
                  strlcpy(str, "http://localhost", sizeof(str));
               if (elog_tcp_port != 80)
                  sprintf(str + strlen(str), ":%d", elog_tcp_port);
               strlcat(str, "/", sizeof(str));
            }

            /* add trailing '/' if not present */
            if (str[strlen(str) - 1] != '/')
               strlcat(str, "/", sizeof(str));

            /* add top group if existing and not logbook */
            if (!lbs && getcfg_topgroup()) {
               strlcat(str, getcfg_topgroup(), sizeof(str));
               strlcat(str, "/", sizeof(str));
            }

            if (strncmp(rel_path, "../", 3) == 0)
               strlcat(str, rel_path + 3, sizeof(str));
            else if (strcmp(rel_path, ".") == 0) {
               if (lbs)
                  strlcat(str, lbs->name_enc, sizeof(str));
            } else if (rel_path[0] == '/')
               strlcat(str, rel_path + 1, sizeof(str));
            else {
               if (lbs) {
                  strlcat(str, lbs->name_enc, sizeof(str));
                  strlcat(str, "/", sizeof(str));
                  strlcat(str, rel_path, sizeof(str));
               } else
                  strlcat(str, rel_path, sizeof(str));
            }

            rsputs("Location: ");
            rsputs(str);
         }
      }
   }

   rsprintf("\r\n\r\n<html>redir</html>\r\n");
}

/*------------------------------------------------------------------*/

void set_redir(LOGBOOK * lbs, char *redir)
{
   char str[NAME_LENGTH];

   str[0] = 0;

   /* prepare relative path */
   if (redir[0])
      strlcpy(str, redir, sizeof(str));
   else {
      if (lbs)
         sprintf(str, "../%s/", lbs->name_enc);
      else if (getcfg_topgroup())
         sprintf(str, ".");
   }

   set_location(lbs, str);
}

/*------------------------------------------------------------------*/

void set_cookie(LOGBOOK * lbs, char *name, char *value, BOOL global, char *expiration)
{
   char lb_name[256], str[NAME_LENGTH], format[80];
   double exp;
   time_t now;
   struct tm *gmt;

   if (lbs)
      strcpy(lb_name, lbs->name);
   else
      strcpy(lb_name, "global");

   if (value != NULL)
      rsprintf("Set-Cookie: %s=%s;", name, value);
   else
      rsprintf("Set-Cookie: %s;", name);

   /* add path */
   if (global) {
      /* path for all logbooks */
      if (getcfg(lb_name, "URL", str, sizeof(str))) {
         extract_path(str);
         url_encode(str, sizeof(str));
         rsprintf(" path=/%s;", str);
      } else
         rsprintf(" path=/;");
   } else {
      /* path for individual logbook */
      if (getcfg(lb_name, "URL", str, sizeof(str))) {
         extract_path(str);
         url_encode(str, sizeof(str));
         if (str[0])
            rsprintf(" path=/%s/%s;", str, lbs->name_enc);
         else
            rsprintf(" path=/%s;", lbs->name_enc);
      } else
         rsprintf(" path=/%s;", lbs->name_enc);
   }

   exp = atof(expiration);

   /* to clear a cookie, set expiration date to yesterday */
   if (value != NULL && value[0] == 0)
      exp = -24;

   /* add expriation date */
   if (exp != 0 && exp < 100000) {
      time(&now);
      now += (int) (3600 * exp);
      gmt = gmtime(&now);
      strcpy(format, "%A, %d-%b-%y %H:%M:%S GMT");
      strftime(str, sizeof(str), format, gmt);

      rsprintf(" expires=%s;", str);
   }

   rsprintf("\r\n");
}

/*------------------------------------------------------------------*/

void redirect(LOGBOOK * lbs, char *rel_path)
{
   /* redirect */
   rsprintf("HTTP/1.1 302 Found\r\n");
   rsprintf("Server: ELOG HTTP %s-%d\r\n", VERSION, atoi(svn_revision + 13));
   if (use_keepalive) {
      rsprintf("Connection: Keep-Alive\r\n");
      rsprintf("Keep-Alive: timeout=60, max=10\r\n");
   }

   set_location(lbs, rel_path);
}

/*------------------------------------------------------------------*/

int strbreak(char *str, char list[][NAME_LENGTH], int size, char *brk, BOOL ignore_quotes)
/* break comma-separated list into char array, stripping leading
 and trailing blanks */
{
   int i, j;
   char *p;

   memset(list, 0, size * NAME_LENGTH);
   p = str;
   if (!p || !*p)
      return 0;

   while (*p == ' ')
      p++;

   for (i = 0; *p && i < size; i++) {
      if (*p == '"' && !ignore_quotes) {
         p++;
         j = 0;
         memset(list[i], 0, NAME_LENGTH);
         do {
            /* convert two '"' to one */
            if (*p == '"' && *(p + 1) == '"') {
               list[i][j++] = '"';
               p += 2;
            } else if (*p == '"') {
               break;
            } else
               list[i][j++] = *p++;

         } while (j < NAME_LENGTH - 1);
         list[i][j] = 0;

         /* skip second '"' */
         p++;

         /* skip blanks and break character */
         while (*p == ' ')
            p++;
         if (*p && strchr(brk, *p))
            p++;
         while (*p == ' ')
            p++;

      } else {
         strlcpy(list[i], p, NAME_LENGTH);

         for (j = 0; j < (int) strlen(list[i]); j++)
            if (strchr(brk, list[i][j])) {
               list[i][j] = 0;
               break;
            }

         p += strlen(list[i]);
         while (*p == ' ')
            p++;
         if (*p && strchr(brk, *p))
            p++;
         while (*p == ' ')
            p++;
      }

      while (list[i][strlen(list[i]) - 1] == ' ')
         list[i][strlen(list[i]) - 1] = 0;

      if (!*p)
         break;
   }

   if (i == size)
      return size;

   return i + 1;
}

/*------------------------------------------------------------------*/

int scan_attributes(char *logbook)
/* scan configuration file for attributes and fill attr_list, attr_options
 and attr_flags arrays */
{
   char list[10000], str[NAME_LENGTH], type[NAME_LENGTH], tmp_list[MAX_N_ATTR][NAME_LENGTH];
   int i, j, n, m;

   if (getcfg(logbook, "Attributes", list, sizeof(list))) {
      /* reset attribute flags */
      memset(attr_flags, 0, sizeof(attr_flags));

      /* get attribute list */
      memset(attr_list, 0, sizeof(attr_list));
      n = strbreak(list, attr_list, MAX_N_ATTR, ",", FALSE);

      /* get options lists for attributes */
      memset(attr_options, 0, sizeof(attr_options));
      for (i = 0; i < n; i++) {
         sprintf(str, "Options %s", attr_list[i]);
         if (getcfg(logbook, str, list, sizeof(list)))
            strbreak(list, attr_options[i], MAX_N_LIST, ",", FALSE);

         sprintf(str, "MOptions %s", attr_list[i]);
         if (getcfg(logbook, str, list, sizeof(list))) {
            strbreak(list, attr_options[i], MAX_N_LIST, ",", FALSE);
            attr_flags[i] |= AF_MULTI;
         }

         sprintf(str, "ROptions %s", attr_list[i]);
         if (getcfg(logbook, str, list, sizeof(list))) {
            strbreak(list, attr_options[i], MAX_N_LIST, ",", FALSE);
            attr_flags[i] |= AF_RADIO;
         }

         sprintf(str, "IOptions %s", attr_list[i]);
         if (getcfg(logbook, str, list, sizeof(list))) {
            strbreak(list, attr_options[i], MAX_N_LIST, ",", FALSE);
            attr_flags[i] |= AF_ICON;
         }
      }

      /* check if attribute required */
      getcfg(logbook, "Required Attributes", list, sizeof(list));
      m = strbreak(list, tmp_list, MAX_N_ATTR, ",", FALSE);
      for (i = 0; i < m; i++) {
         for (j = 0; j < n; j++)
            if (strieq(attr_list[j], tmp_list[i]))
               attr_flags[j] |= AF_REQUIRED;
      }

      /* check if locked attribute */
      getcfg(logbook, "Locked Attributes", list, sizeof(list));
      m = strbreak(list, tmp_list, MAX_N_ATTR, ",", FALSE);
      for (i = 0; i < m; i++) {
         for (j = 0; j < n; j++)
            if (strieq(attr_list[j], tmp_list[i]))
               attr_flags[j] |= AF_LOCKED;
      }

      /* check if fixed attribute for Edit */
      getcfg(logbook, "Fixed Attributes Edit", list, sizeof(list));
      m = strbreak(list, tmp_list, MAX_N_ATTR, ",", FALSE);
      for (i = 0; i < m; i++) {
         for (j = 0; j < n; j++)
            if (strieq(attr_list[j], tmp_list[i]))
               attr_flags[j] |= AF_FIXED_EDIT;
      }

      /* check if fixed attribute for Reply */
      getcfg(logbook, "Fixed Attributes Reply", list, sizeof(list));
      m = strbreak(list, tmp_list, MAX_N_ATTR, ",", FALSE);
      for (i = 0; i < m; i++) {
         for (j = 0; j < n; j++)
            if (strieq(attr_list[j], tmp_list[i]))
               attr_flags[j] |= AF_FIXED_REPLY;
      }

      /* check for extendable options */
      getcfg(logbook, "Extendable Options", list, sizeof(list));
      m = strbreak(list, tmp_list, MAX_N_ATTR, ",", FALSE);
      for (i = 0; i < m; i++) {
         for (j = 0; j < n; j++)
            if (strieq(attr_list[j], tmp_list[i]))
               attr_flags[j] |= AF_EXTENDABLE;
      }

      for (i = 0; i < n; i++) {
         sprintf(str, "Type %s", attr_list[i]);
         if (getcfg(logbook, str, type, sizeof(type))) {
            if (strieq(type, "date"))
               attr_flags[i] |= AF_DATE;
            if (strieq(type, "datetime"))
               attr_flags[i] |= AF_DATETIME;
            if (strieq(type, "time"))
               attr_flags[i] |= AF_TIME;
            if (strieq(type, "numeric"))
               attr_flags[i] |= AF_NUMERIC;
            if (strieq(type, "userlist"))
               attr_flags[i] |= AF_USERLIST;
            if (strieq(type, "muserlist"))
               attr_flags[i] |= AF_MUSERLIST;
            if (strieq(type, "useremail"))
               attr_flags[i] |= AF_USEREMAIL;
            if (strieq(type, "museremail"))
               attr_flags[i] |= AF_MUSEREMAIL;
         }
      }

   } else {
      memcpy(attr_list, attr_list_default, sizeof(attr_list_default));
      memcpy(attr_options, attr_options_default, sizeof(attr_options_default));
      memcpy(attr_flags, attr_flags_default, sizeof(attr_flags_default));
      n = 4;
   }

   return n;
}

/*------------------------------------------------------------------*/

void show_http_header(LOGBOOK * lbs, BOOL expires, char *cookie)
{
   char str[256];

   rsprintf("HTTP/1.1 200 Document follows\r\n");
   rsprintf("Server: ELOG HTTP %s-%d\r\n", VERSION, atoi(svn_revision + 13));

   if (getcfg("global", "charset", str, sizeof(str)))
      rsprintf("Content-Type: text/html;charset=%s\r\n", str);
   else
      rsprintf("Content-Type: text/html;charset=%s\r\n", DEFAULT_HTTP_CHARSET);

   if (cookie && cookie[0])
      set_cookie(lbs, cookie, NULL, FALSE, "99999");    /* ten years by default */

   if (use_keepalive) {
      rsprintf("Connection: Keep-Alive\r\n");
      rsprintf("Keep-Alive: timeout=60, max=10\r\n");
   }

   if (expires) {
      rsprintf("Pragma: no-cache\r\n");
      rsprintf("Expires: Fri, 01 Jan 1983 00:00:00 GMT\r\n");
   }

   rsprintf("\r\n");
}

void show_plain_header(int size, char *file_name)
{
   /* header */
   rsprintf("HTTP/1.1 200 Document follows\r\n");
   rsprintf("Server: ELOG HTTP %s-%d\r\n", VERSION, atoi(svn_revision + 13));
   rsprintf("Accept-Ranges: bytes\r\n");

   if (use_keepalive) {
      rsprintf("Connection: Keep-Alive\r\n");
      rsprintf("Keep-Alive: timeout=60, max=10\r\n");
   }

   rsprintf("Pragma: no-cache\r\n");
   rsprintf("Expires: Fri, 01 Jan 1983 00:00:00 GMT\r\n");
   rsprintf("Content-Type: text/plain\r\n");
   rsprintf("Content-disposition: attachment; filename=\"%s\"\r\n", file_name);
   if (size)
      rsprintf("Content-Length: %d\r\n", size);
   rsprintf("\r\n");
}

void show_html_header(LOGBOOK * lbs, BOOL expires, char *title, BOOL close_head, BOOL rss_feed, char *cookie,
                      int absolute_link)
{
   int i, n;
   char css[1000], str[1000], media[1000];
   char css_list[MAX_N_LIST][NAME_LENGTH];

   show_http_header(lbs, expires, cookie);

   /* DOCTYPE */
   rsprintf("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">\n");

   /* this code would be for XML files...
      rsprintf("<?xml-stylesheet type=\"text/xsl\" href=\"http://www.w3.org/Math/XSL/mathml.xsl\"?>\n");
      rsprintf("<html xmlns=\"http://www.w3.org/1999/xhtml\"><head>\n");
    */

   /* page title */
   rsprintf("<html><head>\n");
   rsprintf("<META NAME=\"ROBOTS\" CONTENT=\"NOINDEX, NOFOLLOW\">\n");
   rsprintf("<title>%s</title>\n", title);

   /* Cascading Style Sheet */
   if (absolute_link)
      compose_base_url(lbs, css, sizeof(css), FALSE);
   else
      css[0] = 0;

   if (lbs != NULL && getcfg(lbs->name, "CSS", str, sizeof(str)))
      strlcat(css, str, sizeof(css));
   else if (lbs == NULL && getcfg("global", "CSS", str, sizeof(str)))
      strlcat(css, str, sizeof(css));
   else
      strlcat(css, "default.css", sizeof(css));

   if (strchr(css, ',')) {
      n = strbreak(css, css_list, MAX_N_LIST, ",", FALSE);
      for (i = 0; i < n; i++) {
         strlcpy(str, css_list[i], sizeof(str));
         if (strchr(str, '&')) {
            strlcpy(media, strchr(str, '&') + 1, sizeof(media));
            *strchr(str, '&') = 0;
            rsprintf("<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\" media=\"%s\">\n", str, media);
         }
      }
   } else
      rsprintf("<link rel=\"stylesheet\" type=\"text/css\" href=\"%s\">\n", css);
   rsprintf("<link rel=\"shortcut icon\" href=\"favicon.ico\" />\n");
   rsprintf("<link rel=\"icon\" href=\"favicon.png\" type=\"image/png\" />\n");

   if (rss_feed) {
      rsprintf("<link rel=\"alternate\" type=\"application/rss+xml\" ");
      rsprintf("title=\"ELOG %s\" ", lbs->name);
      rsprintf("href=\"elog.rdf\" />\n");
   }

   if (close_head)
      rsprintf("</head>\n");
}

void show_browser(char *browser)
{
   if (stristr(browser, "opera"))
      rsprintf("var browser = \"Opera\";\n");
   else if (stristr(browser, "konqueror"))
      rsprintf("var browser = \"Konqueror\";\n");
   else if (stristr(browser, "Safari"))
      rsprintf("var browser = \"Safari\";\n");
   else if (stristr(browser, "MSIE"))
      rsprintf("var browser = \"MSIE\";\n");
   else if (stristr(browser, "Mozilla"))
      rsprintf("var browser = \"Mozilla\";\n");
   else
      rsprintf("var browser = \"Other\";\n");
}

void show_standard_header(LOGBOOK * lbs, BOOL expires, char *title, char *path, BOOL rss_feed, char *cookie,
                          char *script)
{
   if (script) {
      show_html_header(lbs, expires, title, FALSE, rss_feed, cookie, FALSE);

      rsprintf("<script type=\"text/javascript\">\n");
      rsprintf("<!--\n");
      show_browser(browser);

      rsprintf("logbook = \"%s\";\n", lbs->name_enc);
      rsprintf("//-->\n");
      rsprintf("</script>\n");
      rsprintf("<script type=\"text/javascript\" src=\"../elcode.js\"></script>\n\n");
      rsprintf("</head>\n");
   } else
      show_html_header(lbs, expires, title, TRUE, rss_feed, cookie, FALSE);

   if (script)
      rsprintf("<body %s>\n", script);
   else
      rsprintf("<body>\n");

   show_top_text(lbs);

   if (path && path[0])
      rsprintf("<form name=\"form1\" method=\"GET\" action=\"%s\">\n\n", path);
   else
      rsprintf("<form name=\"form1\" method=\"GET\" action=\".\">\n\n");
}

/*------------------------------------------------------------------*/

void show_upgrade_page(LOGBOOK * lbs)
{
   char str[1000];

   show_html_header(lbs, FALSE, "ELOG Upgrade Information", TRUE, FALSE, NULL, FALSE);

   rsprintf("<body>\n");

   rsprintf("<table class=\"frame\" cellpadding=\"0\" cellspacing=\"0\">\n\n");

   rsprintf("<tr><td class=\"title2\">ELog Electronic Logbook Upgrade Information</font></td></tr>\n");

   rsprintf("<tr><td class=\"form1\"><br>\n");

   rsprintf("You probably use an <b>%s</b> configuration file for a ELOG version\n", CFGFILE);
   rsprintf("1.1.x, since it contains a <b><code>\"Types = ...\"</code></b> entry. From version\n");
   rsprintf("1.2.0 on, the fixed attributes <b>Type</b> and <b>Category</b> have been\n");
   rsprintf("replaced by arbitrary attributes. Please replace these two lines with the\n");
   rsprintf("following entries:<p>\n");
   rsprintf("<pre>\n");
   rsprintf("Attributes = Author, Type, Category, Subject\n");
   rsprintf("Required Attributes = Author\n");
   getcfg(lbs->name, "Types", str, sizeof(str));
   rsprintf("Options Type = %s\n", str);
   getcfg(lbs->name, "Categories", str, sizeof(str));
   rsprintf("Options Category = %s\n", str);
   rsprintf("Page title = $subject\n");
   rsprintf("</pre>\n");
   rsprintf("<p>\n");

   rsprintf("It is of course possible to change the attributes or add new ones. The new\n");
   rsprintf("options in the configuration file are described under <a href=\"\n");
   rsprintf("https://midas.psi.ch/elog/config.html\">https://midas.psi.ch/elog/config.html\n");
   rsprintf("</a>.\n");

   rsprintf("</td></tr></table>\n\n");

   rsprintf("<hr>\n");
   rsprintf("<address>\n");
   rsprintf("<a href=\"https://midas.psi.ch/~stefan\">S. Ritt</a>, 18 October 2001");
   rsprintf("</address>");
   show_bottom_text(lbs);
   rsprintf("</body></html>\r\n");
}

/*------------------------------------------------------------------*/

LBLIST *get_subgroup(LBLIST pgrp, char *logbook)
/* retrieve parent of group member "logbook" (which might be group by itself) */
{
   int i;

   for (i = 0; i < pgrp->n_members; i++) {
      /* check if logbook is current member */
      if (strieq(logbook, pgrp->member[i]->name))
         return &(pgrp->member[i]);

      /* check if logbook is in subgroup of current member */
      if (pgrp->member[i]->n_members > 0 && get_subgroup(pgrp->member[i], logbook))
         return get_subgroup(pgrp->member[i], logbook);
   }

   return NULL;
}

/*------------------------------------------------------------------*/

LBLIST get_logbook_hierarchy(void)
{
   int i, j, n, m, flag;
   char str[1000], grpname[256], grpmembers[1000];
   LBLIST root, *pgrp;
   char grplist[MAX_N_LIST][NAME_LENGTH];

   /* allocate root node */
   root = xmalloc(sizeof(LBNODE));
   memset(root, 0, sizeof(LBNODE));

   /* enumerate groups */
   for (i = n = 0;; i++) {
      if (!enumcfg("global", grpname, sizeof(grpname), grpmembers, sizeof(grpmembers), i))
         break;

      /* flag indicates top group (2) or group (1) or other entry (0) */
      flag = 0;
      strlcpy(str, grpname, sizeof(str));
      str[9] = 0;
      if (strieq(str, "top group"))
         flag = 2;
      str[5] = 0;
      if (strieq(str, "group"))
         flag = 1;

      if (flag) {

         /* allocate new node, increase member pointer array by one */
         if (n == 0)
            root->member = xmalloc(sizeof(void *));
         else
            root->member = xrealloc(root->member, (n + 1) * sizeof(void *));
         root->member[n] = xmalloc(sizeof(LBNODE));

         if (strlen(grpname) < 7)
            strlcpy(root->member[n]->name, "Invalid group definition!", 256);
         else if (flag == 1)
            strlcpy(root->member[n]->name, grpname + 6, 256);
         else
            strlcpy(root->member[n]->name, grpname + 10, 256);

         m = strbreak(grpmembers, grplist, MAX_N_LIST, ",", FALSE);
         root->member[n]->n_members = m;

         root->member[n]->member = xcalloc(sizeof(void *), m);
         root->member[n]->n_members = m;
         for (j = 0; j < m; j++) {
            root->member[n]->member[j] = xcalloc(sizeof(LBNODE), 1);
            strlcpy(root->member[n]->member[j]->name, grplist[j], 256);
         }

         root->member[n]->is_top = (flag == 2);

         n++;
      }
   }

   root->n_members = n;

   /* populate nodes with logbooks or other groups */
   for (i = 0; i < root->n_members; i++)
      if (root->member[i]) {

         for (j = 0; j < root->n_members; j++) {
            if (i != j && root->member[j] != NULL && (pgrp = get_subgroup(root->member[j],
                                                                          root->member[i]->name)) != NULL) {

               /* node is allocated twice, so free one... */
               xfree(*pgrp);

               /* ... and reference the other */
               *pgrp = root->member[i];

               /* mark original pointer invalid */
               root->member[i] = NULL;

               break;
            }
         }
      }

   /* remove empty slots */
   for (i = 0; i < root->n_members; i++)
      if (root->member[i] == NULL) {
         for (j = i + 1; j < root->n_members; j++)
            if (root->member[j])
               break;

         if (j < root->n_members && root->member[j]) {
            root->member[i] = root->member[j];
            root->member[j] = NULL;
         }
      }

   for (i = 0; i < root->n_members; i++)
      if (root->member[i] == NULL)
         break;
   if (i < root->n_members)
      root->n_members = i;

   if (n == 0) {
      for (n = 0; lb_list[n].name[0]; n++);

      /* make simple list with logbooks */
      root->member = xcalloc(n, sizeof(void *));
      root->n_members = n;

      for (i = 0; i < n; i++) {
         root->member[i] = xcalloc(1, sizeof(LBNODE));
         strlcpy(root->member[i]->name, lb_list[i].name, 256);
      }
   }

   return root;
}

/*------------------------------------------------------------------*/

void free_logbook_hierarchy(LBLIST root)
{
   int i;

   for (i = 0; i < root->n_members; i++) {
      if (root->member[i]) {
         free_logbook_hierarchy(root->member[i]);
         root->member[i] = NULL;
      }
   }

   xfree(root->member);
   xfree(root);
}

/*------------------------------------------------------------------*/

BOOL is_logbook_in_group(LBLIST pgrp, char *logbook)
/* test if "logbook" is in group node plb */
{
   int i;

   if (strieq(logbook, pgrp->name))
      return TRUE;

   for (i = 0; i < pgrp->n_members; i++) {
      if (strieq(logbook, pgrp->member[i]->name))
         return TRUE;

      if (pgrp->member[i]->n_members > 0 && is_logbook_in_group(pgrp->member[i], logbook))
         return TRUE;
   }

   return FALSE;
}

/*------------------------------------------------------------------*/

void change_logbook_in_group(LOGBOOK * lbs, char *new_name)
{
   int i, j, n, flag;
   char str[1000], grpname[256], grpmembers[1000];
   char grplist[MAX_N_LIST][NAME_LENGTH];

   /* enumerate groups */
   for (i = 0;; i++) {
      if (!enumcfg("global", grpname, sizeof(grpname), grpmembers, sizeof(grpmembers), i))
         break;

      flag = 0;
      strlcpy(str, grpname, sizeof(str));
      str[9] = 0;
      if (strieq(str, "top group"))
         flag = 2;
      str[5] = 0;
      if (strieq(str, "group"))
         flag = 1;

      if (flag) {

         n = strbreak(grpmembers, grplist, MAX_N_LIST, ",", FALSE);
         for (j = 0; j < n; j++) {
            if (strieq(lbs->name, grplist[j])) {
               /* rename or remove logbook */
               change_config_line(lbs, grpname, lbs->name, new_name);
               break;
            }
         }
      }
   }
}

/*------------------------------------------------------------------*/

void add_logbook_to_group(LOGBOOK * lbs, char *new_name)
{
   int i, j, n, flag;
   char str[1000], grpname[256], grpmembers[1000];
   char grplist[MAX_N_LIST][NAME_LENGTH];

   /* enumerate groups */
   for (i = 0;; i++) {
      if (!enumcfg("global", grpname, sizeof(grpname), grpmembers, sizeof(grpmembers), i))
         break;

      flag = 0;
      strlcpy(str, grpname, sizeof(str));
      str[9] = 0;
      if (strieq(str, "top group"))
         flag = 2;
      str[5] = 0;
      if (strieq(str, "group"))
         flag = 1;

      if (flag) {

         n = strbreak(grpmembers, grplist, MAX_N_LIST, ",", FALSE);
         for (j = 0; j < n; j++) {
            if (strieq(lbs->name, grplist[j])) {
               /* rename or remove logbook */
               change_config_line(lbs, grpname, "", new_name);
               break;
            }
         }
      }
   }
}

/*------------------------------------------------------------------*/

void show_standard_title(char *logbook, char *text, int printable)
{
   char str[256], ref[256], sclass[32], comment[256], url[256];
   int i, j, level;
   LBLIST phier, pnode, pnext, flb;

   if (printable)
      rsprintf
          ("<table class=\"pframe\" cellpadding=\"0\" cellspacing=\"0\"><!-- show_standard_title -->\n\n");
   else
      rsprintf("<table class=\"frame\" cellpadding=\"0\" cellspacing=\"0\"><!-- show_standard_title -->\n\n");

   /* scan logbook hierarchy */
   phier = get_logbook_hierarchy();

   /*---- logbook selection row ----*/

   pnode = phier;               /* start at root of tree */
   pnext = NULL;

   if (!printable && (!getcfg(logbook, "logbook tabs", str, sizeof(str)) || atoi(str) == 1)) {

      for (level = 0;; level++) {
         rsprintf("<tr><td class=\"tabs\">\n");

         if (level == 0 && getcfg("global", "main tab", str, sizeof(str)) && !getcfg_topgroup()) {
            if (getcfg("global", "main tab url", url, sizeof(url)))
               rsprintf("<span class=\"ltab\"><a href=\"%s\">%s</a></span>\n", url, str);
            else
               rsprintf("<span class=\"ltab\"><a href=\"../\">%s</a></span>\n", str);
         }

         if (level == 1 && getcfg("global", "main tab", str, sizeof(str)) && getcfg_topgroup()) {
            if (getcfg("global", "main tab url", url, sizeof(url)))
               rsprintf("<span class=\"ltab\"><a href=\"%s/\">%s</a></span>\n", url, str);
            else
               rsprintf("<span class=\"ltab\"><a href=\"../%s/\">%s</a></span>\n", getcfg_topgroup(), str);
         }

         /* iterate through members of this group */
         for (i = 0; i < pnode->n_members; i++) {

            if (getcfg(pnode->member[i]->name, "Hidden", str, sizeof(str)) && atoi(str) == 1)
               continue;

            strlcpy(str, pnode->member[i]->name, sizeof(str));

            /* build reference to first logbook in group */
            comment[0] = 0;
            if (pnode->member[i]->member == NULL) {
               getcfg(pnode->member[i]->name, "Comment", comment, sizeof(comment));
               strlcpy(ref, str, sizeof(ref));  // current node is logbook
            } else {
               flb = pnode->member[i]->member[0];       // current node is group
               while (flb->member)
                  // so traverse hierarchy
                  flb = flb->member[0];
               strlcpy(ref, flb->name, sizeof(ref));
            }
            url_encode(ref, sizeof(ref));

            if (is_logbook_in_group(pnode->member[i], logbook)) {

               /* remember member list of this group for next row */
               pnext = pnode->member[i];

               if (pnode->member[i]->member == NULL)
                  /* selected logbook */
                  strcpy(sclass, "sltab");
               else
                  /* selected group */
                  strcpy(sclass, "sgtab");
            } else {
               if (pnode->member[i]->member == NULL)
                  /* unselected logbook */
                  strcpy(sclass, "ltab");
               else
                  /* unselected group */
                  strcpy(sclass, "gtab");
            }

            if (!pnode->member[i]->is_top) {

               rsprintf("<span class=\"%s\">", sclass);

               if (comment[0]) {
                  rsprintf("<a href=\"../%s/\" title=\"", ref);
                  rsputs3(comment);
                  rsprintf("\">");
               } else
                  rsprintf("<a href=\"../%s/\">", ref);

               strlcpy(str, pnode->member[i]->name, sizeof(str));

               for (j = 0; j < (int) strlen(str); j++)
                  if (str[j] == ' ')
                     rsprintf("&nbsp;");
                  else
                     rsprintf("%c", str[j]);

               rsprintf("</a></span>\n");
            }
         }

         rsprintf("</td></tr>\n\n");

         pnode = pnext;
         pnext = NULL;

         if (pnode == NULL || pnode->n_members == 0)
            break;
      }
   }

   free_logbook_hierarchy(phier);

   /*---- title row ----*/

   rsprintf("<tr><td><table width=\"100%%\" border=\"0\" cellpadding=\"0\" cellspacing=\"0\">\n");

   /* left cell */
   rsprintf("<tr><td class=\"title1\">");

   /* use comment as title if available, else logbook name */
   if (!getcfg(logbook, "Comment", str, sizeof(str)))
      strcpy(str, logbook);

   rsprintf("&nbsp;&nbsp;");

   if (is_html(str))
      rsputs(str);
   else
      rsputs3(str);

   rsputs3(text);
   rsprintf("&nbsp;</td>\n");

   /* middle cell */
   if (isparam("full_name"))
      rsprintf("<td class=\"title2\">%s \"%s\"</td>\n", loc("Logged in as"), getparam("full_name"));
   else if (getcfg(logbook, "Guest menu commands", str, sizeof(str)))
      rsprintf("<td class=\"title2\" align=center>%s</td>\n", loc("Not logged in"));

   /* right cell */
   rsprintf("<td class=\"title3\">");

   if (getcfg(logbook, "Title image URL", str, sizeof(str)))
      rsprintf("<a href=\"%s\">\n", str);

   if (getcfg(logbook, "Title image", str, sizeof(str)))
      rsprintf("%s", str);
   else
      rsprintf("<img border=0 src=\"elog.png\" alt=\"ELOG logo\" title=\"ELOG logo\">");

   if (getcfg(logbook, "Title image URL", str, sizeof(str)))
      rsprintf("</a>\n");

   rsprintf("</td>\n");

   rsprintf("</tr></table></td></tr>\n\n");
}

/*------------------------------------------------------------------*/

void show_top_text(LOGBOOK * lbs)
{
   char str[NAME_LENGTH];
   int size;

   if (getcfg(lbs->name, "top text", str, sizeof(str)) && str[0]) {
      FILE *f;
      char file_name[256], *buf;

      /* check if file starts with an absolute directory */
      if (str[0] == DIR_SEPARATOR || str[1] == ':')
         strcpy(file_name, str);
      else {
         strlcpy(file_name, logbook_dir, sizeof(file_name));
         strlcat(file_name, str, sizeof(file_name));
      }

      f = fopen(file_name, "rb");
      if (f != NULL) {
         fseek(f, 0, SEEK_END);
         size = TELL(fileno(f));
         fseek(f, 0, SEEK_SET);

         buf = xmalloc(size + 1);
         fread(buf, 1, size, f);
         buf[size] = 0;
         fclose(f);

         rsputs(buf);
      } else
         rsputs(str);
   }
}

/*------------------------------------------------------------------*/

void show_bottom_text(LOGBOOK * lbs)
{
   char str[NAME_LENGTH], slist[20][NAME_LENGTH], svalue[20][NAME_LENGTH];
   int i, size;

   if (getcfg(lbs->name, "bottom text", str, sizeof(str))) {
      FILE *f;
      char file_name[256], *buf;

      if (str[0]) {
         /* check if file starts with an absolute directory */
         if (str[0] == DIR_SEPARATOR || str[1] == ':')
            strcpy(file_name, str);
         else {
            strlcpy(file_name, logbook_dir, sizeof(file_name));
            strlcat(file_name, str, sizeof(file_name));
         }

         f = fopen(file_name, "rb");
         if (f != NULL) {
            fseek(f, 0, SEEK_END);
            size = TELL(fileno(f));
            fseek(f, 0, SEEK_SET);

            buf = xmalloc(size + 100);
            fread(buf, 1, size, f);
            buf[size] = 0;
            fclose(f);

            i = build_subst_list(lbs, slist, svalue, NULL, TRUE);
            strsubst_list(buf, size + 100, slist, svalue, i);

            rsputs(buf);
            xfree(buf);
         } else {
            i = build_subst_list(lbs, slist, svalue, NULL, TRUE);
            strsubst_list(str, sizeof(str), slist, svalue, i);

            rsputs(str);
         }
      }
   } else
      /* add little logo */
      rsprintf
          ("<center><a class=\"bottomlink\" title=\"%s\" href=\"https://midas.psi.ch/elog/\">ELOG V%s-%d</a></center>",
           loc("Goto ELOG home page"), VERSION, atoi(svn_revision + 13));
}

/*------------------------------------------------------------------*/

void show_bottom_text_login(LOGBOOK * lbs)
{
   char str[NAME_LENGTH], slist[20][NAME_LENGTH], svalue[20][NAME_LENGTH];
   int i, size;

   if (getcfg(lbs->name, "bottom text login", str, sizeof(str))) {
      FILE *f;
      char file_name[256], *buf;

      if (str[0]) {
         /* check if file starts with an absolute directory */
         if (str[0] == DIR_SEPARATOR || str[1] == ':')
            strcpy(file_name, str);
         else {
            strlcpy(file_name, logbook_dir, sizeof(file_name));
            strlcat(file_name, str, sizeof(file_name));
         }

         f = fopen(file_name, "rb");
         if (f != NULL) {
            fseek(f, 0, SEEK_END);
            size = TELL(fileno(f));
            fseek(f, 0, SEEK_SET);

            buf = xmalloc(size + 100);
            fread(buf, 1, size, f);
            buf[size] = 0;
            fclose(f);

            i = build_subst_list(lbs, slist, svalue, NULL, TRUE);
            strsubst_list(buf, size + 100, slist, svalue, i);

            rsputs(buf);
            xfree(buf);
         } else {
            i = build_subst_list(lbs, slist, svalue, NULL, TRUE);
            strsubst_list(str, sizeof(str), slist, svalue, i);

            rsputs(str);
         }
      }
   } else
      /* add little logo */
      rsprintf
          ("<center><a class=\"bottomlink\" title=\"%s\" href=\"https://midas.psi.ch/elog/\">ELOG V%s-%d</a></center>",
           loc("Goto ELOG home page"), VERSION, atoi(svn_revision + 13));
}

/*------------------------------------------------------------------*/

void show_error(char *error)
{
   /* header */
   show_html_header(NULL, FALSE, "ELOG error", TRUE, FALSE, NULL, FALSE);

   rsprintf("<body><center>\n");
   rsprintf("<table class=\"dlgframe\" width=\"50%%\" cellpadding=\"1\" cellspacing=\"0\"");
   rsprintf("<tr><td class=\"errormsg\">%s</td></tr>\n", error);

   rsprintf("<tr><td class=\"errormsg\">");

   /*
      rsprintf("<script language=\"javascript\" type=\"text/javascript\">\n");
      rsprintf("<button type=button onClick=history.back()>%s</button>\n", loc("Back"));
      rsprintf("</script>\n");

      rsprintf("<noscript>\n");
    */

   rsprintf("%s\n", loc("Please use your browser's back button to go back"));

   /*
      rsprintf("</noscript>\n");
    */

   rsprintf("</td></tr>\n</table>\n");
   rsprintf("</center></body></html>\n");
}

/*------------------------------------------------------------------*/

void show_query(LOGBOOK * lbs, char *title, char *query_string, char *button1, char *button1_url,
                char *button2, char *button2_url)
{
   show_standard_header(lbs, TRUE, "ELog query", title, FALSE, NULL, NULL);

   rsprintf("<table class=\"dlgframe\" cellspacing=0 align=center>");

   rsprintf("<tr><td class=\"dlgtitle\">\n");
   rsprintf("%s</td></tr>\n", title);

   rsprintf("<tr><td align=center class=\"dlgform\">");
   rsprintf("%s", query_string);
   rsprintf("</td></tr>\n\n");

   rsprintf("<tr><td align=center class=\"dlgform\">");
   rsprintf("<input type=button value=\"%s\" onClick=\"window.location.href='%s';\">\n", button1,
            button1_url);
   rsprintf("<input type=button value=\"%s\" onClick=\"window.location.href='%s';\">\n", button2,
            button2_url);
   rsprintf("</td></tr>\n\n");

   rsprintf("</table>\n");
   show_bottom_text(lbs);
   rsprintf("</body></html>\r\n");
}

/*------------------------------------------------------------------*/

void set_login_cookies(LOGBOOK * lbs, char *user, char *enc_pwd)
{
   char str[256], lb_name[256], exp[80];
   BOOL global;
   int i;

   rsprintf("HTTP/1.1 302 Found\r\n");
   rsprintf("Server: ELOG HTTP %s-%d\r\n", VERSION, atoi(svn_revision + 13));
   if (use_keepalive) {
      rsprintf("Connection: Keep-Alive\r\n");
      rsprintf("Keep-Alive: timeout=60, max=10\r\n");
   }

   if (lbs)
      strcpy(lb_name, lbs->name);
   else
      strcpy(lb_name, "global");

   /* get optional expriation from configuration file */
   if (getcfg(lbs->name, "Login expiration", str, sizeof(str)) || atof(str) > 0)
      strcpy(exp, str);
   else if (isparam("remember")) {
      strcpy(exp, "744");       /* one month by default = 31*24 */
   } else
      exp[0] = 0;

   /* check if cookies should be global */
   global = getcfg("global", "Password file", str, sizeof(str));

   /* two cookies for password and user name */
   set_cookie(lbs, "unm", user, global, exp);
   set_cookie(lbs, "upwd", enc_pwd, global, exp);

   if (global &&user[0] == 0 && enc_pwd[0] == 0) {
      /* if logging out global, also delete possible non-global cookies */
      for (i = 0; lb_list[i].name[0]; i++) {
         set_cookie(&lb_list[i], "unm", user, 0, exp);
         set_cookie(&lb_list[i], "upwd", enc_pwd, 0, exp);
      }
   }

   if (user[0]) {
      /* set "remember me" cookie on login */
      if (isparam("remember"))
         set_cookie(lbs, "urem", "1", global, "8760");  /* one year = 24*365 */
      else
         set_cookie(lbs, "urem", "0", global, "8760");
   }

   set_redir(lbs, isparam("redir") ? getparam("redir") : "");
}

/*------------------------------------------------------------------*/

void remove_all_login_cookies(LOGBOOK * lbs)
{
   int i;

   rsprintf("HTTP/1.1 302 Found\r\n");
   rsprintf("Server: ELOG HTTP %s-%d\r\n", VERSION, atoi(svn_revision + 13));
   if (use_keepalive) {
      rsprintf("Connection: Keep-Alive\r\n");
      rsprintf("Keep-Alive: timeout=60, max=10\r\n");
   }

   /* remove global cookies */
   set_cookie(NULL, "unm", "", TRUE, "");
   set_cookie(NULL, "upwd", "", TRUE, "");

   for (i = 0; lb_list[i].name[0]; i++) {
      set_cookie(&lb_list[i], "unm", "", 0, "");
      set_cookie(&lb_list[i], "upwd", "", 0, "");
   }

   set_redir(lbs, isparam("redir") ? getparam("redir") : "");
}

/*------------------------------------------------------------------*/

int exist_file(char *file_name)
{
   int fh;

   fh = open(file_name, O_RDONLY | O_BINARY);
   if (fh > 0) {
      close(fh);
      return 1;
   }
   return 0;
}

/*------------------------------------------------------------------*/

void send_file_direct(char *file_name)
{
   int fh, i, length, delta;
   char str[MAX_PATH_LENGTH], charset[80], format[80];
   time_t now;
   struct tm *gmt;

   fh = open(file_name, O_RDONLY | O_BINARY);
   if (fh > 0) {
      lseek(fh, 0, SEEK_END);
      length = TELL(fh);
      lseek(fh, 0, SEEK_SET);

      rsprintf("HTTP/1.1 200 Document follows\r\n");
      rsprintf("Server: ELOG HTTP %s-%d\r\n", VERSION, atoi(svn_revision + 13));
      rsprintf("Accept-Ranges: bytes\r\n");

      /* set expiration time to one day if no thumbnail */
      if (isparam("thumb")) {
         rsprintf("Pragma: no-cache\r\n");
         rsprintf("Expires: Fri, 01 Jan 1983 00:00:00 GMT\r\n");
      } else {
         time(&now);
         now += (int) (3600 * 24);
         gmt = gmtime(&now);
         strcpy(format, "%A, %d-%b-%y %H:%M:%S GMT");
         strftime(str, sizeof(str), format, gmt);
         rsprintf("Expires: %s\r\n", str);
      }

      if (use_keepalive) {
         rsprintf("Connection: Keep-Alive\r\n");
         rsprintf("Keep-Alive: timeout=60, max=10\r\n");
      }

      /* return proper header for file type */
      for (i = 0; i < (int) strlen(file_name); i++)
         str[i] = toupper(file_name[i]);
      str[i] = 0;

      for (i = 0; filetype[i].ext[0]; i++)
         if (chkext(str, filetype[i].ext))
            break;

      if (!getcfg("global", "charset", charset, sizeof(charset)))
         strcpy(charset, DEFAULT_HTTP_CHARSET);

      if (filetype[i].ext[0]) {
         if (strncmp(filetype[i].type, "text", 4) == 0)
            rsprintf("Content-Type: %s;charset=%s\r\n", filetype[i].type, charset);
         else
            rsprintf("Content-Type: %s\r\n", filetype[i].type);
      } else if (is_ascii(file_name))
         rsprintf("Content-Type: text/plain;charset=%s\r\n", charset);
      else
         rsprintf("Content-Type: application/octet-stream;charset=%s\r\n", charset);

      rsprintf("Content-Length: %d\r\n\r\n", length);

      /* increase return buffer size if file too big */
      if (length > return_buffer_size - (int) strlen(return_buffer)) {
         delta = length - (return_buffer_size - strlen(return_buffer)) + 1000;

         return_buffer = xrealloc(return_buffer, return_buffer_size + delta);
         memset(return_buffer + return_buffer_size, 0, delta);
         return_buffer_size += delta;
      }

      return_length = strlen(return_buffer) + length;
      read(fh, return_buffer + strlen(return_buffer), length);

      close(fh);
   } else {
      char encodedname[256];
      show_html_header(NULL, FALSE, "404 Not Found", TRUE, FALSE, NULL, FALSE);

      rsprintf("<body><h1>Not Found</h1>\r\n");
      rsprintf("The requested file <b>");
      strencode2(encodedname, file_name, sizeof(encodedname));
      rsprintf("%s", encodedname);
      rsprintf("</b> was not found on this server<p>\r\n");
      rsprintf("<hr><address>ELOG version %s</address></body></html>\r\n\r\n", VERSION);
      return_length = strlen_retbuf;
      keep_alive = FALSE;
   }
}

/*------------------------------------------------------------------*/

void strencode(char *text)
{
   int i;

   for (i = 0; i < (int) strlen(text); i++) {
      switch (text[i]) {
      case '\n':
         rsprintf("<br>\n");
         break;
      case '<':
         rsprintf("&lt;");
         break;
      case '>':
         rsprintf("&gt;");
         break;
      case '&':
         rsprintf("&amp;");
         break;
      case '\"':
         rsprintf("&quot;");
         break;
      case ' ':
         rsprintf("&nbsp;");
         break;

         /* the translation for the search highliting */

      case '\001':
         rsprintf("<");
         break;
      case '\002':
         rsprintf(">");
         break;
      case '\003':
         rsprintf("\"");
         break;
      case '\004':
         rsprintf(" ");
         break;

      default:
         rsprintf("%c", text[i]);
      }
   }
}

/*------------------------------------------------------------------*/

void strencode_nouml(char *text)
{
   int i;

   for (i = 0; i < (int) strlen(text); i++) {
      switch (text[i]) {
      case '\n':
         rsprintf("<br>\n");
         break;
      case '<':
         rsprintf("&lt;");
         break;
      case '>':
         rsprintf("&gt;");
         break;
      case '\"':
         rsprintf("&quot;");
         break;
      case ' ':
         rsprintf("&nbsp;");
         break;

         /* the translation for the search highliting */

      case '\001':
         rsprintf("<");
         break;
      case '\002':
         rsprintf(">");
         break;
      case '\003':
         rsprintf("\"");
         break;
      case '\004':
         rsprintf(" ");
         break;

      default:
         rsprintf("%c", text[i]);
      }
   }
}

/*------------------------------------------------------------------*/

void xmlencode(char *text)
{
   int i;

   for (i = 0; i < (int) strlen(text); i++) {
      switch (text[i]) {
      case '<':
         rsprintf("&lt;");
         break;
      case '>':
         rsprintf("&gt;");
         break;
      case '&':
         rsprintf("&amp;");
         break;
      case '\"':
         rsprintf("&quot;");
         break;

      default:
         rsprintf("%c", text[i]);
      }
   }
}

/*------------------------------------------------------------------*/

void strencode2(char *b, const char *text, int size)
{
   int i;

   *b = 0;
   for (i = 0; i < (int) strlen(text); i++) {
      switch (text[i]) {
      case '\n':
         if (strlen(b) + 5 >= (unsigned int) size)
            return;
         strcat(b, "<br>\n");
         break;
      case '<':
         if (strlen(b) + 4 >= (unsigned int) size)
            return;
         strcat(b, "&lt;");
         break;
      case '>':
         if (strlen(b) + 4 >= (unsigned int) size)
            return;
         strcat(b, "&gt;");
         break;
      case '&':
         if (strlen(b) + 5 >= (unsigned int) size)
            return;
         strcat(b, "&amp;");
         break;
      case '\"':
         if (strlen(b) + 6 >= (unsigned int) size)
            return;
         strcat(b, "&quot;");
         break;
      default:
         if (strlen(b) + 1 >= (unsigned int) size)
            return;
         sprintf(b + strlen(b), "%c", text[i]);
      }
   }
}

/*------------------------------------------------------------------*/

int build_subst_list(LOGBOOK * lbs, char list[][NAME_LENGTH], char value[][NAME_LENGTH],
                     char attrib[][NAME_LENGTH], BOOL format_date)
{
   int i;
   char str[NAME_LENGTH], format[256];
   time_t t;
   struct tm *ts;

   /* copy attribute list */
   i = 0;
   if (attrib != NULL)
      for (; i < lbs->n_attr; i++) {
         strcpy(list[i], attr_list[i]);
         if (attrib) {
            if ((attr_flags[i] & (AF_DATE | AF_DATETIME)) && format_date) {

               t = (time_t) atoi(attrib[i]);
               ts = localtime(&t);
               assert(ts);
               if (!getcfg(lbs->name, "Date format", format, sizeof(format)))
                  strcpy(format, DEFAULT_DATE_FORMAT);

               my_strftime(value[i], NAME_LENGTH, format, ts);

            } else
               strcpy(value[i], attrib[i]);
         } else
            strlcpy(value[i], isparam(attr_list[i]) ? getparam(attr_list[i]) : "", NAME_LENGTH);
      }

   /* add remote host */
   strcpy(list[i], "remote_host");
   strlcpy(value[i++], rem_host, NAME_LENGTH);

   /* add local host */
   strcpy(list[i], "host");
   strlcpy(value[i++], host_name, NAME_LENGTH);

   /* add user names */
   strcpy(list[i], "short_name");
   if (isparam("unm"))
      strlcpy(value[i++], getparam("unm"), NAME_LENGTH);
   else
      strlcpy(value[i++], loc("Anonymous"), NAME_LENGTH);

   strcpy(list[i], "long_name");
   if (isparam("full_name"))
      strlcpy(value[i++], getparam("full_name"), NAME_LENGTH);
   else
      strlcpy(value[i++], loc("Anonymous"), NAME_LENGTH);

   /* add email */
   if (isparam("user_email")) {
      strcpy(list[i], "user_email");
      strcpy(value[i], "mailto:");
      strlcat(value[i++], getparam("user_email"), NAME_LENGTH);
   }

   /* add logbook */
   strcpy(list[i], "logbook");
   strlcpy(value[i++], lbs->name, NAME_LENGTH);

   /* add logbook */
   strcpy(list[i], "elogbook");
   strlcpy(value[i++], lbs->name_enc, NAME_LENGTH);

   /* add date */
   strcpy(list[i], "date");
   time(&t);
   if (format_date) {
      ts = localtime(&t);
      assert(ts);
      if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
         strcpy(format, DEFAULT_TIME_FORMAT);

      my_strftime(str, sizeof(str), format, ts);
   } else
      sprintf(str, "%d", (int) t);
   strcpy(value[i++], str);

   /* add UTC date */
   strcpy(list[i], "utcdate");
   time(&t);
   if (format_date) {
      ts = gmtime(&t);
      if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
         strcpy(format, DEFAULT_TIME_FORMAT);

      my_strftime(str, sizeof(str), format, ts);
   } else
      sprintf(str, "%d", (int) t);
   strcpy(value[i++], str);

   /* add ELOG version and revision */
   strcpy(list[i], "version");
   strcpy(value[i++], VERSION);

   strcpy(list[i], "revision");
   sprintf(value[i++], "%d", atoi(svn_revision + 13));

   return i;
}

/*------------------------------------------------------------------*/

void add_subst_list(char list[][NAME_LENGTH], char value[][NAME_LENGTH], char *item, char *str, int *i)
{
   strlcpy(list[*i], item, NAME_LENGTH);
   strlcpy(value[(*i)++], str, NAME_LENGTH);
}

void add_subst_time(LOGBOOK * lbs, char list[][NAME_LENGTH], char value[][NAME_LENGTH], char *item,
                    char *date, int *i, int flags)
{
   char format[80], str[256];
   time_t ltime;
   struct tm *pts;

   if (flags & (AF_DATE | AF_DATETIME)) {
      ltime = date_to_ltime(date);
      sprintf(str, "%d", (int) ltime);
      add_subst_list(list, value, item, str, i);
   } else {
      if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
         strcpy(format, DEFAULT_TIME_FORMAT);
      ltime = date_to_ltime(date);
      pts = localtime(&ltime);
      assert(pts);
      my_strftime(str, sizeof(str), format, pts);

      add_subst_list(list, value, item, str, i);
   }
}

/*------------------------------------------------------------------*/

BOOL get_password_file(LOGBOOK * lbs, char *file_name, int size)
{
   char str[256];

   getcfg(lbs->name, "Password file", str, sizeof(str));

   if (!str[0])
      return FALSE;

   if (str[0] == DIR_SEPARATOR || str[1] == ':')
      strlcpy(file_name, str, size);
   else {
      strlcpy(file_name, logbook_dir, size);
      strlcat(file_name, str, size);
   }

   return TRUE;
}

/*------------------------------------------------------------------*/

BOOL change_pwd(LOGBOOK * lbs, char *user, char *pwd)
{
   char str[256], file_name[256];
   PMXML_NODE node;

   if (!lbs->pwd_xml_tree)
      return FALSE;

   sprintf(str, "/list/user[name=%s]/password", user);
   node = mxml_find_node(lbs->pwd_xml_tree, str);
   if (node == NULL)
      return FALSE;

   mxml_replace_node_value(node, pwd);

   if (get_password_file(lbs, file_name, sizeof(file_name)))
      mxml_write_tree(file_name, lbs->pwd_xml_tree);

   return TRUE;
}

/*------------------------------------------------------------------*/

void show_change_pwd_page(LOGBOOK * lbs)
{
   char str[256], config[80], old_pwd[32], new_pwd[32], new_pwd2[32], act_pwd[32], user[80];
   int wrong_pwd;

   old_pwd[0] = new_pwd[0] = new_pwd2[0] = 0;

   if (isparam("oldpwd"))
      do_crypt(getparam("oldpwd"), old_pwd, sizeof(old_pwd));
   if (isparam("newpwd"))
      do_crypt(getparam("newpwd"), new_pwd, sizeof(new_pwd));
   if (isparam("newpwd2"))
      do_crypt(getparam("newpwd2"), new_pwd2, sizeof(new_pwd2));

   strlcpy(user, isparam("unm") ? getparam("unm") : "", sizeof(user));
   if (isparam("config"))
      strlcpy(user, getparam("config"), sizeof(user));

   wrong_pwd = FALSE;

   if (old_pwd[0] || new_pwd[0]) {
      if (user[0] && get_user_line(lbs, user, act_pwd, NULL, NULL, NULL, NULL)) {

         /* administrator does not have to supply old password if changing other user's password */
         if (isparam("unm") && is_admin_user(lbs->name, getparam("unm"))
             && stricmp(getparam("unm"), user) != 0)
            wrong_pwd = 0;
         else {
            if (strcmp(old_pwd, act_pwd) != 0)
               wrong_pwd = 1;
         }

         if (strcmp(new_pwd, new_pwd2) != 0)
            wrong_pwd = 2;
      }

      if (new_pwd[0]) {
         /* replace password */
         if (!wrong_pwd)
            change_pwd(lbs, user, new_pwd);

         if (!wrong_pwd && isparam("unm") && strcmp(user, getparam("unm")) == 0) {
            set_login_cookies(lbs, user, new_pwd);
            return;
         }

         if (!wrong_pwd) {
            /* redirect back to configuration page */
            if (isparam("config")) {
               strlcpy(config, getparam("config"), sizeof(config));
               sprintf(str, "?cmd=%s&cfg_user=%s", loc("Config"), config);
            } else
               sprintf(str, "?cmd=%s", loc("Config"));
            redirect(lbs, str);
            return;
         }
      }
   }

   show_standard_header(lbs, TRUE, loc("ELOG change password"), NULL, FALSE, NULL, NULL);

   rsprintf("<table class=\"dlgframe\" cellspacing=0 align=center>");

   if (wrong_pwd == 1)
      rsprintf("<tr><td colspan=2 class=\"dlgerror\">%s!</td></tr>\n", loc("Wrong password"));

   if (wrong_pwd == 2)
      rsprintf("<tr><td colspan=2 class=\"dlgerror\">%s!</td></tr>\n",
               loc("New passwords do not match, please retype"));

   rsprintf("<tr><td colspan=2 class=\"dlgtitle\">\n");

   rsprintf("<input type=hidden name=config value=\"%s\">", user);

   rsprintf("%s \"%s\"</td></tr>\n", loc("Change password for user"), user);

   /* do not ask for old pwasword if admin changes other user's password */
   if (isparam("unm")) {
      if (!is_admin_user(lbs->name, getparam("unm")) || stricmp(getparam("unm"), user) == 0) {
         if (isparam("oldpwd") && !(wrong_pwd == 1))
            rsprintf("<input type=hidden name=oldpwd value=\"%s\"", getparam("oldpwd"));
         else {
            rsprintf("<tr><td align=right class=\"dlgform\">%s:\n", loc("Old password"));
            rsprintf("<td align=left class=\"dlgform\"><input type=password name=oldpwd>\n");
            rsprintf("</td></tr>\n");
         }
      }
   }

   rsprintf("<tr><td align=right class=\"dlgform\">%s:</td>\n", loc("New password"));
   rsprintf("<td align=left class=\"dlgform\"><input type=password name=newpwd></td></tr>\n");

   rsprintf("<tr><td align=right class=\"dlgform\">%s:</td>\n", loc("Retype new password"));
   rsprintf("<td align=left class=\"dlgform\"><input type=password name=newpwd2></td></tr>\n");

   rsprintf("<tr><td align=center colspan=2 class=\"dlgform\"><input type=submit value=\"%s\"></td></tr>",
            loc("Submit"));

   rsprintf("</table>\n");
   show_bottom_text(lbs);
   rsprintf("</form></body></html>\r\n");
}

/*------------------------------------------------------------------*/

void get_auto_index(LOGBOOK * lbs, int index, char *format, char *retstr, int size)
/* return value of specific attribute of last entry, can be used to
 auto-increment tags */
{
   int i, message_id, loc, len, old_index;
   char str[NAME_LENGTH], attrib[MAX_N_ATTR][NAME_LENGTH], att[MAX_ATTACHMENTS][256];
   time_t now;

   if (strchr(format, '%') == NULL && strchr(format, '#') == NULL) {
      strlcpy(retstr, format, size);
      return;
   }

   time(&now);
   my_strftime(retstr, size, format, localtime(&now));

   if (strchr(retstr, '#') == NULL)
      return;

   /* record location and length of ###'s */
   for (i = loc = 0, len = 1; i < (int) strlen(retstr); i++) {
      if (retstr[i] == '#') {
         if (loc == 0)
            loc = i;
         if (i > 0 && retstr[i - 1] == '#')
            len++;
      }
   }

   /* get attribute from last entry */
   str[0] = 0;
   message_id = el_search_message(lbs, EL_LAST, 0, FALSE);

   if (!message_id) {
      /* start with 1 */
      sprintf(retstr + loc, "%0*d", len, 1);
      return;
   }

   /* search all entries to find largest index */
   old_index = 0;
   do {
      el_retrieve(lbs, message_id, NULL, attr_list, attrib, lbs->n_attr, NULL, 0, NULL, NULL, att, NULL,
                  NULL);

      /* if date part changed, start over with index */
      if (strlen(attrib[index]) > 0 && strncmp(attrib[index], retstr, loc) != 0)
         old_index = 0;
      else
         /* retrieve old index */
      if (atoi(attrib[index] + loc) > old_index)
         old_index = atoi(attrib[index] + loc);

      message_id = el_search_message(lbs, EL_PREV, message_id, FALSE);

   } while (message_id);

   /* increment index */
   sprintf(retstr + loc, "%0*d", len, old_index + 1);
}

/*------------------------------------------------------------------*/

BOOL is_author(LOGBOOK * lbs, char attrib[MAX_N_ATTR][NAME_LENGTH], char *owner)
{
   char str[NAME_LENGTH], preset[NAME_LENGTH];
   int i;

   /* check if current user is admin */
   if (is_admin_user(lbs->name, getparam("unm")))
      return TRUE;

   /* search attribute which contains short_name of author */
   for (i = 0; i < lbs->n_attr; i++) {
      sprintf(str, "Preset %s", attr_list[i]);
      if (getcfg(lbs->name, str, preset, sizeof(preset))) {
         if (strstr(preset, "$short_name")) {
            if (!isparam("unm") || strstr(attrib[i], getparam("unm")) == NULL) {
               strcpy(owner, attrib[i]);
               return FALSE;
            } else
               break;
         }
      }
   }

   if (i == lbs->n_attr) {
      /* if not found, search attribute which contains full_name of author */
      for (i = 0; i < lbs->n_attr; i++) {
         sprintf(str, "Preset %s", attr_list[i]);
         if (getcfg(lbs->name, str, preset, sizeof(preset))) {
            if (strstr(preset, "$long_name")) {
               if (!isparam("full_name") || strstr(attrib[i], getparam("full_name")) == NULL) {
                  strcpy(owner, attrib[i]);
                  return FALSE;
               } else
                  break;
            }
         }
      }
   }

   return TRUE;
}

/*------------------------------------------------------------------*/

BOOL get_author(LOGBOOK * lbs, char attrib[MAX_N_ATTR][NAME_LENGTH], char *author)
{
   char str[NAME_LENGTH], preset[NAME_LENGTH];
   int i;

   /* search attribute which contains full_name of author */
   for (i = 0; i < lbs->n_attr; i++) {
      sprintf(str, "Preset %s", attr_list[i]);
      if (getcfg(lbs->name, str, preset, sizeof(preset))) {
         if (stristr(preset, "$long_name")) {
            strcpy(author, attrib[i]);
            return TRUE;
         }
      }
   }

   /* if not found, search attribute which contains short_name of author */
   for (i = 0; i < lbs->n_attr; i++) {
      sprintf(str, "Preset %s", attr_list[i]);
      if (getcfg(lbs->name, str, preset, sizeof(preset))) {
         if (stristr(preset, "$short_name")) {
            strcpy(author, attrib[i]);
            return TRUE;
         }
      }
   }

   return FALSE;
}

/*------------------------------------------------------------------*/

BOOL is_cond_attr(int index)
{
   int i;

   for (i = 0; i < MAX_N_LIST && attr_options[index][i][0]; i++)
      if (strchr(attr_options[index][i], '{') && strchr(attr_options[index][i], '}'))
         return TRUE;

   return FALSE;
}

/*------------------------------------------------------------------*/

void show_date_selector(int day, int month, int year, char *index)
{
   int i;

   rsprintf("<select name=\"m%s\">\n", index);

   if (isparam("nsel"))
      rsprintf("<option value=\"<keep>\">- %s -\n", loc("keep original values"));
   else
      rsprintf("<option value=\"\">\n");
   for (i = 0; i < 12; i++)
      if (i + 1 == month)
         rsprintf("<option selected value=\"%d\">%s\n", i + 1, month_name(i));
      else
         rsprintf("<option value=\"%d\">%s\n", i + 1, month_name(i));
   rsprintf("</select>\n");

   rsprintf("<select name=\"d%s\">", index);

   rsprintf("<option value=\"\">\n");
   for (i = 0; i < 31; i++)
      if (i + 1 == day)
         rsprintf("<option selected value=%d>%d\n", i + 1, i + 1);
      else
         rsprintf("<option value=%d>%d\n", i + 1, i + 1);
   rsprintf("</select>\n");

   rsprintf("&nbsp;%s: ", loc("Year"));
   if (year)
      rsprintf("<input type=\"text\" size=5 maxlength=5 name=\"y%s\" value=\"%d\">", index, year);
   else
      rsprintf("<input type=\"text\" size=5 maxlength=5 name=\"y%s\">", index);

   rsprintf("\n<script language=\"javascript\" type=\"text/javascript\">\n");
   rsprintf("<!--\n");
   rsprintf("function opencal(i)\n");
   rsprintf("{\n");
   rsprintf("  window.open(\"cal.html?i=\"+i, \"\",\n");
   rsprintf("  \"top=280,left=350,width=300,height=195,dependent=yes,menubar=no,status=no,");
   rsprintf("scrollbars=no,location=no,resizable=yes\");\n");
   rsprintf("}\n\n");

   rsprintf("  document.write(\"&nbsp;&nbsp;\");\n");
   rsprintf("  document.write(\"<a href=\\\"javascript:opencal('%s')\\\">\");\n", index);
   rsprintf("  document.writeln(\"<img src=\\\"cal.png\\\" align=\\\"middle\\\" border=\\\"0\\\"");
   rsprintf("alt=\\\"%s\\\"></a>\");\n", loc("Pick a date"));

   rsprintf("//-->\n");
   rsprintf("</script>&nbsp;&nbsp;\n");
}

/*------------------------------------------------------------------*/

void show_time_selector(int hour, int min, int sec, char *index)
{
   int i;

   rsprintf("<select name=\"h%s\">\n", index);
   
   if (isparam("nsel"))
      rsprintf("<option value=\"<keep>\">- %s -\n", loc("keep original values"));
   else
      rsprintf("<option value=\"\">\n");
   for (i = 0; i < 24; i++)
      if (i == hour)
         rsprintf("<option selected value=\"%d\">%02d\n", i, i);
      else
         rsprintf("<option value=\"%d\">%02d\n", i, i);
   rsprintf("</select>&nbsp;<b>:</b>&nbsp;\n");

   rsprintf("<select name=\"n%s\">", index);
   rsprintf("<option value=\"\">\n");
   for (i = 0; i < 60; i++)
      if (i == min)
         rsprintf("<option selected value=%d>%02d\n", i, i);
      else
         rsprintf("<option value=%d>%02d\n", i, i);
   rsprintf("</select>&nbsp;<b>:</b>&nbsp;\n");

   rsprintf("<select name=\"c%s\">", index);
   rsprintf("<option value=\"\">\n");
   for (i = 0; i < 60; i++)
      if (i == sec)
         rsprintf("<option selected value=%d>%02d\n", i, i);
      else
         rsprintf("<option value=%d>%02d\n", i, i);
   rsprintf("</select>\n");

   rsprintf("\n<script language=\"javascript\" type=\"text/javascript\">\n");
   rsprintf("<!--\n");
   rsprintf("function settime_%s()\n", index);
   rsprintf("{\n");
   rsprintf("  var d = new Date();\n");
   rsprintf("  document.form1.d%s.value = d.getDate();\n", index);
   rsprintf("  document.form1.m%s.value = d.getMonth()+1;\n", index);
   rsprintf("  year = d.getYear();\n");
   rsprintf("  if (year < 1900)\n");
   rsprintf("     year += 1900;\n");
   rsprintf("  document.form1.y%s.value = year;\n", index);
   rsprintf("  document.form1.h%s.value = d.getHours();\n", index);
   rsprintf("  document.form1.n%s.value = d.getMinutes();\n", index);
   rsprintf("  document.form1.c%s.value = d.getSeconds();\n", index);
   rsprintf("}\n\n");

   rsprintf("  document.write(\"&nbsp;&nbsp;\");\n");
   rsprintf("  document.write(\"<a href=\\\"javascript:settime_%s()\\\">\");\n", index);
   rsprintf("  document.writeln(\"<img src=\\\"clock.png\\\" align=\\\"middle\\\" border=\\\"0\\\" ");
   rsprintf("alt=\\\"%s\\\"></a>\");\n", loc("Insert current time"));

   rsprintf("//-->\n");
   rsprintf("</script>\n");

}

/*------------------------------------------------------------------*/

void attrib_from_param(int n_attr, char attrib[MAX_N_ATTR][NAME_LENGTH])
{
   int i, j, first, year, month, day, hour, min, sec;
   char str[NAME_LENGTH], ua[NAME_LENGTH];
   time_t ltime;
   struct tm ts;

   for (i = 0; i < n_attr; i++) {
      strcpy(ua, attr_list[i]);
      stou(ua);
      if (attr_flags[i] & (AF_MULTI | AF_MUSERLIST | AF_MUSEREMAIL)) {
         attrib[i][0] = 0;
         first = 1;
         for (j = 0; j < MAX_N_LIST; j++) {
            sprintf(str, "%s_%d", ua, j);
            if (isparam(str)) {
               if (first)
                  first = 0;
               else
                  strlcat(attrib[i], " | ", NAME_LENGTH);
               if (strlen(attrib[i]) + strlen(getparam(str)) < NAME_LENGTH - 2)
                  strlcat(attrib[i], getparam(str), NAME_LENGTH);
               else
                  break;
            }
         }
      } else if (attr_flags[i] & AF_DATE) {

         if (isparam(ua))
            strlcpy(attrib[i], getparam(ua), NAME_LENGTH);
         else {
            sprintf(str, "y%d", i);
            year = atoi(isparam(str) ? getparam(str) : "");
            if (year < 100)
               year += 2000;

            sprintf(str, "m%d", i);
            month = atoi(isparam(str) ? getparam(str) : "");

            sprintf(str, "d%d", i);
            day = atoi(isparam(str) ? getparam(str) : "");

            memset(&ts, 0, sizeof(struct tm));
            ts.tm_year = year - 1900;
            ts.tm_mon = month - 1;
            ts.tm_mday = day;
            ts.tm_hour = 12;

            if (month && day) {
               ltime = mktime(&ts);
               sprintf(attrib[i], "%d", (int) ltime);
            } else
               strcpy(attrib[i], "");
         }

      } else if (attr_flags[i] & AF_DATETIME) {

         if (isparam(ua))
            strlcpy(attrib[i], getparam(ua), NAME_LENGTH);
         else {
            sprintf(str, "y%d", i);
            year = atoi(isparam(str) ? getparam(str) : "");
            if (year < 100)
               year += 2000;

            sprintf(str, "m%d", i);
            month = atoi(isparam(str) ? getparam(str) : "");

            sprintf(str, "d%d", i);
            day = atoi(isparam(str) ? getparam(str) : "");

            sprintf(str, "h%d", i);
            hour = atoi(isparam(str) ? getparam(str) : "");

            sprintf(str, "n%d", i);
            min = atoi(isparam(str) ? getparam(str) : "");

            sprintf(str, "c%d", i);
            sec = atoi(isparam(str) ? getparam(str) : "");

            memset(&ts, 0, sizeof(struct tm));
            ts.tm_year = year - 1900;
            ts.tm_mon = month - 1;
            ts.tm_mday = day;
            ts.tm_hour = hour;
            ts.tm_min = min;
            ts.tm_sec = sec;
            ts.tm_isdst = -1;

            if (month && day) {
               ltime = mktime(&ts);
               sprintf(attrib[i], "%d", (int) ltime);
            } else
               strcpy(attrib[i], "");
         }

      } else {
         if (isparam(attr_list[i]))
            strlcpy(attrib[i], getparam(attr_list[i]), NAME_LENGTH);
         else if (isparam(ua))
            strlcpy(attrib[i], getparam(ua), NAME_LENGTH);
         else
            attrib[i][0] = 0;
      }
   }
}

/*------------------------------------------------------------------*/

void ricon(char *name, char *comment, char *onclick)
{
   rsprintf
       ("<img align=\"middle\" name=\"%s\" src=\"icons/elc_%s.png\" alt=\"%s\" title=\"%s\" border=\"0\"\n",
        name, name, comment, comment);
   rsprintf(" onclick=\"%s\"", onclick);
   rsprintf(" onmousedown=\"document.images.%s.src='icons/eld_%s.png'\"\n", name, name);
   rsprintf(" onmouseup=\"document.images.%s.src='icons/elc_%s.png'\"\n", name, name);
   rsprintf(" onmouseover=\"this.style.cursor='pointer';\" />\n");
}

/*------------------------------------------------------------------*/

void rsicon(char *name, char *comment, char *elcode)
{
   rsprintf("<img align=\"middle\" name=\"%s\" src=\"icons/elc_%s.png\" alt=\"%s\" title=\"%s\" border=\"0\"",
            name, name, comment, comment);
   rsprintf(" onclick=\"elcode(document.form1.Text, '','%s')\"", elcode);
   rsprintf(" onmousedown=\"document.images.%s.src='icons/eld_%s.png'\"", name, name);
   rsprintf(" onmouseup=\"document.images.%s.src='icons/elc_%s.png'\"", name, name);
   rsprintf(" onmouseover=\"this.style.cursor='pointer';\" />");
}

/*------------------------------------------------------------------*/

void compare_attributes(LOGBOOK * lbs, int message_id, char attrib[MAX_N_ATTR][NAME_LENGTH], int *n)
{
   int status, i, n_reply;
   char reply_to[MAX_REPLY_TO * 10], attr[MAX_N_ATTR][NAME_LENGTH], list[MAX_N_ATTR][NAME_LENGTH];

   status = el_retrieve(lbs, message_id, NULL, attr_list, attr, lbs->n_attr,
                        NULL, NULL, NULL, reply_to, NULL, NULL, NULL);
   if (status != EL_SUCCESS)
      return;

   if (*n == 0)
      memcpy(attrib, attr, sizeof(attr));
   else {
      for (i = 0; i < lbs->n_attr; i++)
         if (!strieq(attrib[i], attr[i]))
            sprintf(attrib[i], "- %s -", loc("keep original values"));
   }
   (*n)++;
   if (isparam("elmode") && strieq(getparam("elmode"), "threaded")) {

      // go through all replies in threaded mode
      n_reply = strbreak(reply_to, list, MAX_N_ATTR, ",", FALSE);
      for (i = 0; i < n_reply; i++) {
         compare_attributes(lbs, atoi(list[i]), attrib, n);
      }
   }
}

/*------------------------------------------------------------------*/

void show_edit_form(LOGBOOK * lbs, int message_id, BOOL breply, BOOL bedit, BOOL bupload, BOOL breedit,
                    BOOL bduplicate, BOOL bpreview)
{
   int i, j, n, index, aindex, size, width, height, fh, length, input_size, input_maxlen,
       format_flags[MAX_N_ATTR], year, month, day, hour, min, sec, n_attr, n_disp_attr, n_lines,
       attr_index[MAX_N_ATTR], enc_selected, show_smileys, show_text, n_moptions, display_inline,
       allowed_encoding, thumb_status, max_n_lines;
   char str[2 * NAME_LENGTH], str2[NAME_LENGTH], preset[2 * NAME_LENGTH], *p, *pend, star[80],
       comment[10000], reply_string[256], list[MAX_N_ATTR][NAME_LENGTH], file_name[256], *buffer,
       format[256], date[80], script_onload[256], script_onfocus[256], script_onunload[256],
       attrib[MAX_N_ATTR][NAME_LENGTH], *text, orig_tag[80], reply_tag[MAX_REPLY_TO * 10],
       att[MAX_ATTACHMENTS][256], encoding[80], slist[MAX_N_ATTR + 10][NAME_LENGTH],
       svalue[MAX_N_ATTR + 10][NAME_LENGTH], owner[256], locked_by[256], class_value[80], class_name[80],
       ua[NAME_LENGTH], mid[80], title[256], login_name[256], full_name[256], cookie[256],
       orig_author[256], attr_moptions[MAX_N_LIST][NAME_LENGTH], ref[256], file_enc[256], tooltip[10000],
       enc_attr[NAME_LENGTH], user_email[256], cmd[256], thumb_name[256], **user_list;
   time_t now, ltime;
   char fl[8][NAME_LENGTH];
   struct tm *pts;
   FILE *f;
   BOOL preset_text, subtable;

   for (i = 0; i < MAX_ATTACHMENTS; i++)
      att[i][0] = 0;

   for (i = 0; i < lbs->n_attr; i++)
      attrib[i][0] = 0;

   text = xmalloc(TEXT_SIZE);
   text[0] = 0;
   orig_author[0] = 0;
   encoding[0] = 0;
   date[0] = 0;
   locked_by[0] = 0;

   /* check for file attachment (mhttpd) */
   if (isparam("fa")) {
      strlcpy(att[0], getparam("fa"), 256);

      /* remove any leading directory, to accept only files in the logbook directory ! */
      if (strchr(att[0], DIR_SEPARATOR)) {
         strlcpy(str, att[0], sizeof(str));
         strlcpy(att[0], strrchr(str, DIR_SEPARATOR) + 1, 256);
      }
   }

   if (breedit || bupload) {
      /* get date from parameter */
      if (isparam("entry_date"))
         strlcpy(date, getparam("entry_date"), sizeof(date));

      /* get attributes from parameters */
      attrib_from_param(lbs->n_attr, attrib);

      strlcpy(text, getparam("text"), TEXT_SIZE);

      for (i = 0; i < MAX_ATTACHMENTS; i++) {
         sprintf(str, "attachment%d", i);
         if (isparam(str))
            strlcpy(att[i], getparam(str), 256);
      }

      if (isparam("inlineatt")) {
         for (i = 0; i < MAX_ATTACHMENTS; i++) {
            sprintf(str, "attachment%d", i);
            if (!isparam(str) && isparam("inlineatt")) {
               strlcpy(att[i], getparam("inlineatt"), 256);
               break;
            }
         }
      }

      /* get encoding */
      strlcpy(encoding, isparam("encoding") ? getparam("encoding") : "", sizeof(encoding));
   } else {
      if (message_id) {
         /* get message for reply/edit */

         size = TEXT_SIZE;
         el_retrieve(lbs, message_id, date, attr_list, attrib, lbs->n_attr, text, &size, orig_tag, reply_tag,
                     att, encoding, locked_by);

         get_author(lbs, attrib, orig_author);

         /* strip attachments on duplicate */
         if (bduplicate)
            memset(att, 0, sizeof(att));
      } else if (isparam("nsel")) {

         /* multi edit: get all entries and check if attributes are the same */
         memset(attrib, 0, sizeof(attrib));
         for (i = n = 0; i < atoi(getparam("nsel")); i++) {
            sprintf(str, "s%d", i);
            if (isparam(str))
               compare_attributes(lbs, atoi(getparam(str)), attrib, &n);
         }
      }
   }

   if (message_id && getcfg(lbs->name, "Use Lock", str, sizeof(str)) && atoi(str) == 1 && locked_by[0]
       && !isparam("steal")) {
      sprintf(str, "%d", message_id);
      sprintf(text, "%s %s", loc("Entry is currently edited by"), locked_by);
      sprintf(cmd, "?cmd=%s&steal=1", loc("Edit"));
      show_query(lbs, loc("Entry is locked"), text, loc("Edit anyhow"), cmd, loc("Cancel"), str);
      return;
   }

   /* Determine encoding */
   if (getcfg(lbs->name, "Allowed encoding", str, sizeof(str)))
      allowed_encoding = atoi(str);
   else
      allowed_encoding = 7;

   enc_selected = 2;            /* Default is HTML */

   if (allowed_encoding == 2)   /* select ELCode if the only one allowed */
      enc_selected = 0;
   else if (allowed_encoding == 1)      /* select plain if the only one allowed */
      enc_selected = 1;
   else if (allowed_encoding == 3)      /* select ELCode if only plain and ELCode allowed */
      enc_selected = 0;

   /* Overwrite from config file */
   if (getcfg(lbs->name, "Default Encoding", str, sizeof(str)))
      enc_selected = atoi(str);

   /* determine if smiley bar should be displayed */
   show_smileys = 0;
   cookie[0] = 0;
   if (isparam("hsm") && atoi(getparam("hsm")) == 1)    /* cookie */
      show_smileys = 0;
   if (isparam("smcmd") && strieq(getparam("smcmd"), "hsm")) {  /* turn off */
      show_smileys = 0;
      strcpy(cookie, "hsm=1");
   }
   if (isparam("smcmd") && strieq(getparam("smcmd"), "ssm")) {  /* turn on */
      show_smileys = 1;
      strcpy(cookie, "hsm=0");
   }

   /* Overwrite from current entry */
   if (encoding[0]) {
      if (encoding[0] == 'E')
         enc_selected = 0;
      else if (encoding[0] == 'p')
         enc_selected = 1;
      else if (encoding[0] == 'H')
         enc_selected = 2;
   }

   show_text = !getcfg(lbs->name, "Show text", str, sizeof(str)) || atoi(str) == 1;

   /* check for preset attributes without any condition */
   set_condition("");
   for (index = 0; index < lbs->n_attr; index++) {

      /* check for preset string */
      sprintf(str, "Preset %s", attr_list[index]);
      if ((i = getcfg(lbs->name, str, preset, sizeof(preset))) > 0) {

         if ((!bedit && !breply && !bduplicate) ||      /* don't subst on edit or reply */
             (breedit && i == 2)) {     /* subst on reedit only if preset is under condition */

            /* do not format date for date attributes */
            i = build_subst_list(lbs, slist, svalue, attrib, (attr_flags[index] & (AF_DATE | AF_DATETIME))
                                 == 0);
            strsubst_list(preset, sizeof(preset), slist, svalue, i);

            /* check for index substitution */
            if (!bedit && strchr(preset, '#')) {
               /* get index */
               get_auto_index(lbs, index, preset, str, sizeof(str));
               strcpy(preset, str);
            }

            strcpy(attrib[index], preset);
         }
      }

      sprintf(str, "Preset on reply %s", attr_list[index]);
      if ((i = getcfg(lbs->name, str, preset, sizeof(preset))) > 0 && breply) {

         if (!breedit || (breedit && i == 2)) { /* subst on reedit only if preset is under condition */

            /* do not format date for date attributes */
            i = build_subst_list(lbs, slist, svalue, attrib, (attr_flags[index] & (AF_DATE | AF_DATETIME))
                                 == 0);
            strsubst_list(preset, sizeof(preset), slist, svalue, i);

            /* check for index substitution */
            if (!bedit && (strchr(preset, '%') || strchr(preset, '#'))) {
               /* get index */
               get_auto_index(lbs, index, preset, str, sizeof(str));
               strcpy(preset, str);
            }

            if (!strchr(preset, '%'))
               strcpy(attrib[index], preset);
         }
      }

      sprintf(str, "Preset on duplicate %s", attr_list[index]);
      if ((i = getcfg(lbs->name, str, preset, sizeof(preset))) > 0 && bduplicate) {

         if (!breedit || (breedit && i == 2)) { /* subst on reedit only if preset is under condition */

            /* do not format date for date attributes */
            i = build_subst_list(lbs, slist, svalue, attrib, (attr_flags[index] & (AF_DATE | AF_DATETIME))
                                 == 0);
            strsubst_list(preset, sizeof(preset), slist, svalue, i);

            /* check for index substitution */
            if (!bedit && (strchr(preset, '%') || strchr(preset, '#'))) {
               /* get index */
               get_auto_index(lbs, index, preset, str, sizeof(str));
               strcpy(preset, str);
            }

            if (!strchr(preset, '%'))
               strcpy(attrib[index], preset);
         }
      }

      /* check for p<attribute> */
      sprintf(str, "p%s", attr_list[index]);
      if (isparam(str))
         strlcpy(attrib[index], getparam(str), NAME_LENGTH);
   }

   /* evaluate conditional attributes */
   evaluate_conditions(lbs, attrib);

   /* rescan attributes if condition set */
   if (_condition[0]) {
      n_attr = scan_attributes(lbs->name);
      if (breedit || bupload)
         attrib_from_param(n_attr, attrib);

      /* now check again for conditional preset */
      for (index = 0; index < lbs->n_attr; index++) {

         /* check for preset string */
         sprintf(str, "Preset %s", attr_list[index]);
         if ((i = getcfg(lbs->name, str, preset, sizeof(preset))) > 0) {

            if ((!bedit && !breply && !bduplicate) ||   /* don't subst on edit or reply */
                (breedit && i == 2)) {  /* subst on reedit only if preset is under condition */

               /* do not format date for date attributes */
               i = build_subst_list(lbs, slist, svalue, attrib, (attr_flags[index] & (AF_DATE | AF_DATETIME))
                                    == 0);
               strsubst_list(preset, sizeof(preset), slist, svalue, i);

               /* check for index substitution */
               if (!bedit && (strchr(preset, '%') || strchr(preset, '#'))) {
                  /* get index */
                  get_auto_index(lbs, index, preset, str, sizeof(str));
                  strcpy(preset, str);
               }

               if (!strchr(preset, '%'))
                  strcpy(attrib[index], preset);
            }
         }

         sprintf(str, "Preset on reply %s", attr_list[index]);
         if ((i = getcfg(lbs->name, str, preset, sizeof(preset))) > 0 && breply) {

            if (!breedit || (breedit && i == 2)) {      /* subst on reedit only if preset is under condition */

               /* do not format date for date attributes */
               i = build_subst_list(lbs, slist, svalue, attrib, (attr_flags[index] & (AF_DATE | AF_DATETIME))
                                    == 0);
               strsubst_list(preset, sizeof(preset), slist, svalue, i);

               /* check for index substitution */
               if (!bedit && (strchr(preset, '%') || strchr(preset, '#'))) {
                  /* get index */
                  get_auto_index(lbs, index, preset, str, sizeof(str));
                  strcpy(preset, str);
               }

               if (!strchr(preset, '%'))
                  strcpy(attrib[index], preset);
            }
         }

         sprintf(str, "Preset on duplicate %s", attr_list[index]);
         if ((i = getcfg(lbs->name, str, preset, sizeof(preset))) > 0 && bduplicate) {

            if (!breedit || (breedit && i == 2)) {      /* subst on reedit only if preset is under condition */

               /* do not format date for date attributes */
               i = build_subst_list(lbs, slist, svalue, attrib, (attr_flags[index] & (AF_DATE | AF_DATETIME))
                                    == 0);
               strsubst_list(preset, sizeof(preset), slist, svalue, i);

               /* check for index substitution */
               if (!bedit && (strchr(preset, '%') || strchr(preset, '#'))) {
                  /* get index */
                  get_auto_index(lbs, index, preset, str, sizeof(str));
                  strcpy(preset, str);
               }

               if (!strchr(preset, '%'))
                  strcpy(attrib[index], preset);
            }
         }
      }

   } else
      // if (_condition[0])
      n_attr = lbs->n_attr;

   /* check for maximum number of replies */
   if (breply) {
      i = 0;
      p = strtok(reply_tag, ",");
      while (p) {
         i++;
         p = strtok(NULL, ",");
      }

      if (i >= MAX_REPLY_TO) {
         sprintf(str, loc("Maximum number of replies (%d) exceeded"), MAX_REPLY_TO);
         show_error(str);
         xfree(text);
         return;
      }
   }

   /* check for author */
   if (bedit && getcfg(lbs->name, "Restrict edit", str, sizeof(str)) && atoi(str) == 1) {
      if (!is_author(lbs, attrib, owner)) {
         sprintf(str, loc("Only user <i>%s</i> can edit this entry"), owner);
         strencode2(str2, str, sizeof(str2));
         show_error(str2);
         xfree(text);
         return;
      }
   }

   /* check for editing interval */
   if (bedit && getcfg(lbs->name, "Restrict edit time", str, sizeof(str))) {
      for (i = 0; i < *lbs->n_el_index; i++)
         if (lbs->el_index[i].message_id == message_id)
            break;

      if (i < *lbs->n_el_index && time(NULL) > lbs->el_index[i].file_time + atof(str) * 3600) {
         sprintf(str, loc("Entry can only be edited %1.2lg hours after creation"), atof(str));
         show_error(str);
         xfree(text);
         return;
      }
   }

   /* check for locking */
   if (message_id && bedit && !breedit && !bupload) {
      if (getcfg(lbs->name, "Use Lock", str, sizeof(str)) && atoi(str) == 1) {
         if (isparam("full_name"))
            strlcpy(str, getparam("full_name"), sizeof(str));
         else
            strlcpy(str, loc("user"), sizeof(str));

         strcat(str, " ");
         strcat(str, loc("on"));
         strcat(str, " ");
         strcat(str, rem_host);

         el_lock_message(lbs, message_id, str);
      }
   }

   /* remove attributes for replies */
   if (breply) {
      getcfg(lbs->name, "Remove on reply", str, sizeof(str));
      n = strbreak(str, list, MAX_N_ATTR, ",", FALSE);
      for (i = 0; i < n; i++)
         for (j = 0; j < n_attr; j++) {
            if (strieq(attr_list[j], list[i]))
               attrib[j][0] = 0;
         }
   }

   /* subst attributes for replies */
   if (breply) {
      for (index = 0; index < n_attr; index++) {
         sprintf(str, "Subst on reply %s", attr_list[index]);
         if (getcfg(lbs->name, str, preset, sizeof(preset))) {
            /* check if already second reply */
            if (orig_tag[0] == 0) {

               /* do not format date for date attributes */
               i = build_subst_list(lbs, slist, svalue, attrib, (attr_flags[index] & (AF_DATE | AF_DATETIME))
                                    == 0);
               sprintf(str, "%d", message_id);
               add_subst_list(slist, svalue, "message id", str, &i);
               add_subst_time(lbs, slist, svalue, "entry time", date, &i, attr_flags[index]);
               strsubst_list(preset, sizeof(preset), slist, svalue, i);
               strcpy(attrib[index], preset);
            }
         }
      }
   }

   /* subst attributes for edits */
   if (message_id && bedit && !breedit && !bupload) {
      for (index = 0; index < n_attr; index++) {
         sprintf(str, "Subst on edit %s", attr_list[index]);
         if (getcfg(lbs->name, str, preset, sizeof(preset))) {

            /* do not format date for date attributes */
            i = build_subst_list(lbs, slist, svalue, attrib, (attr_flags[index] & (AF_DATE | AF_DATETIME))
                                 == 0);

            sprintf(str, "%d", message_id);
            add_subst_list(slist, svalue, "message id", str, &i);
            add_subst_time(lbs, slist, svalue, "entry time", date, &i, attr_flags[index]);

            strsubst_list(preset, sizeof(preset), slist, svalue, i);
            if (strlen(preset) > NAME_LENGTH - 100) {
               if (strstr(preset + 100, "<br>")) {
                  strlcpy(str, strstr(preset + 100, "<br>"), sizeof(str));
               } else
                  strlcpy(str, preset + 100, sizeof(str));

               strcpy(preset, "...");
               strlcat(preset, str, sizeof(preset));
            }
            if (strncmp(preset, "<br>", 4) == 0)
               strcpy(attrib[index], preset + 4);
            else
               strcpy(attrib[index], preset);
         }
      }
   }

   /* header */

   if (getcfg(lbs->name, "Edit Page Title", str, sizeof(str))) {
      i = build_subst_list(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, NULL, TRUE);
      strsubst_list(str, sizeof(str), (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, i);
      strip_html(str);
   } else
      sprintf(str, "ELOG %s", lbs->name);

   show_html_header(lbs, FALSE, str, FALSE, FALSE, cookie, FALSE);

   /* java script for checking required attributes and to check for cancelled edits */
   rsprintf("<script type=\"text/javascript\">\n");
   rsprintf("<!--\n\n");
   rsprintf("var submitted = false;\n");

   if (breedit) {
      if (isparam("entry_modified") && atoi(getparam("entry_modified")) == 1) {
         rsprintf("var entry_modified = true;\n");
         rsprintf("window.status = \"%s\";\n", loc("Entry has been modified"));
      } else
         rsprintf("var entry_modified = false;\n");
   } else
      rsprintf("var entry_modified = false;\n");

   rsprintf("\n");
   rsprintf("function chkform()\n");
   rsprintf("{\n");

   for (i = 0; i < n_attr; i++) {
      if ((attr_flags[i] & AF_REQUIRED)) {

         /* convert blanks etc. to underscores */
         strcpy(ua, attr_list[i]);
         stou(ua);

         if (attr_flags[i] & AF_MULTI) {
            rsprintf("  if (\n");
            for (j = 0; j < MAX_N_LIST && attr_options[i][j][0]; j++) {
               sprintf(str, "%s_%d", ua, j);
               rsprintf("    !document.form1.%s.checked", str);
               if (attr_options[i][j + 1][0])
                  rsprintf(" &&\n");
            }
            rsprintf(") {\n");
            sprintf(str, loc("Please select at least one '%s'"), attr_list[i]);
            rsprintf("    alert(\"%s\");\n", str);
            rsprintf("    document.form1.%s_0.focus();\n", ua);
            rsprintf("    return false;\n");
            rsprintf("  }\n");

         } else if (attr_flags[i] & (AF_MUSERLIST | AF_MUSEREMAIL)) {
            rsprintf("  if (\n");
            for (j = 0;; j++) {
               if (!enum_user_line(lbs, j, login_name, sizeof(login_name)))
                  break;
               get_user_line(lbs, login_name, NULL, NULL, NULL, NULL, NULL);

               sprintf(str, "%s_%d", ua, j);
               rsprintf("    !document.form1.%s.checked", str);
               if (enum_user_line(lbs, j + 1, login_name, sizeof(login_name)))
                  rsprintf(" &&\n");
            }
            rsprintf(") {\n");
            sprintf(str, loc("Please select at least one '%s'"), attr_list[i]);
            rsprintf("    alert(\"%s\");\n", str);
            rsprintf("    document.form1.%s_0.focus();\n", ua);
            rsprintf("    return false;\n");
            rsprintf("  }\n");

         } else if (attr_flags[i] & AF_RADIO) {
            rsprintf("  for (var i=0 ; i<document.form1.%s.length ; i++)\n", ua);
            rsprintf("    if (document.form1.%s[i].checked) { break }\n", ua);
            rsprintf("  if (i == document.form1.%s.length) {\n", ua);
            sprintf(str, loc("Please select a '%s'"), attr_list[i]);
            rsprintf("    alert(\"%s\");\n", str);
            rsprintf("    document.form1.%s[0].focus();\n", ua);
            rsprintf("    return false;\n");
            rsprintf("  }\n");

         } else if (attr_flags[i] & (AF_DATE | AF_DATETIME)) {
            rsprintf("  if (document.form1.m%d.value == \"\") {\n", i);
            sprintf(str, loc("Please enter month for attribute '%s'"), attr_list[i]);
            rsprintf("    alert(\"%s\");\n", str);
            rsprintf("    document.form1.m%d.focus();\n", i);
            rsprintf("    return false;\n");
            rsprintf("  }\n");
            rsprintf("  if (document.form1.d%d.value == \"\") {\n", i);
            sprintf(str, loc("Please enter day for attribute '%s'"), attr_list[i]);
            rsprintf("    alert(\"%s\");\n", str);
            rsprintf("    document.form1.d%d.focus();\n", i);
            rsprintf("    return false;\n");
            rsprintf("  }\n");
            rsprintf("  if (document.form1.y%d.value == \"\") {\n", i);
            sprintf(str, loc("Please enter year for attribute '%s'"), attr_list[i]);
            rsprintf("    alert(\"%s\");\n", str);
            rsprintf("    document.form1.y%d.focus();\n", i);
            rsprintf("    return false;\n");
            rsprintf("  }\n");

            if (attr_flags[i] & AF_DATETIME) {
               rsprintf("  if (document.form1.h%d.value == \"\") {\n", i);
               sprintf(str, loc("Please enter hour for attribute '%s'"), attr_list[i]);
               rsprintf("    alert(\"%s\");\n", str);
               rsprintf("    document.form1.h%d.focus();\n", i);
               rsprintf("    return false;\n");
               rsprintf("  }\n");
               rsprintf("  if (document.form1.n%d.value == \"\") {\n", i);
               sprintf(str, loc("Please enter minute for attribute '%s'"), attr_list[i]);
               rsprintf("    alert(\"%s\");\n", str);
               rsprintf("    document.form1.n%d.focus();\n", i);
               rsprintf("    return false;\n");
               rsprintf("  }\n");
               rsprintf("  if (document.form1.s%d.value == \"\") {\n", i);
               sprintf(str, loc("Please enter second for attribute '%s'"), attr_list[i]);
               rsprintf("    alert(\"%s\");\n", str);
               rsprintf("    document.form1.s%d.focus();\n", i);
               rsprintf("    return false;\n");
               rsprintf("  }\n");
            }

         } else {
            rsprintf("  if (document.form1.%s.value == \"\") {\n", ua);
            sprintf(str, loc("Please enter attribute '%s'"), attr_list[i]);
            rsprintf("    alert(\"%s\");\n", str);
            rsprintf("    document.form1.%s.focus();\n", ua);
            rsprintf("    return false;\n");
            rsprintf("  }\n");
         }
      }

      if (attr_flags[i] & AF_NUMERIC) {
         /* convert blanks etc. to underscores */
         strcpy(ua, attr_list[i]);
         stou(ua);

         rsprintf("  if (document.form1.%s.value != '- %s -') {\n", ua, loc("keep original values"));
         rsprintf("    for (var i=0 ; i<document.form1.%s.value.length ; i++)\n", ua);
         rsprintf("      if (document.form1.%s.value.charAt(i) != \",\" &&\n", ua);
         rsprintf("          document.form1.%s.value.charAt(i) != \".\" &&\n", ua);
         rsprintf("          document.form1.%s.value.charAt(i) != \"-\" &&\n", ua);
         rsprintf("          (document.form1.%s.value.charAt(i) < \"0\" ||\n", ua);
         rsprintf("           document.form1.%s.value.charAt(i) > \"9\")) { break }\n", ua);
         rsprintf("    if (i<document.form1.%s.value.length) {\n", ua);
         sprintf(str, loc("Please enter numeric value for '%s'"), attr_list[i]);
         rsprintf("      alert(\"%s\");\n", str);
         rsprintf("      document.form1.%s.focus();\n", ua);
         rsprintf("      return false;\n");
         rsprintf("    }\n");
         rsprintf("  }\n");
      }

      if (attr_flags[i] & (AF_DATE | AF_DATETIME)) {
         rsprintf("  for (var i=0 ; i<document.form1.y%d.value.length ; i++)\n", i);
         rsprintf("    if ((document.form1.y%d.value.charAt(i) < \"0\" ||\n", i);
         rsprintf("         document.form1.y%d.value.charAt(i) > \"9\")) { break }\n", i);
         rsprintf("  if (i<document.form1.y%d.value.length) {\n", i);
         sprintf(str, loc("Please enter numeric value for year of attribute '%s'"), attr_list[i]);
         rsprintf("    alert(\"%s\");\n", str);
         rsprintf("    document.form1.y%d.focus();\n", i);
         rsprintf("    return false;\n");
         rsprintf("  }\n");
      }

   }

   rsprintf("  submitted = true;\n");
   rsprintf("  return true;\n");
   rsprintf("}\n\n");

   /* mark_submit() gets called via "Back" button */
   rsprintf("function mark_submit()\n");
   rsprintf("{\n");
   rsprintf("  submitted = true;\n");
   rsprintf("  return true;\n");
   rsprintf("}\n\n");

   /* chkupload() gets called via "Upload" button */
   rsprintf("function chkupload()\n");
   rsprintf("{\n");
   rsprintf("  if (document.form1.attfile.value == \"\") {\n");
   rsprintf("    alert(\"%s\");\n", loc("No attachment file specified"));
   rsprintf("    return false;\n");
   rsprintf("  }\n");
   rsprintf("  submitted = true;\n");
   rsprintf("  return true;\n");
   rsprintf("}\n\n");

   /* cond_submit() gets called via selection of new conditional attribute */
   rsprintf("function cond_submit()\n");
   rsprintf("{\n");
   rsprintf("  submitted = true;\n");
   rsprintf("  document.form1.submit();\n");
   rsprintf("}\n\n");

   /* abandon() gets called "onUnload" */
   rsprintf("function unload()\n");
   rsprintf("{\n");
   rsprintf("  if (!submitted && entry_modified) {\n");
   rsprintf("    var subm = confirm(\"%s\");\n", loc("Submit modified ELOG entry?"));
   rsprintf("    if (subm) {\n");
   rsprintf("      document.form1.jcmd.value = \"%s\";\n", loc("Submit"));
   rsprintf("      document.form1.submit();\n");
   rsprintf("    } else {\n");
   rsprintf("      document.form1.jcmd.value = \"%s\";\n", loc("Back"));
   rsprintf("      document.form1.submit();\n");
   rsprintf("    }\n");
   rsprintf("  }\n");
   rsprintf("  if (!submitted && !entry_modified) {\n");
   rsprintf("    document.form1.jcmd.value = \"%s\";\n", loc("Back"));
   rsprintf("    document.form1.submit();\n");
   rsprintf("    }\n");
   rsprintf("}\n\n");

   /* mod() gets called via throuch "onchange" event */
   rsprintf("function mod()\n");
   rsprintf("{\n");
   rsprintf("  entry_modified = true;\n");
   rsprintf("  window.status = \"%s\";\n", loc("Entry has been modified"));
   rsprintf("  document.form1.entry_modified.value = \"1\";\n");
   rsprintf("}\n\n");

   /* switch_smileys turn on/off the smiley bar by setting the smcmd, which in turn
      sets the hsm=0/hsm=1 cookie */
   rsprintf("function switch_smileys()\n");
   rsprintf("{\n");
   if (show_smileys)
      rsprintf("   document.form1.smcmd.value = 'hsm';\n");
   else
      rsprintf("   document.form1.smcmd.value = 'ssm';\n");
   rsprintf("   submitted = true;\n");
   rsprintf("   document.form1.submit();\n");
   rsprintf("}\n\n");

   if (enc_selected != 2) {
      if (!getcfg(lbs->name, "Message height", str, sizeof(str)) && !getcfg(lbs->name, "Message width", str,
                                                                            sizeof(str))) {
         /* javascript for resizing edit box */
         rsprintf("\nfunction init_resize()\n");
         rsprintf("{\n");
         rsprintf("   window.onresize = resize_textarea;\n");
         rsprintf("   resize_textarea();\n");
         rsprintf("}\n\n");
         rsprintf("function resize_textarea()\n");
         rsprintf("{\n");
         rsprintf("   p = document.form1.Text;\n");
         rsprintf("   t = p.offsetTop;\n");
         rsprintf("   while (p = p.offsetParent)\n");
         rsprintf("      t += p.offsetTop;\n");
         rsprintf("   if (window.innerHeight) // netscape\n");
         rsprintf("      height = window.innerHeight - t - 135;\n");
         rsprintf("   else // IE\n");
         rsprintf("      height = document.body.offsetHeight - t - 135;\n");
         rsprintf("   if (height < 300)\n");
         rsprintf("      height = 300;\n");
         rsprintf("   document.form1.Text.style.height = height;\n");
         rsprintf("}\n\n");
      }
   }

   /* strings for elcode.js */
   if (enc_selected == 0) {
      rsprintf("linkText_prompt = \"%s\";\n", loc("Enter name of hyperlink"));
      rsprintf("linkURL_prompt  = \"%s\";\n", loc("Enter URL of hyperlink"));
      rsprintf("linkHeading_prompt  = \"%s\";\n", loc("Enter heading level (1, 2 or 3)"));
   }

   show_browser(browser);

   rsprintf("var logbook = \"%s\";\n", lbs->name_enc);
   for (i = 0; i < MAX_ATTACHMENTS; i++)
      if (!att[i][0]) {
         /* put first free attachment for uploader */
         rsprintf("var next_attachment = %d;\n", i + 1);
         break;
      }
   rsprintf("//-->\n");
   rsprintf("</script>\n");

   /* optionally load ImageMagic JavaScript code */
   if (image_magick_exist)
      rsprintf("<script type=\"text/javascript\" src=\"../im.js\"></script>\n\n");

   /* optionally load ELCode JavaScript code */
   if (enc_selected == 0)
      rsprintf("<script type=\"text/javascript\" src=\"../elcode.js\"></script>\n\n");

   if (enc_selected == 2 && fckedit_exist) {
      rsprintf("<script type=\"text/javascript\" src=\"../fckeditor/fckeditor.js\"></script>\n");
      rsprintf("<script type=\"text/javascript\">\n");
      /* define strings for current language */
      rsprintf("var ELOGSubmitEntry = \"%s\";\n", loc("Submit entry"));
      rsprintf("var ELOGInsertImage = \"%s\";\n", loc("Insert image"));
      rsprintf("var ELOGInsertDateTime = \"%s\";\n\n", loc("Insert Date/Time"));
      rsprintf("var ELOGLinkTextPrompt = \"%s\";\n\n", loc("Enter name of hyperlink"));
      rsprintf("var ELOGLinkURLPrompt = \"%s\";\n\n", loc("Enter URL of hyperlink"));
      rsprintf("var ELOGSource = \"%s\";\n\n", loc("Show HTML source code"));
      rsprintf("function initFCKedit()\n");
      rsprintf("{\n");
      rsprintf("   var oFCKeditor = new FCKeditor('Text', '100%%', '500');\n");
      rsprintf("   oFCKeditor.BasePath = '../fckeditor/';\n");
      rsprintf("   oFCKeditor.Config['CustomConfigurationsPath'] = '../fckelog.js';\n");
      rsprintf("   oFCKeditor.Config['AutoDetectLanguage'] = false;\n");
      getcfg("global", "Language", str, sizeof(str));
      if (!str[0])
         strcpy(str, "en");
      else if (stricmp(str, "german") == 0)
         strcpy(str, "de");
      else if (stricmp(str, "brazilian") == 0)
         strcpy(str, "pt-br");
      else if (stricmp(str, "czech") == 0)
         strcpy(str, "cs");
      else if (stricmp(str, "dutch") == 0)
         strcpy(str, "nl");
      else if (stricmp(str, "spanish") == 0)
         strcpy(str, "es");
      else if (stricmp(str, "swedish") == 0)
         strcpy(str, "sv");
      else if (stricmp(str, "turkish") == 0)
         strcpy(str, "tr");
      else if (stricmp(str, "zh_CN-GB2312") == 0)
         strcpy(str, "zh-cn");
      else
         str[2] = 0;
      rsprintf("   oFCKeditor.Config['DefaultLanguage'] = \"%s\";\n", str);
      rsprintf("   oFCKeditor.ReplaceTextarea();\n");
      rsprintf("   document.getElementById('HTML').checked = true;\n");
      rsprintf("}\n");
      rsprintf("</script>\n\n");
   }

   /* external script if requested */
   if (isparam("js")) {
      rsprintf("<script src=\"%s\" type=\"text/javascript\">\n", getparam("js"));
      rsprintf("</script>\n\n");
   }

   rsprintf("</head>\n");

   script_onload[0] = 0;
   script_onfocus[0] = 0;
   if ((isparam("inlineatt") && *getparam("inlineatt")) || bpreview)
      strcpy(script_onload, "document.form1.Text.focus();");

   if (enc_selected == 0) {
      if (!getcfg(lbs->name, "Message height", str, sizeof(str)) && !getcfg(lbs->name, "Message width", str,
                                                                            sizeof(str))) {

         strcat(script_onload, "elKeyInit();init_resize();");
         strcat(script_onfocus, "elKeyInit();");
      } else
         strcat(script_onload, "elKeyInit();");
      strcat(script_onfocus, "elKeyInit();");
   } else if (enc_selected == 2 && fckedit_exist) {
      strcat(script_onload, "initFCKedit();");
   } else
      strcat(script_onload, "init_resize();");

   script_onunload[0] = 0;
   if (getcfg(lbs->name, "Use Lock", str, sizeof(str)) && atoi(str) == 1)
      strcat(script_onunload, "unload();");

   rsprintf("<body");
   if (script_onload[0])
      rsprintf(" OnLoad=\"%s\"", script_onload);
   if (script_onfocus[0])
      rsprintf(" OnFocus=\"%s\"", script_onfocus);
   if (script_onunload[0])
      rsprintf(" OnUnload=\"%s\"", script_onunload);
   rsprintf(">\n");

   show_top_text(lbs);
   rsprintf("<form name=form1 method=\"POST\" action=\"./\" ");
   rsprintf("enctype=\"multipart/form-data\">\n");

   /*---- add password in case cookie expires during edit ----*/

   if (getcfg(lbs->name, "Write password", str, sizeof(str)))
      rsprintf("<input type=hidden name=\"wpwd\" value=\"%s\">\n", str);

   if (getcfg(lbs->name, "Password file", str, sizeof(str)) && isparam("unm") && isparam("upwd")) {
      rsprintf("<input type=hidden name=\"unm\" value=\"%s\">\n", getparam("unm"));
      rsprintf("<input type=hidden name=\"upwd\" value=\"%s\">\n", getparam("upwd"));
   }

   rsprintf("<input type=hidden name=\"jcmd\">\n");
   rsprintf("<input type=hidden name=\"smcmd\">\n");
   rsprintf("<input type=hidden name=\"inlineatt\">\n");

   if (isparam("entry_modified") && atoi(getparam("entry_modified")) == 1)
      rsprintf("<input type=hidden name=\"entry_modified\" value=\"1\">\n");
   else
      rsprintf("<input type=hidden name=\"entry_modified\" value=\"0\">\n");

   /*---- title row ----*/

   show_standard_title(lbs->name, "", 0);

   /*---- menu buttons ----*/

   rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");

   /* default cmd */
   rsprintf("<input type=hidden name=cmd value=\"%s\">\n", loc("Update"));

   rsprintf("<input type=\"submit\" name=\"cmd\" value=\"%s\" onClick=\"return chkform();\">\n",
            loc("Submit"));
   rsprintf("<input type=\"submit\" name=\"cmd\" value=\"%s\" onClick=\"return chkform();\">\n",
            loc("Preview"));
   rsprintf("<input type=\"submit\" name=\"cmd\" value=\"%s\" onClick=\"return mark_submit();\">\n",
            loc("Back"));
   rsprintf("</span></td></tr>\n\n");

   /*---- entry form ----*/

   /* table for two-column items */
   rsprintf("<tr><td><table class=\"listframe\" width=\"100%%\" cellspacing=\"0\" cellpadding=\"0\">");

   /* print required message if one of the attributes has it set */
   for (i = 0; i < n_attr; i++) {
      if (attr_flags[i] & AF_REQUIRED) {
         rsprintf("<tr><td colspan=2 class=\"attribvalue\">%s <font color=red>*</font> %s</td></tr>\n",
                  loc("Fields marked with"), loc("are required"));
         break;
      }
   }

   if (!isparam("nsel")) {
      time(&now);
      if (bedit) {
         if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
            strcpy(format, DEFAULT_TIME_FORMAT);

         ltime = date_to_ltime(date);
         pts = localtime(&ltime);
         assert(pts);
         my_strftime(str, sizeof(str), format, pts);
      } else {
         if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
            strcpy(format, DEFAULT_TIME_FORMAT);
         my_strftime(str, sizeof(str), format, localtime(&now));
         strcpy(date, ctime(&now));
         date[24] = 0;
      }

      rsprintf("<tr><td nowrap width=\"10%%\" class=\"attribname\">%s:</td>", loc("Entry time"));
      rsprintf("<td class=\"attribvalue\">%s\n", str);
      rsprintf("<input type=hidden name=entry_date value=\"%s\"></td></tr>\n", date);
   }

   if (_condition[0])
      rsprintf("<input type=hidden name=condition value=\"%s\"></td></tr>\n", _condition);

   /* retrieve attribute flags */
   for (i = 0; i < n_attr; i++) {
      format_flags[i] = 0;
      sprintf(str, "Format %s", attr_list[i]);
      if (getcfg(lbs->name, str, format, sizeof(format))) {
         n = strbreak(format, fl, 8, ",", FALSE);
         if (n > 0)
            format_flags[i] = atoi(fl[0]);
      }
   }

   subtable = 0;

   /* generate list of attributes to show */
   if (getcfg(lbs->name, "Show attributes edit", str, sizeof(str))) {
      n_disp_attr = strbreak(str, list, MAX_N_ATTR, ",", FALSE);
      for (i = 0; i < n_disp_attr; i++) {
         for (j = 0; j < n_attr; j++)
            if (strieq(attr_list[j], list[i]))
               break;
         if (!strieq(attr_list[j], list[i]))
            /* attribute not found */
            j = 0;
         attr_index[i] = j;
      }
   } else {
      for (i = 0; i < n_attr; i++)
         attr_index[i] = i;
      n_disp_attr = n_attr;
   }

   /* display attributes */
   for (aindex = 0; aindex < n_disp_attr; aindex++) {

      index = attr_index[aindex];

      strcpy(class_name, "attribname");
      strcpy(class_value, "attribvalue");
      input_size = 80;
      input_maxlen = NAME_LENGTH;
      strcpy(ua, attr_list[index]);
      stou(ua);

      sprintf(str, "Format %s", attr_list[index]);
      if (getcfg(lbs->name, str, format, sizeof(format))) {
         n = strbreak(format, fl, 8, ",", FALSE);
         if (n > 1)
            strlcpy(class_name, fl[1], sizeof(class_name));
         if (n > 2)
            strlcpy(class_value, fl[2], sizeof(class_value));
         if (n > 3 && atoi(fl[3]) > 0)
            input_size = atoi(fl[3]);
         if (n > 4 && atoi(fl[4]) > 0)
            input_maxlen = atoi(fl[4]);
      }

      if (format_flags[index] & AFF_SAME_LINE)
         /* if attribute on same line, do nothing */
         rsprintf("");
      else if (aindex < n_disp_attr - 1 && (format_flags[attr_index[aindex + 1]] & AFF_SAME_LINE)) {
         /* if next attribute on same line, start a new subtable */
         rsprintf("<tr><td colspan=2><table width=\"100%%\" cellpadding=\"0\" cellspacing=\"0\"><tr>");
         subtable = 1;
      } else
         /* for normal attribute, start new row */
         rsprintf("<tr>");

      strcpy(star, (attr_flags[index] & AF_REQUIRED) ? "<font color=red>*</font>" : "");

      /* display text box with optional tooltip */
      sprintf(str, "Tooltip %s", attr_list[index]);
      title[0] = 0;
      if (getcfg(lbs->name, str, comment, sizeof(comment)))
         sprintf(title, " title=\"%s\"", comment);

      rsprintf("<td%s nowrap class=\"%s\">", title, class_name);

      /* display attribute name */
      rsprintf("%s%s:", attr_list[index], star);

      /* show optional comment */
      sprintf(str, "Comment %s", attr_list[index]);
      if (getcfg(lbs->name, str, comment, sizeof(comment)))
         rsprintf("<br><span class=\"selcomment\"><b>%s</b></span>\n", comment);

      rsprintf("</td>\n");

      /* if attribute cannot be changed, just display text */
      if ((attr_flags[index] & AF_LOCKED) || ((bedit && !breedit && !bupload) && (attr_flags[index]
                                                                                  & AF_FIXED_EDIT))
          || (message_id && !bedit && (attr_flags[index] & AF_FIXED_REPLY))) {

         if (attr_flags[index] & AF_DATE) {

            if (!getcfg(lbs->name, "Date format", format, sizeof(format)))
               strcpy(format, DEFAULT_DATE_FORMAT);

            ltime = atoi(attrib[index]);
            pts = localtime(&ltime);
            assert(pts);
            if (ltime == 0)
               strcpy(str, "-");
            else
               my_strftime(str, sizeof(str), format, pts);

         } else if (attr_flags[index] & AF_DATETIME) {

            if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
               strcpy(format, DEFAULT_TIME_FORMAT);

            ltime = atoi(attrib[index]);
            pts = localtime(&ltime);
            assert(pts);
            if (ltime == 0)
               strcpy(str, "-");
            else
               my_strftime(str, sizeof(str), format, pts);

         } else
            strlcpy(str, attrib[index], sizeof(str));

         rsprintf("<td%s class=\"%s\">\n", title, class_value);
         rsputs2(lbs, FALSE, str);
         rsprintf("&nbsp;");

         if (attr_flags[index] & AF_MULTI) {
            for (i = 0; i < MAX_N_LIST && attr_options[index][i][0]; i++) {
               sprintf(str, "%s_%d", ua, i);

               if (strstr(attrib[index], attr_options[index][i]))
                  rsprintf("<input type=\"hidden\" name=\"%s\" value=\"%s\">\n", str, attr_options[index][i]);
            }
         } else if (attr_flags[index] & AF_MUSERLIST) {

            for (i = 0;; i++) {
               if (!enum_user_line(lbs, i, login_name, sizeof(login_name)))
                  break;
               get_user_line(lbs, login_name, NULL, full_name, NULL, NULL, NULL);

               sprintf(str, "%s_%d", ua, i);

               if (strstr(attrib[index], full_name))
                  rsprintf("<input type=\"hidden\" name=\"%s\" value=\"%s\">\n", str, full_name);
            }
         } else if (attr_flags[index] & AF_MUSEREMAIL) {

            for (i = 0;; i++) {
               if (!enum_user_line(lbs, i, login_name, sizeof(login_name)))
                  break;
               get_user_line(lbs, login_name, NULL, NULL, user_email, NULL, NULL);

               sprintf(str, "%s_%d", ua, i);

               if (strstr(attrib[index], user_email))
                  rsprintf("<input type=\"hidden\" name=\"%s\" value=\"%s\">\n", str, user_email);
            }
         } else if (attr_flags[index] & AF_ICON) {
            for (i = 0; i < MAX_N_LIST && attr_options[index][i][0]; i++) {
               sprintf(str, "%s_%d", ua, i);

               if (strstr(attrib[index], attr_options[index][i]))
                  rsprintf("<input type=\"hidden\" name=\"%s\" value=\"%s\">\n", str, attr_options[index][i]);
            }
         } else {
            strencode2(str, attrib[index], sizeof(str));
            rsprintf("<input type=\"hidden\" name=\"%s\" value=\"%s\"></td>\n", ua, str);
         }
      } else {
         if (attr_options[index][0][0] == 0) {

            if (attr_flags[index] & AF_DATE) {

               year = month = day = 0;
               if (attrib[index][0]) {
                  ltime = atoi(attrib[index]);
                  if (ltime > 0) {
                     pts = localtime(&ltime);
                     assert(pts);
                     year = pts->tm_year + 1900;
                     month = pts->tm_mon + 1;
                     day = pts->tm_mday;
                  }
               }

               rsprintf("<td%s class=\"attribvalue\">", title);
               sprintf(str, "%d", index);
               show_date_selector(day, month, year, str);
               rsprintf("</td>\n");

            } else if (attr_flags[index] & AF_DATETIME) {

               year = month = day = 0;
               hour = min = sec = -1;
               if (attrib[index][0]) {
                  ltime = atoi(attrib[index]);
                  if (ltime > 0) {
                     pts = localtime(&ltime);
                     assert(pts);
                     year = pts->tm_year + 1900;
                     month = pts->tm_mon + 1;
                     day = pts->tm_mday;
                     hour = pts->tm_hour;
                     min = pts->tm_min;
                     sec = pts->tm_sec;
                  }
               }

               rsprintf("<td%s class=\"%s\">", title, class_value);
               sprintf(str, "%d", index);
               show_date_selector(day, month, year, str);
               rsprintf("&nbsp;&nbsp;");
               show_time_selector(hour, min, sec, str);

               rsprintf("</td>\n");

            } else if (attr_flags[index] & AF_USERLIST) {

               rsprintf("<td%s class=\"%s\">\n", title, class_value);

               /* display drop-down box with list of users */
               rsprintf("<select name=\"%s\"", ua);
               rsprintf(" onChange=\"mod();\">\n");

               /* display emtpy option */
               sprintf(str, "- %s -", loc("keep original values"));
               if (strcmp(str, attrib[index]) == 0 && isparam("nsel"))
                  rsprintf("<option value=\"<keep>\">%s\n", str);
               else
                  rsprintf("<option value=\"\">- %s -\n", loc("please select"));

               for (i = 0;; i++) {
                  if (!enum_user_line(lbs, i, login_name, sizeof(login_name)))
                     break;
                  get_user_line(lbs, login_name, NULL, str, NULL, NULL, NULL);

                  if (strieq(str, attrib[index]))
                     rsprintf("<option selected value=\"%s\">%s\n", str, str);
                  else
                     rsprintf("<option value=\"%s\">%s\n", str, str);
               }

               rsprintf("</select>\n");

               rsprintf("</td>\n");

            } else if (attr_flags[index] & AF_MUSERLIST) {

               /* display multiple check boxes with user names */
               rsprintf("<td%s class=\"%s\">\n", title, class_value);

               n_moptions = strbreak(attrib[index], attr_moptions, MAX_N_LIST, "|", FALSE);

               /* allocate list of users and populate it */
               for (n = 0;; n++) {
                  if (!enum_user_line(lbs, n, login_name, sizeof(login_name)))
                     break;
               }

               user_list = xcalloc(sizeof(char *), n);
               for (i = 0; i < n; i++)
                  user_list[i] = xcalloc(NAME_LENGTH, 1);

               for (i = 0; i < n; i++) {
                  enum_user_line(lbs, i, str, NAME_LENGTH);
                  get_user_line(lbs, str, NULL, user_list[i], NULL, NULL, NULL);
               }

               /* sort list */
               qsort(user_list, n, sizeof(char *), ascii_compare);

               for (i = 0; i < n; i++) {
                  sprintf(str, "%s_%d", ua, i);

                  rsprintf("<span style=\"white-space:nowrap;\">\n");

                  for (j = 0; j < n_moptions; j++)
                     if (strcmp(attr_moptions[j], user_list[i]) == 0)
                        break;

                  if (j < n_moptions)
                     rsprintf
                         ("<input type=checkbox id=\"%s\" name=\"%s\" value=\"%s\" checked onChange=\"mod();\">\n",
                          str, str, user_list[i]);
                  else
                     rsprintf
                         ("<input type=checkbox id=\"%s\" name=\"%s\" value=\"%s\" onChange=\"mod();\">\n",
                          str, str, user_list[i]);

                  rsprintf("<label for=\"%s\">%s</label>\n", str, user_list[i]);
                  rsprintf("</span>\n");

                  if (format_flags[index] & AFF_MULTI_LINE)
                     rsprintf("<br>");
               }

               rsprintf("</td>\n");

               for (i = 0; i < n; i++)
                  xfree(user_list[i]);
               xfree(user_list);

            } else if (attr_flags[index] & AF_MUSEREMAIL) {

               /* display multiple check boxes with user emails */
               rsprintf("<td%s class=\"%s\">\n", title, class_value);

               n_moptions = strbreak(attrib[index], attr_moptions, MAX_N_LIST, "|", FALSE);

               /* allocate list of users and populate it */
               for (n = 0;; n++) {
                  if (!enum_user_line(lbs, n, login_name, sizeof(login_name)))
                     break;
               }

               user_list = xcalloc(sizeof(char *), n);
               for (i = 0; i < n; i++)
                  user_list[i] = xcalloc(NAME_LENGTH, 1);

               for (i = 0; i < n; i++) {
                  enum_user_line(lbs, i, str, NAME_LENGTH);
                  get_user_line(lbs, str, NULL, NULL, user_list[i], NULL, NULL);
               }

               /* sort list */
               qsort(user_list, n, sizeof(char *), ascii_compare);

               for (i = 0; i < n; i++) {
                  sprintf(str, "%s_%d", ua, i);

                  rsprintf("<span style=\"white-space:nowrap;\">\n");

                  for (j = 0; j < n_moptions; j++)
                     if (strcmp(attr_moptions[j], user_list[i]) == 0 ||
                         strcmp(attr_moptions[j] + 7, user_list[i]) == 0)
                        break;

                  if (j < n_moptions)
                     rsprintf
                         ("<input type=checkbox id=\"%s\" name=\"%s\" value=\"%s\" checked onChange=\"mod();\">\n",
                          str, str, user_list[i]);
                  else
                     rsprintf
                         ("<input type=checkbox id=\"%s\" name=\"%s\" value=\"%s\" onChange=\"mod();\">\n",
                          str, str, user_list[i]);

                  rsprintf("<label for=\"%s\">%s</label>\n", str, user_list[i]);
                  rsprintf("</span>\n");

                  if (format_flags[index] & AFF_MULTI_LINE)
                     rsprintf("<br>");
               }

               rsprintf("</td>\n");

               for (i = 0; i < n; i++)
                  xfree(user_list[i]);
               xfree(user_list);

            } else if (attr_flags[index] & AF_USEREMAIL) {

               rsprintf("<td%s class=\"%s\">\n", title, class_value);

               /* display drop-down box with list of users */
               rsprintf("<select name=\"%s\"", ua);
               rsprintf(" onChange=\"mod();\">\n");

               /* display emtpy option */
               sprintf(str, "- %s -", loc("keep original values"));
               if (strcmp(str, attrib[index]) == 0 && isparam("nsel"))
                  rsprintf("<option value=\"<keep>\">%s\n", str);
               else
                  rsprintf("<option value=\"\">- %s -\n", loc("please select"));

               for (i = 0;; i++) {
                  if (!enum_user_line(lbs, i, login_name, sizeof(login_name)))
                     break;
                  get_user_line(lbs, login_name, NULL, NULL, str, NULL, NULL);

                  if (strieq(str, attrib[index]) || strieq(str, attrib[index] + 7))
                     rsprintf("<option selected value=\"%s\">%s\n", str, str);
                  else
                     rsprintf("<option value=\"%s\">%s\n", str, str);
               }

               rsprintf("</select>\n");

               rsprintf("</td>\n");

            } else {

               /* show normal edit field */
               rsprintf("<td%s class=\"%s\">", title, class_value);

               strencode2(str, attrib[index], sizeof(str));
               rsprintf
                   ("<input type=\"text\" size=%d maxlength=%d name=\"%s\" value=\"%s\" onChange=\"mod();\">\n",
                    input_size, input_maxlen, ua, str);

               rsprintf("</td>\n");
            }

         } else {
            if (strieq(attr_options[index][0], "boolean")) {
               /* display three radio buttons instead of three-state check box for multi-edit */
               sprintf(str, "- %s -", loc("keep original values"));
               if (isparam("nsel") && strieq(attrib[index], str)) {
                  rsprintf("<td%s class=\"%s\">", title, class_value);
                  sprintf(str, "%s_0", ua);
                  rsprintf("<span style=\"white-space:nowrap;\">\n");
                  rsprintf("<input type=radio id=\"%s\" name=\"%s\" value=\"0\" onChange=\"mod();\">\n", str,
                           ua);
                  rsprintf("<label for=\"%s\">0</label>\n", str);
                  rsprintf("</span>\n");

                  sprintf(str, "%s_1", ua);
                  rsprintf("<span style=\"white-space:nowrap;\">\n");
                  rsprintf("<input type=radio id=\"%s\" name=\"%s\" value=\"1\" onChange=\"mod();\">\n", str,
                           ua);
                  rsprintf("<label for=\"%s\">1</label>\n", str);
                  rsprintf("</span>\n");

                  sprintf(str, "%s_2", ua);
                  rsprintf("<span style=\"white-space:nowrap;\">\n");
                  rsprintf
                      ("<input type=radio id=\"%s\" name=\"%s\" value=\"<keep>\" checked onChange=\"mod();\">\n",
                       str, ua);
                  rsprintf("<label for=\"%s\">%s</label>\n", str, loc("keep original values"));
                  rsprintf("</span>\n");
               }

               /* display checkbox */
               else if (atoi(attrib[index]) == 1)
                  rsprintf
                      ("<td%s class=\"%s\"><input type=checkbox checked name=\"%s\" value=1 onChange=\"mod();\">\n",
                       title, class_value, ua);
               else
                  rsprintf
                      ("<td%s class=\"%s\"><input type=checkbox name=\"%s\" value=1 onChange=\"mod();\">\n",
                       title, class_value, ua);
            } else {

               sprintf(str, "extend_%d", index);
               if (isparam(str)) {

                  rsprintf("<td%s class=\"%s\">\n", title, class_value);
                  rsprintf("<i>");
                  rsprintf(loc("Add new option here"), attr_list[index]);
                  rsprintf("&nbsp;:&nbsp;</i>\n");

                  if (attr_flags[index] & (AF_MULTI | AF_MUSERLIST | AF_MUSEREMAIL))
                     rsprintf
                         ("<input type=\"text\" size=20 maxlength=%d name=\"%s_0\" value=\"%s\" onChange=\"mod();\">\n",
                          input_maxlen, ua, attrib[index]);
                  else
                     rsprintf
                         ("<input type=\"text\" size=20 maxlength=%d name=\"%s\" value=\"%s\" onChange=\"mod();\">\n",
                          input_maxlen, ua, attrib[index]);

                  rsprintf("<input type=\"hidden\" name=\"extend_%d\" value=\"1\">\n", index);
                  rsprintf("</td>\n");

               } else if (attr_flags[index] & AF_MULTI) {

                  /* display multiple check boxes */
                  rsprintf("<td%s class=\"%s\">\n", title, class_value);

                  sprintf(str, "- %s -", loc("keep original values"));
                  if (isparam("nsel") && strieq(attrib[index], str)) {
                     rsprintf("<span style=\"white-space:nowrap;\">\n");
                     sprintf(str, "%s_keep", ua);
                     rsprintf
                         ("<input type=checkbox id=\"%s\" name=\"%s\" value=\"<keep>\" checked onChange=\"mod();\">\n",
                          str, ua);

                     rsprintf("<label for=\"%s\">%s</label>\n", str, loc("keep original values"));
                     rsprintf("</span>\n");
                  }

                  n_moptions = strbreak(attrib[index], attr_moptions, MAX_N_LIST, "|", FALSE);
                  for (i = 0; i < MAX_N_LIST && attr_options[index][i][0]; i++) {

                     /* display check box with optional tooltip */
                     sprintf(str, "Tooltip %s", attr_options[index][i]);
                     tooltip[0] = 0;
                     if (getcfg(lbs->name, str, comment, sizeof(comment)))
                        sprintf(tooltip, " title=\"%s\"", comment);

                     sprintf(str, "%s_%d", ua, i);
                     rsprintf("<span%s style=\"white-space:nowrap;\">\n", tooltip);

                     for (j = 0; j < n_moptions; j++)
                        if (strcmp(attr_moptions[j], attr_options[index][i]) == 0)
                           break;

                     if (j < n_moptions)
                        rsprintf
                            ("<input type=checkbox id=\"%s\" name=\"%s\" value=\"%s\" checked onChange=\"mod();\">\n",
                             str, str, attr_options[index][i]);
                     else
                        rsprintf
                            ("<input type=checkbox id=\"%s\" name=\"%s\" value=\"%s\" onChange=\"mod();\">\n",
                             str, str, attr_options[index][i]);

                     rsprintf("<label for=\"%s\">%s</label>\n", str, attr_options[index][i]);
                     rsprintf("</span>\n");

                     if (format_flags[index] & AFF_MULTI_LINE)
                        rsprintf("<br>");
                  }

                  if (attr_flags[index] & AF_EXTENDABLE) {
                     sprintf(str, loc("Add %s"), attr_list[index]);
                     rsprintf
                         ("<input type=submit name=\"extend_%d\" value=\"%s\" onClick=\"return mark_submit();\">\n",
                          index, str);
                  }

                  rsprintf("</td>\n");

               } else if (attr_flags[index] & AF_RADIO) {
                  /* display radio buttons */
                  rsprintf("<td%s class=\"%s\">\n", title, class_value);

                  for (i = 0; i < MAX_N_LIST && attr_options[index][i][0]; i++) {
                     /* display check box with optional tooltip */
                     sprintf(str, "Tooltip %s", attr_options[index][i]);
                     tooltip[0] = 0;
                     if (getcfg(lbs->name, str, comment, sizeof(comment)))
                        sprintf(tooltip, " title=\"%s\"", comment);

                     rsprintf("<span%s style=\"white-space:nowrap;\">\n", tooltip);

                     strencode2(str, attr_options[index][i], sizeof(str));
                     if (strchr(str, '{'))
                        *strchr(str, '{') = 0;
                     strencode2(enc_attr, attrib[index], sizeof(enc_attr));

                     if (strstr(attrib[index], attr_options[index][i]) || strieq(str, enc_attr))
                        rsprintf
                            ("<input type=radio id=\"%s\" name=\"%s\" value=\"%s\" checked onChange=\"mod();\">\n",
                             str, ua, str);
                     else
                        rsprintf
                            ("<input type=radio id=\"%s\" name=\"%s\" value=\"%s\" onChange=\"mod();\">\n",
                             str, ua, str);

                     rsprintf("<label for=\"%s\">%s</label>\n", str, str);

                     rsprintf("</span>\n");

                     if (format_flags[index] & AFF_MULTI_LINE)
                        rsprintf("<br>");
                  }

                  if (attr_flags[index] & AF_EXTENDABLE) {
                     sprintf(str, loc("Add %s"), attr_list[index]);
                     rsprintf
                         ("<input type=submit name=\"extend_%d\" value=\"%s\" onClick=\"return mark_submit();\">\n",
                          index, str);
                  }

                  rsprintf("</td>\n");

               } else if (attr_flags[index] & AF_ICON) {
                  /* display icons */
                  rsprintf("<td%s class=\"%s\">\n", title, class_value);
                  rsprintf("<table cellpadding=\"0\" cellspacing=\"0\"><tr>\n");

                  for (i = 0; i < MAX_N_LIST && attr_options[index][i][0]; i++) {
                     if (strstr(attrib[index], attr_options[index][i]))
                        rsprintf
                            ("<td><input type=radio checked name=\"%s\" value=\"%s\" onChange=\"mod();\">",
                             ua, attr_options[index][i]);
                     else
                        rsprintf("<td><input type=radio name=\"%s\" value=\"%s\" onChange=\"mod();\">", ua,
                                 attr_options[index][i]);

                     sprintf(str, "Icon comment %s", attr_options[index][i]);
                     getcfg(lbs->name, str, comment, sizeof(comment));

                     if (comment[0])
                        rsprintf("<img src=\"icons/%s\" alt=\"%s\" title=\"%s\">\n", attr_options[index][i],
                                 comment, comment);
                     else
                        rsprintf("<img src=\"icons/%s\" alt=\"%s\" title=\"%s\">\n", attr_options[index][i],
                                 attr_options[index][i], attr_options[index][i]);

                     rsprintf("</td>\n");
                     if ((format_flags[index] & AFF_MULTI_LINE) && attr_options[index][i + 1][0]) {
                        rsprintf("</tr><tr>\n");
                     }
                  }

                  rsprintf("</tr></table></td>\n");

               } else {

                  rsprintf("<td%s class=\"%s\">\n", title, class_value);

                  /* display drop-down box */
                  rsprintf("<select name=\"%s\"", ua);

                  if (is_cond_attr(index))
                     rsprintf(" onChange=\"cond_submit()\">\n");
                  else
                     rsprintf(" onChange=\"mod();\">\n");

                  /* display emtpy option */
                  sprintf(str, "- %s -", loc("keep original values"));
                  if (strcmp(str, attrib[index]) == 0 && isparam("nsel"))
                     rsprintf("<option value=\"<keep>\">%s\n", str);
                  else
                     rsprintf("<option value=\"\">- %s -\n", loc("please select"));

                  for (i = 0; i < MAX_N_LIST && attr_options[index][i][0]; i++) {
                     strencode2(str, attr_options[index][i], sizeof(str));
                     if (strchr(str, '{'))
                        *strchr(str, '{') = 0;
                     strencode2(enc_attr, attrib[index], sizeof(enc_attr));

                     if (strieq(attr_options[index][i], attrib[index]) || strieq(str, enc_attr))
                        rsprintf("<option selected value=\"%s\">%s\n", str, str);
                     else
                        rsprintf("<option value=\"%s\">%s\n", str, str);
                  }

                  rsprintf("</select>\n");

                  if (is_cond_attr(index)) {
                     /* show "update" button only of javascript is not enabled */
                     rsprintf("<noscript>\n");
                     rsprintf("<input type=submit value=\"%s\">\n", loc("Update"));
                     rsprintf("</noscript>\n");
                  }

                  if (attr_flags[index] & AF_EXTENDABLE) {
                     sprintf(str, loc("Add %s"), attr_list[index]);
                     rsprintf
                         ("<input type=submit name=\"extend_%d\" value=\"%s\" onClick=\"return mark_submit();\">\n",
                          index, str);
                  }

                  rsprintf("</td>\n");
               }
            }
         }
      }

      if (aindex < n_disp_attr - 1 && (format_flags[attr_index[aindex + 1]] & AFF_SAME_LINE) == 0) {
         /* if next attribute not on same line, close row or subtable */
         if (subtable) {
            rsprintf("</table></td></tr>\n");
            subtable = 0;
         } else
            rsprintf("</tr>");
      }

      /* if last attribute, close row or subtable */
      if (aindex == n_disp_attr - 1) {
         if (subtable) {
            rsprintf("</table></td></tr>\n");
            subtable = 0;
         } else
            rsprintf("</tr>");
      }
   }

   if (bpreview) {
      _current_message_id = message_id;
      rsprintf("<tr><td colspan=2 class=\"messageframe\">\n");

      if (strieq(encoding, "plain")) {
         rsputs("<pre class=\"messagepre\">");
         rsputs2(lbs, FALSE, text);
         rsputs("</pre>");
      } else if (strieq(encoding, "ELCode"))
         rsputs_elcode(lbs, FALSE, text);
      else
         rsputs(text);

      rsprintf("</td></tr>\n");
   }

   if (enc_selected == 0 && show_text) {
      rsprintf("<tr><td colspan=2 class=\"toolframe\">\n");

      ricon("bold", loc("Bold text CTRL+B"), "elcode(document.form1.Text, 'B','')");
      ricon("italic", loc("Italics text CTRL+I"), "elcode(document.form1.Text, 'I','')");
      ricon("underline", loc("Underlined text CTRL+U"), "elcode(document.form1.Text, 'U','')");

      rsprintf(" ");
      ricon("center", loc("Centered text"), "elcode(document.form1.Text, 'CENTER','')");

      rsprintf(" ");
      ricon("url", loc("Insert hyperlink"), "queryURL(document.form1.Text)");
      ricon("email", loc("Insert email"), "elcode(document.form1.Text, 'EMAIL','')");

      sprintf(str, "window.open('upload.html', '',");
      strlcat(str, "'top=280,left=350,width=500,height=120,dependent=yes,", sizeof(str));
      strlcat(str, "menubar=no,status=no,scrollbars=no,location=no,resizable=yes')", sizeof(str));
      ricon("image", loc("Insert image CTRL+M"), str);

      rsprintf(" ");
      ricon("quote", loc("Insert quote"), "elcode(document.form1.Text, 'QUOTE','')");
      ricon("list", loc("Insert list CTRL+L"), "elcode(document.form1.Text, 'LIST','')");
      ricon("table", loc("Insert table"), "elcode(document.form1.Text, 'TABLE','')");
      ricon("heading", loc("Insert heading CTRL+H"), "queryHeading(document.form1.Text)");
      ricon("line", loc("Insert horizontal line"), "elcode(document.form1.Text, 'LINE','')");
      ricon("anchor", loc("Insert anchor point"), "elcode(document.form1.Text, 'ANCHOR','')");

      rsprintf(" ");
      ricon("code", loc("Insert code CTRL+O"), "elcode(document.form1.Text, 'CODE','')");

      if (show_smileys)
         rsprintf
             (" <img align=\"middle\" name=\"smileys\" src=\"icons/eld_smile.png\" alt=\"%s\" title=\"%s\" border=\"0\"",
              loc("Hide the smiley bar"), loc("Hide the smiley bar"));
      else
         rsprintf
             (" <img align=\"middle\" name=\"smileys\" src=\"icons/elc_smile.png\" alt=\"%s\" title=\"%s\" border=\"0\"",
              loc("Show the smiley bar"), loc("Show the smiley bar"));
      rsprintf(" onclick=\"switch_smileys()\"");
      rsprintf(" onmouseover=\"this.style.cursor='pointer';\" />\n");

      rsprintf(" <select name=\"font\" ");
      rsprintf("onchange=\"elcode(document.form1.Text,'FONT',this.options[this.selectedIndex].value);");
      rsprintf("this.selectedIndex=0;\">\n");
      rsprintf("<option value=\"0\">%s</option>\n", loc("FONT"));

      if (!getcfg(lbs->name, "Fonts", str, sizeof(str)))
         strcpy(str, "Arial, Comic Sans MS, Courier New, Tahoma, Times New Roman, Verdana");

      n = strbreak(str, list, MAX_N_ATTR, ",", FALSE);
      for (i = 0; i < n; i++)
         rsprintf("<option value=\"%s\">%s</option>\n", list[i], list[i]);

      rsprintf("</select>\n");

      rsprintf(" <select name=\"size\" ");
      rsprintf("onchange=\"elcode(document.form1.Text,'SIZE',this.options[this.selectedIndex].value);");
      rsprintf("this.selectedIndex=0;\">\n");
      rsprintf("<option value=\"0\">%s</option>\n", loc("SIZE"));
      rsprintf("<option value=\"1\">1</option>\n");
      rsprintf("<option value=\"2\">2</option>\n");
      rsprintf("<option value=\"3\">3</option>\n");
      rsprintf("<option value=\"4\">4</option>\n");
      rsprintf("<option value=\"5\">5</option>\n");
      rsprintf("<option value=\"6\">6</option>\n");
      rsprintf("</select>\n");

      rsprintf(" <select name=\"color\" ");
      rsprintf("onchange=\"elcode(document.form1.Text,'COLOR',this.options[this.selectedIndex].value);");
      rsprintf("this.selectedIndex=0;\">\n");
      rsprintf("<option value=\"0\">%s</option>\n", loc("COLOR"));
      rsprintf("<option value=\"blue\" style=\"color:blue\">blue</option>\n");
      rsprintf("<option value=\"darkblue\" style=\"color:darkblue\">dark-blue</option>\n");
      rsprintf("<option value=\"orange\" style=\"color:orange\">orange</option>\n");
      rsprintf("<option value=\"red\" style=\"color:red\">red</option>\n");
      rsprintf("<option value=\"darkred\" style=\"color:darkred\">dark red</option>\n");
      rsprintf("<option value=\"green\" style=\"color:green\">green</option>\n");
      rsprintf("<option value=\"darkgreen\" style=\"color:darkgreen\">dark-green</option>\n");
      rsprintf("<option value=\"pink\" style=\"color:deeppink\">pink</option>\n");
      rsprintf("<option value=\"purple\" style=\"color:purple\">purple</option>\n");
      rsprintf("<option value=\"chocolate\" style=\"color:chocolate\">chocolate</option>\n");
      rsprintf("</select>");

      rsprintf(" ");
      ricon("clock", loc("Insert current time/date"), "insertTime(document.form1.Text)");

      rsprintf("</td></tr>\n");
   }

   /* main box for text box and icons */
   rsprintf("<tr><td width=\"100%%\" colspan=2 class=\"attribvalue\">\n");
   if (enc_selected == 0)
      rsprintf("<table width=\"100%%\" border=\"0\"><tr>\n");

   if (enc_selected == 0 && show_smileys) {

      rsprintf("<td class=\"menuframe\">\n");
      rsicon("smile", loc("smiling"), ":)");
      rsprintf("<br />\n");
      rsicon("happy", loc("happy"), ":))");
      rsprintf("<br />\n");
      rsicon("wink", loc("winking"), ";)");
      rsprintf("<br />\n");
      rsicon("biggrin", loc("big grin"), ":D");
      rsprintf("<br />\n");
      rsicon("crying", loc("crying"), ";(");
      rsprintf("<br />\n");
      rsicon("cool", loc("cool"), "8-)");
      rsprintf("<br />\n");
      rsicon("frown", loc("frowning"), ":(");
      rsprintf("<br />\n");
      rsicon("confused", loc("confused"), "?-)");
      rsprintf("<br />\n");
      rsicon("astonished", loc("astonished"), "8o");
      rsprintf("<br />\n");
      rsicon("mad", loc("mad"), "X-(");
      rsprintf("<br />\n");
      rsicon("pleased", loc("pleased"), ":]");
      rsprintf("<br />\n");
      rsicon("tongue", loc("tongue"), ":P");
      rsprintf("<br />\n");
      rsicon("yawn", loc("yawn"), ":O");

      rsprintf("</td>\n");
   }

   if (enc_selected == 0)
      rsprintf("<td width=\"100%%\" class=\"attribvalue\">\n");

   /* set textarea width */
   width = 112;

   if (getcfg(lbs->name, "Message width", str, sizeof(str)))
      width = atoi(str);

   /* increased width according to longest line */
   if (message_id && enc_selected == 1) {
      p = text;
      do {
         pend = strchr(p, '\n');
         if (pend == NULL)
            pend = p + strlen(p);

         if (pend - p + 1 > width)
            width = pend - p + 1;

         if (*pend == 0)
            break;

         p = pend;
         while (*p && (*p == '\r' || *p == '\n'))
            p++;
      } while (1);

      /* leave space for '> ' */
      if (!bedit && !bduplicate)
         width += 2;
   }

   /* set textarea height */
   height = 20;
   if (getcfg(lbs->name, "Message height", str, sizeof(str)))
      height = atoi(str);

   if (breply)
      /* hidden text for original message */
      rsprintf("<input type=hidden name=reply_to value=\"%d\">\n", message_id);

   if (breedit || bupload)
      /* hidden text for original message */
      if (isparam("reply_to"))
         rsprintf("<input type=hidden name=reply_to value=\"%s\">\n", getparam("reply_to"));

   if (bedit && message_id)
      rsprintf("<input type=hidden name=edit_id value=\"%d\">\n", message_id);

   if (isparam("nsel")) {
      rsprintf("<input type=hidden name=nsel value=\"%s\">\n", getparam("nsel"));
      for (i = 0; i < atoi(getparam("nsel")); i++) {
         sprintf(str, "s%d", i);
         if (isparam(str)) {
            rsprintf("<input type=hidden name=\"s%d\" value=\"%s\">\n", i, getparam(str));
         }
      }
   }

   if (getcfg(lbs->name, "Message comment", comment, sizeof(comment)) && !message_id) {
      rsputs(comment);
      rsputs("<br>\n");
   }

   if (getcfg(lbs->name, "Reply comment", comment, sizeof(comment)) && breply) {
      rsputs(comment);
      rsputs("<br>\n");
   }

   preset_text = getcfg(lbs->name, "Preset text", str, sizeof(str));
   if (preset_text) {

      /* don't use preset text if editing or replying */
      if (bedit || bduplicate || breply)
         preset_text = FALSE;

      /* user preset on reedit only if preset is under condition */
      if (breedit && !bpreview && !bupload && getcfg(lbs->name, "Preset text", str, sizeof(str)) == 2)
         preset_text = TRUE;
   }

   if (show_text) {

      if (getcfg(lbs->name, "Fix text", str, sizeof(str)) && atoi(str) == 1)
         strcpy(str, " readonly");
      else
         strcpy(str, "");

      if (enc_selected == 1)
         /* use hard wrapping only for plain text */
         rsprintf("<textarea rows=%d cols=%d wrap=hard %s name=\"Text\" onChange=\"mod();\">\n", height,
                  width, str);
      else
         rsprintf("<textarea rows=%d cols=%d %s name=\"Text\" onChange=\"mod();\" style=\"width:100%%;\">\n",
                  height, width, str);

      if (isparam("nsel")) {
         rsprintf("- %s -\n", loc("keep original text"));
      } else if (bedit) {
         if (!preset_text) {

            j = build_subst_list(lbs, slist, svalue, attrib, TRUE);
            sprintf(mid, "%d", message_id);
            add_subst_list(slist, svalue, "message id", mid, &j);
            add_subst_time(lbs, slist, svalue, "entry time", date, &j, 0);

            if (!bupload)
               if (getcfg(lbs->name, "Prepend on edit", str, sizeof(str))) {
                  strsubst_list(str, sizeof(str), slist, svalue, j);
                  while (strstr(str, "\\n"))
                     memcpy(strstr(str, "\\n"), "\r\n", 2);
                  rsputs3(str);
               }

            /* use rsputs3 which just converts "<", ">", "&", "\"" to &gt; etc. */
            /* otherwise some HTML statments would break the page syntax        */
            rsputs3(text);

            if (!bupload)
               if (getcfg(lbs->name, "Append on edit", str, sizeof(str))) {
                  strsubst_list(str, sizeof(str), slist, svalue, j);
                  while (strstr(str, "\\n"))
                     memcpy(strstr(str, "\\n"), "\r\n", 2);
                  rsputs3(str);
               }
         }
      } else if (bduplicate) {

         rsputs3(text);

      } else if (breply) {
         if (!getcfg(lbs->name, "Quote on reply", str, sizeof(str)) || atoi(str) > 0) {
            if (getcfg(lbs->name, "Prepend on reply", str, sizeof(str))) {
               j = build_subst_list(lbs, slist, svalue, attrib, TRUE);
               sprintf(mid, "%d", message_id);
               add_subst_list(slist, svalue, "message id", mid, &j);
               add_subst_time(lbs, slist, svalue, "entry time", date, &j, 0);

               strsubst_list(str, sizeof(str), slist, svalue, j);
               while (strstr(str, "\\n"))
                  memcpy(strstr(str, "\\n"), "\r\n", 2);
               rsputs3(str);
            }

            p = text;

            if (text[0]) {
               if (!getcfg(lbs->name, "Reply string", reply_string, sizeof(reply_string)))
                  strcpy(reply_string, "> ");

               if (enc_selected == 0) {

                  /* check for author */
                  if (orig_author[0])
                     rsprintf("[quote=\"%s\"]", orig_author);
                  else
                     rsprintf("[quote]");
                  rsputs3(text);
                  rsprintf("[/quote]\r\n");

               } else if (enc_selected == 2) {

                  rsprintf("<p>\n");
                  rsprintf
                      ("<table width=\"98%%\" align=\"center\" cellspacing=\"1\" style=\"border:1px solid #486090;\">\n");
                  rsprintf("<tbody>\n");
                  rsprintf("<tr>\n");
                  rsprintf
                      ("<td cellpadding=\"3px\" style=\"background-color:#486090; font-weidht:bold; color:white;\">");

                  /* check for author */
                  if (orig_author[0]) {
                     rsprintf(loc("%s wrote"), orig_author);
                  } else {
                     rsprintf(loc("Quote"));
                  }
                  rsprintf(":</td></tr>\n");
                  rsprintf("<tr>\n");
                  rsprintf("<td cellpadding=\"10px\" style=\"background-color:#FFFFB0;\">");
                  rsputs3(text);
                  rsprintf("</td>\n");
                  rsprintf("</tr>\n");
                  rsprintf("</tbody>\n");
                  rsprintf("</table>\n");
                  rsprintf("</p><p>&nbsp;</p>\n");

               } else {
                  do {
                     if (strchr(p, '\n')) {
                        *strchr(p, '\n') = 0;

                        if (encoding[0] == 'H') {
                           rsputs3(reply_string);
                           rsprintf("%s<br>\n", p);
                        } else {
                           rsputs(reply_string);
                           rsputs3(p);
                           rsprintf("\n");
                        }

                        p += strlen(p) + 1;
                        if (*p == '\n')
                           p++;
                     } else {
                        if (encoding[0] == 'H') {
                           rsputs3(reply_string);
                           rsprintf("%s<p>\n", p);
                        } else {
                           rsputs(reply_string);
                           rsputs3(p);
                           rsprintf("\n\n");
                        }

                        break;
                     }

                  } while (TRUE);
               }
            }

            if (getcfg(lbs->name, "Append on reply", str, sizeof(str))) {

               j = build_subst_list(lbs, slist, svalue, attrib, TRUE);
               sprintf(mid, "%d", message_id);
               add_subst_list(slist, svalue, "message id", mid, &j);
               add_subst_time(lbs, slist, svalue, "entry time", date, &j, 0);
               strsubst_list(str, sizeof(str), slist, svalue, j);
               while (strstr(str, "\\n"))
                  memcpy(strstr(str, "\\n"), "\r\n", 2);
               rsputs3(str);
            }
         }
      }

      if (preset_text && !isparam("nsel")) {
         getcfg(lbs->name, "Preset text", str, sizeof(str));

         /* check if file starts with an absolute directory */
         if (str[0] == DIR_SEPARATOR || str[1] == ':')
            strcpy(file_name, str);
         else {
            strlcpy(file_name, logbook_dir, sizeof(file_name));
            strlcat(file_name, str, sizeof(file_name));
         }

         /* check if file exists */
         fh = open(file_name, O_RDONLY | O_BINARY);
         if (fh > 0) {
            length = lseek(fh, 0, SEEK_END);
            lseek(fh, 0, SEEK_SET);
            buffer = xmalloc(length + 1);
            read(fh, buffer, length);
            buffer[length] = 0;
            close(fh);
            rsputs3(buffer);
            xfree(buffer);
         } else {
            j = build_subst_list(lbs, slist, svalue, attrib, TRUE);
            strsubst_list(str, sizeof(str), slist, svalue, j);
            while (strstr(str, "\\n"))
               memcpy(strstr(str, "\\n"), "\r\n", 2);
            rsputs3(str);
         }
      }

      rsprintf("</textarea><br>\n");

      /* Encoding radio buttons */

      if (allowed_encoding < 1 || allowed_encoding > 7) {
         rsprintf
             ("<h1>Invalid \"Allowed encoding\" in configuration file, value must be between 1 and 7</h1>\n");
         rsprintf("</table><!-- show_standard_title -->\n");
         show_bottom_text(lbs);
         rsprintf("</form></body></html>\r\n");
         return;
      }

      if (allowed_encoding == 1)
         rsprintf("<input type=\"hidden\" name=\"encoding\" value=\"plain\">\n");
      else if (allowed_encoding == 2)
         rsprintf("<input type=\"hidden\" name=\"encoding\" value=\"ELCode\">\n");
      else if (allowed_encoding == 4)
         rsprintf("<input type=\"hidden\" name=\"encoding\" value=\"HTML\">\n");
      else {
         if (allowed_encoding == 4)
            rsprintf("<input type=hidden name=\"encoding\" value=\"HTML\">\n");
         else if (allowed_encoding == 2)
            rsprintf("<input type=hidden name=\"encoding\" value=\"ELCode\">\n");
         else if (allowed_encoding == 1)
            rsprintf("<input type=hidden name=\"encoding\" value=\"plain\">\n");
         else {
            rsprintf("<b>%s</b>: ", loc("Encoding"));

            if (allowed_encoding & 4) {
               if (enc_selected == 2)
                  rsprintf
                      ("<input type=radio id=\"HTML\" name=\"encoding\" value=\"HTML\" checked=\"checked\">");
               else
                  rsprintf
                      ("<input type=radio id=\"HTML\" name=\"encoding\" value=\"HTML\" onclick=\"cond_submit()\">");
               rsprintf("<label for=\"HTML\">HTML&nbsp;&nbsp;</label>\n");
            }

            if (allowed_encoding & 2) {
               if (enc_selected == 0)
                  rsprintf("<input type=radio id=\"ELCode\" name=\"encoding\" value=\"ELCode\" checked>");
               else
                  rsprintf
                      ("<input type=radio id=\"ELCode\" name=\"encoding\" value=\"ELCode\" onclick=\"cond_submit()\">");
               rsprintf
                   ("<label for=\"ELCode\"><a target=\"_blank\" href=\"?cmd=HelpELCode\">ELCode</a>&nbsp;&nbsp;</label>\n");
            }

            if (allowed_encoding & 1) {
               if (enc_selected == 1)
                  rsprintf("<input type=radio id=\"plain\" name=\"encoding\" value=\"plain\" checked>");
               else
                  rsprintf
                      ("<input type=radio id=\"plain\" name=\"encoding\" value=\"plain\" onclick=\"cond_submit()\">");
               rsprintf("<label for=\"plain\">plain&nbsp;&nbsp;</label>\n");
            }
         }
      }

      rsprintf("<br>\n");
   }

   /* Suppress email check box */
   if (message_id && bedit)
      getcfg(lbs->name, "Suppress Email on edit", str, sizeof(str));
   else
      getcfg(lbs->name, "Suppress default", str, sizeof(str));

   if (atoi(str) == 0) {
      rsprintf("<input type=\"checkbox\" name=\"suppress\" id=\"suppress\" value=\"1\">");
      rsprintf("<label for=\"suppress\">%s</label>\n", loc("Suppress Email notification"));
   } else if (atoi(str) == 1) {
      rsprintf("<input type=\"checkbox\" checked name=\"suppress\" id=\"suppress\" value=\"1\">");
      rsprintf("<label for=\"suppress\">%s</label>\n", loc("Suppress Email notification"));
   } else if (atoi(str) == 2) {
      rsprintf("<input type=\"hidden\" name=\"suppress\" id=\"suppress\" value=\"2\">");
   } else if (atoi(str) == 3) {
      rsprintf("<input type=\"hidden\" name=\"suppress\" id=\"suppress\" value=\"3\">");
   }

   /* Suppress execute shell check box */
   if (!bedit && getcfg(lbs->name, "Execute new", str, sizeof(str))) {
      if (getcfg(lbs->name, "Suppress execute default", str, sizeof(str))) {
         if (atoi(str) == 0) {
            rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;\n");
            rsprintf("<input type=\"checkbox\" name=\"shell_suppress\" id=\"shell_suppress\" value=1>");
            rsprintf("<label for=\"shell_suppress\">%s</label>\n", loc("Suppress shell execution"));
         } else if (atoi(str) == 1) {
            rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;\n");
            rsprintf
                ("<input type=\"checkbox\" checked name=\"shell_suppress\" id=\"shell_suppress\" value=1>");
            rsprintf("<label for=\"shell_suppress\">%s</label>\n", loc("Suppress shell execution"));
         }
      } else {
         rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;\n");
         rsprintf("<input type=\"checkbox\" name=\"shell_suppress\" id=\"shell_suppress\" value=1>");
         rsprintf("<label for=\"shell_suppress\">%s</label>\n", loc("Suppress shell execution"));
      }
   }

   if (bedit && getcfg(lbs->name, "Execute edit", str, sizeof(str))) {
      if (getcfg(lbs->name, "Suppress execute default", str, sizeof(str))) {
         if (atoi(str) == 0) {
            rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;\n");
            rsprintf("<input type=\"checkbox\" name=\"shell_suppress\" id=\"shell_suppress\" value=1>");
            rsprintf("<label for=\"shell_suppress\">%s</label>\n", loc("Suppress shell execution"));
         } else if (atoi(str) == 1) {
            rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;\n");
            rsprintf
                ("<input type=\"checkbox\" checked name=\"shell_suppress\" id=\"shell_suppress\" value=1>");
            rsprintf("<label for=\"shell_suppress\">%s</label>\n", loc("Suppress shell execution"));
         }
      } else {
         rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;\n");
         rsprintf("<input type=\"checkbox\" name=\"shell_suppress\" id=\"shell_suppress\" value=1>");
         rsprintf("<label for=\"shell_suppress\">%s</label>\n", loc("Suppress shell execution"));
      }
   }

   /* Resubmit check box */
   if (bedit && message_id) {
      if (getcfg(lbs->name, "Resubmit default", str, sizeof(str))) {
         if (atoi(str) == 0) {
            rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;\n");
            rsprintf("<input type=\"checkbox\" name=\"resubmit\" id=\"resubmit\" value=1>");
            rsprintf("<label for=\"resubmit\">%s</lable>\n", loc("Resubmit as new entry"));
         } else if (atoi(str) == 1) {
            rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;\n");
            rsprintf("<input type=\"checkbox\" checked name=\"resubmit\" id=\"resubmit\" value=1>");
            rsprintf("<label for=\"resubmit\">%s</lable>\n", loc("Resubmit as new entry"));
         }
      } else {
         rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;\n");
         rsprintf("<input type=\"checkbox\" name=\"resubmit\" id=\"resubmit\" value=1>");
         rsprintf("<label for=\"resubmit\">%s</lable>\n", loc("Resubmit as new entry"));
      }
   }

   rsprintf("</tr>\n");

   if (enc_selected == 0)
      rsprintf("</table></td></tr>\n");

   for (i = 0; i < MAX_ATTACHMENTS; i++)
      if (!att[i][0]) {
         /* put first free attachment for show_uploader_finished() */
         rsprintf("<tr><td><input type=hidden name=\"next_attachment\" value=\"%d\"></td></tr>\n", i + 1);
         break;
      }

   index = 0;
   if (!getcfg(lbs->name, "Enable attachments", str, sizeof(str)) || atoi(str) > 0) {
      if (bedit || bduplicate || isparam("fa")) {
         /* show existing attachments */
         for (index = 0; index < MAX_ATTACHMENTS; index++)
            if (att[index][0]) {
               rsprintf("<tr><td nowrap class=\"attribname\">%s %d:</td>\n", loc("Attachment"), index + 1);
               sprintf(str, "attachment%d", index);
               rsprintf("<td class=\"attribvalue\">\n");
               rsprintf("<input type=hidden name=\"%s\" value=\"%s\">\n", str, att[index]);

               if (strlen(att[index]) < 14 || att[index][6] != '_' || att[index][13] != '_') {
                  rsprintf("<b>Error: Invalid attachment \"%s\"</b><br>", att);
               } else {

                  strlcpy(file_name, lbs->data_dir, sizeof(file_name));
                  strlcat(file_name, att[index], sizeof(file_name));

                  display_inline = is_image(file_name) || is_ascii(file_name);
                  if (chkext(file_name, ".ps") || chkext(file_name, ".pdf") || chkext(file_name, ".eps"))
                     display_inline = 0;
                  if ((chkext(file_name, ".htm") || chkext(file_name, ".html")) && is_full_html(file_name))
                     display_inline = 0;

                  thumb_status = create_thumbnail(lbs, file_name);
                  if (thumb_status)
                     display_inline = 1;

                  if (getcfg(lbs->name, "Preview attachments", str, sizeof(str)) && atoi(str) == 0)
                     display_inline = 0;

                  if (thumb_status && display_inline) {
                     get_thumb_name(file_name, thumb_name, sizeof(thumb_name), 0);
                     if (strrchr(thumb_name, DIR_SEPARATOR))
                        strlcpy(str, strrchr(thumb_name, DIR_SEPARATOR) + 1, sizeof(str));
                     else
                        strlcpy(str, thumb_name, sizeof(str));
                     strlcpy(thumb_name, str, sizeof(str));
                     if (thumb_status == 2)
                        strsubst(thumb_name, sizeof(thumb_name), "-0.png", "");

                     rsprintf("<table><tr><td class=\"toolframe\">\n");
                     sprintf(str, "im('att'+'%d','%s','%s','smaller');", index, thumb_name, att[index]);
                     ricon("smaller", loc("Make smaller"), str);
                     sprintf(str, "im('att'+'%d','%s','%s','original');", index, thumb_name, att[index]);
                     ricon("original", loc("Original size"), str);
                     sprintf(str, "im('att'+'%d','%s','%s','larger');", index, thumb_name, att[index]);
                     ricon("larger", loc("Make larger"), str);
                     rsprintf("&nbsp;\n");
                     sprintf(str, "im('att'+'%d','%s','%s','rotleft');", index, thumb_name, att[index]);
                     ricon("rotleft", loc("Rotate left"), str);
                     sprintf(str, "im('att'+'%d','%s','%s','rotright');", index, thumb_name, att[index]);
                     ricon("rotright", loc("Rotate right"), str);
                     rsprintf("&nbsp;\n");
                     sprintf(str, "deleteAtt('%d')", index);
                     ricon("delatt", loc("Delete attachment"), str);
                     rsprintf("&nbsp;&nbsp;\n");

                     /* ImageMagick available, so get image size */
                     rsprintf("<b>%s</b>&nbsp;\n", att[index] + 14);
                     sprintf(cmd, "identify -format '%%wx%%h' '%s'", file_name);
#ifdef OS_WINNT
                     for (i = 0; i < (int) strlen(cmd); i++)
                        if (cmd[i] == '\'')
                           cmd[i] = '\"';
#endif
                     my_shell(cmd, str, sizeof(str));
                     if (atoi(str) > 0)
                        rsprintf("<span class=\"bytes\">%s: %s</span>\n", loc("Original size"), str);

                     rsprintf("</td></tr>\n");
                     rsprintf("<tr><td align=center>");
                  } else {
                     rsprintf("%s\n", att[index] + 14);
                     rsprintf("&nbsp;&nbsp;<input type=\"submit\" name=\"delatt%d\" value=\"%s\" ", index,
                              loc("Delete"));
                     rsprintf("onClick=\"return mark_submit();\">");
                     rsprintf("<br>\n");
                  }

                  if (display_inline) {

                     if (is_image(att[index]) || thumb_status) {
                        if (thumb_status == 1) {
                           get_thumb_name(file_name, thumb_name, sizeof(thumb_name), 0);
                           strlcpy(str, att[index], sizeof(str));
                           str[13] = 0;
                           if (strrchr(thumb_name, DIR_SEPARATOR))
                              strlcpy(file_enc, strrchr(thumb_name, DIR_SEPARATOR) + 1 + 14,
                                      sizeof(file_enc));
                           else
                              strlcpy(file_enc, thumb_name + 14, sizeof(file_enc));
                           url_encode(file_enc, sizeof(file_enc));      /* for file names with special characters like "+" */
                           sprintf(ref, "%s/%s?thumb=1", str, file_enc);

                           rsprintf("<img src=\"%s\" alt=\"%s\" title=\"%s\" name=\"att%d\">\n", ref,
                                    att[index] + 14, att[index] + 14, index);
                        } else if (thumb_status == 2) {
                           for (i = 0;; i++) {
                              get_thumb_name(file_name, thumb_name, sizeof(thumb_name), i);
                              if (thumb_name[0]) {
                                 strlcpy(str, att[index], sizeof(str));
                                 str[13] = 0;
                                 if (strrchr(thumb_name, DIR_SEPARATOR))
                                    strlcpy(file_enc, strrchr(thumb_name, DIR_SEPARATOR) + 1 + 14,
                                            sizeof(file_enc));
                                 else
                                    strlcpy(file_enc, thumb_name + 14, sizeof(file_enc));
                                 url_encode(file_enc, sizeof(file_enc));        /* for file names with special characters like "+" */
                                 sprintf(ref, "%s/%s?thumb=1", str, file_enc);

                                 rsprintf("<img src=\"%s\" alt=\"%s\" title=\"%s\" name=\"att%d_%d\">\n",
                                          ref, att[index] + 14, att[index] + 14, index, i);
                              } else
                                 break;
                           }

                        } else {
                           strlcpy(str, att[index], sizeof(str));
                           str[13] = 0;
                           strcpy(file_enc, att[index] + 14);
                           url_encode(file_enc, sizeof(file_enc));      /* for file names with special characters like "+" */
                           sprintf(ref, "%s/%s", str, file_enc);

                           rsprintf("<img src=\"%s\" alt=\"%s\" title=\"%s\" name=\"att%d\">\n", ref,
                                    att[index] + 14, att[index] + 14, index);
                        }
                     } else {
                        if (is_ascii(file_name)) {
                           if (!chkext(att[index], ".HTML"))
                              rsprintf("<pre class=\"messagepre\">");

                           f = fopen(file_name, "rt");
                           n_lines = 0;
                           if (getcfg(lbs->name, "Attachment lines", str, sizeof(str)))
                              max_n_lines = atoi(str);
                           else
                              max_n_lines = 300;

                           if (f != NULL) {
                              while (!feof(f)) {
                                 str[0] = 0;
                                 fgets(str, sizeof(str), f);

                                 if (n_lines < max_n_lines) {
                                    if (!chkext(att[index], ".HTML"))
                                       rsputs2(lbs, FALSE, str);
                                    else
                                       rsputs(str);
                                 }
                                 n_lines++;
                              }
                              fclose(f);
                           }

                           if (!chkext(att[index], ".HTML"))
                              rsprintf("</pre>");
                           rsprintf("\n");
                           if (max_n_lines == 0)
                              rsprintf("<i><b>%d lines</b></i>\n", n_lines);
                           else if (n_lines > max_n_lines)
                              rsprintf("<i><b>... %d more lines ...</b></i>\n", n_lines - max_n_lines);
                        }
                     }
                  }

                  if (thumb_status)
                     rsprintf("</td></tr></table>\n");
               }

               rsprintf("</td></tr>\n");
            } else
               break;
      }

      /* optional attachment comment */
      if (getcfg(lbs->name, "Attachment comment", comment, sizeof(comment))) {
         rsprintf("<tr><td colspan=2 class=\"attribvalue\">\n");
         rsputs(comment);
         rsputs("</td></tr>\n");
      }

      /* field for add attachment */
      if (att[MAX_ATTACHMENTS - 1][0]) {
         rsprintf("<tr><td colspan=2 class=\"attribname\">%s</td>\n",
                  loc("Maximum number of attachments reached"));
         rsprintf("</td></tr>\n");
      } else {
         rsprintf("<tr><td nowrap class=\"attribname\">%s %d:</td>\n", loc("Attachment"), index + 1);
         rsprintf
             ("<td class=\"attribvalue\"><input type=\"file\" size=\"60\" maxlength=\"200\" name=\"attfile\" accept=\"filetype/*\">\n");
         rsprintf
             ("&nbsp;&nbsp;<input type=\"submit\" name=\"cmd\" value=\"%s\" onClick=\"return chkupload();\">\n",
              loc("Upload"));
         rsprintf("</td></tr>\n");
      }
   }

   rsprintf("</table><!-- listframe -->\n");

   /*---- menu buttons again ----*/

   rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");
   rsprintf("<input type=\"submit\" name=\"cmd\" value=\"%s\" onClick=\"return chkform();\">\n",
            loc("Submit"));
   rsprintf("<input type=\"submit\" name=\"cmd\" value=\"%s\" onClick=\"return chkform();\">\n",
            loc("Preview"));
   rsprintf("<input type=\"submit\" name=\"cmd\" value=\"%s\" onClick=\"return mark_submit();\">\n",
            loc("Back"));
   rsprintf("</span></td></tr>\n\n");

   rsprintf("</table><!-- show_standard_title -->\n");
   show_bottom_text(lbs);
   rsprintf("</form></body></html>\r\n");

   /* rescan unconditional attributes */
   if (_condition[0])
      scan_attributes(lbs->name);

   xfree(text);
}

/*------------------------------------------------------------------*/

void show_find_form(LOGBOOK * lbs)
{
   int i, j, year, month, day, flag;
   char str[NAME_LENGTH], mode[NAME_LENGTH], comment[NAME_LENGTH], option[NAME_LENGTH], login_name[256],
      full_name[256], user_email[256], enc_attr[NAME_LENGTH], whole_attr[NAME_LENGTH], 
      attrib[MAX_N_ATTR][NAME_LENGTH];

   /*---- header ----*/

   show_standard_header(lbs, FALSE, loc("ELOG find"), NULL, FALSE, NULL, NULL);

   /*---- title ----*/

   show_standard_title(lbs->name, "", 0);

   /*---- menu buttons ----*/

   rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");

   rsprintf("<input type=submit value=\"%s\">\n", loc("Search"));
   rsprintf("<input type=reset value=\"%s\">\n", loc("Reset Form"));
   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Back"));
   rsprintf("<input type=hidden name=jcmd value=\"\">\n");
   rsprintf("</span></td></tr>\n\n");

   /*---- evaluate conditional attributes ----*/

   for (i = 0; i < lbs->n_attr; i++)
      attrib[i][0] = 0;

   /* get attributes from parameters */
   attrib_from_param(lbs->n_attr, attrib);

   evaluate_conditions(lbs, attrib);

   /*---- entry form ----*/

   rsprintf("<tr><td class=\"form1\">\n");

   rsprintf("<b>%s:</b>&nbsp;&nbsp;", loc("Mode"));

   if (!getcfg(lbs->name, "Display mode", mode, sizeof(mode)))
      strcpy(mode, "Full");

   if (!getcfg(lbs->name, "Show text", str, sizeof(str)) || atoi(str) == 1) {
      if (strieq(mode, "Full"))
         rsprintf("<input type=radio id=\"full\" name=\"mode\" value=\"full\" checked>");
      else
         rsprintf("<input type=radio id=\"full\" name=\"mode\" value=\"full\">");
      rsprintf("<label for=\"full\">%s&nbsp;&nbsp;</label>\n", loc("Display full entries"));

      if (strieq(mode, "Summary"))
         rsprintf("<input type=radio id=\"summary\" name=\"mode\" value=\"summary\" checked>");
      else
         rsprintf("<input type=radio id=\"summary\" name=\"mode\" value=\"summary\">");
      rsprintf("<label for=\"summary\">%s&nbsp;&nbsp;</label>\n", loc("Summary only"));

   } else {
      if (strieq(mode, "Full") || strieq(mode, "Summary"))
         rsprintf("<input type=radio id=\"summary\" name=\"mode\" value=\"summary\" checked>");
      else
         rsprintf("<input type=radio id=\"summary\" name=\"mode\" value=\"summary\">");
      rsprintf("<label for=\"summary\">%s&nbsp;&nbsp;</label>\n", loc("Summary"));
   }

   if (strieq(mode, "Threaded"))
      rsprintf("<input type=radio id=\"threaded\" name=\"mode\" value=\"threaded\" checked>");
   else
      rsprintf("<input type=radio id=\"threaded\" name=\"mode\" value=\"threaded\">");
   rsprintf("<label for=\"threaded\">%s&nbsp;&nbsp;</label>\n", loc("Display threads"));

   rsprintf("<b>%s:</b>&nbsp;&nbsp;", loc("Export to"));

   if (strieq(mode, "CSV1"))
      rsprintf("<input type=radio id=\"CSV1\" name=\"mode\" value=\"CSV1\" checked>");
   else
      rsprintf("<input type=radio id=\"CSV1\" name=\"mode\" value=\"CSV1\">");
   rsprintf("<label for=\"CSV1\">%s&nbsp;&nbsp;</label>\n", loc("CSV (\",\" separated)"));

   if (strieq(mode, "CSV2"))
      rsprintf("<input type=radio id=\"CSV2\" name=\"mode\" value=\"CSV2\" checked>");
   else
      rsprintf("<input type=radio id=\"CSV2\" name=\"mode\" value=\"CSV2\">");
   rsprintf("<label for=\"CSV2\">%s&nbsp;&nbsp;</label>\n", loc("CSV (\";\" separated)"));

   if (strieq(mode, "XML"))
      rsprintf("<input type=radio id=\"XML\" name=\"mode\" value=\"XML\" checked>");
   else
      rsprintf("<input type=radio id=\"XML\" name=\"mode\" value=\"XML\">");
   rsprintf("<label for=\"XML\">XML&nbsp;&nbsp;</label>\n");

   if (strieq(mode, "Raw"))
      rsprintf("<input type=radio id=\"Raw\" name=\"mode\" value=\"Raw\" checked>");
   else
      rsprintf("<input type=radio id=\"Raw\" name=\"mode\" value=\"Raw\">");
   rsprintf("<label for=\"Raw\">Raw&nbsp;&nbsp;</label>\n");

   rsprintf("</td></tr>\n");

   rsprintf("<tr><td class=\"form2\"><b>%s:</b><br>", loc("Options"));

   rsprintf("<input type=checkbox id=\"attach\" name=\"attach\" value=1>");
   rsprintf("<label for=\"attach\">%s<br></label>\n", loc("Show attachments"));

   rsprintf("<input type=checkbox id=\"printable\" name=\"printable\" value=1>");
   rsprintf("<label for=\"printable\">%s<br></label>\n", loc("Printable output"));

   /* put hidden reverse=0, which gets used if the reverse checkbox is unchecked and "reverse sort=1" is in elogd.cfg */
   rsprintf("<input type=hidden name=\"reverse\" value=0>\n");
   if (getcfg(lbs->name, "Reverse sort", str, sizeof(str)) && atoi(str) == 1)
      rsprintf("<input type=checkbox id=\"reverse\" name=\"reverse\" value=1 checked>");
   else
      rsprintf("<input type=checkbox id=\"reverse\" name=\"reverse\" value=1>");
   rsprintf("<label for=\"reverse\">%s<br></label>\n", loc("Sort in reverse order"));

   /* count logbooks */
   for (i = 0;; i++) {
      if (!enumgrp(i, str))
         break;

      if (is_logbook(str))
         continue;
   }

   if (i > 2) {
      if (!getcfg(lbs->name, "Search all logbooks", str, sizeof(str)) || atoi(str) == 1 || atoi(str) == 2) {

         if (atoi(str) == 2)
            rsprintf("<input type=checkbox checked id=all name=all value=1>");
         else
            rsprintf("<input type=checkbox id=all name=all value=1>");
         rsprintf("<label for=\"all\">%s</label><br>\n", loc("Search all logbooks"));
      }
   }

   rsprintf(loc("Display"));
   if (!getcfg(lbs->name, "Entries per page", str, sizeof(str)))
      strcpy(str, "20");
   rsprintf(" <input type=text name=npp size=3 value=%s> ", str);
   rsprintf(loc("entries per page"));

   rsprintf("</td></tr>\n");

   rsprintf("<tr><td class=\"form2\"><b>%s:</b>", loc("Filters"));
   sprintf(str, "<a href=\"http://dmoz.org/Computers/Programming/Languages/Regular_Expressions/\">");
   strcat(str, loc("regular expressions"));
   strcat(str, "</a>");
   rsprintf("&nbsp;&nbsp;<span class=\"selcomment\">(");
   rsprintf(loc("Text fields are treated as %s"), str);
   rsprintf(")</span><br>");

   /* table for two-column items */
   rsprintf("<table width=\"100%%\" cellspacing=0>\n");

   rsprintf("<tr><td class=\"attribname\" nowrap width=\"10%%\">%s:</td>", loc("Entry date"));
   rsprintf("<td class=\"attribvalue\"><table width=\"100%%\" cellspacing=0 border=0>\n");
   rsprintf("<tr><td width=\"1%%\">%s:<td>", loc("Start"));

   year = month = day = 0;
   sprintf(str, "ya");
   if (isparam(str))
      year = atoi(getparam(str));
   sprintf(str, "ma");
   if (isparam(str))
      month = atoi(getparam(str));
   sprintf(str, "da");
   if (isparam(str))
      day = atoi(getparam(str));
   show_date_selector(day, month, year, "a");

   rsprintf("&nbsp;&nbsp;/&nbsp;&nbsp;%s:&nbsp;", loc("Show last"));

   rsprintf("<select name=last>\n");
   rsprintf("<option value=\"\">\n");
   rsprintf("<option value=1>%s\n", loc("Day"));
   rsprintf("<option value=7>%s\n", loc("Week"));
   rsprintf("<option value=31>%s\n", loc("Month"));
   rsprintf("<option value=92>%s\n", loc("3 Months"));
   rsprintf("<option value=182>%s\n", loc("6 Months"));
   rsprintf("<option value=364>%s\n", loc("Year"));
   rsprintf("</select> \n");

   rsprintf("</td></tr>\n");

   rsprintf("<tr><td width=\"1%%\">%s:<td>", loc("End"));

   year = month = day = 0;
   sprintf(str, "yb");
   if (isparam(str))
      year = atoi(getparam(str));
   sprintf(str, "mb");
   if (isparam(str))
      month = atoi(getparam(str));
   sprintf(str, "db");
   if (isparam(str))
      day = atoi(getparam(str));
   show_date_selector(day, month, year, "b");

   rsprintf("</td></tr></table></td></tr>\n");

   for (i = 0; i < lbs->n_attr; i++) {
      rsprintf("<tr><td class=\"attribname\" nowrap>%s:</td>", attr_list[i]);
      rsprintf("<td class=\"attribvalue\">");
      if (attr_options[i][0][0] == 0) {

         if (attr_flags[i] & (AF_DATE | AF_DATETIME)) {

            rsprintf("<table width=\"100%%\" cellspacing=0 border=0>\n");
            rsprintf("<tr><td width=\"1%%\">%s:<td>", loc("Start"));
            
            year = month = day = 0;
            sprintf(str, "y%da", i);
            if (isparam(str))
               year = atoi(getparam(str));
            sprintf(str, "m%da", i);
            if (isparam(str))
               month = atoi(getparam(str));
            sprintf(str, "d%da", i);
            if (isparam(str))
               day = atoi(getparam(str));
            
            sprintf(str, "%da", i);
            show_date_selector(day, month, year, str);
            if (attr_flags[i] & AF_DATETIME) {
               rsprintf("&nbsp;&nbsp;");
               show_time_selector(-1, -1, -1, str);
            }

            rsprintf("</td></tr>\n");
            rsprintf("<tr><td width=\"1%%\">%s:<td>", loc("End"));
            
            year = month = day = 0;
            sprintf(str, "y%db", i);
            if (isparam(str))
               year = atoi(getparam(str));
            sprintf(str, "m%db", i);
            if (isparam(str))
               month = atoi(getparam(str));
            sprintf(str, "d%db", i);
            if (isparam(str))
               day = atoi(getparam(str));

            sprintf(str, "%db", i);
            show_date_selector(day, month, year, str);
            if (attr_flags[i] & AF_DATETIME) {
               rsprintf("&nbsp;&nbsp;");
               show_time_selector(-1, -1, -1, str);
            }

            rsprintf("</td></tr></table>\n");

         } else if (attr_flags[i] & AF_MUSERLIST) {

            for (j = 0;; j++) {
               if (!enum_user_line(lbs, j, login_name, sizeof(login_name)))
                  break;
               get_user_line(lbs, login_name, NULL, full_name, NULL, NULL, NULL);

               sprintf(str, "%s_%d", attr_list[i], j);

               rsprintf("<nobr><input type=checkbox id=\"%s\" name=\"%s\" value=\"%s\"\">\n", str, str,
                        full_name);

               rsprintf("<label for=\"%s\">%s</label></nobr>\n", str, full_name);
            }

         } else if (attr_flags[i] & AF_MUSEREMAIL) {

            for (j = 0;; j++) {
               if (!enum_user_line(lbs, j, login_name, sizeof(login_name)))
                  break;
               get_user_line(lbs, login_name, NULL, NULL, user_email, NULL, NULL);

               sprintf(str, "%s_%d", attr_list[i], j);

               rsprintf("<nobr><input type=checkbox id=\"%s\" name=\"%s\" value=\"%s\"\">\n", str, str,
                        user_email);

               rsprintf("<label for=\"%s\">%s</label></nobr>\n", str, user_email);
            }

         } else {
            rsprintf("<input type=\"text\" size=\"30\" maxlength=\"80\" name=\"%s\" value=\"%s\">\n", 
               attr_list[i], attrib[i]);
         }

      } else {
         if (strieq(attr_options[i][0], "boolean")) {

            if (isparam(attr_list[i]) && *getparam(attr_list[i]))
               flag = atoi(getparam(attr_list[i]));
            else
               flag = -1;

            sprintf(str, "%s_0", attr_list[i]);
            rsprintf("<span style=\"white-space:nowrap;\">\n");
            if (flag == 0)
               rsprintf("<input type=radio checked id=\"%s\" name=\"%s\" value=\"0\">\n", str, attr_list[i]);
            else
               rsprintf("<input type=radio id=\"%s\" name=\"%s\" value=\"0\">\n", str, attr_list[i]);
            rsprintf("<label for=\"%s\">0</label>\n", str);
            rsprintf("</span>\n");

            sprintf(str, "%s_1", attr_list[i]);
            rsprintf("<span style=\"white-space:nowrap;\">\n");
            if (flag == 1)
               rsprintf("<input type=radio checked id=\"%s\" name=\"%s\" value=\"1\">\n", str, attr_list[i]);
            else
               rsprintf("<input type=radio id=\"%s\" name=\"%s\" value=\"1\">\n", str, attr_list[i]);
            rsprintf("<label for=\"%s\">1</label>\n", str);
            rsprintf("</span>\n");

            sprintf(str, "%s_2", attr_list[i]);
            rsprintf("<span style=\"white-space:nowrap;\">\n");
            if (flag == -1)
               rsprintf("<input type=radio checked id=\"%s\" name=\"%s\" value=\"\">\n", str, attr_list[i]);
            else
               rsprintf("<input type=radio id=\"%s\" name=\"%s\" value=\"\">\n", str, attr_list[i]);
            rsprintf("<label for=\"%s\">%s</label>\n", str, loc("unspecified"));
            rsprintf("</span>\n");
         }

         /* display image for icon */
         else if (attr_flags[i] & AF_ICON) {
            for (j = 0; j < MAX_N_LIST && attr_options[i][j][0]; j++) {
               strcpy(option, attr_options[i][j]);
               if (strchr(option, '{'))
                  *strchr(option, '{') = 0;

               sprintf(str, "Icon comment %s", option);
               getcfg(lbs->name, str, comment, sizeof(comment));

               if (comment[0] == 0)
                  strcpy(comment, option);

               rsprintf("<nobr><input type=radio name=\"%s\" value=\"%s\">", attr_list[i], comment);

               rsprintf("<img src=\"icons/%s\" alt=\"%s\" title=\"%s\"></nobr>\n", option, comment, comment);
            }
         }

         /* display check boxes (or'ed) */
         else if (attr_flags[i] & AF_MULTI) {
            for (j = 0; j < MAX_N_LIST && attr_options[i][j][0]; j++) {
               sprintf(str, "%s_%d", attr_list[i], j);

               if (isparam(str))
                  rsprintf("<nobr><input type=checkbox checked id=\"%s\" name=\"%s\" value=\"%s\"\">\n", 
                           str, str, attr_options[i][j]);
               else
                  rsprintf("<nobr><input type=checkbox id=\"%s\" name=\"%s\" value=\"%s\"\">\n", 
                           str, str, attr_options[i][j]);

               rsprintf("<label for=\"%s\">%s</label></nobr>\n", str, attr_options[i][j]);
            }
         }

         else {
            rsprintf("<select name=\"%s\"", attr_list[i]);

            if (is_cond_attr(i))
               rsprintf(" onChange=\"document.form1.jcmd.value='Find';document.form1.submit();\"");
            rsprintf(">\n");

            rsprintf("<option value=\"\">\n");
            for (j = 0; j < MAX_N_LIST && attr_options[i][j][0]; j++) {
               strencode2(str, attr_options[i][j], sizeof(str));
               if (strchr(str, '{'))
                  *strchr(str, '{') = 0;
               strencode2(enc_attr, attrib[i], sizeof(enc_attr));

               sprintf(whole_attr, "^%s$", str);
               if (strieq(attr_options[i][j], attrib[i]) || strieq(str, enc_attr) || strieq(whole_attr, enc_attr))
                  rsprintf("<option selected value=\"^%s$\">%s\n", str, str);
               else
                  rsprintf("<option value=\"^%s$\">%s\n", str, str);
            }
            rsprintf("</select>\n");
         }
      }
      rsprintf("</td></tr>\n");
   }

   rsprintf("<tr><td class=\"attribname\">%s:</td>", loc("Text"));
   rsprintf("<td class=\"attribvalue\">\n");
   if (isparam("subtext"))
      strlcpy(str, getparam("subtext"), sizeof(str));
   else
      str[0] = 0;
   rsprintf("<input type=\"text\" size=\"30\" maxlength=\"80\" name=\"subtext\" value=\"%s\">\n", str);

   rsprintf("<tr><td><td class=\"attribvalue\">\n");
   rsprintf("<input type=checkbox id=\"sall\" name=\"sall\" value=1>\n");
   rsprintf("<label for=\"sall\">%s</label>\n", loc("Search text also in attributes"));

   if (getcfg(lbs->name, "Case sensitive search", str, sizeof(str)) && atoi(str))
      rsprintf("<input type=checkbox id=\"sall\" name=\"casesensitive\" value=1 checked>\n");
   else
      rsprintf("<input type=checkbox id=\"sall\" name=\"casesensitive\" value=1>\n");
   rsprintf("<label for=\"casesensitive\">%s</label>\n", loc("Case sensitive"));

   rsprintf("</td></tr></table></td></tr></table>\n");
   show_bottom_text(lbs);
   rsprintf("</form></body></html>\r\n");
}

/*------------------------------------------------------------------*/

const char *find_section(const char *buf, const char *name)
{
   const char *pstart;
   char *pstr, str[80];

   do {
      if (*buf == '[') {
         pstart = buf;
         buf++;
         pstr = str;
         while (*buf && *buf != ']' && *buf != '\n' && *buf != '\r')
            *pstr++ = *buf++;
         *pstr = 0;
         if (strieq(str, name))
            return pstart;
      }

      if (buf)
         buf = strchr(buf, '\n');
      if (buf)
         buf++;
      if (buf && *buf == '\r')
         buf++;

   } while (buf);

   return NULL;
}

/*------------------------------------------------------------------*/

const char *find_next_section(const char *buf)
{
   do {
      if (*buf == '[')
         return buf;

      if (buf)
         buf = strchr(buf, '\n');
      if (buf)
         buf++;
   } while (buf);

   return NULL;
}

/*------------------------------------------------------------------*/

void load_config_section(char *section, char **buffer, char *error)
{
   int fh, length;
   char *p;

   error[0] = 0;
   *buffer = NULL;
   fh = open(config_file, O_RDONLY | O_BINARY);
   if (fh < 0) {
      sprintf(error, "Cannot read configuration file \"%s\": %s", config_file, strerror(errno));
      return;
   }
   length = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);
   *buffer = xmalloc(length + 1);
   read(fh, *buffer, length);
   (*buffer)[length] = 0;
   close(fh);

   if (section == NULL)
      return;

   if ((p = (char *) find_section(*buffer, section)) != NULL) {
      if (strchr(p, ']')) {
         p = strchr(p, ']') + 1;
         while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
      }
      strlcpy(*buffer, p, length);
      if ((p = (char *) find_next_section(*buffer + 1)) != NULL) {
         *p = 0;

         /* strip trailing newlines */
         if (p) {
            p--;

            while (p > *buffer && (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t'))
               *p-- = 0;
         }
      } else {
         p = *buffer + strlen(*buffer) - 1;

         while (p > *buffer && (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t'))
            *p-- = 0;
      }
   }
}

/*------------------------------------------------------------------*/

void show_admin_page(LOGBOOK * lbs, char *top_group)
{
   int rows, cols;
   char *buffer, error_str[256];
   char section[NAME_LENGTH], str[NAME_LENGTH], grp[NAME_LENGTH];

   /*---- header ----*/

   sprintf(str, "ELOG %s", loc("Admin"));
   show_html_header(lbs, FALSE, str, TRUE, FALSE, NULL, FALSE);

   rsprintf("<body><form method=\"POST\" action=\"./\" enctype=\"multipart/form-data\">\n");

   /*---- title ----*/

   show_standard_title(lbs->name, "", 0);

   /*---- menu buttons ----*/

   rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");
   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Save"));
   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Cancel"));

   if (lbs->top_group[0] && (!top_group || strieq(top_group, "global"))) {
      if (is_admin_user("global", getparam("unm"))) {
         if (lbs->top_group[0]) {

            sprintf(str, "global %s", lbs->top_group);

            if (is_group(str)) {
               sprintf(grp, "[global %s]", lbs->top_group);
               sprintf(str, loc("Change %s"), grp);
               rsprintf("<input type=submit name=cmd value=\"%s\">\n", str);
            }
         }
      }
   }

   if (is_group("global") && !strieq(top_group, "global")) {
      if (is_admin_user_global(getparam("unm"))) {
         sprintf(str, loc("Change %s"), "[global]");
         rsprintf("<input type=submit name=cmd value=\"%s\">\n", str);
      }
   }

   if (top_group) {
      if (strieq(top_group, "global")) {
         rsprintf("<input type=hidden name=global value=\"global\">\n");
         strcpy(str, "[global]");
      } else {
         rsprintf("<input type=hidden name=global value=\"%s\">\n", top_group);
         sprintf(str, "[global %s]", top_group);
      }
      rsprintf("<br><center><b>%s</b></center>", str);
   }

   if (is_group("global") && !strieq(top_group, "global")) {
      if (is_admin_user("global", getparam("unm"))) {
         rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Delete this logbook"));
         rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Rename this logbook"));
         rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Create new logbook"));
      }
   }

   rsprintf("</span></td></tr>\n\n");

   /*---- entry form ----*/

   rsprintf("<tr><td class=\"form1\">\n");

   /* extract section of current logbook */
   if (top_group) {
      if (strieq(top_group, "global"))
         strcpy(section, "global");
      else
         sprintf(section, "global %s", top_group);
   } else
      strcpy(section, lbs->name);

   load_config_section(section, &buffer, error_str);

   if (error_str[0]) {
      rsprintf("<h2>%s</h2>\n", error_str);
      rsprintf("</table></td></tr></table>\n");
      rsprintf("</body></html>\r\n");
      return;
   }

   if (getcfg(section, "Admin textarea", str, sizeof(str)) && strchr(str, ',') != NULL) {
      cols = atoi(str);
      rows = atoi(strchr(str, ',') + 1);
   } else {
      cols = 120;
      rows = 30;
   }

   rsprintf("<textarea cols=%d rows=%d wrap=virtual name=Text>", cols, rows);

   rsputs3(buffer);
   xfree(buffer);

   rsprintf("</textarea>\n");

   /* put link for config page */
   rsprintf("<br><a target=\"_blank\" href=\"https://midas.psi.ch/elog/config.html\">Syntax Help</a>");

   rsprintf("</td></tr>\n");

   /*---- menu buttons ----*/

   rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");

   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Save"));
   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Cancel"));
   rsprintf("</span></td></tr>\n\n");

   rsprintf("</table>\n\n");
   show_bottom_text(lbs);
   rsprintf("</form></body></html>\r\n");
}

/*------------------------------------------------------------------*/

void remove_crlf(char *buffer)
{
   char *p;

   /* convert \r\n -> \n */
   p = buffer;
   while ((p = strstr(p, "\r\n")) != NULL) {
      strcpy(p, p + 1);
   }
}

/*------------------------------------------------------------------*/

void adjust_crlf(char *buffer, int bufsize)
{
   char *p;

#ifdef OS_UNIX

   /* convert \r\n -> \n */
   bufsize = 0;                 // avoid compiler warning about unused bufsize
   p = buffer;
   while ((p = strstr(p, "\r\n")) != NULL) {
      strcpy(p, p + 1);
   }
#else

   char *tmpbuf;

   assert(bufsize);
   tmpbuf = xmalloc(bufsize);

   /* convert \n -> \r\n */
   p = buffer;
   while ((p = strstr(p, "\n")) != NULL) {

      if (p > buffer && *(p - 1) == '\r') {
         p++;
         continue;
      }

      if ((int) strlen(buffer) + 2 >= bufsize) {
         xfree(tmpbuf);
         return;
      }

      strlcpy(tmpbuf, p, bufsize);
      *(p++) = '\r';
      strlcpy(p, tmpbuf, bufsize - (p - buffer));
      p++;
   }

   xfree(tmpbuf);
#endif
}

/*------------------------------------------------------------------*/

int save_admin_config(char *section, char *buffer, char *error)
{
   int fh, i, length;
   char *buf, *buf2, *p1, *p2;

   error[0] = 0;

   fh = open(config_file, O_RDWR | O_BINARY, 644);
   if (fh < 0) {
      sprintf(error, loc("Cannot open file <b>%s</b>"), config_file);
      strcat(error, ": ");
      strcat(error, strerror(errno));
      return 0;
   }

   /* read previous contents */
   length = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);
   buf = xmalloc(length + strlen(buffer) + 10);
   read(fh, buf, length);
   buf[length] = 0;

   /* find previous logbook config */
   p1 = (char *) find_section(buf, section);
   p2 = (char *) find_next_section(p1 + 1);

   /* save tail */
   buf2 = NULL;

   if (p2)
      buf2 = xstrdup(p2);

   /* combine old and new config */
   sprintf(p1, "[%s]\r\n", section);
   strcat(p1, buffer);
   strcat(p1, "\r\n\r\n");

   if (p2) {
      strlcat(p1, buf2, length + strlen(buffer) + 1);
      xfree(buf2);
   }

   adjust_crlf(buf, length + strlen(buffer) + 10);

   lseek(fh, 0, SEEK_SET);
   i = write(fh, buf, strlen(buf));
   if (i < (int) strlen(buf)) {
      sprintf(error, loc("Cannot write to <b>%s</b>"), config_file);
      strcat(error, ": ");
      strcat(error, strerror(errno));
      close(fh);
      xfree(buf);
      return 0;
   }

   TRUNCATE(fh);

   close(fh);
   xfree(buf);

   /* force re-read of config file */
   check_config_file(TRUE);

   return 1;
}

/*------------------------------------------------------------------*/

int change_config_line(LOGBOOK * lbs, char *option, char *old_value, char *new_value)
{
   int fh, i, j, n, length, bufsize;
   char str[NAME_LENGTH], *buf, *buf2, *p1, *p2, *p3;
   char list[MAX_N_LIST][NAME_LENGTH], line[NAME_LENGTH];

   fh = open(config_file, O_RDWR | O_BINARY, 644);
   if (fh < 0) {
      sprintf(str, loc("Cannot open file <b>%s</b>"), config_file);
      strcat(str, ": ");
      strcat(str, strerror(errno));
      show_error(str);
      return 0;
   }

   /* read previous contents */
   length = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);
   bufsize = 2 * (length + strlen(new_value) + 10);
   buf = xmalloc(bufsize);
   read(fh, buf, length);
   buf[length] = 0;

   /* find location of option */
   p1 = (char *) find_param(buf, lbs->name, option);
   if (p1 == NULL)
      return 0;

   p2 = strchr(p1, '=');
   if (p2 == 0)
      return 0;

   p2++;
   while (*p2 == ' ' || *p2 == '\t')
      p2++;

   strlcpy(line, p2, sizeof(line));
   if (strchr(line, '\r'))
      *strchr(line, '\r') = 0;
   if (strchr(line, '\n'))
      *strchr(line, '\n') = 0;
   n = strbreak(line, list, MAX_N_LIST, ",", FALSE);

   /* save tail */
   p3 = strchr(p2, '\n');
   if (p3 && *(p3 - 1) == '\r')
      p3--;

   buf2 = NULL;
   if (p3)
      buf2 = xstrdup(p3);

   if (old_value[0]) {
      for (i = 0; i < n; i++) {
         if (strieq(old_value, list[i])) {
            if (new_value[0]) {
               /* rename value */
               strcpy(list[i], new_value);
            } else {
               /* delete value */
               for (j = i; j < n - 1; j++)
                  strcpy(list[j], list[j + 1]);
               n--;
            }
            break;
         }
      }
   } else {
      if (n < MAX_N_LIST)
         strcpy(list[n++], new_value);
   }

   /* write new option list */
   for (i = 0; i < n; i++) {
      strcpy(p2, list[i]);
      if (i < n - 1)
         strcat(p2, ", ");
      p2 += strlen(p2);
   }

   /* append tail */
   if (buf2) {
      strlcat(p2, buf2, length + strlen(new_value) + 10);
      xfree(buf2);
   }

   adjust_crlf(buf, bufsize);

   lseek(fh, 0, SEEK_SET);
   i = write(fh, buf, strlen(buf));
   if (i < (int) strlen(buf)) {
      sprintf(str, loc("Cannot write to <b>%s</b>"), config_file);
      strcat(str, ": ");
      strcat(str, strerror(errno));
      show_error(str);
      close(fh);
      xfree(buf);
      return 0;
   }

   TRUNCATE(fh);

   close(fh);
   xfree(buf);

   /* force re-read of config file */
   check_config_file(TRUE);

   return 1;
}

/*------------------------------------------------------------------*/

int delete_logbook(LOGBOOK * lbs, char *error)
{
   int fh, i, length;
   char *buf, *p1, *p2;

   error[0] = 0;

   fh = open(config_file, O_RDWR | O_BINARY, 644);
   if (fh < 0) {
      sprintf(error, loc("Cannot open file <b>%s</b>"), config_file);
      strcat(error, ": ");
      strcat(error, strerror(errno));
      return 0;
   }

   /* remove logbook name in groups */
   change_logbook_in_group(lbs, "");

   /* read previous contents */
   length = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);
   buf = xmalloc(length + 1);
   read(fh, buf, length);
   buf[length] = 0;

   /* find logbook config */
   p1 = (char *) find_section(buf, lbs->name);
   p2 = (char *) find_next_section(p1 + 1);

   if (p2)
      strlcpy(p1, p2, strlen(p2) + 1);
   else
      *p1 = 0;

   lseek(fh, 0, SEEK_SET);
   i = write(fh, buf, strlen(buf));
   if (i < (int) strlen(buf)) {
      sprintf(error, loc("Cannot write to <b>%s</b>"), config_file);
      strcat(error, ": ");
      strcat(error, strerror(errno));
      close(fh);
      xfree(buf);
      return 0;
   }

   TRUNCATE(fh);

   close(fh);
   xfree(buf);

   /* force re-read of config file */
   check_config_file(TRUE);
   el_index_logbooks();

   return 1;
}

/*------------------------------------------------------------------*/

int rename_logbook(LOGBOOK * lbs, char *new_name)
{
   int fh, i, length, bufsize;
   char *buf, *buf2, *p1, *p2;
   char str[256], lb_dir[256], old_dir[256], new_dir[256];

   fh = open(config_file, O_RDWR | O_BINARY, 644);
   if (fh < 0) {
      sprintf(str, loc("Cannot open file <b>%s</b>"), config_file);
      strcat(str, ": ");
      strcat(str, strerror(errno));
      show_error(str);
      return 0;
   }

   /* rename logbook file */
   if (!getcfg(lbs->name, "Subdir", str, sizeof(str))) {
      strlcpy(lb_dir, logbook_dir, sizeof(lb_dir));
      if (lb_dir[strlen(lb_dir) - 1] != DIR_SEPARATOR)
         strlcat(lb_dir, DIR_SEPARATOR_STR, sizeof(lb_dir));

      sprintf(old_dir, "%s%s", lb_dir, lbs->name);
      sprintf(new_dir, "%s%s", lb_dir, new_name);
      rename(old_dir, new_dir);
   }

   /* change logbook name in groups */
   change_logbook_in_group(lbs, new_name);

   /* read previous contents */
   length = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);
   bufsize = 2 * (length + strlen(new_name) + 10);
   buf = xmalloc(bufsize);
   read(fh, buf, length);
   buf[length] = 0;

   /* find logbook config */
   p1 = (char *) find_section(buf, lbs->name);
   p2 = strchr(p1, ']');
   if (p2 == NULL) {
      close(fh);
      xfree(buf);
      show_error(loc("Syntax error in config file"));
      return 0;
   }
   p2++;

   /* save tail */
   buf2 = xstrdup(p2);

   /* replace logbook name */
   sprintf(p1, "[%s]", new_name);

   strlcat(p1, buf2, length + strlen(new_name) + 1);
   xfree(buf2);

   adjust_crlf(buf, bufsize);

   lseek(fh, 0, SEEK_SET);
   i = write(fh, buf, strlen(buf));
   if (i < (int) strlen(buf)) {
      sprintf(str, loc("Cannot write to <b>%s</b>"), config_file);
      strcat(str, ": ");
      strcat(str, strerror(errno));
      show_error(str);
      close(fh);
      xfree(buf);
      return 0;
   }

   TRUNCATE(fh);

   close(fh);
   xfree(buf);

   /* force re-read of config file */
   check_config_file(TRUE);
   el_index_logbooks();

   return 1;
}

/*------------------------------------------------------------------*/

int create_logbook(LOGBOOK * oldlbs, char *logbook, char *templ)
{
   int fh, i, length, bufsize, templ_length;
   char *buf, *p1, *p2, str[256];

   fh = open(config_file, O_RDWR | O_BINARY, 644);
   if (fh < 0) {
      sprintf(str, loc("Cannot open file <b>%s</b>"), config_file);
      strcat(str, ": ");
      strcat(str, strerror(errno));
      show_error(str);
      return 0;
   }

   /* add logbook to current group */
   add_logbook_to_group(oldlbs, logbook);

   /* read previous contents */
   length = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);
   bufsize = 2 * (2 * length + 1);
   buf = xmalloc(bufsize);
   read(fh, buf, length);
   buf[length] = 0;
   templ_length = 0;
   p2 = NULL;

   /* find template logbook */
   if (templ[0]) {
      p1 = (char *) find_section(buf, templ);
      p2 = (char *) find_next_section(p1 + 1);
   } else
      p1 = NULL;

   if (p1) {
      p1 = strchr(p1, ']');
      if (p1)
         while (*p1 == ']' || *p1 == '\r' || *p1 == '\n')
            p1++;

      if (p2)
         templ_length = p2 - p1;
      else
         templ_length = strlen(p1);
   }

   /* insert single blank line after last logbook */
   p2 = buf + strlen(buf) - 1;

   while (p2 > buf && (*p2 == '\r' || *p2 == '\n' || *p2 == ' ' || *p2 == '\t')) {
      *p2 = 0;
      p2--;
   }
   if (p2 > buf)
      p2++;

   strcat(p2, "\r\n\r\n[");
   strcat(p2, logbook);
   strcat(p2, "]\r\n");
   if (p1) {
      p2 = buf + strlen(buf);
      strncpy(p2, p1, templ_length);
      p2[templ_length] = 0;
   }

   adjust_crlf(buf, bufsize);

   lseek(fh, 0, SEEK_SET);
   i = write(fh, buf, strlen(buf));
   if (i < (int) strlen(buf)) {
      sprintf(str, loc("Cannot write to <b>%s</b>"), config_file);
      strcat(str, ": ");
      strcat(str, strerror(errno));
      show_error(str);
      close(fh);
      xfree(buf);
      return 0;
   }

   TRUNCATE(fh);

   close(fh);
   xfree(buf);

   /* force re-read of config file */
   check_config_file(TRUE);
   el_index_logbooks();

   return 1;
}

/*------------------------------------------------------------------*/

int save_config(char *buffer, char *error)
{
   int fh, i;
   char *buf;

   error[0] = 0;

   fh = open(config_file, O_RDWR | O_BINARY | O_CREAT, 0644);
   if (fh < 0) {
      sprintf(error, loc("Cannot open file <b>%s</b>"), config_file);
      strcat(error, ": ");
      strcat(error, strerror(errno));
      return 0;
   }

   buf = xmalloc(strlen(buffer) * 2);
   strlcpy(buf, buffer, strlen(buffer) * 2);
   adjust_crlf(buf, strlen(buffer) * 2);

   i = write(fh, buf, strlen(buf));
   if (i < (int) strlen(buf)) {
      sprintf(error, loc("Cannot write to <b>%s</b>"), config_file);
      strcat(error, ": ");
      strcat(error, strerror(errno));
      close(fh);
      return 0;
   }

   TRUNCATE(fh);

   close(fh);

   /* force re-read of config file */
   check_config_file(TRUE);

   return 1;
}

/*------------------------------------------------------------------*/

int save_user_config(LOGBOOK * lbs, char *user, BOOL new_user, BOOL activate)
{
   char file_name[256], str[256], *pl, user_enc[256], new_pwd[80], new_pwd2[80], smtp_host[256],
       email_addr[256], mail_from[256], mail_from_name[256], subject[256], mail_text[2000], str2[256],
       admin_user[80], enc_pwd[80], url[256], error[2000];
   int i, self_register;
   PMXML_NODE node, subnode;

   /* do not allow HTML in user name */
   strencode2(user_enc, user, sizeof(user_enc));

   /* check for user name */
   if (!isparam("new_user_name") || *getparam("new_user_name") == 0) {
      sprintf(str, loc("Please enter \"%s\""), loc("Login name"));
      show_error(str);
      return 0;
   }

   /* check for full name */
   if (!isparam("new_full_name") || *getparam("new_full_name") == 0) {
      sprintf(str, loc("Please enter \"%s\""), loc("Full name"));
      show_error(str);
      return 0;
   }

   /* check for blank character in user name */
   strlcpy(str, getparam("new_user_name"), sizeof(str));
   if (strchr(str, ' ')) {
      show_error(loc("User name may not contain blanks"));
      return 0;
   }

   /* check for blank password */
   str[0] = 0;
   if (isparam("newpwd"))
      strlcpy(str, getparam("newpwd"), sizeof(str));
   if (isparam("hpwd"))
      strlcpy(str, getparam("hpwd"), sizeof(str));
   if (isparam("encpwd"))
      strlcpy(str, getparam("encpwd"), sizeof(str));
   if (str[0] == 0) {
      show_error(loc("Empty password not allowed"));
      return 0;
   }
   if (strchr(str, ' ')) {
      show_error(loc("Password may not contain blanks"));
      return 0;
   }

   /* check self register flag */
   self_register = 0;
   if (getcfg(lbs->name, "Self register", str, sizeof(str)))
      self_register = atoi(str);

   if (!activate) {
      /* check for hidden password */
      if (isparam("hpwd")) {
         strlcpy(new_pwd, getparam("hpwd"), sizeof(new_pwd));
      } else {
         /* check if passwords match */
         if (isparam("newpwd") && isparam("newpwd2")) {
            do_crypt(getparam("newpwd"), new_pwd, sizeof(new_pwd));
            do_crypt(getparam("newpwd2"), new_pwd2, sizeof(new_pwd2));
         }

         if (strcmp(new_pwd, new_pwd2) != 0) {
            show_error(loc("New passwords do not match, please retype"));
            return 0;
         }
      }

      /* check if user exists */
      if (new_user && self_register == 3) {
         if (get_user_line(lbs, user_enc, NULL, NULL, NULL, NULL, NULL) == 1) {
            sprintf(str, "%s \"%s\" %s", loc("Login name"), user_enc, loc("exists already"));
            show_error(str);
            return 0;
         }
      }

      /* check for valid email address */
      strlcpy(str, getparam("new_user_email"), sizeof(str));
      if (strchr(str, '@') == NULL || strchr(str, '.') == NULL) {
         show_error(loc("Please specify a valid email address"));
         return 0;
      }
   }

   /* if register through selection page, use first logbook with same password file */
   if (lbs == NULL) {
      getcfg(NULL, "password file", file_name, sizeof(file_name));
      for (i = 0; lb_list[i].name[0]; i++) {
         getcfg(lb_list[i].name, "password file", str, sizeof(str));
         if (strieq(file_name, str))
            break;
      }
      if (lb_list[i].name[0] == 0)
         lbs = &lb_list[0];
      else
         lbs = &lb_list[i];
   }

   if (activate || !new_user || self_register != 3) {   /* do not save in mode 3 */
      getcfg(lbs->name, "Password file", str, sizeof(str));

      if (lbs->pwd_xml_tree == NULL)
         return 0;

      sprintf(str, "/list/user[name=%s]", user_enc);
      node = mxml_find_node(lbs->pwd_xml_tree, str);

      if (node && new_user) {
         sprintf(str, "%s \"%s\" %s", loc("Login name"), user_enc, loc("exists already"));
         show_error(str);
         return 0;
      }

      if (new_user) {
         node = mxml_find_node(lbs->pwd_xml_tree, "/list");
         node = mxml_add_node(node, "user", NULL);

         if (isparam("new_user_name")) {
            strencode2(str, getparam("new_user_name"), sizeof(str));
            mxml_add_node(node, "name", str);
         }
         if (activate) {
            if (isparam("encpwd"))
               mxml_add_node(node, "password", getparam("encpwd"));
         } else
            mxml_add_node(node, "password", new_pwd);

         if (isparam("new_full_name")) {
            strencode2(str, getparam("new_full_name"), sizeof(str));
            mxml_add_node(node, "full_name", str);
         }
         mxml_add_node(node, "last_logout", "0");
         mxml_add_node(node, "last_activity", "0");
         if (isparam("new_user_email"))
            mxml_add_node(node, "email", getparam("new_user_email"));

      } else {
         /* replace record */
         if (isparam("new_user_name")) {
            strencode2(str, getparam("new_user_name"), sizeof(str));
            mxml_replace_subvalue(node, "name", str);
         }
         mxml_replace_subvalue(node, "password", new_pwd);
         if (isparam("new_full_name")) {
            strencode2(str, getparam("new_full_name"), sizeof(str));
            mxml_replace_subvalue(node, "full_name", str);
         }
         if (isparam("new_user_email"))
            mxml_replace_subvalue(node, "email", getparam("new_user_email"));
      }

      subnode = mxml_find_node(node, "email_notify");
      if (subnode)
         mxml_delete_node(subnode);
      mxml_add_node(node, "email_notify", NULL);
      subnode = mxml_find_node(node, "email_notify");
      for (i = 0; lb_list[i].name[0]; i++) {
         sprintf(str, "sub_lb%d", i);
         if (isparam(str) && getparam(str) && atoi(getparam(str)))
            mxml_add_node(subnode, "logbook", lb_list[i].name);
      }

      if (get_password_file(lbs, file_name, sizeof(file_name)))
         mxml_write_tree(file_name, lbs->pwd_xml_tree);
   }

   /* if requested, send notification email to admin user */
   if (new_user && (self_register == 2 || self_register == 3) && !isparam("admin")) {
      if (!getcfg("global", "SMTP host", smtp_host, sizeof(smtp_host))) {
         show_error(loc("No SMTP host defined in [global] section of configuration file"));
         return 0;
      }

      /* try to get URL from referer */
      if (!getcfg("global", "URL", url, sizeof(url))) {
         if (referer[0])
            strcpy(url, referer);
         else {
            if (elog_tcp_port == 80) {
               if (_ssl_flag)
                  sprintf(url, "https://%s/", http_host);
               else
                  sprintf(url, "http://%s/", http_host);
            } else {
               if (_ssl_flag)
                  sprintf(url, "https://%s:%d/", http_host, elog_tcp_port);
               else
                  sprintf(url, "http://%s:%d/", http_host, elog_tcp_port);
            }
            if (lbs) {
               strlcat(url, lbs->name_enc, sizeof(url));
               strlcat(url, "/", sizeof(url));
            }
         }
      } else {
         if (url[strlen(url) - 1] != '/')
            strlcat(url, "/", sizeof(url));
         if (lbs) {
            strlcat(url, lbs->name_enc, sizeof(url));
            strlcat(url, "/", sizeof(url));
         }
      }

      retrieve_email_from(lbs, mail_from, mail_from_name, NULL);

      if (activate) {
         mail_text[0] = 0;
         if (isparam("new_user_email") && isparam("new_user_name")) {
            compose_email_header(lbs, loc("Your ELOG account has been activated"), mail_from_name,
                                 getparam("new_user_email"), NULL, mail_text, sizeof(mail_text), 1, 0, NULL,
                                 0, 0);

            strlcat(mail_text, "\r\n", sizeof(mail_text));
            strlcat(mail_text, loc("Your ELOG account has been activated on host"), sizeof(mail_text));
            sprintf(mail_text + strlen(mail_text), " %s", http_host);
            sprintf(mail_text + strlen(mail_text), ".\r\n\r\n");
            sprintf(url + strlen(url), "?cmd=Login&unm=%s", getparam("new_user_name"));
            sprintf(mail_text + strlen(mail_text), "%s %s.\r\n\r\n", loc("You can access it at"), url);
            sprintf(mail_text + strlen(mail_text), "%s.\r\n",
                    loc("To subscribe to any logbook, click on 'Config' in that logbook"));

            if (sendmail(lbs, smtp_host, mail_from, getparam("new_user_email"), mail_text, error,
                         sizeof(error)) == -1) {
               sprintf(str, loc("Cannot send email notification to \"%s\""), getparam("new_user_email"));
               strlcat(str, " : ", sizeof(str));
               strlcat(str, error, sizeof(str));
               strencode2(str2, str, sizeof(str2));
               show_error(str2);
               return 0;
            }
         }
      } else {
         if (getcfg(lbs->name, "Admin user", admin_user, sizeof(admin_user))) {
            pl = strtok(admin_user, " ,");
            while (pl) {
               get_user_line(lbs, pl, NULL, NULL, email_addr, NULL, NULL);
               if (email_addr[0]) {

                  /* compose subject */
                  if (self_register == 3) {
                     if (lbs)
                        sprintf(subject, loc("Registration request on logbook \"%s\""), lbs->name);
                     else
                        sprintf(subject, loc("Registration request on host \"%s\""), host_name);
                     sprintf(str, loc("A new ELOG user wants to register on \"%s\""), host_name);
                  } else {
                     if (isparam("new_user_name")) {
                        if (lbs)
                           sprintf(subject, loc("User \"%s\" registered on logbook \"%s\""),
                                   getparam("new_user_name"), lbs->name);
                        else
                           sprintf(subject, loc("User \"%s\" registered on host \"%s\""),
                                   getparam("new_user_name"), host_name);
                     }

                     sprintf(str, loc("A new ELOG user has been registered on %s"), host_name);
                  }

                  mail_text[0] = 0;
                  compose_email_header(lbs, subject, mail_from_name, email_addr,
                                       NULL, mail_text, sizeof(mail_text), 1, 0, NULL, 0, 0);
                  sprintf(mail_text + strlen(mail_text), "\r\n%s\r\n", str);

                  if (lbs)
                     sprintf(mail_text + strlen(mail_text), "%s             : %s\r\n", loc("Logbook"),
                             lbs->name);
                  else
                     sprintf(mail_text + strlen(mail_text), "%s                : %s\r\n", loc("Host"),
                             host_name);

                  if (isparam("new_user_name"))
                     sprintf(mail_text + strlen(mail_text), "%s          : %s\r\n", loc("Login name"),
                             getparam("new_user_name"));
                  if (isparam("new_full_name"))
                     sprintf(mail_text + strlen(mail_text), "%s           : %s\r\n", loc("Full name"),
                             getparam("new_full_name"));
                  if (isparam("new_user_email"))
                     sprintf(mail_text + strlen(mail_text), "%s               : %s\r\n", loc("Email"),
                             getparam("new_user_email"));

                  if (self_register == 3) {
                     sprintf(mail_text + strlen(mail_text), "\r\n%s:\r\n",
                             loc("Hit following URL to activate that account"));

                     sprintf(mail_text + strlen(mail_text), "\r\nURL                 : %s", url);

                     strlcpy(str, isparam("new_full_name") ? getparam("new_full_name") : "", sizeof(str));
                     url_encode(str, sizeof(str));
                     if (isparam("newpwd"))
                        do_crypt(getparam("newpwd"), enc_pwd, sizeof(enc_pwd));
                     else
                        enc_pwd[0] = 0;
                     url_encode(enc_pwd, sizeof(enc_pwd));
                     if (isparam("new_user_name"))
                        sprintf(mail_text + strlen(mail_text), "?cmd=%s&new_user_name=%s&new_full_name=%s",
                                loc("Activate"), getparam("new_user_name"), str);
                     if (isparam("new_user_email"))
                        sprintf(mail_text + strlen(mail_text), "&new_user_email=%s",
                                getparam("new_user_email"));

                     for (i = 0; lb_list[i].name[0]; i++) {
                        sprintf(str, "sub_lb%d", i);
                        if (isparam(str) && atoi(getparam(str)) == 1)
                           sprintf(mail_text + strlen(mail_text), "&%s=1", str);
                     }

                     sprintf(mail_text + strlen(mail_text), "&encpwd=%s&unm=%s\r\n", enc_pwd, pl);
                  } else {
                     if (isparam("new_user_name"))
                        sprintf(mail_text + strlen(mail_text),
                                "\r\n%s URL         : %s?cmd=Config&cfg_user=%s&unm=%s\r\n", loc("Logbook"),
                                url, getparam("new_user_name"), pl);
                  }

                  if (sendmail(lbs, smtp_host, mail_from, email_addr, mail_text, error, sizeof(error)) == -1) {
                     sprintf(str, loc("Cannot send email notification to \"%s\""),
                             getparam("new_user_email"));
                     strlcat(str, " : ", sizeof(str));
                     strlcat(str, error, sizeof(str));
                     strencode2(str2, str, sizeof(str2));
                     show_error(str2);
                     return 0;
                  };
               }

               pl = strtok(NULL, " ,");
            }
         } else {
            show_error(loc("No admin user has been defined in configuration file"));
            return 0;
         }

         if (self_register == 3) {
            sprintf(str, "?cmd=%s", loc("Requested"));
            redirect(lbs, str);
            return 0;
         }
      }
   }

   /* if user name changed, set cookie */
   if (isparam("new_user_name") && isparam("unm")) {
      if (strcmp(user_enc, getparam("new_user_name")) != 0 && strcmp(user_enc, getparam("unm")) == 0) {
         set_login_cookies(lbs, getparam("new_user_name"), new_pwd);
         return 0;
      }
   }

   /* if new user, login as this user */
   if (new_user && !isparam("unm")) {
      if (isparam("new_user_name"))
         set_login_cookies(lbs, getparam("new_user_name"), new_pwd);
      return 0;
   }

   return 1;
}

/*------------------------------------------------------------------*/

int remove_user(LOGBOOK * lbs, char *user)
{
   char file_name[256], str[1000], str2[1000];
   PMXML_NODE node;

   if (lbs->pwd_xml_tree == NULL) {
      show_error("No password file loaded");
      return FALSE;
   }

   sprintf(str, "/list/user[name=%s]", user);
   node = mxml_find_node(lbs->pwd_xml_tree, str);
   if (node == NULL) {
      sprintf(str, loc("User \"%s\" not found in password file"), user);
      strencode2(str2, str, sizeof(str2));
      show_error(str2);
      return FALSE;
   }

   mxml_delete_node(node);

   if (get_password_file(lbs, file_name, sizeof(file_name))) {
      if (!mxml_write_tree(file_name, lbs->pwd_xml_tree)) {
         sprintf(str, loc("Cannot write to file <b>%s</b>"), file_name);
         strcat(str, ": ");
         strcat(str, strerror(errno));
         show_error(str);
         return FALSE;
      }
   }

   return TRUE;
}

/*------------------------------------------------------------------*/

int ascii_compare(const void *s1, const void *s2)
{
   return stricmp(*(char **) s1, *(char **) s2);
}

/*------------------------------------------------------------------*/

void show_config_page(LOGBOOK * lbs)
{
   char str[256], user[80], password[80], full_name[80], user_email[80], logbook[256];
   char **user_list;
   int i, n;
   BOOL email_notify[1000];

   if (lbs)
      strcpy(logbook, lbs->name);
   else
      strcpy(logbook, "global");

   /* get user */
   strcpy(user, isparam("unm") ? getparam("unm") : "");
   if (isparam("cfg_user"))
      strcpy(user, getparam("cfg_user"));

   /*---- header ----*/

   show_standard_header(lbs, TRUE, loc("ELOG user config"), ".", FALSE, NULL, NULL);

   /*---- javascript to warn removal of user ----*/
   rsprintf("<script type=\"text/javascript\">\n");
   rsprintf("<!--\n\n");
   rsprintf("function chkrem()\n");
   rsprintf("{\n");
   sprintf(str, loc("Really remove user %s?"), user);
   rsprintf("    var subm = confirm(\"%s\");\n", str);
   rsprintf("    return subm;\n");
   rsprintf("}\n\n");
   rsprintf("//-->\n");
   rsprintf("</script>\n\n");

   /*---- title ----*/

   show_standard_title(logbook, "", 0);

   /*---- activation notice ----*/
   if (isparam("cmd") && strieq(getparam("cmd"), loc("Activate"))) {
      rsprintf("<tr><td class=\"notifymsg\" colspan=2>\n");
      sprintf(str, " <b>%s &lt;%s&gt;</b>", getparam("new_user_name"), getparam("new_user_email"));
      rsprintf(loc("Activation notice has been sent to %s"), str);
      rsprintf("</tr>\n");
   }

   /*---- menu buttons ----*/

   rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");

   rsprintf("<input type=hidden name=cmd value=\"%s\">\n", loc("Config"));      // for select javascript
   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Save"));
   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Back"));
   rsprintf("<input type=hidden name=config value=\"%s\">\n", user);

   rsprintf("</span></td></tr>\n\n");

   /* table for two-column items */
   rsprintf("<tr><td class=\"form2\">");
   rsprintf("<table width=\"100%%\" cellspacing=0>\n");

   /*---- if admin user, show user list ----*/

   if (is_admin_user(logbook, getparam("unm"))) {
      rsprintf("<input type=hidden name=admin value=1>\n");
      rsprintf("<tr><td nowrap width=\"10%%\">%s:</td>\n", loc("Select user"));
      rsprintf("<td><select name=cfg_user onChange=\"document.form1.submit()\">\n");

      /* count user list */
      for (n = 0;; n++) {
         if (!enum_user_line(lbs, n, str, sizeof(str)))
            break;
      }

      /* allocate list of users and populate it */
      user_list = xcalloc(sizeof(char *), n);
      for (i = 0; i < n; i++)
         user_list[i] = xcalloc(NAME_LENGTH, 1);

      for (i = 0; i < n; i++)
         enum_user_line(lbs, i, user_list[i], NAME_LENGTH);

      /* sort list */
      qsort(user_list, n, sizeof(char *), ascii_compare);

      for (i = 0; i < n; i++) {
         if (strcmp(user_list[i], user) == 0)
            rsprintf("<option selected value=\"%s\">%s\n", user_list[i], user_list[i]);
         else
            rsprintf("<option value=\"%s\">%s\n", user_list[i], user_list[i]);
      }

      for (i = 0; i < n; i++)
         xfree(user_list[i]);
      xfree(user_list);

      rsprintf("</select>\n");

      /* show "update" button only of javascript is not enabled */
      rsprintf("<noscript>\n");
      rsprintf("<input type=submit value=\"%s\">\n", loc("Update"));
      rsprintf("</noscript>\n");
   }

   /*---- entry form ----*/

   rsprintf("<tr><td nowrap width=\"15%%\">%s:</td>\n", loc("Login name"));

   if (get_user_line(lbs, user, password, full_name, user_email, email_notify, NULL) != 1)
      sprintf(str, loc("User [%s] has been deleted"), user);
   else
      strlcpy(str, user, sizeof(str));

   rsprintf("<td><input type=text size=40 name=new_user_name value=\"%s\"></td></tr>\n", str);

   rsprintf("<tr><td nowrap width=\"15%%\">%s:</td>\n", loc("Full name"));
   rsprintf("<td><input type=text size=40 name=new_full_name value=\"%s\"></tr>\n", full_name);

   rsprintf("<tr><td nowrap width=\"15%%\">Email:</td>\n");
   rsprintf("<td><input type=text size=40 name=new_user_email value=\"%s\"></td></tr>\n", user_email);

   for (i = n = 0; lb_list[i].name[0]; i++) {

      if (!getcfg_topgroup() || strieq(getcfg_topgroup(), lb_list[i].top_group)) {

         /* check if user has access */
         if (!isparam("unm") || check_login_user(&lb_list[i], getparam("unm"))) {

            /* check if emails are enabled for this logbook */
            if (!getcfg(lb_list[i].name, "Suppress email to users", str, sizeof(str)) || atoi(str) == 0)
               n++;
         }
      }
   }

   if (n > 0) {

      rsprintf("<tr><td width=\"15%%\">%s:\n", loc("Subscribe to logbooks"));

      rsprintf("<br><span class=\"selcomment\"><b>(%s)</b></span>\n",
               loc("enable automatic email notifications"));

      rsprintf("<td>\n");

      for (i = 0; lb_list[i].name[0]; i++) {

         if (!getcfg_topgroup() || strieq(getcfg_topgroup(), lb_list[i].top_group)) {

            /* check if user has access */
            if (!isparam("unm") || check_login_user(&lb_list[i], getparam("unm"))) {

               /* check if emails are enabled for this logbook */
               if (!getcfg(lb_list[i].name, "Suppress email to users", str, sizeof(str)) || atoi(str) == 0) {
                  if (email_notify[i])
                     rsprintf("<input type=checkbox checked id=\"lb%d\" name=\"sub_lb%d\" value=\"1\">\n", i,
                              i);
                  else
                     rsprintf("<input type=checkbox id=\"lb%d\" name=\"sub_lb%d\" value=\"1\">\n", i, i);
                  rsprintf("<label for=\"lb%d\">%s</label><br>\n", i, lb_list[i].name);
               }
            }
         }
      }
   }

   if (n > 2) {
      rsprintf("<script language=\"JavaScript\" type=\"text/javascript\">\n");
      rsprintf("<!--\n");
      rsprintf("function SetNone()\n");
      rsprintf("  {\n");
      rsprintf("  for (var i = 0; i < document.form1.elements.length; i++)\n");
      rsprintf("    {\n");
      rsprintf("    if( document.form1.elements[i].type == 'checkbox' )\n");
      rsprintf("      document.form1.elements[i].checked = false;\n");
      rsprintf("    }\n");
      rsprintf("  }\n");
      rsprintf("function SetAll()\n");
      rsprintf("  {\n");
      rsprintf("  for (var i = 0; i < document.form1.elements.length; i++)\n");
      rsprintf("    {\n");
      rsprintf("    if( document.form1.elements[i].type == 'checkbox' )\n");
      rsprintf("      document.form1.elements[i].checked = true;\n");
      rsprintf("    }\n");
      rsprintf("  }\n");
      rsprintf("//-->\n");
      rsprintf("</script>\n");

      rsprintf("<input type=button value=\"%s\" onClick=\"SetAll();\">\n", loc("Set all"));
      rsprintf("<input type=button value=\"%s\" onClick=\"SetNone();\">\n", loc("Set none"));
   }

   rsprintf("</td></tr>\n");

   rsprintf("</table></td></tr>\n");

   rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");

   if (is_admin_user(logbook, getparam("unm")) || !getcfg(logbook, "allow password change", str, sizeof(str))
       || atoi(str) == 1)
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Change password"));

   rsprintf("<input type=submit name=cmd value=\"%s\" onClick=\"return chkrem();\">\n", loc("Remove user"));

   if (is_admin_user(logbook, getparam("unm"))) {
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("New user"));
      sprintf(str, loc("Change config file"));
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", str);
   }

   /* hidden field for password */
   rsprintf("<input type=hidden name=hpwd value=\"%s\">\n", password);

   rsprintf("</span></td></tr></table>\n\n");
   show_bottom_text(lbs);
   rsprintf("</form></body></html>\r\n");
}

/*------------------------------------------------------------------*/

void show_forgot_pwd_page(LOGBOOK * lbs)
{
   int i;
   char str[1000], str2[1000], login_name[256], full_name[256], user_email[256], name[256], pwd[256],
       redir[256], pwd_encrypted[256], smtp_host[256], mail_from[256], mail_from_name[256], subject[256],
       mail_text[1000], url[1000], error[1000];

   if (isparam("login_name")) {
      /* seach in pwd file */

      strcpy(name, getparam("login_name"));

      for (i = 0;; i++) {
         if (!enum_user_line(lbs, i, login_name, sizeof(login_name)))
            break;

         get_user_line(lbs, login_name, NULL, full_name, user_email, NULL, NULL);

         if (strieq(name, login_name) || strieq(name, full_name) || strieq(name, user_email)) {
            if (user_email[0] == 0) {
               sprintf(str, loc("No Email address registered with user name <i>\"%s\"</i>"), name);
               strencode2(str2, str, sizeof(str2));
               show_error(str2);
               return;
            }

            /* create random password */
            srand((unsigned int) time(NULL));
            for (i = 0; i < 8; i++)
               str[i] = 'A' + (rand() % 25);
            str[i] = 0;
            base64_encode((unsigned char *) str, (unsigned char *) pwd, sizeof(pwd));
            do_crypt(pwd, pwd_encrypted, sizeof(pwd_encrypted));

            /* send email with new password */
            if (!getcfg("global", "SMTP host", smtp_host, sizeof(smtp_host))) {
               show_error(loc("No SMTP host defined in [global] section of configuration file"));
               return;
            }

            /* try to get URL from referer */
            if (!getcfg("global", "URL", url, sizeof(url))) {
               if (referer[0])
                  strcpy(url, referer);
               else {
                  if (elog_tcp_port == 80) {
                     if (_ssl_flag)
                        sprintf(url, "https://%s/", http_host);
                     else
                        sprintf(url, "http://%s/", http_host);
                  } else {
                     if (_ssl_flag)
                        sprintf(url, "https://%s:%d/", http_host, elog_tcp_port);
                     else
                        sprintf(url, "http://%s:%d/", http_host, elog_tcp_port);
                  }
               }
            } else {
               if (url[strlen(url) - 1] != '/')
                  strlcat(url, "/", sizeof(url));
               if (lbs) {
                  strlcat(url, lbs->name_enc, sizeof(url));
                  strlcat(url, "/", sizeof(url));
               }
            }

            url_slash_encode(pwd, sizeof(pwd));
            sprintf(redir, "?cmd=%s&oldpwd=%s", loc("Change password"), pwd);
            url_encode(redir, sizeof(redir));
            sprintf(str, "?redir=%s&uname=%s&upassword=%s", redir, login_name, pwd);
            strlcat(url, str, sizeof(url));

            retrieve_email_from(lbs, mail_from, mail_from_name, NULL);

            if (lbs)
               sprintf(subject, loc("Password recovery for ELOG %s"), lbs->name);
            else
               sprintf(subject, loc("Password recovery for ELOG %s"), http_host);

            mail_text[0] = 0;
            compose_email_header(lbs, subject, mail_from_name, user_email, NULL, mail_text,
                                 sizeof(mail_text), 1, 0, NULL, 0, 0);

            strlcat(mail_text, "\r\n", sizeof(mail_text));
            sprintf(mail_text + strlen(mail_text), loc("A new password has been created for you on host %s"),
                    http_host);
            strlcat(mail_text, ".\r\n", sizeof(mail_text));
            strlcat(mail_text, loc("Please log on by clicking on following link and change your password"),
                    sizeof(mail_text));
            strlcat(mail_text, ":\r\n\r\n", sizeof(mail_text));
            strlcat(mail_text, url, sizeof(mail_text));
            strlcat(mail_text, "\r\n\r\n", sizeof(mail_text));
            sprintf(mail_text + strlen(mail_text), "ELOG Version %s\r\n", VERSION);

            if (sendmail(lbs, smtp_host, mail_from, user_email, mail_text, error, sizeof(error)) != -1) {
               /* save new password */
               change_pwd(lbs, login_name, pwd_encrypted);

               /* show notification web page */
               show_standard_header(lbs, FALSE, loc("ELOG password recovery"), "", FALSE, NULL, NULL);

               rsprintf("<table class=\"dlgframe\" cellspacing=0 align=center>");
               rsprintf("<tr><td class=\"dlgtitle\">\n");
               rsprintf(loc("Email notification"));
               rsprintf("</td></tr>\n");

               rsprintf("<tr><td align=center class=\"dlgform\">\n");
               rsprintf(loc("A new password for user <i>\"%s\"</i> has been sent to %s"), full_name,
                        user_email);
               rsprintf("</td></tr></table>\n");
               show_bottom_text(lbs);
               rsprintf("</body></html>\n");
               return;
            } else {
               sprintf(str, loc("Error sending Email via <i>\"%s\"</i>"), smtp_host);
               strlcat(str, ": ", sizeof(str));
               strlcat(str, error, sizeof(str));
               show_error(str);
               return;
            }
         }
      }

      if (strchr(name, '@'))
         sprintf(str, loc("Email address <i>\"%s\"</i> not registered"), name);
      else
         sprintf(str, loc("User name <i>\"%s\"</i> not registered"), name);

      strencode2(str2, str, sizeof(str2));
      show_error(str2);

      return;
   } else {
      /*---- header ----*/

      show_standard_header(lbs, TRUE, loc("ELOG password recovery"), NULL, FALSE, NULL, NULL);

      rsprintf("<table class=\"dlgframe\" cellspacing=0 align=center>");

      /*---- entry form ----*/

      rsprintf("<tr><td class=\"dlgtitle\">%s</td></tr>\n", loc("Enter your user name or email address"));

      rsprintf("<tr><td align=center class=\"dlgform\">\n");
      rsprintf("<input type=hidden name=cmd value=%s>\n", loc("Forgot"));
      rsprintf("<input type=text size=40 name=login_name></td></tr>\n");

      rsprintf("<tr><td align=center class=\"dlgform\"><input type=submit value=\"%s\">\n", loc("Submit"));

      rsprintf("</td></tr></table>\n\n");
      show_bottom_text(lbs);
      rsprintf("</form></body></html>\r\n");
   }
}

/*------------------------------------------------------------------*/

void show_new_user_page(LOGBOOK * lbs)
{
   /*---- header ----*/

   show_html_header(lbs, TRUE, loc("ELOG new user"), TRUE, FALSE, NULL, FALSE);
   rsprintf("<body><center><br><br>\n");
   show_top_text(lbs);
   rsprintf("<form name=form1 method=\"GET\" action=\".\">\n\n");

   /*---- title ----*/

   if (lbs)
      show_standard_title(lbs->name, "", 1);
   else
      show_standard_title("ELOG", "", 1);

   /* table for two-column items */
   rsprintf("<tr><td class=\"form2\">");
   rsprintf("<table width=\"100%%\" cellspacing=0>\n");

   /*---- entry form ----*/

   rsprintf("<tr><td nowrap>%s:</td>\n", loc("Login name"));
   rsprintf("<td><input type=text size=40 name=new_user_name></td>\n");
   rsprintf("<td nowrap align=left><font size=2><i>(%s)</i></font></td></tr>\n",
            loc("name may not contain blanks"));

   rsprintf("<tr><td nowrap>%s:</td>\n", loc("Full name"));
   rsprintf("<td colspan=2><input type=text size=40 name=new_full_name></tr>\n");

   rsprintf("<tr><td nowrap>Email:</td>\n");
   rsprintf("<td colspan=2><input type=text size=40 name=new_user_email></tr>\n");

   rsprintf("<tr><td nowrap>%s:</td>\n", loc("Password"));
   rsprintf("<td colspan=2><input type=password size=40 name=newpwd>\n");

   rsprintf("<tr><td nowrap>%s:</td>\n", loc("Retype password"));
   rsprintf("<td colspan=2><input type=password size=40 name=newpwd2>\n");

   rsprintf("</td></tr></table>\n");

   /*---- menu buttons ----*/

   rsprintf("<tr><td class=\"menucenter\">\n");
   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Save"));
   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Cancel"));
   rsprintf("</td></tr>\n\n");

   rsprintf("</td></tr></table>\n\n");
   rsprintf("</form></center></body></html>\r\n");
}

/*------------------------------------------------------------------*/

void show_elog_delete(LOGBOOK * lbs, int message_id)
{
   int i, status, reply = 0, next, nsel;
   char str[256], str2[256], in_reply_to[80], reply_to[MAX_REPLY_TO * 10], owner[256];
   char attrib[MAX_N_ATTR][NAME_LENGTH], mode[80];

   /* redirect if confirm = NO */
   if (isparam("confirm") && strcmp(getparam("confirm"), loc("No")) == 0) {
      if (message_id) {
         sprintf(str, "%d", message_id);
         redirect(lbs, str);
      } else {
         strlcpy(str, isparam("lastcmd") ? getparam("lastcmd") : "", sizeof(str));
         url_decode(str);
         redirect(lbs, str);
      }
      return;
   }

   if (isparam("elmode"))
      strlcpy(mode, getparam("elmode"), sizeof(mode));

   if (isparam("confirm")) {
      if (strcmp(getparam("confirm"), loc("Yes")) == 0) {
         if (message_id) {
            /* delete message */
            if (strieq(mode, "threaded"))
               status = el_delete_message(lbs, message_id, TRUE, NULL, TRUE, TRUE);
            else
               status = el_delete_message(lbs, message_id, TRUE, NULL, TRUE, FALSE);
            if (status != EL_SUCCESS) {
               sprintf(str, "%s = %d", loc("Error deleting message: status"), status);
               show_error(str);
               return;
            } else {
               strlcpy(str, isparam("nextmsg") ? getparam("nextmsg") : "", sizeof(str));
               if (atoi(str) == 0)
                  sprintf(str, "%d", el_search_message(lbs, EL_LAST, 0, TRUE));
               if (atoi(str) == 0)
                  redirect(lbs, "");
               else
                  redirect(lbs, str);
               return;
            }
         } else {
            if (isparam("nsel")) {
               for (i = 0; i < atoi(getparam("nsel")); i++) {
                  sprintf(str, "s%d", i);
                  if (isparam(str)) {
                     if (strieq(mode, "threaded"))
                        status = el_delete_message(lbs, atoi(getparam(str)), TRUE, NULL, TRUE, TRUE);
                     else
                        status = el_delete_message(lbs, atoi(getparam(str)), TRUE, NULL, TRUE, FALSE);
                  }
               }
            }

            redirect(lbs, isparam("lastcmd") ? getparam("lastcmd") : "");
            return;
         }
      }
   } else {
      /* check if at least one message is selected */
      if (!message_id) {
         nsel = isparam("nsel") ? atoi(getparam("nsel")) : 0;
         for (i = 0; i < nsel; i++) {
            sprintf(str, "s%d", i);
            if (isparam(str))
               break;
         }
         if (i == nsel) {
            show_error(loc("No entry selected for deletion"));
            return;
         }
      }

      /* check for author */
      if (getcfg(lbs->name, "Restrict edit", str, sizeof(str)) && atoi(str) == 1) {
         /* get message for reply/edit */

         el_retrieve(lbs, message_id, NULL, attr_list, attrib, lbs->n_attr, NULL, NULL,
                     NULL, NULL, NULL, NULL, NULL);

         if (!is_author(lbs, attrib, owner)) {
            sprintf(str, loc("Only user <i>%s</i> can delete this entry"), owner);
            strencode2(str2, str, sizeof(str2));
            show_error(str2);
            return;
         }
      }

      /* header */
      if (message_id)
         sprintf(str, "%d", message_id);
      else
         str[0] = 0;
      show_standard_header(lbs, TRUE, "Delete ELog entry", str, FALSE, NULL, NULL);

      rsprintf("<table class=\"dlgframe\" cellspacing=0 align=center>");
      rsprintf("<tr><td class=\"dlgtitle\">\n");

      /* define hidden field for command */
      rsprintf("<input type=hidden name=cmd value=\"%s\">\n", loc("Delete"));

      if (!message_id) {
         rsprintf("%s</td></tr>\n", loc("Are you sure to delete these messages?"));

         rsprintf("<tr><td align=center class=\"dlgform\">\n");
         if (isparam("nsel"))
            rsprintf("<input type=hidden name=nsel value=%s>\n", getparam("nsel"));

         if (isparam("lastcmd")) {
            strlcpy(str, getparam("lastcmd"), sizeof(str));
            rsprintf("<input type=hidden name=lastcmd value=\"%s\">\n", str);
         }

         if (isparam("nsel")) {
            reply = FALSE;
            for (i = 0; i < atoi(getparam("nsel")); i++) {
               sprintf(str, "s%d", i);
               if (isparam(str)) {
                  rsprintf("#%s ", getparam(str));
                  rsprintf("<input type=hidden name=%s value=%s>\n", str, getparam(str));
               }

               if (!reply) {
                  el_retrieve(lbs, isparam(str) ? atoi(getparam(str)) : 0,
                              NULL, attr_list, NULL, 0, NULL, NULL, in_reply_to, reply_to, NULL, NULL, NULL);
                  if (reply_to[0])
                     reply = TRUE;
               }
            }
         }

         rsprintf("</td></tr>\n");

         if (reply && strieq(mode, "threaded"))
            rsprintf("<tr><td align=center class=\"dlgform\">%s</td></tr>\n", loc("and all their replies"));

      } else {
         rsprintf("%s</td></tr>\n", loc("Are you sure to delete this entry?"));

         /* check for replies */

         /* retrieve original message */
         el_retrieve(lbs, message_id, NULL, attr_list, NULL, 0, NULL, NULL, in_reply_to, reply_to, NULL,
                     NULL, NULL);

         if (reply_to[0])
            rsprintf("<tr><td align=center class=\"dlgform\">#%d<br>%s</td></tr>\n", message_id,
                     loc("and all its replies"));
         else
            rsprintf("<tr><td align=center class=\"dlgform\">#%d</td></tr>\n", message_id);

         /* put link to next message */
         next = el_search_message(lbs, EL_NEXT, message_id, TRUE);

         rsprintf("<input type=hidden name=nextmsg value=%d>\n", next);
      }

      rsprintf("<tr><td align=center class=\"dlgform\"><input type=submit name=confirm value=\"%s\">\n",
               loc("Yes"));
      rsprintf("<input type=submit name=confirm value=\"%s\">\n", loc("No"));
      rsprintf("</td></tr>\n\n");
   }

   rsprintf("</table>\n");
   show_bottom_text(lbs);
   rsprintf("</body></html>\r\n");
}

/*------------------------------------------------------------------*/

void show_logbook_delete(LOGBOOK * lbs)
{
   char str[256];

   /* redirect if confirm = NO */
   if (isparam("confirm") && strcmp(getparam("confirm"), loc("No")) == 0) {

      redirect(lbs, "?cmd=Config");
      return;
   }

   if (isparam("confirm")) {
      if (strcmp(getparam("confirm"), loc("Yes")) == 0) {

         /* delete logbook */
         str[0] = 0;
         delete_logbook(lbs, str);
         if (str[0])
            show_error(str);
         else
            redirect(NULL, "../");
         return;
      }

   } else {

      strcpy(str, "Delete logbook");
      show_standard_header(lbs, TRUE, str, "", FALSE, NULL, NULL);

      rsprintf("<table class=\"dlgframe\" cellspacing=0 align=center>");
      rsprintf("<tr><td class=\"dlgtitle\">\n");

      /* define hidden field for command */
      rsprintf("<input type=hidden name=cmd value=\"%s\">\n", loc("Delete this logbook"));

      sprintf(str, loc("Are you sure to delete logbook \"%s\"?"), lbs->name);
      rsprintf("%s</td></tr>\n", str);

      rsprintf("<tr><td align=center class=\"dlgform\">");
      rsprintf("<input type=submit name=confirm value=\"%s\">\n", loc("Yes"));
      rsprintf("<input type=submit name=confirm value=\"%s\">\n", loc("No"));
      rsprintf("</td></tr>\n\n");
   }

   rsprintf("</table>\n");
   show_bottom_text(lbs);
   rsprintf("</body></html>\r\n");
}

/*------------------------------------------------------------------*/

void show_logbook_rename(LOGBOOK * lbs)
{
   int i;
   char str[256], lbn[256];

   if (isparam("lbname")) {

      /* check if logbook name exists already */
      strcpy(lbn, getparam("lbname"));
      for (i = 0; lb_list[i].name[0]; i++)
         if (strieq(lbn, lb_list[i].name)) {
            sprintf(str, loc("Logbook \"%s\" exists already, please choose different name"), lbn);
            show_error(str);
            return;
         }

      if (!rename_logbook(lbs, getparam("lbname")))
         return;

      sprintf(str, "../%s/?cmd=Config", getparam("lbname"));
      redirect(NULL, str);
      return;

   } else {

      strcpy(str, loc("Rename logbook"));
      show_standard_header(lbs, TRUE, str, "", FALSE, NULL, NULL);

      rsprintf("<table class=\"dlgframe\" cellspacing=0 align=center>");
      rsprintf("<tr><td class=\"dlgtitle\">\n");

      /* define hidden field for command */
      rsprintf("<input type=hidden name=cmd value=\"%s\">\n", loc("Rename this logbook"));

      rsprintf("%s</td></tr>\n", loc("Enter new logbook name"));

      rsprintf("<tr><td align=center class=\"dlgform\">");
      rsprintf("<input type=text name=\"lbname\"><br><br>\n");
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Rename this logbook"));
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Cancel"));
      rsprintf("</td></tr>\n\n");
   }

   rsprintf("</table>\n");
   show_bottom_text(lbs);
   rsprintf("</body></html>\r\n");
}

/*------------------------------------------------------------------*/

void show_logbook_new(LOGBOOK * lbs)
{
   char str[256], lbn[256];
   int i;

   if (isparam("lbname")) {

      /* check if logbook name exists already */
      strcpy(lbn, getparam("lbname"));
      for (i = 0; lb_list[i].name[0]; i++)
         if (strieq(lbn, lb_list[i].name)) {
            sprintf(str, loc("Logbook \"%s\" exists already, please choose different name"), lbn);
            show_error(str);
            return;
         }

      /* create new logbook */
      if (!create_logbook(lbs, getparam("lbname"), getparam("template")))
         return;

      strcpy(lbn, getparam("lbname"));
      url_encode(lbn, sizeof(lbn));
      sprintf(str, "../%s/?cmd=Config", lbn);
      for (i = 0; lb_list[i].name[0]; i++)
         if (strieq(lbn, lb_list[i].name))
            break;
      if (lb_list[i].name[0])
         redirect(&lb_list[i], str);
      else
         redirect(NULL, str);
      return;
   }

   show_standard_header(lbs, TRUE, "Delete Logbook", "", FALSE, NULL, NULL);

   rsprintf("<table class=\"dlgframe\" cellspacing=0 align=center>");
   rsprintf("<tr><td class=\"dlgtitle\">\n");

   /* define hidden field for command */
   rsprintf("<input type=hidden name=cmd value=\"%s\">\n", loc("Create new logbook"));
   rsprintf("%s</td></tr>\n", loc("Create new logbook"));

   rsprintf("<tr><td align=center class=\"dlgform\">\n");
   rsprintf("%s :&nbsp;&nbsp;", loc("Logbook name"));
   rsprintf("<input type=text name=lbname>\n");
   rsprintf("</td></tr>\n");

   rsprintf("<tr><td align=center class=\"dlgform\">\n");
   rsprintf("%s : \n", loc("Use existing logbook as template"));
   rsprintf("<select name=template>\n");
   rsprintf("<option value=\"\">- %s -\n", loc("none"));
   for (i = 0; lb_list[i].name[0]; i++)
      rsprintf("<option value=\"%s\">%s\n", lb_list[i].name, lb_list[i].name);
   rsprintf("</select>\n");
   rsprintf("</td></tr>\n\n");

   rsprintf("<tr><td align=center class=\"dlgform\">\n");
   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Create new logbook"));
   rsprintf("<input type=submit name=tmp value=\"%s\">\n", loc("Cancel"));
   rsprintf("</td></tr>\n\n");

   rsprintf("</table>\n");
   show_bottom_text(lbs);
   rsprintf("</body></html>\r\n");
}

/*------------------------------------------------------------------*/

int show_download_page(LOGBOOK * lbs, char *path)
{
   char file_name[256], error_str[256];
   int index, message_id, fh, i, size, delta;
   char message[TEXT_SIZE + 1000], *p, *buffer;

   if (stricmp(path, "gbl") == 0) {

      /* return complete config file */
      load_config_section(NULL, &buffer, error_str);
      if (error_str[0]) {
         rsprintf("<h2>%s</h2>", error_str);
         return EL_FILE_ERROR;
      }

      size = strlen(buffer);
      strlcpy(message, buffer, sizeof(message));
      xfree(buffer);

   } else {

      message_id = atoi(path);

      if (message_id == 0) {

         /* return config */
         load_config_section(lbs->name, &buffer, error_str);
         if (error_str[0]) {
            rsprintf("<h2>%s</h2>", error_str);
            return EL_FILE_ERROR;
         }

         size = strlen(buffer);
         strlcpy(message, buffer, sizeof(message));
         xfree(buffer);

      } else {

         /* return entry */

         for (index = 0; index < *lbs->n_el_index; index++)
            if (lbs->el_index[index].message_id == message_id)
               break;

         if (index == *lbs->n_el_index)
            return EL_NO_MSG;

         sprintf(file_name, "%s%s", lbs->data_dir, lbs->el_index[index].file_name);
         fh = open(file_name, O_RDWR | O_BINARY, 0644);
         if (fh < 0)
            return EL_FILE_ERROR;

         lseek(fh, lbs->el_index[index].offset, SEEK_SET);
         i = my_read(fh, message, sizeof(message) - 1);
         if (i <= 0) {
            close(fh);
            return EL_FILE_ERROR;
         }

         message[i] = 0;
         close(fh);

         /* decode message size */
         p = strstr(message + 8, "$@MID@$:");
         if (p == NULL)
            size = strlen(message);
         else
            size = p - message;

         message[size] = 0;
      }
   }

   show_plain_header(size, "export.txt");

   /* increase return buffer size if file too big */
   if (size + 1 >= return_buffer_size - (int) strlen(return_buffer)) {
      delta = size - (return_buffer_size - strlen(return_buffer)) + 1000;

      return_buffer = xrealloc(return_buffer, return_buffer_size + delta);
      memset(return_buffer + return_buffer_size, 0, delta);
      return_buffer_size += delta;
   }

   return_length = strlen(return_buffer) + size;
   strlcat(return_buffer, message, return_buffer_size);

   return EL_SUCCESS;
}

/*------------------------------------------------------------------*/

int download_config()
{
   char error_str[256];
   int size, delta;
   char message[TEXT_SIZE + 1000], *buffer;

   /* return complete config file */
   load_config_section(NULL, &buffer, error_str);
   if (error_str[0]) {
      rsprintf("Error loading configuration file: %s", error_str);
      return EL_FILE_ERROR;
   }

   size = strlen(buffer);
   strlcpy(message, buffer, sizeof(message));
   xfree(buffer);

   show_plain_header(size, "export.txt");

   /* increase return buffer size if file too big */
   if (size + 1 >= return_buffer_size - (int) strlen(return_buffer)) {
      delta = size - (return_buffer_size - strlen(return_buffer)) + 1000;

      return_buffer = xrealloc(return_buffer, return_buffer_size + delta);
      memset(return_buffer + return_buffer_size, 0, delta);
      return_buffer_size += delta;
   }

   return_length = strlen(return_buffer) + size;
   strlcat(return_buffer, message, return_buffer_size);

   return EL_SUCCESS;
}

/*------------------------------------------------------------------*/

void show_import_page_csv(LOGBOOK * lbs)
{
   char str[256], str2[256];

   /*---- header ----*/

   show_html_header(lbs, FALSE, loc("ELOG CSV import"), TRUE, FALSE, NULL, FALSE);

   rsprintf("<body><form method=\"POST\" action=\"./\" enctype=\"multipart/form-data\">\n");

   /*---- title ----*/

   show_standard_title(lbs->name, "", 0);

   /*---- menu buttons ----*/

   rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");

   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Cancel"));
   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Import"));

   rsprintf("</span></td></tr>\n\n");

   /* table for two-column items */
   rsprintf("<tr><td class=\"form2\">");
   rsprintf("<table width=\"100%%\" cellspacing=0>\n");

   /*---- entry form ----*/

   rsprintf("<tr><td class=\"attribname\" nowrap width=\"10%%\">%s:</td>\n", loc("Field separator"));
   rsprintf("<td class=\"attribvalue\">");

   str[0] = 0;
   if (isparam("sep"))
      strlcpy(str, getparam("sep"), sizeof(str));

   if (str[0] == 0)
      rsprintf("<input type=\"radio\" checked id=\"comma\" name=\"sep\" value=\"auto\">");
   else
      rsprintf("<input type=\"radio\" id=\"comma\" name=\"sep\" value=\"auto\">");
   rsprintf("<label for=\"comma\">%s</label>\n", loc("Auto detect"));

   if (str[0] == ',')
      rsprintf("<input type=\"radio\" checked id=\"comma\" name=\"sep\" value=\",\">");
   else
      rsprintf("<input type=\"radio\" id=\"comma\" name=\"sep\" value=\",\">");
   rsprintf("<label for=\"comma\">%s (,)</label>\n", loc("Comma"));

   if (str[0] == ';')
      rsprintf("<input type=\"radio\" checked id=\"semi\" name=\"sep\" value=\";\">");
   else
      rsprintf("<input type=\"radio\" id=\"semi\" name=\"sep\" value=\";\">");
   rsprintf("<label for=\"semi\">%s (;)</label>\n", loc("Semicolon"));

   rsprintf("</td></tr>\n");

   rsprintf("<tr><td class=\"attribname\" nowrap width=\"10%%\">%s:</td>\n", loc("Options"));
   rsprintf("<td class=\"attribvalue\">");

   if (isparam("head"))
      rsprintf("<input type=checkbox checked id=\"head\" name=\"head\" value=\"1\">\n");
   else
      rsprintf("<input type=checkbox id=\"head\" name=\"head\" value=\"1\">\n");
   rsprintf("<label for=\"head\">%s</label><br>\n", loc("Derive attributes from CSV file"));

   if (isparam("ignore"))
      rsprintf("<input type=checkbox checked id=\"ignore\" name=\"ignore\" value=\"1\">\n");
   else
      rsprintf("<input type=checkbox id=\"ignore\" name=\"ignore\" value=\"1\">\n");
   rsprintf("<label for=\"ignore\">%s</label><br>\n", loc("Ignore first line"));

   rsprintf("<input type=checkbox id=\"preview\" name=\"preview\" value=\"1\">\n");
   rsprintf("<label for=\"preview\">%s</label><br>\n", loc("Preview import"));

   if (isparam("filltext"))
      rsprintf("<input type=checkbox checked id=\"filltext\" name=\"filltext\" value=\"1\">\n");
   else
      rsprintf("<input type=checkbox id=\"filltext\" name=\"filltext\" value=\"1\">\n");
   strcpy(str, loc("text"));
   sprintf(str2, loc("Column header '%s' must be present in CSV file"), str);
   rsprintf("<label for=\"filltext\">%s (%s)</label><br>\n", loc("Fill text body"), str2);

   rsprintf("</td></tr>\n");

   rsprintf("<tr><td class=\"attribname\" nowrap width=\"10%%\">%s:</td>\n", loc("CSV filename"));

   rsprintf("<td class=\"attribvalue\">");

   if (isparam("csvfile"))
      rsprintf("<b>%s</b>:<br>\n", loc("Please re-enter filename"));

   rsprintf("<input type=\"file\" size=\"60\" maxlength=\"200\" name=\"csvfile\" ");
   if (isparam("csvfile"))
      rsprintf(" value=\"%s\" ", getparam("csvfile"));
   rsprintf("accept=\"filetype/*\"></td></tr>\n");

   rsprintf("</table></td></tr></table>\n\n");
   show_bottom_text(lbs);
   rsprintf("</form></body></html>\r\n");
}

/*------------------------------------------------------------------*/

void show_import_page_xml(LOGBOOK * lbs)
{
   /*---- header ----*/

   show_html_header(lbs, FALSE, loc("ELOG XML import"), TRUE, FALSE, NULL, FALSE);

   rsprintf("<body><form method=\"POST\" action=\"./\" enctype=\"multipart/form-data\">\n");

   /*---- title ----*/

   show_standard_title(lbs->name, "", 0);

   /*---- menu buttons ----*/

   rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");

   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Cancel"));
   rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Import"));

   rsprintf("</span></td></tr>\n\n");

   /* table for two-column items */
   rsprintf("<tr><td class=\"form2\">");
   rsprintf("<table width=\"100%%\" cellspacing=0>\n");

   /*---- entry form ----*/

   rsprintf("<tr><td class=\"attribname\" nowrap width=\"10%%\">%s:</td>\n", loc("Options"));
   rsprintf("<td class=\"attribvalue\">");

   if (isparam("head"))
      rsprintf("<input type=checkbox checked id=\"head\" name=\"head\" value=\"1\">\n");
   else
      rsprintf("<input type=checkbox id=\"head\" name=\"head\" value=\"1\">\n");
   rsprintf("<label for=\"head\">%s</label><br>\n", loc("Derive attributes from XML file"));

   if (isparam("keep"))
      rsprintf("<input type=checkbox checked id=\"keep\" name=\"keep\" value=\"1\">\n");
   else
      rsprintf("<input type=checkbox id=\"keep\" name=\"keep\" value=\"1\">\n");
   rsprintf("<label for=\"keep\">%s</label><br>\n",
            loc
            ("Keep original entry IDs (may overwrite existing entries, but is required if imported entries contain replies)"));

   rsprintf("<input type=checkbox id=\"preview\" name=\"preview\" value=\"1\">\n");
   rsprintf("<label for=\"preview\">%s</label><br>\n", loc("Preview import"));

   rsprintf("</td></tr>\n");

   rsprintf("<tr><td class=\"attribname\" nowrap width=\"10%%\">%s:</td>\n", loc("XML filename"));
   rsprintf("<td class=\"attribvalue\">");

   if (isparam("xmlfile"))
      rsprintf("<b>%s</b>:<br>\n", loc("Please re-enter filename"));

   rsprintf("<input type=\"file\" size=\"60\" maxlength=\"200\" name=\"xmlfile\" ");
   if (isparam("xmlfile"))
      rsprintf(" value=\"%s\" ", getparam("xmlfile"));
   rsprintf("accept=\"filetype/*\"></td></tr>\n");

   rsprintf("</table></td></tr></table>\n\n");
   show_bottom_text(lbs);
   rsprintf("</form></body></html>\r\n");

}

/*------------------------------------------------------------------*/

void csv_import(LOGBOOK * lbs, const char *csv, const char *csvfile)
{
   const char *p;
   char *line, *list;
   char str[256], date[80], sep[80];
   int i, j, n, n_attr, iline, n_imported, textcol, attr_offset;
   BOOL first, in_quotes, filltext;
   time_t ltime;

   list = xmalloc(MAX_N_ATTR * NAME_LENGTH);
   line = xmalloc(10000);

   first = TRUE;
   in_quotes = FALSE;
   iline = n_imported = 0;
   filltext = FALSE;
   textcol = -1;
   attr_offset = 0;

   strcpy(sep, ",");
   if (isparam("sep"))
      strcpy(sep, getparam("sep"));
   if (sep[0] == 0)
      strcpy(sep, ",");
   if (strieq(sep, "auto")) {

      /* count commas */
      for (i = 0, p = csv; p; i++) {
         p = strchr(p, ',');
         if (p)
            p++;
      }
      n = i;

      /* count semicolon */
      for (i = 0, p = csv; p; i++) {
         p = strchr(p, ';');
         if (p)
            p++;
      }

      strcpy(sep, i > n ? ";" : ",");
   }

   n_attr = lbs->n_attr;

   if (isparam("preview")) {

      /* title row */
      sprintf(str, loc("CSV import preview of %s"), csvfile);
      show_standard_header(lbs, TRUE, str, "./", FALSE, NULL, NULL);
      rsprintf("<table class=\"frame\" cellpadding=\"0\" cellspacing=\"0\">\n");
      rsprintf("<tr><td class=\"title1\">%s</td></tr>\n", str, str);

      /* menu buttons */
      rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Cancel"));
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("CSV Import"));

      /* hidden fields */
      rsprintf("<input type=hidden name=sep value=\"%s\">\n", sep);
      if (isparam("head"))
         rsprintf("<input type=hidden name=head value=\"%s\">\n", getparam("head"));
      if (isparam("ignore"))
         rsprintf("<input type=hidden name=ignore value=\"%s\">\n", getparam("ignore"));
      if (isparam("filltext"))
         rsprintf("<input type=hidden name=filltext value=\"%s\">\n", getparam("filltext"));
      rsprintf("<input type=hidden name=csvfile value=\"%s\">\n", csvfile);

      rsprintf("</span></td></tr>\n\n");

      rsprintf("<tr><td><table class=\"listframe\" width=\"100%%\" cellspacing=0>");
   }

   p = csv;

   do {
      for (i = 0; i < 10000 && *p; i++) {
         if (!in_quotes && (*p == '\r' || *p == '\n'))
            break;

         line[i] = *p++;
         if (line[i] == '"')
            in_quotes = !in_quotes;
      }

      line[i] = 0;
      while (*p == '\r' || *p == '\n')
         p++;
      if (!*p)
         break;

      memset(list, 0, MAX_N_ATTR * NAME_LENGTH);
      n = strbreak(line, (char (*)[NAME_LENGTH]) list, MAX_N_ATTR, sep, FALSE);

      if (n == MAX_N_ATTR) {
         sprintf(str, loc("Too many attributes in CSV file"));
         show_error(str);
      }

      /* ignore first line */
      if (first && isparam("ignore")) {
         first = FALSE;
         continue;
      }

      /* interprete date entries correctly */
      if (!(first && isparam("head"))) {
         for (i = attr_offset; i < n; i++) {
            if (attr_flags[i - attr_offset] & AF_DATE) {
               /* convert to seconds in Unix format */
               ltime = convert_date(list + i * NAME_LENGTH);
               if (ltime == 0) {
                  show_error(loc("Invalid date format"));
                  return;
               }
               sprintf(list + i * NAME_LENGTH, "%d", (int) ltime);
            }
            if (attr_flags[i - attr_offset] & AF_DATETIME) {
               /* convert to seconds in Unix format */
               ltime = convert_datetime(list + i * NAME_LENGTH);
               if (ltime == 0) {
                  show_error(loc("Invalid date format"));
                  return;
               }
               sprintf(list + i * NAME_LENGTH, "%d", (int) ltime);
            }
         }
      }

      /* check if text column is present */
      if (first && isparam("filltext") && atoi(getparam("filltext"))) {
         for (i = 0; i < n; i++)
            if (strieq(list + i * NAME_LENGTH, loc("text"))) {
               filltext = TRUE;
               textcol = i;
               break;
            }
      }

      /* derive attributes from first line */
      if (first && isparam("head")) {

         /* skip message ID and date attributes */
         for (i = attr_offset = 0; i < n; i++)
            if (strieq(list + i * NAME_LENGTH, "Message ID") || strieq(list + i * NAME_LENGTH, "Date"))
               attr_offset++;

         if (isparam("preview")) {
            rsprintf("<tr>\n");
            for (i = attr_offset; i < n; i++)
               if (i != textcol)
                  rsprintf("<th class=\"listtitle\">%s</th>\n", list + i * NAME_LENGTH);

            if (filltext)
               rsprintf("<th class=\"listtitle\">%s</th>\n", loc("text"));

            rsprintf("</tr>\n");

            if (filltext)
               n_attr = n - 1 - attr_offset;
            else
               n_attr = n - attr_offset;

         } else {
            for (i = j = attr_offset; i < n; i++)
               if (i != textcol)
                  strlcpy(attr_list[j++ - attr_offset], list + i * NAME_LENGTH, NAME_LENGTH);

            if (filltext) {
               if (!set_attributes(lbs, attr_list, n - 1 - attr_offset))
                  return;
               lbs->n_attr = n - 1 - attr_offset;
            } else {
               if (!set_attributes(lbs, attr_list, n - attr_offset))
                  return;
               lbs->n_attr = n - attr_offset;
            }
         }

      } else {

         if (isparam("preview")) {
            rsprintf("<tr>\n");
            for (i = j = attr_offset; i < n_attr; i++) {
               if (iline % 2 == 0)
                  rsputs("<td class=\"list1\">");
               else
                  rsputs("<td class=\"list2\">");

               /* skip text column */
               if (i == textcol)
                  j++;

               if (i >= n || !list[j * NAME_LENGTH])
                  rsputs("&nbsp;");
               else
                  rsputs(list + j * NAME_LENGTH);

               rsputs("</td>\n");
               j++;
            }

            if (filltext) {
               rsputs("<td class=\"summary\">");
               if (list[textcol * NAME_LENGTH])
                  rsputs(list + textcol * NAME_LENGTH);
               else
                  rsputs("&nbsp;");
               rsputs("</td>\n");
            }

            rsputs("</tr>\n");
            iline++;

         } else {

            if (!filltext) {
               /* submit entry */
               date[0] = 0;
               if (el_submit
                   (lbs, 0, FALSE, date, attr_list,
                    (char (*)[NAME_LENGTH]) (list + attr_offset * NAME_LENGTH), n_attr, "", "", "", "plain",
                    NULL, TRUE, NULL))
                  n_imported++;
            } else {
               strlcpy(line, list + textcol * NAME_LENGTH, 10000);
               insert_breaks(line, 78, 10000);

               for (i = textcol; i < n_attr + attr_offset; i++)
                  strlcpy(list + i * NAME_LENGTH, list + (i + 1) * NAME_LENGTH, NAME_LENGTH);

               /* submit entry */
               date[0] = 0;
               if (el_submit
                   (lbs, 0, FALSE, date, attr_list,
                    (char (*)[NAME_LENGTH]) (list + attr_offset * NAME_LENGTH), n_attr, line, "", "", "plain",
                    NULL, TRUE, NULL))
                  n_imported++;
            }
         }
      }

      first = FALSE;

   } while (*p);

   xfree(line);
   xfree(list);

   if (isparam("preview")) {
      rsprintf("</table></td></tr></table>\n");
      show_bottom_text(lbs);
      rsprintf("</form></body></html>\r\n");

      return;
   }

   sprintf(str, loc("%d entries successfully imported"), n_imported);
   show_elog_list(lbs, 0, 0, 0, TRUE, str);
}

/*------------------------------------------------------------------*/

void xml_import(LOGBOOK * lbs, const char *xml, const char *xmlfile)
{
   char str[256], date[80], error[256], encoding[256], *list, *p, in_reply_to[80],
       reply_to[MAX_REPLY_TO * 10], attachment[MAX_ATTACHMENTS][MAX_PATH_LENGTH], attachment_all[64
                                                                                                 *
                                                                                                 MAX_ATTACHMENTS];
   int i, j, index, n_attr, iline, n_imported, textcol, i_line, line_len, message_id, bedit;
   time_t ltime;
   PMXML_NODE root, entry;

   iline = n_imported = 0;
   textcol = -1;

   n_attr = lbs->n_attr;

   root = mxml_parse_buffer(xml, error, sizeof(error));
   if (root == NULL) {
      strencode2(str, error, sizeof(str));
      show_error(str);
      return;
   }

   root = mxml_find_node(root, "ELOG_LIST");
   if (root == NULL) {
      sprintf(str, loc("XML file does not contain %s element"), "&lt;ELOG_LIST&gt;");
      show_error(str);
      return;
   }

   entry = mxml_subnode(root, 0);

   if (mxml_find_node(entry, "MID") == NULL) {
      sprintf(str, loc("XML file does not contain %s element"), "&lt;MID&gt;");
      show_error(str);
      return;
   }

   if (mxml_find_node(entry, "DATE") == NULL) {
      sprintf(str, loc("XML file does not contain %s element"), "&lt;DATE&gt;");
      show_error(str);
      return;
   }

   if (mxml_find_node(entry, "ENCODING") == NULL) {
      sprintf(str, loc("XML file does not contain %s element"), "&lt;ENCODING&gt;");
      show_error(str);
      return;
   }

   if (isparam("preview")) {

      /* title row */
      sprintf(str, loc("XML import preview of %s"), xmlfile);
      show_standard_header(lbs, TRUE, str, "./", FALSE, NULL, NULL);
      rsprintf("<table class=\"frame\" cellpadding=\"0\" cellspacing=\"0\">\n");
      rsprintf("<tr><td class=\"title1\">%s</td></tr>\n", str, str);

      /* menu buttons */
      rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Cancel"));
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("XML Import"));

      /* hidden fields */
      if (isparam("head"))
         rsprintf("<input type=hidden name=head value=\"%s\">\n", getparam("head"));
      if (isparam("keep"))
         rsprintf("<input type=hidden name=keep value=\"%s\">\n", getparam("keep"));
      rsprintf("<input type=hidden name=xmlfile value=\"%s\">\n", xmlfile);

      rsprintf("</span></td></tr>\n\n");
      rsprintf("<tr><td><table class=\"listframe\" width=\"100%%\" cellspacing=0>");
   }

   list = xmalloc(MAX_N_ATTR * NAME_LENGTH);

   /* derive attributes from XML file */
   if (isparam("head")) {
      if (isparam("preview")) {
         rsprintf("<tr>\n");
         for (i = 0; i < mxml_get_number_of_children(entry); i++) {
            strlcpy(str, mxml_get_name(mxml_subnode(entry, i)), sizeof(str));
            if (strieq(str, "MID"))
               strcpy(str, "ID");
            if (strieq(str, "DATE"))
               strcpy(str, loc("Date"));
            if (strieq(str, "TEXT"))
               strcpy(str, loc("Text"));
            if (!strieq(str, "ENCODING") && !strieq(str, "IN_REPLY_TO") && !strieq(str, "REPLY_TO")
                && !strieq(str, "ATTACHMENT"))
               rsprintf("<th class=\"listtitle\">%s</th>\n", str);
         }

         rsprintf("</tr>\n");
         n_attr = i;
      } else {
         for (i = j = 0; i < mxml_get_number_of_children(entry); i++) {
            strlcpy(str, mxml_get_name(mxml_subnode(entry, i)), NAME_LENGTH);
            if (stricmp(str, "MID") != 0 && stricmp(str, "DATE") != 0 &&
                stricmp(str, "ENCODING") != 0 && stricmp(str, "TEXT") != 0 &&
                stricmp(str, "IN_REPLY_TO") != 0 && stricmp(str, "REPLY_TO") != 0 &&
                stricmp(str, "ATTACHMENT") != 0)
               strlcpy(attr_list[j++], mxml_get_name(mxml_subnode(entry, i)), NAME_LENGTH);
         }

         if (!set_attributes(lbs, attr_list, j))
            return;
         lbs->n_attr = n_attr = j;
      }
   } else {
      if (isparam("preview")) {
         rsprintf("<tr>\n");
         rsprintf("<th class=\"listtitle\">%s</th>\n", "ID");
         rsprintf("<th class=\"listtitle\">%s</th>\n", loc("Date"));
         for (i = 0; i < n_attr; i++)
            rsprintf("<th class=\"listtitle\">%s</th>\n", attr_list[i]);
         rsprintf("<th class=\"listtitle\">%s</th>\n", loc("Text"));
         rsprintf("</tr>\n");
      }
   }

   for (index = 0; index < mxml_get_number_of_children(root); index++) {

      entry = mxml_subnode(root, index);

      if (isparam("preview")) {
         rsprintf("<tr>\n");
         for (i = 0; i < mxml_get_number_of_children(entry); i++) {

            strlcpy(str, mxml_get_name(mxml_subnode(entry, i)), NAME_LENGTH);
            if (strieq(str, "ENCODING") || strieq(str, "IN_REPLY_TO") || strieq(str, "REPLY_TO")
                || strieq(str, "ATTACHMENT"))
               continue;
            if (strieq(str, "TEXT"))
               break;

            if (iline % 2 == 0)
               rsputs("<td class=\"list1\">");
            else
               rsputs("<td class=\"list2\">");

            strlcpy(str, mxml_get_value(mxml_subnode(entry, i)), NAME_LENGTH);

            if (!str[0])
               rsputs("&nbsp;");
            else
               rsputs(str);

            rsputs("</td>\n");
         }

         rsputs("<td class=\"summary\">");
         if (mxml_find_node(entry, "TEXT")) {
            strlcpy(str, mxml_get_value(mxml_find_node(entry, "TEXT")), sizeof(str));
            if (str[0]) {

               /* limit output to 3 lines */
               for (i = i_line = line_len = 0; i < (int) sizeof(str) - 1; i++, line_len++) {
                  if (str[i] == '\n') {
                     i_line++;
                     line_len = 0;
                  } else
                     /* limit line length to 150 characters */
                  if (line_len > 150 && str[i] == ' ') {
                     str[i] = '\n';
                     i_line++;
                     line_len = 0;
                  }

                  if (i_line == 3)
                     break;
               }
               str[i] = 0;

               strip_html(str);
               if (str[0])
                  strencode(str);
               else
                  rsputs("&nbsp;");
            } else
               rsputs("&nbsp;");
         }
         rsputs("</td>\n");
         rsputs("</tr>\n");
         iline++;

      } else {

         message_id = 0;
         if (isparam("keep"))
            message_id = atoi(mxml_get_value(mxml_find_node(entry, "MID")));

         for (i = 0; i < n_attr; i++) {
            strlcpy(str, attr_list[i], sizeof(str));
            while (strchr(str, ' '))
               *strchr(str, ' ') = '_';
            if (mxml_find_node(entry, str) == NULL)
               *(list + (i * NAME_LENGTH)) = 0;
            else
               strlcpy(list + i * NAME_LENGTH, mxml_get_value(mxml_find_node(entry, str)), NAME_LENGTH);
         }

         /* interprete date entries correctly */
         for (i = 0; i < n_attr; i++) {
            if (attr_flags[i] & AF_DATE) {
               /* convert to seconds in Unix format */
               ltime = convert_date(list + i * NAME_LENGTH);
               if (ltime == 0) {
                  show_error(loc("Invalid date format"));
                  return;
               }
               sprintf(list + i * NAME_LENGTH, "%d", (int) ltime);
            }
            if (attr_flags[i] & AF_DATETIME) {
               /* convert to seconds in Unix format */
               ltime = convert_datetime(list + i * NAME_LENGTH);
               if (ltime == 0) {
                  show_error(loc("Invalid date format"));
                  return;
               }
               sprintf(list + i * NAME_LENGTH, "%d", (int) ltime);
            }
         }

         encoding[0] = 0;
         if (mxml_find_node(entry, "ENCODING"))
            strlcpy(encoding, mxml_get_value(mxml_find_node(entry, "ENCODING")), sizeof(encoding));
         else
            strcpy(encoding, "plain");

         reply_to[0] = 0;
         if (mxml_find_node(entry, "REPLY_TO"))
            strlcpy(reply_to, mxml_get_value(mxml_find_node(entry, "REPLY_TO")), sizeof(reply_to));

         in_reply_to[0] = 0;
         if (mxml_find_node(entry, "IN_REPLY_TO"))
            strlcpy(in_reply_to, mxml_get_value(mxml_find_node(entry, "IN_REPLY_TO")), sizeof(in_reply_to));

         date[0] = 0;
         if (mxml_find_node(entry, "DATE"))
            strlcpy(date, mxml_get_value(mxml_find_node(entry, "DATE")), sizeof(date));

         attachment_all[0] = 0;
         if (mxml_find_node(entry, "ATTACHMENT"))
            strlcpy(attachment_all, mxml_get_value(mxml_find_node(entry, "ATTACHMENT")),
                    sizeof(attachment_all));
         memset(attachment, 0, sizeof(attachment));
         for (i = 0; i < MAX_ATTACHMENTS; i++) {
            if (i == 0)
               p = strtok(attachment_all, ",");
            else
               p = strtok(NULL, ",");

            if (p != NULL)
               strlcpy(attachment[i], p, MAX_PATH_LENGTH);
            else
               break;
         }

         str[0] = 0;
         if (mxml_find_node(entry, "TEXT"))
            p = mxml_get_value(mxml_find_node(entry, "TEXT"));
         else
            p = str;

         bedit = FALSE;
         if (isparam("keep")) {
            for (i = 0; i < *lbs->n_el_index; i++)
               if (lbs->el_index[i].message_id == message_id)
                  break;
            if (lbs->el_index[i].message_id == message_id)
               bedit = TRUE;
         }

         /* submit entry */
         if (el_submit
             (lbs, message_id, bedit, date, attr_list, (char (*)[NAME_LENGTH]) list, n_attr, p, in_reply_to,
              reply_to, encoding, attachment, FALSE, NULL))
            n_imported++;
      }
   }

   xfree(list);

   if (isparam("preview")) {
      rsprintf("</table></td></tr></table>\n");
      show_bottom_text(lbs);
      rsprintf("</form></body></html>\r\n");

      return;
   }

   sprintf(str, loc("%d entries successfully imported"), n_imported);
   show_elog_list(lbs, 0, 0, 0, TRUE, str);
}

/*------------------------------------------------------------------*/

int show_md5_page(LOGBOOK * lbs)
{
   int i, j;
   char *buffer, error_str[256];
   unsigned char digest[16];

   /* header */
   rsprintf("HTTP/1.1 200 Document follows\r\n");
   rsprintf("Server: ELOG HTTP %s-%d\r\n", VERSION, atoi(svn_revision + 13));
   rsprintf("Accept-Ranges: bytes\r\n");
   rsprintf("Connection: close\r\n");
   rsprintf("Content-Type: text/plain;charset=%s\r\n", DEFAULT_HTTP_CHARSET);
   rsprintf("Pragma: no-cache\r\n");
   rsprintf("Expires: Fri, 01 Jan 1983 00:00:00 GMT\r\n\r\n");

   /* calculate MD5 for logbook section in config file */
   load_config_section(lbs->name, &buffer, error_str);
   if (error_str[0])
      rsprintf("<h2>%s</h2>", error_str);
   else {
      rsprintf("ID: %6d MD5:", 0);

      remove_crlf(buffer);
      MD5_checksum(buffer, strlen(buffer), digest);

      for (i = 0; i < 16; i++)
         rsprintf("%02X", digest[i]);
      rsprintf("\n");
   }
   xfree(buffer);

   /* show MD5's of logbook entries */
   for (i = 0; i < *lbs->n_el_index; i++) {
      rsprintf("ID: %6d MD5:", lbs->el_index[i].message_id);
      for (j = 0; j < 16; j++)
         rsprintf("%02X", lbs->el_index[i].md5_digest[j]);
      rsprintf("\n");
   }

   keep_alive = FALSE;

   return EL_SUCCESS;
}

/*------------------------------------------------------------------*/

void combine_url(LOGBOOK * lbs, char *url, char *param, char *result, int size)
{

   if (strstr(url, "http://"))
      strlcpy(result, url + 7, size);
   else if (strstr(url, "https://"))
      strlcpy(result, url + 8, size);
   else
      strlcpy(result, url, size);

   url_encode(result, size);

   if (result[strlen(result) - 1] != '/')
      strlcat(result, "/", size);

   if (lbs != NULL) {
      if (!strstr(result, lbs->name_enc)) {
         strlcat(result, lbs->name_enc, size);
         strlcat(result, "/", size);
      }
   }

   if (param)
      strlcat(result, param, size);
}

/*------------------------------------------------------------------*/

int retrieve_remote_md5(LOGBOOK * lbs, char *host, MD5_INDEX ** md5_index, char *error_str)
{
   int i, n, id, x, version;
   char *text, *p, url[256], str[1000];

   *md5_index = NULL;

   combine_url(lbs, host, "?cmd=GetMD5", url, sizeof(url));

   text = NULL;
   error_str[0] = 0;
   if (retrieve_url(url, &text, NULL) < 0) {
      sprintf(error_str, loc("Cannot connect to remote server \"%s\""), host);
      return -1;
   }
   p = strstr(text, "ELOG HTTP ");
   if (!p) {
      if (isparam("debug"))
         rsputs(text);
      sprintf(error_str, loc("Remote server is not an ELOG server"));
      xfree(text);
      return -1;
   }
   version = atoi(p + 10) * 100 + atoi(p + 12) * 10 + atoi(p + 14);
   if (version < 250) {
      if (isparam("debug"))
         rsputs(text);
      memset(str, 0, sizeof(str));
      strncpy(str, p + 10, 5);
      sprintf(error_str, loc("Incorrect remote ELOG server version %s"), str);
      xfree(text);
      return -1;
   }

   p = strstr(text, "Location: ");
   if (p) {
      if (isparam("debug"))
         rsputs(text);

      if (strstr(text, "?fail="))
         sprintf(error_str, loc("Invalid user name \"%s\" or password for remote logbook"),
                 isparam("unm") ? getparam("unm") : "");
      else {
         strlcpy(str, p + 9, sizeof(str));
         if (strchr(str, '?'))
            *strchr(str, '?') = 0;
         strcpy(error_str, loc("URL is redirected to:"));
         strcat(error_str, str);
      }

      return -3;
   }

   p = strstr(text, "\r\n\r\n");
   if (!p) {
      if (isparam("debug"))
         rsputs(text);
      sprintf(error_str, loc("Invalid HTTP header"));
      xfree(text);
      return -1;
   }

   for (n = 0;; n++) {
      p = strstr(p, "ID:");
      if (!p)
         break;
      p += 3;

      id = atoi(p);

      p = strstr(p, "MD5:");
      if (!p)
         break;
      p += 4;

      if (n == 0)
         *md5_index = xmalloc(sizeof(MD5_INDEX));
      else
         *md5_index = xrealloc(*md5_index, (n + 1) * sizeof(MD5_INDEX));

      (*md5_index)[n].message_id = id;

      for (i = 0; i < 16; i++) {
         sscanf(p + 2 * i, "%02X", &x);
         (*md5_index)[n].md5_digest[i] = (unsigned char) x;
      }
   }

   if (n == 0) {
      if (isparam("debug"))
         rsputs(text);
      if (strstr(text, "Login")) {
         sprintf(error_str, loc("No user name supplied to access remote logbook"));
         xfree(text);
         return -2;
      } else
         sprintf(error_str, loc("Error accessing remote logbook"));
   }

   xfree(text);

   return n;
}

/*------------------------------------------------------------------*/

int send_tcp(int sock, char *buffer, unsigned int buffer_size, int flags)
/********************************************************************
 Send network data over TCP port. Break buffer in smaller
 parts if larger than maximum TCP buffer size (usually 64k).

 \********************************************************************/
{
#ifndef NET_TCP_SIZE
#define NET_TCP_SIZE 65536
#endif

   unsigned int count;
   int status;

   /* transfer fragments until complete buffer is transferred */

   for (count = 0; count < buffer_size - NET_TCP_SIZE;) {
      status = send(sock, buffer + count, NET_TCP_SIZE, flags);
      if (status != -1)
         count += status;
      else {
         return status;
      }
   }

   while (count < buffer_size) {
      status = send(sock, buffer + count, buffer_size - count, flags);
      if (status != -1)
         count += status;
      else {
         return status;
      }
   }

   return count;
}

/*------------------------------------------------------------------*/

int submit_message(LOGBOOK * lbs, char *host, int message_id, char *error_str)
{
   int size, i, n, status, fh, port, sock, content_length, header_length, remote_id, n_attr;
   char str[256], file_name[MAX_PATH_LENGTH], attrib[MAX_N_ATTR][NAME_LENGTH];
   char subdir[256], param[256], remote_host_name[256], url[256];
   char date[80], *text, in_reply_to[80], reply_to[MAX_REPLY_TO * 10],
       attachment[MAX_ATTACHMENTS][MAX_PATH_LENGTH], encoding[80], locked_by[256], *buffer;
   char *content, *p, boundary[80], request[10000], response[10000];
   struct hostent *phe;
   struct sockaddr_in bind_addr;

   text = xmalloc(TEXT_SIZE);
   error_str[0] = 0;

   /* get message with attribute list devied from database */
   size = TEXT_SIZE;
   status = el_retrieve(lbs, message_id, date, attr_list, attrib, -1, text, &size, in_reply_to, reply_to,
                        attachment, encoding, locked_by);

   if (status != EL_SUCCESS) {
      xfree(text);
      strcpy(error_str, loc("Cannot read entry from local logbook"));
      return -1;
   }

   /* count attributes */
   for (n_attr = 0; attr_list[n_attr][0]; n_attr++);

   combine_url(lbs, host, "", url, sizeof(url));
   split_url(url, remote_host_name, &port, subdir, param);

   /* create socket */
   if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      xfree(text);
      strcpy(error_str, loc("Cannot create socket"));
      return -1;
   }

   /* compose remote address */
   memset(&bind_addr, 0, sizeof(bind_addr));
   bind_addr.sin_family = AF_INET;
   bind_addr.sin_addr.s_addr = 0;
   bind_addr.sin_port = htons((unsigned short) port);

   phe = gethostbyname(remote_host_name);
   if (phe == NULL) {
      closesocket(sock);
      xfree(text);
      sprintf(error_str, loc("Cannot resolve host name \"%s\""), remote_host_name);
      return -1;
   }
   memcpy((char *) &(bind_addr.sin_addr), phe->h_addr, phe->h_length);

   /* connect to server */
   status = connect(sock, (void *) &bind_addr, sizeof(bind_addr));
   if (status != 0) {
      closesocket(sock);
      xfree(text);
      sprintf(error_str, loc("Cannot connect to host %s, port %d"), remote_host_name, port);
      return -1;
   }

   content_length = 100000;
   for (i = 0; i < MAX_ATTACHMENTS; i++)
      if (attachment[i][0]) {
         strlcpy(file_name, lbs->data_dir, sizeof(file_name));
         strlcat(file_name, attachment[i], sizeof(file_name));

         fh = open(file_name, O_RDONLY | O_BINARY);
         if (fh > 0) {
            lseek(fh, 0, SEEK_END);
            size = TELL(fh);
            close(fh);
         } else
            size = 0;

         content_length += size;
      }

   content = xmalloc(content_length);

   /* compose content */
   srand((unsigned) time(NULL));
   sprintf(boundary, "---------------------------%04X%04X%04X", rand(), rand(), rand());
   strcpy(content, boundary);
   strcat(content, "\r\nContent-Disposition: form-data; name=\"cmd\"\r\n\r\nSubmit\r\n");

   sprintf(content + strlen(content),
           "%s\r\nContent-Disposition: form-data; name=\"mirror_id\"\r\n\r\n%d\r\n", boundary, message_id);

   if (isparam("unm"))
      sprintf(content + strlen(content), "%s\r\nContent-Disposition: form-data; name=\"unm\"\r\n\r\n%s\r\n",
              boundary, getparam("unm"));

   if (isparam("upwd"))
      sprintf(content + strlen(content), "%s\r\nContent-Disposition: form-data; name=\"upwd\"\r\n\r\n%s\r\n",
              boundary, getparam("upwd"));

   if (in_reply_to[0])
      sprintf(content + strlen(content),
              "%s\r\nContent-Disposition: form-data; name=\"in_reply_to\"\r\n\r\n%s\r\n", boundary,
              in_reply_to);

   if (reply_to[0])
      sprintf(content + strlen(content),
              "%s\r\nContent-Disposition: form-data; name=\"reply_to\"\r\n\r\n%s\r\n", boundary, reply_to);

   for (i = 0; i < n_attr; i++)
      sprintf(content + strlen(content), "%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n",
              boundary, attr_list[i], attrib[i]);

   sprintf(content + strlen(content),
           "%s\r\nContent-Disposition: form-data; name=\"entry_date\"\r\n\r\n%s\r\n", boundary, date);

   sprintf(content + strlen(content),
           "%s\r\nContent-Disposition: form-data; name=\"encoding\"\r\n\r\n%s\r\n", boundary, encoding);

   sprintf(content + strlen(content),
           "%s\r\nContent-Disposition: form-data; name=\"Text\"\r\n\r\n%s\r\n%s\r\n", boundary, text,
           boundary);

   content_length = strlen(content);
   p = content + content_length;

   /* read attachments */
   for (i = 0; i < MAX_ATTACHMENTS; i++)
      if (attachment[i][0]) {
         strlcpy(file_name, lbs->data_dir, sizeof(file_name));
         strlcat(file_name, attachment[i], sizeof(file_name));

         fh = open(file_name, O_RDONLY | O_BINARY);
         if (fh > 0) {
            lseek(fh, 0, SEEK_END);
            size = TELL(fh);
            lseek(fh, 0, SEEK_SET);
            buffer = xmalloc(size);
            read(fh, buffer, size);
            close(fh);

            /* submit attachment */
            sprintf(p, "Content-Disposition: form-data; name=\"attfile%d\"; filename=\"%s\"\r\n\r\n", i + 1,
                    attachment[i]);

            content_length += strlen(p);
            p += strlen(p);

            memcpy(p, buffer, size);
            p += size;
            strcpy(p, boundary);
            strcat(p, "\r\n");

            content_length += size + strlen(p);
            p += strlen(p);

            xfree(buffer);
         }
      }

   /* compose request */
   strcpy(request, "POST ");
   if (subdir[0]) {
      if (subdir[0] != '/')
         strcat(request, "/");
      strcat(request, subdir);
      if (request[strlen(request) - 1] != '/')
         strcat(request, "/");
   }
   strcat(request, " HTTP/1.0\r\n");

   sprintf(request + strlen(request), "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
   sprintf(request + strlen(request), "Host: %s\r\n", host_name);
   sprintf(request + strlen(request), "User-Agent: ELOGD\r\n");
   sprintf(request + strlen(request), "Content-Length: %d\r\n", content_length);

   if (isparam("wpwd"))
      sprintf(request + strlen(request), "Cookie: wpwd=%s\r\n", getparam("wpwd"));

   strcat(request, "\r\n");

   header_length = strlen(request);

   /* send request */
   send(sock, request, header_length, 0);

   /* send content */
   send_tcp(sock, content, content_length, 0);

   /* receive response */
   i = recv(sock, response, 10000, 0);
   if (i < 0) {
      closesocket(sock);
      xfree(text);
      strcpy(error_str, "Cannot receive response");
      return -1;
   }

   /* discard remainder of response */
   n = i;
   while (i > 0) {
      i = recv(sock, response + n, 10000, 0);
      if (i > 0)
         n += i;
   }
   response[n] = 0;

   closesocket(sock);
   remote_id = -1;

   /* check response status */
   if (strstr(response, "302 Found")) {
      if (strstr(response, "Location:")) {
         if (strstr(response, "fail"))
            sprintf(error_str, "Invalid user name or password\n");

         strlcpy(str, strstr(response, "Location:") + 9, sizeof(str));
         if (strchr(str, '\n'))
            *strchr(str, '\n') = 0;
         if (strchr(str, '?'))
            *strchr(str, '?') = 0;

         if (strrchr(str, '/'))
            remote_id = atoi(strrchr(str, '/') + 1);
         else
            remote_id = atoi(str);
      }
   } else if (strstr(response, "Logbook Selection"))
      sprintf(error_str, "No logbook specified\n");
   else if (strstr(response, "enter password"))
      sprintf(error_str, "Missing or invalid password\n");
   else if (strstr(response, "form name=form1"))
      sprintf(error_str, "Missing or invalid user name/password\n");
   else if (strstr(response, "Error: Attribute")) {
      strncpy(str, strstr(response, "Error: Attribute") + 20, sizeof(str));
      if (strchr(str, '<'))
         *strchr(str, '<') = 0;
      sprintf(error_str, "Missing required attribute \"%s\"\n", str);
   } else
      sprintf(error_str, "Error transmitting message\n");

   if (error_str[0] && isparam("debug"))
      rsputs(response);

   xfree(text);

   if (error_str[0])
      return -1;

   return remote_id;
}

/*------------------------------------------------------------------*/

int receive_message(LOGBOOK * lbs, char *url, int message_id, char *error_str, BOOL bnew)
{
   int i, status, size, n_attr, header_size;
   char str[NAME_LENGTH], str2[NAME_LENGTH], *p, *p2, *message, date[80], attrib[MAX_N_ATTR][NAME_LENGTH],
       in_reply_to[80], reply_to[MAX_REPLY_TO * 10], encoding[80], locked_by[256],
       attachment[MAX_ATTACHMENTS][MAX_PATH_LENGTH], attachment_all[64 * MAX_ATTACHMENTS];

   error_str[0] = 0;

   combine_url(lbs, url, "", str, sizeof(str));
   sprintf(str + strlen(str), "%d?cmd=%s", message_id, loc("Download"));

   retrieve_url(str, &message, NULL);
   if (message == NULL) {
      sprintf(error_str, loc("Cannot receive \"%s\""), str);
      return -1;
   }
   p = strstr(message, "\r\n\r\n");
   if (p == NULL) {
      if (isparam("debug"))
         rsputs(message);
      xfree(message);
      sprintf(error_str, loc("Cannot receive \"%s\""), str);
      return -1;
   }
   p += 4;

   /* check for correct ID */
   if (atoi(p + 8) != message_id) {
      if (isparam("debug"))
         rsputs(message);
      sprintf(error_str, loc("Received wrong entry id \"%d\""), atoi(p + 8));
      xfree(message);
      return -1;
   }

   /* decode entry */
   el_decode(p, "Date: ", date);
   el_decode(p, "Reply to: ", reply_to);
   el_decode(p, "In reply to: ", in_reply_to);

   /* derive attribute names from message */
   for (i = 0;; i++) {
      el_enum_attr(p, i, attr_list[i], attrib[i]);
      if (!attr_list[i][0])
         break;
   }
   n_attr = i;

   el_decode(p, "Attachment: ", attachment_all);
   el_decode(p, "Encoding: ", encoding);

   /* break apart attachements */
   for (i = 0; i < MAX_ATTACHMENTS; i++)
      attachment[i][0] = 0;

   for (i = 0; i < MAX_ATTACHMENTS; i++) {
      if (i == 0)
         p2 = strtok(attachment_all, ",");
      else
         p2 = strtok(NULL, ",");

      if (p2 != NULL)
         strcpy(attachment[i], p2);
      else
         break;
   }

   el_decode(p, "Locked by: ", locked_by);
   if (locked_by[0]) {
      xfree(message);
      sprintf(error_str, loc("Entry #%d is locked on remote server"), message_id);
      return -1;
   }

   p = strstr(message, "========================================\n");

   /* check for \n -> \r conversion (e.g. zipping/unzipping) */
   if (p == NULL)
      p = strstr(message, "========================================\r");

   if (p != NULL) {
      p += 41;

      /* remove last CR */
      if (p[strlen(p) - 1] == '\n')
         p[strlen(p) - 1] = 0;

      status = el_submit(lbs, message_id, !bnew, date, attr_list, attrib, n_attr, p, in_reply_to, reply_to,
                         encoding, attachment, FALSE, "");

      xfree(message);

      if (status != message_id) {
         sprintf(error_str, loc("Cannot save remote entry locally"));
         return -1;
      }

      for (i = 0; i < MAX_ATTACHMENTS; i++) {
         if (attachment[i][0]) {

            combine_url(lbs, url, "", str, sizeof(str));
            strlcpy(str2, attachment[i], sizeof(str2));
            str2[13] = '/';
            strlcat(str, str2, sizeof(str));

            size = retrieve_url(str, &message, NULL);
            p = strstr(message, "\r\n\r\n");
            if (p == NULL) {
               xfree(message);
               sprintf(error_str, loc("Cannot receive \"%s\""), str);
               return -1;
            }
            p += 4;
            header_size = p - message;

            el_submit_attachment(lbs, attachment[i], p, size - header_size, NULL);
            xfree(message);
         }
      }
   } else {
      xfree(message);
      return -1;
   }

   return 1;
}

/*------------------------------------------------------------------*/

void submit_config(LOGBOOK * lbs, char *server, char *buffer, char *error_str)
{
   int i, n, status, port, sock, content_length, header_length;
   char str[256];
   char subdir[256], param[256], remote_host_name[256];
   char *content, *p, boundary[80], request[10000], response[10000];
   struct hostent *phe;
   struct sockaddr_in bind_addr;

   error_str[0] = 0;

   combine_url(lbs, server, "", str, sizeof(str));
   split_url(str, remote_host_name, &port, subdir, param);

   /* create socket */
   if ((sock = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
      strcpy(error_str, loc("Cannot create socket"));
      return;
   }

   /* compose remote address */
   memset(&bind_addr, 0, sizeof(bind_addr));
   bind_addr.sin_family = AF_INET;
   bind_addr.sin_addr.s_addr = 0;
   bind_addr.sin_port = htons((unsigned short) port);

   phe = gethostbyname(remote_host_name);
   if (phe == NULL) {
      closesocket(sock);
      sprintf(error_str, loc("Cannot resolve host name \"%s\""), remote_host_name);
      return;
   }
   memcpy((char *) &(bind_addr.sin_addr), phe->h_addr, phe->h_length);

   /* connect to server */
   status = connect(sock, (void *) &bind_addr, sizeof(bind_addr));
   if (status != 0) {
      closesocket(sock);
      sprintf(error_str, loc("Cannot connect to host %s, port %d"), remote_host_name, port);
      return;
   }

   content_length = 100000;
   content = xmalloc(content_length);

   /* compose content */
   srand((unsigned) time(NULL));
   sprintf(boundary, "---------------------------%04X%04X%04X", rand(), rand(), rand());
   strcpy(content, boundary);
   strcat(content, "\r\nContent-Disposition: form-data; name=\"cmd\"\r\n\r\nSave\r\n");

   if (isparam("unm"))
      sprintf(content + strlen(content), "%s\r\nContent-Disposition: form-data; name=\"unm\"\r\n\r\n%s\r\n",
              boundary, getparam("unm"));

   if (isparam("upwd"))
      sprintf(content + strlen(content), "%s\r\nContent-Disposition: form-data; name=\"upwd\"\r\n\r\n%s\r\n",
              boundary, getparam("upwd"));

   sprintf(content + strlen(content),
           "%s\r\nContent-Disposition: form-data; name=\"Text\"\r\n\r\n%s\r\n%s\r\n", boundary, buffer,
           boundary);

   content_length = strlen(content);
   p = content + content_length;

   /* compose request */
   strcpy(request, "POST ");
   if (subdir[0]) {
      if (subdir[0] != '/')
         strcat(request, "/");
      strcat(request, subdir);
      if (request[strlen(request) - 1] != '/')
         strcat(request, "/");
   }
   strcat(request, " HTTP/1.0\r\n");

   sprintf(request + strlen(request), "Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
   sprintf(request + strlen(request), "Host: %s\r\n", host_name);
   sprintf(request + strlen(request), "User-Agent: ELOGD\r\n");
   sprintf(request + strlen(request), "Content-Length: %d\r\n", content_length);

   if (isparam("wpwd"))
      sprintf(request + strlen(request), "Cookie: wpwd=%s\r\n", getparam("wpwd"));

   strcat(request, "\r\n");

   header_length = strlen(request);

   /* send request */
   send(sock, request, header_length, 0);

   /* send content */
   send(sock, content, content_length, 0);

   /* receive response */
   i = recv(sock, response, 10000, 0);
   if (i < 0) {
      closesocket(sock);
      strcpy(error_str, "Cannot receive response");
      return;
   }

   /* discard remainder of response */
   n = i;
   while (i > 0) {
      i = recv(sock, response + n, 10000, 0);
      if (i > 0)
         n += i;
   }
   response[n] = 0;

   closesocket(sock);

   /* check response status */
   if (strstr(response, "302 Found")) {
      if (strstr(response, "Location:")) {
         if (strstr(response, "fail"))
            sprintf(error_str, "Invalid usr name or password\n");
      }
   } else if (strstr(response, "Logbook Selection"))
      sprintf(error_str, "No logbook specified\n");
   else if (strstr(response, "enter password"))
      sprintf(error_str, "Missing or invalid password\n");
   else if (strstr(response, "form name=form1"))
      sprintf(error_str, "Missing or invalid user name/password\n");
   else if (strstr(response, "Error: Attribute")) {
      strncpy(str, strstr(response, "Error: Attribute") + 20, sizeof(str));
      if (strchr(str, '<'))
         *strchr(str, '<') = 0;
      sprintf(error_str, "Missing required attribute \"%s\"\n", str);
   } else
      sprintf(error_str, "Error transmitting message\n");
}

/*------------------------------------------------------------------*/

void receive_config(LOGBOOK * lbs, char *server, char *error_str)
{
   char str[256], pwd[256], *buffer, *p;
   int status, version;

   error_str[0] = pwd[0] = 0;

   do {

      combine_url(lbs, server, "", str, sizeof(str));
      if (lbs == NULL)
         strcat(str, "?cmd=GetConfig"); // request complete config file
      else
         strcat(str, "?cmd=Download");  // request config section of logbook

      if (retrieve_url(str, &buffer, pwd) < 0) {
         *strchr(str, '?') = 0;
         sprintf(error_str, "Cannot contact elogd server at http://%s", str);
         return;
      }

      /* check version */
      p = strstr(buffer, "ELOG HTTP ");
      if (!p) {
         if (verbose)
            puts(buffer);
         sprintf(error_str, "Remote server is not an ELOG server");
         xfree(buffer);
         return;
      }
      version = atoi(p + 10) * 100 + atoi(p + 12) * 10 + atoi(p + 14);
      if (version < 254) {
         if (verbose)
            puts(buffer);

         strlcpy(str, p + 10, 10);
         if (strchr(str, '\r'))
            *strchr(str, '\r') = 0;

         sprintf(error_str, "Incorrect remote ELOG server version %s, must be 2.5.4 or later", str);
         xfree(buffer);
         return;
      }

      /* evaluate status */
      p = strchr(buffer, ' ');
      if (p == NULL) {
         if (verbose)
            puts(buffer);
         xfree(buffer);
         *strchr(str, '?') = 0;
         sprintf(error_str, "Received invalid response from elogd server at http://%s", str);
         xfree(buffer);
         return;
      }
      p++;
      status = atoi(p);
      if (status == 401) {
         if (verbose)
            puts(buffer);
         xfree(buffer);
         eprintf("Please enter password to access remote elogd server: ");
         fgets(pwd, sizeof(pwd), stdin);
         while (pwd[strlen(pwd) - 1] == '\n' || pwd[strlen(pwd) - 1] == '\r')
            pwd[strlen(pwd) - 1] = 0;
      } else if (status != 200) {
         if (verbose)
            puts(buffer);
         xfree(buffer);
         *strchr(str, '?') = 0;
         sprintf(error_str, "Received invalid response from elogd server at http://%s", str);
         return;
      }

   } while (status != 200);

   p = strstr(buffer, "\r\n\r\n");
   if (p == NULL) {
      if (verbose)
         puts(buffer);
      xfree(buffer);
      sprintf(error_str, loc("Cannot receive \"%s\""), str);
      return;
   }
   p += 4;

   if (lbs == NULL) {
      if (!save_config(p, str))
         rsprintf("%s", str);
   } else {
      if (!save_admin_config(lbs->name, p, str))
         rsprintf("%s", str);
   }

   xfree(buffer);
}

/*------------------------------------------------------------------*/

int adjust_config(char *url)
{
   int fh, i, length;
   char *buf, *buf2, *p1, *p2;
   char str[256];

   fh = open(config_file, O_RDWR | O_BINARY, 0644);
   if (fh < 0) {
      sprintf(str, loc("Cannot open file <b>%s</b>"), config_file);
      strcat(str, ": ");
      strcat(str, strerror(errno));
      eputs(str);
      return 0;
   }

   /* read previous contents */
   length = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);
   buf = xmalloc(2 * length + 1000);
   read(fh, buf, length);
   buf[length] = 0;

   /* add mirror server */
   p1 = stristr(buf, "Mirror server =");
   if (p1 != NULL) {
      p2 = strchr(p1, '\n');
      if (p2 && *(p2 - 1) == '\r')
         p2--;
   } else {
      p1 = strstr(buf, "[global]");
      if (p1 == NULL) {
         eputs("Cannot find [global] section in config file.");
         return 0;
      }
      p1 = strchr(p1, '\n');
      while (*p1 == '\n' || *p1 == '\r')
         p1++;

      p2 = p1;
   }

   /* save tail */
   buf2 = NULL;
   if (p2)
      buf2 = xstrdup(p2);

   sprintf(p1, "Mirror server = %s\r\n", url);
   strlcat(p1, buf2, length + 1000);
   xfree(buf2);
   eprintf("Option \"Mirror server = %s\" added to config file.\n", url);

   /* outcomment "URL = xxx" */
   p1 = strstr(buf, "URL =");
   if (p1 != NULL) {

      /* save tail */
      buf2 = xstrdup(p1);

      /* add comment */
      sprintf(p1, "; Following line has been outcommented after cloning\r\n");
      strlcat(p1, "; ", length + 1000);
      strlcat(p1, buf2, length + 1000);
      xfree(buf2);

      eputs("Option \"URL = xxx\" has been outcommented from config file.");
   }

   adjust_crlf(buf, 2 * length + 1000);

   lseek(fh, 0, SEEK_SET);
   i = write(fh, buf, strlen(buf));
   if (i < (int) strlen(buf)) {
      sprintf(str, loc("Cannot write to <b>%s</b>"), config_file);
      strcat(str, ": ");
      strcat(str, strerror(errno));
      eputs(str);
      close(fh);
      xfree(buf);
      return 0;
   }

   TRUNCATE(fh);

   close(fh);
   xfree(buf);

   return 1;
}

/*------------------------------------------------------------------*/

void receive_pwdfile(LOGBOOK * lbs, char *server, char *error_str)
{
   char str[256], pwd[256], url[256], *buffer, *buf, *p;
   int i, status, version, fh;

   error_str[0] = pwd[0] = 0;

   do {

      combine_url(lbs, server, "", url, sizeof(url));
      strlcpy(str, url, sizeof(str));
      strcat(str, "?cmd=GetPwdFile");   // request password file

      if (retrieve_url(str, &buffer, pwd) < 0) {
         *strchr(str, '?') = 0;
         sprintf(error_str, "Cannot contact elogd server at http://%s", str);
         return;
      }

      /* check version */
      p = strstr(buffer, "ELOG HTTP ");
      if (!p) {
         sprintf(error_str, "Remote server is not an ELOG server");
         xfree(buffer);
         return;
      }
      version = atoi(p + 10) * 100 + atoi(p + 12) * 10 + atoi(p + 14);
      if (version < 254) {
         strlcpy(str, p + 10, 10);
         if (strchr(str, '\r'))
            *strchr(str, '\r') = 0;

         sprintf(error_str, "Incorrect remote ELOG server version %s, must be 2.5.4 or later", str);
         xfree(buffer);
         return;
      }

      /* evaluate status */
      p = strchr(buffer, ' ');
      if (p == NULL) {
         xfree(buffer);
         *strchr(str, '?') = 0;
         sprintf(error_str, "Received invalid response from elogd server at http://%s", str);
         xfree(buffer);
         return;
      }
      p++;
      status = atoi(p);
      if (status == 401) {
         xfree(buffer);
         eprintf("Please enter password to access remote elogd server: ");
         fgets(pwd, sizeof(pwd), stdin);
         while (pwd[strlen(pwd) - 1] == '\n' || pwd[strlen(pwd) - 1] == '\r')
            pwd[strlen(pwd) - 1] = 0;
      } else if (status != 200 && status != 302) {
         xfree(buffer);
         *strchr(str, '?') = 0;
         sprintf(error_str, "Received invalid response from elogd server at http://%s", str);
         return;
      }

      p = strstr(buffer, "\r\n\r\n");
      if (p == NULL) {
         xfree(buffer);
         sprintf(error_str, loc("Cannot receive \"%s\""), str);
         return;
      }
      p += 4;

      /* check for logbook access */
      if (strstr(p, loc("Please login")) || strstr(p, "GetPwdFile") || status == 302) {

         if (strstr(buffer, "?fail="))
            eprintf("\nInvalid username or password.");

         if (strstr(p, loc("Please login")) == NULL && strstr(p, "GetPwdFile") && isparam("unm"))
            eprintf("\nUser \"%s\" has no admin rights on remote server.", getparam("unm"));

         /* ask for username and password */
         eprintf("\nPlease enter admin username to access\n%s: ", url);
         fgets(str, sizeof(str), stdin);
         while (str[strlen(str) - 1] == '\r' || str[strlen(str) - 1] == '\n')
            str[strlen(str) - 1] = 0;
         setparam("unm", str);

         eprintf("\nPlease enter admin password to access\n%s: ", url);
         read_password(str, sizeof(str));
         eprintf("\n");
         while (str[strlen(str) - 1] == '\r' || str[strlen(str) - 1] == '\n')
            str[strlen(str) - 1] = 0;
         do_crypt(str, pwd, sizeof(pwd));
         setparam("upwd", pwd);
         status = 0;
      }

   } while (status != 200);

   get_password_file(lbs, str, sizeof(str));
   fh = open(str, O_CREAT | O_RDWR | O_BINARY, 0644);
   if (fh < 0) {
      sprintf(error_str, loc("Cannot open file <b>%s</b>"), str);
      strcat(error_str, ": ");
      strcat(error_str, strerror(errno));
      return;
   }

   buf = xmalloc(2 * strlen(p));
   strlcpy(buf, p, 2 * strlen(p));
   adjust_crlf(buf, 2 * strlen(p));

   i = write(fh, buf, strlen(buf));
   if (i < (int) strlen(buf)) {
      sprintf(error_str, loc("Cannot write to <b>%s</b>"), str);
      strcat(error_str, ": ");
      strcat(error_str, strerror(errno));
      close(fh);
      xfree(buf);
      xfree(buffer);
      return;
   }

   TRUNCATE(fh);

   close(fh);

   xfree(buf);
   xfree(buffer);
}

/*------------------------------------------------------------------*/

int save_md5(LOGBOOK * lbs, char *server, MD5_INDEX * md5_index, int n)
{
   char str[256], url[256], file_name[256];
   int i, j;
   FILE *f;

   combine_url(lbs, server, "", url, sizeof(url));
   url_decode(url);
   if (strstr(url, "http://"))
      strlcpy(str, url + 7, sizeof(str));
   else if (strstr(url, "https://"))
      strlcpy(str, url + 8, sizeof(str));
   else
      strlcpy(str, url, sizeof(str));

   for (i = 0; i < (int) strlen(str); i++)
      if (strchr(":/\\ ", str[i]))
         str[i] = '_';

   while (str[strlen(str) - 1] == '_')
      str[strlen(str) - 1] = 0;

   strlcpy(file_name, logbook_dir, sizeof(file_name));
   strlcat(file_name, str, sizeof(file_name));
   strlcat(file_name, ".md5", sizeof(file_name));

   f = fopen(file_name, "wt");
   if (f == NULL)
      return -1;

   for (i = 0; i < n; i++) {
      fprintf(f, "ID%d: ", md5_index[i].message_id);
      for (j = 0; j < 16; j++)
         fprintf(f, "%02X", md5_index[i].md5_digest[j]);
      fprintf(f, "\n");
   }

   fclose(f);
   return 1;
}

/*------------------------------------------------------------------*/

int load_md5(LOGBOOK * lbs, char *server, MD5_INDEX ** md5_index)
{
   char str[256], url[256], file_name[256], *p;
   int i, j, x;
   FILE *f;

   *md5_index = NULL;

   combine_url(lbs, server, "", url, sizeof(url));
   url_decode(url);
   if (strstr(url, "http://"))
      strlcpy(str, url + 7, sizeof(str));
   else if (strstr(url, "https://"))
      strlcpy(str, url + 8, sizeof(str));
   else
      strlcpy(str, url, sizeof(str));

   for (i = 0; i < (int) strlen(str); i++)
      if (strchr(":/\\ ", str[i]))
         str[i] = '_';

   while (str[strlen(str) - 1] == '_')
      str[strlen(str) - 1] = 0;

   strlcpy(file_name, logbook_dir, sizeof(file_name));
   strlcat(file_name, str, sizeof(file_name));
   strlcat(file_name, ".md5", sizeof(file_name));

   f = fopen(file_name, "rt");
   if (f == NULL)
      return 0;

   for (i = 0; !feof(f); i++) {
      str[0] = 0;
      fgets(str, sizeof(str), f);
      if (!str[0])
         break;

      if (i == 0)
         *md5_index = xcalloc(sizeof(MD5_INDEX), 1);
      else
         *md5_index = xrealloc(*md5_index, sizeof(MD5_INDEX) * (i + 1));

      p = str + 2;

      (*md5_index)[i].message_id = atoi(p);
      while (*p && *p != ' ')
         p++;
      while (*p && *p == ' ')
         p++;

      for (j = 0; j < 16; j++) {
         sscanf(p + j * 2, "%02X", &x);
         (*md5_index)[i].md5_digest[j] = (unsigned char) x;
      }
   }

   fclose(f);
   return i;
}

/*------------------------------------------------------------------*/

BOOL equal_md5(unsigned char m1[16], unsigned char m2[16])
{
   int i;
   for (i = 0; i < 16; i++)
      if (m1[i] != m2[i])
         break;

   return i == 16;
}

/*------------------------------------------------------------------*/

#define SYNC_HTML   1
#define SYNC_CRON   2
#define SYNC_CLONE  3

void mprint(LOGBOOK * lbs, int mode, char *str)
{
   char line[1000];

   if (mode == SYNC_HTML)
      rsprintf("%s\n", str);
   else if (mode == SYNC_CRON) {
      if (_logging_level > 1) {
         sprintf(line, "MIRROR: %s", str);
         write_logfile(lbs, line);
      }
   } else
      eputs(str);
}

void synchronize_logbook(LOGBOOK * lbs, int mode, BOOL sync_all)
{
   int index, i, j, i_msg, i_remote, i_cache, n_remote, n_cache, nserver, remote_id, exist_remote,
       exist_cache, message_id, max_id;
   int all_identical, n_delete;
   char str[2000], url[256], loc_ref[256], rem_ref[256], pwd[256], locked_by[256];
   MD5_INDEX *md5_remote, *md5_cache;
   char list[MAX_N_LIST][NAME_LENGTH], error_str[256], *buffer;
   unsigned char digest[16];

   if (!getcfg(lbs->name, "Mirror server", str, sizeof(str))) {
      show_error(loc("No mirror server defined in configuration file"));
      return;
   }

   nserver = strbreak(str, list, MAX_N_LIST, ",", FALSE);

   for (index = 0; index < nserver; index++) {

      if (mode == SYNC_HTML) {
         rsprintf("<table width=\"100%%\" cellpadding=\"1\" cellspacing=\"0\"");

         if (getcfg_topgroup())
            sprintf(loc_ref, "<a href=\"../%s/\">%s</a>", lbs->name_enc, lbs->name);
         else if (sync_all)
            sprintf(loc_ref, "<a href=\"%s/\">%s</a>", lbs->name_enc, lbs->name);
         else
            sprintf(loc_ref, "<a href=\".\">%s</a>", lbs->name);

         sprintf(str, loc("Synchronizing logbook %s with server \"%s\""), loc_ref, list[index]);
         rsprintf("<tr><td class=\"title1\">%s</td></tr>\n", str);
         rsprintf("</table><p>\n");

         rsprintf("<pre>");
      } else if (mode == SYNC_CLONE) {
         if (list[index][strlen(list[index]) - 1] != '/')
            eprintf("\nRetrieving entries from \"%s/%s\"...\n", list[index], lbs->name);
         else
            eprintf("\nRetrieving entries from \"%s%s\"...\n", list[index], lbs->name);
      }

      /* send partial return buffer */
      flush_return_buffer();

      do {

         n_remote = retrieve_remote_md5(lbs, list[index], &md5_remote, error_str);
         if (n_remote <= 0) {

            if ((n_remote == -2 || n_remote == -3) && mode == SYNC_CLONE) {

               if (n_remote == -3)
                  eprintf("\nInvalid username or password.");

               combine_url(lbs, list[index], "", url, sizeof(url));
               /* ask for username and password */
               eprintf("\nPlease enter username to access\n%s: ", url);
               fgets(str, sizeof(str), stdin);
               while (str[strlen(str) - 1] == '\r' || str[strlen(str) - 1] == '\n')
                  str[strlen(str) - 1] = 0;
               setparam("unm", str);

               eprintf("\nPlease enter password to access\n%s: ", url);
               read_password(str, sizeof(str));
               eprintf("\n");
               while (str[strlen(str) - 1] == '\r' || str[strlen(str) - 1] == '\n')
                  str[strlen(str) - 1] = 0;
               do_crypt(str, pwd, sizeof(pwd));
               setparam("upwd", pwd);

            } else {

               mprint(lbs, mode, error_str);

               if (md5_remote)
                  xfree(md5_remote);

               if (mode == SYNC_HTML)
                  rsprintf("</pre>\n");

               break;
            }
         }

      } while (n_remote <= 0);

      if (n_remote <= 0)
         continue;

      /* load local copy of remote MD5s from file */
      n_cache = load_md5(lbs, list[index], &md5_cache);

      all_identical = TRUE;

      /*---- check for configuration file ----*/

      if (getcfg(lbs->name, "Mirror config", str, sizeof(str)) && atoi(str) == 1 && md5_cache && mode
          != SYNC_CLONE) {

         load_config_section(lbs->name, &buffer, error_str);
         if (error_str[0])
            mprint(lbs, mode, error_str);
         else {
            remove_crlf(buffer);
            MD5_checksum(buffer, strlen(buffer), digest);
         }

         /* compare MD5s */
         if (verbose) {
            eprintf("CONFIG : ");
            for (j = 0; j < 16; j++)
               eprintf("%02X", digest[j]);
            eprintf("\nCache  : ");
            for (j = 0; j < 16; j++)
               eprintf("%02X", md5_cache[0].md5_digest[j]);
            eprintf("\nRemote : ");
            for (j = 0; j < 16; j++)
               eprintf("%02X", md5_remote[0].md5_digest[j]);
            eprintf("\n\n");
         }

         if (n_remote > 0) {
            /* if config has been changed on this server, but not remotely, send it */
            if (!equal_md5(md5_cache[0].md5_digest, digest) && equal_md5(md5_cache[0].md5_digest,
                                                                         md5_remote[0].md5_digest)) {

               all_identical = FALSE;

               if (_logging_level > 1)
                  write_logfile(lbs, "MIRROR send config");

               /* submit configuration section */
               if (!getcfg(lbs->name, "Mirror simulate", str, sizeof(str)) || atoi(str) == 0) {
                  submit_config(lbs, list[index], buffer, error_str);

                  if (error_str[0])
                     mprint(lbs, mode, error_str);
                  else
                     mprint(lbs, mode, "Local config submitted");
                  md5_cache[0].message_id = -1;
               } else
                  mprint(lbs, mode, "Local config should be submitted");

            } else
               /* if config has been changed remotely, but not on this server, receive it */
               if (!equal_md5(md5_cache[0].md5_digest, md5_remote[0].md5_digest)
                   && equal_md5(md5_cache[0].md5_digest, digest)) {

               all_identical = FALSE;

               if (_logging_level > 1)
                  write_logfile(lbs, "MIRROR receive config");

               if (!getcfg(lbs->name, "Mirror simulate", str, sizeof(str)) || atoi(str) == 0) {
                  receive_config(lbs, list[index], error_str);

                  if (error_str[0])
                     mprint(lbs, mode, error_str);
                  else
                     mprint(lbs, mode, "Remote config received");

                  md5_cache[0].message_id = -1;
               } else
                  mprint(lbs, mode, loc("Remote config should be received"));

            } else
               /* if config has been changed remotely and on this server, show conflict */
               if (!equal_md5(md5_cache[0].md5_digest, md5_remote[0].md5_digest)
                   && !equal_md5(md5_cache[0].md5_digest, digest)
                   && !equal_md5(md5_remote[0].md5_digest, digest)) {

               if (_logging_level > 1)
                  write_logfile(lbs, "MIRROR config conflict");

               sprintf(str, "%s. ", loc("Configuration has been changed locally and remotely"));
               strcat(str, loc("Please merge manually to resolve conflict"));
               strcat(str, ".");

               mprint(lbs, mode, str);

            } else {
               /* configs are identical */
               md5_cache[0].message_id = -1;
            }
         } else {               /* n_remote == 0 */

            sprintf(str, loc("Logbook \"%s\" does not exist on remote server"), lbs->name);

            mprint(lbs, mode, str);
            continue;

         }

         flush_return_buffer();

         if (buffer)
            xfree(buffer);
      }

      /*---- loop through logbook entries ----*/

      n_delete = 0;

      for (i_msg = 0; i_msg < *lbs->n_el_index; i_msg++) {

         message_id = lbs->el_index[i_msg].message_id;

         /* check if message is locked */
         el_retrieve(lbs, message_id, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL, locked_by);
         if (locked_by[0]) {
            sprintf(str, "ID%d:\t%s", message_id,
                    loc("Entry is locked on local server and therefore skipped"));
            mprint(lbs, mode, str);
            all_identical = FALSE;
            continue;
         }

         /* look for message id in MD5s */
         for (i_remote = 0; i_remote < n_remote; i_remote++)
            if (md5_remote[i_remote].message_id == message_id)
               break;
         exist_remote = i_remote < n_remote;

         for (i_cache = 0; i_cache < n_cache; i_cache++)
            if (md5_cache[i_cache].message_id == message_id)
               break;
         exist_cache = i_cache < n_cache;

         /* if message exists in both lists, compare MD5s */
         if (exist_remote && exist_cache) {

            /* compare MD5s */
            if (verbose) {
               eprintf("ID%-5d: ", message_id);
               for (j = 0; j < 16; j++)
                  eprintf("%02X", lbs->el_index[i_msg].md5_digest[j]);
               eprintf("\nCache  : ");
               for (j = 0; j < 16; j++)
                  eprintf("%02X", md5_cache[i_cache].md5_digest[j]);
               eprintf("\nRemote : ");
               for (j = 0; j < 16; j++)
                  eprintf("%02X", md5_remote[i_remote].md5_digest[j]);
               eprintf("\n\n");
            }

            /* if message has been changed on this server, but not remotely, send it */
            if (!equal_md5(md5_cache[i_cache].md5_digest, lbs->el_index[i_msg].md5_digest)
                && equal_md5(md5_cache[i_cache].md5_digest, md5_remote[i_remote].md5_digest)) {

               all_identical = FALSE;

               if (_logging_level > 1) {
                  sprintf(str, "MIRROR send entry #%d", message_id);
                  write_logfile(lbs, str);
               }

               /* submit local message */
               if (!getcfg(lbs->name, "Mirror simulate", str, sizeof(str)) || atoi(str) == 0) {
                  submit_message(lbs, list[index], message_id, error_str);

                  /* not that submit_message() may have changed attr_list !!! */

                  if (error_str[0])
                     sprintf(str, "%s: %s", loc("Error sending local entry"), error_str);
                  else
                     sprintf(str, "ID%d:\t%s", message_id, loc("Local entry submitted"));
                  mprint(lbs, mode, str);

                  md5_cache[i_cache].message_id = -1;
               } else {
                  sprintf(str, "ID%d:\t%s", message_id, loc("Local entry should be submitted"));
                  mprint(lbs, mode, str);
               }

            } else
               /* if message has been changed remotely, but not on this server, receive it */
               if (!equal_md5(md5_cache[i_cache].md5_digest, md5_remote[i_remote].md5_digest)
                   && equal_md5(md5_cache[i_cache].md5_digest, lbs->el_index[i_msg].md5_digest)) {

               all_identical = FALSE;

               if (mode == SYNC_CLONE) {
                  eprintf("ID%d:\t", message_id);
               } else if (mode == SYNC_HTML) {
                  if (getcfg_topgroup())
                     rsprintf("<a href=\"../%s/%d\">ID%d:</a>\t", lbs->name_enc, message_id, message_id);
                  else if (sync_all)
                     rsprintf("<a href=\"%s/%d\">ID%d:</a>\t", lbs->name_enc, message_id, message_id);
                  else
                     rsprintf("<a href=\"%d\">ID%d:</a>\t", message_id, message_id);

                  flush_return_buffer();
               }

               if (_logging_level > 1) {
                  sprintf(str, "MIRROR receive entry #%d", message_id);
                  write_logfile(lbs, str);
               }

               if (!getcfg(lbs->name, "Mirror simulate", str, sizeof(str)) || atoi(str) == 0) {
                  receive_message(lbs, list[index], message_id, error_str, FALSE);

                  if (error_str[0]) {
                     sprintf(str, "%s: %s", loc("Error receiving message"), error_str);
                     mprint(lbs, mode, str);
                  } else if (mode == SYNC_HTML) {

                     rsprintf("%s\n", loc("Remote entry received"));

                  } else if (mode == SYNC_CLONE) {
                     eprintf("%s\n", loc("Remote entry received"));
                  } else {
                     sprintf(str, "ID%d:\t%s", message_id, loc("Remote entry received"));
                     mprint(lbs, mode, str);
                  }

                  if (!error_str[0])
                     md5_cache[i_cache].message_id = -1;

               } else {
                  sprintf(str, "ID%d:\t%s", message_id, loc("Remote entry should be received"));
                  mprint(lbs, mode, str);
               }

            } else
               /* if message has been changed remotely and on this server, show conflict */
               if (!equal_md5(md5_cache[i_cache].md5_digest, md5_remote[i_remote].md5_digest)
                   && !equal_md5(md5_cache[i_cache].md5_digest, lbs->el_index[i_msg].md5_digest)
                   && !equal_md5(md5_remote[i_remote].md5_digest, lbs->el_index[i_msg].md5_digest)) {

               all_identical = FALSE;

               if (mode == SYNC_CLONE) {

                  eprintf("Warning: Entry #%d has been changed locally and remotely, will not be retrieved\n",
                          message_id);

               } else {

                  if (_logging_level > 1) {
                     sprintf(str, "MIRROR conflict entry #%d", message_id);
                     write_logfile(lbs, str);
                  }

                  combine_url(lbs, list[index], "", str, sizeof(str));

                  if (getcfg_topgroup())
                     sprintf(loc_ref, "<a href=\"../%s/%d\">%s</a>", lbs->name_enc, message_id, loc("local"));
                  else if (sync_all)
                     sprintf(loc_ref, "<a href=\"%s/%d\">%s</a>", lbs->name_enc, message_id, loc("local"));
                  else
                     sprintf(loc_ref, "<a href=\"%d\">%s</a>", message_id, loc("local"));

                  sprintf(rem_ref, "<a href=\"http://%s%d\">%s</a>", str, message_id, loc("remote"));

                  sprintf(str, "ID%d:\t%s. ", message_id, loc("Entry has been changed locally and remotely"));
                  sprintf(str + strlen(str), loc("Please delete %s or %s entry to resolve conflict"),
                          loc_ref, rem_ref);
                  strcat(str, ".");
                  mprint(lbs, mode, str);
               }

            } else {

               /* messages are identical */
               md5_cache[i_cache].message_id = -1;
            }
         }

         if (exist_cache && !exist_remote) {

            /* if message has been changed locally, send it */
            if (!equal_md5(md5_cache[i_cache].md5_digest, lbs->el_index[i_msg].md5_digest)) {

               /* compare MD5s */
               if (verbose) {
                  eprintf("ID%-5d: ", message_id);
                  for (j = 0; j < 16; j++)
                     eprintf("%02X", lbs->el_index[i_msg].md5_digest[j]);
                  eprintf("\nCache  : ");
                  for (j = 0; j < 16; j++)
                     eprintf("%02X", md5_cache[i_cache].md5_digest[j]);
                  eprintf("\nRemote : none");
                  eprintf("\n\n");
               }

               all_identical = FALSE;

               if (_logging_level > 1) {
                  sprintf(str, "MIRROR send entry #%d", message_id);
                  write_logfile(lbs, str);
               }

               /* submit local message */
               if (!getcfg(lbs->name, "Mirror simulate", str, sizeof(str)) || atoi(str) == 0) {
                  submit_message(lbs, list[index], message_id, error_str);

                  /* not that submit_message() may have changed attr_list !!! */

                  if (error_str[0])
                     sprintf(str, "%s: %s", loc("Error sending local message"), error_str);
                  else
                     sprintf(str, "ID%d:\t%s", message_id, loc("Local entry submitted"));
                  mprint(lbs, mode, str);

                  md5_cache[i_cache].message_id = -1;

               } else {
                  sprintf(str, "ID%d:\t%s", message_id, loc("Local entry should be submitted"));
                  mprint(lbs, mode, str);
               }

            } else {

               /* if message exists only in cache, but not remotely,
                  it must have been deleted remotely, so remove it locally */

               if (!isparam("confirm") && mode == SYNC_HTML) {

                  combine_url(lbs, list[index], "", str, sizeof(str));

                  if (getcfg_topgroup())
                     sprintf(loc_ref, "<a href=\"../%s/%d\">%s</a>", lbs->name_enc, message_id, loc("local"));
                  else if (sync_all)
                     sprintf(loc_ref, "<a href=\"%s/%d\">%s</a>", lbs->name_enc, message_id,
                             loc("Local entry"));
                  else
                     sprintf(loc_ref, "<a href=\"%d\">%s</a>", message_id, loc("Local entry"));

                  sprintf(str, loc("%s should be deleted"), loc_ref);
                  rsprintf("ID%d:\t%s\n", message_id, str);
                  n_delete++;

               }
               if (!isparam("confirm") && mode == SYNC_CLONE) {

                  sprintf(str, "ID%d:\t%s", message_id, loc("Entry should be deleted locally"));
                  mprint(lbs, mode, str);

               } else {

                  all_identical = FALSE;

                  if (mode == SYNC_CLONE) {

                     el_delete_message(lbs, message_id, TRUE, NULL, TRUE, TRUE);

                     sprintf(str, "ID%d:\t%s", message_id, loc("Entry deleted locally"));
                     mprint(lbs, mode, str);

                     /* message got deleted from local message list, so redo current index */
                     i_msg--;

                  } else {

                     if (_logging_level > 1) {
                        sprintf(str, "MIRROR delete local entry #%d", message_id);
                        write_logfile(lbs, str);
                     }

                     if (!getcfg(lbs->name, "Mirror simulate", str, sizeof(str)) || atoi(str) == 0) {
                        el_delete_message(lbs, message_id, TRUE, NULL, TRUE, TRUE);

                        sprintf(str, "ID%d:\t%s", message_id, loc("Entry deleted locally"));
                        mprint(lbs, mode, str);

                        /* message got deleted from local message list, so redo current index */
                        i_msg--;
                     } else {
                        sprintf(str, "ID%d:\t%s", message_id, loc("Entry should be deleted locally"));
                        mprint(lbs, mode, str);
                     }
                  }

                  /* mark message non-conflicting */
                  md5_cache[i_cache].message_id = -1;
               }
            }
         }

         /* if message does not exist in cache and remotely,
            it must be new, so send it  */
         if (!exist_cache && !exist_remote) {

            all_identical = FALSE;

            if (_logging_level > 1) {
               sprintf(str, "MIRROR send entry #%d", message_id);
               write_logfile(lbs, str);
            }
            remote_id = 0;
            if (!getcfg(lbs->name, "Mirror simulate", str, sizeof(str)) || atoi(str) == 0) {
               remote_id = submit_message(lbs, list[index], message_id, error_str);

               if (error_str[0])
                  sprintf(str, "%s: %s", loc("Error sending local entry"), error_str);
               else if (remote_id != message_id)
                  sprintf(str, "Error: Submitting entry #%d resulted in remote entry #%d\n", message_id,
                          remote_id);
               else
                  sprintf(str, "ID%d:\t%s", message_id, loc("Local entry submitted"));
               mprint(lbs, mode, str);
            } else {
               sprintf(str, "ID%d:\t%s", message_id, loc("Local entry should be submitted"));
               mprint(lbs, mode, str);
            }
         }

         /* if message does not exist in cache but remotely,
            messages were added on both sides, so resubmit local one and retrieve remote one
            if messages are different */
         if (!exist_cache && exist_remote && !equal_md5(md5_remote[i_remote].md5_digest,
                                                        lbs->el_index[i_msg].md5_digest)) {

            /* compare MD5s */
            if (verbose) {
               eprintf("ID%-5d: ", message_id);
               for (j = 0; j < 16; j++)
                  eprintf("%02X", lbs->el_index[i_msg].md5_digest[j]);
               eprintf("\nCache  : none");
               eprintf("\nRemote : ");
               for (j = 0; j < 16; j++)
                  eprintf("%02X", md5_remote[i_remote].md5_digest[j]);
               eprintf("\n\n");
            }

            all_identical = FALSE;

            /* find max id both locally and remotely */
            max_id = 1;
            for (i = 0; i < *lbs->n_el_index; i++)
               if (lbs->el_index[i].message_id > max_id)
                  max_id = lbs->el_index[i].message_id;

            for (i = 0; i < n_remote; i++)
               if (md5_remote[i].message_id > max_id)
                  max_id = md5_remote[i].message_id;

            if (_logging_level > 1) {
               sprintf(str, "MIRROR change entry #%d to #%d", message_id, max_id + 1);
               write_logfile(lbs, str);
            }

            /* rearrange local message not to conflict with remote message */
            if (!getcfg(lbs->name, "Mirror simulate", str, sizeof(str)) || atoi(str) == 0) {
               el_move_message(lbs, message_id, max_id + 1);

               sprintf(str, "ID%d:\t", message_id);
               sprintf(str + strlen(str), loc("Changed local entry ID to %d"), max_id + 1);
               mprint(lbs, mode, str);

               /* current message has been changed, so start over */
               i_msg--;
            } else {
               sprintf(str, "ID%d:\t", message_id);
               sprintf(str + strlen(str), loc("Local entry ID should be changed to %d"), max_id + 1);
               mprint(lbs, mode, str);
            }
         }

         flush_return_buffer();
      }

      /* go through remote message which do not exist locally */
      for (i_remote = 0; i_remote < n_remote; i_remote++)
         if (md5_remote[i_remote].message_id) {

            message_id = md5_remote[i_remote].message_id;

            for (i_msg = 0; i_msg < *lbs->n_el_index; i_msg++)
               if (message_id == lbs->el_index[i_msg].message_id)
                  break;

            if (i_msg == *lbs->n_el_index) {

               for (i_cache = 0; i_cache < n_cache; i_cache++)
                  if (md5_cache[i_cache].message_id == message_id)
                     break;
               exist_cache = i_cache < n_cache;

               if (!exist_cache) {

                  all_identical = FALSE;

                  if (mode == SYNC_HTML) {
                     if (getcfg_topgroup())
                        rsprintf("<a href=\"../%s/%d\">ID%d:</a>\t", lbs->name_enc, message_id, message_id);
                     else if (sync_all)
                        rsprintf("<a href=\"%s/%d\">ID%d:</a>\t", lbs->name_enc, message_id, message_id);
                     else
                        rsprintf("<a href=\"%d\">ID%d:</a>\t", message_id, message_id);

                     flush_return_buffer();
                  } else if (mode == SYNC_CLONE) {
                     eprintf("ID%d:\t", message_id);
                  }

                  /* if message does not exist locally and in cache, it is new, so retrieve it */
                  if (!getcfg(lbs->name, "Mirror simulate", str, sizeof(str)) || atoi(str) == 0) {
                     receive_message(lbs, list[index], message_id, error_str, TRUE);

                     if (error_str[0]) {
                        sprintf(str, "Error receiving message: %s", error_str);
                        mprint(lbs, mode, str);
                     } else if (mode == SYNC_HTML) {
                        rsprintf("%s\n", loc("Remote entry received"));
                     } else if (mode == SYNC_CLONE) {
                        eprintf("%s\n", loc("Remote entry received"));
                     } else {
                        sprintf(str, "ID%d:\t%s", message_id, loc("Remote entry received"));
                        mprint(lbs, mode, str);
                     }
                  } else {
                     sprintf(str, "ID%d:\t%s", message_id, loc("Remote entry should be received"));
                     mprint(lbs, mode, str);
                  }

               } else {

                  if (!equal_md5(md5_cache[i_cache].md5_digest, md5_remote[i_remote].md5_digest)) {

                     /* compare MD5s */
                     if (verbose) {
                        eprintf("ID-%5: none", message_id);
                        eprintf("\nCache  : ");
                        for (j = 0; j < 16; j++)
                           eprintf("%02X", md5_cache[i_cache].md5_digest[j]);
                        eprintf("\nRemote : ");
                        for (j = 0; j < 16; j++)
                           eprintf("%02X", md5_remote[i_remote].md5_digest[j]);
                        eprintf("\n\n");
                     }

                     all_identical = FALSE;

                     /* if message has changed remotely, receive it */
                     if (!getcfg(lbs->name, "Mirror simulate", str, sizeof(str)) || atoi(str) == 0) {
                        receive_message(lbs, list[index], message_id, error_str, TRUE);

                        if (error_str[0]) {
                           sprintf(str, "Error receiving message: %s", error_str);
                           mprint(lbs, mode, str);
                        } else if (mode == SYNC_HTML) {

                           if (getcfg_topgroup())
                              rsprintf("<a href=\"../%s/%d\">ID%d:</a>\t", lbs->name_enc, message_id,
                                       message_id);
                           else if (sync_all)
                              rsprintf("<a href=\"%s/%d\">ID%d:</a>\t", lbs->name_enc, message_id,
                                       message_id);
                           else
                              rsprintf("<a href=\"%d\">ID%d:</a>\t", message_id, message_id);

                           rsprintf("%s\n", loc("Remote entry received"));
                        }
                     } else {
                        sprintf(str, "ID%d:\t%s", message_id, loc("Remote entry should be received"));
                        mprint(lbs, mode, str);
                     }

                  } else {

                     /* if message does not exist locally but in cache, delete remote message */

                     all_identical = FALSE;

                     if (!isparam("confirm") && mode == SYNC_HTML) {

                        combine_url(lbs, list[index], "", str, sizeof(str));
                        sprintf(rem_ref, "<a href=\"http://%s%d\">%s</a>", str, message_id,
                                loc("Remote entry"));

                        sprintf(str, loc("%s should be deleted"), rem_ref);
                        rsprintf("ID%d:\t%s\n", message_id, str);
                        n_delete++;

                     } else if (!isparam("confirm") && mode == SYNC_CLONE) {

                        sprintf(str, "ID%d:\t%s", message_id, loc("Entry should be deleted remotely"));
                        mprint(lbs, mode, str);

                     } else {

                        if (_logging_level > 1) {
                           sprintf(str, "MIRROR delete remote entry #%d", message_id);
                           write_logfile(lbs, str);
                        }

                        sprintf(str, "%d?cmd=%s&confirm=%s", message_id, loc("Delete"), loc("Yes"));
                        combine_url(lbs, list[index], str, url, sizeof(url));

                        if (!getcfg(lbs->name, "Mirror simulate", str, sizeof(str)) || atoi(str) == 0) {
                           retrieve_url(url, &buffer, NULL);

                           if (strstr(buffer, "Location: ")) {
                              if (mode == SYNC_HTML)
                                 rsprintf("ID%d:\t%s\n", message_id, loc("Entry deleted remotely"));
                           } else {

                              if (mode == SYNC_HTML && isparam("debug"))
                                 rsputs(buffer);

                              mprint(lbs, mode, loc("Error deleting remote entry"));
                           }

                           md5_cache[i_cache].message_id = -1;
                           xfree(buffer);
                        } else {
                           sprintf(str, "ID%d:\t%s", message_id, loc("Entry should be deleted remotely"));
                           mprint(lbs, mode, str);
                        }
                     }
                  }
               }
            }

            flush_return_buffer();
         }

      xfree(md5_remote);

      /* save remote MD5s in file */
      if (!all_identical) {
         n_remote = retrieve_remote_md5(lbs, list[index], &md5_remote, error_str);
         if (n_remote < 0)
            rsprintf("%s\n", error_str);

         /* keep conflicting messages in cache */
         for (i = 0; i < n_cache; i++)
            if (md5_cache[i].message_id != -1) {

               if (i == 0)
                  memcpy(md5_remote[0].md5_digest, md5_cache[0].md5_digest, 16);
               else
                  for (j = 0; j < n_remote; j++)
                     if (md5_remote[j].message_id == md5_cache[i].message_id) {
                        memcpy(md5_remote[j].md5_digest, md5_cache[i].md5_digest, 16);
                        break;
                     }
            }

         if (!getcfg(lbs->name, "Mirror simulate", str, sizeof(str)) || atoi(str) == 0)
            save_md5(lbs, list[index], md5_remote, n_remote);

         if (md5_remote)
            xfree(md5_remote);
      }

      if (md5_cache)
         xfree(md5_cache);

      if (mode == SYNC_HTML && n_delete) {

         if (getcfg_topgroup())
            rsprintf("<br><b><a href=\"../%s/?cmd=Synchronize&confirm=1\">", lbs->name_enc);
         else if (sync_all)
            rsprintf("<br><b><a href=\"%s/?cmd=Synchronize&confirm=1\">", lbs->name_enc);
         else
            rsprintf("<br><b><a href=\"../%s/?cmd=Synchronize&confirm=1\">", lbs->name_enc);

         if (n_delete > 1)
            rsprintf(loc("Click here to delete %d entries"), n_delete);
         else
            rsprintf(loc("Click here to delete this entry"));

         rsprintf("</a></b>\n");
      }

      if (mode == SYNC_HTML && all_identical)
         rsprintf(loc("All entries identical"));

      if (mode == SYNC_CLONE && all_identical)
         mprint(lbs, mode, loc("All entries identical"));

      if (mode == SYNC_HTML)
         rsprintf("</pre>\n");
   }

   flush_return_buffer();
   keep_alive = FALSE;
}

/*------------------------------------------------------------------*/

void synchronize(LOGBOOK * lbs, int mode)
{
   int i;
   char str[256], pwd[256];

   if (mode == SYNC_HTML) {
      show_html_header(NULL, FALSE, loc("Synchronization"), TRUE, FALSE, NULL, FALSE);
      rsprintf("<body>\n");
   }

   if (lbs == NULL) {
      for (i = 0; lb_list[i].name[0]; i++)
         if (getcfg(lb_list[i].name, "mirror server", str, sizeof(str))) {

            if (exist_top_group() && getcfg_topgroup())
               if (lb_list[i].top_group[0] && !strieq(lb_list[i].top_group, getcfg_topgroup()))
                  continue;

            /* skip if excluded */
            if (getcfg(lb_list[i].name, "Mirror exclude", str, sizeof(str)) && atoi(str) == 1)
               continue;

            /* if called by cron, set user name and password */
            if (mode == SYNC_CRON && getcfg(lb_list[i].name, "mirror user", str, sizeof(str))) {
               if (get_user_line(&lb_list[i], str, pwd, NULL, NULL, NULL, NULL) == EL_SUCCESS) {
                  setparam("unm", str);
                  setparam("upwd", pwd);
               }
            }

            synchronize_logbook(&lb_list[i], mode, TRUE);
         }
   } else
      synchronize_logbook(lbs, mode, FALSE);

   if (mode == SYNC_HTML) {
      rsprintf("<table width=\"100%%\" cellpadding=\"1\" cellspacing=\"0\"");
      rsprintf("<tr><td class=\"seltitle\"><a href=\".\">%s</a></td></tr>\n", loc("Back"));
      rsprintf("</table><p>\n");

      rsprintf("</body></html>\n");
      flush_return_buffer();
      keep_alive = FALSE;
   }
}

/*------------------------------------------------------------------*/

void display_line(LOGBOOK * lbs, int message_id, int number, char *mode, int expand, int level,
                  BOOL printable, int n_line, int show_attachments, char *date, char *reply_to,
                  int n_attr_disp, char disp_attr[MAX_N_ATTR + 4][NAME_LENGTH],
                  BOOL disp_attr_link[MAX_N_ATTR + 4], char attrib[MAX_N_ATTR][NAME_LENGTH], int n_attr,
                  char *text, BOOL show_text, char attachment[MAX_ATTACHMENTS][MAX_PATH_LENGTH],
                  char *encoding, BOOL select, int *n_display, char *locked_by, int highlight,
                  regex_t * re_buf, int highlight_mid, int absolute_link)
{
   char str[NAME_LENGTH], ref[256], *nowrap, sclass[80], format[256], file_name[MAX_PATH_LENGTH], *slist,
       *svalue, comment[256];
   char display[NAME_LENGTH], attr_icon[80];
   int i, j, n, i_line, index, colspan, n_attachments, line_len, thumb_status, max_line_len, n_lines,
       max_n_lines;
   BOOL skip_comma;
   FILE *f;
   struct tm *pts;
   time_t ltime;

   slist = xmalloc((MAX_N_ATTR + 10) * NAME_LENGTH);
   svalue = xmalloc((MAX_N_ATTR + 10) * NAME_LENGTH);

   _current_message_id = message_id;
   ref[0] = 0;
   if (absolute_link)
      compose_base_url(lbs, ref, sizeof(ref), FALSE);
   sprintf(ref + strlen(ref), "../%s/%d", lbs->name_enc, message_id);

   if (strieq(mode, "Summary")) {
      if (highlight_mid == message_id) {
         if (number % 2 == 1)
            strcpy(sclass, "list1h");
         else
            strcpy(sclass, "list2h");
      } else {
         if (number % 2 == 1)
            strcpy(sclass, "list1");
         else
            strcpy(sclass, "list2");
      }
   } else if (strieq(mode, "Full")) {
      if (highlight_mid == message_id)
         strcpy(sclass, "list1h");
      else
         strcpy(sclass, "list1");
   } else if (strieq(mode, "Threaded")) {
      if (highlight) {
         if (highlight == message_id)
            strcpy(sclass, "thread");
         else
            strcpy(sclass, "threadreply");
      } else {
         if (highlight_mid == message_id) {
            if (level == 0)
               strcpy(sclass, "threadh");
            else
               strcpy(sclass, "threadreplyh");
         } else {
            if (level == 0)
               strcpy(sclass, "thread");
            else
               strcpy(sclass, "threadreply");
         }
      }
   }

   rsprintf("<tr>");

   /* check attributes for style */
   for (i = 0; i < n_attr; i++) {
      sprintf(str, "Style %s %s", attr_list[i], attrib[i]);
      if (getcfg(lbs->name, str, display, sizeof(display))) {
         sprintf(str, "%s\" style=\"%s\"", sclass, display);
         strlcpy(sclass, str, sizeof(sclass));
         break;
      }
   }

   /* only single cell for threaded display */
   if (strieq(mode, "Threaded")) {
      rsprintf("<td align=left class=\"%s\">", sclass);

      if (locked_by && locked_by[0]) {
         sprintf(str, "%s %s", loc("Entry is currently edited by"), locked_by);
         rsprintf("<img src=\"stop.png\" alt=\"%s\" title=\"%s\">&nbsp;", str, str);
      }

      /* show select box */
      if (select && level == 0)
         rsprintf("<input type=checkbox name=\"s%d\" value=%d>\n", (*n_display)++, message_id);

      for (i = 0; i < level; i++)
         rsprintf("&nbsp;&nbsp;&nbsp;");

      /* display "+" if expandable */
      if (expand == 0 && reply_to[0])
         rsprintf("+&nbsp;");
   }

   nowrap = printable ? "" : "nowrap";
   skip_comma = FALSE;

   if (getcfg(lbs->name, "List conditions", str, sizeof(str)) && atoi(str) == 1)
      evaluate_conditions(lbs, attrib);

   if (strieq(mode, "Threaded") && getcfg(lbs->name, "Thread display", display, sizeof(display))) {
      /* check if to use icon from attributes */
      attr_icon[0] = 0;
      if (getcfg(lbs->name, "Thread icon", attr_icon, sizeof(attr_icon))) {
         for (i = 0; i < n_attr; i++)
            if (strieq(attr_list[i], attr_icon))
               break;
         if (i < n_attr && attrib[i][0])
            strcpy(attr_icon, attrib[i]);
         else
            attr_icon[0] = 0;
      }

      if (highlight != message_id)
         rsprintf("\n<a href=\"%s\">", ref);

      if (attr_icon[0])
         rsprintf("\n<img border=0 src=\"icons/%s\" alt=\"%s\" title=\"%s\">\n&nbsp;", attr_icon, attr_icon,
                  attr_icon);
      else {
         /* display standard icons */
         if (level == 0)
            rsprintf("\n<img border=0 src=\"entry.png\" alt=\"%s\" title=\"%s\">\n&nbsp;", loc("Entry"),
                     loc("Entry"));
         else
            rsprintf("\n<img border=0 src=\"reply.png\" alt=\"%s\" title=\"%s\">\n&nbsp;", loc("Reply"),
                     loc("Reply"));
      }
      if (highlight != message_id)
         rsprintf("</a>\n");

      j = build_subst_list(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, attrib, TRUE);
      sprintf(str, "%d", message_id);
      add_subst_list((char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, "message id", str, &j);
      add_subst_time(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, "entry time", date,
                     &j, 0);

      strsubst_list(display, sizeof(display), (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue,
                    j);

      if (highlight != message_id)
         rsprintf("\n<a href=\"%s\">\n", ref);
      else
         rsprintf("\n<b>");

      if (is_html(display))
         rsputs(display);
      else
         rsputs2(lbs, absolute_link, display);

      rsputs("&nbsp;");
      for (i = n = 0; i < MAX_ATTACHMENTS; i++)
         if (attachment && attachment[i][0])
            n++;
      if (n > 5) {
         if (highlight != message_id)
            rsprintf("<a href=\"%s\">", ref);
         rsprintf("<b>%dx", n);
         rsprintf("<img border=\"0\" align=\"absmiddle\" src=\"attachment.png\"></b></a>");
      } else {
         for (i = 0; i < MAX_ATTACHMENTS; i++)
            if (attachment && attachment[i][0]) {
               strlcpy(str, attachment[i], sizeof(str));
               str[13] = 0;
               sprintf(ref, "../%s/%s/%s", lbs->name, str, attachment[i] + 14);
               url_encode(ref, sizeof(ref));    /* for file names with special characters like "+" */

               rsprintf("<a href=\"%s\" target=\"_blank\">", ref);
               rsprintf
                   ("<img border=\"0\" align=\"absmiddle\" src=\"attachment.png\" alt=\"%s\" title=\"%s\"></a>",
                    attachment[i] + 14, attachment[i] + 14);
            }
      }

      if (highlight != message_id)
         rsprintf("</a>\n", ref);
      else
         rsprintf("</b>\n");
   } else {
      /* show select box */
      if (select && !strieq(mode, "Threaded")) {
         rsprintf("<td class=\"%s\">", sclass);
         rsprintf("<input type=checkbox name=\"s%d\" value=%d>\n", (*n_display)++, message_id);
         rsprintf("</td>\n");
      }

      if (strieq(mode, "Threaded")) {
         if (highlight != message_id)
            rsprintf("\n<a href=\"%s\">\n", ref);
         else
            rsprintf("\n<b>");
      }

      skip_comma = TRUE;
      for (index = 0; index < n_attr_disp; index++) {
         if (strieq(disp_attr[index], loc("ID"))) {
            if (strieq(mode, "Threaded")) {
               if (level == 0)
                  rsprintf("\n<img border=0 src=\"entry.png\" alt=\"%s\" title=\"%s\">&nbsp;", loc("Entry"),
                           loc("Entry"));
               else
                  rsprintf("\n<img border=0 src=\"reply.png\" alt=\"%s\" title=\"%s\">&nbsp;", loc("Reply"),
                           loc("Reply"));

            } else {
               rsprintf("<td class=\"%s\">", sclass);

               if (locked_by && locked_by[0]) {
                  sprintf(str, "%s %s", loc("Entry is currently edited by"), locked_by);
                  rsprintf("\n<img src=\"stop.png\" alt=\"%s\" title=\"%s\">&nbsp;", str, str);
               }

               if (getcfg(lbs->name, "ID display", display, sizeof(display))) {
                  j = build_subst_list(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue,
                                       attrib, TRUE);
                  sprintf(str, "%d", message_id);
                  add_subst_list((char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, "message id",
                                 str, &j);
                  add_subst_time(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue,
                                 "entry time", date, &j, 0);

                  strsubst_list(display, sizeof(display), (char (*)[NAME_LENGTH]) slist,
                                (char (*)[NAME_LENGTH]) svalue, j);

               } else
                  sprintf(display, "%d", message_id);

               rsprintf("\n<a href=\"%s\">&nbsp;&nbsp;%s&nbsp;&nbsp;</a>\n", ref, display);
               rsprintf("</td>\n");
            }
         }

         if (strieq(disp_attr[index], loc("Logbook"))) {
            if (strieq(mode, "Threaded")) {
               if (skip_comma) {
                  rsprintf(" %s", lbs->name);
                  skip_comma = FALSE;
               } else
                  rsprintf(", %s", lbs->name);
            } else {
               if (disp_attr_link == NULL || disp_attr_link[index])
                  rsprintf("\n<td class=\"%s\" %s><a href=\"%s\">%s</a></td>\n", sclass, nowrap, ref,
                           lbs->name);
               else
                  rsprintf("\n<td class=\"%s\" %s>%s</td>\n", sclass, nowrap, lbs->name);
            }
         }

         if (strieq(disp_attr[index], loc("Edit"))) {
            if (!strieq(mode, "Threaded")) {
               rsprintf("\n<td class=\"%s\" %s><a href=\"%s?cmd=%s\">", sclass, nowrap, ref, loc("Edit"));
               rsprintf("\n<img src=\"edit.png\" border=0 alt=\"%s\" title=\"%s\"></a></td>\n",
                        loc("Edit entry"), loc("Edit entry"));
            }
         }

         if (strieq(disp_attr[index], loc("Delete"))) {
            if (!strieq(mode, "Threaded")) {
               rsprintf("\n<td class=\"%s\" %s><a href=\"%s?cmd=%s\">", sclass, nowrap, ref, loc("Delete"));
               rsprintf("\n<img src=\"delete.png\" border=0 alt=\"%s\" title=\"%s\"></a></td>\n",
                        loc("Delete entry"), loc("Delete entry"));
            }
         }

         if (strieq(disp_attr[index], loc("Date"))) {
            if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
               strcpy(format, DEFAULT_TIME_FORMAT);

            ltime = date_to_ltime(date);
            pts = localtime(&ltime);
            assert(pts);
            my_strftime(str, sizeof(str), format, pts);

            if (strieq(mode, "Threaded")) {
               if (skip_comma) {
                  rsprintf(" %s", str);
                  skip_comma = FALSE;
               } else
                  rsprintf(", %s", str);
            } else {
               if (disp_attr_link == NULL || disp_attr_link[index])
                  rsprintf("\n<td class=\"%s\" %s><a href=\"%s\">%s</a></td>\n", sclass, nowrap, ref, str);
               else
                  rsprintf("\n<td class=\"%s\" %s>%s</td>\n", sclass, nowrap, str);
            }
         }

         for (i = 0; i < n_attr; i++)
            if (strieq(disp_attr[index], attr_list[i])) {
               if (strieq(mode, "Threaded")) {
                  if (strieq(attr_options[i][0], "boolean")) {
                     if (atoi(attrib[i]) == 1) {
                        if (skip_comma) {
                           rsprintf(" ");
                           skip_comma = FALSE;
                        } else
                           rsprintf(", ");

                        if (is_html(attrib[i]))
                           rsputs(attrib[i]);
                        else
                           rsputs2(lbs, absolute_link, attrib[i]);

                        rsprintf("&nbsp");
                     }
                  }

                  else if (attr_flags[i] & AF_DATE) {
                     if (skip_comma) {
                        rsprintf(" ");
                        skip_comma = FALSE;
                     } else
                        rsprintf(", ");

                     if (!getcfg(lbs->name, "Date format", format, sizeof(format)))
                        strcpy(format, DEFAULT_DATE_FORMAT);

                     ltime = atoi(attrib[i]);
                     pts = localtime(&ltime);
                     assert(pts);
                     if (ltime == 0)
                        strcpy(str, "-");
                     else
                        my_strftime(str, sizeof(str), format, pts);

                     rsputs(str);
                  }

                  else if (attr_flags[i] & AF_DATETIME) {
                     if (skip_comma) {
                        rsprintf(" ");
                        skip_comma = FALSE;
                     } else
                        rsprintf(", ");

                     if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
                        strcpy(format, DEFAULT_TIME_FORMAT);

                     ltime = atoi(attrib[i]);
                     pts = localtime(&ltime);
                     assert(pts);
                     if (ltime == 0)
                        strcpy(str, "-");
                     else
                        my_strftime(str, sizeof(str), format, pts);

                     rsputs(str);
                  }

                  else if (attr_flags[i] & AF_ICON) {

                     sprintf(str, "Icon comment %s", attrib[i]);
                     getcfg(lbs->name, str, comment, sizeof(comment));
                     if (!comment[0])
                        strcpy(comment, attrib[i]);

                     if (attrib[i][0])
                        rsprintf("&nbsp;\n<img border=0 src=\"icons/%s\" alt=\"%s\" title=\"%s\">&nbsp;",
                                 attrib[i], comment, comment);
                  }

                  else {
                     if (skip_comma) {
                        rsprintf(" ");
                        skip_comma = FALSE;
                     } else
                        rsprintf(", ");

                     if (is_html(attrib[i]))
                        rsputs(attrib[i]);
                     else
                        rsputs2(lbs, absolute_link, attrib[i]);

                  }
               } else {
                  if (strieq(attr_options[i][0], "boolean")) {
                     if (atoi(attrib[i]) == 1)
                        rsprintf("\n<td class=\"%s\"><input type=checkbox checked disabled></td>\n", sclass);
                     else
                        rsprintf("\n<td class=\"%s\"><input type=checkbox disabled></td>\n", sclass);
                  }

                  else if (attr_flags[i] & AF_DATE) {

                     if (!getcfg(lbs->name, "Date format", format, sizeof(format)))
                        strcpy(format, DEFAULT_DATE_FORMAT);

                     ltime = atoi(attrib[i]);
                     pts = localtime(&ltime);
                     assert(pts);
                     if (ltime == 0)
                        strcpy(str, "-");
                     else
                        my_strftime(str, sizeof(str), format, pts);

                     if (disp_attr_link == NULL || disp_attr_link[index])
                        rsprintf("\n<td class=\"%s\"><a href=\"%s\">%s</a></td>\n", sclass, ref, str);
                     else
                        rsprintf("\n<td class=\"%s\">%s</td>\n", sclass, str);
                  }

                  else if (attr_flags[i] & AF_DATETIME) {

                     if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
                        strcpy(format, DEFAULT_TIME_FORMAT);

                     ltime = atoi(attrib[i]);
                     pts = localtime(&ltime);
                     assert(pts);
                     if (ltime == 0)
                        strcpy(str, "-");
                     else
                        my_strftime(str, sizeof(str), format, pts);

                     if (disp_attr_link == NULL || disp_attr_link[index])
                        rsprintf("\n<td class=\"%s\"><a href=\"%s\">%s</a></td>\n", sclass, ref, str);
                     else
                        rsprintf("\n<td class=\"%s\">%s</td>\n", sclass, str);
                  }

                  else if (attr_flags[i] & AF_ICON) {
                     rsprintf("<td class=\"%s\">", sclass);

                     sprintf(str, "Icon comment %s", attrib[i]);
                     getcfg(lbs->name, str, comment, sizeof(comment));
                     if (!comment[0])
                        strcpy(comment, attrib[i]);

                     if (attrib[i][0])
                        rsprintf("\n<img border=0 src=\"icons/%s\" alt=\"%s\" title=\"%s\">", attrib[i],
                                 comment, comment);

                     rsprintf("&nbsp;</td>");
                  }

                  else {
                     rsprintf("<td class=\"%s\">", sclass);

                     if (is_html(attrib[i]))
                        rsputs(attrib[i]);
                     else {
                        if (disp_attr_link == NULL || disp_attr_link[index])
                           rsprintf("<a href=\"%s\">", ref);

                        sprintf(str, "List Change %s", attr_list[i]);
                        if (getcfg(lbs->name, str, display, sizeof(display))) {
                           j = build_subst_list(lbs, (char (*)[NAME_LENGTH]) slist,
                                                (char (*)[NAME_LENGTH]) svalue, attrib, TRUE);
                           sprintf(str, "%d", message_id);
                           add_subst_list((char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue,
                                          "message id", str, &j);
                           add_subst_time(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue,
                                          "entry time", date, &j, 0);

                           strsubst_list(display, sizeof(display), (char (*)[NAME_LENGTH]) slist,
                                         (char (*)[NAME_LENGTH]) svalue, j);

                        } else
                           strcpy(display, attrib[i]);

                        if (is_html(display))
                           rsputs(display);
                        else {
                           if (isparam(attr_list[i])) {
                              highlight_searchtext(re_buf + 1 + i, display, str, TRUE);
                              strlcpy(display, str, sizeof(display));
                           } else if (isparam("subtext") && isparam("sall") && atoi(getparam("sall"))) {
                              highlight_searchtext(re_buf, display, str, TRUE);
                              strlcpy(display, str, sizeof(display));
                           }
                           rsputs2(lbs, absolute_link, display);
                        }

                        if (disp_attr_link == NULL || disp_attr_link[index])
                           rsprintf("</a>");

                        /* at least one space to produce non-empty table cell */
                        if (!display[0])
                           rsprintf("&nbsp;");
                     }

                     rsprintf("</td>");
                  }
               }
            }
      }

      if (strieq(mode, "Threaded")) {

         rsputs("&nbsp;");

         for (i = n = 0; i < MAX_ATTACHMENTS; i++)
            if (attachment && attachment[i][0])
               n++;
         if (n > 5) {
            if (highlight != message_id)
               rsprintf("<a href=\"%s\">", ref);
            rsprintf("<b>%dx", n);
            rsprintf("<img border=\"0\" align=\"absmiddle\" src=\"attachment.png\"></b></a>");
         } else {
            for (i = 0; i < MAX_ATTACHMENTS; i++)
               if (attachment && attachment[i][0]) {
                  strlcpy(str, attachment[i], sizeof(str));
                  str[13] = 0;
                  sprintf(ref, "../%s/%s/%s", lbs->name, str, attachment[i] + 14);
                  url_encode(ref, sizeof(ref)); /* for file names with special characters like "+" */

                  rsprintf("<a href=\"%s\" target=\"_blank\">", ref);
                  rsprintf
                      ("<img border=\"0\" align=\"absmiddle\" src=\"attachment.png\" alt=\"%s\" title=\"%s\"></a>",
                       attachment[i] + 14, attachment[i] + 14);
               }
         }

         if (highlight != message_id)
            rsprintf("</a>\n");
         else
            rsprintf("</b>\n");
      }
   }

   if (strieq(mode, "Threaded") && expand > 1 && show_text) {
      rsprintf("</td></tr>\n");

      rsprintf("<tr><td class=\"summary\">");

      if (expand == 2) {
         for (i = i_line = line_len = 0; i < (int) sizeof(str) - 1; i++, line_len++) {
            str[i] = text[i];
            if (line_break(text+i, encoding)) {
               i_line++;
               line_len = 0;
            } else
               /* limit line length to 150 characters */
            if (line_len > 150 && text[i] == ' ') {
               str[i] = '\n';
               i_line++;
               line_len = 0;
            }

            if (i_line == n_line)
               break;
         }
         str[i] = 0;

         /* only show text, not to rip apart HTML documents,
            e.g. only the start of a table */
         strip_html(str);
         if (str[0])
            strencode_nouml(str);
         else
            rsputs("&nbsp;");
      } else {
         if (strieq(encoding, "plain")) {
            rsputs("<pre>");
            if (text[0])
               rsputs2(lbs, absolute_link, text);
            else
               rsputs("&nbsp;");
            rsputs("</pre>");
         } else if (strieq(encoding, "ELCode"))
            rsputs_elcode(lbs, FALSE, text);
         else if (text[0])
            rsputs(text);
         else
            rsputs("&nbsp;");
      }

      rsprintf("</td></tr>\n");
   }

   if (strieq(mode, "Summary") && n_line > 0 && show_text) {
      rsprintf("<td class=\"summary\">");

      max_line_len = n_line >= 10 ? 140 : 40;
      for (i = i_line = line_len = 0; i < (int) sizeof(str) - 1; line_len++, i++) {
         str[i] = text[i];
         if (line_break(text+i, encoding)) {
            i_line++;
            line_len = 0;
         } else
            /* limit line length to max_line_len characters */
         if (line_len > max_line_len && text[i] == ' ') {
            str[i] = '\n';
            i_line++;
            line_len = 0;
         }

         if (i_line == n_line)
            break;
      }
      str[i] = 0;

      /* only show text, not to rip apart HTML documents,
         e.g. only the start of a table */
      strip_html(str);
      if (str[0])
         strencode_nouml(str);
      else
         rsputs("&nbsp;");

      rsputs("</td>\n");
   }

   if (strieq(mode, "Summary")) {
      /* show attachment icons */
      rsputs("<td class=\"listatt\">&nbsp;");

      for (i = n = 0; i < MAX_ATTACHMENTS; i++)
         if (attachment && attachment[i][0])
            n++;
      if (n > 5) {
         if (highlight != message_id)
            rsprintf("<a href=\"%s\">", ref);
         rsprintf("<b>%dx", n);
         rsprintf("<img border=\"0\" align=\"absmiddle\" src=\"attachment.png\"></b></a>");
      } else {
         for (i = 0; i < MAX_ATTACHMENTS; i++)
            if (attachment && attachment[i][0]) {
               strlcpy(str, attachment[i], sizeof(str));
               str[13] = 0;
               sprintf(ref, "../%s/%s/%s", lbs->name, str, attachment[i] + 14);
               url_encode(ref, sizeof(ref));    /* for file names with special characters like "+" */

               rsprintf("<a href=\"%s\" target=\"_blank\">", ref);
               rsprintf
                   ("<img border=\"0\" align=\"absmiddle\" src=\"attachment.png\" alt=\"%s\" title=\"%s\"></a>",
                    attachment[i] + 14, attachment[i] + 14);
            }
      }

      rsputs("&nbsp;</td>");
   }

   colspan = n_attr_disp;

   if (select)
      colspan++;

   if (strieq(mode, "Full") && show_text) {
      if (!getcfg(lbs->name, "Show text", str, sizeof(str)) || atoi(str) == 1) {
         rsprintf("<tr><td class=\"messagelist\" colspan=%d>", colspan);

         if (strieq(encoding, "plain")) {
            rsputs("<pre>");
            rsputs2(lbs, absolute_link, text);
            rsputs("</pre>");
         } else if (strieq(encoding, "ELCode"))
            rsputs_elcode(lbs, FALSE, text);
         else
            rsputs(text);

         rsprintf("</td></tr>\n");
      }

      /* count number of attachments */
      n_attachments = 0;
      if (show_attachments) {
         for (index = 0; index < MAX_ATTACHMENTS; index++) {
            if (attachment[index][0]) {
               /* check if attachment is inlined */
               sprintf(str, "[img]elog:/%d[/img]", index + 1);
               if (strieq(encoding, "ELCode") && stristr(text, str))
                  continue;

               n_attachments++;
            }
         }
      }

      for (index = 0; index < MAX_ATTACHMENTS; index++) {
         if (show_attachments && attachment[index][0]) {

            /* check if attachment is inlined */
            if (is_inline_attachment(encoding, message_id, text, index, attachment[index]))
               continue;

            strlcpy(str, attachment[index], sizeof(str));
            str[13] = 0;
            sprintf(ref, "../%s/%s/%s", lbs->name, str, attachment[index] + 14);
            url_encode(ref, sizeof(ref));       /* for file names with special characters like "+" */

            strlcpy(file_name, lbs->data_dir, sizeof(file_name));
            strlcat(file_name, attachment[index], sizeof(file_name));
            thumb_status = create_thumbnail(lbs, file_name);

            if (!show_attachments) {
               rsprintf("<a href=\"%s\" target=\"_blank\">%s</a>&nbsp;&nbsp;&nbsp;&nbsp; ", ref,
                        attachment[index] + 14);
            } else {

               if (thumb_status) {
                  rsprintf
                      ("<tr><td colspan=%d class=\"attachment\">%s %d: <a href=\"%s\" target=\"_blank\">%s</a>\n",
                       colspan, loc("Attachment"), index + 1, ref, attachment[index] + 14);

                  if (show_attachments) {
                     rsprintf("<tr><td colspan=%d class=\"attachmentframe\">\n", colspan);
                     if (thumb_status == 2) {
                        for (i = 0;; i++) {
                           strlcpy(str, file_name, sizeof(str));
                           if (chkext(str, ".pdf") || chkext(str, ".ps"))
                              if (strrchr(str, '.'))
                                 *strrchr(str, '.') = 0;
                           sprintf(str + strlen(str), "-%d.png", i);
                           if (file_exist(str)) {
                              strlcpy(str, ref, sizeof(str));
                              if (chkext(file_name, ".pdf") || chkext(file_name, ".ps"))
                                 if (strrchr(str, '.'))
                                    *strrchr(str, '.') = 0;
                              sprintf(str + strlen(str), "-%d.png", i);
                              rsprintf("<a name=\"att%d\" href=\"%s\">\n", index + 1, ref);
                              rsprintf("<img src=\"%s\" alt=\"%s\" title=\"%s\"></a>\n", str,
                                       attachment[index] + 14, attachment[index] + 14);
                           } else
                              break;
                        }
                     } else {
                        rsprintf("<a name=\"att%d\" href=\"%s\">\n", index + 1, ref);
                        strlcpy(str, ref, sizeof(str));
                        if (chkext(str, ".pdf") || chkext(str, ".ps"))
                           if (strrchr(str, '.'))
                              *strrchr(str, '.') = 0;
                        strlcat(str, ".png", sizeof(str));
                        rsprintf("<img src=\"%s\" alt=\"%s\" title=\"%s\"></a>\n", str, attachment[index]
                                 + 14, attachment[index] + 14);
                     }
                     rsprintf("</td></tr>\n\n");
                  }
               } else {
                  if (is_image(attachment[index])) {
                     rsprintf
                         ("<tr><td colspan=%d class=\"attachment\">%s %d: <a href=\"%s\" target=\"_blank\">%s</a>\n",
                          colspan, loc("Attachment"), index + 1, ref, attachment[index] + 14);
                     if (show_attachments) {
                        rsprintf("</td></tr><tr>");
                        rsprintf("<td colspan=%d class=\"attachmentframe\">", colspan);
                        rsprintf("<img src=\"%s\" alt=\"%s\" title=\"%s\">", ref, attachment[index] + 14,
                                 attachment[index] + 14);
                        rsprintf("</td></tr>\n");
                     }
                  } else {
                     rsprintf
                         ("<tr><td colspan=%d class=\"attachment\">%s %d: <a href=\"%s\" target=\"_blank\">%s</a>\n",
                          colspan, loc("Attachment"), index + 1, ref, attachment[index] + 14);

                     strlcpy(file_name, lbs->data_dir, sizeof(file_name));
                     strlcat(file_name, attachment[index], sizeof(file_name));

                     if (is_ascii(file_name) && !chkext(attachment[index], ".PS")
                         && !chkext(attachment[index], ".PDF") && !chkext(attachment[index], ".EPS")
                         && !chkext(attachment[index], ".HTM") && !chkext(attachment[index], ".HTML")
                         && show_attachments) {

                        rsprintf("</td></tr>\n");

                        /* display attachment */
                        rsprintf("<tr><td colspan=%d class=\"messageframe\">\n", colspan);

                        /* anchor for references */
                        rsprintf("<a name=\"att%d\"></a>\n", index + 1);

                        /* display attachment */

                        if (!chkext(attachment[index], ".HTML"))
                           rsprintf("<pre class=\"messagepre\">");

                        strlcpy(file_name, lbs->data_dir, sizeof(file_name));
                        strlcat(file_name, attachment[index], sizeof(file_name));

                        f = fopen(file_name, "rt");

                        n_lines = 0;
                        if (getcfg(lbs->name, "Attachment lines", str, sizeof(str)))
                           max_n_lines = atoi(str);
                        else
                           max_n_lines = 300;

                        if (f != NULL) {
                           while (!feof(f)) {
                              str[0] = 0;
                              fgets(str, sizeof(str), f);

                              if (n_lines < max_n_lines) {
                                 if (!chkext(attachment[index], ".HTML"))
                                    rsputs2(lbs, absolute_link, str);
                                 else
                                    rsputs(str);
                              }
                              n_lines++;
                           }
                           fclose(f);
                        }

                        if (!chkext(attachment[index], ".HTML"))
                           rsprintf("</pre>");
                        rsprintf("\n");
                        if (max_n_lines == 0)
                           rsprintf("<i><b>%d lines</b></i>\n", n_lines);
                        else if (n_lines > max_n_lines)
                           rsprintf("<i><b>... %d more lines ...</b></i>\n", n_lines - max_n_lines);
                     }
                     rsprintf("</td></tr>\n");
                  }
               }
            }
         }
      }
   }

   xfree(slist);
   xfree(svalue);
}

/*------------------------------------------------------------------*/

void display_reply(LOGBOOK * lbs, int message_id, int printable, int expand, int n_line, int n_attr_disp,
                   char disp_attr[MAX_N_ATTR + 4][NAME_LENGTH], BOOL show_text, int level, int highlight,
                   regex_t * re_buf, int highlight_mid, int absolute_link)
{
   char *date, *text, *in_reply_to, *reply_to, *encoding, *locked_by, *attachment, *attrib, *p;
   int status, size;

   text = xmalloc(TEXT_SIZE);
   attachment = xmalloc(MAX_ATTACHMENTS * MAX_PATH_LENGTH);
   attrib = xmalloc(MAX_N_ATTR * NAME_LENGTH);
   date = xmalloc(80);
   in_reply_to = xmalloc(80);
   reply_to = xmalloc(256);
   encoding = xmalloc(80);
   locked_by = xmalloc(256);

   if (locked_by == NULL)
      return;

   reply_to[0] = 0;
   size = TEXT_SIZE;
   status = el_retrieve(lbs, message_id, date, attr_list, (void *) attrib, lbs->n_attr, text, &size,
                        in_reply_to, reply_to, (void *) attachment, encoding, locked_by);

   if (status != EL_SUCCESS) {
      xfree(text);
      xfree(attachment);
      xfree(attrib);
      xfree(date);
      xfree(in_reply_to);
      xfree(reply_to);
      xfree(encoding);
      xfree(locked_by);
      return;
   }

   display_line(lbs, message_id, 0, "threaded", expand, level, printable, n_line, FALSE, date, reply_to,
                n_attr_disp, disp_attr, NULL, (void *) attrib, lbs->n_attr, text, show_text,
                NULL, encoding, 0, NULL, locked_by, highlight, &re_buf[0], highlight_mid, absolute_link);

   if (reply_to[0]) {
      p = reply_to;
      do {
         display_reply(lbs, atoi(p), printable, expand, n_line, n_attr_disp, disp_attr, show_text, level + 1,
                       highlight, &re_buf[0], highlight_mid, absolute_link);

         while (*p && isdigit(*p))
            p++;
         while (*p && (*p == ',' || *p == ' '))
            p++;
      } while (*p);
   }

   xfree(text);
   xfree(attachment);
   xfree(attrib);
   xfree(date);
   xfree(in_reply_to);
   xfree(reply_to);
   xfree(encoding);
   xfree(locked_by);
}

/*------------------------------------------------------------------*/

int msg_compare(const void *m1, const void *m2)
{
   return strcoll(((MSG_LIST *) m1)->string, ((MSG_LIST *) m2)->string);
}

int msg_compare_reverse(const void *m1, const void *m2)
{
   return strcoll(((MSG_LIST *) m2)->string, ((MSG_LIST *) m1)->string);
}

int msg_compare_numeric(const void *m1, const void *m2)
{
   return ((MSG_LIST *) m1)->number - ((MSG_LIST *) m2)->number;
}

int msg_compare_reverse_numeric(const void *m1, const void *m2)
{
   return ((MSG_LIST *) m2)->number - ((MSG_LIST *) m1)->number;
}

/*------------------------------------------------------------------*/

char *param_in_str(char *str, char *param)
{
   char *p;

   p = str;
   do {
      if (stristr(p, param) == NULL)
         return NULL;

      p = stristr(p, param);

      /* if parameter is value of another parameter, skip it */
      if (p > str + 1 && *(p - 1) == '=')
         p += strlen(param);
      else
         return p;

      if (*p == 0)
         return NULL;

   } while (1);
}

/*------------------------------------------------------------------*/

BOOL subst_param(char *str, int size, char *param, char *value)
{
   int len;
   char *p1, *p2, *s, param_enc[256];

   strlcpy(param_enc, param, sizeof(param_enc));
   url_slash_encode(param_enc, sizeof(param_enc));

   if (!value[0]) {

      /* remove parameter */
      s = param_in_str(str, param_enc);

      if (s == NULL)
         return FALSE;

      /* remove parameter */
      p1 = s - 1;

      for (p2 = p1 + strlen(param_enc) + 1; *p2 && *p2 != '&'; p2++);
      strlcpy(p1, p2, size - (p1 - str));

      if (!strchr(str, '?') && strchr(str, '&'))
         *strchr(str, '&') = '?';

      return TRUE;
   }

   if ((p1 = param_in_str(str, param_enc)) == NULL) {
      if (strchr(str, '?'))
         strlcat(str, "&", size);
      else
         strlcat(str, "?", size);

      strlcat(str, param_enc, size);
      strlcat(str, "=", size);
      strlcat(str, value, size);
      return FALSE;
   }

   p1 += strlen(param_enc) + 1;
   for (p2 = p1; *p2 && *p2 != '&'; p2++);
   len = p2 - p1;
   if (len > (int) strlen(value)) {
      /* new value is shorter than old one */
      strlcpy(p1, value, size - (p1 - str));
      strlcpy(p1 + strlen(value), p2, size - (p1 + strlen(value) - str));
   } else {
      /* new value is longer than old one */
      s = xmalloc(size);
      strlcpy(s, p2, size);
      strlcpy(p1, value, size - (p1 - str));
      strlcat(p1, s, size - (p1 + strlen(value) - str));
      xfree(s);
   }

   return TRUE;
}

/*------------------------------------------------------------------*/

BOOL is_user_allowed(LOGBOOK * lbs, char *command)
{
   char str[1000], users[2000];
   char list[MAX_N_LIST][NAME_LENGTH];
   int i, n;

   /* check for user level access */
   if (!getcfg(lbs->name, "Password file", str, sizeof(str)))
      return TRUE;

   /* check for deny */
   sprintf(str, "Deny %s", command);
   if (getcfg(lbs->name, str, users, sizeof(users))) {

      if (!isparam("unm"))
         return FALSE;

      /* check if current user in list */
      n = strbreak(users, list, MAX_N_LIST, ",", FALSE);
      for (i = 0; i < n; i++)
         if (strieq(list[i], getparam("unm")))
            break;

      if (i < n)
         return FALSE;
   }

   /* check admin command */
   if (strieq(command, loc("Admin"))) {
      if (getcfg(lbs->name, "Admin user", str, sizeof(str))) {
         return is_admin_user(lbs->name, getparam("unm"));
      }
   }

   /* check for allow */
   sprintf(str, "Allow %s", command);
   if (!getcfg(lbs->name, str, users, sizeof(users)))
      return TRUE;

   /* check if current user in list */
   if (!isparam("unm"))
      return FALSE;

   n = strbreak(users, list, MAX_N_LIST, ",", FALSE);
   for (i = 0; i < n; i++)
      if (strieq(list[i], getparam("unm")))
         return TRUE;

   return FALSE;
}

/*------------------------------------------------------------------*/

BOOL is_command_allowed(LOGBOOK * lbs, char *command)
{
   char str[1000], menu_str[1000], other_str[1000];
   char menu_item[MAX_N_LIST][NAME_LENGTH];
   int i, n;

   if (command[0] == 0)
      return TRUE;

   /* check for guest access */
   if (!getcfg(lbs->name, "Guest Menu commands", menu_str, sizeof(menu_str)) || isparam("unm") != 0)
      getcfg(lbs->name, "Menu commands", menu_str, sizeof(menu_str));

   /* default menu commands */
   if (menu_str[0] == 0) {
      strcpy(menu_str, "List, New, Edit, Delete, Reply, Duplicate, Synchronize, Find, ");

      if (getcfg(lbs->name, "Password file", str, sizeof(str))) {

         if (is_admin_user(lbs->name, getparam("unm"))) {

            strcat(menu_str, "Admin, ");
            strcat(menu_str, "Change config file, ");
            strcat(menu_str, "Delete this logbook, ");
            strcat(menu_str, "Rename this logbook, ");
            strcat(menu_str, "Create new logbook, ");
            strcat(menu_str, "GetPwdFile, ");

            if (is_admin_user("global", getparam("unm"))) {

               if (lbs->top_group[0]) {
                  sprintf(str, "Change [global %s]", lbs->top_group);
                  strcat(menu_str, str);
                  strcat(menu_str, ", ");
               }

               if (!lbs->top_group[0] || (is_admin_user("global", getparam("unm")))) {

                  strcat(menu_str, "Change [global]");
                  strcat(menu_str, ", ");
               }
            }
         }
         strcat(menu_str, "Config, Logout, ");
      } else {
         strcat(menu_str, "Config, ");
         strcat(menu_str, "Change [global], ");
         strcat(menu_str, "Delete this logbook, ");
         strcat(menu_str, "Rename this logbook, ");
         strcat(menu_str, "Create new logbook, ");
      }

      strcat(menu_str, "Help, HelpELCode, ");

   } else {
      /* check for admin command */
      n = strbreak(menu_str, menu_item, MAX_N_LIST, ",", FALSE);
      menu_str[0] = 0;
      for (i = 0; i < n; i++) {
         if (strcmp(menu_item[i], "Admin") == 0) {
            if (!is_admin_user(lbs->name, getparam("unm")))
               continue;
         }
         strcat(menu_str, menu_item[i]);
         strcat(menu_str, ", ");
      }

      strcat(menu_str, "HelpELCode, Synchronize, ");

      if (is_admin_user(lbs->name, getparam("unm"))) {

         strcat(menu_str, "Change config file, ");
         strcat(menu_str, "Delete this logbook, ");
         strcat(menu_str, "Rename this logbook, ");
         strcat(menu_str, "Create new logbook, ");
         strcat(menu_str, "GetPwdFile, ");

         if (is_admin_user("global", getparam("unm"))) {

            if (lbs->top_group[0]) {
               sprintf(str, "Change [global %s]", lbs->top_group);
               strcat(menu_str, str);
               strcat(menu_str, ", ");
            }

            if (!lbs->top_group[0] || (is_admin_user("global", getparam("unm")))) {

               strcat(menu_str, "Change [global]");
               strcat(menu_str, ", ");
            }
         }
      }
   }

   /* check list menu commands */
   str[0] = 0;
   if (!getcfg(lbs->name, "Guest List Menu commands", str, sizeof(str)) || isparam("unm") != 0)
      getcfg(lbs->name, "list menu commands", str, sizeof(str));

   if (!str[0]) {
      if (!getcfg(lbs->name, "Guest Find Menu commands", str, sizeof(str)) || isparam("unm") != 0)
         getcfg(lbs->name, "Find Menu commands", str, sizeof(str));
   }

   if (str[0])
      strlcat(menu_str, str, sizeof(menu_str));
   else {
      strlcat(menu_str, "New, Find, Select, Last x, Help, HelpELCode, ", sizeof(menu_str));

      if (getcfg(lbs->name, "Password file", str, sizeof(str)))
         strlcat(menu_str, "Admin, Config, Logout, ", sizeof(menu_str));
      else
         strlcat(menu_str, "Config, ", sizeof(menu_str));
   }

   strcpy(other_str, "Preview, Back, Search, Download, Import, CSV Import, XML Import, ");
   strlcat(other_str, "Cancel, First, Last, Previous, Next, Requested, Forgot, ", sizeof(other_str));

   /* only allow Submit & Co if "New" is allowed */
   if (stristr(menu_str, "New"))
      strlcat(other_str, "Update, Upload, Submit, Save, ", sizeof(other_str));

   /* add save for new user registration */
   if (isparam("new_user_name"))
      strlcat(other_str, "Save, ", sizeof(other_str));

   /* admin commands */
   if (is_admin_user(lbs->name, getparam("unm"))) {
      strcat(other_str, "Remove user, New user, Activate, ");
   } else if (getcfg(lbs->name, "Self register", str, sizeof(str)) && atoi(str) > 0) {
      strcat(other_str, "Remove user, New user, ");
   }

   /* allow change password if "config" possible */
   if (strieq(command, loc("Change password")) && stristr(menu_str, "Config")) {
      return TRUE;
   }
   /* exclude non-localized submit for elog */
   else if (command[0] && strieq(command, "Submit") && stristr(menu_str, "New")) {
      return TRUE;
   }
   /* exclude other non-localized commands */
   else if (command[0] && strieq(command, "GetMD5")) {
      return TRUE;
   } else if (command[0] && strieq(command, "IM")) {
      return TRUE;
   }
   /* check if command is present in the menu list */
   else if (command[0]) {
      n = strbreak(menu_str, menu_item, MAX_N_LIST, ",", FALSE);
      for (i = 0; i < n; i++)
         if (strieq(command, menu_item[i]) || strieq(command, loc(menu_item[i])))
            break;

      if (i == n) {
         n = strbreak(other_str, menu_item, MAX_N_LIST, ",", FALSE);
         for (i = 0; i < n; i++)
            if (strieq(command, menu_item[i]) || strieq(command, loc(menu_item[i])))
               break;

         if (i == n)
            return FALSE;
      }
   }

   return TRUE;
}

/*------------------------------------------------------------------*/

void build_ref(char *ref, int size, char *mode, char *expand, char *attach, char *new_entries)
{
   char str[1000], *p;

   if (strchr(getparam("cmdline"), '?'))
      strlcat(ref, strchr(getparam("cmdline"), '?'), size);

   /* eliminate old search */
   if (strstr(ref, "cmd=Search&"))
      strlcpy(strstr(ref, "cmd=Search&"), strstr(ref, "cmd=Search&") + 11, sizeof(str));

   /* eliminate id=xxx */
   if (strstr(ref, "id=")) {
      p = strstr(ref, "id=") + 3;
      while (*p && isdigit(*p))
         p++;
      strlcpy(strstr(ref, "id="), p, sizeof(str));
      if (strlen(ref) > 0 && ref[strlen(ref) - 1] == '?')
         ref[strlen(ref) - 1] = 0;
   }

   /* eliminate old mode if new one is present */
   if (mode[0])
      subst_param(ref, size, "mode", mode);

   /* eliminate old expand if new one is present */
   if (expand[0])
      subst_param(ref, size, "expand", expand);

   /* eliminate old attach if new one is present */
   if (attach[0])
      subst_param(ref, size, "attach", attach);

   /* eliminate old new_entries if new one is present */
   if (new_entries[0])
      subst_param(ref, size, "new_entries", new_entries);

   /* eliminate old last= */
   if (isparam("last"))
      subst_param(ref, size, "last", getparam("last"));

   /* replace any '&' by '&amp;' */
   strlcpy(str, ref, sizeof(str));
   strencode2(ref, str, size);
}

/*------------------------------------------------------------------*/

void show_page_filters(LOGBOOK * lbs, int n_msg, int page_n, BOOL mode_commands, char *mode)
{
   int cur_exp, n, i, j, i1, i2, index, attr_index, size;
   char ref[256], str[NAME_LENGTH], comment[NAME_LENGTH], list[MAX_N_LIST][NAME_LENGTH],
       option[NAME_LENGTH], option_whole[NAME_LENGTH];

   rsprintf("<tr><td class=\"menuframe\">\n");
   rsprintf("<table width=\"100%%\" border=0 cellpadding=\"0\" cellspacing=\"0\">\n");

   rsprintf("<tr>\n");

   if (mode_commands) {
      rsprintf("<td class=\"menu2a\">\n");

      if (!getcfg(lbs->name, "Show text", str, sizeof(str)) || atoi(str) == 1) {
         if (page_n != 1)
            sprintf(ref, "page%d", page_n);
         else
            ref[0] = 0;
         build_ref(ref, sizeof(ref), "full", "", "", "");

         if (strieq(mode, "full"))
            rsprintf("&nbsp;%s&nbsp;|", loc("Full"));
         else
            rsprintf("&nbsp;<a href=\"%s\">%s</a>&nbsp;|", ref, loc("Full"));
      }

      if (page_n != 1)
         sprintf(ref, "page%d", page_n);
      else
         ref[0] = 0;
      build_ref(ref, sizeof(ref), "summary", "", "", "");

      if (strieq(mode, "summary"))
         rsprintf("&nbsp;%s&nbsp;|", loc("Summary"));
      else
         rsprintf("&nbsp;<a href=\"%s\">%s</a>&nbsp;|", ref, loc("Summary"));

      if (page_n != 1)
         sprintf(ref, "page%d", page_n);
      else
         ref[0] = 0;
      build_ref(ref, sizeof(ref), "threaded", "", "", "");

      if (strieq(mode, "threaded"))
         rsprintf("&nbsp;%s&nbsp;", loc("Threaded"));
      else
         rsprintf("&nbsp;<a href=\"%s\">%s</a>&nbsp;", ref, loc("Threaded"));

      if (strieq(mode, "full")) {
         if (page_n != 1)
            sprintf(ref, "page%d", page_n);
         else
            ref[0] = 0;

         cur_exp = 0;
         if (strieq(mode, "full"))
            cur_exp = 1;
         if (isparam("elattach"))
            cur_exp = atoi(getparam("elattach"));
         if (isparam("attach"))
            cur_exp = atoi(getparam("attach"));

         if (cur_exp) {
            build_ref(ref, sizeof(ref), "", "", "0", "");
            rsprintf("|&nbsp;<a href=\"%s\">%s</a>&nbsp;", ref, loc("Hide attachments"));
         } else {
            build_ref(ref, sizeof(ref), "", "", "1", "");
            rsprintf("|&nbsp;<a href=\"%s\">%s</a>&nbsp;", ref, loc("Show attachments"));
         }
      }

      if (strieq(mode, "threaded")) {
         if (page_n != 1)
            sprintf(ref, "page%d", page_n);
         else
            ref[0] = 0;

         cur_exp = 1;
         if (getcfg(lbs->name, "Expand default", str, sizeof(str)))
            cur_exp = atoi(str);
         if (isparam("expand"))
            cur_exp = atoi(getparam("expand"));

         if (cur_exp > 0) {
            sprintf(str, "%d", cur_exp > 0 ? cur_exp - 1 : 0);
            build_ref(ref, sizeof(ref), "", str, "", "");
            rsprintf("|&nbsp;<a href=\"%s\">%s</a>&nbsp;", ref, loc("Collapse"));
         } else
            rsprintf("|&nbsp;%s&nbsp;", loc("Collapse"));

         if (cur_exp < 3) {
            if (page_n != 1)
               sprintf(ref, "page%d", page_n);
            else
               ref[0] = 0;
            sprintf(str, "%d", cur_exp < 3 ? cur_exp + 1 : 3);
            build_ref(ref, sizeof(ref), "", str, "", "");
            rsprintf("|&nbsp;<a href=\"%s\">%s</a>&nbsp;", ref, loc("Expand"));
         } else
            rsprintf("|&nbsp;%s&nbsp;", loc("Expand"));
      }

      rsprintf("</td>\n");
   }

   rsprintf("<td class=\"menu2b\">\n");

   /*---- filter menu text ----*/

   if (getcfg(lbs->name, "filter menu text", str, sizeof(str))) {
      FILE *f;
      char file_name[256], *buf;

      /* check if file starts with an absolute directory */
      if (str[0] == DIR_SEPARATOR || str[1] == ':')
         strcpy(file_name, str);
      else {
         strlcpy(file_name, logbook_dir, sizeof(file_name));
         strlcat(file_name, str, sizeof(file_name));
      }

      f = fopen(file_name, "rb");
      if (f != NULL) {
         fseek(f, 0, SEEK_END);
         size = TELL(fileno(f));
         fseek(f, 0, SEEK_SET);

         buf = xmalloc(size + 1);
         fread(buf, 1, size, f);
         buf[size] = 0;
         fclose(f);

         rsputs(buf);

      } else
         rsprintf("<center><b>Error: file <i>\"%s\"</i> not found</b></center>", file_name);
   }

   ref[0] = 0;
   if (!isparam("new_entries") || atoi(getparam("new_entries")) == 0) {
      build_ref(ref, sizeof(ref), "", "", "", "1");
      rsprintf
          ("<a href=\"%s\"><img align=\"middle\" border=\"0\" src=\"new_entry.png\" alt=\"%s\" title=\"%s\"></a>&nbsp;&nbsp;",
           ref, loc("Show only new entries"), loc("Show only new entries"));
   } else {
      build_ref(ref, sizeof(ref), "", "", "", "0");
      rsprintf
          ("<a href=\"%s\"><img align=\"middle\" border=\"0\" src=\"all_entry.png\" alt=\"%s\" title=\"%s\"></a>&nbsp;&nbsp;",
           ref, loc("Show all entries"), loc("Show all entries"));
   }

   if (getcfg(lbs->name, "Quick filter", str, sizeof(str))) {

      n = strbreak(str, list, MAX_N_LIST, ",", FALSE);

      if (getcfg(lbs->name, "Case sensitive search", str, sizeof(str)) && atoi(str))
         rsprintf("<input type=hidden name=\"casesensitive\" value=1>\n");

      for (index = 0; index < n; index++) {
         /* find according attribute index */
         for (attr_index = 0; attr_index < lbs->n_attr; attr_index++)
            if (strieq(list[index], attr_list[attr_index]))
               break;

         if (attr_index == lbs->n_attr && !strieq(list[index], "Date") && !strieq(list[index], "Subtext")
             && !strieq(list[index], "ID")) {
            rsprintf("Error: Attribute \"%s\" for quick filter not found", list[index]);
            attr_index = 0;
         }

         if (strieq(list[index], "Date")) {
            i = isparam("last") ? atoi(getparam("last")) : 0;

            rsprintf("<select title=\"%s\" name=last onChange=\"document.form1.submit()\">\n",
                     loc("Select period"));

            rsprintf("<option value=\"_all_\">-- %s --\n", loc("All entries"));

            rsprintf("<option %s value=1>%s\n", i == 1 ? "selected" : "", loc("Last day"));
            rsprintf("<option %s value=7>%s\n", i == 7 ? "selected" : "", loc("Last week"));
            rsprintf("<option %s value=31>%s\n", i == 31 ? "selected" : "", loc("Last month"));
            rsprintf("<option %s value=92>%s\n", i == 92 ? "selected" : "", loc("Last 3 Months"));
            rsprintf("<option %s value=182>%s\n", i == 182 ? "selected" : "", loc("Last 6 Months"));
            rsprintf("<option %s value=364>%s\n", i == 364 ? "selected" : "", loc("Last Year"));

            rsprintf("</select>\n");
         } else if (strieq(attr_options[attr_index][0], "boolean")) {

            sprintf(str, loc("Select %s"), list[index]);
            rsprintf("<select title=\"%s\" name=\"%s\" onChange=\"document.form1.submit()\">\n", str,
                     list[index]);

            rsprintf("<option value=\"_all_\">-- %s --\n", list[index]);

            if (isparam(attr_list[attr_index]) && strieq("1", getparam(attr_list[attr_index])))
               rsprintf("<option selected value=\"1\">1\n");
            else
               rsprintf("<option value=\"1\">1\n");

            if (isparam(attr_list[attr_index]) && strieq("0", getparam(attr_list[attr_index])))
               rsprintf("<option selected value=\"0\">0\n");
            else
               rsprintf("<option value=\"0\">0\n");

            rsprintf("</select>\n");
         } else {

            /* check if attribute has options */
            if (attr_list[attr_index][0] == 0 || attr_options[attr_index][0][0] == 0) {

               if (attr_flags[attr_index] & (AF_DATE | AF_DATETIME)) {

                  rsprintf("<select name=\"%s\" onChange=\"document.form1.submit()\">\n", list[index]);

                  i = isparam(list[index]) ? atoi(getparam(list[index])) : 0;

                  rsprintf("<option %s value=-364>%s %s\n", i == -364 ? "selected" : "", loc("Last"),
                           loc("Year"));
                  rsprintf("<option %s value=-182>%s %s\n", i == -182 ? "selected" : "", loc("Last"),
                           loc("6 Months"));
                  rsprintf("<option %s value=-92>%s %s\n", i == -92 ? "selected" : "", loc("Last"),
                           loc("3 Months"));
                  rsprintf("<option %s value=-31>%s %s\n", i == -31 ? "selected" : "", loc("Last"),
                           loc("Month"));
                  rsprintf("<option %s value=-7>%s %s\n", i == -7 ? "selected" : "", loc("Last"),
                           loc("Week"));
                  rsprintf("<option %s value=-1>%s %s\n", i == -1 ? "selected" : "", loc("Last"), loc("Day"));

                  sprintf(str, "-- %s  --", list[index]);
                  rsprintf("<option %s value=\"_all_\">%s\n", i == 0 ? "selected" : "", str);

                  rsprintf("<option %s value=1>%s %s\n", i == 1 ? "selected" : "", loc("Next"), loc("Day"));
                  rsprintf("<option %s value=7>%s %s\n", i == 7 ? "selected" : "", loc("Next"), loc("Week"));
                  rsprintf("<option %s value=31>%s %s\n", i == 31 ? "selected" : "", loc("Next"),
                           loc("Month"));
                  rsprintf("<option %s value=92>%s %s\n", i == 92 ? "selected" : "", loc("Next"),
                           loc("3 Months"));
                  rsprintf("<option %s value=182>%s %s\n", i == 182 ? "selected" : "", loc("Next"),
                           loc("6 Months"));
                  rsprintf("<option %s value=364>%s %s\n", i == 364 ? "selected" : "", loc("Next"),
                           loc("Year"));

                  rsprintf("</select>\n");
               }

               else {
                  if (strieq(list[index], "Subtext")) {
                     rsprintf
                         ("<input onClick=\"this.value='';\" title=\"%s\" type=text onChange=\"document.form1.submit()\"",
                          loc("Enter text"));
                     sprintf(str, "-- %s --", loc("Text"));
                     if (isparam(list[index]) && *getparam(list[index]))
                        strencode2(str, getparam(list[index]), sizeof(str));

                     rsprintf(" name=\"Subtext\" value=\"%s\">\n", str);
                  } else {
                     sprintf(str, loc("Enter %s"), list[index]);
                     rsprintf
                         ("<input onClick=\"this.value='';\" title=\"%s\" type=text onChange=\"document.form1.submit()\"",
                          str);
                     sprintf(str, "-- %s --", list[index]);
                     if (isparam(list[index]) && *getparam(list[index]))
                        strencode2(str, getparam(list[index]), sizeof(str));

                     rsprintf(" name=\"%s\" value=\"%s\">\n", list[index], str);
                  }
               }
            } else {

               sprintf(str, loc("Select %s"), list[index]);
               rsprintf("<select title=\"%s\" name=\"%s\" onChange=\"document.form1.submit()\">\n", str,
                        list[index]);

               rsprintf("<option value=\"_all_\">-- %s --\n", list[index]);

               for (j = 0; j < MAX_N_LIST && attr_options[attr_index][j][0]; j++) {
                  comment[0] = 0;
                  if (attr_flags[attr_index] & AF_ICON) {
                     sprintf(str, "Icon comment %s", attr_options[attr_index][j]);
                     getcfg(lbs->name, str, comment, sizeof(comment));
                  }

                  if (comment[0] == 0)
                     strcpy(comment, attr_options[attr_index][j]);

                  for (i1 = i2 = 0; i1 <= (int) strlen(comment); i1++) {
                     if (comment[i1] == '(') {
                        option[i2++] = '\\';
                        option[i2++] = '(';
                     } else if (comment[i1] == '{') {
                        option[i2] = 0;
                        break;
                     } else
                        option[i2++] = comment[i1];
                  }

                  sprintf(option_whole, "^%s$", option);
                  if (isparam(attr_list[attr_index]) &&
                      (strieq(option, getparam(attr_list[attr_index])) ||
                       strieq(option_whole, getparam(attr_list[attr_index]))))
                     rsprintf("<option selected value=\"%s\">%s\n", option_whole, option);
                  else
                     rsprintf("<option value=\"%s\">%s\n", option_whole, option);
               }

               rsprintf("</select> \n");
            }
         }
      }

      /* show button if JavaScript is switched off */
      rsprintf("<noscript>\n");
      rsprintf("<input type=submit value=\"%s\">\n", loc("Search"));
      rsprintf("</noscript>\n");

      /* show submit button for IE (otherwise <Return> will not work) */
      if (strstr(browser, "MSIE"))
         rsprintf("<input type=submit value=\"%s\">\n", loc("Search"));
   }

   rsprintf("&nbsp;<b>%d %s</b>&nbsp;", n_msg, loc("Entries"));

   rsprintf("</td></tr></table></td></tr>\n\n");
}

/*------------------------------------------------------------------*/

void show_page_navigation(LOGBOOK * lbs, int n_msg, int page_n, int n_page)
{
   int i, num_pages, skip, max_n_msg;
   char ref[256], str[256];

   if (!page_n || n_msg <= n_page)
      return;

   rsprintf("<tr><td class=\"menuframe\"><span class=\"menu3\">\n");

   rsprintf("%s \n", loc("Goto page"));

   if (page_n > 1) {
      sprintf(ref, "page%d", page_n - 1);
      build_ref(ref, sizeof(ref), "", "", "", "");

      rsprintf("<a href=\"%s\">%s</a>&nbsp;&nbsp;", ref, loc("Previous"));
   }

   if (page_n && n_msg > n_page) {
      /* number of pages */
      num_pages = (n_msg - 1) / n_page + 1;
      skip = FALSE;

      for (i = 1; i <= num_pages; i++) {
         sprintf(ref, "page%d", i);
         build_ref(ref, sizeof(ref), "", "", "", "");

         if (i <= 3 || (i >= page_n - 1 && i <= page_n + 1) || i >= num_pages - 2) {
            if (i > 1 && !skip)
               rsprintf(", \n");

            skip = FALSE;
         } else {
            if (!skip)
               rsprintf("&nbsp;...&nbsp;");

            skip = TRUE;
         }

         if (skip)
            continue;

         if (page_n == i)
            rsprintf("%d", i);
         else
            rsprintf("<a href=\"%s\">%d</a>", ref, i);

         /*
            if (i == num_pages )
            rsprintf("&nbsp;&nbsp;\n");
            else
            rsprintf(", ");
          */
      }

      rsprintf("&nbsp;&nbsp;\n");
   }

   if (page_n != -1 && n_page < n_msg && page_n * n_page < n_msg) {
      sprintf(ref, "page%d", page_n + 1);
      build_ref(ref, sizeof(ref), "", "", "", "");

      rsprintf("<a href=\"%s\">%s</a>&nbsp;&nbsp;", ref, loc("Next"));
   }

   if (getcfg(lbs->name, "All display limit", str, sizeof(str)))
      max_n_msg = atoi(str);
   else
      max_n_msg = 500;

   if (page_n != -1 && n_page < n_msg && n_msg < max_n_msg) {
      sprintf(ref, "page");
      build_ref(ref, sizeof(ref), "", "", "", "");

      rsprintf("<a href=\"%s\">%s</a>\n", ref, loc("All"));
   }

   rsprintf("</span></td></tr>\n");
}

/*------------------------------------------------------------------*/

void show_select_navigation(LOGBOOK * lbs)
{
   int i, n_log;
   char str[NAME_LENGTH];
   char lbk_list[MAX_N_LIST][NAME_LENGTH];

   rsprintf("<tr><td class=\"menuframe\"><span class=\"menu4\">\n");

   rsprintf("<script language=\"JavaScript\" type=\"text/javascript\">\n");
   rsprintf("<!--\n");
   rsprintf("function ToggleAll()\n");
   rsprintf("  {\n");
   rsprintf("  for (var i = 0; i < document.form1.elements.length; i++)\n");
   rsprintf("    {\n");
   rsprintf
       ("    if (document.form1.elements[i].type == 'checkbox' && document.form1.elements[i].disabled == false)\n");
   rsprintf("      document.form1.elements[i].checked = !(document.form1.elements[i].checked);\n");
   rsprintf("    }\n");
   rsprintf("  }\n");
   rsprintf("//-->\n");
   rsprintf("</script>\n");

   rsprintf("%s:&nbsp;\n", loc("Selected entries"));

   rsprintf("<input type=button value=\"%s\" onClick=\"ToggleAll();\">\n", loc("Toggle all"));

   if (!getcfg(lbs->name, "Menu commands", str, sizeof(str)) || stristr(str, "Delete")) {
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Delete"));
   }

   if (!getcfg(lbs->name, "Menu commands", str, sizeof(str)) || stristr(str, "Edit")) {
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Edit"));
   }

   if (getcfg(lbs->name, "Menu commands", str, sizeof(str)) && stristr(str, "Copy to")) {
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Copy to"));
      rsprintf("<select name=destc>\n");

      if (getcfg(lbs->name, "Copy to", str, sizeof(str))) {
         n_log = strbreak(str, lbk_list, MAX_N_LIST, ",", FALSE);

         for (i = 0; i < n_log; i++)
            rsprintf("<option value=\"%s\">%s\n", lbk_list[i], lbk_list[i]);
      } else {
         for (i = 0;; i++) {
            if (!enumgrp(i, str))
               break;

            if (!is_logbook(str))
               continue;

            if (strieq(str, lbs->name))
               continue;

            rsprintf("<option value=\"%s\">%s\n", str, str);
         }
      }
      rsprintf("</select>\n");
   }

   if (getcfg(lbs->name, "Menu commands", str, sizeof(str)) && stristr(str, "Move to")) {
      rsprintf("<input type=submit name=cmd value=\"%s\">\n", loc("Move to"));
      rsprintf("<select name=destm>\n");

      if (getcfg(lbs->name, "Move to", str, sizeof(str))) {
         n_log = strbreak(str, lbk_list, MAX_N_LIST, ",", FALSE);

         for (i = 0; i < n_log; i++)
            rsprintf("<option value=\"%s\">%s\n", lbk_list[i], lbk_list[i]);
      } else {
         for (i = 0;; i++) {
            if (!enumgrp(i, str))
               break;

            if (!is_logbook(str))
               continue;

            if (strieq(str, lbs->name))
               continue;

            rsprintf("<option value=\"%s\">%s\n", str, str);
         }
      }
      rsprintf("</select>\n");
   }

   rsprintf("</span></td></tr>\n\n");
}

/*------------------------------------------------------------------*/

time_t retrieve_date(char *index, BOOL bstart)
{
   int year, month, day, hour, min, sec, current_year, current_month, current_day;
   char pm[10], py[10], pd[10], ph[10], pn[10], ps[10], str[NAME_LENGTH], str2[NAME_LENGTH];
   struct tm tms;
   time_t ltime;

   sprintf(pm, "m%s", index);
   sprintf(py, "y%s", index);
   sprintf(pd, "d%s", index);
   sprintf(ph, "h%s", index);
   sprintf(pn, "n%s", index);
   sprintf(ps, "c%s", index);

   time(&ltime);
   memcpy(&tms, localtime(&ltime), sizeof(tms));
   current_year = tms.tm_year + 1900;
   current_month = tms.tm_mon + 1;
   current_day = tms.tm_mday;

   if (!isparam(pm) && !isparam(py) && !isparam(pd))
      return 0;

   /* if year not given, use current year */
   if (!isparam(py))
      year = current_year;
   else
      year = atoi(getparam(py));
   if (year < 1970) {
      sprintf(str, "Error: Year %s out of range", getparam(py));
      strencode2(str2, str, sizeof(str2));
      show_error(str2);
      return -1;
   }

   /* if month not given, use current month */
   if (isparam(pm)) {
      month = atoi(getparam(pm));
   } else
      month = current_month;

   if (isparam(pd))
      day = atoi(getparam(pd));
   else {
      /* if day not given, use 1 if start date */
      if (bstart)
         day = 1;
      else {
         /* use last day of month */
         memset(&tms, 0, sizeof(struct tm));
         tms.tm_year = year - 1900;
         tms.tm_mon = month - 1 + 1;
         tms.tm_mday = 1;
         tms.tm_hour = 12;

         if (tms.tm_year < 90)
            tms.tm_year += 100;
         ltime = mktime(&tms);
         ltime -= 3600 * 24;
         memcpy(&tms, localtime(&ltime), sizeof(struct tm));
         day = tms.tm_mday;
      }

   }

   /* if hour not given, use 0 */
   if (isparam(ph)) {
      hour = atoi(getparam(ph));
   } else
      hour = 0;

   /* if minute not given, use 0 */
   if (isparam(pn)) {
      min = atoi(getparam(pn));
   } else
      min = 0;

   /* if second not given, use 0 */
   if (isparam(ps)) {
      sec = atoi(getparam(ps));
   } else
      sec = 0;

   memset(&tms, 0, sizeof(struct tm));
   tms.tm_year = year - 1900;
   tms.tm_mon = month - 1;
   tms.tm_mday = day;
   tms.tm_hour = hour;
   tms.tm_min = min;
   tms.tm_sec = sec;
   tms.tm_isdst = -1;

   if (tms.tm_year < 90)
      tms.tm_year += 100;

   ltime = mktime(&tms);

   if (!bstart && isparam(ph) == 0)
      /* end time is first second of next day */
      ltime += 3600 * 24;

   return ltime;
}

/*------------------------------------------------------------------*/

time_t convert_date(char *date_string)
{
   /* convert date string in MM/DD/YY or DD.MM.YY format into Unix time */
   int year, month, day;
   char *p, str[256];
   struct tm tms;
   time_t ltime;

   strlcpy(str, date_string, sizeof(str));
   month = day = year = 0;

   if (strchr(str, '/')) {
      /* MM/DD/YY format */
      p = strtok(str, "/");
      if (p) {
         month = atoi(p);
         p = strtok(NULL, "/");
         if (p) {
            day = atoi(p);
            p = strtok(NULL, "/");
            if (p)
               year = atoi(p);
         }
      }
   } else if (strchr(str, '.')) {
      /* DD.MM.YY format */
      p = strtok(str, ".");
      if (p) {
         day = atoi(p);
         p = strtok(NULL, ".");
         if (p) {
            month = atoi(p);
            p = strtok(NULL, ".");
            if (p)
               year = atoi(p);
         }
      }
   } else
      return 0;

   /* calculate years */
   if (year > 1900)             /* 1900-2100 */
      year += 0;
   else if (year < 70)          /* 00-69 */
      year += 2000;
   else if (year < 100)         /* 70-99 */
      year += 1900;

   /* use last day of month */
   memset(&tms, 0, sizeof(struct tm));
   tms.tm_year = year - 1900;
   tms.tm_mon = month - 1;
   tms.tm_mday = day;
   tms.tm_hour = 12;

   ltime = mktime(&tms);

   return ltime;
}

/*------------------------------------------------------------------*/

time_t convert_datetime(char *date_string)
{
   /* convert date string in MM/DD/YY h:m:s AM/PM or DD.MM.YY hh:m:s format into Unix time */
   int year, month, day, hour, min, sec;
   char *p, str[256];
   struct tm tms;
   time_t ltime;

   strlcpy(str, date_string, sizeof(str));
   month = day = year = 0;

   if (strchr(str, '/')) {
      /* MM/DD/YY format */
      p = strtok(str, "/");
      if (p) {
         month = atoi(p);
         p = strtok(NULL, "/");
         if (p) {
            day = atoi(p);
            p = strtok(NULL, "/");
            if (p)
               year = atoi(p);
         }
      }
   } else if (strchr(str, '.')) {
      /* DD.MM.YY format */
      p = strtok(str, ".");
      if (p) {
         day = atoi(p);
         p = strtok(NULL, ".");
         if (p) {
            month = atoi(p);
            p = strtok(NULL, ".");
            if (p)
               year = atoi(p);
         }
      }
   } else
      return 0;

   strlcpy(str, p, sizeof(str));
   p = strtok(p, ":");
   if (p) {
      hour = atoi(p);
      p = strtok(NULL, ":");
      if (p) {
         min = atoi(p);
         p = strtok(NULL, ":");
         if (p)
            sec = atoi(p);
      }
   } else
      return 0;

   if (stristr(p, "PM") && hour < 12)
      hour += 12;

   /* calculate years */
   if (year > 1900)             /* 1900-2100 */
      year += 0;
   else if (year < 70)          /* 00-69 */
      year += 2000;
   else if (year < 100)         /* 70-99 */
      year += 1900;

   /* use last day of month */
   memset(&tms, 0, sizeof(struct tm));
   tms.tm_year = year - 1900;
   tms.tm_mon = month - 1;
   tms.tm_mday = day;
   tms.tm_hour = hour;
   tms.tm_min = min;
   tms.tm_sec = sec;

   ltime = mktime(&tms);

   return ltime;
}

/*------------------------------------------------------------------*/

void show_rss_feed(LOGBOOK * lbs)
{
   int i, n, size, index, status, message_id, offset;
   char str[256], charset[256], url[256], attrib[MAX_N_ATTR][NAME_LENGTH], date[80], *text, title[2000],
       slist[MAX_N_ATTR + 10][NAME_LENGTH], svalue[MAX_N_ATTR + 10][NAME_LENGTH];
   time_t ltime;
   struct tm *ts;

   rsprintf("HTTP/1.1 200 Document follows\r\n");
   rsprintf("Server: ELOG HTTP %s-%d\r\n", VERSION, atoi(svn_revision + 13));

   if (!getcfg("global", "charset", charset, sizeof(charset)))
      strcpy(charset, DEFAULT_HTTP_CHARSET);

   rsprintf("Content-Type: text/xml;charset=%s\r\n", charset);
   rsprintf("\r\n");

   rsprintf("<?xml version=\"1.0\" encoding=\"%s\"?>\n", charset);
   rsprintf("<rss version=\"2.0\">\n");
   rsprintf("<channel>\n");

   rsprintf("<title>ELOG %s</title>\n", lbs->name);

   /* retrive URL */
   getcfg(lbs->name, "URL", url, sizeof(url));

   /* if HTTP request comes from localhost, use localhost as
      absolute link (needed if running on DSL at home) */
   if (!url[0] && strstr(http_host, "localhost")) {
      if (_ssl_flag)
         strcpy(url, "https://localhost");
      else
         strcpy(url, "http://localhost");
      if (elog_tcp_port != 80)
         sprintf(url + strlen(url), ":%d", elog_tcp_port);
      strcat(url, "/");
   }

   if (!url[0]) {
      /* assemble absolute path from host name and port */
      if (_ssl_flag)
         sprintf(url, "https://%s", host_name);
      else
         sprintf(url, "http://%s", host_name);
      if (elog_tcp_port != 80)
         sprintf(url + strlen(url), ":%d", elog_tcp_port);
      strcat(url, "/");
   }

   /* add trailing '/' if not present */
   if (url[strlen(url) - 1] != '/')
      strcat(url, "/");

   strlcat(url, lbs->name_enc, sizeof(url));

   rsprintf("<link>%s</link>\n", url);

   if (getcfg(lbs->name, "Comment", str, sizeof(str))) {
      rsprintf("<description>");
      xmlencode(str);
      rsprintf("</description>\n");
   }

   rsprintf("<generator>ELOG V%s</generator>\n", VERSION);

   rsprintf("<image>\n");
   rsprintf("<url>%s/elog.png</url>\n", url);
   rsprintf("<title>ELOG %s</title>\n", lbs->name_enc);
   rsprintf("<link>%s</link>\n", url);
   rsprintf("</image>\n");

   /*---- show last <n> items ----*/

   n = 15;
   if (getcfg(lbs->name, "RSS Entries", str, sizeof(str)))
      n = atoi(str);

   text = xmalloc(TEXT_SIZE);
   message_id = el_search_message(lbs, EL_LAST, 0, FALSE);
   for (index = 0; index < n; index++) {
      rsprintf("<item>\n");

      size = TEXT_SIZE;
      status = el_retrieve(lbs, message_id, date, attr_list, attrib, lbs->n_attr, text, &size, NULL, NULL,
                           NULL, NULL, NULL);

      if (getcfg(lbs->name, "RSS Title", title, sizeof(title))) {
         i = build_subst_list(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, attrib,
                              TRUE);
         sprintf(str, "%d", message_id);
         add_subst_list((char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, "message id", str, &i);
         add_subst_time(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, "entry time",
                        date, &i, 0);

         strsubst_list(title, sizeof(title), (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue,
                       i);
      } else {

         title[0] = 0;
         for (i = 0; i < lbs->n_attr; i++) {
            if (attrib[i][0]) {
               strlcat(title, attrib[i], sizeof(title));
               if (i < lbs->n_attr - 1)
                  strlcat(title, ", ", sizeof(title));
            }
         }

      }

      /* convert date to RFC-822 date */
      setlocale(LC_ALL, "C");
      ltime = date_to_ltime(date);
      ts = localtime(&ltime);
      assert(ts);
      strftime(str, sizeof(str), "%a, %d %b %Y %H:%M:%S", ts);
      offset = (-(int) my_timezone());
      if (ts->tm_isdst)
         offset += 3600;
      snprintf(date, sizeof(date) - 1, "%s %+03d%02d", str, (int) (offset / 3600), (int) ((abs((int) offset)
                                                                                           / 60) % 60));
      getcfg("global", "Language", str, sizeof(str));
      if (str[0])
         setlocale(LC_ALL, str);

      rsprintf("<title>");
      xmlencode(title);
      rsprintf("</title>\n");

      rsprintf("<link>");
      strlcpy(str, url, sizeof(str));
      sprintf(str + strlen(str), "/%d", message_id);
      xmlencode(str);
      rsprintf("</link>\n");

      rsprintf("<description>\n");
      xmlencode(text);
      rsprintf("</description>\n");

      rsprintf("<pubDate>\n");
      rsprintf("%s", date);
      rsprintf("</pubDate>\n");

      rsprintf("</item>\n");
      message_id = el_search_message(lbs, EL_PREV, message_id, FALSE);
      if (!message_id)
         break;
   }

   xfree(text);
   rsprintf("</channel>\n");
   rsprintf("</rss>\n");
}

/*------------------------------------------------------------------*/

void highlight_searchtext(regex_t * re_buf, char *src, char *dst, int hidden)
{
   char *pt, *pt1;
   int size, status;
   regmatch_t pmatch[10];

   dst[0] = 0;
   pt = src;                    /* original text */
   pt1 = dst;                   /* text with inserted coloring */
   do {
      status = regexec(re_buf, pt, 10, pmatch, 0);
      if (status != REG_NOMATCH) {
         size = pmatch[0].rm_so;

         /* abort if zero length match, for example from "m*" */
         if (pmatch[0].rm_eo - pmatch[0].rm_so == 0) {
            status = REG_NOMATCH;
            strcpy(pt1, pt);
            break;
         }
         /* copy first part original text */
         memcpy(pt1, pt, size);
         pt1 += size;
         pt += size;

         /* add coloring 1st part */

         /* here: \001='<', \002='>', /003='"', and \004=' ' */
         /* see also rsputs2(char* ) */

         if (hidden)
            strcpy(pt1, "\001B\004style=\003color:black;background-color:#ffff66\003\002");
         else
            strcpy(pt1, "<B style=\"color:black;background-color:#ffff66\">");

         pt1 += strlen(pt1);

         /* copy origial search text */
         size = pmatch[0].rm_eo - pmatch[0].rm_so;
         memcpy(pt1, pt, size);
         pt1 += size;
         pt += size;

         /* add coloring 2nd part */
         if (hidden)
            strcpy(pt1, "\001/B\002");
         else
            strcpy(pt1, "</B>");
         pt1 += strlen(pt1);
      }
   } while (status != REG_NOMATCH);

   strcpy(pt1, pt);
}

/*------------------------------------------------------------------*/

void show_elog_list(LOGBOOK * lbs, int past_n, int last_n, int page_n, BOOL default_page, char *info)
{
   int i, j, n, index, size, status, d1, m1, y1, d2, m2, y2, n_line, flags, current_year, current_month,
       current_day, printable, n_logbook, n_display, reverse, numeric, n_attr_disp, total_n_msg, n_msg,
       search_all, message_id, n_page, i_start, i_stop, in_reply_to_id, page_mid, page_mid_head;
   char date[80], attrib[MAX_N_ATTR][NAME_LENGTH], disp_attr[MAX_N_ATTR + 4][NAME_LENGTH], *list, *text,
       *text1, in_reply_to[80], reply_to[MAX_REPLY_TO * 10], attachment[MAX_ATTACHMENTS][MAX_PATH_LENGTH],
       encoding[80], locked_by[256], str[NAME_LENGTH], ref[256], img[80], comment[NAME_LENGTH], mode[80],
       mid[80], menu_str[1000], menu_item[MAX_N_LIST][NAME_LENGTH], param[NAME_LENGTH], format[80],
       sort_attr[MAX_N_ATTR + 4][NAME_LENGTH], mode_cookie[80], charset[25], sort_item[NAME_LENGTH];
   char *p, *pt1, *pt2, *slist, *svalue, *gattr, line[1024], iattr[256];
   BOOL show_attachments, threaded, csv, xml, raw, mode_commands, expand, filtering, disp_filter, show_text,
       text_in_attr, searched, found, disp_attr_link[MAX_N_ATTR + 4], sort_attributes;
   time_t ltime, ltime_start, ltime_end, now, ltime1, ltime2;
   struct tm tms, *ptms;
   MSG_LIST *msg_list;
   LOGBOOK *lbs_cur;
   regex_t re_buf[MAX_N_ATTR + 1];
   regmatch_t pmatch[10];

   /* redirect if empty parameters */
   if (strstr(_cmdline, "=&")) {
      while ((pt1 = strstr(_cmdline, "=&")) != NULL) {
         pt2 = pt1;
         while (*pt1 != '&' && *pt1 != '?')
            pt1--;
         pt1++;
         strcpy(param, pt1);
         param[pt2 - pt1] = 0;
         strcpy(pt1, pt2 + 2);

         /* remove param from lastcmd if present */
         if ((pt1 = strstr(_cmdline, "lastcmd=")) != NULL) {
            sprintf(str, "%s%%3D", param);
            if ((pt1 = strstr(_cmdline, str)) != NULL) {
               pt2 = pt1 + strlen(str);
               while (*pt2 && *pt2 != '%')
                  pt2++;
               if (*pt2 == '%')
                  pt2 += 3;
               strcpy(pt1, pt2);
            }
         }
      }
      if (_cmdline[strlen(_cmdline) - 1] == '=') {
         pt1 = _cmdline + strlen(_cmdline) - 1;
         while (*pt1 != '&' && *pt1 != '?')
            pt1--;
         pt1++;
         strcpy(param, pt1);
         if (param[strlen(param) - 1] == '=')
            param[strlen(param) - 1] = 0;
         *pt1 = 0;

         /* remove param from lastcmd if present */
         if ((pt1 = strstr(_cmdline, "lastcmd=")) != NULL) {
            sprintf(str, "%s%%3D", param);
            if ((pt1 = strstr(_cmdline, str)) != NULL) {
               pt2 = pt1 + strlen(str);
               while (*pt2 && *pt2 != '%' && *pt2 != '&')
                  pt2++;
               if (*pt2 == '%')
                  pt2 += 3;
               strcpy(pt1, pt2);
            }
         }
      }
      if (_cmdline[strlen(_cmdline) - 1] == '&')
         _cmdline[strlen(_cmdline) - 1] = 0;
      redirect(lbs, _cmdline);
      return;
   }

   /* redirect "go" command */
   if (isparam("lastcmd")) {

      strlcpy(str, getparam("lastcmd"), sizeof(str));
      url_decode(str);

      /* subsitute "last" in command line from new parameter */
      if (isparam("last")) {
         if (strieq(getparam("last"), "_all_"))
            subst_param(str, sizeof(str), "last", "");
         else
            subst_param(str, sizeof(str), "last", getparam("last"));
      }

      /* subsitute attributes in command line from new parameter */
      for (i = 0; i < MAX_N_ATTR; i++)
         if (isparam(attr_list[i])) {
            if (strieq(getparam(attr_list[i]), "_all_"))
               subst_param(str, sizeof(str), attr_list[i], "");
            else
               subst_param(str, sizeof(str), attr_list[i], getparam(attr_list[i]));
         }

      /* do the same for subtext */
      if (isparam("subtext"))
         subst_param(str, sizeof(str), "subtext", getparam("subtext"));

      redirect(lbs, str);
      return;
   }

   /* remove remaining "_all_" in parameters */
   if (isparam("last") && strieq(getparam("last"), "_all_")) {
      strlcpy(str, _cmdline, sizeof(str));
      subst_param(str, sizeof(str), "last", "");
      redirect(lbs, str);
      return;
   }

   /* remove remaining "_all_" or empty or "--+<attrib>+--" parameters */
   strlcpy(str, _cmdline, sizeof(str));
   found = 0;
   for (i = 0; i < MAX_N_ATTR; i++) {
      if (isparam(attr_list[i])) {
         if (strieq(getparam(attr_list[i]), "_all_")) {
            subst_param(str, sizeof(str), attr_list[i], "");
            found = 1;
         }
         if (*getparam(attr_list[i]) == 0) {
            subst_param(str, sizeof(str), attr_list[i], "");
            found = 1;
         }
         sprintf(ref, "-- %s --", attr_list[i]);
         if (strieq(getparam(attr_list[i]), ref)) {
            subst_param(str, sizeof(str), attr_list[i], "");
            found = 1;
         }
      }
   }

   if (isparam("subtext")) {
      if (*getparam("subtext") == 0) {
         subst_param(str, sizeof(str), "subtext", "");
         found = 1;
      }
      sprintf(ref, "-- %s --", loc("Text"));
      if (strieq(getparam("subtext"), ref)) {
         subst_param(str, sizeof(str), "subtext", "");
         found = 1;
      }
   }

   if (found) {
      redirect(lbs, str);
      return;
   }

   /* check for invalid attributes in config file */
   for (i = 0; i < MAX_N_ATTR; i++) {
      if (strieq(attr_list[i], "Date")) {
         show_error("Attribute name \"Date\" is not allowed in config file");
         return;
      }
      if (strieq(attr_list[i], "ID")) {
         show_error("Attribute name \"Date\" is not allowed in config file");
         return;
      }
   }

   slist = xmalloc((MAX_N_ATTR + 10) * NAME_LENGTH);
   svalue = xmalloc((MAX_N_ATTR + 10) * NAME_LENGTH);
   gattr = xmalloc(MAX_N_ATTR * NAME_LENGTH);
   list = xmalloc(10000);

   printable = isparam("Printable") ? atoi(getparam("Printable")) : 0;

   /* in printable mode, display all pages */
   if (printable)
      page_n = -1;

   if (isparam("Reverse"))
      reverse = atoi(getparam("Reverse"));
   else {
      reverse = 0;
      if (getcfg(lbs->name, "Reverse sort", str, sizeof(str)))
         reverse = atoi(str);
   }

   /* get message ID from "list" command */
   if (isparam("id"))
      page_mid = atoi(getparam("id"));
   else
      page_mid = 0;
   page_mid_head = 0;

   /* default mode */
   strlcpy(mode, "Summary", sizeof(mode));
   show_attachments = FALSE;

   if (past_n || last_n || page_n || page_mid || default_page) {
      /* for page display, get mode from config file */
      if (getcfg(lbs->name, "Display Mode", str, sizeof(str)))
         strlcpy(mode, str, sizeof(mode));

      /* supersede mode from cookie */
      if (isparam("elmode"))
         strlcpy(mode, getparam("elmode"), sizeof(mode));

      /* supersede mode from direct parameter */
      if (isparam("mode"))
         strlcpy(mode, getparam("mode"), sizeof(mode));

   } else {
      /* for find result, get mode from find form */
      if (isparam("mode"))
         strlcpy(mode, getparam("mode"), sizeof(mode));
      else
         strlcpy(mode, "Full", sizeof(mode));
   }

   /* set cookie if mode changed */
   mode_cookie[0] = 0;
   if (strieq(mode, "Summary") || strieq(mode, "Full") || strieq(mode, "Threaded")) {
      if (!isparam("elmode") || !strieq(getparam("elmode"), mode))
         sprintf(mode_cookie, "elmode=%s", mode);
   }

   threaded = strieq(mode, "threaded");
   csv = strieq(mode, "CSV1") || strieq(mode, "CSV2");
   xml = strieq(mode, "XML");
   raw = strieq(mode, "Raw");

   if (csv || xml || raw) {
      page_n = -1;              /* display all pages */
      show_attachments = FALSE; /* hide attachments */
   }

   /* show attachments in full mode by default */
   if (strieq(mode, "Full"))
      show_attachments = TRUE;

   /* supersede attachment mode if in cookie */
   if (isparam("elattach"))
      show_attachments = atoi(getparam("elattach"));

   /* supersede attachment mode if in parameter */
   if (isparam("attach"))
      show_attachments = atoi(getparam("attach"));

   /* set cookie if attachment mode changed in full view */
   if (mode_cookie[0] == 0 && strieq(mode, "Full")) {
      if (!isparam("elattach") || atoi(getparam("elattach")) != show_attachments)
         sprintf(mode_cookie, "elattach=%d", show_attachments);
   }

   /*---- convert dates to ltime ----*/

   time(&now);
   ptms = localtime(&now);
   assert(ptms);
   current_year = ptms->tm_year + 1900;
   current_month = ptms->tm_mon + 1;
   current_day = ptms->tm_mday;

   ltime_end = ltime_start = 0;

   d1 = m1 = y1 = d2 = m2 = y2 = 0;

   if (!past_n && !last_n) {

      ltime_start = retrieve_date("a", TRUE);
      if (ltime_start < 0) {
         xfree(slist);
         xfree(svalue);
         xfree(gattr);
         xfree(list);
         return;
      }

      if (ltime_start) {
         memcpy(&tms, localtime(&ltime_start), sizeof(struct tm));
         y1 = tms.tm_year + 1900;
         m1 = tms.tm_mon + 1;
         d1 = tms.tm_mday;
      }

      ltime_end = retrieve_date("b", FALSE);
      if (ltime_end < 0) {
         xfree(slist);
         xfree(svalue);
         xfree(gattr);
         xfree(list);
         return;
      }

      if (ltime_end) {

         if (ltime_end <= ltime_start) {
            sprintf(str, "Error: Start date after end date");
            show_error(str);
            xfree(slist);
            xfree(svalue);
            xfree(gattr);
            xfree(list);
            return;
         }

         memcpy(&tms, localtime(&ltime_end), sizeof(struct tm));
         y2 = tms.tm_year + 1900;
         m2 = tms.tm_mon + 1;
         d2 = tms.tm_mday;
      }
   }

   if (ltime_start && ltime_end && ltime_start > ltime_end) {
      show_error(loc("Error: start date after end date"));
      xfree(slist);
      xfree(svalue);
      xfree(gattr);
      xfree(list);
      return;
   }

   /*---- apply last login cut ----*/

   if (isparam("new_entries") && atoi(getparam("new_entries")) == 1 && isparam("unm"))
      get_user_line(lbs, getparam("unm"), NULL, NULL, NULL, NULL, &ltime_start);

   /*---- assemble message list ----*/

   /* check for search all */
   search_all = isparam("all") ? atoi(getparam("all")) : 0;

   if (getcfg(lbs->name, "Search all logbooks", str, sizeof(str)) && atoi(str) == 0)
      search_all = 0;

   n_msg = 0;
   n_display = 0;
   if (search_all) {
      /* count logbooks */
      for (n_logbook = 0;; n_logbook++) {
         if (!lb_list[n_logbook].name[0])
            break;

         if (lbs->top_group[0] && !strieq(lbs->top_group, lb_list[n_logbook].top_group))
            continue;

         if (isparam("unm") && !check_login_user(&lb_list[n_logbook], getparam("unm")))
            continue;

         n_msg += *lb_list[n_logbook].n_el_index;
      }
   } else {
      n_logbook = 1;
      n_msg = *lbs->n_el_index;
   }

   msg_list = xmalloc(sizeof(MSG_LIST) * n_msg);

   lbs_cur = lbs;
   numeric = TRUE;
   for (i = n = 0; i < n_logbook; i++) {
      if (search_all)
         lbs_cur = &lb_list[i];

      if (lbs->top_group[0] && !strieq(lbs->top_group, lbs_cur->top_group))
         continue;

      if (isparam("unm") && !check_login_user(lbs_cur, getparam("unm")))
         continue;

      for (j = 0; j < *lbs_cur->n_el_index; j++) {
         msg_list[n].lbs = lbs_cur;
         msg_list[n].index = j;
         msg_list[n].number = (int) lbs_cur->el_index[j].file_time;
         msg_list[n].in_reply_to = lbs_cur->el_index[j].in_reply_to;
         n++;
      }
   }

   /*---- apply start/end date cut ----*/

   if (past_n > 0)
      ltime_start = now - 3600 * 24 * past_n; // past n days
   else if (past_n < 0)
      ltime_start = now + 3600 * past_n;      // past n hours

   if (last_n && last_n < n_msg) {
      for (i = n_msg - last_n - 1; i >= 0; i--)
         msg_list[i].lbs = NULL;
   }

   if (ltime_start) {
      for (i = 0; i < n_msg; i++)
         if (msg_list[i].lbs && msg_list[i].lbs->el_index[msg_list[i].index].file_time < ltime_start)
            msg_list[i].lbs = NULL;
   }

   if (ltime_end) {
      for (i = 0; i < n_msg; i++)
         if (msg_list[i].lbs && msg_list[i].lbs->el_index[msg_list[i].index].file_time > ltime_end)
            msg_list[i].lbs = NULL;
   }

   if (isparam("last")) {
      n = atoi(getparam("last"));

      if (n > 0) {
         for (i = 0; i < n_msg; i++)
            if (msg_list[i].lbs && msg_list[i].lbs->el_index[msg_list[i].index].file_time < now - 3600 * 24
                * n)
               msg_list[i].lbs = NULL;
      }
   }

   /*---- filter message list ----*/

   filtering = FALSE;
   show_text = TRUE;
   searched = found = FALSE;

   for (i = 0; i < lbs->n_attr; i++) {
      /* check if attribute filter */
      if (isparam(attr_list[i]))
         break;

      if (attr_flags[i] & (AF_DATE | AF_DATETIME)) {
         sprintf(str, "%da", i);
         if (retrieve_date(str, TRUE))
            break;
         sprintf(str, "%db", i);
         if (retrieve_date(str, TRUE))
            break;
      }

      if (attr_flags[i] & AF_MULTI) {
         for (j = 0; j < MAX_N_LIST && attr_options[i][j][0]; j++) {
            sprintf(str, "%s_%d", attr_list[i], j);
            if (isparam(str)) {
               filtering = TRUE;
               break;
            }
         }
      }

      if (attr_flags[i] & (AF_MUSERLIST | AF_MUSEREMAIL)) {
         for (j = 0; j < MAX_N_LIST; j++) {
            sprintf(str, "%s_%d", attr_list[i], j);
            if (isparam(str)) {
               filtering = TRUE;
               break;
            }
         }
      }

      /* check if sort by attribute */
      if ((isparam("sort") && strieq(getparam("sort"), attr_list[i]))
          || (isparam("rsort") && strieq(getparam("rsort"), attr_list[i])))
         break;
   }

   /* turn on filtering if found */
   if (i < lbs->n_attr)
      filtering = TRUE;

   if (isparam("subtext"))
      filtering = TRUE;

   if (getcfg(lbs->name, "Sort Attributes", list, 10000))
      filtering = TRUE;

   text = xmalloc(TEXT_SIZE);
   text1 = xmalloc(TEXT_SIZE);

   /* prepare for regex search */
   memset(re_buf, 0, sizeof(re_buf));

   /* compile regex for subtext */
   if (isparam("subtext")) {
      strlcpy(str, getparam("subtext"), sizeof(str));
      flags = REG_EXTENDED;
      if (!isparam("casesensitive"))
         flags |= REG_ICASE;
      status = regcomp(re_buf, str, flags);
      if (status) {
         sprintf(line, loc("Error in regular expression \"%s\""), str);
         strlcat(line, ": ", sizeof(line));
         regerror(status, re_buf, str, sizeof(str));
         strlcat(line, str, sizeof(line));
         strencode2(str, line, sizeof(str));
         show_error(str);
         return;
      }
   }

   /* compile regex for attributes */
   for (i = 0; i < lbs->n_attr; i++) {
      if (isparam(attr_list[i])) {
         strlcpy(str, getparam(attr_list[i]), sizeof(str));

         /* if value starts with '$', substitute it */
         if (str[0] == '$') {
            j = build_subst_list(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, attrib,
                                 TRUE);
            add_subst_time(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, "entry time",
                           date, &j, 0);

            strsubst_list(str, sizeof(str), (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, j);
            setparam(attr_list[i], str);
         }

         flags = REG_EXTENDED;

         if (!isparam("casesensitive"))
            flags |= REG_ICASE;

         status = regcomp(re_buf + i + 1, str, flags);
         if (status) {
            sprintf(line, loc("Error in regular expression \"%s\""), str);
            strlcat(line, ": ", sizeof(line));
            regerror(status, re_buf + i + 1, str, sizeof(str));
            strlcat(line, str, sizeof(line));
            strencode2(str, line, sizeof(str));
            show_error(str);
            return;
         }
      }
   }

   sort_item[0] = 0;
   if (isparam("sort"))
      strlcpy(sort_item, getparam("sort"), sizeof(sort_item));
   if (isparam("rsort"))
      strlcpy(sort_item, getparam("rsort"), sizeof(sort_item));

   sort_attributes = getcfg(lbs->name, "Sort Attributes", str, sizeof(str));

   /* do filtering */
   for (index = 0; index < n_msg; index++) {
      if (!msg_list[index].lbs)
         continue;

      /* retrieve message */
      size = TEXT_SIZE;
      message_id = msg_list[index].lbs->el_index[msg_list[index].index].message_id;

      if (filtering) {
         status = el_retrieve(msg_list[index].lbs, message_id, date, attr_list, attrib, lbs->n_attr, text,
                              &size, in_reply_to, reply_to, attachment, encoding, locked_by);
         if (status != EL_SUCCESS)
            break;

         /* apply filter for attributes */
         for (i = 0; i < lbs->n_attr; i++) {

            /* replace icon name with their comments if present */
            if (attr_flags[i] & AF_ICON) {
               sprintf(str, "Icon comment %s", attrib[i]);
               if (getcfg(lbs->name, str, comment, sizeof(comment)))
                  strlcpy(attrib[i], comment, NAME_LENGTH);
            }

            /* check for multi attributes */
            if (attr_flags[i] & AF_MULTI) {

               /* OR of any of the values */
               searched = found = FALSE;
               for (j = 0; j < MAX_N_LIST && attr_options[i][j][0]; j++) {
                  sprintf(str, "%s_%d", attr_list[i], j);
                  if (isparam(str)) {
                     searched = TRUE;
                     if (strstr(attrib[i], getparam(str))) {
                        found = TRUE;
                        break;
                     }
                  }
               }

               /* search for parameter without '_' coming from quick filter */
               if (isparam(attr_list[i])) {
                  searched = TRUE;

                  strlcpy(str, getparam(attr_list[i]), sizeof(str));
                  if (str[0] == '^' && str[strlen(str)-1] == '$') {
                     str[strlen(str)-1] = 0;
                     strlcpy(comment, str+1, NAME_LENGTH);
                  } else
                     strlcpy(comment, str, NAME_LENGTH);
                  strlcpy(str, comment, sizeof(str));

                  if (strstr(attrib[i], str))
                     found = TRUE;
               }

               if (searched && !found)
                  break;
            }

            /* check for multi user list or multi user email */
            else if (attr_flags[i] & (AF_MUSERLIST | AF_MUSEREMAIL)) {

               /* OR of any of the values */
               searched = found = FALSE;
               for (j = 0; j < MAX_N_LIST; j++) {
                  sprintf(str, "%s_%d", attr_list[i], j);
                  if (isparam(str)) {
                     searched = TRUE;
                     if (strstr(attrib[i], getparam(str))) {
                        found = TRUE;
                        break;
                     }
                  }
               }

               /* search for parameter without '_' coming from quick filter */
               if (isparam(attr_list[i])) {
                  searched = TRUE;
                  if (strstr(attrib[i], getparam(attr_list[i])))
                     found = TRUE;
               }

               if (searched && !found)
                  break;

            } else if (attr_flags[i] & (AF_DATE | AF_DATETIME)) {

               /* check for last[i]/next[i] */

               ltime = isparam(attr_list[i]) ? atoi(getparam(attr_list[i])) : 0;

               /* today 12h noon */
               time(&now);
               memcpy(&tms, localtime(&now), sizeof(struct tm));
               tms.tm_hour = 12;
               tms.tm_min = 0;
               tms.tm_sec = 0;
               now = mktime(&tms);

               /* negative i: last [i] days */
               if (ltime < 0)
                  if (atoi(attrib[i]) < now + ltime * 3600 * 24 - 3600 * 12 || atoi(attrib[i]) > now)
                     break;

               /* positive i: next [i] days */
               if (ltime > 0)
                  if (atoi(attrib[i]) > now + ltime * 3600 * 24 + 3600 * 12 || atoi(attrib[i]) < now)
                     break;

               /* check for start date / end date */
               sprintf(str, "%da", i);
               ltime = retrieve_date(str, TRUE);
               if (ltime > 0 && atoi(attrib[i]) < ltime)
                  break;

               sprintf(str, "%db", i);
               ltime = retrieve_date(str, FALSE);
               if (ltime > 0 && (atoi(attrib[i]) > ltime || atoi(attrib[i]) == 0))
                  break;

            } else {

               strlcpy(str, isparam(attr_list[i]) ? getparam(attr_list[i]) : "", sizeof(str));

               /* if value starts with '$', substitute it */
               if (str[0] == '$') {
                  j = build_subst_list(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue,
                                       attrib, TRUE);
                  sprintf(mid, "%d", message_id);
                  add_subst_list((char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, "message id",
                                 mid, &j);
                  add_subst_time(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue,
                                 "entry time", date, &j, 0);

                  strsubst_list(str, sizeof(str), (char (*)[NAME_LENGTH]) slist,
                                (char (*)[NAME_LENGTH]) svalue, j);
                  setparam(attr_list[i], str);
               }

               status = regexec(re_buf + 1 + i, attrib[i], 10, pmatch, 0);
               if (status == REG_NOMATCH)
                  break;
            }
         }

         if (i < lbs->n_attr) {
            msg_list[index].lbs = NULL;
            continue;
         }

         if (isparam("subtext")) {

            status = regexec(re_buf, text, 10, pmatch, 0);
            if (isparam("sall") && atoi(getparam("sall")) && status == REG_NOMATCH) {

               // search text in attributes
               for (i = 0; i < lbs->n_attr; i++) {
                  status = regexec(re_buf, attrib[i], 10, pmatch, 0);
                  if (status != REG_NOMATCH)
                     break;
               }

               if (i == lbs->n_attr) {
                  msg_list[index].lbs = NULL;
                  continue;
               }
            } else if (status == REG_NOMATCH) {
               msg_list[index].lbs = NULL;
               continue;
            }
         }
      }                         // if (filtering)

      /* evaluate "sort attributes" */
      if (sort_attributes) {
         getcfg(lbs->name, "Sort Attributes", list, 10000);
         msg_list[index].string[0] = 0;
         n = strbreak(list, sort_attr, MAX_N_ATTR, ",", FALSE);
         for (i = 0; i < n; i++) {
            for (j = 0; j < lbs->n_attr; j++) {
               if (strieq(sort_attr[i], attr_list[j])) {
                  strlcat(msg_list[index].string, " ", sizeof(msg_list[index].string));
                  strlcat(msg_list[index].string, attrib[j], sizeof(msg_list[index].string));
                  if (attr_flags[i] & AF_NUMERIC) {
                     msg_list[index].number = atoi(attrib[j]);
                     numeric = TRUE;
                  } else
                     numeric = FALSE;
                  break;
               }
            }
            if (strieq(sort_attr[i], loc("ID"))) {
               strlcat(msg_list[index].string, " ", sizeof(msg_list[index].string));
               sprintf(str, "%08d", message_id);
               strlcat(msg_list[index].string, str, sizeof(msg_list[index].string));
            } else if (strieq(sort_attr[i], loc("Logbook"))) {
               strlcpy(msg_list[index].string, msg_list[index].lbs->name, 256);
            }
         }
      }

      /* add attribute for sorting */
      if (sort_item[0]) {
         for (i = 0; i < lbs->n_attr; i++) {
            if (strieq(sort_item, attr_list[i])) {
               if (attr_flags[i] & AF_NUMERIC) {
                  numeric = TRUE;
                  msg_list[index].number = atoi(attrib[i]);
               } else {
                  numeric = FALSE;
                  strlcpy(msg_list[index].string, attrib[i], 256);
               }
            }

            if (strieq(sort_item, loc("ID"))) {
               numeric = TRUE;
               msg_list[index].number = message_id;
            }

            if (strieq(sort_item, loc("Logbook")))
               strlcpy(msg_list[index].string, msg_list[index].lbs->name, 256);
         }

         if (isparam("rsort"))
            reverse = 1;

         if (isparam("sort"))
            reverse = 0;
      }
   }

   /*---- in threaded mode, set date of latest entry of thread ----*/

   if (threaded) {
      for (index = 0; index < n_msg; index++) {
         if (!msg_list[index].lbs)
            continue;

         message_id = msg_list[index].lbs->el_index[msg_list[index].index].message_id;
         in_reply_to_id = msg_list[index].lbs->el_index[msg_list[index].index].in_reply_to;
         if (!in_reply_to_id)
            continue;

         do {
            message_id = in_reply_to_id;

            /* search index of message */
            for (i = 0; i < *msg_list[index].lbs->n_el_index; i++)
               if (msg_list[index].lbs->el_index[i].message_id == message_id)
                  break;

            /* stop if not found */
            if (i == *msg_list[index].lbs->n_el_index)
               break;

            in_reply_to_id = msg_list[index].lbs->el_index[i].in_reply_to;

         } while (in_reply_to_id);

         /* if head not found, skip message */
         if (i == *msg_list[index].lbs->n_el_index) {
            msg_list[index].lbs = NULL;
            continue;
         }

         /* set new page message ID with head message */
         if (page_mid && msg_list[index].lbs->el_index[msg_list[index].index].message_id == page_mid)
            page_mid_head = message_id;

         /* search message head in list */
         for (j = 0; j < n_msg; j++)
            if (msg_list[j].lbs == msg_list[index].lbs && msg_list[j].index == i)
               break;

         if (j < index) {
            /* set date from current message, if later */
            if (msg_list[j].number < msg_list[index].number)
               msg_list[j].number = msg_list[index].number;
         }

         /* now delete current message, to leave only heads in list */
         msg_list[index].lbs = NULL;
      }
   }

   /*---- compact messasges ----*/

   total_n_msg = n_msg;

   for (i = j = 0; i < n_msg; i++)
      if (msg_list[i].lbs)
         memcpy(&msg_list[j++], &msg_list[i], sizeof(MSG_LIST));
   n_msg = j;

   /*---- sort messasges ----*/

   if (numeric)
      qsort(msg_list, n_msg, sizeof(MSG_LIST), reverse ? msg_compare_reverse_numeric : msg_compare_numeric);
   else
      qsort(msg_list, n_msg, sizeof(MSG_LIST), reverse ? msg_compare_reverse : msg_compare);

   /*---- search page for specific message ----*/

   if (getcfg(lbs->name, "Entries per page", str, sizeof(str)))
      n_page = atoi(str);
   else
      n_page = 20;
   if (isparam("npp"))
      n_page = atoi(getparam("npp"));

   if (page_mid) {
      default_page = 0;

      for (i = 0; i < n_msg; i++)
         if (msg_list[i].lbs->el_index[msg_list[i].index].message_id == page_mid
             || msg_list[i].lbs->el_index[msg_list[i].index].message_id == page_mid_head)
            break;

      if (i < n_msg)
         page_n = i / n_page + 1;
   }

   /*---- number of messages per page ----*/

   n_attr_disp = n_line = 0;
   i_start = 0;
   i_stop = n_msg - 1;
   if (!csv && !xml && !raw) {
      if (page_n || default_page) {
         if (default_page && page_n != -1)
            page_n = reverse ? 1 : (n_msg - 1) / n_page + 1;

         if (page_n != -1) {
            i_start = (page_n - 1) * n_page;
            i_stop = i_start + n_page - 1;

            if (i_start >= n_msg && n_msg > 0) {
               page_n = 1;
               i_start = 0;
            }

            if (i_stop >= n_msg)
               i_stop = n_msg - 1;
         }
      }
   }

   /*---- header ----*/

   if (getcfg(lbs->name, "List Page Title", str, sizeof(str))) {
      i = build_subst_list(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, NULL, TRUE);
      strsubst_list(str, sizeof(str), (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, i);
      strip_html(str);
   } else
      sprintf(str, "ELOG %s", lbs->name);

   if (csv) {

      /* no menus and tables */
      show_plain_header(0, "export.csv");

      rsprintf("\"%s\"", loc("Message ID"));
      if (strieq(mode, "CSV1"))
         rsprintf(",");
      else
         rsprintf(";");

      rsprintf("\"%s\"", loc("Date"));
      if (strieq(mode, "CSV1"))
         rsprintf(",");
      else
         rsprintf(";");

      for (i = 0; i < lbs->n_attr; i++) {
         strlcpy(str, attr_list[i], sizeof(str));
         if (str[0]) {
            rsputs("\"");
            pt1 = str;
            while ((pt2 = strchr(pt1, '"')) != NULL) {
               *pt2 = 0;
               rsputs(pt1);
               rsputs("\"\"");
               pt1 = pt2 + 1;
            }
            rsputs(pt1);
            rsputs("\"");
         }
         if (i < lbs->n_attr - 1) {
            if (strieq(mode, "CSV1"))
               rsprintf(",");
            else
               rsprintf(";");
         } else
            rsprintf("\r\n");
      }

   } else if (xml) {

      /* no menus and tables */
      show_plain_header(0, "export.xml");
      if (!getcfg("global", "charset", charset, sizeof(charset)))
         strcpy(charset, DEFAULT_HTTP_CHARSET);
      rsprintf("<?xml version=\"1.0\" encoding=\"%s\"?>\n", charset);
      rsprintf("<!-- ELOGD Version %s export.xml -->\n", VERSION);
      rsprintf("<ELOG_LIST>\n");

   } else if (raw) {

      /* no menus and tables */
      show_plain_header(0, "export.txt");

   } else {

      show_standard_header(lbs, TRUE, str, NULL, TRUE, mode_cookie, NULL);

      /*---- title ----*/

      strlcpy(str, ", ", sizeof(str));
      if (past_n == 1)
         strcat(str, loc("Last day"));
      else if (past_n > 1)
         sprintf(str + strlen(str), loc("Last %d days"), past_n);
      else if (past_n < 0)
         sprintf(str + strlen(str), loc("Last %d hours"), -past_n);
      else if (last_n)
         sprintf(str + strlen(str), loc("Last %d entries"), last_n);
      else if (page_n == -1)
         sprintf(str + strlen(str), loc("all entries"));
      else if (page_n)
         sprintf(str + strlen(str), loc("Page %d of %d"), page_n, (n_msg - 1) / n_page + 1);
      if (strlen(str) == 2)
         str[0] = 0;

      if (printable)
         show_standard_title(lbs->name, str, 1);
      else
         show_standard_title(lbs->name, str, 0);

      /*---- menu buttons ----*/

      if (!printable) {
         rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");

         /* current command line for select command */
         strlcpy(str, isparam("cmdline") ? getparam("cmdline") : "", sizeof(str));

         /* remove select switch */
         if (strstr(str, "select=1")) {
            *strstr(str, "select=1") = 0;
            if (strlen(str) > 1 && (str[strlen(str) - 1] == '&' || str[strlen(str) - 1] == '?'))
               str[strlen(str) - 1] = 0;
         }

         /* store current command line as hidden parameter for page navigation */
         if (str[0] && !strieq(str, "?")) {
            rsprintf("<input type=hidden name=lastcmd value=\"");
            rsputs3(str);
            rsprintf("\">\n", str);
         }

         if (!getcfg(lbs->name, "Guest Find menu commands", menu_str, sizeof(menu_str)) || isparam("unm")
             != 0)
            getcfg(lbs->name, "Find menu commands", menu_str, sizeof(menu_str));

         if (!menu_str[0]) {
            if (!getcfg(lbs->name, "Guest list menu commands", menu_str, sizeof(menu_str)) || isparam("unm")
                != 0)
               getcfg(lbs->name, "list menu commands", menu_str, sizeof(menu_str));
         }

         /* default menu commands */
         if (menu_str[0] == 0) {
            strlcpy(menu_str, "New, Find, Select, Import, ", sizeof(menu_str));

            if (getcfg(lbs->name, "Password file", str, sizeof(str)))
               strlcat(menu_str, "Config, Logout, ", sizeof(menu_str));
            else
               strlcat(menu_str, "Config, ", sizeof(menu_str));

            if (getcfg(lbs->name, "Mirror server", str, sizeof(str)))
               strlcat(menu_str, "Synchronize, ", sizeof(menu_str));

            strlcpy(str, loc("Last x"), sizeof(str));
            strlcat(menu_str, "Last x, Help, ", sizeof(menu_str));
         }

         n = strbreak(menu_str, menu_item, MAX_N_LIST, ",", FALSE);

         for (i = 0; i < n; i++) {
            if (is_user_allowed(lbs, menu_item[i])) {
               if (strieq(menu_item[i], "Last x")) {
                  if (past_n > 0) {
                     sprintf(str, loc("Last %d days"), past_n * 2);
                     rsprintf("&nbsp;<a href=\"past%d?mode=%s\">%s</a>&nbsp;|\n", past_n * 2, mode, str);
                  }

                  if (last_n) {
                     sprintf(str, loc("Last %d entries"), last_n * 2);
                     rsprintf("&nbsp;<a href=\"last%d?mode=%s\">%s</a>&nbsp;|\n", last_n * 2, mode, str);
                  }
               } else if (strieq(menu_item[i], "Select")) {
                  strlcpy(str, getparam("cmdline"), sizeof(str));
                  if (isparam("select") && atoi(getparam("select")) == 1) {
                     /* remove select switch */
                     if (strstr(str, "select=1")) {
                        *strstr(str, "select=1") = 0;
                        if (strlen(str) > 1 && (str[strlen(str) - 1] == '&' || str[strlen(str) - 1] == '?'))
                           str[strlen(str) - 1] = 0;
                     }
                  } else {
                     /* add select switch */
                     if (strchr(str, '?'))
                        strcat(str, "&select=1");
                     else
                        strcat(str, "?select=1");
                  }
                  rsprintf("&nbsp;<a href=\"");
                  rsputs3(str);
                  rsprintf("\">%s</a>&nbsp;|\n", loc("Select"));
               } else {
                  strlcpy(str, loc(menu_item[i]), sizeof(str));
                  url_encode(str, sizeof(str));

                  if (i < n - 1)
                     rsprintf("&nbsp;<a href=\"?cmd=%s\">%s</a>&nbsp;|\n", str, loc(menu_item[i]));
                  else
                     rsprintf("&nbsp;<a href=\"?cmd=%s\">%s</a>&nbsp;\n", str, loc(menu_item[i]));
               }
            }
         }

         rsprintf("</span></td></tr>\n\n");
      }

      /*---- list menu text ----*/

      if ((getcfg(lbs->name, "find menu text", str, sizeof(str)) || getcfg(lbs->name, "list menu text", str,
                                                                           sizeof(str))) && !printable) {
         FILE *f;
         char file_name[256], *buf;

         rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");

         /* check if file starts with an absolute directory */
         if (str[0] == DIR_SEPARATOR || str[1] == ':')
            strlcpy(file_name, str, sizeof(file_name));
         else {
            strlcpy(file_name, logbook_dir, sizeof(file_name));
            strlcat(file_name, str, sizeof(file_name));
         }

         f = fopen(file_name, "rb");
         if (f != NULL) {
            fseek(f, 0, SEEK_END);
            size = TELL(fileno(f));
            fseek(f, 0, SEEK_SET);

            buf = xmalloc(size + 1);
            fread(buf, 1, size, f);
            buf[size] = 0;
            fclose(f);

            rsputs(buf);

         } else
            rsprintf("<center><b>Error: file <i>\"%s\"</i> not found</b></center>", file_name);

         rsprintf("</span></td></tr>");
      }

      /*---- display filters ----*/

      disp_filter = isparam("ma") || isparam("ya") || isparam("da") || isparam("mb") || isparam("yb")
          || isparam("db") || isparam("subtext");

      for (i = 0; i < lbs->n_attr; i++)
         if (isparam(attr_list[i]) && (attr_flags[i] & (AF_DATE | AF_DATETIME)) == 0)
            disp_filter = TRUE;

      for (i = 0; i < lbs->n_attr; i++) {
         if (attr_flags[i] & (AF_DATE | AF_DATETIME)) {
            sprintf(str, "%da", i);
            ltime = retrieve_date(str, TRUE);
            if (ltime > 0)
               disp_filter = TRUE;
            sprintf(str, "%db", i);
            ltime = retrieve_date(str, FALSE);
            if (ltime > 0)
               disp_filter = TRUE;
         }
         if (attr_flags[i] & (AF_MULTI | AF_MUSERLIST | AF_MUSEREMAIL)) {
            for (j = 0; j < MAX_N_LIST; j++) {
               sprintf(str, "%s_%d", attr_list[i], j);
               if (isparam(str))
                  disp_filter = TRUE;
               if (isparam(attr_list[i]))
                  disp_filter = TRUE;
            }
         }
      }

      if (isparam("new_entries") && atoi(getparam("new_entries")) == 1) {
         rsprintf("<tr><td class=\"listframe\">\n");
         rsprintf("<table width=\"100%%\" border=\"0\" cellpadding=\"0\" cellspacing=\"0\">\n");
         rsprintf("<tr><td nowrap width=\"10%%\" class=\"attribname\">%s:</td>", loc("New entries since"));
         memcpy(&tms, localtime(&ltime_start), sizeof(struct tm));
         my_strftime(str, sizeof(str), "%c", &tms);
         rsprintf("<td class=\"attribvalue\">%s</td></tr>", str);
         rsprintf("</table></td></tr>\n\n");
      }

      if (disp_filter) {
         rsprintf("<tr><td class=\"listframe\">\n");
         rsprintf("<table width=\"100%%\" border=\"0\" cellpadding=\"0\" cellspacing=\"0\">\n");

         if (isparam("ma") || isparam("ya") || isparam("da")) {
            memset(&tms, 0, sizeof(struct tm));
            tms.tm_year = y1 - 1900;
            tms.tm_mon = m1 - 1;
            tms.tm_mday = d1;
            tms.tm_hour = 12;
            if (tms.tm_year < 90)
               tms.tm_year += 100;
            mktime(&tms);
            strcpy(format, "%x");
            strftime(str, sizeof(str), format, &tms);

            rsprintf("<tr><td nowrap width=\"10%%\" class=\"attribname\">%s:</td>", loc("Start date"));
            rsprintf("<td class=\"attribvalue\">%s</td></tr>", str);
         }

         if (isparam("mb") || isparam("yb") || isparam("db")) {
            /* calculate previous day */
            memset(&tms, 0, sizeof(struct tm));
            tms.tm_year = y2 - 1900;
            tms.tm_mon = m2 - 1;
            tms.tm_mday = d2;
            tms.tm_hour = 12;
            if (tms.tm_year < 90)
               tms.tm_year += 100;
            ltime = mktime(&tms);
            ltime -= 3600 * 24;
            memcpy(&tms, localtime(&ltime), sizeof(struct tm));
            strcpy(format, "%x");
            strftime(str, sizeof(str), format, &tms);

            rsprintf("<tr><td nowrap width=\"10%%\" class=\"attribname\">%s:</td>", loc("End date"));

            rsprintf("<td class=\"attribvalue\">%s</td></tr>", str);
         }

         for (i = 0; i < lbs->n_attr; i++) {
            if (attr_flags[i] & (AF_DATE | AF_DATETIME)) {

               sprintf(str, "%da", i);
               ltime1 = retrieve_date(str, TRUE);
               sprintf(str, "%db", i);
               ltime2 = retrieve_date(str, TRUE);

               if (ltime1 > 0 || ltime2 > 0) {
                  rsprintf("<tr><td nowrap width=\"10%%\" class=\"attribname\">%s:</td>", attr_list[i]);
                  rsprintf("<td class=\"attribvalue\">");
                  if (ltime1) {
                     memcpy(&tms, localtime(&ltime1), sizeof(struct tm));
                     if (attr_flags[i] & AF_DATE)
                        strcpy(format, DEFAULT_DATE_FORMAT);
                     else
                        strcpy(format, DEFAULT_TIME_FORMAT);
                     my_strftime(str, sizeof(str), format, &tms);
                     if (ltime2 > 0)
                        rsprintf("%s %s", loc("From"), str);
                     else
                        rsprintf("%s %s", loc("After"), str);
                  }
                  if (ltime2) {
                     memcpy(&tms, localtime(&ltime2), sizeof(struct tm));
                     if (attr_flags[i] & AF_DATE)
                        strcpy(format, DEFAULT_DATE_FORMAT);
                     else
                        strcpy(format, DEFAULT_TIME_FORMAT);
                     my_strftime(str, sizeof(str), format, &tms);
                     if (ltime1 > 0)
                        rsprintf(" %s %s", loc("to"), str);
                     else
                        rsprintf("%s %s", loc("Before"), str);
                  }
                  rsprintf("</td></tr>", comment);
               }

            } else if (attr_flags[i] & AF_MULTI) {

               line[0] = 0;

               for (j = 0; j < MAX_N_LIST && attr_options[i][j][0]; j++) {
                  sprintf(iattr, "%s_%d", attr_list[i], j);
                  if (isparam(iattr)) {

                     comment[0] = 0;
                     if (attr_flags[i] & AF_ICON) {
                        sprintf(str, "Icon comment %s", getparam(iattr));
                        getcfg(lbs->name, str, comment, sizeof(comment));
                     }

                     if (line[0])
                        strlcat(line, " | ", sizeof(line));

                     if (comment[0] == 0) {
                        strlcpy(str, getparam(iattr), sizeof(str));
                        if (str[0] == '^' && str[strlen(str)-1] == '$') {
                           str[strlen(str)-1] = 0;
                           strlcpy(comment, str+1, NAME_LENGTH);
                        } else
                           strlcpy(comment, str, NAME_LENGTH);
                        strlcpy(str, comment, sizeof(str));

                        strencode2(line + strlen(line), str, sizeof(line) - strlen(line));
                     } else
                        strlcat(line, comment, sizeof(line));
                  }
               }

               if (isparam(attr_list[i])) {
                  comment[0] = 0;
                  if (attr_flags[i] & AF_ICON) {
                     sprintf(str, "Icon comment %s", getparam(attr_list[i]));
                     getcfg(lbs->name, str, comment, sizeof(comment));
                  }

                  if (line[0])
                     strlcat(line, " | ", sizeof(line));

                  if (comment[0] == 0) {
                     strlcpy(str, getparam(attr_list[i]), sizeof(str));
                     if (str[0] == '^' && str[strlen(str)-1] == '$') {
                        str[strlen(str)-1] = 0;
                        strlcpy(comment, str+1, NAME_LENGTH);
                     } else
                        strlcpy(comment, str, NAME_LENGTH);
                     strlcpy(str, comment, sizeof(str));
                     strencode2(line + strlen(line), str, sizeof(line) - strlen(line));
                  } else
                     strlcat(line, comment, sizeof(line));
               }

               if (line[0]) {
                  rsprintf("<tr><td nowrap width=\"10%%\" class=\"attribname\">%s:</td>", attr_list[i]);
                  rsprintf("<td class=\"attribvalue\"><span style=\"color:black;background-color:#ffff66\">");
                  rsprintf("%s</span></td></tr>", line);
               }

            } else if (attr_flags[i] & (AF_MUSERLIST | AF_MUSEREMAIL)) {

               line[0] = 0;

               for (j = 0; j < MAX_N_LIST; j++) {
                  sprintf(iattr, "%s_%d", attr_list[i], j);
                  if (isparam(iattr)) {
                     if (line[0])
                        strlcat(line, " | ", sizeof(line));
                     strlcat(line, getparam(iattr), sizeof(line));
                  }
               }

               if (isparam(attr_list[i])) {
                  if (line[0])
                     strlcat(line, " | ", sizeof(line));
                  strencode2(line + strlen(line), getparam(attr_list[i]), sizeof(line) - strlen(line));
               }

               if (line[0]) {
                  rsprintf("<tr><td nowrap width=\"10%%\" class=\"attribname\">%s:</td>", attr_list[i]);
                  rsprintf("<td class=\"attribvalue\"><span style=\"color:black;background-color:#ffff66\">");
                  rsprintf("%s</span></td></tr>", line);
               }

            } else if (isparam(attr_list[i])) {

               strlcpy(str, getparam(attr_list[i]), sizeof(str));
               if (str[0] && !strieq(str, "_all_") && strncmp(str, "--", 2) != 0) {
                  comment[0] = 0;
                  if (attr_flags[i] & AF_ICON) {
                     sprintf(str, "Icon comment %s", getparam(attr_list[i]));
                     getcfg(lbs->name, str, comment, sizeof(comment));
                  }

                  if (comment[0] == 0) {
                     strlcpy(str, getparam(attr_list[i]), sizeof(str));
                     if (str[0] == '^' && str[strlen(str) - 1] == '$') {
                        str[strlen(str) - 1] = 0;
                        strlcpy(comment, str + 1, NAME_LENGTH);
                     } else
                        strlcpy(comment, str, NAME_LENGTH);
                     strlcpy(str, comment, sizeof(str));
                     strencode2(comment, str, sizeof(comment));
                  }

                  rsprintf("<tr><td nowrap width=\"10%%\" class=\"attribname\">%s:</td>", attr_list[i]);
                  rsprintf("<td class=\"attribvalue\"><span style=\"color:black;background-color:#ffff66\">");
                  rsprintf("%s</span></td></tr>", comment);
               }
            }
         }

         if (isparam("subtext")) {
            rsprintf("<tr><td nowrap width=\"10%%\" class=\"attribname\">%s:</td>", loc("Text"));
            rsprintf("<td class=\"attribvalue\"><span style=\"color:black;background-color:#ffff66\">");
            strencode2(str, getparam("subtext"), sizeof(str));
            rsprintf("%s</span></td></tr>", str);
         }

         rsprintf("</table></td></tr>\n\n");
      }

      /* get number of summary lines */
      n_line = 3;
      if (getcfg(lbs->name, "Summary lines", str, sizeof(str)))
         n_line = atoi(str);

      /* get mode commands flag */
      mode_commands = TRUE;
      if (getcfg(lbs->name, "Mode commands", str, sizeof(str)) && atoi(str) == 0)
         mode_commands = FALSE;

      /*---- evaluate conditions for quick filters */
      for (i = 0; i < lbs->n_attr; i++) {
         attrib[i][0] = 0;
         if (isparam(attr_list[i])) {
            strlcpy(str, getparam(attr_list[i]), sizeof(str));
            if (str[0] == '^' && str[strlen(str) - 1] == '$') {
               str[strlen(str) - 1] = 0;
               strlcpy(attrib[i], str + 1, NAME_LENGTH);
            } else
               strlcpy(attrib[i], str, NAME_LENGTH);
         }
      }
      evaluate_conditions(lbs, attrib);

      /*---- notification message ----*/

      if (info && info[0]) {
         rsprintf("<tr><td class=\"notifyleft\">%s</td></tr>\n", info);
      }

      /*---- page navigation ----*/

      if (!printable) {
         show_page_filters(lbs, n_msg, page_n, mode_commands, mode);
         show_page_navigation(lbs, n_msg, page_n, n_page);
      }

      /*---- select navigation ----*/

      if (isparam("select") && atoi(getparam("select")) == 1)
         show_select_navigation(lbs);

      /*---- table titles ----*/

      /* overall listing table */
      rsprintf("<tr><td><table class=\"listframe\" width=\"100%%\" cellspacing=0>\n");

      size = printable ? 2 : 3;
      show_text = TRUE;
      text_in_attr = FALSE;

      list[0] = 0;
      getcfg(lbs->name, "List display", list, 10000);

      /* evaluate Guest display list */
      if (getcfg(lbs->name, "Password file", str, sizeof(str)) && getcfg(lbs->name, "Guest list display",
                                                                         str, sizeof(str))
          && !isparam("unm")) {

         strcpy(list, str);

         n = strbreak(list, (char (*)[NAME_LENGTH]) gattr, MAX_N_ATTR, ",", FALSE);
         for (j = 0; j < n; j++)
            if (strieq(gattr + j * NAME_LENGTH, "text"))
               break;

         if (n > 0 && j == n)
            show_text = FALSE;
         else
            text_in_attr = TRUE;
      }

      if (list[0]) {
         n_attr_disp = strbreak(list, disp_attr, MAX_N_ATTR, ",", FALSE);

         /* if text is in guest display list, adjust number of *real* attributes */
         if (text_in_attr)
            n_attr_disp--;

         if (search_all) {
            for (i = n_attr_disp - 1; i >= 0; i--)
               strcpy(disp_attr[i + 1], disp_attr[i]);
            strcpy(disp_attr[0], loc("Logbook"));
            n_attr_disp++;
         }
      } else {
         if (search_all) {
            n_attr_disp = lbs->n_attr + 3;

            strcpy(disp_attr[0], loc("Logbook"));
            strcpy(disp_attr[1], loc("ID"));
            strcpy(disp_attr[2], loc("Date"));
            memcpy(disp_attr + 3, attr_list, sizeof(attr_list));
         } else {
            n_attr_disp = lbs->n_attr + 2;

            strcpy(disp_attr[0], loc("ID"));
            strcpy(disp_attr[1], loc("Date"));
            memcpy(disp_attr + 2, attr_list, sizeof(attr_list));
         }
      }

      list[0] = 0;
      getcfg(lbs->name, "Link display", list, 10000);
      if (list[0]) {
         n = strbreak(list, (char (*)[NAME_LENGTH]) gattr, MAX_N_ATTR, ",", FALSE);

         for (i = 0; i < n_attr_disp; i++) {
            for (j = 0; j < n; j++)
               if (strieq(gattr + j * NAME_LENGTH, disp_attr[i]))
                  break;

            if (j < n)
               disp_attr_link[i] = TRUE;
            else
               disp_attr_link[i] = FALSE;
         }
      } else
         for (i = 0; i < n_attr_disp; i++)
            disp_attr_link[i] = TRUE;

      if (threaded) {
      } else {
         rsprintf("<tr>\n");

         /* empty title for selection box */
         if (isparam("select") && atoi(getparam("select")) == 1)
            rsprintf("<th class=\"listtitle\">&nbsp;</th>\n");

         for (i = 0; i < n_attr_disp; i++) {
            /* assemble current command line, replace sort statements */
            strlcpy(ref, getparam("cmdline"), sizeof(ref));

            strlcpy(str, disp_attr[i], sizeof(str));
            url_encode(str, sizeof(str));
            if (isparam("sort") && strcmp(getparam("sort"), disp_attr[i]) == 0) {
               subst_param(ref, sizeof(ref), "sort", "");
               subst_param(ref, sizeof(ref), "rsort", str);
            } else {
               subst_param(ref, sizeof(ref), "rsort", "");
               subst_param(ref, sizeof(ref), "sort", str);
            }

            img[0] = 0;
            if (isparam("sort") && strcmp(getparam("sort"), disp_attr[i]) == 0)
               sprintf(img, "<img align=top src=\"up.png\" alt=\"%s\" title=\"%s\">", loc("up"), loc("up"));
            else if (isparam("rsort") && strcmp(getparam("rsort"), disp_attr[i]) == 0)
               sprintf(img, "<img align=top src=\"down.png\" alt=\"%s\" title=\"%s\">", loc("down"),
                       loc("down"));

            sprintf(str, "Tooltip %s", disp_attr[i]);
            if (getcfg(lbs->name, str, comment, sizeof(comment)))
               sprintf(str, "title=\"%s\"", comment);
            else
               str[0] = 0;

            if (strieq(disp_attr[i], "Edit") || strieq(disp_attr[i], "Delete"))
               rsprintf("<th %s class=\"listtitle\">%s</th>\n", str, disp_attr[i]);
            else {
               rsprintf("<th %s class=\"listtitle\"><a href=\"", str);
               rsputs3(ref);
               rsprintf("\">%s</a>%s</th>\n", disp_attr[i], img);
            }
         }

         if (!strieq(mode, "Full") && n_line > 0 && show_text)
            rsprintf("<th class=\"listtitle\">%s</th>\n", loc("Text"));

         if (strieq(mode, "Summary")) {
            rsprintf("<th class=\"listtitle\"><img src=\"attachment.png\" alt=\"%s\" title=\"%s\"</th>",
                     loc("Attachments"), loc("Attachments"));
         }

         rsprintf("</tr>\n\n");
      }
   }                            /* if (!csv && !xml) */

   /*---- display message list ----*/

   for (index = i_start; index <= i_stop; index++) {
      size = TEXT_SIZE;
      message_id = msg_list[index].lbs->el_index[msg_list[index].index].message_id;

      status = el_retrieve(msg_list[index].lbs, message_id, date, attr_list, attrib, lbs->n_attr, text,
                           &size, in_reply_to, reply_to, attachment, encoding, locked_by);
      if (status != EL_SUCCESS)
         break;

      if (csv) {

         rsprintf("%d", message_id);
         if (strieq(mode, "CSV1"))
            rsprintf(",");
         else
            rsprintf(";");

         strlcpy(str, date, sizeof(str));
         while (strchr(str, ','))
            *strchr(str, ',') = ' ';
         rsprintf(str);
         if (strieq(mode, "CSV1"))
            rsprintf(",");
         else
            rsprintf(";");

         for (i = 0; i < lbs->n_attr; i++) {
            strlcpy(str, attrib[i], sizeof(str));
            if (str[0]) {

               if (attr_flags[i] & AF_DATE) {
                  if (!getcfg(lbs->name, "Date format", format, sizeof(format)))
                     strcpy(format, DEFAULT_DATE_FORMAT);

                  ltime = atoi(attrib[i]);
                  ptms = localtime(&ltime);
                  assert(ptms);
                  if (ltime == 0)
                     strcpy(str, "-");
                  else
                     my_strftime(str, sizeof(str), format, ptms);

               } else if (attr_flags[i] & AF_DATETIME) {

                  if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
                     strcpy(format, DEFAULT_TIME_FORMAT);

                  ltime = atoi(attrib[i]);
                  ptms = localtime(&ltime);
                  assert(ptms);
                  if (ltime == 0)
                     strcpy(str, "-");
                  else
                     my_strftime(str, sizeof(str), format, ptms);

               }

               rsputs("\"");
               pt1 = str;
               while ((pt2 = strchr(pt1, '"')) != NULL) {
                  *pt2 = 0;
                  rsputs(pt1);
                  rsputs("\"\"");
                  pt1 = pt2 + 1;
               }
               rsputs(pt1);
               rsputs("\"");
            }
            if (i < lbs->n_attr - 1) {
               if (strieq(mode, "CSV1"))
                  rsprintf(",");
               else
                  rsprintf(";");
            } else
               rsprintf("\r\n");
         }

      } else if (xml) {

         rsputs("\t<ENTRY>\n");
         rsprintf("\t\t<MID>%d</MID>\n", message_id);
         rsprintf("\t\t<DATE>%s</DATE>\n", date);
         if (in_reply_to[0])
            rsprintf("\t\t<IN_REPLY_TO>%s</IN_REPLY_TO>\n", in_reply_to);
         if (reply_to[0])
            rsprintf("\t\t<REPLY_TO>%s</REPLY_TO>\n", reply_to);
         if (attachment[0][0]) {
            rsprintf("\t\t<ATTACHMENT>");
            rsprintf(attachment[0]);
            for (i = 1; i < MAX_ATTACHMENTS; i++)
               if (attachment[i][0])
                  rsprintf(",%s", attachment[i]);
            rsprintf("</ATTACHMENT>\n", attachment);
         }
         rsprintf("\t\t<ENCODING>%s</ENCODING>\n", encoding);

         for (i = 0; i < lbs->n_attr; i++) {
            strcpy(iattr, attr_list[i]);
            for (j = 0; j < (int) strlen(iattr); j++)
               /* replace special characters with "_", exclude any UTF-8 */
               if (!isalnum(iattr[j]) && ((unsigned char) iattr[j] < 128))
                  iattr[j] = '_';
            rsprintf("\t\t<%s>", iattr);

            strlcpy(str, attrib[i], sizeof(str));
            if (attr_flags[i] & AF_DATE) {
               if (!getcfg(lbs->name, "Date format", format, sizeof(format)))
                  strcpy(format, DEFAULT_DATE_FORMAT);

               ltime = atoi(attrib[i]);
               ptms = localtime(&ltime);
               assert(ptms);
               if (ltime == 0)
                  strcpy(str, "-");
               else
                  my_strftime(str, sizeof(str), format, ptms);

            } else if (attr_flags[i] & AF_DATETIME) {

               if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
                  strcpy(format, DEFAULT_TIME_FORMAT);

               ltime = atoi(attrib[i]);
               ptms = localtime(&ltime);
               assert(ptms);
               if (ltime == 0)
                  strcpy(str, "-");
               else
                  my_strftime(str, sizeof(str), format, ptms);

            }

            xmlencode(str);
            rsprintf("</%s>\n", iattr);
         }
         rsputs("\t\t<TEXT>");
         xmlencode(text);
         rsputs("</TEXT>\n");

         rsputs("\t</ENTRY>\n");

      } else if (raw) {

         rsprintf("$@MID@$: %d\r\n", message_id);
         rsprintf("Date: %s\r\n", date);

         if (reply_to[0])
            rsprintf("Reply to: %s\r\n", reply_to);

         if (in_reply_to[0])
            rsprintf("In reply to: %s\r\n", in_reply_to);

         for (i = 0; i < lbs->n_attr; i++)
            rsprintf("%s: %s\r\n", attr_list[i], attrib[i]);

         rsprintf("Attachment: ");

         if (attachment[0][0]) {
            rsprintf("%s", attachment[0]);
            for (i = 1; i < MAX_ATTACHMENTS; i++)
               if (attachment[i][0])
                  rsprintf(",%s", attachment[i]);
         }
         rsprintf("\r\n");

         rsprintf("Encoding: %s\r\n", encoding);
         if (locked_by && locked_by[0])
            rsprintf("Locked by: %s\r\n", locked_by);

         rsprintf("========================================\r\n");
         rsputs(text);
         rsputs("\r\n");

      } else {

         /*---- add highlighting for searched subtext ----*/

         if (isparam("subtext")) {
            highlight_searchtext(re_buf, text, text1, strieq(encoding, "plain") || strieq(encoding, "ELCode")
                                 || !strieq(mode, "Full"));
            strlcpy(text, text1, TEXT_SIZE);
         }

         /*---- display line ----*/

         expand = 1;
         if (threaded) {
            if (getcfg(lbs->name, "Expand default", str, sizeof(str)))
               expand = atoi(str);

            if (isparam("expand"))
               expand = atoi(getparam("expand"));
         }

         display_line(msg_list[index].lbs, message_id, index, mode, expand, 0, printable, n_line,
                      show_attachments, date, reply_to, n_attr_disp, disp_attr, disp_attr_link, attrib,
                      lbs->n_attr, text, show_text, attachment, encoding,
                      isparam("select") ? atoi(getparam("select")) : 0, &n_display, locked_by, 0, re_buf,
                      page_mid, FALSE);

         if (threaded) {
            if (reply_to[0] && expand > 0) {
               p = reply_to;
               do {
                  display_reply(msg_list[index].lbs, atoi(p), printable, expand, n_line, n_attr_disp,
                                disp_attr, show_text, 1, 0, re_buf, page_mid, FALSE);

                  while (*p && isdigit(*p))
                     p++;
                  while (*p && (*p == ',' || *p == ' '))
                     p++;
               } while (*p);
            }
         }
      }                         /* if (!csv && !xml) */
   }                            /* for() */

   if (!csv && !xml && !raw) {
      rsprintf("</table>\n");

      if (n_display)
         rsprintf("<input type=hidden name=nsel value=%d>\n", n_display);

      rsprintf("</td></tr>\n");

      if (n_msg == 0)
         rsprintf("<tr><td class=\"errormsg\">%s</td></tr>", loc("No entries found"));

      /*---- page navigation ----*/

      if (!printable)
         show_page_navigation(lbs, n_msg, page_n, n_page);

      rsprintf("</table>\n");
      show_bottom_text(lbs);
      rsprintf("</form></body></html>\r\n");
   }

   if (xml) {
      rsputs("</ELOG_LIST>\n");
   }

   regfree(re_buf);
   for (i = 0; i < lbs->n_attr; i++)
      regfree(re_buf + 1 + i);

   xfree(slist);
   xfree(svalue);
   xfree(gattr);
   xfree(list);
   xfree(msg_list);
   xfree(text);
   xfree(text1);
}

/*------------------------------------------------------------------*/

void show_elog_thread(LOGBOOK * lbs, int message_id, int absolute_links, int highlight_mid)
{
   int i, size, status, in_reply_to_id, head_id, n_display, n_attr_disp;
   char date[80], attrib[MAX_N_ATTR][NAME_LENGTH], *text, in_reply_to[80], reply_to[MAX_REPLY_TO * 10],
       attachment[MAX_ATTACHMENTS][MAX_PATH_LENGTH], encoding[80], locked_by[256],
       disp_attr[MAX_N_ATTR + 4][NAME_LENGTH];
   char *p;

   text = xmalloc(TEXT_SIZE);

   /* retrieve message */
   size = TEXT_SIZE;
   status = el_retrieve(lbs, message_id, date, attr_list, attrib, lbs->n_attr, text, &size, in_reply_to,
                        reply_to, attachment, encoding, locked_by);

   in_reply_to_id = atoi(in_reply_to);

   /* find message head */
   head_id = message_id;
   if (in_reply_to_id) {
      do {
         head_id = in_reply_to_id;

         /* search index of message */
         for (i = 0; i < *lbs->n_el_index; i++)
            if (lbs->el_index[i].message_id == head_id)
               break;

         /* stop if not found */
         if (i == *lbs->n_el_index)
            break;

         in_reply_to_id = lbs->el_index[i].in_reply_to;

      } while (in_reply_to_id);
   }

   n_attr_disp = lbs->n_attr + 2;
   strcpy(disp_attr[0], loc("ID"));
   strcpy(disp_attr[1], loc("Date"));
   memcpy(disp_attr + 2, attr_list, sizeof(attr_list));

   size = TEXT_SIZE;
   status = el_retrieve(lbs, head_id, date, attr_list, attrib, lbs->n_attr, text, &size, in_reply_to,
                        reply_to, attachment, encoding, locked_by);

   rsprintf("<tr><td><table width=\"100%%\" border=\"0\" cellpadding=\"0\" cellspacing=\"0\">\n");

   display_line(lbs, head_id, 0, "Threaded", 1, 0, FALSE, 0,
                FALSE, date, reply_to, n_attr_disp, disp_attr, NULL, attrib, lbs->n_attr, text, FALSE,
                attachment, encoding, 0, &n_display, locked_by, message_id, NULL, highlight_mid,
                absolute_links);

   if (reply_to[0]) {
      p = reply_to;
      do {
         display_reply(lbs, atoi(p), FALSE, 1, 0, n_attr_disp, disp_attr, FALSE, 1, message_id, NULL,
                       highlight_mid, absolute_links);

         while (*p && isdigit(*p))
            p++;
         while (*p && (*p == ',' || *p == ' '))
            p++;
      } while (*p);
   }

   rsprintf("</table>\n");
   rsprintf("</td></tr>\n");

   xfree(text);
}

/*------------------------------------------------------------------*/

int has_attachments(LOGBOOK * lbs, int message_id)
{
   char attachment[MAX_ATTACHMENTS][MAX_PATH_LENGTH];

   el_retrieve(lbs, message_id, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, attachment, NULL, NULL);
   return attachment[0][0] > 0;
}

/*------------------------------------------------------------------*/

void format_email_attachments(LOGBOOK * lbs, int message_id, int attachment_type,
                              char att_file[MAX_ATTACHMENTS][256], char *mail_text, int size,
                              char *multipart_boundary, int content_id)
{
   int i, index, n_att, fh, n, is_inline, length;
   char str[256], file_name[256], buffer[256], email_from[256], *p;

   /* count attachments */
   for (n_att = 0; att_file[n_att][0] && n_att < MAX_ATTACHMENTS; n_att++);

   for (index = 0; index < MAX_ATTACHMENTS; index++) {

      if (att_file[index][0] == 0)
         continue;

      is_inline = is_inline_attachment(getparam("encoding"), message_id, getparam("text"), index,
                                       att_file[index]);
      if (attachment_type == 1 && is_inline)
         continue;
      if (attachment_type == 2 && !is_inline)
         continue;

      snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1, "\r\n--%s\r\n",
               multipart_boundary);

      /* return proper Content-Type for file type */
      for (i = 0; i < (int) strlen(att_file[index]) && i < (int) sizeof(str) - 1; i++)
         str[i] = toupper(att_file[index][i]);
      str[i] = 0;

      for (i = 0; filetype[i].ext[0]; i++)
         if (strstr(str, filetype[i].ext))
            break;

      if (filetype[i].ext[0])
         snprintf(str, sizeof(str), "Content-Type: %s; name=\"%s\"\r\n", filetype[i].type, att_file[index]
                  + 14);
      else if (strchr(str, '.') == NULL)
         snprintf(str, sizeof(str), "Content-Type: text/plain; name=\"%s\"\r\n", att_file[index] + 14);
      else
         snprintf(str, sizeof(str), "Content-Type: application/octet-stream; name=\"%s\"\r\n",
                  att_file[index] + 14);

      strlcat(mail_text, str, size);

      strlcat(mail_text, "Content-Transfer-Encoding: BASE64\r\n", size);

      if (content_id) {
         retrieve_email_from(lbs, email_from, NULL, NULL);
         if (strchr(email_from, '@'))
            p = strchr(email_from, '@') + 1;
         else
            p = email_from;
         snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1, "Content-ID: <att%d@%s>\r\n",
                  index, p);
         snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1,
                  "Content-Disposition: inline; filename=\"%s\"\r\n\r\n", att_file[index] + 14);
      } else
         snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1,
                  "Content-Disposition: attachment; filename=\"%s\"\r\n\r\n", att_file[index] + 14);

      /* encode file */
      strlcpy(file_name, lbs->data_dir, sizeof(file_name));
      strlcat(file_name, att_file[index], sizeof(file_name));
      if (is_image(file_name)) {
         get_thumb_name(file_name, str, sizeof(str), 0);
         strlcpy(file_name, str, sizeof(file_name));
      }

      fh = open(file_name, O_RDONLY | O_BINARY);
      length = strlen(mail_text);
      if (fh > 0) {
         do {
            n = my_read(fh, buffer, 45);
            if (n <= 0)
               break;

            base64_bufenc((unsigned char *) buffer, n, str);

            if (length + (int) strlen(str) + 2 < size) {
               strcpy(mail_text + length, str);
               length += strlen(str);
               strcpy(mail_text + length, "\r\n");
               length += 2;
            }
         } while (1);

         close(fh);
      }
   }
}

/*------------------------------------------------------------------*/

void format_email_text(LOGBOOK * lbs, char attrib[MAX_N_ATTR][NAME_LENGTH],
                       char att_file[MAX_ATTACHMENTS][256], int old_mail, char *url, char *multipart_boundary,
                       char *mail_text, int size)
{
   int i, j, k, flags, n_email_attr, attr_index[MAX_N_ATTR];
   char str[NAME_LENGTH + 100], str2[256], mail_from[256], mail_from_name[256], format[256],
       list[MAX_N_ATTR][NAME_LENGTH], comment[256], charset[256], heading[256],
       slist[MAX_N_ATTR + 10][NAME_LENGTH], svalue[MAX_N_ATTR + 10][NAME_LENGTH];
   time_t ltime;
   struct tm *pts;

   if (multipart_boundary[0]) {
      if (!getcfg("global", "charset", charset, sizeof(charset)))
         strcpy(charset, DEFAULT_HTTP_CHARSET);

      strlcat(mail_text, "--", size);
      strlcat(mail_text, multipart_boundary, size);
      strlcat(mail_text, "\r\n", size);
      sprintf(mail_text + strlen(mail_text), "Content-Type: text/plain; charset=%s; format=flowed\r\n",
              charset);
      sprintf(mail_text + strlen(mail_text), "Content-Transfer-Encoding: 7bit\r\n\r\n");
   } else
      strlcat(mail_text, "\r\n", size);

   flags = 63;
   if (getcfg(lbs->name, "Email format", str, sizeof(str)))
      flags = atoi(str);

   retrieve_email_from(lbs, mail_from, mail_from_name, attrib);

   if (flags & 1) {
      if (getcfg(lbs->name, "Use Email heading", heading, sizeof(heading))) {
         if (old_mail) {
            if (!getcfg(lbs->name, "Use Email heading edit", heading, sizeof(heading)))
               getcfg(lbs->name, "Use Email heading", heading, sizeof(heading));
         }

         i = build_subst_list(lbs, slist, svalue, attrib, TRUE);
         strsubst_list(heading, sizeof(heading), slist, svalue, i);

         sprintf(mail_text + strlen(mail_text), heading);

      } else {
         if (old_mail)
            sprintf(mail_text + strlen(mail_text), loc("An old ELOG entry has been updated"));
         else
            sprintf(mail_text + strlen(mail_text), loc("A new ELOG entry has been submitted"));
         strcat(mail_text, ":");
      }

      sprintf(mail_text + strlen(mail_text), "\r\n\r\n");
   }

   if (flags & 32)
      sprintf(mail_text + strlen(mail_text), "%s             : %s\r\n", loc("Logbook"), lbs->name);

   if (flags & 2) {

      if (getcfg(lbs->name, "Email attributes", str, sizeof(str))) {
         n_email_attr = strbreak(str, list, MAX_N_ATTR, ",", FALSE);
         for (i = 0; i < n_email_attr; i++) {
            for (j = 0; j < lbs->n_attr; j++)
               if (strieq(attr_list[j], list[i]))
                  break;
            if (!strieq(attr_list[j], list[i]))
               /* attribute not found */
               j = 0;
            attr_index[i] = j;
         }
      } else {
         for (i = 0; i < lbs->n_attr; i++)
            attr_index[i] = i;
         n_email_attr = lbs->n_attr;
      }

      for (j = 0; j < n_email_attr; j++) {

         i = attr_index[j];
         strcpy(str, "                                                                       ");
         memcpy(str, attr_list[i], strlen(attr_list[i]));

         comment[0] = 0;
         if (attr_flags[i] & AF_ICON) {

            sprintf(str2, "Icon comment %s", attrib[i]);
            getcfg(lbs->name, str2, comment, sizeof(comment));

         } else if (attr_flags[i] & AF_DATE) {

            if (!getcfg(lbs->name, "Date format", format, sizeof(format)))
               strcpy(format, DEFAULT_DATE_FORMAT);

            ltime = atoi(attrib[i]);
            pts = localtime(&ltime);
            assert(pts);
            if (ltime == 0)
               strcpy(comment, "-");
            else
               my_strftime(comment, sizeof(str), format, pts);
         } else if (attr_flags[i] & AF_DATETIME) {

            if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
               strcpy(format, DEFAULT_TIME_FORMAT);

            ltime = atoi(attrib[i]);
            pts = localtime(&ltime);
            assert(pts);
            if (ltime == 0)
               strcpy(comment, "-");
            else
               my_strftime(comment, sizeof(str), format, pts);
         }

         if (!comment[0])
            strcpy(comment, attrib[i]);

         if (strieq(attr_options[i][0], "boolean"))
            strcpy(comment, atoi(attrib[i]) ? "1" : "0");

         for (k = strlen(str) - 1; k > 0; k--)
            if (str[k] != ' ')
               break;

         if (k < 20)
            sprintf(str + 20, ": %s\r\n", comment);
         else
            sprintf(str + k + 1, ": %s\r\n", comment);

         strcpy(mail_text + strlen(mail_text), str);
      }
   }

   if (flags & 4)
      sprintf(mail_text + strlen(mail_text), "\r\n%s URL         : %s\r\n", loc("Logbook"), url);

   if (flags & 64) {
      for (i = 0; i < MAX_ATTACHMENTS && att_file[i][0]; i++)
         sprintf(mail_text + strlen(mail_text), "\r\n%s %d        : %s (%s/%d)\r\n", loc("Attachment"),
                 i + 1, att_file[i] + 14, url, i + 1);
   }

   if (flags & 8) {
      if (isparam("text")) {
         sprintf(mail_text + strlen(mail_text), "\r\n=================================\r\n\r\n%s",
                 getparam("text"));
      }
   }

   strlcat(mail_text, "\r\n\r\n", size);
}

/*------------------------------------------------------------------*/

void format_email_html(LOGBOOK * lbs, int message_id, char attrib[MAX_N_ATTR][NAME_LENGTH],
                       char att_file[MAX_ATTACHMENTS][256], int old_mail, char *encoding, char *url,
                       char *multipart_boundary, char *mail_text, int size)
{
   int i, j, k, flags, n_email_attr, attr_index[MAX_N_ATTR], attachments_present;
   char str[NAME_LENGTH + 100], str2[256], mail_from[256], mail_from_name[256], format[256],
       list[MAX_N_ATTR][NAME_LENGTH], comment[256], charset[256], multipart_boundary_related[256],
       heading[256], slist[MAX_N_ATTR + 10][NAME_LENGTH], svalue[MAX_N_ATTR + 10][NAME_LENGTH];
   time_t ltime;
   struct tm *pts;

   if (!getcfg("global", "charset", charset, sizeof(charset)))
      strcpy(charset, DEFAULT_HTTP_CHARSET);

   if (multipart_boundary[0]) {
      strlcat(mail_text, "--", size);
      strlcat(mail_text, multipart_boundary, size);
      strlcat(mail_text, "\r\n", size);
   }

   attachments_present = has_attachments(lbs, message_id);

   if (attachments_present) {
      sprintf(multipart_boundary_related, "------------%04X%04X%04X", rand(), rand(), rand());
      snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1,
               "Content-Type: multipart/related;\r\n boundary=\"%s\"\r\n\r\n", multipart_boundary_related);
      strlcat(mail_text, "--", size);
      strlcat(mail_text, multipart_boundary_related, size);
      strlcat(mail_text, "\r\n", size);
   }

   snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1,
            "Content-Type: text/html; charset=\"%s\"\r\n", charset);
   snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1,
            "Content-Transfer-Encoding: 7bit\r\n\r\n");

   retrieve_email_from(lbs, mail_from, mail_from_name, attrib);

   flags = 63;
   if (getcfg(lbs->name, "Email format", str, sizeof(str)))
      flags = atoi(str);

   strcpy(mail_text + strlen(mail_text),
          "<!DOCTYPE html PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\">\r\n");
   strcpy(mail_text + strlen(mail_text), "<html>\r\n<head>\r\n  <title></title>\r\n</head>\r\n<body>\r\n");

   if (flags & 1) {
      strcpy(mail_text + strlen(mail_text), "<h3>\r\n");
      if (getcfg(lbs->name, "Use Email heading", heading, sizeof(heading))) {
         if (old_mail) {
            if (!getcfg(lbs->name, "Use Email heading edit", heading, sizeof(heading)))
               getcfg(lbs->name, "Use Email heading", heading, sizeof(heading));
         }

         i = build_subst_list(lbs, slist, svalue, attrib, TRUE);
         strsubst_list(heading, sizeof(heading), slist, svalue, i);

         sprintf(mail_text + strlen(mail_text), heading);

      } else {
         if (old_mail)
            sprintf(mail_text + strlen(mail_text), loc("A old entry has been updated on %s"), host_name);
         else
            sprintf(mail_text + strlen(mail_text), loc("A new entry has been submitted on %s"), host_name);
         strcat(mail_text, ":");
      }

      strcpy(mail_text + strlen(mail_text), "</h3>\r\n");
   }

   sprintf(mail_text + strlen(mail_text), "<table border=\"3\" cellpadding=\"4\">\r\n");

   if (flags & 32) {
      sprintf(mail_text + strlen(mail_text), "<tr><td bgcolor=\"#CCCCFF\">%s</td>", loc("Logbook"));
      sprintf(mail_text + strlen(mail_text), "<td bgcolor=\"#DDEEBB\">%s</td></tr>\r\n", lbs->name);
   }

   if (flags & 2) {

      if (getcfg(lbs->name, "Email attributes", str, sizeof(str))) {
         n_email_attr = strbreak(str, list, MAX_N_ATTR, ",", FALSE);
         for (i = 0; i < n_email_attr; i++) {
            for (j = 0; j < lbs->n_attr; j++)
               if (strieq(attr_list[j], list[i]))
                  break;
            if (!strieq(attr_list[j], list[i]))
               /* attribute not found */
               j = 0;
            attr_index[i] = j;
         }
      } else {
         for (i = 0; i < lbs->n_attr; i++)
            attr_index[i] = i;
         n_email_attr = lbs->n_attr;
      }

      for (j = 0; j < n_email_attr; j++) {

         i = attr_index[j];
         strcpy(str, "                                                                       ");
         memcpy(str, attr_list[i], strlen(attr_list[i]));

         comment[0] = 0;
         if (attr_flags[i] & AF_ICON) {

            sprintf(str2, "Icon comment %s", attrib[i]);
            getcfg(lbs->name, str2, comment, sizeof(comment));

         } else if (attr_flags[i] & AF_DATE) {

            if (!getcfg(lbs->name, "Date format", format, sizeof(format)))
               strcpy(format, DEFAULT_DATE_FORMAT);

            ltime = atoi(attrib[i]);
            pts = localtime(&ltime);
            assert(pts);
            if (ltime == 0)
               strcpy(comment, "-");
            else
               my_strftime(comment, sizeof(str), format, pts);
         } else if (attr_flags[i] & AF_DATETIME) {

            if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
               strcpy(format, DEFAULT_TIME_FORMAT);

            ltime = atoi(attrib[i]);
            pts = localtime(&ltime);
            assert(pts);
            if (ltime == 0)
               strcpy(comment, "-");
            else
               my_strftime(comment, sizeof(str), format, pts);
         }

         if (!comment[0])
            strcpy(comment, attrib[i]);

         if (strieq(attr_options[i][0], "boolean"))
            strcpy(comment, atoi(attrib[i]) ? "1" : "0");

         for (k = strlen(str) - 1; k > 0; k--)
            if (str[k] != ' ')
               break;

         sprintf(mail_text + strlen(mail_text), "<tr><td bgcolor=\"#CCCCFF\">%s</td>", attr_list[i]);
         sprintf(mail_text + strlen(mail_text), "<td bgcolor=\"#DDEEBB\">%s</td></tr>\r\n", comment);
      }
   }

   if (flags & 4) {
      sprintf(mail_text + strlen(mail_text),
              "<tr><td bgcolor=\"#CCCCFF\">%s URL</td><td bgcolor=\"#DDEEBB\">", loc("Logbook"));
      sprintf(mail_text + strlen(mail_text), "<a href=\"%s\">%s</a></td></tr>\r\n", url, url);
   }

   if (flags & 64) {
      for (i = 0; i < MAX_ATTACHMENTS && att_file[i][0]; i++) {
         sprintf(mail_text + strlen(mail_text),
                 "<tr><td bgcolor=\"#CCCCFF\">%s %d</td><td bgcolor=\"#DDEEBB\">", loc("Attachment"), i + 1);
         sprintf(mail_text + strlen(mail_text), "<a href=\"%s/%d\">%s</a></td></tr>\r\n", url, i + 1,
                 att_file[i] + 14);
      }
   }

   sprintf(mail_text + strlen(mail_text), "</table>\r\n");

   if (flags & 8) {
      if (isparam("text")) {
         if (encoding[0] == 'H')
            sprintf(mail_text + strlen(mail_text), "\r\n<HR>\r\n%s", getparam("text"));
         else if (encoding[0] == 'E') {
            sprintf(mail_text + strlen(mail_text), "\r\n<HR>\r\n");
            strlen_retbuf = 0;
            rsputs_elcode(lbs, TRUE, getparam("text"));
            strlcpy(mail_text + strlen(mail_text), return_buffer, TEXT_SIZE + 1000 - strlen(mail_text));
            strlen_retbuf = 0;
         } else
            sprintf(mail_text + strlen(mail_text), "\r\n=================================\r\n\r\n%s",
                    getparam("text"));
      }
   }

   strcpy(mail_text + strlen(mail_text), "\r\n</html></body>\r\n\r\n");

   if (attachments_present) {
      format_email_attachments(lbs, message_id, 2, att_file, mail_text, size, multipart_boundary_related,
                               TRUE);
      strlcat(mail_text, "--", size);
      strlcat(mail_text, multipart_boundary_related, size);
      strlcat(mail_text, "--\r\n\r\n", size);
   }
}

/*------------------------------------------------------------------*/

void format_email_html2(LOGBOOK * lbs, int message_id, char att_file[MAX_ATTACHMENTS][256], int old_mail,
                        char *multipart_boundary, char *mail_text, int size)
{
   char str[256], charset[256], multipart_boundary_related[256], *p;
   int attachments_present;

   sprintf(str, "%d", message_id);
   if (!getcfg("global", "charset", charset, sizeof(charset)))
      strcpy(charset, DEFAULT_HTTP_CHARSET);

   if (multipart_boundary[0]) {
      strlcat(mail_text, "--", size);
      strlcat(mail_text, multipart_boundary, size);
      strlcat(mail_text, "\r\n", size);
   }

   attachments_present = has_attachments(lbs, message_id);

   if (attachments_present) {
      sprintf(multipart_boundary_related, "------------%04X%04X%04X", rand(), rand(), rand());
      snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1,
               "Content-Type: multipart/related;\r\n boundary=\"%s\"\r\n\r\n", multipart_boundary_related);
      strlcat(mail_text, "--", size);
      strlcat(mail_text, multipart_boundary_related, size);
      strlcat(mail_text, "\r\n", size);
   }

   snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1,
            "Content-Type: text/html; charset=\"%s\"\r\n", charset);
   snprintf(mail_text + strlen(mail_text), size - strlen(mail_text) - 1,
            "Content-Transfer-Encoding: 7bit\r\n\r\n");

   strlen_retbuf = 0;
   if (old_mail)
      show_elog_entry(lbs, str, "oldemail");
   else
      show_elog_entry(lbs, str, "email");
   p = strstr(return_buffer, "\r\n\r\n");
   if (p)
      strlcpy(mail_text + strlen(mail_text), p + 4, size - strlen(mail_text));
   strlen_retbuf = 0;
   strlcat(mail_text, "\r\n", size);

   if (attachments_present) {
      format_email_attachments(lbs, message_id, 2, att_file, mail_text, size, multipart_boundary_related,
                               TRUE);
      strlcat(mail_text, "--", size);
      strlcat(mail_text, multipart_boundary_related, size);
      strlcat(mail_text, "--\r\n\r\n", size);
   }
}

/*------------------------------------------------------------------*/

int compose_email(LOGBOOK * lbs, char *rcpt_to, char *mail_to, int message_id,
                  char attrib[MAX_N_ATTR][NAME_LENGTH], char *mail_param, int old_mail,
                  char att_file[MAX_ATTACHMENTS][256], char *encoding, int reply_id)
{
   int i, n, flags, status, mail_encoding, mail_text_size, n_attachments;
   char str[NAME_LENGTH + 100], mail_from[256], mail_from_name[256], *mail_text, smtp_host[256],
       subject[256], error[256];
   char list[MAX_PARAM][NAME_LENGTH], url[256];
   char slist[MAX_N_ATTR + 10][NAME_LENGTH], svalue[MAX_N_ATTR + 10][NAME_LENGTH];
   char multipart_boundary[80];

   if (!getcfg("global", "SMTP host", smtp_host, sizeof(smtp_host))) {
      show_error(loc("No SMTP host defined in [global] section of configuration file"));
      return 0;
   }

   evaluate_conditions(lbs, attrib);

   flags = 63;
   if (getcfg(lbs->name, "Email format", str, sizeof(str)))
      flags = atoi(str);

   /* get initial HTML flag from message encoding */
   mail_encoding = 1;           // 1:text, 2:short HTML, 4:full HTML
   if (encoding[0] == 'E' || encoding[0] == 'H')
      mail_encoding = 4;

   /* overwrite with config setting */
   if (getcfg(lbs->name, "Email encoding", str, sizeof(str)))
      mail_encoding = atoi(str);

   retrieve_email_from(lbs, mail_from, mail_from_name, attrib);

   /* compose subject from attributes */
   if (getcfg(lbs->name, "Use Email Subject", subject, sizeof(subject))) {
      i = build_subst_list(lbs, slist, svalue, attrib, TRUE);
      sprintf(str, "%d", message_id);
      add_subst_list(slist, svalue, "message id", str, &i);
      strsubst_list(subject, sizeof(subject), slist, svalue, i);
   } else {
      if (old_mail)
         strcpy(subject, "Updated ELOG entry");
      else
         strcpy(subject, "New ELOG entry");
   }

   /* count attachments */
   n_attachments = 0;
   if (att_file)
      for (i = 0; att_file[i][0] && i < MAX_ATTACHMENTS; i++) {
         if ((mail_encoding & 6) == 0 || !is_inline_attachment(encoding, message_id, getparam("text"), i,
                                                               att_file[i]))
            n_attachments++;
      }

   compose_base_url(lbs, str, sizeof(str), TRUE);
   sprintf(url, "%s%d", str, message_id);

   mail_text_size = MAX_CONTENT_LENGTH + 1000;
   mail_text = xmalloc(mail_text_size);
   mail_text[0] = 0;

   compose_email_header(lbs, subject, mail_from_name, mail_to, url, mail_text, mail_text_size, mail_encoding,
                        n_attachments, multipart_boundary, message_id, reply_id);

   if (mail_encoding & 1)
      format_email_text(lbs, attrib, att_file, old_mail, url, multipart_boundary, mail_text, mail_text_size);
   if (mail_encoding & 2)
      format_email_html(lbs, message_id, attrib, att_file, old_mail, encoding, url, multipart_boundary,
                        mail_text, mail_text_size);
   if (mail_encoding & 4)
      format_email_html2(lbs, message_id, att_file, old_mail, multipart_boundary, mail_text, mail_text_size);

   if (n_attachments && (flags & 16)) {
      if ((mail_encoding & 6) > 0)
         /* only non-inline attachements */
         format_email_attachments(lbs, message_id, 1, att_file, mail_text, mail_text_size,
                                  multipart_boundary, FALSE);
      else
         /* all attachments */
         format_email_attachments(lbs, message_id, 0, att_file, mail_text, mail_text_size,
                                  multipart_boundary, FALSE);
   }

   if (multipart_boundary[0]) {
      strlcat(mail_text, "--", mail_text_size);
      strlcat(mail_text, multipart_boundary, mail_text_size);
      strlcat(mail_text, "--\r\n\r\n", mail_text_size);
   }

   status = sendmail(lbs, smtp_host, mail_from, rcpt_to, mail_text, error, sizeof(error));

   /*
      {
      int fh;
      fh = open("mail.html", O_WRONLY | O_BINARY | O_CREAT | O_TRUNC, 0644);
      write(fh, mail_text, strlen(mail_text));
      close(fh);
      }
    */

   if (status < 0) {
      sprintf(str, loc("Error sending Email via <i>\"%s\"</i>"), smtp_host);
      if (error[0]) {
         strlcat(str, ": ", sizeof(str));
         strlcat(str, error, sizeof(str));
      }
      url_encode(str, sizeof(str));
      sprintf(mail_param, "?error=%s", str);
   } else if (error[0]) {
      sprintf(str, loc("Error sending Email via <i>\"%s\"</i>"), smtp_host);
      strlcat(str, ": ", sizeof(str));
      strlcat(str, error, sizeof(str));
      url_encode(str, sizeof(str));
      sprintf(mail_param, "?error=%s", str);
   } else {
      if (!getcfg(lbs->name, "Display email recipients", str, sizeof(str)) || atoi(str) == 1) {
         if (mail_param[0] == 0)
            strcpy(mail_param, "?");
         else
            strcat(mail_param, "&");

         /* convert '"',CR,LF,TAB to ' ' */
         while (strchr(mail_to, '"'))
            *strchr(mail_to, '"') = ' ';
         while (strchr(mail_to, '\r'))
            *strchr(mail_to, '\r') = ' ';
         while (strchr(mail_to, '\n'))
            *strchr(mail_to, '\n') = ' ';
         while (strchr(mail_to, '\t'))
            *strchr(mail_to, '\t') = ' ';

         n = strbreak(mail_to, list, MAX_PARAM, ",", FALSE);

         if (n < 10) {
            for (i = 0; i < n && i < MAX_PARAM; i++) {
               strencode2(str, list[i], sizeof(str));
               url_encode(str, sizeof(str));
               sprintf(mail_param + strlen(mail_param), "mail%d=%s", i, str);
               if (i < n - 1)
                  strcat(mail_param, "&");
            }
         } else {
            sprintf(str, "%d%%20%s", n, loc("recipients"));
            sprintf(mail_param + strlen(mail_param), "mail0=%s", str);
         }
      }
   }

   xfree(mail_text);

   return status;
}

/*------------------------------------------------------------------*/

int execute_shell(LOGBOOK * lbs, int message_id, char attrib[MAX_N_ATTR][NAME_LENGTH],
                  char att_file[MAX_ATTACHMENTS][256], char *sh_cmd)
{
   int i;
   char slist[MAX_N_ATTR + 10][NAME_LENGTH], svalue[MAX_N_ATTR + 10][NAME_LENGTH];
   char shell_cmd[10000], tail[1000], str[NAME_LENGTH], *p;

   if (!enable_execute) {
      eprintf("Shell execution not enabled via -x flag.\n");
      return SUCCESS;
   }

   strlcpy(shell_cmd, sh_cmd, sizeof(shell_cmd));

   i = build_subst_list(lbs, slist, svalue, attrib, TRUE);
   sprintf(str, "%d", message_id);
   add_subst_list(slist, svalue, "message id", str, &i);
   add_subst_list(slist, svalue, "text", getparam("text"), &i);
   strsubst_list(shell_cmd, sizeof(shell_cmd), slist, svalue, i);

   if (att_file && stristr(shell_cmd, "$attachments")) {
      /* substitute attachments */
      p = stristr(shell_cmd, "$attachments");
      strlcpy(tail, p + strlen("$attachments"), sizeof(tail));
      *p = 0;
      for (i = 0; i < MAX_ATTACHMENTS; i++)
         if (att_file[i][0] && strlen(shell_cmd) + strlen(lbs->data_dir) + strlen(att_file[i])
             < sizeof(shell_cmd) + 1) {
            strcpy(p, "\"");
            strcat(p, lbs->data_dir);
            strlcpy(str, att_file[i], sizeof(str));
            str_escape(str, sizeof(str));
            strcat(p, str);
            strcat(p, "\" ");
            p += strlen(p);
         }
      strlcat(shell_cmd, tail, sizeof(shell_cmd));
   }

   sprintf(str, "SHELL \"%s\"", shell_cmd);
   write_logfile(lbs, str);

   my_shell(shell_cmd, str, sizeof(str));

   return SUCCESS;
}

/*------------------------------------------------------------------*/

int add_attribute_option(LOGBOOK * lbs, char *attrname, char *attrvalue, char *condition)
{
   int fh, i, length;
   char str[NAME_LENGTH], av_encoded[NAME_LENGTH], *buf, *buf2, *p1, *p2, *p3;

   fh = open(config_file, O_RDWR | O_BINARY, 0644);
   if (fh < 0) {
      sprintf(str, loc("Cannot open file <b>%s</b>"), config_file);
      strcat(str, ": ");
      strcat(str, strerror(errno));
      show_error(str);
      return 0;
   }

   /* do not allow HTML code in value */
   strencode2(av_encoded, attrvalue, sizeof(av_encoded));

   /* read previous contents */
   length = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);
   buf = xmalloc(length + strlen(av_encoded) + 3);
   read(fh, buf, length);
   buf[length] = 0;

   /* find location of options */
   if (condition && condition[0])
      set_condition(condition);
   else
      set_condition("");

   sprintf(str, "Options %s", attrname);
   p1 = (char *) find_param(buf, lbs->name, str);
   if (p1 == NULL) {
      sprintf(str, "MOptions %s", attrname);
      p1 = (char *) find_param(buf, lbs->name, str);
   }
   if (p1 == NULL) {
      sprintf(str, "ROptions %s", attrname);
      p1 = (char *) find_param(buf, lbs->name, str);
   }
   if (p1 == NULL)
      return 0;

   p2 = strchr(p1, '\n');
   if (p2 && *(p2 - 1) == '\r')
      p2--;

   /* save tail */
   buf2 = NULL;
   if (p2)
      buf2 = xstrdup(p2);

   /* add option */
   p3 = strchr(p1, '\n');
   if (p3 == NULL)
      p3 = p1 + strlen(p1);
   while (*(p3 - 1) == '\n' || *(p3 - 1) == '\r' || *(p3 - 1) == ' ' || *(p3 - 1) == '\t')
      p3--;

   sprintf(p3, ", %s", av_encoded);
   if (p2) {
      strlcat(buf, buf2, length + strlen(av_encoded) + 3);
      xfree(buf2);
   }

   lseek(fh, 0, SEEK_SET);
   i = write(fh, buf, strlen(buf));
   if (i < (int) strlen(buf)) {
      sprintf(str, loc("Cannot write to <b>%s</b>"), config_file);
      strcat(str, ": ");
      strcat(str, strerror(errno));
      show_error(str);
      close(fh);
      xfree(buf);
      return 0;
   }

   TRUNCATE(fh);

   close(fh);
   xfree(buf);

   /* force re-read of config file */
   check_config_file(TRUE);

   return 1;
}

/*------------------------------------------------------------------*/

int set_attributes(LOGBOOK * lbs, char attributes[][NAME_LENGTH], int n)
{
   int fh, i, length, size;
   char str[NAME_LENGTH], *buf, *buf2, *p1, *p2, *p3;

   fh = open(config_file, O_RDWR | O_BINARY, 0644);
   if (fh < 0) {
      sprintf(str, loc("Cannot open file <b>%s</b>"), config_file);
      strcat(str, ": ");
      strcat(str, strerror(errno));
      show_error(str);
      return 0;
   }

   /* determine length of attributes */
   for (i = size = 0; i < n; i++)
      size += strlen(attributes[i]) + 2;

   /* read previous contents */
   length = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);
   buf = xmalloc(length + size + 3);
   read(fh, buf, length);
   buf[length] = 0;

   /* find location of attributes */
   p1 = (char *) find_param(buf, lbs->name, "Attributes");
   if (p1 == NULL) {
      sprintf(str, loc("No 'Attributes' option present in %s"), config_file);
      show_error(str);
      return 0;
   }

   p2 = strchr(p1, '\n');
   if (p2 && *(p2 - 1) == '\r')
      p2--;

   /* save tail */
   buf2 = NULL;
   if (p2)
      buf2 = xstrdup(p2);

   /* add list */
   p3 = strchr(p1, '=');
   if (p3 == NULL)
      return 0;
   p3++;
   while (*p3 == ' ')
      p3++;

   for (i = 0; i < n - 1; i++) {
      sprintf(p3, "%s, ", attributes[i]);
      p3 += strlen(p3);
   }
   sprintf(p3, "%s", attributes[i]);

   if (p2) {
      strlcat(buf, buf2, length + size + 3);
      xfree(buf2);
   }

   lseek(fh, 0, SEEK_SET);
   i = write(fh, buf, strlen(buf));
   if (i < (int) strlen(buf)) {
      sprintf(str, loc("Cannot write to <b>%s</b>"), config_file);
      strcat(str, ": ");
      strcat(str, strerror(errno));
      show_error(str);
      close(fh);
      xfree(buf);
      return 0;
   }

   TRUNCATE(fh);

   close(fh);
   xfree(buf);

   /* force re-read of config file */
   check_config_file(TRUE);

   return 1;
}

/*------------------------------------------------------------------*/

int submit_elog_reply(LOGBOOK * lbs, int message_id, char attrib[MAX_N_ATTR][NAME_LENGTH], char *text)
{
   int n_reply, i, status;
   char str1[80], str2[80], att_file[MAX_ATTACHMENTS][256], reply_to[MAX_REPLY_TO * 10],
       list[MAX_N_ATTR][NAME_LENGTH];

   status = el_retrieve(lbs, message_id, NULL, attr_list, NULL, 0,
                        NULL, NULL, NULL, reply_to, att_file, NULL, NULL);
   if (status != EL_SUCCESS)
      return status;

   sprintf(str1, "- %s -", loc("keep original text"));
   sprintf(str2, "<p>- %s -</p>", loc("keep original text"));
   if (strcmp(text, str1) == 0 || strcmp(text, str2) == 0)
      message_id = el_submit(lbs, message_id, TRUE, "<keep>", attr_list, attrib, lbs->n_attr, "<keep>",
                             "<keep>", "<keep>", "<keep>", att_file, TRUE, NULL);
   else
      message_id = el_submit(lbs, message_id, TRUE, "<keep>", attr_list, attrib, lbs->n_attr, text, "<keep>",
                             "<keep>", "<keep>", att_file, TRUE, NULL);

   if (message_id < 0)
      return 0;

   if (isparam("elmode") && strieq(getparam("elmode"), "threaded")) {

      // go through all replies in threaded mode
      n_reply = strbreak(reply_to, list, MAX_N_ATTR, ",", FALSE);
      for (i = 0; i < n_reply; i++) {
         submit_elog_reply(lbs, atoi(list[i]), attrib, text);
      }
   }

   return EL_SUCCESS;
}

/*------------------------------------------------------------------*/

void submit_elog(LOGBOOK * lbs)
{
   char str[NAME_LENGTH], str2[NAME_LENGTH], file_name[256], error[1000], date[80], *mail_list, *rcpt_list,
       list[10000], *p, locked_by[256], encoding[80], attrib[MAX_N_ATTR][NAME_LENGTH],
       subst_str[MAX_PATH_LENGTH], in_reply_to[80], reply_to[MAX_REPLY_TO * 10], user[256],
       user_email[256], mail_param[1000], *mail_to, *rcpt_to, full_name[256],
       att_file[MAX_ATTACHMENTS][256], slist[MAX_N_ATTR + 10][NAME_LENGTH],
       svalue[MAX_N_ATTR + 10][NAME_LENGTH], ua[NAME_LENGTH];
   int i, j, k, n, missing, first, index, mindex, suppress, message_id, resubmit_orig, mail_to_size,
       rcpt_to_size, ltime, year, month, day, hour, min, sec, n_attr, email_notify[1000], allowed_encoding,
       status;
   BOOL bedit, bmultiedit;
   struct tm tms;

   bmultiedit = isparam("nsel");
   bedit = isparam("edit_id");

   /* check for required attributs */
   missing = 0;
   for (i = 0; i < lbs->n_attr; i++) {
      strcpy(ua, attr_list[i]);
      stou(ua);

      if (attr_flags[i] & AF_REQUIRED) {
         if (attr_flags[i] & AF_DATE) {
            sprintf(str, "d%d", i);
            if (isparam(str) == 0)
               missing = 1;
            sprintf(str, "m%d", i);
            if (isparam(str) == 0)
               missing = 1;
            sprintf(str, "y%d", i);
            if (isparam(str) == 0)
               missing = 1;
            if (missing)
               break;
         } else if (attr_flags[i] & AF_DATETIME) {
            sprintf(str, "d%d", i);
            if (isparam(str) == 0)
               missing = 1;
            sprintf(str, "m%d", i);
            if (isparam(str) == 0)
               missing = 1;
            sprintf(str, "y%d", i);
            if (isparam(str) == 0)
               missing = 1;
            sprintf(str, "h%d", i);
            if (isparam(str) == 0)
               missing = 1;
            sprintf(str, "n%d", i);
            if (isparam(str) == 0)
               missing = 1;
            if (missing)
               break;
         } else if ((attr_flags[i] & (AF_MULTI | AF_MUSERLIST | AF_MUSEREMAIL))) {
            for (j = 0; j < MAX_N_LIST; j++) {
               sprintf(str, "%s_%d", ua, j);
               if (isparam(str))
                  break;

               /* check for attributes without the _<i> from elog */
               if (isparam(ua))
                  break;
            }

            if (j == MAX_N_LIST) {
               missing = 1;
               break;
            }
         } else if (isparam(ua) == 0 || *getparam(ua) == 0) {
            missing = 1;
            break;
         }
      }
   }

   if (missing) {
      sprintf(error, "<i>");
      sprintf(error + strlen(error), loc("Error: Attribute <b>%s</b> not supplied"), attr_list[i]);
      sprintf(error + strlen(error), ".</i><p>\n");
      sprintf(error + strlen(error), loc("Please go back and enter the <b>%s</b> field"), attr_list[i]);
      strcat(error, ".\n");

      show_error(error);
      return;
   }

   /* check for numeric attributes */
   for (index = 0; index < lbs->n_attr; index++)
      if (attr_flags[index] & AF_NUMERIC) {
         strcpy(ua, attr_list[index]);
         stou(ua);
         strlcpy(str, isparam(ua) ? getparam(ua) : "", sizeof(str));

         for (j = 0; i < (int) strlen(str); i++)
            if (!isdigit(str[i]))
               break;

         sprintf(str2, "- %s -", loc("keep original values"));
         if (i < (int) strlen(str) && strcmp(str, "<keep>") != 0 && strcmp(str, str2) != 0) {
            sprintf(error, loc("Error: Attribute <b>%s</b> must be numeric"), attr_list[index]);
            show_error(error);
            return;
         }
      }

   /* check for extended attributs */
   if (isparam("condition")) {
      set_condition(getparam("condition"));

      /* rescan attributes */
      n_attr = scan_attributes(lbs->name);
   } else
      n_attr = lbs->n_attr;

   for (i = 0; i < n_attr; i++) {
      strcpy(ua, attr_list[i]);
      stou(ua);
      if (attr_flags[i] & (AF_MULTI | AF_MUSERLIST | AF_MUSEREMAIL))
         strcat(ua, "_0");

      if (isparam(ua) && *getparam(ua) && attr_options[i][0][0]) {

         if (strieq(attr_options[i][0], "boolean")) {
            if (atoi(getparam(ua)) != 0 && atoi(getparam(ua)) != 1 && strcmp(getparam(ua), "<keep>") != 0) {
               strencode2(str, getparam(ua), sizeof(str));
               sprintf(error, loc("Error: Value <b>%s</b> not allowed for boolean attributes"), str);
               show_error(error);
               return;
            }

         } else {

            /* check if option exists */
            for (j = 0; attr_options[i][j][0]; j++)
               if (strieq(attr_options[i][j], getparam(ua)))
                  break;

            /* check if option without {n} exists */
            if (!attr_options[i][j][0]) {
               for (j = 0; attr_options[i][j][0]; j++) {
                  strlcpy(str, attr_options[i][j], sizeof(str));
                  if (strchr(str, '{'))
                     *strchr(str, '{') = 0;
                  if (strieq(str, getparam(ua)))
                     break;
               }
            }

            if (!attr_options[i][j][0] && isparam(ua) && strcmp(getparam(ua), "<keep>") != 0) {
               if (attr_flags[i] & AF_EXTENDABLE) {

                  /* check if maximal number of options exceeded */
                  if (attr_options[i][MAX_N_LIST - 1][0]) {
                     strcpy(error, loc("Maximum number of attribute options exceeded"));
                     strcat(error, "<br>");
                     strcat(error, loc("Please increase MAX_N_LIST in elogd.c and recompile"));
                     show_error(error);
                     return;
                  }

                  if (!add_attribute_option(lbs, attr_list[i], getparam(ua), getparam("condition")))
                     return;
               } else {
                  char encoded[100];
                  strencode2(encoded, getparam(ua), sizeof(encoded));
                  sprintf(error, loc("Error: Attribute option <b>%s</b> not existing"), encoded);
                  show_error(error);
                  return;
               }
            }
         }
      }
   }

   /* check if allowed encoding */
   if (getcfg(lbs->name, "Allowed encoding", str, sizeof(str)))
      allowed_encoding = atoi(str);
   else
      allowed_encoding = 7;

   strcpy(encoding, isparam("encoding") ? getparam("encoding") : "plain");

   if (strieq(encoding, "plain") && (allowed_encoding & 1) == 0) {
      show_error("Plain encoding not allowed");
      return;
   }
   if (strieq(encoding, "ELCode") && (allowed_encoding & 2) == 0) {
      show_error("ELCode encoding not allowed");
      return;
   }
   if (strieq(encoding, "HTML") && (allowed_encoding & 4) == 0) {
      show_error("HTML encoding not allowed");
      return;
   }

   /* get attachments */
   for (i = 0; i < MAX_ATTACHMENTS; i++) {
      sprintf(str, "attachment%d", i);
      strcpy(att_file[i], isparam(str) ? getparam(str) : "");
   }

   /* retrieve attributes */
   for (i = 0; i < n_attr; i++) {

      strcpy(ua, attr_list[i]);
      stou(ua);

      if (strieq(attr_options[i][0], "boolean") && !isparam(ua)) {

         strcpy(attrib[i], "0");

      } else if (attr_flags[i] & (AF_MULTI | AF_MUSERLIST | AF_MUSEREMAIL)) {

         if (isparam(ua)) {
            strlcpy(attrib[i], getparam(ua), NAME_LENGTH);
         } else {
            attrib[i][0] = 0;
            first = 1;
            for (j = 0; j < MAX_N_LIST; j++) {
               sprintf(str, "%s_%d", ua, j);
               if (isparam(str)) {
                  if (first)
                     first = 0;
                  else
                     strlcat(attrib[i], " | ", NAME_LENGTH);
                  if (strlen(attrib[i]) + strlen(getparam(str)) < NAME_LENGTH - 2)
                     strlcat(attrib[i], getparam(str), NAME_LENGTH);
                  else
                     break;
               }
            }
         }
      } else if (attr_flags[i] & AF_DATE) {

         if (isparam(ua))       // from edit/reply of fixed attributes
            strlcpy(attrib[i], getparam(ua), NAME_LENGTH);
         else {

            sprintf(str, "m%d", i);
            if (isparam(str) && strieq(getparam(str), "<keep>"))
               strcpy(attrib[i], "<keep>");
            else {
               sprintf(str, "y%d", i);
               year = isparam(str) ? atoi(getparam(str)) : 0;
               if (year < 100)
                  year += 2000;

               sprintf(str, "m%d", i);
               month = isparam(str) ? atoi(getparam(str)) : 0;

               sprintf(str, "d%d", i);
               day = isparam(str) ? atoi(getparam(str)) : 0;

               if (month == 0 || day == 0)
                  strcpy(attrib[i], "");
               else {
                  memset(&tms, 0, sizeof(struct tm));
                  tms.tm_year = year - 1900;
                  tms.tm_mon = month - 1;
                  tms.tm_mday = day;
                  tms.tm_hour = 12;

                  ltime = (int) mktime(&tms);

                  if (ltime == -1) {
                     show_error(loc("Date must be between 1970 and 2037"));
                     return;
                  }
                  sprintf(attrib[i], "%d", ltime);
               }
            }
         }

      } else if (attr_flags[i] & AF_DATETIME) {

         if (isparam(ua))       // from edit/reply of fixed attributes
            strlcpy(attrib[i], getparam(ua), NAME_LENGTH);
         else {

            sprintf(str, "m%d", i);
            if (isparam(str) && strieq(getparam(str), "<keep>"))
               strcpy(attrib[i], "<keep>");
            else {
               sprintf(str, "y%d", i);
               year = isparam(str) ? atoi(getparam(str)) : 0;
               if (year < 100)
                  year += 2000;

               sprintf(str, "m%d", i);
               month = isparam(str) ? atoi(getparam(str)) : 0;

               sprintf(str, "d%d", i);
               day = isparam(str) ? atoi(getparam(str)) : 0;

               sprintf(str, "h%d", i);
               hour = isparam(str) ? atoi(getparam(str)) : 0;

               sprintf(str, "n%d", i);
               min = isparam(str) ? atoi(getparam(str)) : 0;

               sprintf(str, "c%d", i);
               sec = isparam(str) ? atoi(getparam(str)) : 0;

               if (month == 0 || day == 0)
                  strcpy(attrib[i], "");
               else {
                  memset(&tms, 0, sizeof(struct tm));
                  tms.tm_year = year - 1900;
                  tms.tm_mon = month - 1;
                  tms.tm_mday = day;
                  tms.tm_hour = hour;
                  tms.tm_min = min;
                  tms.tm_sec = sec;
                  tms.tm_isdst = -1;

                  ltime = (int) mktime(&tms);

                  if (ltime == -1) {
                     show_error(loc("Date must be between 1970 and 2037"));
                     return;
                  }
                  sprintf(attrib[i], "%d", ltime);
               }
            }
         }

      } else {
         strlcpy(attrib[i], isparam(ua) ? getparam(ua) : "", NAME_LENGTH);

         /* remove any CR/LF */
         if (strchr(attrib[i], '\r'))
            *strchr(attrib[i], '\r') = 0;
         if (strchr(attrib[i], '\n'))
            *strchr(attrib[i], '\n') = 0;

         /* strip trailing "{...}" */
         if (is_cond_attr(i) && strchr(attrib[i], '{') && strchr(attrib[i], '}'))
            *strchr(attrib[i], '{') = 0;
      }

   }

   /* compile substitution list */
   n = build_subst_list(lbs, slist, svalue, attrib, TRUE);
   if (isparam("edit_id") && atoi(getparam("edit_id")))
      add_subst_list(slist, svalue, "message id", getparam("edit_id"), &n);

   /* substitute attributes */
   for (i = 0; i < n_attr; i++) {
      if (!isparam("edit_id")) {
         sprintf(str, "Subst %s", attr_list[i]);
         if (getcfg(lbs->name, str, subst_str, sizeof(subst_str))) {
            strsubst_list(subst_str, sizeof(subst_str), slist, svalue, n);

            /* check for index substitution if not in edit mode */
            if (!bedit && strchr(subst_str, '#')) {
               /* get index */
               get_auto_index(lbs, i, subst_str, str, sizeof(str));
               strcpy(subst_str, str);
            }

            strcpy(attrib[i], subst_str);
         }
      }
   }

   /* check for attributes to keep */
   if (bmultiedit) {
      sprintf(str, "- %s -", loc("keep original values"));
      for (i = 0; i < n_attr; i++) {
         if (strieq(str, attrib[i]))
            strlcpy(attrib[i], "<keep>", NAME_LENGTH);
      }
   }

   message_id = 0;
   reply_to[0] = 0;
   in_reply_to[0] = 0;
   date[0] = 0;
   resubmit_orig = 0;
   locked_by[0] = 0;

   if (isparam("edit_id") && isparam("resubmit") && atoi(getparam("resubmit")) == 1) {
      resubmit_orig = atoi(getparam("edit_id"));

      /* get old links */
      el_retrieve(lbs, resubmit_orig, NULL, NULL, NULL, 0, NULL, 0, in_reply_to, reply_to, NULL, NULL, NULL);

      /* if not message head, move all preceeding messages */
      /* outcommented, users want only resubmitted message occur at end (see what's new)
         if (in_reply_to[0])
         {
         do
         {
         resubmit_orig = atoi(in_reply_to);
         el_retrieve(lbs, resubmit_orig, NULL, NULL, NULL, 0,
         NULL, 0, in_reply_to, reply_to, NULL, NULL);
         } while (in_reply_to[0]);
         }
       */

      message_id = atoi(getparam("edit_id"));
      strcpy(in_reply_to, "<keep>");
      strcpy(reply_to, "<keep>");
      date[0] = 0;
   } else {
      if (isparam("edit_id")) {
         message_id = atoi(getparam("edit_id"));
         strcpy(in_reply_to, "<keep>");
         strcpy(reply_to, "<keep>");
         strcpy(date, "<keep>");
      } else
         strcpy(in_reply_to, isparam("reply_to") ? getparam("reply_to") : "");
   }

   if (_logging_level > 1) {
      if (bmultiedit)
         sprintf(str, "EDIT multiple entries");
      else if (isparam("edit_id"))
         sprintf(str, "EDIT entry #%d", message_id);
      else
         sprintf(str, "NEW entry #%d", message_id);

      write_logfile(lbs, str);
   }

   /* check if lock has been stolen */
   if (!isparam("skiplock")) {
      if (getcfg(lbs->name, "Use Lock", str, sizeof(str)) && atoi(str) == 1 && message_id) {
         el_retrieve(lbs, message_id, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL, NULL, NULL, locked_by);

         if (isparam("full_name"))
            strlcpy(str, getparam("full_name"), sizeof(str));
         else
            strlcpy(str, loc("user"), sizeof(str));

         strcat(str, " ");
         strcat(str, loc("on"));
         strcat(str, " ");
         strcat(str, rem_host);

         if (locked_by[0] == 0 || strcmp(locked_by, str) != 0) {
            if (locked_by[0])
               sprintf(str, loc("This entry has in meantime been locked by %s"), locked_by);
            else
               sprintf(str, loc("This entry has in meantime been modified by someone else"));
            strlcat(str, ".<p>\n", sizeof(str));
            strlcat(str,
                    loc
                    ("Submitting it now would overwrite the other modification and is therefore prohibited"),
                    sizeof(str));
            strlcat(str, ".", sizeof(str));

            strencode2(str2, str, sizeof(str2));
            show_error(str2);
            return;
         }
      }
   }

   if (bmultiedit) {
      for (i = n = 0; i < atoi(getparam("nsel")); i++) {
         sprintf(str, "s%d", i);
         if (isparam(str)) {

            message_id = atoi(getparam(str));

            status = submit_elog_reply(lbs, message_id, attrib, getparam("text"));
            if (status != EL_SUCCESS) {
               sprintf(str, loc("New entry cannot be written to directory \"%s\""), lbs->data_dir);
               strcat(str, "\n<p>");
               strcat(str,
                      loc("Please check that it exists and elogd has write access and disk is not full"));
               show_error(str);
               return;
            }
         }
      }

      redirect(lbs, isparam("redir") ? getparam("redir") : "");
      return;                   /* no email notifications etc */
   } else {
      message_id = el_submit(lbs, message_id, bedit, date, attr_list, attrib, n_attr, getparam("text"),
                             in_reply_to, reply_to, encoding, att_file, TRUE, NULL);

      if (message_id <= 0) {
         sprintf(str, loc("New entry cannot be written to directory \"%s\""), lbs->data_dir);
         strcat(str, "\n<p>");
         strcat(str, loc("Please check that it exists and elogd has write access and disk is not full"));
         show_error(str);
         return;
      }
   }

   /* resubmit thread if requested */
   if (resubmit_orig)
      message_id = el_move_message_thread(lbs, resubmit_orig);

   /* retrieve submission date */
   if (date[0] == 0)
      el_retrieve(lbs, message_id, date, NULL, NULL, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL);

   /*---- replace relative elog:/x link by elog:n/x link */
   if (stristr(getparam("text"), "elog:/")) {
      p = getparam("text");
      if (stricmp(encoding, "HTML") == 0) {
         sprintf(str, "%d/", message_id);
      } else
         sprintf(str, "elog:%d/", message_id);
      strsubst(p, TEXT_SIZE, "elog:/", str);
      el_submit(lbs, message_id, TRUE, date, attr_list, attrib, n_attr, p, in_reply_to, reply_to, encoding,
                att_file, TRUE, NULL);
   }

   /*---- replace elog: by HTML link ----*/
   if (strieq(encoding, "HTML") && stristr(getparam("text"), "elog:")) {
      p = stristr(getparam("text"), "elog:");
      while (p) {
         for (i = 0; i < 5 || (p[i] == '/' || isalnum(p[i])); i++)
            str[i] = p[i];
         str[i] = 0;
         convert_elog_link(lbs, str + 5, str + 5, str2, 0, message_id);
         strsubst(p, TEXT_SIZE, str, str2);
         p += strlen(str2);
         p = stristr(p, "elog:");
      }
      el_submit(lbs, message_id, TRUE, date, attr_list, attrib, n_attr, getparam("text"), in_reply_to,
                reply_to, encoding, att_file, TRUE, NULL);
   }

   /*---- email notifications ----*/

   suppress = isparam("suppress") ? atoi(getparam("suppress")) : 0;

   /* check for mail submissions */
   mail_param[0] = 0;
   mail_to = xmalloc(256);
   mail_to[0] = 0;
   mail_to_size = 256;
   rcpt_to = xmalloc(256);
   rcpt_to[0] = 0;
   rcpt_to_size = 256;
   mail_list = xmalloc(200 * NAME_LENGTH);
   rcpt_list = xmalloc(200 * NAME_LENGTH);

   if (suppress == 1 || suppress == 3) {
      if (suppress == 1)
         strcpy(mail_param, "?suppress=1");
   } else {
      /* go throuch "Email xxx" in configuration file */
      for (index = mindex = 0; index <= n_attr; index++) {

         strcpy(ua, attr_list[index]);
         stou(ua);

         if (index < n_attr) {
            strcpy(str, "Email ");
            if (strchr(attr_list[index], ' '))
               sprintf(str + strlen(str), "\"%s\"", attr_list[index]);
            else
               strlcat(str, attr_list[index], sizeof(str));
            strcat(str, " ");

            if (attr_flags[index] & (AF_MULTI | AF_MUSERLIST | AF_MUSEREMAIL)) {
               sprintf(str2, "%s_%d", ua, mindex);

               mindex++;
               if (mindex == MAX_N_LIST)
                  mindex = 0;
               else
                  index--;      /* repeat this loop */
            } else
               strlcpy(str2, ua, sizeof(str));

            if (isparam(str2)) {
               if (strchr(getparam(str2), ' '))
                  sprintf(str + strlen(str), "\"%s\"", getparam(str2));
               else
                  strlcat(str, getparam(str2), sizeof(str));
            }
         } else
            sprintf(str, "Email ALL");

         if (getcfg(lbs->name, str, list, sizeof(list))) {
            i = build_subst_list(lbs, slist, svalue, attrib, TRUE);
            strsubst_list(list, sizeof(list), slist, svalue, i);

            n = strbreak(list, (char (*)[1500]) mail_list, 200, ",", FALSE);

            if (verbose)
               eprintf("\n%s to %s\n\n", str, list);

            for (i = 0; i < n; i++) {
               /* remove possible 'mailto:' */
               if ((p = strstr(&mail_list[i * NAME_LENGTH], "mailto:")) != NULL)
                  strcpy(p, p + 7);

               if ((int) strlen(mail_to) + (int) strlen(&mail_list[i * NAME_LENGTH]) + 10 >= mail_to_size) {
                  mail_to_size += 256;
                  mail_to = xrealloc(mail_to, mail_to_size);
               }
               strcat(mail_to, &mail_list[i * NAME_LENGTH]);
               strcat(mail_to, ",");

               if ((int) strlen(rcpt_to) + (int) strlen(&mail_list[i * NAME_LENGTH]) + 10 >= rcpt_to_size) {
                  rcpt_to_size += 256;
                  rcpt_to = xrealloc(rcpt_to, rcpt_to_size);
               }
               strcat(rcpt_to, &mail_list[i * NAME_LENGTH]);
               strcat(rcpt_to, ",");
            }
         }
      }

      if (!getcfg(lbs->name, "Suppress Email to users", str, sizeof(str)) || atoi(str) == 0) {
         /* go through password file */
         for (index = 0;; index++) {
            if (!enum_user_line(lbs, index, user, sizeof(user)))
               break;

            get_user_line(lbs, user, NULL, full_name, user_email, email_notify, NULL);

            for (i = 0; lb_list[i].name[0] && i < 1000; i++)
               if (strieq(lb_list[i].name, lbs->name))
                  break;

            if (email_notify[i]) {
               /* check if user has access to this logbook */
               if (!check_login_user(lbs, user))
                  continue;

               sprintf(str, "\"%s\" <%s>,", full_name, user_email);
               if ((int) strlen(mail_to) + (int) strlen(str) + 1 >= mail_to_size) {
                  mail_to_size += 256;
                  mail_to = xrealloc(mail_to, mail_to_size);
               }
               strcat(mail_to, str);

               sprintf(str, "%s,", user_email);
               if ((int) strlen(rcpt_to) + (int) strlen(str) + 1 >= rcpt_to_size) {
                  rcpt_to_size += 256;
                  rcpt_to = xrealloc(rcpt_to, rcpt_to_size);
               }
               strcat(rcpt_to, str);
            }
         }
      }
   }

   if (strlen(mail_to) > 0) {
      /* convert any '|' to ',', remove duplicate email to's */
      strbreak(rcpt_to, (void *) rcpt_list, 200, ",|", TRUE);
      strbreak(mail_to, (void *) mail_list, 200, ",|", TRUE);
      for (i = 0; i < 200 && rcpt_list[i * NAME_LENGTH]; i++) {
         for (j = i + 1; j < 200 && rcpt_list[j * NAME_LENGTH]; j++) {
            if (strstr(&rcpt_list[j * NAME_LENGTH], &rcpt_list[i * NAME_LENGTH])) {
               for (k = i; k < 199 && rcpt_list[k * NAME_LENGTH]; k++) {
                  memcpy(&rcpt_list[k * NAME_LENGTH], &rcpt_list[(k + 1) * NAME_LENGTH], NAME_LENGTH);
                  memcpy(&mail_list[k * NAME_LENGTH], &mail_list[(k + 1) * NAME_LENGTH], NAME_LENGTH);
               }
               i = i - 1;
               break;
            }
         }
      }
      rcpt_to[0] = 0;
      mail_to[0] = 0;
      for (i = 0; i < 200 && rcpt_list[i * NAME_LENGTH]; i++) {

         if ((int) strlen(rcpt_to) + (int) strlen(&rcpt_list[i * NAME_LENGTH]) + 5 >= rcpt_to_size) {
            rcpt_to_size += 256;
            rcpt_to = xrealloc(rcpt_to, rcpt_to_size);
         }
         strcat(rcpt_to, &rcpt_list[i * NAME_LENGTH]);

         if ((int) strlen(mail_to) + (int) strlen(&mail_list[i * NAME_LENGTH]) + 5 >= mail_to_size) {
            mail_to_size += 256;
            mail_to = xrealloc(mail_to, mail_to_size);
         }
         strcat(mail_to, &mail_list[i * NAME_LENGTH]);

         if (i < 199 && rcpt_list[(i + 1) * NAME_LENGTH]) {
            strcat(rcpt_to, ",");
            strcat(mail_to, ",\r\n\t");
         }
      }

      if (compose_email(lbs, rcpt_to, mail_to, message_id, attrib, mail_param, isparam("edit_id"), att_file,
                        isparam("encoding") ? getparam("encoding") : "plain", atoi(in_reply_to)) == 0) {
         xfree(mail_to);
         xfree(rcpt_to);
         xfree(mail_list);
         xfree(rcpt_list);
         return;
      }
   }

   xfree(mail_to);
   xfree(rcpt_to);
   xfree(mail_list);
   xfree(rcpt_list);

   /*---- shell execution ----*/

   if (!(isparam("shell_suppress") && atoi(getparam("shell_suppress")))) {
      if (!isparam("edit_id")) {
         if (getcfg(lbs->name, "Execute new", str, sizeof(str)))
            execute_shell(lbs, message_id, attrib, att_file, str);
      } else {
         if (getcfg(lbs->name, "Execute edit", str, sizeof(str)))
            execute_shell(lbs, message_id, attrib, att_file, str);
      }
   }

   /*---- custom submit page ----*/

   if (getcfg(lbs->name, "Submit page", str, sizeof(str))) {
      /* check if file starts with an absolute directory */
      if (str[0] == DIR_SEPARATOR || str[1] == ':')
         strcpy(file_name, str);
      else {
         strlcpy(file_name, logbook_dir, sizeof(file_name));
         strlcat(file_name, str, sizeof(file_name));
      }
      send_file_direct(file_name);
      return;
   }

   sprintf(str, "%d%s", message_id, mail_param);
   redirect(lbs, str);
}

/*------------------------------------------------------------------*/

void submit_elog_mirror(LOGBOOK * lbs)
{
   char str[1000], date[80], attrib_value[MAX_N_ATTR][NAME_LENGTH], attrib_name[MAX_N_ATTR][NAME_LENGTH],
       in_reply_to[80], encoding[80], reply_to[MAX_REPLY_TO * 10], att_file[MAX_ATTACHMENTS][256],
       name[NAME_LENGTH], value[NAME_LENGTH];
   int i, message_id, n_attr;
   BOOL bedit;

   /* get attachments */
   for (i = 0; i < MAX_ATTACHMENTS; i++) {
      sprintf(str, "attachment%d", i);
      strcpy(att_file[i], isparam(str) ? getparam(str) : "");
   }

   reply_to[0] = 0;
   in_reply_to[0] = 0;
   date[0] = 0;
   encoding[0] = 0;
   bedit = FALSE;
   n_attr = 0;
   message_id = 0;

   /* retrieve attributes */
   for (i = 0; i < MAX_PARAM; i++) {
      if (enumparam(i, name, value)) {

         if (strieq(name, "mirror_id"))
            message_id = atoi(value);
         else if (strieq(name, "entry_date"))
            strlcpy(date, value, sizeof(date));
         else if (strieq(name, "reply_to"))
            strlcpy(reply_to, value, sizeof(reply_to));
         else if (strieq(name, "in_reply_to"))
            strlcpy(in_reply_to, value, sizeof(in_reply_to));
         else if (strieq(name, "encoding"))
            strlcpy(encoding, value, sizeof(encoding));
         else if (!strieq(name, "cmd") && !strieq(name, "full_name") && !strieq(name, "user_email")
                  && !strieq(name, "unm") && !strieq(name, "upwd") && !strieq(name, "wpwd") && strncmp(name,
                                                                                                       "attachment",
                                                                                                       10) !=
                  0) {
            strlcpy(attrib_name[n_attr], name, NAME_LENGTH);
            strlcpy(attrib_value[n_attr++], value, NAME_LENGTH);
         }
      } else
         break;
   }

   if (message_id == 0 || date[0] == 0) {
      show_error(loc("Invalid mirror_id or entry_date"));
      return;
   }

   /* check if message already exists */
   for (i = 0; i < *lbs->n_el_index; i++)
      if (lbs->el_index[i].message_id == message_id) {
         bedit = TRUE;
         break;
      }

   message_id = el_submit(lbs, message_id, bedit, date, attrib_name, attrib_value, n_attr, getparam("text"),
                          in_reply_to, reply_to, encoding, att_file, FALSE, NULL);

   if (message_id <= 0) {
      sprintf(str, loc("New entry cannot be written to directory \"%s\""), lbs->data_dir);
      strcat(str, "\n<p>");
      strcat(str, loc("Please check that it exists and elogd has write access"));
      show_error(str);
      return;
   }

   sprintf(str, "%d", message_id);
   redirect(lbs, str);
}

/*------------------------------------------------------------------*/

void copy_to(LOGBOOK * lbs, int src_id, char *dest_logbook, int move, int orig_id)
{
   int size, i, n, n_done, n_done_reply, n_reply, index, status, fh, source_id, message_id;
   char str[256], file_name[MAX_PATH_LENGTH], attrib[MAX_N_ATTR][NAME_LENGTH];
   char date[80], *text, msg_str[32], in_reply_to[80], reply_to[MAX_REPLY_TO * 10],
       attachment[MAX_ATTACHMENTS][MAX_PATH_LENGTH], encoding[80], locked_by[256], *buffer,
       list[MAX_N_ATTR][NAME_LENGTH];
   LOGBOOK *lbs_dest;

   for (i = 0; lb_list[i].name[0]; i++)
      if (strieq(lb_list[i].name, dest_logbook))
         break;
   if (!lb_list[i].name[0])
      return;
   lbs_dest = &lb_list[i];

   if (src_id)
      n = 1;
   else
      n = isparam("nsel") ? atoi(getparam("nsel")) : 0;

   text = xmalloc(TEXT_SIZE);

   n_done = n_done_reply = source_id = status = 0;
   for (index = 0; index < n; index++) {
      if (src_id)
         source_id = src_id;
      else {
         sprintf(str, "s%d", index);
         if (!isparam(str))
            continue;

         source_id = isparam(str) ? atoi(getparam(str)) : 0;
      }

      /* get message */
      size = TEXT_SIZE;
      status = el_retrieve(lbs, source_id, date, attr_list, attrib, lbs->n_attr, text, &size, in_reply_to,
                           reply_to, attachment, encoding, locked_by);

      if (status != EL_SUCCESS) {
         sprintf(msg_str, "%d", source_id);
         sprintf(str, loc("Entry %s cannot be read from logbook \"%s\""), msg_str, lbs->name);
         show_error(str);
         xfree(text);
         return;
      }

      if (orig_id == 0) {
         /* search message head */
         while (atoi(in_reply_to) > 0) {
            source_id = atoi(in_reply_to);
            size = TEXT_SIZE;
            status = el_retrieve(lbs, source_id, date, attr_list, attrib, lbs->n_attr, text, &size,
                                 in_reply_to, reply_to, attachment, encoding, locked_by);

            if (status != EL_SUCCESS) {
               sprintf(msg_str, "%d", source_id);
               sprintf(str, loc("Entry %s cannot be read from logbook \"%s\""), msg_str, lbs->name);
               show_error(str);
               xfree(text);
               return;
            }
         }
      }

      /* read attachments */
      for (i = 0; i < MAX_ATTACHMENTS; i++)
         if (attachment[i][0]) {
            strlcpy(file_name, lbs->data_dir, sizeof(file_name));
            strlcat(file_name, attachment[i], sizeof(file_name));

            fh = open(file_name, O_RDONLY | O_BINARY);
            if (fh > 0) {
               lseek(fh, 0, SEEK_END);
               size = TELL(fh);
               lseek(fh, 0, SEEK_SET);
               buffer = xmalloc(size);
               read(fh, buffer, size);
               close(fh);

               /* stip date/time from file name */
               strlcpy(file_name, attachment[i] + 14, NAME_LENGTH);

               el_submit_attachment(lbs_dest, file_name, buffer, size, attachment[i]);

               if (buffer)
                  xfree(buffer);
            } else
               /* attachment is invalid */
               attachment[i][0] = 0;
         }

      /* if called recursively (for threads), put in correct in_reply_to */
      str[0] = 0;
      if (orig_id)
         sprintf(str, "%d", orig_id);

      /* submit in destination logbook without links, submit all attributes from
         the source logbook even if the destination has a differnt number of attributes */

      message_id = el_submit(lbs_dest, 0, FALSE, date, attr_list, attrib, lbs->n_attr, text, str, "",
                             encoding, attachment, TRUE, NULL);

      if (message_id <= 0) {
         sprintf(str, loc("New entry cannot be written to directory \"%s\""), lbs_dest->data_dir);
         strcat(str, "\n<p>");
         strcat(str, loc("Please check that it exists and elogd has write access"));
         show_error(str);
         xfree(text);
         return;
      }

      n_done++;

      /* submit all replies */
      n_reply = strbreak(reply_to, list, MAX_N_ATTR, ",", FALSE);
      for (i = 0; i < n_reply; i++) {
         copy_to(lbs, atoi(list[i]), dest_logbook, move, message_id);
      }

      n_done_reply += n_reply;

      /* delete original message for move */
      if (move && orig_id == 0) {
         el_delete_message(lbs, source_id, TRUE, NULL, TRUE, TRUE);

         /* check if this was the last message */
         source_id = el_search_message(lbs, EL_NEXT, source_id, FALSE);

         /* if yes, force display of new last message */
         if (source_id <= 0)
            source_id = el_search_message(lbs, EL_LAST, 0, FALSE);
      }
   }

   xfree(text);

   if (orig_id)
      return;

   /* redirect to next entry of source logbook */
   if (source_id)
      sprintf(str, "%d", source_id);
   else
      str[0] = 0;
   redirect(lbs, str);
   return;
}

/*------------------------------------------------------------------*/

int is_inline_attachment(char *encoding, int message_id, char *text, int i, char *att)
{
   char str[256], att_enc[256];

   if (text == NULL)
      return 0;
   if (strieq(encoding, "ELCode")) {
      sprintf(str, "[img]elog:/%d[/img]", i + 1);
      if (stristr(text, str))
         return 1;
      sprintf(str, "[img]elog:%d/%d[/img]", message_id, i + 1);
      if (stristr(text, str))
         return 1;
   } else if (strieq(encoding, "HTML")) {
      strlcpy(att_enc, att, sizeof(att_enc));
      att_enc[13] = '/';
      if (stristr(text, att_enc))
         return 1;
      sprintf(str, "cid:att%d@psi.ch", i);
      if (stristr(text, str))
         return 1;
   }

   return 0;
}

/*------------------------------------------------------------------*/

int create_thumbnail(LOGBOOK * lbs, char *file_name)
{
   char str[2 * MAX_PATH_LENGTH], cmd[2 * MAX_PATH_LENGTH], thumb_size[256];
   int i;

   if (!image_magick_exist)
      return 0;

   if (getcfg(lbs->name, "Thumbnail size", str, sizeof(str)))
      sprintf(thumb_size, " -thumbnail '%s'", str);
   else
      thumb_size[0] = 0;

   if (!chkext(file_name, ".ps") && !chkext(file_name, ".pdf") && !chkext(file_name, ".eps")
       && !chkext(file_name, ".gif") && !chkext(file_name, ".jpg") && !chkext(file_name, ".jpeg")
       && !chkext(file_name, ".png") && !chkext(file_name, ".ico") && !chkext(file_name, ".tif"))
      return 0;

   i = get_thumb_name(file_name, str, sizeof(str), 0);
   if (i)
      return i;

   strlcpy(str, file_name, sizeof(str));
   if (chkext(file_name, ".pdf") || chkext(file_name, ".ps")) {
      if (strrchr(str, '.'))
         *strrchr(str, '.') = 0;
   }
   if (chkext(file_name, ".pdf") || chkext(file_name, ".ps"))
      strlcat(str, "-%d.png", sizeof(str));
   else
      strlcat(str, ".png", sizeof(str));

   if (chkext(file_name, ".pdf") || chkext(file_name, ".ps"))
      snprintf(cmd, sizeof(cmd), "convert '%s[0-7]'%s '%s'", file_name, thumb_size, str);
   else
      snprintf(cmd, sizeof(cmd), "convert '%s'%s '%s'", file_name, thumb_size, str);

#ifdef OS_WINNT
   for (i = 0; i < (int) strlen(cmd); i++)
      if (cmd[i] == '\'')
         cmd[i] = '\"';
#endif

   snprintf(str, sizeof(str), "SHELL \"%s\"", cmd);
   write_logfile(lbs, str);
   if (verbose) {
      eprintf(str);
      eprintf("\n");
   }

   my_shell(cmd, str, sizeof(str));

   i = get_thumb_name(file_name, str, sizeof(str), 0);
   if (i)
      return i;

   return 3;
}

/*------------------------------------------------------------------*/

int get_thumb_name(const char *file_name, char *thumb_name, int size, int index)
{
   char str[MAX_PATH_LENGTH];

   thumb_name[0] = 0;

   /* append .png for all files as thumbnail name, except for PDF files (convert bug!) */
   memset(str, 0, sizeof(str));
   if (chkext(file_name, ".pdf") || chkext(file_name, ".ps")) {
      strlcpy(str, file_name, sizeof(str));
      if (strrchr(str, '.'))
         *strrchr(str, '.') = 0;
      snprintf(str + strlen(str), sizeof(str) - strlen(str) - 1, "-%d.png", index);
      if (file_exist(str)) {
         strlcpy(thumb_name, str, size);
         return 2;
      }

      if (index > 0)
         return 0;

      strlcpy(str, file_name, sizeof(str));
      if (strrchr(str, '.'))
         *strrchr(str, '.') = 0;
      strlcat(str, ".png", sizeof(str));
      if (file_exist(str)) {
         strlcpy(thumb_name, str, size);
         return 1;
      }
   } else {
      strlcpy(str, file_name, sizeof(str));
      sprintf(str + strlen(str), "-%d.png", index);
      if (file_exist(str)) {
         strlcpy(thumb_name, str, size);
         return 2;
      }

      if (index > 0)
         return 0;

      strlcpy(str, file_name, sizeof(str));
      strlcat(str, ".png", sizeof(str));
      if (file_exist(str)) {
         strlcpy(thumb_name, str, size);
         return 1;
      }
   }

   return 0;
}

/*------------------------------------------------------------------*/

void call_image_magick(LOGBOOK * lbs)
{
   char str[256], cmd[256], file_name[256], thumb_name[256];
   int cur_width, cur_height, new_size, cur_rot, new_rot, i, thumb_status;

   if (!isparam("req") || !isparam("img")) {
      show_error("Unknown IM request received");
      return;
   }

   strlcpy(file_name, lbs->data_dir, sizeof(file_name));
   strlcat(file_name, getparam("img"), sizeof(file_name));
   thumb_status = get_thumb_name(file_name, thumb_name, sizeof(thumb_name), 0);

   sprintf(cmd, "identify -format '%%wx%%h %%c' '%s'", thumb_name);
#ifdef OS_WINNT
   for (i = 0; i < (int) strlen(cmd); i++)
      if (cmd[i] == '\'')
         cmd[i] = '\"';
#endif
   my_shell(cmd, str, sizeof(str));

   if (atoi(str) > 0) {
      cur_width = atoi(str);
      if (strchr(str, 'x')) {
         cur_height = atoi(strchr(str, 'x') + 1);
      } else
         cur_height = cur_width;
      if (strchr(str, ' ')) {
         cur_rot = atoi(strchr(str, ' ') + 1);
      } else
         cur_rot = 0;
   } else {
      show_http_header(NULL, FALSE, NULL);
      rsputs(str);
      return;
   }

   if (thumb_status == 2)
      strsubst(thumb_name, sizeof(thumb_name), "-0", "-%d");

   i = cmd[0] = 0;
   if (strieq(getparam("req"), "rotleft")) {
      new_rot = (cur_rot + 360 - 90) % 360;
      sprintf(cmd, "convert '%s' -rotate %d -thumbnail %d -set comment ' %d' '%s'", file_name, new_rot,
              cur_height, new_rot, thumb_name);
   }

   if (strieq(getparam("req"), "rotright")) {
      new_rot = (cur_rot + 90) % 360;
      sprintf(cmd, "convert '%s' -rotate %d -thumbnail %d -set comment ' %d' '%s'", file_name, new_rot,
              cur_height, new_rot, thumb_name);
   }

   if (strieq(getparam("req"), "original")) {
      new_size = (int) (cur_width / 1.5);
      sprintf(cmd, "convert '%s' '%s'", file_name, thumb_name);
   }

   if (strieq(getparam("req"), "smaller")) {
      new_size = (int) (cur_width / 1.5);
      sprintf(cmd, "convert '%s' -rotate %d -thumbnail %d -set comment ' %d' '%s'", file_name, cur_rot,
              new_size, cur_rot, thumb_name);
   }

   if (strieq(getparam("req"), "larger")) {
      new_size = (int) (cur_width * 1.5);
      sprintf(cmd, "convert '%s' -rotate %d -thumbnail %d -set comment ' %d' '%s'", file_name, cur_rot,
              new_size, cur_rot, thumb_name);
   }

   if (cmd[0]) {
#ifdef OS_WINNT
      for (i = 0; i < (int) strlen(cmd); i++)
         if (cmd[i] == '\'')
            cmd[i] = '\"';
#endif
      my_shell(cmd, str, sizeof(str));
      show_http_header(NULL, TRUE, NULL);
      rsputs(str);
   }
   return;
}

/*------------------------------------------------------------------*/

void show_elog_entry(LOGBOOK * lbs, char *dec_path, char *command)
{
   int size, i, j, k, n, n_log, status, fh, length, message_error, index, n_hidden, message_id,
       orig_message_id, format_flags[MAX_N_ATTR], att_hide[MAX_ATTACHMENTS], att_inline[MAX_ATTACHMENTS],
       n_attachments, n_lines, n_disp_attr, attr_index[MAX_N_ATTR], thumb_status, max_n_lines;
   char str[2 * NAME_LENGTH], ref[256], file_enc[256], attrib[MAX_N_ATTR][NAME_LENGTH];
   char date[80], text[TEXT_SIZE], menu_str[1000], cmd[256], script[256], orig_tag[80],
       reply_tag[MAX_REPLY_TO * 10], display[NAME_LENGTH], attachment[MAX_ATTACHMENTS][MAX_PATH_LENGTH],
       encoding[80], locked_by[256], att[256], lattr[256], mid[80], menu_item[MAX_N_LIST][NAME_LENGTH],
       format[80], slist[MAX_N_ATTR + 10][NAME_LENGTH], file_name[MAX_PATH_LENGTH],
       gattr[MAX_N_ATTR][NAME_LENGTH], svalue[MAX_N_ATTR + 10][NAME_LENGTH], *p,
       lbk_list[MAX_N_LIST][NAME_LENGTH], comment[256], class_name[80], class_value[80],
       fl[8][NAME_LENGTH], list[MAX_N_ATTR][NAME_LENGTH], email_from[256];
   FILE *f;
   BOOL first, show_text, display_inline, subtable, email;
   struct tm *pts;
   struct tm ts;
   struct stat st;
   time_t ltime, entry_ltime;

   message_id = atoi(dec_path);
   message_error = EL_SUCCESS;
   _current_message_id = message_id;
   email = strieq(command, "email") || strieq(command, "oldemail");

   /* check for guest access */
   if (!getcfg(lbs->name, "Guest Menu commands", menu_str, sizeof(menu_str)) || isparam("unm") != 0)
      getcfg(lbs->name, "Menu commands", menu_str, sizeof(menu_str));

   /* default menu commands */
   if (menu_str[0] == 0) {
      strcpy(menu_str, "List, New, Edit, Delete, Reply, Duplicate, Find, ");

      if (getcfg(lbs->name, "Password file", str, sizeof(str))) {
         strcat(menu_str, "Config, Logout, ");
      } else {
         strcat(menu_str, "Config, ");
      }

      strcat(menu_str, "Help");
   } else {
      /* check for admin command */
      n = strbreak(menu_str, menu_item, MAX_N_LIST, ",", FALSE);
      menu_str[0] = 0;
      for (i = 0; i < n; i++) {
         if (strcmp(menu_item[i], "Admin") == 0) {
            if (!is_admin_user(lbs->name, getparam("unm")))
               continue;
         }
         strcat(menu_str, menu_item[i]);
         if (i < n - 1)
            strcat(menu_str, ", ");
      }
   }

   /*---- check next/previous message -------------------------------*/

   if (strieq(command, loc("Next")) || strieq(command, loc("Previous")) || strieq(command, loc("Last"))
       || strieq(command, loc("First"))) {
      orig_message_id = message_id;

      if (strieq(command, loc("Last")))
         message_id = el_search_message(lbs, EL_LAST, 0, FALSE);

      if (strieq(command, loc("First")))
         message_id = el_search_message(lbs, EL_FIRST, 0, FALSE);

      /* avoid display of "invalid id '0'", if "start page = 0?cmd=Last" */
      if (!message_id)
         dec_path[0] = 0;

      first = TRUE;
      do {
         if (strieq(command, loc("Next")))
            message_id = el_search_message(lbs, EL_NEXT, message_id, FALSE);

         if (strieq(command, loc("Previous")))
            message_id = el_search_message(lbs, EL_PREV, message_id, FALSE);

         if (!first) {
            if (strieq(command, loc("First")))
               message_id = el_search_message(lbs, EL_NEXT, message_id, FALSE);

            if (strieq(command, loc("Last")))
               message_id = el_search_message(lbs, EL_PREV, message_id, FALSE);
         } else
            first = FALSE;

         if (message_id == 0) {
            if (strieq(command, loc("Next")))
               message_error = EL_LAST_MSG;
            else
               message_error = EL_FIRST_MSG;

            message_id = orig_message_id;
            break;
         }

         size = sizeof(text);
         el_retrieve(lbs, message_id, date, attr_list, attrib, lbs->n_attr, text, &size, orig_tag, reply_tag,
                     attachment, encoding, locked_by);

         /* check for locked attributes */
         for (i = 0; i < lbs->n_attr; i++) {
            sprintf(lattr, "l%s", attr_list[i]);
            if (isparam(lattr) == '1'
                && !(isparam(attr_list[i]) && strieq(getparam(attr_list[i]), attrib[i])))
               break;
         }
         if (i < lbs->n_attr)
            continue;

         /* check for attribute filter if not browsing */
         if (!isparam("browsing")) {
            for (i = 0; i < lbs->n_attr; i++) {
               if (isparam(attr_list[i]) && !(isparam(attr_list[i]) && strieq(getparam(attr_list[i]),
                                                                              attrib[i])))
                  break;
            }
            if (i < lbs->n_attr)
               continue;
         }

         sprintf(str, "%d", message_id);

         for (i = 0; i < lbs->n_attr; i++) {
            sprintf(lattr, "l%s", attr_list[i]);
            if (isparam(lattr) == '1') {
               if (strchr(str, '?') == NULL)
                  sprintf(str + strlen(str), "?%s=1", lattr);
               else
                  sprintf(str + strlen(str), "&amp;%s=1", lattr);
            }
         }

         redirect(lbs, str);
         return;

      } while (TRUE);
   }

   /*---- check for valid URL ---------------------------------------*/

   if (dec_path[0] && atoi(dec_path) == 0) {
      sprintf(str, "%s: <b>%s</b>", loc("Invalid URL"), dec_path);
      show_error(str);
      return;
   }

   /*---- get current message ---------------------------------------*/

   if (message_id == 0)
      message_id = el_search_message(lbs, EL_LAST, 0, FALSE);

   status = 0;
   reply_tag[0] = orig_tag[0] = 0;
   if (message_id) {
      size = sizeof(text);
      status = el_retrieve(lbs, message_id, date, attr_list, attrib, lbs->n_attr, text, &size, orig_tag,
                           reply_tag, attachment, encoding, locked_by);

      if (status != EL_SUCCESS)
         message_error = status;
      else {
         if (_logging_level > 2) {
            sprintf(str, "READ entry #%d", message_id);
            write_logfile(lbs, str);
         }
      }
   } else
      message_error = EL_EMPTY;

   /*---- check for conditional attribute ----*/

   evaluate_conditions(lbs, attrib);

   /*---- header ----*/

   /* header */
   if (status == EL_SUCCESS && message_error != EL_EMPTY) {
      str[0] = 0;

      if (getcfg(lbs->name, "Page Title", str, sizeof(str))) {
         i = build_subst_list(lbs, slist, svalue, attrib, TRUE);
         sprintf(mid, "%d", message_id);
         add_subst_list(slist, svalue, "message id", mid, &i);
         add_subst_time(lbs, slist, svalue, "entry time", date, &i, 0);
         strsubst_list(str, sizeof(str), slist, svalue, i);
         strip_html(str);
      } else
         strcpy(str, "ELOG");

      if (email) {
         /* show absolute link for CSS */
         show_html_header(lbs, FALSE, str, TRUE, FALSE, NULL, TRUE);
         rsprintf("<body>\n");
      } else {
         sprintf(ref, "%d", message_id);
         strlcpy(script, "OnLoad=\"document.onkeypress = browse\";", sizeof(script));
         if (str[0])
            show_standard_header(lbs, TRUE, str, ref, FALSE, NULL, script);
         else
            show_standard_header(lbs, TRUE, lbs->name, ref, FALSE, NULL, script);
      }
   } else
      show_standard_header(lbs, TRUE, "", "", FALSE, NULL, NULL);

   /*---- title ----*/

   if (email)
      rsprintf("<table class=\"frame\" cellpadding=\"0\" cellspacing=\"0\">\n");
   else
      show_standard_title(lbs->name, "", 0);

   /*---- menu buttons ----*/

   if (!email) {

      rsprintf("<tr><td class=\"menuframe\">\n");
      rsprintf("<table width=\"100%%\" border=\"0\" cellpadding=\"0\" cellspacing=\"0\">\n");
      rsprintf("<tr>\n");

      /*---- next/previous buttons ----*/

      if (!getcfg(lbs->name, "Enable browsing", str, sizeof(str)) || atoi(str) == 1) {
         rsprintf("<td class=\"menu1a\">\n");

         /* check if first.png exists, just put link there if not */
         strlcpy(file_name, resource_dir, sizeof(file_name));
         if (file_name[0] && file_name[strlen(file_name) - 1] != DIR_SEPARATOR)
            strlcat(file_name, DIR_SEPARATOR_STR, sizeof(file_name));
         strlcat(file_name, "themes", sizeof(file_name));
         strlcat(file_name, DIR_SEPARATOR_STR, sizeof(file_name));
         if (theme_name[0]) {
            strlcat(file_name, theme_name, sizeof(file_name));
            strlcat(file_name, DIR_SEPARATOR_STR, sizeof(file_name));
         }
         strlcat(file_name, "first.png", sizeof(file_name));
         if (stat(file_name, &st) >= 0) {
            rsprintf("<input type=image name=cmd_first alt=\"%s\"  title=\"%s\" src=\"first.png\">\n",
                     loc("First entry, Ctrl-Home"), loc("First entry, Ctrl-Home"));
            rsprintf("<input type=image name=cmd_previous alt=\"%s\"  title=\"%s\" src=\"previous.png\">\n",
                     loc("Previous entry, Ctrl-PgUp"), loc("Previous entry, Ctrl-PgUp"));
            rsprintf("<input type=image name=cmd_next alt=\"%s\" title=\"%s\" src=\"next.png\">\n",
                     loc("Next entry, Ctrl-PgDn"), loc("Next entry, Ctrl-PgDn"));
            rsprintf("<input type=image name=cmd_last alt=\"%s\" title=\"%s\" src=\"last.png\">\n",
                     loc("Last entry, Ctrl-End"), loc("Last entry, Ctrl-End"));
         } else {
            rsprintf("<a href=\"%d?cmd=%s\">|&lt;</a>&nbsp;\n", message_id, loc("First"));
            rsprintf("<a href=\"%d?cmd=%s\">&lt;</a>&nbsp;\n", message_id, loc("Previous"));
            rsprintf("<a href=\"%d?cmd=%s\">&gt;</a>&nbsp;\n", message_id, loc("Next"));
            rsprintf("<a href=\"%d?cmd=%s\">&gt;|</a>&nbsp;\n", message_id, loc("Last"));
         }

         rsprintf("</td>\n");
      }

      n = strbreak(menu_str, menu_item, MAX_N_LIST, ",", FALSE);

      rsprintf("<td class=\"menu1\">\n");

      for (i = 0; i < n; i++) {
         /* display menu item */
         strcpy(cmd, menu_item[i]);

         /* only display allowed commands */
         if (!is_user_allowed(lbs, cmd))
            continue;

         if (strieq(cmd, "Copy to") || strieq(cmd, "Move to")) {
            rsprintf("&nbsp;<input type=submit name=cmd value=\"%s\">\n", loc(cmd));
            if (strieq(cmd, "Copy to"))
               rsprintf("<select name=destc>\n");
            else
               rsprintf("<select name=destm>\n");

            if (getcfg(lbs->name, cmd, str, sizeof(str))) {
               n_log = strbreak(str, lbk_list, MAX_N_LIST, ",", FALSE);

               for (j = 0; j < n_log; j++)
                  rsprintf("<option value=\"%s\">%s\n", lbk_list[j], lbk_list[j]);
            } else {
               for (j = 0;; j++) {
                  if (!enumgrp(j, str))
                     break;

                  if (!is_logbook(str))
                     continue;

                  if (strieq(str, lbs->name))
                     continue;

                  rsprintf("<option value=\"%s\">%s\n", str, str);
               }
            }
            rsprintf("</select>\n");

            if (i < n - 1)
               rsprintf("&nbsp|\n");
         } else {
            strlcpy(str, loc(menu_item[i]), sizeof(str));
            url_encode(str, sizeof(str));

            if (strieq(menu_item[i], "list"))
               rsprintf("&nbsp;<a href=\".?id=%d\">%s</a>&nbsp;\n", message_id, loc(menu_item[i]));
            else
               rsprintf("&nbsp;<a href=\"%d?cmd=%s\">%s</a>&nbsp;\n", message_id, str, loc(menu_item[i]));

            if (i < n - 1)
               rsprintf("|\n");
            else
               rsprintf("\n");
         }
      }

      rsprintf("</td>\n\n");

      rsprintf("</table></td></tr>\n\n");

      /*---- menu text ----*/

      if (getcfg(lbs->name, "menu text", str, sizeof(str))) {
         FILE *f;
         char file_name[256], *buf;

         rsprintf("<tr><td class=\"menuframe\"><span class=\"menu1\">\n");

         /* check if file starts with an absolute directory */
         if (str[0] == DIR_SEPARATOR || str[1] == ':')
            strcpy(file_name, str);
         else {
            strlcpy(file_name, logbook_dir, sizeof(file_name));
            strlcat(file_name, str, sizeof(file_name));
         }

         f = fopen(file_name, "rb");
         if (f != NULL) {
            fseek(f, 0, SEEK_END);
            size = TELL(fileno(f));
            fseek(f, 0, SEEK_SET);

            buf = xmalloc(size + 1);
            fread(buf, 1, size, f);
            buf[size] = 0;
            fclose(f);

            rsputs(buf);

         } else
            rsprintf("<center><b>Error: file <i>\"%s\"</i> not found</b></center>", file_name);

         rsprintf("</span></td></tr>");
      }
   }                            // if (!email)

   /*---- message ----*/

   if (reply_tag[0] || orig_tag[0])
      show_elog_thread(lbs, message_id, email, 0);

   if (message_error == EL_EMPTY)
      rsprintf("<tr><td class=\"errormsg\" colspan=2>%s</td></tr>\n", loc("Logbook is empty"));
   else if (message_error == EL_NO_MSG)
      rsprintf("<tr><td class=\"errormsg\" colspan=2>%s</td></tr>\n", loc("This entry has been deleted"));
   else {
      /* overall message table */
      rsprintf("<tr><td><table class=\"listframe\" width=\"100%%\" cellspacing=\"0\" cellpadding=\"0\">\n");

      /* check for locked attributes */
      for (i = 0; i < lbs->n_attr; i++) {
         sprintf(lattr, "l%s", attr_list[i]);
         if (isparam(lattr) == '1')
            break;
      }
      if (i < lbs->n_attr) {
         if (isparam(attr_list[i]))
            sprintf(str, " %s <i>\"%s = %s\"</i>", loc("with"), attr_list[i], getparam(attr_list[i]));
      } else
         str[0] = 0;

      if (message_error == EL_LAST_MSG)
         rsprintf("<tr><td class=\"notifymsg\" colspan=2>%s %s</td></tr>\n", loc("This is the last entry"),
                  str);

      if (message_error == EL_FIRST_MSG)
         rsprintf("<tr><td class=\"notifymsg\" colspan=2>%s %s</td></tr>\n", loc("This is the first entry"),
                  str);

      /* check for mail submissions */
      if (isparam("suppress")) {
         rsprintf("<tr><td class=\"notifymsg\" colspan=2>%s</td></tr>\n",
                  loc("Email notification suppressed"));
      } else if (isparam("error")) {
         rsprintf("<tr><td class=\"errormsg\" colspan=2>%s</td></tr>\n", getparam("error"));
      } else {
         for (i = 0;; i++) {
            sprintf(str, "mail%d", i);
            if (isparam(str)) {
               if (i == 0)
                  rsprintf("<tr><td class=\"notifymsg\" colspan=2>");
               rsprintf("%s <b>%s</b><br>\n", loc("Email sent to"), getparam(str));
            } else
               break;
         }
         if (i > 0)
            rsprintf("</tr>\n");
      }

      /*---- display message ID ----*/

      _current_message_id = message_id;

      if (email) {
         rsprintf("<tr><td class=\"title1\">\n");
         if (strieq(command, "oldemail"))
            rsprintf("%s:", loc("An old ELOG entry has been updated"));
         else
            rsprintf("%s:", loc("A new ELOG entry has been submitted"));
         rsprintf("</td></tr>\n");
      }

      if (locked_by && locked_by[0]) {
         sprintf(str, "%s %s", loc("Entry is currently edited by"), locked_by);
         rsprintf
             ("<tr><td nowrap colspan=2 class=\"errormsg\"><img src=\"stop.png\" alt=\"%s\"  title=\"%s\">\n",
              loc("stop"), loc("stop"));
         rsprintf("%s<br>%s</td></tr>\n", str, loc("You can \"steal\" the lock by editing this entry"));
      }

      rsprintf("<tr><td class=\"attribhead\">\n");

      for (i = 0; i < lbs->n_attr; i++) {
         strencode2(str, attrib[i], sizeof(str));
         rsprintf("<input type=hidden name=\"%s\" value=\"%s\">\n", attr_list[i], str);
      }

      /* browsing flag to distinguish "/../<attr>=<value>" from browsing */
      rsprintf("<input type=hidden name=browsing value=1>\n");

      if (getcfg(lbs->name, "ID display", display, sizeof(display))) {
         j = build_subst_list(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, attrib,
                              TRUE);
         sprintf(str, "%d", message_id);
         add_subst_list((char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, "message id", str, &j);
         add_subst_time(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, "entry time",
                        date, &j, 0);

         strsubst_list(display, sizeof(display), (char (*)[NAME_LENGTH]) slist,
                       (char (*)[NAME_LENGTH]) svalue, j);

      } else
         sprintf(display, "%d", message_id);

      if (email) {
         compose_base_url(lbs, str, sizeof(str), TRUE);
         sprintf(str + strlen(str), "%d", message_id);
         rsprintf("%s:&nbsp;<b>%s</b>&nbsp;&nbsp;", loc("Logbook"), lbs->name);
         rsprintf("%s:&nbsp;<a href=\"%s\"><b>%d</b></a>", loc("Message ID"), str, message_id);
      } else
         rsprintf("%s:&nbsp;<b>%s</b>\n", loc("Message ID"), display);

      /*---- display date ----*/

      if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
         strcpy(format, DEFAULT_TIME_FORMAT);

      entry_ltime = date_to_ltime(date);
      pts = localtime(&entry_ltime);
      assert(pts);
      my_strftime(str, sizeof(str), format, pts);

      rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;%s:&nbsp;<b>%s</b>\n", loc("Entry time"), str);

      /*---- link to original message or reply ----*/

      if (message_error != EL_FILE_ERROR && (reply_tag[0] || orig_tag[0])) {

         if (orig_tag[0]) {
            if (email)
               compose_base_url(lbs, ref, sizeof(ref), TRUE);
            else
               ref[0] = 0;
            sprintf(ref + strlen(ref), "%s", orig_tag);
            rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;%s:&nbsp;", loc("In reply to"));
            rsprintf("<b><a href=\"%s\">%s</a></b>\n", ref, orig_tag);
         }
         if (reply_tag[0]) {
            rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;%s:&nbsp;<b>", loc("Reply to this"));

            p = strtok(reply_tag, ",");
            do {
               if (email)
                  compose_base_url(lbs, ref, sizeof(ref), TRUE);
               else
                  ref[0] = 0;
               sprintf(ref + strlen(ref), "%s", p);
               rsprintf("<a href=\"%s\">%s</a>\n", ref, p);

               p = strtok(NULL, ",");

               if (p)
                  rsprintf("&nbsp; \n");

            } while (p);

            rsprintf("</b>\n");
         }
      }

      rsprintf("</td></tr>\n");

      /*---- display attributes ----*/

      /* retrieve attribute flags */
      for (i = 0; i < lbs->n_attr; i++) {
         format_flags[i] = 0;
         sprintf(str, "Format %s", attr_list[i]);
         if (getcfg(lbs->name, str, format, sizeof(format))) {
            n = strbreak(format, fl, 8, ",", FALSE);
            if (n > 0)
               format_flags[i] = atoi(fl[0]);
         }
      }

      /* 2 column table for all attributes */
      rsprintf("<tr><td><table width=\"100%%\" cellpadding=\"0\" cellspacing=\"0\">");
      subtable = 0;

      /* generate list of attributes to show */
      if (getcfg(lbs->name, "Show attributes", str, sizeof(str))) {
         n_disp_attr = strbreak(str, list, MAX_N_ATTR, ",", FALSE);
         for (i = 0; i < n_disp_attr; i++) {
            for (j = 0; j < lbs->n_attr; j++)
               if (strieq(attr_list[j], list[i]))
                  break;
            if (!strieq(attr_list[j], list[i]))
               /* attribute not found */
               j = 0;
            attr_index[i] = j;
         }
      } else {
         for (i = 0; i < lbs->n_attr; i++)
            attr_index[i] = i;
         n_disp_attr = lbs->n_attr;
      }

      for (j = 0; j < n_disp_attr; j++) {

         i = attr_index[j];
         if (getcfg(lbs->name, "Password file", str, sizeof(str)) && getcfg(lbs->name, "Guest display", str,
                                                                            sizeof(str)) && !isparam("unm")) {

            n = strbreak(str, gattr, MAX_N_ATTR, ",", FALSE);
            for (k = 0; k < n; k++)
               if (strieq(gattr[k], attr_list[i]))
                  break;

            if (k == n)
               continue;
         }

         strcpy(class_name, "attribname");
         strcpy(class_value, "attribvalue");

         sprintf(str, "Format %s", attr_list[i]);
         if (getcfg(lbs->name, str, format, sizeof(format))) {
            n = strbreak(format, fl, 8, ",", FALSE);
            if (n > 1)
               strlcpy(class_name, fl[1], sizeof(class_name));
            if (n > 2)
               strlcpy(class_value, fl[2], sizeof(class_value));
         }

         if (format_flags[i] & AFF_SAME_LINE)
            /* if attribute on same line, do nothing */
            rsprintf("");
         else if (i < lbs->n_attr - 1 && (format_flags[i + 1] & AFF_SAME_LINE)) {
            /* if next attribute on same line, start a new subtable */
            rsprintf("<tr><td colspan=2><table width=\"100%%\" cellpadding=\"0\" cellspacing=\"0\"><tr>");
            subtable = 1;
         } else
            /* for normal attribute, start new row */
            rsprintf("<tr>");

         sprintf(lattr, "l%s", attr_list[i]);

         /* display cell with optional tooltip */
         sprintf(str, "Tooltip %s", attr_list[i]);
         if (getcfg(lbs->name, str, comment, sizeof(comment)))
            rsprintf("<td nowrap title=\"%s\" class=\"%s\">", comment, class_name);
         else
            rsprintf("<td nowrap class=\"%s\">", class_name);

         if (getcfg(lbs->name, "Filtered browsing", str, sizeof(str)) && atoi(str) == 1) {
            if (isparam(lattr) == '1')
               rsprintf("<input type=\"checkbox\" checked name=\"%s\" value=\"1\">&nbsp;", lattr);
            else
               rsprintf("<input alt=\"text\" title=\"text\"type=\"checkbox\" name=\"%s\" value=\"1\">&nbsp;",
                        lattr);
         }

         /* display checkbox for boolean attributes */
         if (strieq(attr_options[i][0], "boolean")) {
            if (atoi(attrib[i]) == 1)
               rsprintf("%s:</td><td class=\"%s\"><input type=checkbox checked disabled></td>\n",
                        attr_list[i], class_value);
            else
               rsprintf("%s:</td><td class=\"%s\"><input type=checkbox disabled></td>\n", attr_list[i],
                        class_value);
         }
         /* display image for icon */
         else if (attr_flags[i] & AF_ICON) {
            rsprintf("%s:</td><td class=\"%s\">\n", attr_list[i], class_value);
            if (attrib[i][0]) {
               sprintf(str, "Icon comment %s", attrib[i]);
               getcfg(lbs->name, str, comment, sizeof(comment));

               if (comment[0])
                  rsprintf("<img src=\"icons/%s\" alt=\"%s\" title=\"%s\">", attrib[i], comment, comment);
               else
                  rsprintf("<img src=\"icons/%s\" alt=\"%s\" title=\"%s\">", attrib[i], attrib[i], attrib[i]);
            }
            rsprintf("&nbsp;</td>\n");
         } else if ((attr_flags[i] & (AF_MULTI | AF_MUSERLIST | AF_MUSEREMAIL)) && (format_flags[i]
                                                                                    & AFF_MULTI_LINE)) {
            rsprintf("%s:</td><td class=\"%s\">\n", attr_list[i], class_value);

            /* separate options into individual lines */
            strlcpy(str, attrib[i], sizeof(str));
            p = strtok(str, "|");
            while (p) {
               while (*p == ' ')
                  p++;

               rsputs2(lbs, email, p);

               p = strtok(NULL, "|");

               if (p)
                  rsprintf("<br>");
            }

            rsprintf("</td>\n");
         } else if (attr_flags[i] & AF_DATE) {

            if (!getcfg(lbs->name, "Date format", format, sizeof(format)))
               strcpy(format, DEFAULT_DATE_FORMAT);

            ltime = atoi(attrib[i]);
            pts = localtime(&ltime);
            assert(pts);
            if (ltime == 0)
               strcpy(str, "-");
            else
               my_strftime(str, sizeof(str), format, pts);

            rsprintf("%s:</td><td class=\"%s\">%s&nbsp;</td>\n", attr_list[i], class_value, str);

         } else if (attr_flags[i] & AF_DATETIME) {

            if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
               strcpy(format, DEFAULT_TIME_FORMAT);

            ltime = atoi(attrib[i]);
            pts = localtime(&ltime);
            assert(pts);
            if (ltime == 0)
               strcpy(str, "-");
            else
               my_strftime(str, sizeof(str), format, pts);

            rsprintf("%s:</td><td class=\"%s\">%s&nbsp;</td>\n", attr_list[i], class_value, str);

         } else {
            rsprintf("%s:</td><td class=\"%s\">\n", attr_list[i], class_value);

            sprintf(str, "Change %s", attr_list[i]);
            if (getcfg(lbs->name, str, display, sizeof(display))) {
               k = build_subst_list(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue,
                                    attrib, TRUE);
               sprintf(str, "%d", message_id);
               add_subst_list((char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue, "message id",
                              str, &k);
               add_subst_time(lbs, (char (*)[NAME_LENGTH]) slist, (char (*)[NAME_LENGTH]) svalue,
                              "entry time", date, &k, attr_flags[i]);

               strsubst_list(display, sizeof(display), (char (*)[NAME_LENGTH]) slist,
                             (char (*)[NAME_LENGTH]) svalue, k);

            } else
               strcpy(display, attrib[i]);

            if (is_html(display))
               rsputs(display);
            else
               rsputs2(lbs, email, display);

            rsprintf("&nbsp;</td>\n");
         }

         if (i < lbs->n_attr - 1 && (format_flags[i + 1] & AFF_SAME_LINE) == 0) {
            /* if next attribute not on same line, close row or subtable */
            if (subtable) {
               rsprintf("</table></td></tr>\n");
               subtable = 0;
            } else
               rsprintf("</tr>");
         }

         /* if last attribute, close row or subtable */
         if (i == lbs->n_attr - 1) {
            if (subtable) {
               rsprintf("</table></td></tr>\n");
               subtable = 0;
            } else
               rsprintf("</tr>");
         }
      }

      rsputs("</table></td></tr>\n");   // 2 column table

      rsputs("</table><!-- listframe -->\n");

      rsputs("</td></tr>\n");

      /*---- message text ----*/

      show_text = !getcfg(lbs->name, "Show text", str, sizeof(str)) || atoi(str) == 1;

      if (getcfg(lbs->name, "Password file", str, sizeof(str)) && getcfg(lbs->name, "Guest display", str,
                                                                         sizeof(str)) && !isparam("unm")) {

         n = strbreak(str, gattr, MAX_N_ATTR, ",", FALSE);
         for (j = 0; j < n; j++)
            if (strieq(gattr[j], "text"))
               break;

         if (j == n)
            show_text = FALSE;
      }

      if (show_text) {
         rsprintf("<tr><td class=\"messageframe\">");

         if (strieq(encoding, "plain")) {
            rsputs("<pre class=\"messagepre\">");
            rsputs2(lbs, email, text);
            rsputs("</pre>");
         } else if (strieq(encoding, "ELCode"))
            rsputs_elcode(lbs, email, text);
         else {
            if (email)
               replace_inline_img(lbs, text);
            rsputs(text);
         }

         rsputs("</td></tr>\n");

         n_hidden = 0;
         for (i = 0, n_attachments = 0; i < MAX_ATTACHMENTS; i++) {
            att_inline[i] = 0;
            att_hide[i] = getcfg(lbs->name, "Show attachments", str, sizeof(str)) && atoi(str) == 0;

            if (is_inline_attachment(encoding, message_id, text, i, attachment[i]))
               att_inline[i] = 1;

            if (attachment[i][0])
               n_attachments++;
         }

         if (isparam("hide")) {
            strlcpy(str, getparam("hide"), sizeof(str));
            p = strtok(str, ",");
            while (p != NULL) {
               if (atoi(p) < MAX_ATTACHMENTS) {
                  att_hide[atoi(p)] = 1;
                  n_hidden++;
               }
               p = strtok(NULL, ",");
            }
         }
         if (isparam("show")) {
            strlcpy(str, getparam("show"), sizeof(str));
            p = strtok(str, ",");
            while (p != NULL) {
               if (atoi(p) < MAX_ATTACHMENTS) {
                  att_hide[atoi(p)] = 0;
               }
               p = strtok(NULL, ",");
            }
         }

         for (index = 0; index < MAX_ATTACHMENTS; index++) {
            if (attachment[index][0] && strlen(attachment[index]) > 14 && !att_inline[index]) {
               for (i = 0; i < (int) strlen(attachment[index]); i++)
                  att[i] = toupper(attachment[index][i]);
               att[i] = 0;

               /* determine size of attachment */
               strlcpy(file_name, lbs->data_dir, sizeof(file_name));
               strlcat(file_name, attachment[index], sizeof(file_name));
               thumb_status = create_thumbnail(lbs, file_name);

               length = 0;
               fh = open(file_name, O_RDONLY | O_BINARY);
               if (fh > 0) {
                  lseek(fh, 0, SEEK_END);
                  length = TELL(fh);
                  close(fh);
               }

               strlcpy(str, attachment[index], sizeof(str));
               str[13] = 0;
               strcpy(file_enc, attachment[index] + 14);
               url_encode(file_enc, sizeof(file_enc));  /* for file names with special characters like "+" */
               if (email) {
                  retrieve_email_from(lbs, email_from, NULL, NULL);
                  if (strchr(email_from, '@'))
                     p = strchr(email_from, '@') + 1;
                  else
                     p = email_from;
                  sprintf(ref, "cid:att%d@%s", index, p);
               } else
                  sprintf(ref, "%s/%s", str, file_enc);

               /* overall table */
               rsprintf("<tr><td><table class=\"listframe\" width=\"100%%\" cellspacing=0>\n");

               rsprintf("<tr><td nowrap width=\"10%%\" class=\"attribname\">%s %d:</td>\n",
                        loc("Attachment"), index + 1);

               if (email)
                  rsprintf("<td class=\"attribvalue\">%s\n", attachment[index] + 14);
               else
                  rsprintf("<td class=\"attribvalue\"><a href=\"%s\" target=\"_blank\">%s</a>\n", ref,
                           attachment[index] + 14);

               rsprintf("&nbsp;<span class=\"bytes\">");

               if (length < 1024)
                  rsprintf("%d Bytes", length);
               else if (length < 1024 * 1024)
                  rsprintf("%d kB", length / 1024);
               else
                  rsprintf("%1.3lf MB", length / 1024.0 / 1024.0);

               rsprintf("</span>\n");

               /* retrieve submission date */
               memset(&ts, 0, sizeof(ts));
               ts.tm_mon = (attachment[index][2] - '0') * 10 + attachment[index][3] - '0' - 1;
               ts.tm_mday = (attachment[index][4] - '0') * 10 + attachment[index][5] - '0';
               ts.tm_year = (attachment[index][0] - '0') * 10 + attachment[index][1] - '0';

               ts.tm_hour = (attachment[index][7] - '0') * 10 + attachment[index][8] - '0';
               ts.tm_min = (attachment[index][9] - '0') * 10 + attachment[index][10] - '0';
               ts.tm_sec = (attachment[index][11] - '0') * 10 + attachment[index][12] - '0';

               if (ts.tm_year < 90)
                  ts.tm_year += 100;
               ltime = mktime(&ts);

               /* show upload date/time only if different from entry date/time */
               if (abs((int) (ltime - entry_ltime)) > 3600) {
                  if (!getcfg(lbs->name, "Time format", format, sizeof(format)))
                     strcpy(format, DEFAULT_TIME_FORMAT);

                  my_strftime(str, sizeof(str), format, &ts);

                  rsprintf("&nbsp;<span class=\"uploaded\">");
                  rsprintf("Uploaded %s", str);
                  rsprintf("</span>\n");
               }

               /* determine if displayed inline */
               display_inline = is_image(file_name) || is_ascii(file_name);
               if (chkext(att, ".PS") || chkext(att, ".PDF") || chkext(att, ".EPS"))
                  display_inline = 0;
               if ((chkext(att, ".HTM") || chkext(att, ".HTML")) && is_full_html(file_name))
                  display_inline = 0;
               if (thumb_status)
                  display_inline = 1;

               if (display_inline) {
                  /* hide this / show this */
                  if (!email) {
                     rsprintf("<span class=\"bytes\">");

                     rsprintf("&nbsp;|&nbsp;");
                     if (att_hide[index]) {
                        rsprintf("<a href=\"%d?hide=", message_id);
                        for (i = 0; i < MAX_ATTACHMENTS; i++)
                           if (att_hide[i] && i != index) {
                              rsprintf("%d,", i);
                           }
                        rsprintf("&amp;show=%d", index);
                        rsprintf("\">%s</a>", loc("Show"));
                     } else {
                        rsprintf("<a href=\"%d?hide=", message_id);
                        for (i = 0; i < MAX_ATTACHMENTS; i++)
                           if (att_hide[i] || i == index) {
                              rsprintf("%d,", i);
                           }
                        rsprintf("\">%s</a>", loc("Hide"));
                     }

                     /* hide all */
                     if (n_hidden < n_attachments) {
                        rsprintf("&nbsp;|&nbsp;<a href=\"%d?hide=", message_id);
                        for (i = 0; i < MAX_ATTACHMENTS; i++)
                           if (attachment[i][0]) {
                              rsprintf("%d,", i);
                           }
                        rsprintf("\">%s</a>", loc("Hide all"));
                     }

                     /* show all */
                     if (n_hidden > 0) {
                        for (i = 0; i < MAX_ATTACHMENTS; i++)
                           if (att_hide[i])
                              break;
                        if (i < MAX_ATTACHMENTS) {
                           rsprintf("&nbsp;|&nbsp;<a href=\"%d?show=", message_id);
                           for (i = 0; i < MAX_ATTACHMENTS; i++)
                              if (att_hide[i])
                                 rsprintf("%d,", i);
                           rsprintf("\">%s</a>", loc("Show all"));
                        }
                     }

                     rsprintf("</span>\n");
                  }
               }

               rsprintf("</td></tr></table></td></tr>\n");
               strlcpy(file_name, lbs->data_dir, sizeof(file_name));
               strlcat(file_name, attachment[index], sizeof(file_name));

               if (!att_hide[index] && display_inline) {

                  if (thumb_status) {
                     rsprintf("<tr><td class=\"attachmentframe\">\n");

                     if (thumb_status == 3) {
                        rsprintf("<font color=red><b>%s</b></font>\n",
                                 loc("Cannot create thumbnail, please check ImageMagick installation"));
                     } else {
                        if (thumb_status == 2 && !email) {
                           for (i = 0;; i++) {
                              strlcpy(str, file_name, sizeof(str));
                              if (chkext(file_name, ".pdf") || chkext(file_name, ".ps"))
                                 if (strrchr(str, '.'))
                                    *strrchr(str, '.') = 0;
                              sprintf(str + strlen(str), "-%d.png", i);
                              if (file_exist(str)) {
                                 strlcpy(str, ref, sizeof(str));
                                 if (chkext(file_name, ".pdf") || chkext(file_name, ".ps"))
                                    if (strrchr(str, '.'))
                                       *strrchr(str, '.') = 0;
                                 sprintf(str + strlen(str), "-%d.png", i);
                                 rsprintf("<a name=\"att%d\" href=\"%s\">\n", index + 1, ref);
                                 rsprintf("<img src=\"%s\" alt=\"%s\" title=\"%s\"></a>\n", str,
                                          attachment[index] + 14, attachment[index] + 14);
                              } else
                                 break;
                           }
                        } else {
                           if (!email) {
                              rsprintf("<a name=\"att%d\" href=\"%s\">\n", index + 1, ref);
                              strlcpy(str, ref, sizeof(str));
                              if (chkext(file_name, ".pdf") || chkext(file_name, ".ps"))
                                 if (strrchr(str, '.'))
                                    *strrchr(str, '.') = 0;
                              strlcat(str, ".png", sizeof(str));
                              rsprintf("<img src=\"%s\" alt=\"%s\" title=\"%s\"></a>\n", str,
                                       attachment[index] + 14, attachment[index] + 14);
                           }
                        }
                     }
                     rsprintf("</td></tr>\n\n");
                  } else if (is_image(att)) {
                     if (!email) {
                        rsprintf("<tr><td class=\"attachmentframe\">\n");
                        rsprintf("<a name=\"att%d\"></a>\n", index + 1);
                        rsprintf("<img src=\"%s\" alt=\"%s\" title=\"%s\">\n", ref, attachment[index] + 14,
                                 attachment[index] + 14);
                        rsprintf("</td></tr>\n\n");
                     }
                  } else {
                     if (is_ascii(file_name)) {
                        /* display attachment */
                        rsprintf("<tr><td class=\"messageframe\">\n");

                        /* anchor for references */
                        rsprintf("<a name=\"att%d\"></a>\n", index + 1);

                        if (!chkext(att, ".HTML"))
                           rsprintf("<pre class=\"messagepre\">");

                        f = fopen(file_name, "rt");
                        n_lines = 0;
                        if (getcfg(lbs->name, "Attachment lines", str, sizeof(str)))
                           max_n_lines = atoi(str);
                        else
                           max_n_lines = 300;

                        if (f != NULL) {
                           while (!feof(f)) {
                              str[0] = 0;
                              fgets(str, sizeof(str), f);

                              if (n_lines < max_n_lines) {
                                 if (!chkext(att, ".HTML"))
                                    rsputs2(lbs, email, str);
                                 else
                                    rsputs(str);
                              }
                              n_lines++;
                           }
                           fclose(f);
                        }

                        if (!chkext(att, ".HTML"))
                           rsprintf("</pre>");
                        rsprintf("\n");
                        if (max_n_lines == 0)
                           rsprintf("<i><b>%d lines</b></i>\n", n_lines);
                        else if (n_lines > max_n_lines)
                           rsprintf("<i><b>... %d more lines ...</b></i>\n", n_lines - max_n_lines);

                        rsprintf("</td></tr>\n");
                     }
                  }
               }
            }
         }
      }
   }

   /* overall table (class "frame" from show_standard_header) */
   rsprintf("\r\n</table><!-- show_standard_title -->\r\n");

   show_bottom_text(lbs);
   if (!email)
      rsprintf("</form>\n");
   rsprintf("</body></html>\r\n");
}

/*------------------------------------------------------------------*/

BOOL check_password(LOGBOOK * lbs, char *name, char *password, char *redir)
{
   char str[1000];

   /* get password from configuration file */
   if (getcfg(lbs->name, name, str, sizeof(str))) {
      if (strcmp(password, str) == 0)
         return TRUE;

      if (!isparam("fail") && password[0]) {
         strlcpy(str, redir, sizeof(str));
         if (strchr(str, '?'))
            strlcat(str, "&fail=1", sizeof(str));
         else
            strlcat(str, "?fail=1", sizeof(str));
         redirect(lbs, str);
         return FALSE;
      }

      /* show web password page */
      show_standard_header(lbs, FALSE, loc("ELOG password"), NULL, FALSE, NULL, NULL);

      rsprintf("<table class=\"dlgframe\" cellspacing=0 align=center>");

      if (isparam("fail"))
         rsprintf("<tr><td class=\"dlgerror\">%s!</td></tr>\n", loc("Wrong password"));

      rsprintf("<tr><td class=\"dlgtitle\">\n");

      /* define hidden fields for current destination */
      if (redir[0]) {
         strlcpy(str, redir, sizeof(str));
         if (strchr(str, '<'))
            url_encode(str, sizeof(str));
         rsprintf("<input type=hidden name=redir value=\"%s\">\n", str);
      }

      if (strcmp(name, "Write password") == 0) {
         rsprintf("%s</td></tr>\n", loc("Please enter password to obtain write access"));
         rsprintf("<tr><td align=center class=\"dlgform\"><input type=password name=wpassword></td></tr>\n");
      } else {
         rsprintf("%s</td></tr>\n", loc("Please enter password to obtain administration access"));
         rsprintf("<tr><td align=center class=\"dlgform\"><input type=password name=apassword></td></tr>\n");
      }

      rsprintf("<tr><td align=center class=\"dlgform\"><input type=submit value=\"%s\"></td></tr>",
               loc("Submit"));

      rsprintf("</table>\n");
      show_bottom_text(lbs);
      rsprintf("</body></html>\r\n");

      return FALSE;
   } else
      return TRUE;
}

/*------------------------------------------------------------------*/

BOOL convert_password_file(char *file_name)
{
   char name[256], password[256], full_name[256], email[256], email_notify[256];
   int i, len, fh;
   char *buf, *p;
   PMXML_NODE root, list, node;

   printf("Converting password file \"%s\" to new XML format ... ", file_name);

   fh = open(file_name, O_RDONLY | O_BINARY);
   if (fh < 0)
      return FALSE;
   len = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);
   buf = xmalloc(len + 1);
   assert(buf);
   i = my_read(fh, buf, len);
   buf[i] = 0;
   close(fh);

   /* create backup */
   strlcpy(name, file_name, sizeof(name));
   strlcat(name, "_bak", sizeof(name));
   fh = open(name, O_WRONLY | O_BINARY | O_CREAT | O_TRUNC, 0644);
   if (fh > 0) {
      write(fh, buf, len);
      close(fh);
   }

   p = buf;

   /* skip leading spaces or new lines */
   while (*p && isspace(*p))
      p++;

   root = mxml_create_root_node();
   list = mxml_add_node(root, "list", NULL);

   while (*p) {

      /* skip comment lines */
      if (*p != ';' && *p != '#') {
         for (i = 0; i < (int) sizeof(name) - 1 && *p && *p != ':'; i++)
            name[i] = *p++;
         name[i] = 0;
         if (*p++ != ':') {
            xfree(buf);
            return FALSE;
         }

         for (i = 0; i < (int) sizeof(password) - 1 && *p && *p != ':'; i++)
            password[i] = *p++;
         password[i] = 0;
         if (*p++ != ':') {
            xfree(buf);
            return FALSE;
         }

         for (i = 0; i < (int) sizeof(full_name) - 1 && *p && *p != ':'; i++)
            full_name[i] = *p++;
         full_name[i] = 0;
         if (*p++ != ':') {
            xfree(buf);
            return FALSE;
         }

         for (i = 0; i < (int) sizeof(email) - 1 && *p && *p != ':'; i++)
            email[i] = *p++;
         email[i] = 0;
         if (*p++ != ':') {
            xfree(buf);
            return FALSE;
         }

         for (i = 0; i < (int) sizeof(email_notify) - 1 && *p && *p != '\r' && *p != '\n'; i++)
            email_notify[i] = *p++;
         email_notify[i] = 0;
         if (*p && *p != '\n' && *p != '\r') {
            xfree(buf);
            return FALSE;
         }

         while (*p && (*p == '\r' || *p == '\n'))
            p++;

         node = mxml_add_node(list, "user", NULL);
         mxml_add_node(node, "name", name);
         mxml_add_node(node, "password", password);
         mxml_add_node(node, "full_name", full_name);
         mxml_add_node(node, "last_logout", "0");
         mxml_add_node(node, "last_activity", "0");
         mxml_add_node(node, "email", email);
         mxml_add_node(node, "email_notify", email_notify);
      }

      while (*p && isspace(*p))
         p++;
   }

   mxml_write_tree(file_name, root);
   mxml_free_tree(root);

   printf("Ok\n");

   xfree(buf);
   return TRUE;
}

/*------------------------------------------------------------------*/

PMXML_NODE load_password_file(LOGBOOK * lbs, char *error, int error_size)
{
   PMXML_NODE root, list, xml_tree;
   char str[256], line[256], file_name[256];
   int fh;

   if (error)
      error[0] = 0;

   if (!get_password_file(lbs, file_name, sizeof(file_name)))
      return NULL;

   fh = open(file_name, O_RDONLY);

   /* if password file doen't exist, try to create it */
   if (fh < 0) {
      fh = open(file_name, O_CREAT | O_RDWR, 0600);
      if (fh < 0) {
         sprintf(str, loc("Cannot open file \"%s\""), file_name);
         strcat(str, ": ");
         strlcat(str, strerror(errno), sizeof(str));
         show_error(str);
         eprintf(str);
         strlcpy(error, str, error_size);
         return NULL;
      }
      close(fh);

      /* put empty XML tree into password file */
      printf("\nCreate empty password file \"%s\"\n", file_name);
      root = mxml_create_root_node();
      list = mxml_add_node(root, "list", NULL);
      mxml_write_tree(file_name, root);
      mxml_free_tree(root);
   } else {

      /* check if in XML format, otherwise convert it */
      line[0] = 0;
      read(fh, line, sizeof(line));
      close(fh);
      if (strstr(line, "<?xml") == 0) {
         if (!convert_password_file(file_name)) {
            sprintf(str, "Cannot convert password file \"%s\"", file_name);
            strlcpy(error, str, error_size);
            return NULL;
         }
      }
   }

   if ((xml_tree = mxml_parse_file(file_name, str, sizeof(str))) == NULL) {
      show_error(str);
      strlcpy(error, str, error_size);
      eprintf("Cannot load password file \"%s\": %s", file_name, error);
      return NULL;
   }

   return xml_tree;
}

/*------------------------------------------------------------------*/

int load_password_files()
{
   int i, j;
   char str1[256], str2[256], error[256];
   PMXML_NODE xml_tree;

   for (i = 0; lb_list[i].name[0]; i++) {
      if (lb_list[i].pwd_xml_tree == NULL) {
         if (lb_list[i].top_group[0])
            setcfg_topgroup(lb_list[i].top_group);
         xml_tree = load_password_file(&lb_list[i], error, sizeof(error));
         if (error[0])
            return 0;
         if (xml_tree) {
            lb_list[i].pwd_xml_tree = xml_tree;

            /* if other logbook has same password file, copy pointer */
            if (lb_list[i].top_group[0])
               setcfg_topgroup(lb_list[i].top_group);
            getcfg(lb_list[i].name, "Password file", str1, sizeof(str1));

            for (j = i + 1; lb_list[j].name[0]; j++) {
               if (lb_list[j].top_group[0])
                  setcfg_topgroup(lb_list[j].top_group);
               getcfg(lb_list[j].name, "Password file", str2, sizeof(str2));
               if (strieq(str1, str2)) {
                  lb_list[j].pwd_xml_tree = xml_tree;
               }
            }
         }
      }
   }

   return 1;
}

/*------------------------------------------------------------------*/

int get_user_line(LOGBOOK * lbs, char *user, char *password, char *full_name, char *email,
                  BOOL email_notify[1000], time_t * last_logout)
{
   int i, j;
   char str[256], global[256], orig_topgroup[256];
   PMXML_NODE user_node, node, subnode;

   if (password)
      password[0] = 0;
   if (full_name)
      full_name[0] = 0;
   if (email)
      email[0] = 0;
   if (email_notify)
      email_notify[0] = 0;
   if (last_logout)
      *last_logout = 0;

   /* if global password file is requested, search for first
      logbook with same password file than global section */
   orig_topgroup[0] = 0;
   if (lbs == NULL) {
      getcfg("global", "Password file", global, sizeof(global));
      if (getcfg_topgroup() && *getcfg_topgroup())
         strcpy(orig_topgroup, getcfg_topgroup());

      for (i = 0; lb_list[i].name[0]; i++) {
         if (lb_list[i].top_group[0])
            setcfg_topgroup(lb_list[i].top_group);
         getcfg(lb_list[i].name, "Password file", str, sizeof(str));
         if (str[0] && strieq(str, global)) {
            lbs = lb_list + i;
            break;
         }
      }

      if (!lb_list[i].name[0])
         return 1;

      if (orig_topgroup[0])
         setcfg_topgroup(orig_topgroup);
   }

   getcfg(lbs->name, "Password file", str, sizeof(str));

   if (!str[0] || !user[0])
      return 1;

   if (lbs->pwd_xml_tree) {
      sprintf(str, "/list/user[name=%s]", user);
      if ((user_node = mxml_find_node(lbs->pwd_xml_tree, str)) == NULL)
         return 2;

      /* if user found, retrieve other info */
      if ((node = mxml_find_node(user_node, "password")) != NULL && password && mxml_get_value(node))
         strlcpy(password, mxml_get_value(node), 256);
      if ((node = mxml_find_node(user_node, "full_name")) != NULL && full_name && mxml_get_value(node))
         strlcpy(full_name, mxml_get_value(node), 256);
      if ((node = mxml_find_node(user_node, "email")) != NULL && email && mxml_get_value(node))
         strlcpy(email, mxml_get_value(node), 256);
      if ((node = mxml_find_node(user_node, "last_logout")) != NULL && last_logout && mxml_get_value(node)) {
         *last_logout = date_to_ltime(mxml_get_value(node));
         if (*last_logout == -1)
            *last_logout = 0;
      }

      if ((node = mxml_find_node(user_node, "email_notify")) != NULL && email_notify) {
         if (mxml_get_number_of_children(node)) {
            for (i = 0; i < 1000; i++)
               email_notify[i] = FALSE;

            for (i = 0; i < mxml_get_number_of_children(node); i++) {
               subnode = mxml_subnode(node, i);
               for (j = 0; lb_list[j].name[0]; j++)
                  if (strieq(lb_list[j].name, mxml_get_value(subnode))) {
                     email_notify[j] = TRUE;
                     break;
                  }
            }

         } else {
            for (i = 0; i < 1000; i++)
               if (strieq(mxml_get_value(node), "all"))
                  email_notify[i] = TRUE;
               else
                  email_notify[i] = FALSE;
         }
      }

      return 1;
   } else {
      if (!user[0])
         return 1;

      /* open password file */
      load_password_files();
      return get_user_line(lbs, user, password, full_name, email, email_notify, last_logout);
   }
}

/*------------------------------------------------------------------*/

void set_user_login_time(LOGBOOK * lbs, char *user)
{
   int i;
   char str[256], global[256], orig_topgroup[256], file_name[256];
   PMXML_NODE user_node, node;
   time_t last, now;

   /* if global password file is requested, search for first
      logbook with same password file than global section */
   orig_topgroup[0] = 0;
   if (lbs == NULL) {
      getcfg("global", "Password file", global, sizeof(global));
      if (getcfg_topgroup() && *getcfg_topgroup())
         strcpy(orig_topgroup, getcfg_topgroup());

      for (i = 0; lb_list[i].name[0]; i++) {
         if (lb_list[i].top_group[0])
            setcfg_topgroup(lb_list[i].top_group);
         getcfg(lb_list[i].name, "Password file", str, sizeof(str));
         if (strieq(str, global)) {
            lbs = lb_list + i;
            break;
         }
      }

      if (!lb_list[i].name[0])
         return;

      if (orig_topgroup[0])
         setcfg_topgroup(orig_topgroup);
   }

   getcfg(lbs->name, "Password file", str, sizeof(str));

   if (!str[0] || !user[0])
      return;

   if (lbs->pwd_xml_tree) {
      sprintf(str, "/list/user[name=%s]", user);
      if ((user_node = mxml_find_node(lbs->pwd_xml_tree, str)) == NULL)
         return;

      if ((node = mxml_find_node(user_node, "last_activity")) != NULL) {
         strlcpy(str, mxml_get_value(node), sizeof(str));
         last = date_to_ltime(str);
      } else
         last = 0;

      time(&now);

      /* check if activity time changed significantly */
      if (now > last + 60) {

         /* if last activity is more than one hour ago, set new logout time from last activity */
         if (now > last + 3600) {
            strcpy(str, "0");
            if ((node = mxml_find_node(user_node, "last_activity")) != NULL)
               strlcpy(str, mxml_get_value(node), sizeof(str));

            if ((node = mxml_find_node(user_node, "last_logout")) != NULL)
               mxml_replace_node_value(node, str);
            else
               mxml_add_node(user_node, "last_logout", str);
         }

         /* set new last activity */
         strcpy(str, ctime(&now));
         str[24] = 0;
         if ((node = mxml_find_node(user_node, "last_activity")) != NULL)
            mxml_replace_node_value(node, str);
         else
            mxml_add_node(user_node, "last_activity", str);

         /* flush to password file */
         if (get_password_file(lbs, file_name, sizeof(file_name)))
            mxml_write_tree(file_name, lbs->pwd_xml_tree);
      }
   }
}

/*------------------------------------------------------------------*/

BOOL enum_user_line(LOGBOOK * lbs, int n, char *user, int size)
{
   char str[256], file_name[256];
   int i;
   PMXML_NODE node;

   if (lbs == NULL) {
      getcfg(NULL, "password file", file_name, sizeof(file_name));
      for (i = 0; lb_list[i].name[0]; i++) {
         getcfg(lb_list[i].name, "password file", str, sizeof(str));
         if (strieq(file_name, str))
            break;
      }
      if (lb_list[i].name[0] == 0)
         lbs = &lb_list[0];
      else
         lbs = &lb_list[i];
   }

   if (!lbs)
      return FALSE;

   if (lbs->pwd_xml_tree == NULL)
      return FALSE;

   sprintf(str, "/list/user[%d]/name", n + 1);
   if ((node = mxml_find_node(lbs->pwd_xml_tree, str)) == NULL)
      return FALSE;

   strlcpy(user, mxml_get_value(node), size);
   return TRUE;
}

/*------------------------------------------------------------------*/

BOOL check_login_user(LOGBOOK * lbs, char *user)
{
   int i, n, status;
   char str[1000];
   char list[MAX_N_LIST][NAME_LENGTH];

   if (user == NULL)
      return FALSE;

   /* check if usr is in password file */
   status = get_user_line(lbs, user, NULL, NULL, NULL, NULL, NULL);
   if (status == 2)
      return FALSE;

   /* treat admin user as login user */
   if (getcfg(lbs->name, "Admin user", str, sizeof(str)) && user[0]) {
      n = strbreak(str, list, MAX_N_LIST, ",", FALSE);
      for (i = 0; i < n; i++)
         if (strcmp(user, list[i]) == 0)
            return TRUE;
   }

   if (getcfg(lbs->name, "Login user", str, sizeof(str)) && user[0]) {
      n = strbreak(str, list, MAX_N_LIST, ",", FALSE);
      for (i = 0; i < n; i++)
         if (strcmp(user, list[i]) == 0)
            break;

      if (i == n)
         return FALSE;
   }
   return TRUE;
}

/*------------------------------------------------------------------*/

BOOL is_admin_user(char *logbook, char *user)
{
   int i, n;
   char str[1000];
   char list[MAX_N_LIST][NAME_LENGTH];

   /* Removed user[0] for cloning, have to check implications, same below.
      if (getcfg(logbook, "Admin user", str, sizeof(str)) && user[0]) { */

   if (user == NULL)
      return FALSE;

   if (getcfg(logbook, "Admin user", str, sizeof(str))) {
      n = strbreak(str, list, MAX_N_LIST, ",", FALSE);
      for (i = 0; i < n; i++)
         if (strcmp(user, list[i]) == 0)
            break;

      if (i == n)
         return FALSE;
   }
   return TRUE;
}

/*------------------------------------------------------------------*/

BOOL is_admin_user_global(char *user)
{
   int i, n;
   char str[1000];
   char list[MAX_N_LIST][NAME_LENGTH];

   if (user == NULL)
      return FALSE;

   if (getcfg_simple("global", "Admin user", str, sizeof(str), FALSE)) {
      n = strbreak(str, list, MAX_N_LIST, ",", FALSE);
      for (i = 0; i < n; i++)
         if (strcmp(user, list[i]) == 0)
            break;

      if (i == n)
         return FALSE;
   }
   return TRUE;
}

/*------------------------------------------------------------------*/

BOOL check_user_password(LOGBOOK * lbs, char *user, char *password, char *redir)
{
   char str[1000], str2[256], upwd[256], full_name[256], email[256];
   int status, show_forgot_link, show_self_register;

   if (user == NULL)
      status = 1;
   else {
      status = get_user_line(lbs, user, upwd, full_name, email, NULL, NULL);

      if (status == 1 && user[0])
         set_user_login_time(lbs, user);

      /* check for logout */
      if (isparam("LO")) {
         /* remove cookies */
         set_login_cookies(lbs, "", "");
         return FALSE;
      }

      /* check for "forgot password" */
      if (isparam("cmd") && strcmp(getparam("cmd"), loc("Forgot")) == 0) {
         if (getcfg(lbs->name, "forgot password link", str, sizeof(str)) && atoi(str) == 0)
            return FALSE;

         show_forgot_pwd_page(lbs);
         return FALSE;
      }

      if (!check_login_user(lbs, user)) {
         if (isparam("fail")) {
            /* remove remaining cookies */
            remove_all_login_cookies(lbs);
            return FALSE;
         }
         redirect(lbs, "?fail=1");
         return FALSE;
      }
   }

   if (status == 1) {
      if (user != NULL && user[0] && password && strcmp(password, upwd) == 0) {
         setparam("full_name", full_name);
         setparam("user_email", email);
         return TRUE;
      }

      if (!isparam("fail") && password && password[0]) {
         redirect(lbs, "?fail=1");
         return FALSE;
      }

      /* if URL is specified in configuration file, check if login happens for
         the specified host, in order to get cookies right... */

      if (getcfg(lbs->name, "URL", str, sizeof(str))) {
         extract_host(str);
         strlcpy(str2, http_host, sizeof(str));
         if (strchr(str2, ':'))
            *strchr(str2, ':') = 0;
         if (!strieq(str, str2)) {
            redirect(lbs, _cmdline);
            return FALSE;
         }
      }

      /* show login password page */
      sprintf(str, "ELOG %s", loc("Login"));
      show_html_header(lbs, TRUE, str, TRUE, FALSE, NULL, FALSE);

      /* set focus on name field */
      rsprintf("<body OnLoad=\"document.form1.uname.focus();\">\n");

      rsprintf("<form name=form1 method=\"POST\" action=\"./\" enctype=\"multipart/form-data\">\n\n");

      /* define hidden fields for current destination */
      strlcpy(str, redir, sizeof(str));
      if (strchr(str, '<'))
         url_encode(str, sizeof(str));
      rsprintf("<input type=hidden name=redir value=\"%s\">\n", str);

      rsprintf("<table class=\"dlgframe\" cellspacing=0 align=center>");

      if (isparam("fail")) {
         sprintf(str, loc("Invalid user name or password"));
         rsprintf("<tr><td colspan=2 class=\"dlgerror\">%s!</td></tr>\n", str);
      }

      rsprintf("<tr><td colspan=2 class=\"dlgtitle\">%s</td></tr>\n", loc("Please login"));

      rsprintf("<tr><td align=right class=\"dlgform\">%s:</td>\n", loc("Username"));
      rsprintf("<td align=left class=\"dlgform\"><input type=text name=uname value=\"%s\"></td></tr>\n",
               isparam("unm") ? getparam("unm") : "");

      rsprintf("<tr><td align=right class=\"dlgform\">%s:</td>\n", loc("Password"));
      rsprintf("<td align=left class=\"dlgform\"><input type=password name=upassword></td></tr>\n");

      if (!getcfg(lbs->name, "Login expiration", str, sizeof(str)) || atof(str) > 0) {
         rsprintf("<tr><td align=center colspan=2 class=\"dlgform\">");

         if (isparam("urem") && atoi(getparam("urem")) == 0)
            rsprintf("<input type=checkbox name=remember value=1>\n");
         else
            rsprintf("<input type=checkbox checked name=remember value=1>\n");
         rsprintf("%s<br>\n", loc("Keep me logged in on this computer"));

         if (str[0] == 0)
            rsprintf(loc("for the next %d days"), 31);
         else if (atof(str) < 1)
            rsprintf(loc("for the next %d minutes"), (int) (atof(str) * 60));
         else if (atof(str) == 1)
            rsprintf(loc("for the next hour"));
         else if (atof(str) <= 48)
            rsprintf(loc("for the next %d hours"), (int) atof(str));
         else
            rsprintf(loc("for the next %d days"), (int) (atof(str) / 24));

         rsprintf(" %s", loc("or until I log out"));
         rsprintf("</td></tr>\n");
      }

      show_forgot_link = (!getcfg(lbs->name, "allow password change", str, sizeof(str)) || atoi(str) == 1);
      show_self_register = (getcfg(lbs->name, "Self register", str, sizeof(str)) && atoi(str) > 0);

      if (show_forgot_link || show_self_register)
         rsprintf("<tr><td align=center colspan=2 class=\"dlgform\">\n");

      if (show_forgot_link)
         rsprintf("<a href=\"?cmd=%s\">%s</a>", loc("Forgot"), loc("Forgot password?"));

      if (show_self_register) {
         strlcpy(str, loc("New user"), sizeof(str));
         url_encode(str, sizeof(str));
         if (show_forgot_link)
            rsprintf("&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;");
         rsprintf("<a href=\"?cmd=%s\">%s</a></td></tr>", str, loc("Register as new user"));
      }

      if (show_forgot_link || show_self_register)
         rsprintf("</td></tr>\n");

      rsprintf("<tr><td align=center colspan=2 class=\"dlgform\"><input type=submit value=\"%s\"></td></tr>",
               loc("Submit"));

      rsprintf("</table>\n");

      show_bottom_text_login(lbs);

      rsprintf("</form></body></html>\r\n");

      return FALSE;
   } else {
      if (status == 2) {

         sprintf(str, "?fail");
         setparam("redir", str);

         /* remove remaining cookies */
         remove_all_login_cookies(lbs);
      } else {
         getcfg(lbs->name, "Password file", full_name, sizeof(full_name));
         sprintf(str, loc("Cannot open file <b>%s</b>"), full_name);
         strcat(str, ": ");
         strlcat(str, strerror(errno), sizeof(str));
         show_error(str);
      }
      return FALSE;
   }
}

/*------------------------------------------------------------------*/

int node_contains(LBLIST pn, char *logbook)
{
   int i;

   for (i = 0; i < pn->n_members; i++) {

      /* check if logbook in this group */
      if (strieq(pn->member[i]->name, logbook))
         return 1;

      /* check if loogbook is in subgroups */
      if (pn->member[i]->n_members > 0 && node_contains(pn->member[i], logbook))
         return 1;
   }

   return 0;
}

/*------------------------------------------------------------------*/

void show_logbook_node(LBLIST plb, LBLIST pparent, int level, int btop)
{
   int i, index, j, expand, expand_all, message_id;
   char str[10000], date[256], slist[MAX_N_ATTR + 10][NAME_LENGTH], svalue[MAX_N_ATTR + 10][NAME_LENGTH],
       mid[80];

   if (plb->n_members > 0) {

      expand = 0;
      if (isparam("gexp")) {
         if (strieq(plb->name, getparam("gexp")) || node_contains(plb, getparam("gexp"))
             || strieq(getparam("gexp"), "all"))
            expand = 1;
      }

      if (!getcfg(plb->name, "Expand selection page", str, sizeof(str)) || atoi(str) == 1)
         expand_all = 1;
      else
         expand_all = 0;

      /* do not display top groups */
      if (!plb->is_top) {

         rsprintf("<tr>");
         for (i = 0; i < level; i++)
            rsprintf("<td class=\"selspace\">&nbsp;</td>\n");
         rsprintf("<td colspan=%d class=\"selgroup\">", 13 - level);
         for (i = 0; i < level; i++)
            rsprintf("&nbsp;&nbsp;");
         if (expand) {

            if (expand_all)
               rsprintf("%s", plb->name);
            else {
               if (pparent != NULL) {
                  if (getcfg_topgroup())
                     rsprintf("<a href=\"%s/?gexp=%s\">- %s</a> ", getcfg_topgroup(), pparent->name,
                              plb->name);
                  else
                     rsprintf("<a href=\"?gexp=%s\">- %s</a> ", pparent->name, plb->name);
               } else {
                  if (getcfg_topgroup())
                     rsprintf("<a href=\"%s/\">- %s</a> ", getcfg_topgroup(), plb->name);
                  else
                     rsprintf("<a href=\".\">- %s</a> ", plb->name);
               }
            }
         } else {
            if (expand_all)
               rsprintf("%s", plb->name);
            else {
               if (getcfg_topgroup())
                  rsprintf("<a href=\"%s/?gexp=%s\">+ %s</a> ", getcfg_topgroup(), plb->name, plb->name);
               else
                  rsprintf("<a href=\".?gexp=%s\">+ %s</a> ", plb->name, plb->name);
            }
         }

         rsprintf("</td></tr>\n");
      }

      if (plb->is_top || expand || expand_all)
         for (i = 0; i < plb->n_members; i++)
            show_logbook_node(plb->member[i], plb->is_top ? NULL : plb, level + 1, btop);
   } else {

      if (!getcfg(plb->name, "Hidden", str, sizeof(str)) || atoi(str) == 0) {

         /* search logbook in list */
         for (index = 0; lb_list[index].name[0]; index++)
            if (strieq(plb->name, lb_list[index].name))
               break;
         if (!lb_list[index].name[0])
            return;

         rsprintf("<tr>");
         for (j = 0; j < level; j++)
            rsprintf("<td class=\"selspace\">&nbsp;</td>\n");
         rsprintf("<td colspan=%d class=\"sellogbook\">", 10 - level);

         if (btop)
            rsprintf("<a href=\"../%s/\">%s</a>", lb_list[index].name_enc, lb_list[index].name);
         else
            rsprintf("<a href=\"%s\">%s</a>", lb_list[index].name_enc, lb_list[index].name);

         if (getcfg(lb_list[index].name, "Read password", str, sizeof(str)) || (getcfg(lb_list[index].name,
                                                                                       "Password file", str,
                                                                                       sizeof(str))
                                                                                && !getcfg(lb_list[index].
                                                                                           name,
                                                                                           "Guest menu commands",
                                                                                           str, sizeof(str))))
            rsprintf("&nbsp;&nbsp;<img src=\"lock.png\" alt=\"%s\" title=\"%s\">",
                     loc("This logbook requires authentication"),
                     loc("This logbook requires authentication"));
         rsprintf("<br>\n");
         str[0] = 0;
         getcfg(lb_list[index].name, "Comment", str, sizeof(str));
         rsprintf("<span class=\"selcomment\">");
         if (is_html(str))
            rsputs(str);
         else
            rsputs3(str);
         rsprintf("</span></td>\n");
         rsprintf("<td nowrap class=\"selentries\">");
         rsprintf("%d", *lb_list[index].n_el_index);
         rsprintf("</td>\n");
         rsprintf("<td nowrap class=\"selentries\">");
         if (*lb_list[index].n_el_index == 0)
            rsprintf("-");
         else {
            char attrib[MAX_N_ATTR][NAME_LENGTH];

            lb_list[index].n_attr = scan_attributes(lb_list[index].name);
            message_id = el_search_message(&lb_list[index], EL_LAST, 0, FALSE);
            el_retrieve(&lb_list[index], message_id, date, attr_list, attrib, lb_list[index].n_attr, NULL, 0,
                        NULL, NULL, NULL, NULL, NULL);
            if (!getcfg(lb_list[index].name, "Last submission", str, sizeof(str))) {
               sprintf(str, "$entry time");
               for (i = 0; i < lb_list[index].n_attr; i++)
                  if (strieq(attr_list[i], "Author"))
                     break;
               if (i < lb_list[index].n_attr)
                  sprintf(str + strlen(str), " %s $author", loc("by"));
            }
            j = build_subst_list(&lb_list[index], slist, svalue, attrib, TRUE);
            sprintf(mid, "%d", message_id);
            add_subst_list(slist, svalue, "message id", mid, &j);
            add_subst_time(&lb_list[index], slist, svalue, "entry time", date, &j, 0);
            strsubst_list(str, sizeof(str), slist, svalue, j);
            rsputs(str);
         }
         rsprintf("</td></tr>\n");
      }
   }
}

/*------------------------------------------------------------------*/

void show_top_selection_page()
{
   int i;
   char str[10000], name[NAME_LENGTH], name_enc[NAME_LENGTH];
   LBLIST phier;

   /* if selection page protected, check password */
   if (getcfg("global", "password file", str, sizeof(str)) && getcfg("global", "protect selection page", str,
                                                                     sizeof(str)) && atoi(str) == 1)
      if (!check_user_password(NULL, getparam("unm"), getparam("upwd"), ""))
         return;

   if (getcfg("global", "Page Title", str, sizeof(str))) {
      strip_html(str);
      show_html_header(NULL, TRUE, str, TRUE, FALSE, NULL, FALSE);
   } else
      show_html_header(NULL, TRUE, "ELOG Logbook Selection", TRUE, FALSE, NULL, FALSE);
   rsprintf("<body>\n\n");
   rsprintf("<table class=\"selframe\" cellspacing=0 align=center>\n");
   rsprintf("<tr><td class=\"dlgtitle\">\n");

   if (getcfg("global", "Welcome title", str, sizeof(str))) {
      rsputs(str);
   } else {
      rsprintf("%s.<BR>\n", loc("Several logbooks groups are defined on this host"));
      rsprintf("%s:\n", loc("Please select one to list the logbooks in that group"));
   }

   rsprintf("</td></tr>\n");

   phier = get_logbook_hierarchy();
   for (i = 0; i < phier->n_members; i++)
      if (phier->member[i]->is_top) {
         rsprintf("<tr><td class=\"sellogbook\">");
         strlcpy(name, phier->member[i]->name, sizeof(name));
         strlcpy(name_enc, name, sizeof(name_enc));
         url_encode(name_enc, sizeof(name_enc));
         rsprintf("<a href=\"%s/\">%s</a>", name_enc, name);
         rsprintf("</td></tr>\n");
      }

   free_logbook_hierarchy(phier);
   rsprintf("</table></body>\n");
   rsprintf("</html>\r\n\r\n");
}

/*------------------------------------------------------------------*/

void show_selection_page(void)
{
   int i, j, expand_all, show_title;
   char str[10000], file_name[256];
   LBLIST phier;

   /* check if at least one logbook defined */
   if (!lb_list[0].name[0]) {
      show_standard_header(NULL, FALSE, "ELOG", "", FALSE, NULL, NULL);

      rsprintf("<table class=\"dlgframe\" cellspacing=0 align=center>");
      rsprintf("<tr><td class=\"dlgtitle\">\n");
      rsprintf(loc("No logbook defined on this server"));
      rsprintf("</td></tr>\n");

      rsprintf("<tr><td align=center class=\"dlgform\">\n");
      rsprintf("<a href=\"?cmd=%s\">%s</a>", loc("Create new logbook"), loc("Create new logbook")),
          rsprintf("</td></tr></table>\n");
      rsprintf("</body></html>\n");
      return;
   }

   /* check for Guest Selection Page */
   if (getcfg("global", "Guest Selection Page", str, sizeof(str)) && !(isparam("unm") && isparam("upwd"))) {
      /* check for URL */
      if (strstr(str, "http://") || strstr(str, "https://")) {
         redirect(NULL, str);
         return;
      }

      /* check if file starts with an absolute directory */
      if (str[0] == DIR_SEPARATOR || str[1] == ':')
         strlcpy(file_name, str, sizeof(file_name));
      else {
         strlcpy(file_name, logbook_dir, sizeof(file_name));
         strlcat(file_name, str, sizeof(file_name));
      }
      send_file_direct(file_name);
      return;
   }

   /* top group present and no top group in URL */
   if (exist_top_group() && getcfg_topgroup() == NULL) {
      if (getcfg("global", "show top groups", str, sizeof(str)) && atoi(str) == 1) {
         show_top_selection_page();
         return;
      } else
         return;                /* abort connection */
   }

   /* if selection page protected, check password */
   if (getcfg("global", "password file", str, sizeof(str)) && getcfg("global", "protect selection page", str,
                                                                     sizeof(str)) && atoi(str) == 1)
      if (!check_user_password(NULL, getparam("unm"), getparam("upwd"), ""))
         return;

   if (getcfg("global", "Page Title", str, sizeof(str))) {
      strip_html(str);
      show_html_header(NULL, TRUE, str, TRUE, FALSE, NULL, FALSE);
   } else
      show_html_header(NULL, TRUE, "ELOG Logbook Selection", TRUE, FALSE, NULL, FALSE);

   rsprintf("<body>\n\n");
   rsprintf("<table class=\"selframe\" cellspacing=0 align=center>\n");
   rsprintf("<tr><td colspan=13 class=\"dlgtitle\">\n");

   if (getcfg("global", "Welcome title", str, sizeof(str))) {
      rsputs(str);
   } else {
      rsprintf("%s.<BR>\n", loc("Several logbooks are defined on this host"));
      rsprintf("%s:\n", loc("Please select the one to connect to"));
   }

   rsprintf("</td></tr>\n");

   if (getcfg("global", "mirror server", str, sizeof(str))) {

      /* only admin user sees synchronization link */
      if (is_admin_user("global", getparam("unm"))) {
         rsprintf("<tr>\n");
         rsprintf("<td colspan=13 class=\"seltitle\">\n");
         rsprintf("<a href=\"?cmd=Synchronize\">%s</a></td>\n", loc("Synchronize all logbooks"));
         rsprintf("</tr>\n");
      }
   }

   phier = get_logbook_hierarchy();
   show_title = 0;

   if (getcfg_topgroup()) {
      for (i = 0; i < phier->n_members; i++)
         if (strieq(getcfg_topgroup(), phier->member[i]->name)) {
            if (phier->member[i]->n_members == 0)
               show_title = 1;
            else
               for (j = 0; j < phier->member[i]->n_members; j++)
                  if (phier->member[i]->member[j]->n_members == 0)
                     show_title = 1;

            break;
         }
   } else
      for (i = 0; i < phier->n_members; i++)
         if (phier->member[i]->n_members == 0)
            show_title = 1;

   if (!getcfg("global", "Expand selection page", str, sizeof(str)) || atoi(str) == 1)
      expand_all = 1;
   else
      expand_all = 0;

   if (isparam("gexp") || expand_all)
      show_title = 1;

   if (show_title) {
      rsprintf("<tr>\n");
      rsprintf("<th colspan=10 class=\"seltitle\">%s</th>\n", loc("Logbook"));
      rsprintf("<th class=\"seltitle\">%s</th>\n", loc("Entries"));
      rsprintf("<th class=\"seltitle\">%s</th>\n", loc("Last submission"));
      rsprintf("</tr>\n");
   } else {
      rsprintf("<tr>\n");
      rsprintf("<td colspan=13 class=\"selexp\">\n");
      rsprintf("<a href=\"?gexp=all\">%s</a></td>\n", loc("Expand all"));
      rsprintf("</tr>\n");
   }

   if (getcfg_topgroup()) {
      for (i = 0; i < phier->n_members; i++)
         if (strieq(getcfg_topgroup(), phier->member[i]->name)) {
            show_logbook_node(phier->member[i], NULL, -1, 1);
            break;
         }
   } else
      for (i = 0; i < phier->n_members; i++)
         show_logbook_node(phier->member[i], NULL, 0, 0);

   free_logbook_hierarchy(phier);
   rsprintf("</table></body>\n");
   rsprintf("</html>\r\n\r\n");
}

/*------------------------------------------------------------------*/

void get_password(char *password)
{
   static char last_password[32];

   if (strncmp(password, "set=", 4) == 0)
      strcpy(last_password, password + 4);
   else
      strcpy(password, last_password);
}

/*------------------------------------------------------------------*/

int do_self_register(LOGBOOK * lbs, char *command)
/* evaluate self-registration commands */
{
   char str[256];

   if (command == NULL)
      return 1;

   /* display new user page if "self register" is clicked */
   if (strieq(command, loc("New user"))) {
      show_new_user_page(lbs);
      return 0;
   }

   /* save user info if "save" is pressed */
   if (strieq(command, loc("Save")) && isparam("new_user_name") && !isparam("config")) {
      if (!save_user_config(lbs, getparam("new_user_name"), TRUE, FALSE))
         return 0;
      if (lbs)
         sprintf(str, "../%s/", lbs->name_enc);
      else
         sprintf(str, ".");
      redirect(lbs, str);
      return 0;
   }

   /* display account request notification */
   if (strieq(command, loc("Requested"))) {
      show_standard_header(lbs, FALSE, loc("ELOG registration"), "", FALSE, NULL, NULL);
      rsprintf("<table class=\"dlgframe\" cellspacing=0 align=center>");
      rsprintf("<tr><td colspan=2 class=\"dlgtitle\">\n");
      rsprintf("%s.", loc("Your request has been forwarded to the administrator"));
      rsprintf("%s.", loc("You will be notified by email upon activation of your new account"));
      rsprintf("</td></tr></table>\n");
      show_bottom_text(lbs);
      rsprintf("</body></html>\n");
      return 0;
   }

   /* indicate continue */
   return 1;
}

/*------------------------------------------------------------------*/

void show_day(char *css_class, char *day)
{
   if (day[0]) {
      rsprintf("<td class=\"%s\" ", css_class);
      rsprintf("onClick='submit_day(\"%s\")' ", day);
      rsprintf("onMouseOver=\"this.className='calsel';\" ");
      rsprintf("onMouseOut=\"this.className='%s';\" ", css_class);
      rsprintf(">%s</td>\n", day);
   } else {
      /* empty cell */
      rsprintf("<td class=\"%s\">&nbsp;</td>\n", css_class);
   }
}

void show_calendar(LOGBOOK * lbs)
{
   int i, j, cur_mon, cur_day, cur_year, today_day, today_mon, today_year;
   time_t now, stime;
   struct tm *ts;
   char str[256], index[10];

   time(&now);
   ts = localtime(&now);
   assert(ts);
   today_mon = ts->tm_mon + 1;
   today_day = ts->tm_mday;
   today_year = ts->tm_year + 1900;

   if (isparam("m") && isparam("y")) {
      cur_mon = atoi(getparam("m"));
      cur_year = atoi(getparam("y"));
      cur_day = -1;
      ts->tm_mday = 1;
      ts->tm_mon = cur_mon - 1;
      ts->tm_year = cur_year - 1900;
      mktime(ts);
   } else {
      cur_mon = ts->tm_mon + 1;
      cur_day = ts->tm_mday;
      cur_year = ts->tm_year + 1900;
   }
   if (isparam("i"))
      strcpy(index, getparam("i"));
   else
      strcpy(index, "1");

   show_html_header(lbs, FALSE, loc("Calendar"), TRUE, FALSE, NULL, FALSE);
   rsprintf("<body class=\"calwindow\"><form name=form1 method=\"GET\" action=\"cal.html\">\n");
   rsprintf("<input type=hidden name=\"i\" value=\"%s\">\n", index);
   rsprintf("<input type=hidden name=\"y\" value=\"%d\">\n", cur_year);

   rsprintf("<script type=\"text/javascript\">\n\n");
   rsprintf("function submit_day(day)\n");
   rsprintf("{\n");
   rsprintf("  opener.document.form1.d%s.value = day;\n", index, cur_year);
   rsprintf("  opener.document.form1.m%s.value = \"%d\";\n", index, cur_mon);
   rsprintf("  opener.document.form1.y%s.value = \"%d\";\n", index, cur_year);
   rsprintf("  window.close();\n");
   rsprintf("}\n");
   rsprintf("</script>\n\n");

   rsprintf("<table border=1 width=300><tr>");
   rsprintf("<td colspan=7 class=\"caltitle\">\n");

   rsprintf("<select name=\"m\" onChange=\"document.form1.submit()\">\n");
   for (i = 0; i < 12; i++)
      if (i + 1 == cur_mon)
         rsprintf("<option selected value=\"%d\">%s\n", i + 1, month_name(i));
      else
         rsprintf("<option value=\"%d\">%s\n", i + 1, month_name(i));
   rsprintf("</select>\n");

   /* link to previous year */
   rsprintf("&nbsp;&nbsp;");
   rsprintf("<a href=\"?i=%s&amp;m=%d&amp;y=%d\">", index, cur_mon, cur_year - 1);
   rsprintf("<img border=0 align=\"absmiddle\" src=\"cal_prev.png\" alt=\"%s\" title=\"%s\"></a>",
            loc("Previous Year"), loc("Previous Year"));

   /* current year */
   rsprintf("&nbsp;%d&nbsp;", cur_year);

   /* link to next year */
   rsprintf("<a href=\"?i=%s&amp;m=%d&amp;y=%d\">", index, cur_mon, cur_year + 1);
   rsprintf("<img border=0 align=\"absmiddle\" src=\"cal_next.png\" alt=\"%s\" title=\"%s\"></a>",
            loc("Next Year"), loc("Next Year"));

   /* go to first day of month */
   ts->tm_mday = 1;
   stime = mktime(ts);

   /* go to last sunday */
   stime = stime - 3600 * 24 * ts->tm_wday;

   rsprintf("<tr>\n");
   for (i = 0; i < 7; i++) {
      ts = localtime(&stime);
      assert(ts);
      strftime(str, sizeof(str), "%a", ts);
      rsprintf("<td class=\"calhead\">%s</td>\n", str);
      stime += 3600 * 24;
   }
   rsprintf("</tr>\n");
   stime -= 3600 * 24 * 7;
   ts = localtime(&stime);
   assert(ts);

   for (i = 0; i < 6; i++) {
      rsprintf("<tr>\n");

      for (j = 0; j < 7; j++) {
         if (ts->tm_mon + 1 == cur_mon)
            sprintf(str, "%d", ts->tm_mday);
         else
            strcpy(str, "");

         if (ts->tm_mday == today_day && ts->tm_mon + 1 == today_mon && ts->tm_year + 1900 == today_year)
            show_day("calcurday", str);
         else {
            if (j == 0)
               show_day("calsun", str);
            else if (j == 6)
               show_day("calsat", str);
            else
               show_day("calday", str);
         }
         stime += 3600 * 24;
         ts = localtime(&stime);
         assert(ts);
      }

      rsprintf("</tr>\n");
      if (ts->tm_mon + 1 != cur_mon)
         break;
   }

   rsprintf("</table>\n</form></body></html>\n");
}

/*------------------------------------------------------------------*/

void show_uploader(LOGBOOK * lbs)
{
   char str[256];

   show_html_header(lbs, FALSE, loc("Upload image"), TRUE, FALSE, NULL, FALSE);
   rsprintf("<body class=\"uploadwindow\"><form name=form1 method=\"POST\" action=\".\" ");
   rsprintf("enctype=\"multipart/form-data\">\n");
   rsprintf("<input type=hidden name=\"jcmd\" value=\"JUpload\">\n");

   rsprintf("<script type=\"text/javascript\">\n\n");
   rsprintf("  function upload()\n");
   rsprintf("  {\n");
   rsprintf("    if (document.form1.attfile.value == \"\") {\n");
   rsprintf("      alert(\"%s\");\n", loc("Please enter filename or URL"));
   rsprintf("      return false;\n");
   rsprintf("    }\n");
   rsprintf("    return true;\n");
   rsprintf("  }\n");

   rsprintf("</script>\n\n");

   rsprintf("<table border=0 width=500>");

   strcpy(str, loc("Maximum allowed file size is"));
   if (MAX_CONTENT_LENGTH >= 1024 * 1024)
      sprintf(str + strlen(str), " %d MB", MAX_CONTENT_LENGTH / 1024 / 1024);
   else
      sprintf(str + strlen(str), " %d kB", MAX_CONTENT_LENGTH / 1024);
   rsprintf("<tr><td nowrap class=\"uploadtext\"><b>%s:</b> <i>(%s)</i></td></tr>\n",
            loc("Enter filename or URL"), str);
   rsprintf("<tr><td class=\"uploadvalue\"><input type=\"file\" size=\"60\" ");
   rsprintf("maxlength=\"200\" name=\"attfile\" accept=\"filetype/*\"></td></tr>\n");

   rsprintf("<tr><td class=\"menuframe\" align=center><br>\n");
   rsprintf("<input type=\"submit\" name=\"cmd\" value=\"%s\" onClick=\"return upload();\">\n",
            loc("Upload"));
   rsprintf("<input type=\"submit\" name=\"cmd\" value=\"%s\" onClick=\"window.close();\">\n", loc("Cancel"));
   rsprintf("<br><br></td></tr>\n");

   rsprintf("</table>\n");
   rsprintf("</form></body></html>\n");
}

/*------------------------------------------------------------------*/

void show_uploader_finished(LOGBOOK * lbs)
{
   int i;
   char str[256], att[256], base_url[256], file_enc[256], ref[256], ref_thumb[256];

   show_html_header(lbs, FALSE, loc("Image uploaded successfully"), FALSE, FALSE, NULL, FALSE);

   rsprintf("<script type=\"text/javascript\" src=\"../elcode.js\"></script>\n\n");

   rsprintf("</head>\n");
   rsprintf("<body OnLoad=\"update();\">\n");

   /* find last attachment */
   att[0] = 0;
   for (i = MAX_ATTACHMENTS - 1; i >= 0; i--) {
      sprintf(str, "attachment%d", i);
      if (isparam(str)) {
         strlcpy(att, getparam(str), sizeof(att));
         break;
      }
   }
   strlcpy(str, att, sizeof(str));
   str[13] = 0;
   strcpy(file_enc, att + 14);
   url_encode(file_enc, sizeof(file_enc));      /* for file names with special characters like "+" */
   sprintf(ref, "%s/%s?lb=%s", str, file_enc, lbs->name_enc);
   sprintf(ref_thumb, "%s/%s?lb=%s&thumb=1", str, file_enc, lbs->name_enc);

   compose_base_url(lbs, base_url, sizeof(base_url), TRUE);

   rsprintf("<script type=\"text/javascript\">\n\n");

   rsprintf("  function update()\n");
   rsprintf("  {\n");
   rsprintf("    if (opener.document.title == \"FCKeditor\") {\n");
   rsprintf("       i = opener.parent.next_attachment;\n");
   rsprintf("       opener.FCKeditorAPI.GetInstance('Text').\n");
   rsprintf
       ("InsertHtml('<a href=\"%s\"><img border=0 alt=\"%s\" src=\"%s\" name=\"att'+(i-1)+'\" id=\"att'+(i-1)+'\"></a>');\n",
        ref, att + 14, ref_thumb);
   rsprintf("       opener.parent.document.form1.inlineatt.value = '%s';\n", att);
   rsprintf("       opener.parent.document.form1.jcmd.value = 'Upload';\n");
   rsprintf("       opener.parent.document.form1.submit();\n");
   rsprintf("    } else {\n");
   rsprintf("       i = opener.document.form1.next_attachment.value;\n");
   rsprintf("       elcode2(opener.document, opener.document.form1.Text, 'IMG', 'elog:/'+i);\n");
   rsprintf("       opener.document.form1.inlineatt.value = '%s';\n", att);
   rsprintf("       opener.document.form1.jcmd.value = 'Upload';\n");
   rsprintf("       opener.cond_submit();\n");
   rsprintf("    }\n");
   rsprintf("    window.close();\n");
   rsprintf("  }\n\n");

   /* strings for elcode.js */
   show_browser(browser);

   rsprintf("</script>\n\n");

   rsprintf("<center>\n");
   rsprintf(loc("Image \"%s\" uploaded successfully"), att + 14);
   rsprintf("</center>\n");

   rsprintf("</body></html>\n");
}

/*------------------------------------------------------------------*/

void interprete(char *lbook, char *path)
/********************************************************************
 Routine: interprete

 Purpose: Interprete parametersand generate HTML output.

 Input:
 char *path              Message path

 <implicit>
 _param/_value array accessible via getparam()

 \********************************************************************/
{
   int status, i, j, n, index, lb_index, message_id;
   char exp[80], list[1000], section[256], str[NAME_LENGTH], str1[NAME_LENGTH], str2[NAME_LENGTH],
       edit_id[80], enc_pwd[80], file_name[256], command[256], enc_path[256], dec_path[256], uname[80],
       logbook[256], logbook_enc[256], *experiment, group[256], css[256], *pfile,
       attachment[MAX_PATH_LENGTH], full_name[256], str3[NAME_LENGTH], thumb_name[256];
   BOOL global;
   LOGBOOK *lbs;
   FILE *f;

   /* encode path for further usage */
   strcpy(dec_path, path);
   url_decode(dec_path);
   strcpy(enc_path, dec_path);
   url_encode(enc_path, sizeof(enc_path));
   strencode2(command, isparam("cmd") ? getparam("cmd") : "", sizeof(command));
   strencode2(group, isparam("group") ? getparam("group") : "", sizeof(group));
   index = isparam("index") ? atoi(getparam("index")) : 0;
   experiment = getparam("exp");
   if (getcfg(lbook, "Logging Level", str, sizeof(str)))
      _logging_level = atoi(str);
   else
      _logging_level = 2;
   set_condition("");

   /* evaluate "jcmd" */
   if (isparam("jcmd") && *getparam("jcmd"))
      strlcpy(command, getparam("jcmd"), sizeof(command));

   /* if experiment given, use it as logbook (for elog!) */
   if (experiment && experiment[0]) {
      strcpy(logbook_enc, experiment);
      strcpy(logbook, experiment);
      url_decode(logbook);
      /* check if logbook exists */
      for (i = 0;; i++) {
         if (!enumgrp(i, str))
            break;
         if (strieq(logbook, str))
            break;
      }
      if (!strieq(logbook, str)) {
         sprintf(str, "Error: logbook \"%s\" not defined in %s", logbook_enc, CFGFILE);
         show_error(str);
         return;
      }

   } else {
      strcpy(logbook_enc, lbook);
      strcpy(logbook, lbook);
      url_decode(logbook);
   }

   /* check for top group */
   setcfg_topgroup("");

   sprintf(str, "Top group %s", logbook);
   if (getcfg("global", str, list, sizeof(list))) {
      setcfg_topgroup(logbook);
      logbook[0] = 0;
   }

   /* check if new logbook */
   for (i = j = 0;; i++) {
      if (!enumgrp(i, str))
         break;

      if (is_logbook(str)) {
         /* redo index if logbooks in cfg file do not match lb_list */
         if (!strieq(str, lb_list[j++].name)) {
            el_index_logbooks();
            break;
         }
      }
   }

   /* check for deleted logbook */
   if (lb_list[j].name[0] != 0)
      el_index_logbooks();

   /*---- direct commands (registration etc) ----*/

   if (!logbook[0]) {

      /* check for self register */
      if (getcfg(group, "Self register", str, sizeof(str)) && atoi(str) > 0) {
         if (!do_self_register(NULL, getparam("cmd")))
            return;
      }

      /* check for activate */
      strcpy(str, loc("Activate"));
      if (isparam("cmd") && strieq(getparam("cmd"), loc("Activate"))) {
         if (!save_user_config(NULL, getparam("new_user_name"), TRUE, TRUE))
            return;
         if (isparam("new_user_name"))
            setparam("cfg_user", getparam("new_user_name"));
         show_config_page(NULL);
         return;
      }

      /* check for save after activate */
      if (isparam("cmd") && strieq(getparam("cmd"), loc("Save"))) {
         if (isparam("config")) {
            /* change existing user */
            if (!isparam("config") || !save_user_config(NULL, getparam("config"), FALSE, FALSE))
               return;
         }

         redirect(NULL, ".");
         return;
      }

      /* check for password recovery */
      if (isparam("cmd") || isparam("newpwd")) {
         if (isparam("newpwd") || strieq(getparam("cmd"), loc("Change password"))) {
            show_change_pwd_page(NULL);
            return;
         }
      }

      /* if data from login screen, evaluate it and set cookies */
      if (isparam("uname") && isparam("upassword")) {
         /* check if password correct */
         do_crypt(getparam("upassword"), enc_pwd, sizeof(enc_pwd));
         /* log logins */
         strlcpy(uname, getparam("uname"), sizeof(uname));
         sprintf(str, "LOGIN user \"%s\" (attempt) for logbook selection page", uname);
         write_logfile(NULL, str);
         if (isparam("redir"))
            strlcpy(str, getparam("redir"), sizeof(str));
         else
            strlcpy(str, isparam("cmdline") ? getparam("cmdline") : "", sizeof(str));
         if (!check_user_password(NULL, getparam("uname"), enc_pwd, str))
            return;
         sprintf(str, "LOGIN user \"%s\" (success)", uname);
         write_logfile(NULL, str);
         /* set cookies */
         set_login_cookies(NULL, uname, enc_pwd);
         return;
      }

      /* check for global selection page if no logbook given */
      if (!logbook[0] && getcfg("global", "Selection page", str, sizeof(str))) {
         /* check for URL */
         if (strstr(str, "http://") || strstr(str, "https://")) {
            redirect(NULL, str);
            return;
         }

         /* check if file starts with an absolute directory */
         if (str[0] == DIR_SEPARATOR || str[1] == ':')
            strlcpy(file_name, str, sizeof(file_name));
         else {
            strlcpy(file_name, logbook_dir, sizeof(file_name));
            strlcat(file_name, str, sizeof(file_name));
         }
         send_file_direct(file_name);
         return;
      }

      /* check for global synchronization */
      if (strieq(command, "Synchronize")) {
         synchronize(NULL, SYNC_HTML);
         return;
      }

   }

   /* count logbooks */
   for (n = 0; lb_list[n].name[0]; n++);

   /* if no logbook given, display logbook selection page */
   if (!logbook[0] && !path[0]) {
      if (n > 1) {
         show_selection_page();
         return;
      }

      strcpy(logbook, lb_list[0].name);
      strcpy(logbook_enc, logbook);
      url_encode(logbook_enc, sizeof(logbook_enc));
   }

   /* get logbook from list */
   for (i = 0; lb_list[i].name[0]; i++)
      if (strieq(logbook, lb_list[i].name))
         break;
   lbs = &lb_list[i];

   /* set top level group for logbook */
   if (lbs->top_group[0])
      setcfg_topgroup(lbs->top_group);

   /* get theme for logbook */
   if (getcfg(logbook, "Theme", str, sizeof(str)))
      strlcpy(theme_name, str, sizeof(theme_name));
   else
      strlcpy(theme_name, "default", sizeof(theme_name));
   lb_index = i;
   lbs = lb_list + i;
   lbs->n_attr = scan_attributes(lbs->name);

   if (isparam("wpassword")) {
      /* check if password correct */
      do_crypt(getparam("wpassword"), enc_pwd, sizeof(enc_pwd));
      if (!check_password(lbs, "Write password", enc_pwd, isparam("redir") ? getparam("redir") : ""))
         return;
      rsprintf("HTTP/1.1 302 Found\r\n");
      rsprintf("Server: ELOG HTTP %s-%d\r\n", VERSION, atoi(svn_revision + 13));
      if (use_keepalive) {
         rsprintf("Connection: Keep-Alive\r\n");
         rsprintf("Keep-Alive: timeout=60, max=10\r\n");
      }

      global = getcfg("global", "Write password", str, sizeof(str));

      /* get optional expriation from configuration file */
      getcfg(logbook, "Write password expiration", exp, sizeof(exp));
      /* set "wpwd" cookie */
      set_cookie(lbs, "wpwd", enc_pwd, global, exp);

      /* redirect according to "redir" parameter */
      set_redir(lbs, isparam("redir") ? getparam("redir") : "");
      return;
   }

   if (isparam("apassword")) {
      /* check if password correct */
      do_crypt(getparam("apassword"), enc_pwd, sizeof(enc_pwd));
      if (!check_password(lbs, "Admin password", enc_pwd, isparam("redir") ? getparam("redir") : ""))
         return;
      rsprintf("HTTP/1.1 302 Found\r\n");
      rsprintf("Server: ELOG HTTP %s-%d\r\n", VERSION, atoi(svn_revision + 13));
      if (use_keepalive) {
         rsprintf("Connection: Keep-Alive\r\n");
         rsprintf("Keep-Alive: timeout=60, max=10\r\n");
      }

      global = getcfg("global", "Admin password", str, sizeof(str));

      /* get optional expriation from configuration file */
      getcfg(logbook, "Admin password expiration", exp, sizeof(exp));
      /* set "apwd" cookie */
      set_cookie(lbs, "apwd", enc_pwd, global, exp);

      /* redirect according to "redir" parameter */
      set_redir(lbs, isparam("redir") ? getparam("redir") : "");
      return;
   }

   if (isparam("uname") && isparam("upassword")) {
      /* check if password correct */
      do_crypt(getparam("upassword"), enc_pwd, sizeof(enc_pwd));
      /* log logins */
      strlcpy(uname, getparam("uname"), sizeof(uname));
      sprintf(str, "LOGIN user \"%s\" (attempt)", uname);
      write_logfile(lbs, str);
      if (isparam("redir"))
         strlcpy(str, getparam("redir"), sizeof(str));
      else
         strlcpy(str, isparam("cmdline") ? getparam("cmdline") : "", sizeof(str));
      if (!check_user_password(lbs, uname, enc_pwd, str))
         return;
      sprintf(str, "LOGIN user \"%s\" (success)", uname);
      write_logfile(lbs, str);
      /* set cookies */
      set_login_cookies(lbs, uname, enc_pwd);
      return;
   }

   /* deliver icons without password */
   if (chkext(path, ".gif") || chkext(path, ".jpg") || chkext(path, ".png") || chkext(path, ".ico")
       || chkext(path, ".htm") || chkext(path, ".css")) {
      /* check if file in resource directory */
      strlcpy(str, resource_dir, sizeof(str));
      strlcat(str, path, sizeof(str));
      if (exist_file(str)) {
         send_file_direct(str);
         return;
      } else {
         /* else search file in themes directory */
         strlcpy(str, resource_dir, sizeof(str));
         strlcat(str, "themes", sizeof(str));
         strlcat(str, DIR_SEPARATOR_STR, sizeof(str));
         strlcat(str, theme_name, sizeof(str));
         strlcat(str, DIR_SEPARATOR_STR, sizeof(str));
         strlcat(str, path, sizeof(str));
         if (exist_file(str)) {
            send_file_direct(str);
            return;
         }
      }
   }

   /* check for valid logbook */
   if (!logbook[0]) {
      sprintf(str, "%s: <b>%s</b>", loc("Invalid URL"), path);
      show_error(str);
      return;
   }

   /* if password file given, check password and user name */
   if (getcfg(logbook, "Password file", str, sizeof(str))) {
      /* get current CSS */
      strlcpy(css, "default.css", sizeof(css));
      if (lbs != NULL && getcfg(lbs->name, "CSS", str, sizeof(str)))
         strlcpy(css, str, sizeof(css));
      else if (lbs == NULL && getcfg("global", "CSS", str, sizeof(str)))
         strlcpy(css, str, sizeof(css));

      /* check if guest access */
      if (!(getcfg(lbs->name, "Guest menu commands", str, sizeof(str)) && isparam("unm") == 0
            && !isparam("fail"))) {
         if (strcmp(path, css) != 0) {
            /* if no guest menu commands but self register, evaluate new user commands */
            if (getcfg(lbs->name, "Self register", str, sizeof(str)) && atoi(str) > 0) {
               if (!do_self_register(lbs, command))
                  return;
            }

            if (!check_user_password(lbs, isparam("unm") ? getparam("unm") : "",
                                     isparam("upwd") ? getparam("upwd") : "", _cmdline))
               return;
         }
      }
   }

   if (strieq(command, loc("Login"))) {
      check_user_password(lbs, "", "", path);
      return;
   }

   if (strieq(command, loc("New")) || strieq(command, loc("Edit")) || strieq(command, loc("Reply"))
       || strieq(command, loc("Duplicate")) || strieq(command, loc("Delete")) || strieq(command,
                                                                                        loc("Upload"))
       || strieq(command, loc("Submit")) || strieq(command, loc("Preview"))) {
      sprintf(str, "%s?cmd=%s", path, command);
      if (!check_password(lbs, "Write password", isparam("wpwd") ? getparam("wpwd") : "", str))
         return;
   }

   if (strieq(command, loc("Delete")) || strieq(command, loc("Config")) || strieq(command, loc("Copy to"))
       || strieq(command, loc("Move to"))) {
      sprintf(str, "%s?cmd=%s", path, command);
      if (!check_password(lbs, "Admin password", isparam("apwd") ? getparam("apwd") : "", str))
         return;
   }

   /* check for "Back" button */
   if (strieq(command, loc("Back"))) {
      if (isparam("edit_id")) {

         /* unlock message */
         if (getcfg(lbs->name, "Use Lock", str, sizeof(str)) && atoi(str) == 1)
            el_lock_message(lbs, atoi(getparam("edit_id")), NULL);

         /* redirect to message */
         strlcpy(edit_id, getparam("edit_id"), sizeof(edit_id));
         sprintf(str, "../%s/%s", logbook_enc, edit_id);
      } else
         sprintf(str, "../%s/", logbook_enc);

      if (getcfg(lbs->name, "Back to main", str, sizeof(str)) && atoi(str) == 1)
         strcpy(str, "../");

      redirect(lbs, str);
      return;
   }

   /* check for "List" button */
   if (strieq(command, loc("List"))) {
      show_elog_list(lbs, 0, 0, 0, TRUE, NULL);
      return;
   }

   /* check for "Cancel" button */
   if (strieq(command, loc("Cancel"))) {
      sprintf(str, "../%s/%s", logbook_enc, path);
      redirect(lbs, str);
      return;
   }

   /* check for "Last n*2 Entries" */
   strlcpy(str, isparam("last") ? getparam("last") : "", sizeof(str));
   if (strchr(str, ' ')) {
      i = atoi(strchr(str, ' '));
      sprintf(str, "last%d", i);
      if (isparam("mode")) {
         sprintf(str + strlen(str), "?mode=");
         strlcat(str, getparam("mode"), sizeof(str));
      }
      redirect(lbs, str);
      return;
   }

   strlcpy(str, isparam("past") ? getparam("past") : "", sizeof(str));
   if (strchr(str, ' ')) {
      i = atoi(strchr(str, ' '));
      sprintf(str, "past%d", i);
      redirect(lbs, str);
      return;
   }

   /* check for pastxx */
   if (strncmp(path, "past", 4) == 0 && (isdigit(path[4]) || isdigit(path[5])) 
       && isparam("cmd") == 0) {
      show_elog_list(lbs, atoi(path + 4), 0, 0, FALSE, NULL);
      return;
   }

   if (strncmp(path, "last", 4) == 0 && !chkext(path, ".png") && (!isparam("cmd") || strieq(getparam("cmd"),
                                                                                            loc("Select")))
       && !isparam("newpwd") && atoi(path + 4) > 0) {
      show_elog_list(lbs, 0, atoi(path + 4), 0, FALSE, NULL);
      return;
   }

   if (strncmp(path, "page", 4) == 0 && isparam("cmd") == 0) {
      if (!path[4])
         show_elog_list(lbs, 0, 0, -1, FALSE, NULL);
      else
         show_elog_list(lbs, 0, 0, atoi(path + 4), FALSE, NULL);
      return;
   }

   /* check for calender */
   if (strieq(dec_path, "cal.html")) {
      show_calendar(lbs);
      return;
   }

   /* check for rss-feed */
   if (strieq(dec_path, "elog.rdf")) {
      show_rss_feed(lbs);
      return;
   }

   /* check for upload window */
   if (strieq(dec_path, "upload.html")) {
      show_uploader(lbs);
      return;
   }

   /* check for finished JavaScript upload */
   if (isparam("jcmd") && isparam("jcmd") && strieq(getparam("jcmd"), "JUpload")) {
      show_uploader_finished(lbs);
      return;
   }

   /*---- check if file requested -----------------------------------*/

   /* skip elog message id in front of possible attachment */
   pfile = dec_path;
   if (strchr(pfile, '/') && pfile[13] != '/' && isdigit(pfile[0]))
      pfile = strchr(pfile, '/') + 1;

   if ((strlen(pfile) > 13 && pfile[6] == '_' && pfile[13] == '_') || (strlen(pfile) > 13 && pfile[6] == '_'
                                                                       && pfile[13] == '/')
       || chkext(pfile, ".gif") || chkext(pfile, ".ico") || chkext(pfile, ".jpg")
       || chkext(pfile, ".jpeg") || chkext(pfile, ".png") || chkext(pfile, ".css") || chkext(pfile, ".js")
       || chkext(pfile, ".html")) {
      if ((strlen(pfile) > 13 && pfile[6] == '_' && pfile[13] == '_') || (strlen(pfile) > 13 && pfile[6]
                                                                          == '_' && pfile[13] == '/')) {
         if (pfile[13] == '/')
            pfile[13] = '_';
         /* file from data directory requested */
         strlcpy(file_name, lbs->data_dir, sizeof(file_name));
         strlcat(file_name, pfile, sizeof(file_name));
      } else {
         /* file from theme directory requested */
         strlcpy(file_name, resource_dir, sizeof(file_name));
         if (file_name[0] && file_name[strlen(file_name)
                                       - 1] != DIR_SEPARATOR)
            strlcat(file_name, DIR_SEPARATOR_STR, sizeof(file_name));
         strlcat(file_name, "themes", sizeof(file_name));
         strlcat(file_name, DIR_SEPARATOR_STR, sizeof(file_name));
         if (theme_name[0]) {
            strlcat(file_name, theme_name, sizeof(file_name));
            strlcat(file_name, DIR_SEPARATOR_STR, sizeof(file_name));
         }
         strlcat(file_name, pfile, sizeof(file_name));
      }

      if (isparam("thumb")) {
         get_thumb_name(file_name, thumb_name, sizeof(thumb_name), 0);
         if (thumb_name[0])
            send_file_direct(thumb_name);
         else
            send_file_direct(file_name);
      } else
         send_file_direct(file_name);
      return;
   }

   /* from here on, logbook must be valid */
   if (!logbook[0]) {
      show_selection_page();
      return;
   }

   /*---- check if attachment requested -----------------------------*/

   if (strchr(dec_path, '/')) {
      message_id = atoi(dec_path);
      n = atoi(strchr(dec_path, '/') + 1) - 1;
      status = el_retrieve_attachment(lbs, message_id, n, attachment);
      if (status != EL_SUCCESS || n >= MAX_ATTACHMENTS) {
         sprintf(str, "Attachment #%d of entry #%d not found", n + 1, message_id);
         show_error(str);
      } else {
         if (isparam("thumb"))
            strlcat(attachment, "?thumb=1", sizeof(attachment));
         redirect(lbs, attachment);
      }

      return;
   }

   /* check for new syntax in config file */
   if (getcfg(lbs->name, "Types", str, sizeof(str))) {
      show_upgrade_page(lbs);
      return;
   }

   /* correct for image buttons */
   if (isparam("cmd_first.x"))
      strcpy(command, loc("First"));
   if (isparam("cmd_previous.x"))
      strcpy(command, loc("Previous"));
   if (isparam("cmd_next.x"))
      strcpy(command, loc("Next"));
   if (isparam("cmd_last.x"))
      strcpy(command, loc("Last"));
   /* check if command allowed for current user */
   if (command[0] && !is_user_allowed(lbs, command)) {
      if (isparam("full_name"))
         strlcpy(full_name, getparam("full_name"), sizeof(full_name));
      else
         full_name[0] = 0;

      strencode2(str2, command, sizeof(str2));
      strencode2(str3, full_name, sizeof(str3));
      sprintf(str, loc("Error: Command \"<b>%s</b>\" is not allowed for user \"<b>%s</b>\""), str2, str3);
      show_error(str);
      return;
   }

   /* check if command in menu list */
   if (!is_command_allowed(lbs, command)) {
      /* redirect to login page for new command */
      if (strieq(command, loc("New")) && !isparam("unm")) {
         check_user_password(lbs, "", "", _cmdline);
         return;
      }

      strencode2(str2, command, sizeof(str3));
      sprintf(str, loc("Error: Command \"<b>%s</b>\" not allowed"), str2);
      show_error(str);
      return;
   }

   /*---- check for various commands --------------------------------*/

   if (strieq(command, loc("Help"))) {
      if (getcfg(lbs->name, "Help URL", str, sizeof(str))) {

         /* if URL is given, redirect */
         if (strstr(str, "http://") || strstr(str, "https://")) {
            redirect(lbs, str);
            return;
         }

         /* send file from resource directory */
         strlcpy(file_name, resource_dir, sizeof(file_name));
         strlcat(file_name, "resources", sizeof(file_name));
         strlcat(file_name, DIR_SEPARATOR_STR, sizeof(file_name));
         strlcat(file_name, str, sizeof(file_name));
         f = fopen(file_name, "r");
         if (f == NULL) {
            sprintf(str, "Cannot find file \"%s\"", file_name);
            show_error(str);
         } else {
            fclose(f);
            send_file_direct(file_name);
         }
         return;
      }

      /* send local help file */
      strlcpy(file_name, resource_dir, sizeof(file_name));
      strlcat(file_name, "resources", sizeof(file_name));
      strlcat(file_name, DIR_SEPARATOR_STR, sizeof(file_name));
      strlcat(file_name, "eloghelp_", sizeof(file_name));
      if (getcfg("global", "Language", str, sizeof(str))) {
         for (i = 0; i < (int) strlen(str); i++)
            str[i] = my_tolower(str[i]);
         strlcat(file_name, str, sizeof(file_name));
      } else
         strlcat(file_name, "english", sizeof(file_name));
      strlcat(file_name, ".html", sizeof(file_name));
      f = fopen(file_name, "r");
      if (f == NULL)
         redirect(lbs, "https://midas.psi.ch/elog/eloghelp_english.html");
      else {
         fclose(f);
         send_file_direct(file_name);
      }
      return;
   }

   if (strieq(command, loc("HelpELCode"))) {
      /* send local help file */
      strlcpy(file_name, resource_dir, sizeof(file_name));
      strlcat(file_name, "resources", sizeof(file_name));
      strlcat(file_name, DIR_SEPARATOR_STR, sizeof(file_name));
      strlcat(file_name, "elcode_", sizeof(file_name));
      if (getcfg("global", "Language", str, sizeof(str))) {
         for (i = 0; i < (int) strlen(str); i++)
            str[i] = my_tolower(str[i]);
         strlcat(file_name, str, sizeof(file_name));
      } else
         strlcat(file_name, "english", sizeof(file_name));
      strlcat(file_name, ".html", sizeof(file_name));
      f = fopen(file_name, "r");
      if (f == NULL)
         redirect(lbs, "https://midas.psi.ch/elog/elcode_english.html");
      else {
         fclose(f);
         send_file_direct(file_name);
      }
      return;
   }

   if (strieq(command, loc("New"))) {
      show_edit_form(lbs, 0, FALSE, FALSE, FALSE, FALSE, FALSE, FALSE);
      return;
   }

   /* check for deletion of attachments */
   for (i = 0; i < MAX_ATTACHMENTS; i++) {
      sprintf(str, "delatt%d", i);
      if (isparam(str) || (isparam("smcmd") && stricmp(getparam("smcmd"), str) == 0)) {
         sprintf(str, "attachment%d", i);
         strlcpy(file_name, getparam(str), sizeof(file_name));
         el_delete_attachment(lbs, file_name);
         /* re-order attachments */
         for (j = i; j < MAX_ATTACHMENTS; j++) {
            sprintf(str, "attachment%d", j + 1);
            if (isparam(str))
               strlcpy(file_name, getparam(str), sizeof(file_name));
            else
               file_name[0] = 0;
            sprintf(str, "attachment%d", j);
            if (file_name[0])
               setparam(str, file_name);
            else
               unsetparam(str);
         }

         show_edit_form(lbs, isparam("edit_id") ? atoi(getparam("edit_id")) : 0,
                        FALSE, TRUE, TRUE, FALSE, FALSE, FALSE);
         return;
      }
   }

   message_id = atoi(dec_path);
   if (strieq(command, loc("Upload")) || strieq(command, "Upload")) {
      show_edit_form(lbs, isparam("edit_id") ? atoi(getparam("edit_id")) : 0,
                     FALSE, TRUE, TRUE, FALSE, FALSE, FALSE);
      return;
   }

   if (strieq(command, loc("Edit"))) {
      if (message_id) {
         show_edit_form(lbs, message_id, FALSE, TRUE, FALSE, FALSE, FALSE, FALSE);
         return;
      } else if (isparam("nsel")) {
         show_edit_form(lbs, 0, FALSE, TRUE, FALSE, FALSE, FALSE, FALSE);
         return;
      }
   }

   if (strieq(command, loc("Reply"))) {
      show_edit_form(lbs, message_id, TRUE, FALSE, FALSE, FALSE, FALSE, FALSE);
      return;
   }

   if (strieq(command, loc("Update"))) {
      show_edit_form(lbs, isparam("edit_id") ? atoi(getparam("edit_id")) : 0,
                     FALSE, TRUE, FALSE, TRUE, FALSE, FALSE);
      return;
   }

   if (strieq(command, loc("Duplicate"))) {
      if (message_id) {
         show_edit_form(lbs, message_id, FALSE, FALSE, FALSE, FALSE, TRUE, FALSE);
         return;
      }
   }

   if (strieq(command, loc("Preview"))) {
      show_edit_form(lbs, isparam("edit_id") ? atoi(getparam("edit_id")) : 0,
                     FALSE, TRUE, FALSE, TRUE, FALSE, TRUE);
      return;
   }

   if (strieq(command, loc("Submit")) || strieq(command, "Submit")) {

      if (isparam("mirror_id"))
         submit_elog_mirror(lbs);
      else
         submit_elog(lbs);
      return;
   }

   if (strieq(command, loc("Find"))) {
      /* stip message id */
      if (dec_path[0]) {
         sprintf(str, "../%s/?cmd=%s", lbs->name_enc, loc("Find"));
         redirect(lbs, str);
         return;
      }
      show_find_form(lbs);
      return;
   }

   if (strieq(command, loc("Search"))) {
      if (dec_path[0] && atoi(dec_path) == 0 && strchr(dec_path, '/') != NULL) {
         sprintf(str, "%s: <b>%s</b>", loc("Invalid URL"), dec_path);
         show_error(str);
         return;
      }

      show_elog_list(lbs, 0, 0, 0, TRUE, NULL);
      return;
   }

   if (strieq(command, loc("Last day"))) {
      redirect(lbs, "past1");
      return;
   }

   if (strieq(command, loc("Last 10"))) {
      redirect(lbs, "last10");
      return;
   }

   if (strieq(command, loc("Copy to"))) {
      copy_to(lbs, message_id, isparam("destc") ? getparam("destc") : "", 0, 0);
      return;
   }

   if (strieq(command, loc("Move to"))) {
      copy_to(lbs, message_id, isparam("destm") ? getparam("destm") : "", 1, 0);
      return;
   }

   if (strieq(command, loc("Admin")) || strieq(command, loc("Change config file"))) {
      show_admin_page(lbs, NULL);
      return;
   }

   sprintf(str, loc("Change %s"), "[global]");
   if (strieq(command, str)) {
      show_admin_page(lbs, "global");
      return;
   }

   sprintf(str2, "[global %s]", lbs->top_group);
   sprintf(str, loc("Change %s"), str2);
   if (strieq(command, str)) {
      show_admin_page(lbs, lbs->top_group);
      return;
   }

   if (strieq(command, loc("Delete this logbook"))) {
      show_logbook_delete(lbs);
      return;
   }

   if (strieq(command, loc("Rename this logbook"))) {
      show_logbook_rename(lbs);
      return;
   }

   if (strieq(command, loc("Create new logbook"))) {
      if (isparam("tmp") && strieq(getparam("tmp"), "Cancel")) {

         if (getcfg(lbs->name, "Password file", str, sizeof(str)))
            sprintf(str, "?cmd=%s", loc("Change config file"));
         else
            sprintf(str, "?cmd=%s", loc("Config"));

         redirect(lbs, str);
         return;
      }
      show_logbook_new(lbs);
      return;
   }

   if (strieq(command, "GetPwdFile")) {
      if (get_password_file(lbs, file_name, sizeof(file_name)))
         send_file_direct(file_name);
      return;
   }

   if (strieq(command, loc("Change password")) || (isparam("newpwd") && !strieq(command, loc("Cancel"))
                                                   && !strieq(command, loc("Save")))) {
      show_change_pwd_page(lbs);
      return;
   }

   if (strieq(command, loc("Save"))) {
      if (isparam("config") && isparam("new_user_name")) {
         if (!strieq(getparam("config"), getparam("new_user_name"))) {
            if (get_user_line(lbs, getparam("new_user_name"), NULL, NULL, NULL, NULL, NULL) == 1) {
               sprintf(str, "%s \"%s\" %s", loc("Login name"), getparam("new_user_name"),
                       loc("exists already"));
               show_error(str);
               return;
            }
         }

         /* change existing user */
         if (!save_user_config(lbs, isparam("config") ? getparam("config") : "", FALSE, FALSE))
            return;
      } else if (isparam("new_user_name")) {
         /* new user */
         if (!save_user_config(lbs, getparam("new_user_name"), TRUE, FALSE))
            return;
      } else {

         if (isparam("global")) {
            if (strieq(getparam("global"), "global"))
               strcpy(section, "global");
            else {
               sprintf(section, "global ");
               strlcat(section, getparam("global"), sizeof(section));
            }
         } else
            strlcpy(section, lbs->name, sizeof(section));

         if (!save_admin_config(section, _mtext, str)) {        /* save cfg file */
            show_error(str);
            return;
         }
      }
      if (lbs)
         sprintf(str, "../%s/", lbs->name_enc);
      else
         sprintf(str, ".");
      if (isparam("new_user_name")) {
         sprintf(str + strlen(str), "?cmd=%s&cfg_user=", loc("Config"));
         strlcat(str, getparam("new_user_name"), sizeof(str));
      } else if (isparam("cfg_user")) {
         sprintf(str + strlen(str), "?cmd=%s&cfg_user=", loc("Config"));
         strlcat(str, getparam("cfg_user"), sizeof(str));
      } else if (getcfg(lbs->name, "password file", str2, sizeof(str2)))
         sprintf(str + strlen(str), "?cmd=%s", loc("Config"));

      redirect(lbs, str);
      return;
   }

   if (strieq(command, loc("Activate")) && isparam("new_user_name")) {
      if (!save_user_config(lbs, getparam("new_user_name"), TRUE, TRUE))
         return;
      setparam("cfg_user", getparam("new_user_name"));
      show_config_page(lbs);
      return;
   }

   if (strieq(command, loc("Remove user")) && isparam("config")) {
      if (!remove_user(lbs, getparam("config")))
         return;
      /* if removed user is current user, do logout */
      if (isparam("unm") && strieq(getparam("config"), getparam("unm"))) {
         /* log activity */
         write_logfile(lbs, "LOGOUT");
         /* set cookies */
         set_login_cookies(lbs, "", "");
      }

      /* continue configuration as administrator */
      unsetparam("cfg_user");
      show_config_page(lbs);
      return;
   }

   if (strieq(command, loc("New user"))) {
      show_new_user_page(lbs);
      return;
   }

   if (strieq(command, loc("Forgot"))) {
      show_forgot_pwd_page(lbs);
      return;
   }

   if (strieq(command, loc("Config"))) {
      if (!check_password(lbs, "Write password", isparam("wpwd") ? getparam("wpwd") : "", str))
         return;
      if (!getcfg(lbs->name, "Password file", str, sizeof(str)))
         show_admin_page(lbs, NULL);
      else
         show_config_page(lbs);
      return;
   }

   if (strieq(command, loc("Download")) || strieq(command, "Download")) {
      show_download_page(lbs, dec_path);
      return;
   }

   if (strieq(command, loc("Import"))) {
      strcpy(str, loc("CSV Import"));
      url_encode(str, sizeof(str));
      sprintf(str1, "?cmd=%s", str);

      strcpy(str, loc("XML Import"));
      url_encode(str, sizeof(str));
      sprintf(str2, "?cmd=%s", str);

      show_query(lbs, loc("ELOG import"), loc("Please choose format to import:"), "CSV", str1, "XML", str2);
      return;
   }

   if (strieq(command, loc("CSV Import"))) {
      show_import_page_csv(lbs);
      return;
   }

   if (strieq(command, loc("XML Import"))) {
      show_import_page_xml(lbs);
      return;
   }

   if (strieq(command, "getmd5")) {
      show_md5_page(lbs);
      return;
   }

   if (strieq(command, loc("Synchronize"))) {
      synchronize(lbs, SYNC_HTML);
      return;
   }

   if (strieq(command, loc("Logout"))) {
      /* log activity */
      write_logfile(lbs, "LOGOUT");
      if (getcfg(lbs->name, "Logout to main", str, sizeof(str)) && atoi(str) == 1) {
         sprintf(str, "../");
         setparam("redir", str);
      }
      set_login_cookies(lbs, "", "");
      return;
   }

   if (strieq(command, loc("Delete"))) {
      show_elog_delete(lbs, message_id);
      return;
   }

   if (strieq(command, "IM")) {
      call_image_magick(lbs);
      return;
   }

   /* check for welcome page */
   if (!_cmdline[0] && getcfg(lbs->name, "Welcome page", str, sizeof(str)) && str[0]) {
      /* check if file starts with an absolute directory */
      if (str[0] == DIR_SEPARATOR || str[1] == ':')
         strcpy(file_name, str);
      else {
         strlcpy(file_name, resource_dir, sizeof(file_name));
         strlcat(file_name, str, sizeof(file_name));
      }
      send_file_direct(file_name);
      return;
   }

   /* check for start page */
   if (!_cmdline[0] && getcfg(lbs->name, "Start page", str, sizeof(str)) && str[0]) {
      redirect(lbs, str);
      return;
   }

   /* show page listing or display single entry */
   if (dec_path[0] == 0)
      show_elog_list(lbs, 0, 0, 0, TRUE, NULL);
   else
      show_elog_entry(lbs, dec_path, command);
   return;
}

/*------------------------------------------------------------------*/

void decode_get(char *logbook, char *string)
{
   char path[256];
   char *p, *pitem;

   setparam("cmdline", string);
   strlcpy(path, string, sizeof(path));
   path[255] = 0;
   if (strchr(path, '?'))
      *strchr(path, '?') = 0;
   setparam("path", path);
   if (strchr(string, '?')) {
      p = strchr(string, '?') + 1;
      /* cut trailing "/" from netscape */
      if (p[strlen(p) - 1] == '/')
         p[strlen(p) - 1] = 0;
      p = strtok(p, "&");
      while (p != NULL) {
         pitem = p;
         p = strchr(p, '=');
         if (p != NULL) {
            *p++ = 0;
            url_decode(pitem);
            url_decode(p);
            if (!setparam(pitem, p))
               return;

            p = strtok(NULL, "&");
         }
      }
   }

   interprete(logbook, path);
}

/*------------------------------------------------------------------*/

void decode_post(char *logbook, LOGBOOK * lbs, const char *string, const char *boundary, int length)
{
   int n_att, size, status, header_size;
   const char *pinit, *p, *pctmp, *pbody;
   char *buffer, *ptmp;
   char file_name[MAX_PATH_LENGTH], full_name[MAX_PATH_LENGTH], str[NAME_LENGTH], str2[NAME_LENGTH],
       line[NAME_LENGTH], item[NAME_LENGTH];

   n_att = 0;
   pinit = string;
   /* return if no boundary defined */
   if (!boundary[0])
      return;
   /* skip first boundary */
   if (strstr(string, boundary))
      string = strstr(string, boundary) + strlen(boundary);
   do {
      if (strstr(string, "name=")) {
         strlcpy(line, strstr(string, "name=") + 5, sizeof(line));
         if (strchr(line, '\r'))
            *strchr(line, '\r') = 0;
         if (strchr(line, '\n'))
            *strchr(line, '\n') = 0;
         strlcpy(item, line, sizeof(item));
         if (item[0] == '\"') {
            strlcpy(item, line + 1, sizeof(item));
            if (strchr(item, '\"'))
               *strchr(item, '\"') = 0;
         } else if (strchr(item, ' '))
            *strchr(item, ' ') = 0;
         if (strncmp(item, "attachment", 10) == 0) {
            /* attachment names from previous uploads */
            n_att = atoi(item + 10) + 1;
         }

         if (strncmp(item, "csvfile", 7) == 0 || strncmp(item, "xmlfile", 7) == 0) {
            /* evaluate CSV/XML import file */
            if (strstr(string, "filename=")) {
               p = strstr(string, "filename=") + 9;
               if (*p == '\"')
                  p++;
               if (strstr(p, "\r\n\r\n"))
                  string = strstr(p, "\r\n\r\n") + 4;
               else if (strstr(p, "\r\r\n\r\r\n"))
                  string = strstr(p, "\r\r\n\r\r\n") + 6;
               if (strchr(p, '\"'))
                  *strchr(p, '\"') = 0;
               /* set attachment filename */
               strlcpy(file_name, p, sizeof(file_name));
               if (file_name[0]) {
                  if (verbose)
                     eprintf("decode_post: Found CSV/XML import file\n");
               }

               /* find next boundary */
               pctmp = string;
               do {
                  while (*pctmp != '-')
                     pctmp++;
                  if ((p = strstr(pctmp, boundary)) != NULL) {
                     if (*(p - 1) == '-')
                        p--;
                     while (*p == '-')
                        p--;
                     if (*p == 10)
                        p--;
                     if (*p == 13)
                        p--;
                     p++;
                     break;
                  } else
                     pctmp += strlen(pctmp);
               } while (TRUE);

               /* import CSV/XML file */
               if (file_name[0] && !(isparam("cmd") && strieq(getparam("cmd"), loc("Cancel")))) {
                  if (strncmp(item, "csvfile", 7) == 0) {
                     setparam("csvfile", file_name);
                     csv_import(lbs, string, file_name);
                     return;
                  } else if (strncmp(item, "xmlfile", 7) == 0) {
                     setparam("xmlfile", file_name);
                     xml_import(lbs, string, file_name);
                     return;
                  }
               }

               string = strstr(p, boundary) + strlen(boundary);
            } else
               string = strstr(string, boundary) + strlen(boundary);

         } else if (strncmp(item, "attfile", 7) == 0) {
            /* evaluate file attachment */
            if (strstr(string, "filename=")) {
               p = strstr(string, "filename=") + 9;
               if (*p == '\"')
                  p++;
               if (strstr(p, "\r\n\r\n"))
                  string = strstr(p, "\r\n\r\n") + 4;
               else if (strstr(p, "\r\r\n\r\r\n"))
                  string = strstr(p, "\r\r\n\r\r\n") + 6;
               if (strchr(p, '\"'))
                  *strchr(p, '\"') = 0;
               /* set attachment filename */
               strlcpy(file_name, p, sizeof(file_name));

               /* remove spaces */
               btou(file_name);
               if (file_name[0]) {
                  if (verbose)
                     eprintf("decode_post: Found attachment %s\n", file_name);
                  /* check filename for invalid characters */
                  if (strpbrk(file_name, ",;+=")) {
                     strencode2(str2, file_name, sizeof(str2));
                     sprintf(str, "Error: Filename \"%s\" contains invalid character", str2);
                     show_error(str);
                     return;
                  }
               }

               /* find next boundary */
               pctmp = string;
               do {
                  while (*pctmp != '-')
                     pctmp++;
                  if ((p = strstr(pctmp, boundary)) != NULL) {
                     if (*(p - 1) == '-')
                        p--;
                     while (*p == '-')
                        p--;
                     if (*p == 10)
                        p--;
                     if (*p == 13)
                        p--;
                     p++;
                     break;
                  } else
                     pctmp += strlen(pctmp);
               } while (TRUE);

               /* check attachment size */
               if (file_name[0] && (p - string) == 0) {

                  /* check for URL */
                  if (stristr(file_name, "http://") || stristr(file_name, "https://")) {
                     size = retrieve_url(file_name, &buffer, NULL);
                     if (size <= 0) {
                        strencode2(str2, file_name, sizeof(str2));
                        sprintf(str, loc("Cannot retrieve file from URL \"%s\""), str2);
                        show_error(str);
                        return;
                     }

                     /* check for HTTP header */
                     pbody = strstr(buffer, "\r\n\r\n");
                     if (!pbody) {
                        show_error(loc("Invalid HTTP header"));
                        xfree(buffer);
                        return;
                     }
                     pbody += 4;
                     header_size = pbody - buffer;

                     /* check for file found */
                     if (strchr(buffer, ' ')) {
                        status = atoi(strchr(buffer, ' ') + 1);
                        if (status != 200) {
                           strencode2(str2, file_name, sizeof(str2));
                           sprintf(str, loc("File not found at URL \"%s\""), str2);
                           show_error(str);
                           return;
                        }
                     }

                     el_submit_attachment(lbs, file_name, pbody, size - header_size, full_name);
                     xfree(buffer);
                     sprintf(str, "attachment%d", n_att++);
                     setparam(str, full_name);
                  } else {
                     strencode2(str2, file_name, sizeof(str2));
                     sprintf(str, loc("Attachment file <b>\"%s\"</b> empty or not found"), str2);
                     show_error(str);
                     return;
                  }
               } else if (file_name[0]) {
                  /* save attachment */
                  el_submit_attachment(lbs, file_name, string, (int) (p - string), full_name);
                  sprintf(str, "attachment%d", n_att++);
                  setparam(str, full_name);
               }

               string = strstr(p, boundary) + strlen(boundary);
            } else
               string = strstr(string, boundary) + strlen(boundary);
         } else {
            p = string;
            if (strstr(p, "\r\n\r\n"))
               p = strstr(p, "\r\n\r\n") + 4;
            else if (strstr(p, "\r\r\n\r\r\n"))
               p = strstr(p, "\r\r\n\r\r\n") + 6;
            if (strstr(p, boundary)) {

               string = strstr(p, boundary) + strlen(boundary);

               if (stricmp(item, "text") == 0) {
                  if (string - p > TEXT_SIZE) {
                     sprintf(str,
                             "Error: Entry text too big. Please increase TEXT_SIZE and recompile elogd\n");
                     show_error(str);
                     return;
                  }
                  strlcpy(_mtext, p, sizeof(_mtext));
                  if (strstr(_mtext, boundary))
                     *strstr(_mtext, boundary) = 0;
                  ptmp = _mtext + (strlen(_mtext) - 1);
                  while (*ptmp == '-')
                     *ptmp-- = 0;
                  while (*ptmp == '\n' || *ptmp == '\r')
                     *ptmp-- = 0;
               } else {
                  strlcpy(str, p, sizeof(str));
                  if (strstr(str, boundary))
                     *strstr(str, boundary) = 0;
                  ptmp = str + (strlen(str) - 1);
                  while (*ptmp == '-')
                     *ptmp-- = 0;
                  while (*ptmp == '\n' || *ptmp == '\r')
                     *ptmp-- = 0;

                  if (setparam(item, str) == 0)
                     return;
               }
            }
         }

         while (*string == '-' || *string == '\n' || *string == '\r')
            string++;
      } else
         return;                /* invalid request */

   } while ((int) (string - pinit) < length);

   if (lbs)
      interprete(lbs->name, "");
   else
      interprete(logbook, "");
}

/*------------------------------------------------------------------*/

#define N_MAX_CONNECTION 10
#define KEEP_ALIVE_TIME  60

int ka_sock[N_MAX_CONNECTION];
int ka_time[N_MAX_CONNECTION];
#ifdef HAVE_SSL
SSL *ka_ssl_con[N_MAX_CONNECTION];
#endif
struct in_addr remote_addr[N_MAX_CONNECTION];
char remote_host[N_MAX_CONNECTION][256];

int process_http_request(const char *request, int i_conn)
{
   int i, n, authorized, header_length, content_length;
   char str[1000], str2[1000], url[2000], pwd[256], cl_pwd[256], format[256], cookie[256], boundary[256],
       list[1000], theme[256], host_list[MAX_N_LIST][NAME_LENGTH], logbook[256], logbook_enc[256],
       global_cmd[256];
   char *p;
   struct hostent *phe;
   time_t now;
   struct tm *ts;

   if (!strchr(request, '\r'))
      return 0;

   if (verbose == 1) {
      strlcpy(str, request, sizeof(str));
      if (strchr(str, '\r'))
         *strchr(str, '\r') = 0;
      if (strchr(str, '\n'))
         *strchr(str, '\n') = 0;
      eputs(str);
   } else if (verbose > 1) {
      eputs("\n");
      eputs(request);
   }

   /* initialize parametr array */
   initparam();
   content_length = 0;

   /* extract cookies */
   if ((p = strstr(request, "Cookie:")) != NULL) {
      p += 6;
      do {
         p++;
         while (*p && *p == ' ')
            p++;
         strlcpy(str, p, sizeof(str));
         for (i = 0; i < (int) strlen(str); i++)
            if (str[i] == '=' || str[i] == ';')
               break;
         if (str[i] == '=') {
            str[i] = 0;
            p += i + 1;
            for (i = 0; *p && *p != ';' && *p != '\r' && *p != '\n' && i < (int) sizeof(cookie)-1; i++)
               cookie[i] = *p++;
            cookie[i] = 0;
         } else {
            /* empty cookie */
            str[i] = 0;
            cookie[0] = 0;
            p += i;
         }

         /* store cookie as parameter */
         setparam(str, cookie);
      } while (*p && *p == ';');
   }

   /* extract referer */
   referer[0] = 0;
   if ((p = strstr(request, "Referer:")) != NULL) {
      p += 9;
      while (*p && *p == ' ')
         p++;
      strlcpy(referer, p, sizeof(referer));
      if (strchr(referer, '\r'))
         *strchr(referer, '\r') = 0;
      if (strchr(referer, '?'))
         *strchr(referer, '?') = 0;
      for (p = referer + strlen(referer) - 1; p > referer && *p != '/'; p--)
         *p = 0;
      if (strchr(referer, ' '))
         url_encode(referer, sizeof(referer));
   }

   /* extract browser */
   browser[0] = 0;
   if ((p = strstr(request, "User-Agent:")) != NULL) {
      p += 11;
      while (*p && *p == ' ')
         p++;
      strlcpy(browser, p, sizeof(browser));
      if (strchr(browser, '\r'))
         *strchr(browser, '\r') = 0;
   }

   /* extract host */
   http_host[0] = 0;
   if ((p = strstr(request, "Host:")) != NULL) {
      p += 5;
      while (*p && *p == ' ')
         p++;
      strlcpy(http_host, p, sizeof(http_host));
      if (strchr(http_host, '\r'))
         *strchr(http_host, '\r') = 0;
   }

   /* extract X-Forwarded-Host, overwrite "Host:" if found */
   if ((p = strstr(request, "X-Forwarded-Host:")) != NULL) {
      p += 17;
      while (*p && *p == ' ')
         p++;
      strlcpy(http_host, p, sizeof(http_host));
      if (strchr(http_host, '\r'))
         *strchr(http_host, '\r') = 0;
   }

   /* extract "X-Forwarded-For:" */
   if ((p = strstr(request, "X-Forwarded-For:")) != NULL) {
      p += 16;
      while (*p && *p == ' ')
         p++;
      strlcpy(str, p, sizeof(str));
      if (strchr(str, '\r'))
         *strchr(str, '\r') = 0;
#ifdef OS_WINNT
      rem_addr.S_un.S_addr = inet_addr(str);
#else
      rem_addr.s_addr = inet_addr(str);
#endif

      if (getcfg("global", "Resolve host names", str, sizeof(str)) && atoi(str) == 1) {
         phe = gethostbyaddr((char *) &rem_addr, 4, PF_INET);
         if (phe != NULL)
            strcpy(remote_host[i_conn], phe->h_name);
         else
            strcpy(remote_host[i_conn], (char *) inet_ntoa(rem_addr));
      } else
         strcpy(remote_host[i_conn], (char *) inet_ntoa(rem_addr));

      strcpy(rem_host, remote_host[i_conn]);
   }

   if (_logging_level > 3) {
      strlcpy(str, request, sizeof(str));
      if (strchr(str, '\r'))
         *strchr(str, '\r') = 0;
      write_logfile(NULL, str);
   }

   memset(return_buffer, 0, return_buffer_size);
   strlen_retbuf = 0;
   if (strncmp(request, "GET", 3) != 0 && strncmp(request, "POST", 4) != 0)
      return 0;

   return_length = 0;

   /* check for Keep-alive */
   if (strstr(request, "Keep-Alive") != NULL && use_keepalive)
      keep_alive = TRUE;

   /* extract logbook */
   if (strchr(request, '/') == NULL || strchr(request, '\r') == NULL || strstr(request, "HTTP") == NULL) {
      /* invalid request, make valid */
      return process_http_request("GET / HTTP/1.0\r\n\r\n", i_conn);
   }

   /* initialize topgroups */
   setcfg_topgroup("");

   p = strchr(request, '/') + 1;

   /* check for ../.. to avoid serving of files on top of the elog directory */
   for (i = 0; p[i] && p[i] != ' ' && p[i] != '?' && i < (int) sizeof(url); i++)
      url[i] = p[i];
   url[i] = 0;

   if (strstr(url, "../..")) {
      strencode2(str2, url, sizeof(str2));
      sprintf(str, "%s: <b>%s</b>", loc("Invalid URL"), str2);
      show_error(str);
      return 1;
   }

   /* check if file is in scripts directory or in its subdirs */
   for (i = 0; p[i] && p[i] != ' ' && p[i] != '?' && i < (int) sizeof(url); i++)
      url[i] = (p[i] == '/') ? DIR_SEPARATOR : p[i];
   url[i] = 0;
   if (strchr(url, '.')) {

      /* do not allow '..' in file name */
      if (strstr(url, "..")) {
         strencode2(str2, url, sizeof(str2));
         sprintf(str, "%s: <b>%s</b>", loc("Invalid URL"), str2);
         show_error(str);
         return 1;
      }

      strlcpy(str, resource_dir, sizeof(str));
      strlcat(str, "scripts", sizeof(str));
      strlcat(str, DIR_SEPARATOR_STR, sizeof(str));
      strlcat(str, url, sizeof(str));
      if (exist_file(str)) {
         send_file_direct(str);
         return 1;
      }
   }

   logbook[0] = 0;
   for (i = 0; *p && *p != '/' && *p != '?' && *p != ' ' && i < (int) sizeof(logbook); i++)
      logbook[i] = *p++;
   logbook[i] = 0;
   strcpy(logbook_enc, logbook);
   url_decode(logbook);
   /* check for trailing '/' after logbook */
   if (strncmp(request, "POST", 4) != 0) {      // fix for konqueror
      if (logbook[0] && *p == ' ') {
         if (!chkext(logbook, ".css") && !chkext(logbook, ".htm") && !chkext(logbook, ".gif")
             && !chkext(logbook, ".jpg") && !chkext(logbook, ".png") && !chkext(logbook, ".ico")) {
            sprintf(str, "%s/", logbook_enc);
            redirect(NULL, str);
            return 1;
         }
      }
   }

   /* check for trailing '/' after logbook/ID */
   if (logbook[0] && *p == '/' && *(p + 1) != ' ') {
      sprintf(url, "%s", logbook_enc);
      for (i = strlen(url); *p && *p != ' ' && i < (int) sizeof(url); i++)
         url[i] = *p++;
      url[i] = 0;
      if (*(p - 1) == '/') {
         strencode2(str2, url, sizeof(str2));
         sprintf(str, "%s: <b>%s</b>", loc("Invalid URL"), str2);
         show_error(str);
         return 1;
      }
   }

   /* check for invalid URL in the form "http:/server//path" */
   if (!logbook[0] && *p == '/') {
      for (i = 0; *p && *p != ' ' && i < (int) sizeof(url); i++)
         url[i] = *p++;
      url[i] = 0;
      strencode2(str2, url, sizeof(str2));
      sprintf(str, "%s: <b>%s</b>", loc("Invalid URL"), str2);
      show_error(str);
      return 1;
   }

   /* check for global command */
   global_cmd[0] = 0;
   if ((p = strstr(request, "?cmd=")) != NULL) {
      p += 5;
      strlcpy(global_cmd, p, sizeof(global_cmd));
      if (strchr(global_cmd, ' '))
         *strchr(global_cmd, ' ') = 0;
      if (strchr(global_cmd, '\r'))
         *strchr(global_cmd, '\r') = 0;
   }

   /* redirect image request from inside FCKeditor */
   if (strieq(logbook, "fckeditor")) {
      if (strstr(url, "?lb=")) {
         strlcpy(logbook, strstr(url, "?lb=") + 4, sizeof(logbook));
         if (strchr(logbook, '&'))
            *strchr(logbook, '&') = 0;
         url_decode(logbook);
      }
   }

   /* check if logbook exists */
   for (i = 0;; i++) {
      if (!enumgrp(i, str))
         break;
      if (strieq(logbook, str) && is_logbook(logbook))
         break;
   }

   if (chkext(logbook, ".gif") || chkext(logbook, ".jpg") || chkext(logbook, ".jpg") || chkext(logbook,
                                                                                               ".png")
       || chkext(logbook, ".ico") || chkext(logbook, ".htm") || chkext(logbook, ".css")
       || chkext(logbook, ".js")) {

      /* do not allow '..' in file name */
      if (strstr(logbook, "..")) {
         strencode2(str2, logbook, sizeof(str2));
         sprintf(str, "%s: <b>%s</b>", loc("Invalid URL"), str2);
         show_error(str);
         return 1;
      }

      /* check if file in resource directory */
      strlcpy(str, resource_dir, sizeof(str));
      strlcat(str, logbook, sizeof(str));
      if (exist_file(str))
         send_file_direct(str);
      else {
         /* else search file in themes directory */
         strlcpy(str, resource_dir, sizeof(str));
         strlcat(str, "themes", sizeof(str));
         strlcat(str, DIR_SEPARATOR_STR, sizeof(str));
         if (getcfg("global", "theme", theme, sizeof(theme)))
            strlcat(str, theme, sizeof(str));
         else
            strlcat(str, "default", sizeof(str));
         strlcat(str, DIR_SEPARATOR_STR, sizeof(str));
         strlcat(str, logbook, sizeof(str));
         send_file_direct(str);
      }

      return 1;

   } else {
      if (logbook[0] && (!strieq(logbook, str) || !is_logbook(logbook))) {

         /* check for top group */
         sprintf(str, "Top group %s", logbook);
         if (!getcfg("global", str, list, sizeof(list))) {

            sprintf(str, "Error: logbook \"%s\" not defined in %s", logbook_enc, CFGFILE);
            show_error(str);
            return 1;
         }
      }
   }

   /* if no logbook is given and only one logbook defined, use this one */
   if (!logbook[0] && !global_cmd[0]) {
      for (i = n = 0;; i++) {
         if (!enumgrp(i, str))
            break;
         if (is_logbook(str))
            n++;
      }

      if (n == 1) {
         strlcpy(logbook, str, sizeof(logbook));
         strlcpy(logbook_enc, logbook, sizeof(logbook_enc));
         url_encode(logbook_enc, sizeof(logbook_enc));
         strlcat(logbook_enc, "/", sizeof(logbook_enc));
         /* redirect to logbook, necessary to get optional cookies for that logbook */
         redirect(NULL, logbook_enc);
         return 1;
      }
   }

   /*---- check "hosts deny" ----*/

   authorized = 1;
   if (getcfg(logbook, "Hosts deny", list, sizeof(list))) {
      strcpy(rem_host_ip, (char *) inet_ntoa(rem_addr));
      n = strbreak(list, host_list, MAX_N_LIST, ",", FALSE);
      /* check if current connection matches anyone on the list */
      for (i = 0; i < n; i++) {
         if (strieq(rem_host, host_list[i]) || strieq(rem_host_ip, host_list[i]) || strieq(host_list[i],
                                                                                           "all")) {
            if (verbose)
               eprintf("Remote host \"%s\" matches \"%s\" in \"Hosts deny\". Access denied.\n",
                       strieq(rem_host_ip, host_list[i]) ? rem_host_ip : rem_host, host_list[i]);
            authorized = 0;
            break;
         }
         if (host_list[i][0] == '.') {
            if (strlen(rem_host) > strlen(host_list[i]) && strieq(host_list[i], rem_host + strlen(rem_host)
                                                                  - strlen(host_list[i]))) {
               if (verbose)
                  eprintf("Remote host \"%s\" matches \"%s\" in \"Hosts deny\". Access denied.\n", rem_host,
                          host_list[i]);
               authorized = 0;
               break;
            }
         }
         if (host_list[i][strlen(host_list[i]) - 1] == '.') {
            strcpy(str, rem_host_ip);
            if (strlen(str) > strlen(host_list[i]))
               str[strlen(host_list[i])] = 0;
            if (strieq(host_list[i], str)) {
               if (verbose)
                  eprintf("Remote host \"%s\" matches \"%s\" in \"Hosts deny\". Access denied.\n",
                          rem_host_ip, host_list[i]);
               authorized = 0;
               break;
            }
         }
      }
   }

   /*---- check "hosts allow" ----*/

   if (getcfg(logbook, "Hosts allow", list, sizeof(list))) {
      strcpy(rem_host_ip, (char *) inet_ntoa(rem_addr));
      n = strbreak(list, host_list, MAX_N_LIST, ",", FALSE);
      /* check if current connection matches anyone on the list */
      for (i = 0; i < n; i++) {
         if (strieq(rem_host, host_list[i]) || strieq(rem_host_ip, host_list[i]) || strieq(host_list[i],
                                                                                           "all")) {
            if (verbose)
               eprintf("Remote host \"%s\" matches \"%s\" in \"Hosts allow\". Access granted.\n",
                       strieq(rem_host_ip, host_list[i]) ? rem_host_ip : rem_host, host_list[i]);
            authorized = 1;
            break;
         }
         if (host_list[i][0] == '.') {
            if (strlen(rem_host) > strlen(host_list[i]) && strieq(host_list[i], rem_host + strlen(rem_host)
                                                                  - strlen(host_list[i]))) {
               if (verbose)
                  eprintf("Remote host \"%s\" matches \"%s\" in \"Hosts allow\". Access granted.\n",
                          rem_host, host_list[i]);
               authorized = 1;
               break;
            }
         }
         if (host_list[i][strlen(host_list[i]) - 1] == '.') {
            strcpy(str, rem_host_ip);
            if (strlen(str) > strlen(host_list[i]))
               str[strlen(host_list[i])] = 0;
            if (strieq(host_list[i], str)) {
               if (verbose)
                  eprintf("Remote host \"%s\" matches \"%s\" in \"Hosts allow\". Access granted.\n",
                          rem_host_ip, host_list[i]);
               authorized = 1;
               break;
            }
         }
      }
   }

   if (!authorized) {
      keep_alive = FALSE;
      return 0;
   }

   /* ask for password if configured */
   authorized = 1;
   if (getcfg(logbook, "Read Password", pwd, sizeof(pwd))) {
      authorized = 0;
      /* decode authorization */
      if (strstr(request, "Authorization:")) {
         p = strstr(request, "Authorization:") + 14;
         if (strstr(p, "Basic")) {
            p = strstr(p, "Basic") + 6;
            while (*p == ' ')
               p++;
            i = 0;
            while (*p && *p != ' ' && *p != '\r' && i < (int) sizeof(cl_pwd) - 1)
               str[i++] = *p++;
            str[i] = 0;
         }
         base64_decode(str, cl_pwd);
         if (strchr(cl_pwd, ':')) {
            p = strchr(cl_pwd, ':') + 1;
            do_crypt(p, str, sizeof(str));
            strcpy(cl_pwd, str);
            /* check authorization */
            if (strcmp(str, pwd) == 0)
               authorized = 1;
         }
      }
   }

   if (!authorized) {
      /* return request for authorization */
      rsprintf("HTTP/1.1 401 Authorization Required\r\n");
      rsprintf("Server: ELOG HTTP %s-%d\r\n", VERSION, atoi(svn_revision + 13));
      rsprintf("WWW-Authenticate: Basic realm=\"%s\"\r\n", logbook);
      rsprintf("Connection: close\r\n");
      rsprintf("Content-Type: text/html\r\n\r\n");
      rsprintf("<HTML><HEAD>\r\n");
      rsprintf("<TITLE>401 Authorization Required</TITLE>\r\n");
      rsprintf("</HEAD><BODY>\r\n");
      rsprintf("<H1>Authorization Required</H1>\r\n");
      rsprintf("This server could not verify that you\r\n");
      rsprintf("are authorized to access the document\r\n");
      rsprintf("requested. Either you supplied the wrong\r\n");
      rsprintf("credentials (e.g., bad password), or your\r\n");
      rsprintf("browser doesn't understand how to supply\r\n");
      rsprintf("the credentials required.<P>\r\n");
      rsprintf("</BODY></HTML>\r\n");
      keep_alive = FALSE;

   } else {

      if (!logbook[0] && global_cmd[0] && stricmp(global_cmd, "GetConfig") == 0) {
         download_config();
      } else if (stricmp(global_cmd, "gettimedate") == 0) {
         if (!getcfg(logbook, "Time format", format, sizeof(format)))
            strcpy(format, DEFAULT_TIME_FORMAT);
         time(&now);
         ts = localtime(&now);
         my_strftime(str, sizeof(str), format, ts);
         show_http_header(NULL, FALSE, NULL);
         rsputs(str);
         rsputs(" ");
      } else if (strncmp(request, "GET", 3) == 0) {
         /* extract path and commands */
         if (strchr(request, '\r'))
            *strchr(request, '\r') = 0;
         if (!strstr(request, "HTTP/1"))
            return 0;
         *(strstr(request, "HTTP/1") - 1) = 0;
         /* strip logbook from path */
         strlcpy(str, request + 5, sizeof(str));
         p = str;
         for (i = 0; *p && *p != '/' && *p != '?'; p++);
         while (*p && *p == '/')
            p++;
         if (strncmp(p, "editor/", 7) == 0)     // fix for image request inside FCKeditor
            p += 7;
         /* decode command and return answer */
         decode_get(logbook, p);
      } else if (strncmp(request, "POST", 4) == 0) {

         /* extract content length */
         if (strstr(request, "Content-Length:"))
            content_length = atoi(strstr(request, "Content-Length:") + 15);
         else if (strstr(request, "Content-length:"))
            content_length = atoi(strstr(request, "Content-length:") + 15);

         /* extract header length */
         if (strstr(request, "\r\n\r\n"))
            header_length = strstr(request, "\r\n\r\n") - request + 4;
         else if (strstr(request, "\r\r\n\r\r\n"))
            header_length = strstr(request, "\r\r\n\r\r\n") - request + 6;
         else {
            show_error("Invalid POST header");
            return 1;
         }

         /* extract boundary */
         if (strstr(request, "boundary=")) {
            strlcpy(boundary, strstr(request, "boundary=") + 9, sizeof(boundary));
            if (strchr(boundary, '\r'))
               *strchr(boundary, '\r') = 0;
         }

         /* get logbook from list (needed for attachment dir) */
         for (i = 0; lb_list[i].name[0]; i++)
            if (strieq(logbook, lb_list[i].name))
               break;
         if (!lb_list[i].name[0])
            /* must be login page of top group */
            decode_post(logbook, NULL, request + header_length, boundary, content_length);
         else
            decode_post(logbook, &lb_list[i], request + header_length, boundary, content_length);
      } else {
         strencode2(str2, request, sizeof(str2));
         sprintf(str, "Unknown request:<p>%s", str2);
         show_error(str);
      }
   }

   return 1;
}

/*------------------------------------------------------------------*/

#ifdef HAVE_SSL
void send_return(int _sock, SSL * ssl_con, const char *net_buffer)
#else
void send_return(int _sock, const char *net_buffer)
#endif
{
   int length, header_length;
   char str[NAME_LENGTH];
   char *p;

   if (return_length != -1) {
      if (return_length == 0)
         return_length = strlen_retbuf;

      if (_logging_level > 3) {
         strlcpy(str, net_buffer, sizeof(str));
         sprintf(str, "Return %d bytes", return_length);
         write_logfile(NULL, str);
      }

      if ((keep_alive && strstr(return_buffer, "Content-Length") == NULL) || strstr(return_buffer,
                                                                                    "Content-Length") >
          strstr(return_buffer, "\r\n\r\n")) {
         /*---- add content-length ----*/

         p = strstr(return_buffer, "\r\n\r\n");
         if (p != NULL) {
            length = strlen(p + 4);
            header_length = (int) (p - return_buffer);
            if (header_length + 100 > (int) sizeof(header_buffer))
               header_length = sizeof(header_buffer) - 100;
            memcpy(header_buffer, return_buffer, header_length);
            sprintf(header_buffer + header_length, "\r\nContent-Length: %d\r\n\r\n", length);
#ifdef HAVE_SSL
            if (_ssl_flag) {
               SSL_write(ssl_con, header_buffer, strlen(header_buffer));
               SSL_write(ssl_con, p + 4, length);
            } else {
               send(_sock, header_buffer, strlen(header_buffer), 0);
               send(_sock, p + 4, length, 0);
            }
#else
            send(_sock, header_buffer, strlen(header_buffer), 0);
            send(_sock, p + 4, length, 0);
#endif
            if (verbose == 1) {
               eprintf("Returned %d bytes\n", length);
            } else if (verbose == 2) {
               if (strrchr(net_buffer, '/'))
                  strlcpy(str, strrchr(net_buffer, '/') + 1, sizeof(str));
               else
                  str[0] = 0;
               eprintf("==== Return ================================\n");
               eputs(header_buffer);
               if (chkext(net_buffer, ".gif") || chkext(net_buffer, ".jpg") || chkext(net_buffer, ".png")
                   || chkext(net_buffer, ".ico") || chkext(net_buffer, ".pdf") || return_length > 10000)
                  eprintf("\n<%d bytes of %s>\n\n", length, str);
               else
                  eputs(p + 4);
               eprintf("\n");
            }
         } else {
            eprintf("Internal error, no valid header!\n");
            keep_alive = FALSE;
         }
      } else {
#ifdef HAVE_SSL
         if (_ssl_flag)
            SSL_write(ssl_con, return_buffer, return_length);
         else
            send(_sock, return_buffer, return_length, 0);
#else
         send(_sock, return_buffer, return_length, 0);
#endif
         if (verbose == 1) {
            eprintf("Returned %d bytes\n", return_length);
         } else if (verbose == 2) {
            if (strrchr(net_buffer, '/'))
               strlcpy(str, strrchr(net_buffer, '/') + 1, sizeof(str));
            else
               str[0] = 0;
            eprintf("==== Return ================================\n");
            if (chkext(net_buffer, ".gif") || chkext(net_buffer, ".jpg") || chkext(net_buffer, ".png")
                || chkext(net_buffer, ".ico") || chkext(net_buffer, ".pdf") || return_length > 10000)
               eprintf("\n<%d bytes of %s>\n\n", return_length, str);
            else
               eputs(return_buffer);
            eprintf("\n\n");
         }
      }
   }
}

/*------------------------------------------------------------------*/

BOOL cron_match(char *str, int value, BOOL ignore_star)
{
   int low, high;

   if (atoi(str) == value)
      return TRUE;

   if (!ignore_star && str[0] == '*')
      return TRUE;

   /* check range */
   if (strchr(str, '-')) {
      low = atoi(str);
      high = atoi(strchr(str, '-') + 1);
      return value >= low && value <= high;
   }

   return FALSE;
}

void check_cron()
/* check 'mirror cron' etnry in configuration file

 minute         (0-59)
 hour           (0-23)
 day of month   (1-31)
 month of year  (1-12)
 day of week    (0-6, 0 is Sunday)

 Each of these patterns might be an asterisk (meaning all legal values)
 or a list of elements separated by commas.
 */
{
   int i, j, n;
   BOOL min_flag, hour_flag, day_flag, mon_flag, wday_flag;
   time_t now;
   char *p, str[256], cron[5][256];
   struct tm *ts;
   static struct tm last_time;
   char list[60][NAME_LENGTH];

   if (!getcfg("global", "mirror cron", str, sizeof(str)))
      return;

   for (i = 0; i < 5; i++)
      strcpy(cron[i], "*");

   i = 0;
   p = strtok(str, " ");
   while (p) {
      strcpy(cron[i++], p);
      p = strtok(NULL, " ");
   }

   time(&now);
   ts = localtime(&now);
   assert(ts);

   /* check once every minute */
   if (last_time.tm_year && last_time.tm_min != ts->tm_min) {

      min_flag = hour_flag = day_flag = mon_flag = wday_flag = FALSE;

      for (i = 0; i < 5; i++) {
         n = strbreak(cron[i], list, 60, ",", FALSE);

         for (j = 0; j < n; j++) {
            /* minutes */
            if (i == 0 && cron_match(list[j], ts->tm_min, FALSE))
               min_flag = TRUE;

            /* hours */
            if (i == 1 && cron_match(list[j], ts->tm_hour, FALSE))
               hour_flag = TRUE;

            /* day of month */
            if (i == 2 && cron_match(list[j], ts->tm_mday, FALSE))
               day_flag = TRUE;

            /* month of year */
            if (i == 3 && cron_match(list[j], ts->tm_mon, FALSE))
               mon_flag = TRUE;

            /* weekday */
            if (i == 4 && cron_match(list[j], ts->tm_wday, TRUE))
               wday_flag = TRUE;
         }
      }

      if (min_flag && hour_flag && ((day_flag && mon_flag) || wday_flag)) {

         rem_host[0] = 0;
         write_logfile(NULL, "Cron job started");

         /* synchronize all logbooks */
         setcfg_topgroup("");
         synchronize(NULL, SYNC_CRON);
      }
   }

   memcpy(&last_time, ts, sizeof(struct tm));
}

/*------------------------------------------------------------------*/

BOOL _abort = FALSE;
BOOL _hup = FALSE;

void ctrlc_handler(int sig)
{
   if (sig)
      _abort = TRUE;
}

void hup_handler(int sig)
{
   if (sig)
      _hup = TRUE;
}

/*------------------------------------------------------------------*/

#ifdef HAVE_SSL

SSL_CTX *init_ssl(void)
{
   char str[256];
   SSL_METHOD *meth;
   SSL_CTX *ctx;

   SSL_library_init();
   SSL_load_error_strings();

   meth = SSLv23_method();
   ctx = SSL_CTX_new(meth);

   strlcpy(str, resource_dir, sizeof(str));
   strlcat(str, "ssl/server.crt", sizeof(str));
   if (!file_exist(str)) {
      eprintf("Cerificate file \"%s\" not found, aborting\n", str);
      return NULL;
   }
   if (SSL_CTX_use_certificate_file(ctx, str, SSL_FILETYPE_PEM) < 0)
      return NULL;

   strlcpy(str, resource_dir, sizeof(str));
   strlcat(str, "ssl/server.key", sizeof(str));
   if (!file_exist(str)) {
      eprintf("Key file \"%s\" not found, aborting\n", str);
      return NULL;
   }
   if (SSL_CTX_use_PrivateKey_file(ctx, str, SSL_FILETYPE_PEM) < 0)
      return NULL;
   if (SSL_CTX_check_private_key(ctx) < 0)
      return NULL;

   strlcpy(str, resource_dir, sizeof(str));
   strlcat(str, "ssl/chain.crt", sizeof(str));
   if (file_exist(str))
      SSL_CTX_use_certificate_chain_file(ctx, str);

   return ctx;
}

#endif                          // HAVE_SSL
/*------------------------------------------------------------------*/

void server_loop(void)
{
   int status, i, n_error, broken, min, i_min, i_conn, more_requests;
   char str[1000], logbook[256], logbook_enc[256];
   char *pend;
   int lsock, len, flag, content_length, header_length;
   struct sockaddr_in serv_addr, acc_addr;
   struct hostent *phe;
   fd_set readfds;
   struct timeval timeout;
   char *net_buffer = NULL;
   int net_buffer_size;
#ifdef HAVE_SSL
   SSL *ssl_con;
   SSL_CTX *ssl_ctx;
#endif

   i_conn = content_length = 0;
   net_buffer_size = 100000;
   net_buffer = xmalloc(net_buffer_size);
   return_buffer_size = 100000;
   return_buffer = xmalloc(return_buffer_size);
   pend = NULL;

   /* determine logging level */
   if (getcfg(NULL, "Logging Level", str, sizeof(str)))
      _logging_level = atoi(str);
   else
      _logging_level = 2;

   /* initialize SSL if requested */
   _ssl_flag = 0;
   if (getcfg("global", "SSL", str, sizeof(str)) && atoi(str) == 1) {
#ifdef HAVE_SSL
      ssl_ctx = init_ssl();
      if (ssl_ctx == NULL) {
         eprintf("Cannot initialize SSL\n");
         exit(EXIT_FAILURE);
      }
      _ssl_flag = 1;
#else
      eprintf("SSL support not compiled into elogd\n");
      exit(EXIT_FAILURE);
#endif
   }

   /* create a new socket */
   lsock = socket(AF_INET, SOCK_STREAM, 0);
   if (lsock == -1) {
      eprintf("Cannot create socket\n");
      exit(EXIT_FAILURE);
   }

   /* bind local node name and port to socket */
   memset(&serv_addr, 0, sizeof(serv_addr));
   serv_addr.sin_family = AF_INET;
   /* if no hostname given with the -n flag, listen on any interface */
   if (listen_interface[0] == 0)
      serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
   else {
      /* look up the given hostname. gethostbyname() will take a hostname
         or an IP address */
      phe = gethostbyname(listen_interface);
      if (!phe) {
         eprintf("Cannot find address for -n %s\n", listen_interface);
         exit(EXIT_FAILURE);
      }
      if (phe->h_addrtype != AF_INET) {
         eprintf("Non Internet address for -n %s\n", listen_interface);
         exit(EXIT_FAILURE);
      }
      memcpy(&serv_addr.sin_addr.s_addr, phe->h_addr_list[0], phe->h_length);
   }

   serv_addr.sin_port = htons((short) elog_tcp_port);

   /* switch on reuse of port */
   flag = 1;
   setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (char *) &flag, sizeof(int));
   status = bind(lsock, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
   if (status < 0) {
      eprintf("Cannot bind to port %d.\nPlease use the \"-p\" flag to specify a different port.\n",
              elog_tcp_port);
      exit(EXIT_FAILURE);
   }

   /* get local host name */
   if (getcfg("global", "URL", str, sizeof(str)))
      split_url(str, host_name, NULL, NULL, NULL);
   else {
      gethostname(host_name, sizeof(host_name));

      phe = gethostbyname(host_name);
      /* if domain name is not in host name, hope to get it from phe */
      if (strchr(host_name, '.') == NULL && phe != NULL && strchr(phe->h_name, '.') != NULL)
         strcpy(host_name, phe->h_name);
   }

   /* open configuration file */
   getcfg("dummy", "dummy", str, sizeof(str));

   /* now, initiate daemon/service */
   if (running_as_daemon) {
      /* Redirect all messages handled with eprintf/efputs to syslog */
      redirect_to_syslog();

#ifdef OS_UNIX
      if (!ss_daemon_init()) {
         eprintf("Couldn't initiate the daemon; aborting\n");
         exit(EXIT_FAILURE);
      }
#endif
   }

   /* about to entering the server loop, welcome user with a brief info */
   eprintf("%s ", ELOGID);
   strcpy(str, svn_revision + 13);
   if (strchr(str, ' '))
      *strchr(str, ' ') = 0;
   eprintf("revision %s\n", str);
   if (verbose) {
      getcwd(str, sizeof(str));
      if (strchr(config_file, DIR_SEPARATOR) == NULL)
         eprintf("Config file  : %s%c%s\n", str, DIR_SEPARATOR, config_file);
      else
         eprintf("Config file  : %s\n", config_file);
      eprintf("Resource dir : %s\n", resource_dir[0] ? resource_dir : str);
      if (logbook_dir[0] && logbook_dir[0] != DIR_SEPARATOR && logbook_dir[1] != ':')
         eprintf("Logbook dir  : %s%c%s\n", str, DIR_SEPARATOR, logbook_dir);
      else
         eprintf("Logbook dir  : %s\n", logbook_dir[0] ? logbook_dir : str);
   }
#ifdef OS_UNIX
   /* create PID file if given as command line parameter or if running under root */

   if (geteuid() == 0 || pidfile[0]) {
      int fd;
      char buf[20];
      struct stat finfo;

      if (pidfile[0] == 0)
         strcpy(pidfile, PIDFILE);

      /* check if file exists */
      if (stat(pidfile, &finfo) >= 0) {
         eprintf("File \"%s\" exists, using \"%s.%d\" instead.\n", pidfile, pidfile, elog_tcp_port);
         sprintf(pidfile + strlen(pidfile), ".%d", elog_tcp_port);

         /* check again for the new name */
         if (stat(pidfile, &finfo) >= 0) {
            /* never overwrite a file */
            eprintf("Refuse to overwrite existing file \"%s\".\n", pidfile);
            _exit(EXIT_FAILURE);        /* don't call atexit() hook */
         }
      }

      fd = open(pidfile, O_CREAT | O_RDWR, 0644);
      if (fd < 0) {
         sprintf(str, "Error creating pid file \"%s\"", pidfile);
         eprintf("%s; %s\n", str, strerror(errno));
         exit(EXIT_FAILURE);
      }

      sprintf(buf, "%d\n", (int) getpid());
      if (write(fd, buf, strlen(buf)) == -1) {
         sprintf(str, "Error writing to pid file \"%s\"", pidfile);
         eprintf("%s; %s\n", str, strerror(errno));
         exit(EXIT_FAILURE);
      }
      close(fd);
   }

   /* install signal handler */
   signal(SIGTERM, ctrlc_handler);
   signal(SIGINT, ctrlc_handler);
   signal(SIGPIPE, SIG_IGN);
   signal(SIGHUP, hup_handler);
   /* give up root privilege */
   if (geteuid() == 0) {
      if (!getcfg("global", "Grp", str, sizeof(str)) || setegroup(str) < 0) {
         eprintf("Falling back to default group \"elog\"\n");
         if (setegroup("elog") < 0) {
            eprintf("Falling back to default group \"%s\"\n", DEFAULT_GROUP);
            if (setegroup(DEFAULT_GROUP) < 0) {
               eprintf("Refuse to run as setgid root.\n");
               eprintf("Please consider to define a Grp statement in configuration file\n");
               exit(EXIT_FAILURE);
            }
         }
      } else if (verbose)
         eprintf("Falling back to group \"%s\"\n", str);

      if (!getcfg("global", "Usr", str, sizeof(str)) || seteuser(str) < 0) {
         eprintf("Falling back to default user \"elog\"\n");
         if (seteuser("elog") < 0) {
            eprintf("Falling back to default user \"%s\"\n", DEFAULT_USER);
            if (seteuser(DEFAULT_USER) < 0) {
               eprintf("Refuse to run as setuid root.\n");
               eprintf("Please consider to define a Usr statement in configuration file\n");
               exit(EXIT_FAILURE);
            }
         }
      } else if (verbose)
         eprintf("Falling back to user \"%s\"\n", str);
   }
#endif

   /* load initial configuration */
   check_config();

   /* check for FCKedit */
   strlcpy(str, resource_dir, sizeof(str));
   strlcat(str, "scripts", sizeof(str));
   strlcat(str, DIR_SEPARATOR_STR, sizeof(str));
   strlcat(str, "fckeditor/fckeditor.js", sizeof(str));
   fckedit_exist = exist_file(str);
   if (fckedit_exist)
      eprintf("FCKedit detected\n");

   /* check for ImageMagick */
   my_shell("convert -version", str, sizeof(str));
   image_magick_exist = (strstr(str, "ImageMagick") != NULL);
   if (image_magick_exist)
      eprintf("ImageMagick detected\n");

   /* check for keepalive */
   if (!use_keepalive)
      eprintf("Keep-alive disabled\n");

   /* build logbook indices */
   if (!verbose && !running_as_daemon)
      eprintf("Indexing logbooks ... ");
   if (el_index_logbooks() != EL_SUCCESS)
      exit(EXIT_FAILURE);
   if (!verbose && !running_as_daemon)
      eputs("done");

   /* listen for connection */
   status = listen(lsock, SOMAXCONN);
   if (status < 0) {
      eprintf("Cannot listen\n");
      exit(EXIT_FAILURE);
   }

   if (_ssl_flag)
      sprintf(str, "SSLServer listening on port %d ...\n", elog_tcp_port);
   else
      sprintf(str, "Server listening on port %d ...\n", elog_tcp_port);
   eprintf("%s", str);
   if (_logging_level > 0)
      write_logfile(NULL, str);

   do {
      FD_ZERO(&readfds);
      FD_SET(lsock, &readfds);
      for (i = 0; i < N_MAX_CONNECTION; i++)
         if (ka_sock[i] > 0)
            FD_SET(ka_sock[i], &readfds);
      timeout.tv_sec = 1;
      timeout.tv_usec = 0;
      status = select(FD_SETSIZE, (void *) &readfds, NULL, NULL, (void *) &timeout);

      /* check UNIX signal flags */
      if (_abort)
         break;

      /* close old connections */
      for (i = 0; i < N_MAX_CONNECTION; i++)
         if (ka_sock[i] && (int) time(NULL) - ka_time[i] > 60) {
#ifdef HAVE_SSL
#endif
            closesocket(ka_sock[i]);
            ka_sock[i] = 0;
            ka_time[i] = 0;
         }

      if (status != -1) {       // if no HUP signal is received
         if (FD_ISSET(lsock, &readfds)) {
            len = sizeof(acc_addr);
            _sock = accept(lsock, (struct sockaddr *) &acc_addr, (void *) &len);

#ifdef HAVE_SSL
            if (_ssl_flag) {
               ssl_con = SSL_new(ssl_ctx);
               SSL_set_fd(ssl_con, _sock);
               if (SSL_accept(ssl_con) < 0) {
                  if (verbose)
                     eprintf("SSL_accept failed\n");
                  closesocket(_sock);
                  ka_sock[i_conn] = 0;
                  ka_ssl_con[i_conn] = ssl_con;
                  continue;
               }
            }
#endif

            /* find new entry in socket table */
            for (i = 0; i < N_MAX_CONNECTION; i++)
               if (ka_sock[i] == 0)
                  break;
            /* recycle last connection */
            if (i == N_MAX_CONNECTION) {
               for (i = i_min = 0, min = ka_time[0]; i < N_MAX_CONNECTION; i++)
                  if (ka_time[i] < min) {
                     min = ka_time[i];
                     i_min = i;
                  }
#ifdef HAVE_SSL
               if (_ssl_flag) {
                  SSL_set_fd(ssl_con, ka_sock[i_min]);
                  SSL_shutdown(ka_ssl_con[i_min]);
                  SSL_free(ka_ssl_con[i_min]);
                  ka_ssl_con[i_min] = NULL;
               }
#endif
               closesocket(ka_sock[i_min]);
               ka_sock[i_min] = 0;
               ka_time[i_min] = 0;
               i = i_min;
            }

            i_conn = i;
            ka_sock[i_conn] = _sock;
            ka_time[i_conn] = (int) time(NULL);
#ifdef HAVE_SSL
            ka_ssl_con[i_conn] = ssl_con;
#endif
            /* save remote host address */
            memcpy(&remote_addr[i_conn], &(acc_addr.sin_addr), sizeof(rem_addr));
            memcpy(&rem_addr, &(acc_addr.sin_addr), sizeof(rem_addr));

            if (getcfg("global", "Resolve host names", str, sizeof(str)) && atoi(str) == 1) {
               phe = gethostbyaddr((char *) &rem_addr, 4, PF_INET);
               if (phe != NULL)
                  strcpy(remote_host[i_conn], phe->h_name);
               else
                  strcpy(remote_host[i_conn], (char *) inet_ntoa(rem_addr));
            } else
               strcpy(remote_host[i_conn], (char *) inet_ntoa(rem_addr));

            strcpy(rem_host, remote_host[i_conn]);
         }

         else {
            /* check if open connection received data */
            for (i = 0; i < N_MAX_CONNECTION; i++)
               if (ka_sock[i] > 0 && FD_ISSET(ka_sock[i], &readfds))
                  break;
            if (i == N_MAX_CONNECTION) {
               _sock = 0;
            } else {
               i_conn = i;
               _sock = ka_sock[i_conn];
#ifdef HAVE_SSL
               ssl_con = ka_ssl_con[i_conn];
#endif
               ka_time[i_conn] = (int) time(NULL);
               memcpy(&rem_addr, &remote_addr[i_conn], sizeof(rem_addr));
               strcpy(rem_host, remote_host[i_conn]);
            }
         }

         /* turn off keep_alive by default */
         keep_alive = FALSE;

         /* receive data */
         if (_sock > 0) {

            memset(net_buffer, 0, net_buffer_size);
            len = 0;
            more_requests = 0;

            do {                /* pipleline loop */
               header_length = 0;
               n_error = 0;
               broken = FALSE;
               return_length = -1;
               do {

                  if (!more_requests) {
                     FD_ZERO(&readfds);
                     FD_SET(_sock, &readfds);
                     timeout.tv_sec = 1;
                     timeout.tv_usec = 0;
                     status = select(FD_SETSIZE, (void *) &readfds, NULL, NULL, (void *) &timeout);
                     if (FD_ISSET(_sock, &readfds)) {
#ifdef HAVE_SSL
                        if (_ssl_flag)
                           i = SSL_read(ssl_con, net_buffer + len, net_buffer_size - len);
                        else
#endif
                           i = recv(_sock, net_buffer + len, net_buffer_size - len, 0);
                     } else
                        break;

                     /* abort if connection got broken */
                     if (i < 0) {
                        broken = TRUE;
                        break;
                     }
                     if (i > 0)
                        len += i;

                     /* check if net_buffer needs to be increased */
                     if (len == net_buffer_size) {
                        net_buffer = xrealloc(net_buffer, net_buffer_size + 100000);
                        if (net_buffer == NULL) {
                           sprintf(str,
                                   "Error: Cannot increase net_buffer, out of memory, net_buffer_size = %d",
                                   net_buffer_size);
                           show_error(str);
                           break;
                        }

                        memset(net_buffer + net_buffer_size, 0, 100000);
                        net_buffer_size += 100000;
                     }

                     /* abort if 100x received zero bytes */
                     if (i == 0) {
                        n_error++;
                        if (n_error == 100) {
                           broken = TRUE;
                           break;
                        }
                     }
                  }

                  /* if we are in pipelining mode, clear this flag now to force a new
                     recv if the request is not complete */
                  more_requests = 0;

                  /* finish when empty line received */
                  pend = NULL;
                  if (strncmp(net_buffer, "GET", 3) == 0 && strncmp(net_buffer, "POST", 4) != 0) {
                     if (len > 4 && strstr(net_buffer, "\r\n\r\n") != NULL) {
                        pend = strstr(net_buffer, "\r\n\r\n") + 4;
                        break;
                     }
                     if (len > 6 && strstr(net_buffer, "\r\r\n\r\r\n") != NULL) {
                        pend = strstr(net_buffer, "\r\r\n\r\r\n") + 6;
                        break;
                     }
                  } else if (strncmp(net_buffer, "POST", 4) == 0) {
                     if (header_length == 0) {
                        /* extract logbook */
                        strlcpy(str, net_buffer + 6, sizeof(str));
                        if (strstr(str, "HTTP"))
                           *(strstr(str, "HTTP") - 1) = 0;
                        strlcpy(logbook, str, sizeof(logbook));
                        strlcpy(logbook_enc, str, sizeof(logbook));
                        url_decode(logbook);

                        /* extract content length */
                        if (strstr(net_buffer, "Content-Length:"))
                           content_length = atoi(strstr(net_buffer, "Content-Length:") + 15);
                        else if (strstr(net_buffer, "Content-length:"))
                           content_length = atoi(strstr(net_buffer, "Content-length:") + 15);

                        /* extract header length */
                        if (strstr(net_buffer, "\r\n\r\n"))
                           header_length = strstr(net_buffer, "\r\n\r\n") - net_buffer + 4;
                        if (strstr(net_buffer, "\r\r\n\r\r\n"))
                           header_length = strstr(net_buffer, "\r\r\n\r\r\n") - net_buffer + 6;

                        if (content_length > _max_content_length) {

                           /* drain socket connection */
                           do {
                              FD_ZERO(&readfds);
                              FD_SET(_sock, &readfds);
                              timeout.tv_sec = 6;
                              timeout.tv_usec = 0;
                              status = select(FD_SETSIZE, (void *) &readfds, NULL, NULL, (void *) &timeout);
                              if (FD_ISSET(_sock, &readfds)) {
#ifdef HAVE_SSL
                                 if (_ssl_flag)
                                    i = SSL_read(ssl_con, net_buffer, net_buffer_size);
                                 else
#endif
                                    i = recv(_sock, net_buffer, net_buffer_size, 0);
                              } else
                                 break;
                           } while (i > 0);

                           /* return error */
                           memset(return_buffer, 0, return_buffer_size);
                           strlen_retbuf = 0;
                           return_length = 0;

                           sprintf(str,
                                   loc("Error: Content length (%d) larger than maximum content length (%d)"),
                                   content_length, _max_content_length);
                           strcat(str, "<br>");
                           strcat(str,
                                  loc
                                  ("Please increase <b>\"Max content length\"</b> in [global] part of config file and restart elogd"));
                           keep_alive = FALSE;
                           show_error(str);
                           break;
                        }
                     }

                     if (header_length > 0 && len >= header_length + content_length) {
                        pend = net_buffer + header_length + content_length;
                        break;
                     }

                  } else if (strstr(net_buffer, "HEAD") != NULL) {
                     /* just return header */
                     rsprintf("HTTP/1.1 200 OK\r\n");
                     rsprintf("Server: ELOG HTTP %s-%d\r\n", VERSION, atoi(svn_revision + 13));
                     rsprintf("Connection: close\r\n");
                     rsprintf("Content-Type: text/html\r\n\r\n");
                     keep_alive = FALSE;
                     return_length = strlen_retbuf + 1;
                     break;
                  } else if (strstr(net_buffer, "OPTIONS") != NULL) {
                     return_length = -1;
                     break;
                  } else {
                     if (strlen(net_buffer) > 0 && verbose) {
                        strcpy(str, "Received unknown HTTP command: ");
                        strencode2(str, net_buffer, sizeof(str));
                        show_error(str);
                     }
                     break;
                  }

               } while (1);

               if (broken) {
                  if (verbose)
                     eprintf("TCP connection broken\n");
                  keep_alive = FALSE;
                  break;
               }

               /* now process HTTP request and put the result into the return_buffer */
               if (process_http_request(net_buffer, i_conn)) {

                  /* send back the return_buffer to the browser */
#ifdef HAVE_SSL
                  send_return(_sock, ssl_con, net_buffer);
#else
                  send_return(_sock, net_buffer);
#endif
               }

               /* check if the net_buffer contains more than one request (pipelining) */
               if (pend && *pend) {
                  memmove(net_buffer, pend, strlen(pend) + 1);
                  more_requests = 1;
                  len -= (pend - net_buffer);
               }

            } while (more_requests);

            if (!keep_alive) {
#ifdef HAVE_SSL
               if (_ssl_flag) {
                  SSL_shutdown(ssl_con);
                  SSL_free(ssl_con);
               }
#endif

               closesocket(_sock);
               ka_sock[i_conn] = 0;
#ifdef HAVE_SSL
               ka_ssl_con[i_conn] = NULL;
#endif
            }
         }
      }
#ifdef OS_WINNT

      /* under windows, check if configuration changed (via stat()) once each access */
      check_config();

#else

      /* under unix, rely on "kill -HUP elogd" */
      if (_hup) {
         /* reload configuration */
         check_config();
         el_index_logbooks();
         _hup = FALSE;
      }
#endif

      /* check for periodic tasks */
      check_cron();

   } while (!_abort);

   eprintf("elogd server aborted.\n");

   /* free all allocated memory */
   for (i = 0; lb_list[i].name[0]; i++) {
      if (lb_list[i].el_index) {
         xfree(lb_list[i].el_index);
         lb_list[i].el_index = NULL;
      }

      if (lb_list[i].n_el_index) {
         xfree(lb_list[i].n_el_index);
         lb_list[i].n_el_index = NULL;
      }
   }

   xfree(net_buffer);
   xfree(return_buffer);
   free_config();
}

/*------------------------------------------------------------------*/

int ss_getchar(BOOL reset)
/********************************************************************
 Routine: ss_getchar

 Purpose: Read a single character

 Input:
 BOOL   reset            Reset terminal to standard mode

 Output:
 <none>

 Function value:
 int             0       for no character available
 n       ASCII code for normal character

 \********************************************************************/
{
#ifdef OS_UNIX

   static BOOL init = FALSE;
   static struct termios save_termios;
   struct termios buf;
   int i, fd;
   char c[3];

   fd = fileno(stdin);

   if (reset) {
      if (init)
         tcsetattr(fd, TCSAFLUSH, &save_termios);
      init = FALSE;
      return 0;
   }

   if (!init) {
      tcgetattr(fd, &save_termios);
      memcpy(&buf, &save_termios, sizeof(buf));

      buf.c_lflag &= ~(ECHO | ICANON | IEXTEN);

      buf.c_iflag &= ~(ICRNL | INPCK | ISTRIP | IXON);

      buf.c_cflag &= ~(CSIZE | PARENB);
      buf.c_cflag |= CS8;
      /* buf.c_oflag &= ~(OPOST); */
      buf.c_cc[VMIN] = 0;
      buf.c_cc[VTIME] = 0;

      tcsetattr(fd, TCSAFLUSH, &buf);
      init = TRUE;
   }

   memset(c, 0, 3);
   i = my_read(fd, c, 1);

   if (i == 0)
      return 0;

   /* BS/DEL -> BS */
   if (c[0] == 127)
      return 8;

   return c[0];

#elif defined(OS_WINNT)

   static BOOL init = FALSE;
   static int repeat_count = 0;
   static int repeat_char;
   HANDLE hConsole;
   DWORD nCharsRead;
   INPUT_RECORD ir;
   OSVERSIONINFO vi;

   /* find out if we are under W95 */
   vi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
   GetVersionEx(&vi);

   if (vi.dwPlatformId != VER_PLATFORM_WIN32_NT) {
      /* under W95, console doesn't work properly */
      int c;

      if (!kbhit())
         return 0;

      c = getch();
      return c;
   }

   hConsole = GetStdHandle(STD_INPUT_HANDLE);

   if (reset) {
      SetConsoleMode(hConsole, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                     ENABLE_PROCESSED_INPUT | ENABLE_MOUSE_INPUT);
      init = FALSE;
      return 0;
   }

   if (!init) {
      SetConsoleMode(hConsole, ENABLE_PROCESSED_INPUT);
      init = TRUE;
   }

   if (repeat_count) {
      repeat_count--;
      return repeat_char;
   }

   PeekConsoleInput(hConsole, &ir, 1, &nCharsRead);

   if (nCharsRead == 0)
      return 0;

   ReadConsoleInput(hConsole, &ir, 1, &nCharsRead);

   if (ir.EventType != KEY_EVENT)
      return ss_getchar(0);

   if (!ir.Event.KeyEvent.bKeyDown)
      return ss_getchar(0);

   if (ir.Event.KeyEvent.wRepeatCount > 1) {
      repeat_count = ir.Event.KeyEvent.wRepeatCount - 1;
      repeat_char = ir.Event.KeyEvent.uChar.AsciiChar;
      return repeat_char;
   }

   if (ir.Event.KeyEvent.uChar.AsciiChar)
      return ir.Event.KeyEvent.uChar.AsciiChar;

   if (ir.Event.KeyEvent.dwControlKeyState & (ENHANCED_KEY))
      return ir.Event.KeyEvent.wVirtualKeyCode;

   return ss_getchar(0);
#endif
}

int read_password(char *pwd, int size)
{
   int n;
   char c, str[256];

   n = 0;
   do {
      c = ss_getchar(0);
      if (c == 13)
         break;

      if (c) {

         if (c == 8) {
            if (n > 0) {
               str[--n] = 0;
               eprintf("\b \b");
            }
         } else {
            str[n++] = c;
            eprintf("*");
         }
#ifdef OS_WINNT
         Sleep(10);
#endif
      }

   } while (1);
   str[n] = 0;

   ss_getchar(1);               // reset

   strlcpy(pwd, str, size);
   return n;
}

/*------------------------------------------------------------------*/

void create_password(char *logbook, char *name, char *pwd)
{
   int fh, length, i;
   char *cfgbuffer, str[256], *p;

   fh = open(config_file, O_RDONLY);
   if (fh < 0) {
      /* create new file */
      fh = open(config_file, O_CREAT | O_WRONLY, 0640);
      if (fh < 0) {
         eprintf("Cannot create \"%s\".\n", config_file);
         return;
      }
      sprintf(str, "[%s]\n%s = %s\n", logbook, name, pwd);
      write(fh, str, strlen(str));
      close(fh);
      eprintf("File \"%s\" created with password in logbook \"%s\".\n", config_file, logbook);
      return;
   }

   /* read existing file and add password */
   length = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);
   cfgbuffer = xmalloc(length + 1);
   length = my_read(fh, cfgbuffer, length);
   cfgbuffer[length] = 0;
   close(fh);
   fh = open(config_file, O_TRUNC | O_WRONLY, 0640);
   sprintf(str, "[%s]", logbook);
   /* check if logbook exists already */
   if (strstr(cfgbuffer, str)) {
      p = strstr(cfgbuffer, str);
      /* search password in current logbook */
      do {
         while (*p && *p != '\n')
            p++;
         if (*p && *p == '\n')
            p++;
         if (strncmp(p, name, strlen(name)) == 0) {
            /* replace existing password */
            i = (int) (p - cfgbuffer);
            write(fh, cfgbuffer, i);
            sprintf(str, "%s = %s\n", name, pwd);
            write(fh, str, strlen(str));
            eprintf("Password replaced in logbook \"%s\".\n", logbook);
            while (*p && *p != '\n')
               p++;
            if (*p && *p == '\n')
               p++;
            /* write remainder of file */
            write(fh, p, strlen(p));
            xfree(cfgbuffer);
            cfgbuffer = NULL;
            close(fh);
            return;
         }

      } while (*p && *p != '[');
      if (!*p || *p == '[') {
         /* enter password into current logbook */
         p = strstr(cfgbuffer, str);
         while (*p && *p != '\n')
            p++;
         if (*p && *p == '\n')
            p++;
         i = (int) (p - cfgbuffer);
         write(fh, cfgbuffer, i);
         sprintf(str, "%s = %s\n", name, pwd);
         write(fh, str, strlen(str));
         eprintf("Password added to logbook \"%s\".\n", logbook);
         /* write remainder of file */
         write(fh, p, strlen(p));
         xfree(cfgbuffer);
         cfgbuffer = NULL;
         close(fh);
         return;
      }
   } else {                     /* write new logbook entry */

      write(fh, cfgbuffer, strlen(cfgbuffer));
      sprintf(str, "\n[%s]\n%s = %s\n\n", logbook, name, pwd);
      write(fh, str, strlen(str));
      eprintf("Password added to new logbook \"%s\".\n", logbook);
   }

   xfree(cfgbuffer);
   cfgbuffer = NULL;
   close(fh);
}

void cleanup(void)
{
#ifdef OS_UNIX
   char str[256];
   struct stat finfo;

   /* regain original uid */
   if (setregid(-1, orig_gid) < 0 || setreuid(-1, orig_uid) < 0)
      eprintf("Cannot restore original GID/UID.\n");
   if (pidfile[0] && stat(pidfile, &finfo) >= 0) {
      if (remove(pidfile) < 0) {
         sprintf(str, "Cannot remove pidfile \"%s\"\n", pidfile);
         eprintf("%s; %s\n", str, strerror(errno));
      }
   }
#endif

   if (running_as_daemon)
#ifdef OS_UNIX
      closelog();
#else
      DeregisterEventSource(hEventLog);
#endif
}

/*------------------------------------------------------------------*/

#ifdef OS_WINNT

/* Routines for Windows service management */

// Executable name
#define ELOGDAPPNAME            "elogd"

// Internal service name
#define ELOGDSERVICENAME        "elogd"

// Displayed service name
#define ELOGDSERVICEDISPLAYNAME "elogd"

SERVICE_STATUS serviceStatus;
SERVICE_STATUS_HANDLE serviceStatusHandle = 0;

int install_service(void)
{
   OSVERSIONINFO vi;
   char path[2048], dir[2048], cmd[2080];
   SC_HANDLE hservice;
   SC_HANDLE hsrvmanager;

   /* check for Windows NT+ */

   vi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
   GetVersionEx(&vi);
   if (vi.dwPlatformId != VER_PLATFORM_WIN32_NT) {
      eprintf("Can install service only under Windows NT/2k/XP.\n");
      return -1;
   }

   if (GetModuleFileName(NULL, path, sizeof(path)) == 0) {
      eprintf("Cannot retrieve module file name.\n");
      return -1;
   }

   strcpy(dir, path);
   if (strrchr(dir, '\\'))
      *(strrchr(dir, '\\') + 1) = 0;

   sprintf(cmd, "\"%s\" -D -c \"%s%s\"", path, dir, CFGFILE);

   /* Open the default, local Service Control Manager database */
   hsrvmanager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
   if (hsrvmanager == NULL) {
      eprintf("Cannot connect to Service Control Manager.\n");
      return -1;
   }

   /* Create an entry for the elogd service */
   hservice = CreateService(hsrvmanager,        // SCManager database
                            ELOGDSERVICENAME,   // name of service
                            ELOGDSERVICEDISPLAYNAME,    // name to display
                            SERVICE_ALL_ACCESS, // desired access
                            SERVICE_WIN32_OWN_PROCESS | SERVICE_INTERACTIVE_PROCESS,
                            // service type
                            SERVICE_AUTO_START, // start type
                            SERVICE_ERROR_NORMAL,       // error control type
                            cmd,        // service's binary
                            NULL,       // no load ordering group
                            NULL,       // no tag identifier
                            "", // dependencies
                            NULL,       // LocalSystem account
                            NULL);      // no password

   if (hservice == NULL) {
      if (GetLastError() == ERROR_SERVICE_EXISTS)
         eprintf("The elogd service is already registered.\n");
      else
         eprintf("The elogd service could not be registered. Error code %d.\n", GetLastError());
   } else {
      eprintf("The elogd service has been registered successfully.\n");
      CloseServiceHandle(hservice);
   }

   /* Try to start the elogd service */
   hservice = OpenService(hsrvmanager, ELOGDSERVICENAME, SERVICE_ALL_ACCESS);

   if (hservice == NULL)
      eprintf("The elogd service could not be accessed. Error code %d.\n", GetLastError());
   else {
      if (!StartService(hservice, 0, NULL))
         eprintf("The elogd service could not be started. Error code %d.\n", GetLastError());
      else
         eprintf("The elogd service has been started successfully.\n");

      CloseServiceHandle(hservice);
   }

   CloseServiceHandle(hsrvmanager);
   return 1;
}

int remove_service(int silent)
{
   SC_HANDLE hservice;
   SC_HANDLE hsrvmanager;
   SERVICE_STATUS status;

   /* Open the SCM */
   hsrvmanager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
   if (hsrvmanager == NULL) {
      if (!silent)
         eprintf("Cannot connect to Service Control Manager.\n");
      return -1;
   }

   hservice = OpenService(hsrvmanager, ELOGDSERVICENAME, SERVICE_ALL_ACCESS);
   if (hservice == NULL) {
      if (!silent)
         eprintf("The elogd service could not be found.\n");
      return -1;
   }

   /* Try to stop the elogd service */
   if (ControlService(hservice, SERVICE_CONTROL_STOP, &status)) {
      while (QueryServiceStatus(hservice, &status)) {
         if (status.dwCurrentState == SERVICE_STOP_PENDING)
            Sleep(100);
         else
            break;
      }

      if (!silent) {
         if (status.dwCurrentState != SERVICE_STOPPED) {
            eprintf("The elogd service could not be stopped.\n");
         } else
            eprintf("elogd service stopped successfully.\n");
      }
   }

   /* Now remove the service from the SCM */
   if (!DeleteService(hservice)) {
      if (!silent) {
         if (GetLastError() == ERROR_SERVICE_MARKED_FOR_DELETE)
            eprintf("The elogd service is already marked to be unregistered.\n");
         else
            eprintf("The elogd service could not be unregistered.\n");
      }
   } else if (!silent)
      eprintf("The elogd service hass been unregistered successfully.\n");

   CloseServiceHandle(hservice);
   CloseServiceHandle(hsrvmanager);
   return 1;
}

void WINAPI ServiceControlHandler(DWORD controlCode)
{
   switch (controlCode) {
   case SERVICE_CONTROL_INTERROGATE:
      break;

   case SERVICE_CONTROL_SHUTDOWN:
   case SERVICE_CONTROL_STOP:
      serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
      SetServiceStatus(serviceStatusHandle, &serviceStatus);

      _abort = TRUE;

      return;

   case SERVICE_CONTROL_PAUSE:
      break;

   case SERVICE_CONTROL_CONTINUE:
      break;

   default:
      if (controlCode >= 128 && controlCode <= 255)
         // user defined control code
         break;
      else
         // unrecognised control code
         break;
   }

   SetServiceStatus(serviceStatusHandle, &serviceStatus);
}

void WINAPI ServiceMain(DWORD argc, LPSTR * argv)
{
   // initialise service status
   serviceStatus.dwServiceType = SERVICE_WIN32;
   serviceStatus.dwCurrentState = SERVICE_STOPPED;
   serviceStatus.dwControlsAccepted = 0;
   serviceStatus.dwWin32ExitCode = NO_ERROR;
   serviceStatus.dwServiceSpecificExitCode = NO_ERROR;
   serviceStatus.dwCheckPoint = 0;
   serviceStatus.dwWaitHint = 0;

   serviceStatusHandle = RegisterServiceCtrlHandler(ELOGDSERVICENAME, ServiceControlHandler);

   if (serviceStatusHandle) {
      // service is starting
      serviceStatus.dwCurrentState = SERVICE_START_PENDING;
      SetServiceStatus(serviceStatusHandle, &serviceStatus);

      // running
      serviceStatus.dwControlsAccepted |= (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
      serviceStatus.dwCurrentState = SERVICE_RUNNING;
      SetServiceStatus(serviceStatusHandle, &serviceStatus);

      /* avoid recursive calls to run_service */
      running_as_daemon = FALSE;

      /* Redirect all messages handled with eprintf/efputs to syslog */
      redirect_to_syslog();

      /* start main server, exit with "_abort = TRUE" */
      server_loop();

      // service was stopped
      serviceStatus.dwCurrentState = SERVICE_STOP_PENDING;
      SetServiceStatus(serviceStatusHandle, &serviceStatus);

      // service is now stopped
      serviceStatus.dwControlsAccepted &= ~(SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN);
      serviceStatus.dwCurrentState = SERVICE_STOPPED;
      SetServiceStatus(serviceStatusHandle, &serviceStatus);
   }
}

int run_service(void)
{
   SERVICE_TABLE_ENTRY serviceTable[] = {
      {ELOGDSERVICENAME, ServiceMain},
      {0, 0}
   };

   if (!StartServiceCtrlDispatcher(serviceTable))
      return FAILURE;

   return SUCCESS;
}

#endif

/*------------------------------------------------------------------*/

int main(int argc, char *argv[])
{
   int i, j, n, fh, tcp_port_cl, silent, sync_flag;
   char read_pwd[80], write_pwd[80], admin_pwd[80], smtp_pwd[80], str[256], logbook[256], clone_url[256],
       error_str[256], file_name[256];
   time_t now;
   struct tm *tms;
   struct stat finfo;

#ifdef OS_UNIX
   /* save gid/uid to regain later */
   orig_gid = getegid();
   orig_uid = geteuid();
   pidfile[0] = 0;
#endif

   /* register cleanup function */
   atexit(cleanup);

   tzset();

   /* initialize variables */
   read_pwd[0] = write_pwd[0] = admin_pwd[0] = smtp_pwd[0] = 0;
   logbook[0] = clone_url[0] = logbook_dir[0] = resource_dir[0] = logbook_dir[0] = 0;
   silent = tcp_port_cl = sync_flag = 0;
   use_keepalive = TRUE;
   running_as_daemon = FALSE;

   /*
    * Initially, redirect all messages handled with eprintf/efputs to stderr.
    * Note that we should use eprintf/efputs wrappers for all logging purposes,
    * but it is OK to use a printf for things like command line parsing till
    * we switch to daemon mode (if required).
    */
   redirect_to_stderr();

   /* evaluate predefined files and directories */
#ifdef CONFIG_PATH
   strcpy(config_file, CONFIG_PATH);
   if (config_file[0] && config_file[strlen(config_file) - 1] != DIR_SEPARATOR)
      strlcat(config_file, DIR_SEPARATOR_STR, sizeof(config_file));
#endif
   /* default config file */
   strlcat(config_file, CFGFILE, sizeof(config_file));

   /* parse command line parameters */
   for (i = 1; i < argc; i++) {
      if (argv[i][0] == '-' && argv[i][1] == 'D')
         running_as_daemon = TRUE;
      else if (argv[i][0] == '-' && argv[i][1] == 'v') {
         if (atoi(&argv[i][2]) > 0)
            verbose = atoi(&argv[i][2]);
         else
            verbose = 2;
      } else if (argv[i][0] == '-' && argv[i][1] == 'S')
         silent = TRUE;
      else if (argv[i][0] == '-' && argv[i][1] == 'k')
         use_keepalive = FALSE;
      else if (argv[i][0] == '-' && argv[i][1] == 'x')
         enable_execute = TRUE;
      else if (argv[i][0] == '-' && argv[i][1] == 'm')
         sync_flag = 1;
      else if (argv[i][0] == '-' && argv[i][1] == 'M')
         sync_flag = 2;
      else if (argv[i][1] == 'T') {
         time(&now);
         tms = localtime(&now);
         assert(tms);
         printf("Actual date/time: %02d%02d%02d_%02d%02d%02d\n", tms->tm_year % 100, tms->tm_mon + 1,
                tms->tm_mday, tms->tm_hour, tms->tm_min, tms->tm_sec);
         exit(EXIT_SUCCESS);
      }
#ifdef OS_WINNT
      else if (stricmp(argv[i], "-install") == 0) {
         install_service();
         if (!silent) {
            printf("Please hit any key ...");
            fgets(str, sizeof(str), stdin);
         }
         exit(EXIT_SUCCESS);
      } else if (stricmp(argv[i], "-remove") == 0) {
         remove_service(silent);
         if (!silent) {
            printf("Please hit any key ...");
            fgets(str, sizeof(str), stdin);
         }
         exit(EXIT_SUCCESS);
      }
#endif

      else if (argv[i][0] == '-') {
         if (argv[i][1] == 'C') {
            if (i + 1 >= argc || argv[i + 1][0] == '-')
               clone_url[0] = 1;
            else
               strcpy(clone_url, argv[++i]);
         } else if (i + 1 >= argc || argv[i + 1][0] == '-')
            goto usage;
         else if (argv[i][1] == 'p')
            tcp_port_cl = atoi(argv[++i]);
         else if (argv[i][1] == 'c')
            strcpy(config_file, argv[++i]);
         else if (argv[i][1] == 's')
            strcpy(resource_dir, argv[++i]);
         else if (argv[i][1] == 'd')
            strcpy(logbook_dir, argv[++i]);
         else if (argv[i][1] == 'r')
            strcpy(read_pwd, argv[++i]);
         else if (argv[i][1] == 'w')
            strcpy(write_pwd, argv[++i]);
         else if (argv[i][1] == 'a')
            strcpy(admin_pwd, argv[++i]);
         else if (argv[i][1] == 't')
            strcpy(smtp_pwd, argv[++i]);
         else if (argv[i][1] == 'l')
            strcpy(logbook, argv[++i]);
         else if (argv[i][1] == 'n')
            strlcpy(listen_interface, argv[++i], sizeof(listen_interface));
#ifdef OS_UNIX
         else if (argv[i][1] == 'f')
            strlcpy(pidfile, argv[++i], sizeof(pidfile));
#endif
         else {
          usage:printf("%s\n", ELOGID);
            printf("usage: elogd [-a <pwd>] [-C <url>] [-c <file>] [-D] [-d <dir>] ");
            printf("[-f <file>] [-h] [-k] [-l <logbook>] [-M] [-m] [-n <hostname>] ");
            printf("[-p <port>] [-r <pwd>] [-S] [-s <dir>] [-t <pwd>] [-v] [-w <pwd>] [-x]\n\n");
            printf("       -a <pwd> create/overwrite admin password in config file\n");
            printf("       -C <url> clone remote elogd configuration\n");
            printf("       -c <file> specify configuration file\n");
            printf("       -M synchronize with removing deleted entries\n");
            printf("       -m synchronize logbook(s) with remote server\n");
            printf("       -D become a daemon\n");
            printf("       -d <dir> specify logbook root directory\n");
#ifdef OS_UNIX
            printf("       -f <file> PID file\n");
#endif
            printf("       -h this help\n");
            printf("       -k do not use keep-alive\n");
            printf("       -l <logbook> specify logbook for -r, -w and -m commands\n");
            printf("       -n <interface> interface to listen on\n");
            printf("       -p <port> TCP/IP port\n");
            printf("       -r <pwd> create/overwrite read password in config file\n");
            printf("       -S be silent\n");
            printf("       -s <dir> specify resource directory (themes, icons, ...)\n");
            printf("       -t <pwd> create/overwrite SMTP password in config file\n");
            printf("       -v debugging output\n");
            printf("       -w <pwd> create/overwrite write password in config file\n");
            printf("       -x enable execution of shell commands\n\n");
#ifdef OS_WINNT
            printf("Windows service funtions:\n");
            printf("       -install     install elogd as service and start it\n");
            printf("       -remove      stop and remove elogd service\n");
#endif
            exit(EXIT_SUCCESS);
         }
      }
   }

#ifdef OS_WINNT
   {
      WSADATA WSAData;

      /* Start windows sockets */
      if (WSAStartup(MAKEWORD(1, 1), &WSAData) != 0)
         return 0;
   }
#endif

#ifdef OS_WINNT
   if (running_as_daemon) {
      /* change to directory of executable */
      strcpy(str, argv[0]);
      for (i = strlen(str) - 1; i > 0; i--)
         if (str[i] != '\\')
            str[i] = 0;
         else
            break;
      chdir(str);
   }
#endif

   /* clone remote elogd configuration */
   if (clone_url[0]) {

      /* check if local config file exists */
      fh = open(config_file, O_RDONLY | O_BINARY);
      if (fh > 0) {
         close(fh);
         eprintf("Overwrite local \"%s\"? [y]/n:  ", CFGFILE);
         fgets(str, sizeof(str), stdin);
         if (str[0] == 'n' || str[0] == 'N')
            exit(EXIT_FAILURE);
      }

      /* contact remote server */
      receive_config(NULL, clone_url, error_str);
      if (error_str[0]) {
         eputs(error_str);
         exit(EXIT_FAILURE);
      } else {
         printf("\nRemote configuration successfully received.\n\n");

         /* adjust config file */
         adjust_config(clone_url);

         /* receive logbook entries after set-up of direcories ... */
      }
   }

   /* check for configuration file */
   fh = open(config_file, O_RDONLY | O_BINARY);
   if (fh < 0) {
      eprintf("Cannot open \"%s\": %s\n", config_file, strerror(errno));
      exit(EXIT_FAILURE);
   }
   close(fh);

   /* parse contents of config file into internal structure */
   check_config();

   /* evaluate undefined directories from config file or compiled-in defaults */
   if (!resource_dir[0])
      if (getcfg("global", "Resource Dir", str, sizeof(str)))
         strlcpy(resource_dir, str, sizeof(resource_dir));
#ifdef RESOURCE_DIR
      else
         strlcpy(resource_dir, RESOURCE_DIR, sizeof(resource_dir));
#endif
   if (!logbook_dir[0])
      if (getcfg("global", "Logbook Dir", str, sizeof(str)))
         strlcpy(logbook_dir, str, sizeof(logbook_dir));
#ifdef LOGBOOK_DIR
      else
         strlcpy(logbook_dir, LOGBOOK_DIR, sizeof(logbook_dir));
#endif

   /* extract resource directory from configuration file if not given */
   if (config_file[0] && strchr(config_file, DIR_SEPARATOR) && !resource_dir[0]) {
      strcpy(resource_dir, config_file);
      for (i = strlen(resource_dir) - 1; i > 0; i--) {
         if (resource_dir[i] == DIR_SEPARATOR) {
            resource_dir[i] = 0;
            break;
         }
         resource_dir[i] = 0;
      }
   }

   /* do the same for the logbook dir */
   if (config_file[0] && strchr(config_file, DIR_SEPARATOR) && !logbook_dir[0]) {
      strcpy(logbook_dir, config_file);
      for (i = strlen(logbook_dir) - 1; i > 0; i--) {
         if (logbook_dir[i] == DIR_SEPARATOR)
            break;
         logbook_dir[i] = 0;
      }
      strlcat(logbook_dir, "logbooks", sizeof(logbook_dir));
   }

   /* set default logbook dir if not given */
   if (!logbook_dir[0])
      strcpy(logbook_dir, "logbooks");

   /* strip trailing dir separator */
   if (logbook_dir[strlen(logbook_dir) - 1] == DIR_SEPARATOR)
      logbook_dir[strlen(logbook_dir) - 1] = 0;

   /* check for directories */
   if (logbook_dir[0] && stat(logbook_dir, &finfo) < 0) {

#ifdef OS_WINNT
      if (mkdir(logbook_dir) == 0)
#else
      if (mkdir(logbook_dir, 0755) == 0)
#endif
         eprintf("Logbook directory \"%s\" successfully created.\n", logbook_dir);
      else {
         eprintf("Cannot create logbook directory \"%s\":%s.\n", logbook_dir, strerror(errno));
         exit(EXIT_FAILURE);
      }
   }

   if (resource_dir[0] && stat(resource_dir, &finfo) < 0) {
      eprintf("Resource directory \"%s\" not found.\n", resource_dir);
      exit(EXIT_FAILURE);
   }

   /* append '/' */
   if (resource_dir[0] && resource_dir[strlen(resource_dir) - 1] != DIR_SEPARATOR)
      strlcat(resource_dir, DIR_SEPARATOR_STR, sizeof(resource_dir));
   if (logbook_dir[0] && logbook_dir[strlen(logbook_dir) - 1] != DIR_SEPARATOR)
      strlcat(logbook_dir, DIR_SEPARATOR_STR, sizeof(logbook_dir));

   if (sync_flag) {
      el_index_logbooks();

      if (sync_flag == 2)
         setparam("confirm", "yes");

      if (logbook[0]) {
         for (i = 0; lb_list[i].name[0]; i++)
            if (stricmp(lb_list[i].name, logbook))
               break;
         if (!lb_list[i].name[0]) {
            eprintf("Logbook \"%s\" not defined in configuration file\n", logbook);
            exit(EXIT_FAILURE);
         }

         synchronize(&lb_list[i], SYNC_CLONE);
      } else
         synchronize(NULL, SYNC_CLONE);

      exit(EXIT_SUCCESS);
   }

   if (clone_url[0]) {

      /* force re-read of config file */
      check_config_file(TRUE);
      el_index_logbooks();

      /* check for retrieving password files */
      for (i = n = 0; lb_list[i].name[0]; i++)
         if (getcfg(lb_list[i].name, "Password file", str, sizeof(str)))
            n++;

      if (n > 0) {
         eprintf("\nRetrieve remote password files? [y]/n:  ");
         fgets(str, sizeof(str), stdin);
         if (str[0] != 'n' && str[0] != 'N')
            for (i = n = 0; lb_list[i].name[0]; i++) {

               if (lb_list[i].top_group[0])
                  setcfg_topgroup(lb_list[i].top_group);
               else
                  setcfg_topgroup("");

               if (getcfg(lb_list[i].name, "Password file", file_name, sizeof(file_name))) {

                  /* check if this file has not already been retrieved */
                  for (j = 0; j < i; j++) {
                     if (lb_list[j].top_group[0])
                        setcfg_topgroup(lb_list[j].top_group);
                     else
                        setcfg_topgroup("");

                     if (getcfg(lb_list[j].name, "Password file", str, sizeof(str))
                         && stricmp(file_name, str) == 0)
                        break;
                  }

                  if (lb_list[i].top_group[0])
                     setcfg_topgroup(lb_list[i].top_group);
                  else
                     setcfg_topgroup("");

                  if (j == i) {
                     receive_pwdfile(&lb_list[i], clone_url, error_str);
                     if (error_str[0]) {
                        eputs(error_str);
                        exit(EXIT_FAILURE);
                     } else
                        eprintf("File \"%s\" received successfully.\n", file_name);
                  }
               }
            }
      }

      eprintf("\nRetrieve remote logbook entries? [y]/n:  ");
      fgets(str, sizeof(str), stdin);
      if (str[0] != 'n' && str[0] != 'N')
         /* synchronize all logbooks */
         synchronize(NULL, SYNC_CLONE);

      puts("\nCloning finished. Check " CFGFILE " and start the server normally.");
      exit(EXIT_SUCCESS);
   }

   if ((read_pwd[0] || write_pwd[0] || admin_pwd[0]) && !logbook[0]) {
      printf("Must specify a lookbook via the -l parameter.\n");
      exit(EXIT_SUCCESS);
   }

   if (read_pwd[0]) {
      do_crypt(read_pwd, str, sizeof(str));
      create_password(logbook, "Read Password", str);
      exit(EXIT_SUCCESS);
   }

   if (write_pwd[0]) {
      do_crypt(write_pwd, str, sizeof(str));
      create_password(logbook, "Write Password", str);
      exit(EXIT_SUCCESS);
   }

   if (admin_pwd[0]) {
      do_crypt(admin_pwd, str, sizeof(str));
      create_password(logbook, "Admin Password", str);
      exit(EXIT_SUCCESS);
   }

   if (smtp_pwd[0]) {
      do_crypt(smtp_pwd, str, sizeof(str));
      create_password("global", "SMTP Password", str);
      exit(EXIT_SUCCESS);
   }

   /* get default port */
   if (getcfg("global", "SSL", str, sizeof(str)) && atoi(str) == 1)
      elog_tcp_port = 443;
   else
      elog_tcp_port = 80;

   /* get port from configuration file */
   if (tcp_port_cl != 0)
      elog_tcp_port = tcp_port_cl;
   else {
      if (getcfg("global", "Port", str, sizeof(str)))
         elog_tcp_port = atoi(str);
   }

   /* get optional content length from configuration file */
   if (getcfg("global", "Max content length", str, sizeof(str)))
      _max_content_length = atoi(str);

#ifdef OS_WINNT
   /* if running as a service, server_loop gets called from the service main routine */
   if (running_as_daemon) {
      redirect_to_syslog();

      if (!run_service()) {
         eprintf("Couldn't run the service; aborting\n");
         exit(EXIT_FAILURE);
      }
   } else
      server_loop();
#else

   server_loop();

#endif

   exit(EXIT_SUCCESS);
}
