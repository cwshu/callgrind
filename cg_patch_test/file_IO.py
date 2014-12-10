#!/usr/bin/python3
fin = open("input.txt", "r")
if not fin:
    print("input.txt not exist")
fout = open("output.txt", "w")
if not fout:
    print("open output.txt error")

s = fin.readline()
fout.write(s)

fin.close()
fout.close()
