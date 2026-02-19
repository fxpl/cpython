# Small range - fits in C long
r_small = range(1, 100, 2)
it_small = iter(r_small)
print(type(r_small))   # <class 'range'>
print(type(it_small))  # <class 'range_iterator'>  <- PyRangeIter_Type

# Large range - values don't fit in C long
r_large = range(2**100, 2**200, 2**50)
it_large = iter(r_large)
print(type(r_large))   # <class 'range'>            <- still PyRange_Type!
print(type(it_large))  # <class 'longrange_iterator'> <- PyLongRangeIter_Type