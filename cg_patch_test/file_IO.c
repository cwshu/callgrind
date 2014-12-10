#include <stdio.h>

int main(){
    FILE* fin_ptr = fopen("./input.txt", "r");
    if( !fin_ptr ) printf("input.txt not exist\n");
    FILE* fout_ptr = fopen("./output.txt", "w");
    if( !fout_ptr ) printf("open output.txt error\n");

    char str[256];
    fscanf(fin_ptr, " %s", str);
    fprintf(fout_ptr, "file read: %s\n", str);

    fclose(fin_ptr);
    fclose(fout_ptr);
    return 0;
}
