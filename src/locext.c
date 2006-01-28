/********************************************************************\

  Name:         locext.c
  Created by:   Stefan Ritt

  Contents:     Extract all localization strings from elogd and put
                them into eloglang.xxxx

  $Log$
  Revision 1.6  2004/02/17 11:14:04  midas
  Version 2.5.1

  Revision 1.5  2004/01/26 14:52:17  midas
  Changed indentation

  Revision 1.4  2004/01/23 07:08:44  midas
  Adjusted comments

  Revision 1.3  2004/01/21 16:24:34  midas
  Added removel of obsolete lines

  Revision 1.2  2004/01/18 21:46:20  midas
  Applied indent

  Revision 1.1  2004/01/15 14:22:38  midas
  Initial revision


\********************************************************************/

#include <stdio.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>

#ifdef _MSC_VER
#include <windows.h>
#include <io.h>
#else
#define O_BINARY 0
#include <unistd.h>
#endif

void read_buf(char *filename, char **buf)
{
   int fh, size, i;

   fh = open(filename, O_RDONLY | O_BINARY);
   if (fh < 0) {
      printf("Cannot open file \"%s\"\n", filename);
      exit(1);
   }

   size = lseek(fh, 0, SEEK_END);
   lseek(fh, 0, SEEK_SET);

   *buf = malloc(size + 1);
   assert(*buf);

   i = read(fh, *buf, size);
   assert(i == size);
   (*buf)[size] = 0;
   close(fh);
}

/*------------------------------------------------------------------*/

int scan_file(char *infile, char *outfile)
{
   int i, fho, size, first;
   char *buf, *bufout, *p, *p2, str[1000], line[1000];

   read_buf(infile, &buf);

   p = buf;
   first = 1;

   do {
      p = strstr(p, "loc(\"");
      if (!p)
         break;

      /* check that we did not find "malloc(" */
      if (isalpha(*(p - 1))) {
         p++;
         continue;
      }

      p += 5;
      p2 = p;
      while (*p2) {
         if (*p2 == '"' && *(p2 - 1) != '\\')
            break;
         p2++;
      }

      size = (int) p2 - (int) p;
      if (size >= (int) sizeof(str)) {
         printf("Error: string too long\n");
         free(buf);
         return 1;
      }

      memset(str, 0, sizeof(str));
      memcpy(str, p, size < (int) sizeof(str) ? size : (int) sizeof(str));

      /* convert \" to " */
      for (p2 = str; *p2; p2++)
         if (*p2 == '\\')
            strcpy(p2, p2 + 1);

      /* check if string exists in output file */
      sprintf(line, "\n%s =", str);
      read_buf(outfile, &bufout);
      p2 = strstr(bufout, line);

      if (p2 == NULL) {
         /* append string to output file */
         fho = open(outfile, O_CREAT | O_WRONLY | O_APPEND | O_BINARY, 644);
         if (fho < 0) {
            printf("Cannot open file \"%s\" for append\n", outfile);
            return 1;
         }

         if (first && !strstr(bufout, "please translate following")) {
            sprintf(line,
                    "\r\n#\r\n#---- please translate following items and then remove this comment ----#\r\n#\r\n");
            write(fho, line, strlen(line));

            first = 0;
         }

         sprintf(line, "%s = \r\n", str);
         write(fho, line, strlen(line));
         close(fho);

         printf("Added string: ");
         puts(str);
      }

      free(bufout);

   } while (1);

   /* now remove obsolete strings */
   read_buf(outfile, &bufout);
   p = bufout;
   do {
      strncpy(line, p, sizeof(line));
      p2 = p;

      if (strchr(line, '\n'))
         *strchr(line, '\n') = 0;
      if (strchr(line, '\r'))
         *strchr(line, '\r') = 0;
      p += strlen(line);
      while (*p == '\r' || *p == '\n')
         p++;

      if (line[0] == '#' || line[0] == ';')
         continue;

      if (strchr(line, '='))
         *strchr(line, '=') = 0;

      if (strlen(line) <= 1)
         continue;

      while (line[strlen(line) - 1] == ' ')
         line[strlen(line) - 1] = 0;

      /* change " to \" */
      for (i = 0; i < (int) strlen(line); i++)
         if (line[i] == '"') {
            strcpy(str, line + i);
            line[i++] = '\\';
            strcpy(line + i, str);
         }

      sprintf(str, "loc(\"%s\")", line);
      if (strstr(buf, str))
         continue;

      printf("Removed string: ");
      puts(line);

      /* if not found, remove line */
      strcpy(p2, p);
      p = p2;

   } while (*p);

   fho = open(outfile, O_CREAT | O_WRONLY | O_BINARY | O_TRUNC, 644);
   if (fho < 0) {
      printf("Cannot open file \"%s\" for output\n", outfile);
      return 1;
   }
   write(fho, bufout, strlen(bufout));
   close(fho);


   free(bufout);
   free(buf);
   return 0;
}

/*------------------------------------------------------------------*/

int main(int argc, char *argv[])
{
   int status;

   if (argc != 3) {
      printf("Usage: %s <file.c> <language file>\n", argv[0]);
      return 1;
   }

   status = scan_file(argv[1], argv[2]);
   return status;
}
