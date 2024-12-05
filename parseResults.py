import sys

f = open (sys.argv[1], "r")

for l in f:
    if "vin1" in l:
        vin1 = round(float(l.split("=")[1]))
        l = f.readline()
        vin2 = round(float(l.split("=")[1]))
        l = f.readline()
        vinc = round(float(l.split("=")[1]))
        l = f.readline()
        vo1 = round(float(l.split("=")[1])/5) 
        l = f.readline()
        vo2 = round(float(l.split("=")[1])/5)
        l = f.readline()
        print (" Test: ", vin1, vin2, vinc, vo1, vo2)
