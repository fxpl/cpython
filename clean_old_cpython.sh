make distclean  # Clears out the old configuration and object files
./configure --with-pydebug --with-assertions CFLAGS='-O0 -g' CXXFLAGS='-O0 -g'
make -j8
