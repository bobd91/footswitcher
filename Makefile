CC = gcc
CFLAGS = -I/usr/include/libevdev-1.0/
OBJ = footswitcher.o
LIBS = -levdev

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS) $(LIBS)

footswitcher: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS) $(LIBS)
