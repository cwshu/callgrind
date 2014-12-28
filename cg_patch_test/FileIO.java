import java.io.File;
import java.io.IOException;

import java.util.Scanner;
import java.io.PrintStream;


public class FileIO{
    public static void main(String[] args){
        try{
            Scanner fin = new Scanner( new File("input.txt") );
            PrintStream fout = new PrintStream( new File("output.txt") );
            String str1 = fin.nextLine();
            fout.println(str1);
        }
        catch(IOException e){
            System.out.println("error");
        }
    }
}
