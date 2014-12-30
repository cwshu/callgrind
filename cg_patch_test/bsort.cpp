#include <iostream>
#include <fstream>

const int SIZE = 1000;
int arr1[SIZE];
int arr2[SIZE];

void init(){
    std::fstream finit;
    finit.open("rand.txt", std::fstream::in);
    for( int i=0; i<SIZE; i++ ){
        finit >> arr1[i];
        arr2[i] = arr1[i];
    }
}

void swap_int(int& a, int& b){
    int tmp = a;
    a = b;
    b = tmp;
}

int bsort(int arr[], int term){
    for( int i=0; i<term-1; i++ ){
        for( int j=0; j<term-1-i; j++ ){
            if( arr[j] > arr[j+1] ){
                swap_int(arr[j], arr[j+1]);
            }
        }
    }
}

int main(){
    std::fstream fin, fout;
    init();

    fin.open("input.txt", std::fstream::in);
    if( !fin.is_open() ) std::cout << "input.txt not exist\n";
    fout.open("output.txt", std::fstream::out);
    if( !fout.is_open() ) std::cout << "open output.txt error\n";

    bsort(arr1, SIZE);

    fin.close();
    fout.close();

    bsort(arr2, SIZE);
    // for( int i=0; i<SIZE; i++ ){
    //     std::cout << arr1[i] << " ";
    // }
    // std::cout << std::endl;

    return 0;
}
