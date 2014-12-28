#!/usr/bin/python2

arr1 = []
arr2 = []
def init():
    finit = open("rand.txt", "r")
    if not finit:
        print "rand.txt not exist"
    s = finit.read()
    l = s.split(" ")
    global arr1
    global arr2
    arr1 = [ int(item) for item in l ]
    arr1 = arr1[:1000]
    arr2 = arr1[::-1]

def bsort(arr):
    size = len(arr)
    for i in range(size-1):
        for j in range(size-1-i):
            if arr[i+j] > arr[i+j+1]:
                arr[i+j], arr[i+j+1] = arr[i+j+1], arr[i+j]

init()

fin = open("input.txt", "r")
if not fin:
    print "input.txt not exist"
fout = open("output.txt", "w")
if not fout:
    print "open output.txt error"

bsort(arr1)

fin.close()
fout.close()

print arr1
print len(arr1)
