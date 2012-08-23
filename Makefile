GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0)
GLIB_LDFLAGS := $(shell pkg-config --libs glib-2.0)

CFLAGS = -Wall -std=c99

default: libwatchit.so watchit

watchit: watchit.c
	$(CC) -o watchit watchit.c $(CFLAGS) $(GLIB_CFLAGS) $(GLIB_LDFLAGS)

libwatchit.so: libwatchit.c
	$(CC) -o libwatchit.so libwatchit.c -shared -fPIC -ldl -nostartfiles $(CFLAGS) $(GLIB_CFLAGS)

run:
	./watchit --output foo gcc -o libwatchit.so libwatchit.c -shared -fPIC -ldl -nostartfiles $(GLIB_CFLAGS) -lglib-2.0
