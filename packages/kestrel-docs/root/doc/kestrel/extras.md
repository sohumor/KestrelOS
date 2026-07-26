# kestrel-extras

Two small tools that are not worth space in the base image.

## cal

    cal                  the current month
    cal 3                March of the current year
    cal 3 2027           March 2027
    cal -y               the current year, three months to a row
    cal -y 2027          that year

Dates are proleptic Gregorian, so any year from 1 to 9999 prints.

## factor

    factor 1234567890
    factor 97

Prints `n: p1 p2 ...` with the prime factors in ascending order,
repeated by multiplicity. With no arguments it reads one number per line
from standard input. It strips 2 and 3, then trial-divides on the 6k+-1
wheel up to the square root, which handles the whole 64-bit range.
