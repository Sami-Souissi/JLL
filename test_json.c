/* vim: set et ts=4*/

/*
 * Test for json.c
 *
 * Compile (static linking) with
 *         gcc -o test_json -I.. test_json.c ./json.c -lm
 *
 * Compile (dynamic linking) with
 *         gcc -o test_json -I.. test_json.c -lm -ljsonparser
 *
 * USAGE: ./test_json <json_file>
 */





#include "json.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_RESET   "\x1b[0m"
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_CYAN    "\x1b[36m"




 


int main(int argc, char** argv)
{
        char* filename;
        json_settings *settings;
        char * error_buf ;
        FILE *fp;
        struct stat filestatus;
        int file_size;
        char* file_contents;
        json_char* json;
        json_value* value;

        if (argc != 2) {
                fprintf(stderr, "%s <file_json>\n", argv[0]);
                return 1;
        }
        filename = argv[1];

        if ( stat(filename, &filestatus) != 0) {
                fprintf(stderr, "File %s not found\n", filename);
                return 1;
        }
        file_size = filestatus.st_size;
        file_contents = (char*)malloc(filestatus.st_size);
        if ( file_contents == NULL) {
                fprintf(stderr, "Memory error: unable to allocate %d bytes\n", file_size);
                return 1;
        }

        fp = fopen(filename, "rt");
        if (fp == NULL) {
                fprintf(stderr, "Unable to open %s\n", filename);
                fclose(fp);
                free(file_contents);
                return 1;
        }
        if ( fread(file_contents, file_size, 1, fp) != 1 ) {
                fprintf(stderr, "Unable to read content of %s\n", filename);
                fclose(fp);
                free(file_contents);
                return 1;
        }
        fclose(fp);

        printf("%s\n", file_contents);

        printf("<-----------------+ EXECUTION +----------------->\n\n");

        json = (json_char*)file_contents;

        value = json_parse(json,file_size);

        if (value == NULL) {
                
                fprintf(stderr, "Parsing failure \n");
                printf("possible explination  :\n");
                printf("_________________________________\n\n");
                error_det (argc,argv);
                printf("\n");
                printf("_________________________________\n\n");
                
                free(file_contents);
                exit(1);
        }

        process_value(value, 0);
        printf("_________________________________\n\n");
        int choice = 0;
          while (true)
  {
    
    printf("1) get object\n");
    printf("2) "ANSI_COLOR_GREEN   "getterV2"   ANSI_COLOR_RESET "\n");
    printf("3) get value\n");
    printf("4) set\n");
    printf("5) display\n");
    printf("6) Update\n");
    printf("7) Export\n");
    printf("8) "ANSI_COLOR_CYAN   "new getter"   ANSI_COLOR_RESET "\n");
    printf("9) "ANSI_COLOR_CYAN   "Add"   ANSI_COLOR_RESET "\n");
    printf("10) "ANSI_COLOR_CYAN   "Export to Json"   ANSI_COLOR_RESET "\n");
    printf("11) "ANSI_COLOR_CYAN   "Updatev2.0"   ANSI_COLOR_RESET "\n");
    printf("12) Quit\n");
    printf("Enter Choice: ");
    scanf("%d", &choice);
    
    
    switch (choice)
    {
      
      case 1:
        getter(value); 
        break;
        case 2:
        getterv2(argc,argv);
        break;
        case 3:
        get_value(value); 
        break;

      
      case 4:
        setter(argc,argv);
        break;
      case 5:
      printf("\n");
        display(argc,argv);
        printf("\n");
        break;
      case 6:
      printf("\n");
        update(argc,argv);
        printf("\n");
        break;
        case 7:
      printf("\n");
        Export(argc,argv);
        printf("\n");
        break;
        case 8:
        new_getter(argc,argv);
        remove("temp.json");
      printf("\n");
        printf("\n");
        break;
        case 9:
        add(argc,argv);
      printf("\n");
        printf("\n");
        break;
        case 10:
        Export_to_json(argc,argv);
        
      printf("\n");
        printf("\n");
        break;
          case 11:
        updatev2(argc,argv);
        remove("temp.json");
      printf("\n");
        printf("\n");
        break;
      
      case 12:
       printf("\n");
        printf("_________________________________\n\n");
        exit(0);
        
    }
    
  }
                
                
                printf("\n");
        printf("_________________________________\n\n");

        json_value_free(value);
        free(file_contents);
        return 0;
}
