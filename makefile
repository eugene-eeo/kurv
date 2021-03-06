ifeq ($(PREFIX),)
    PREFIX := /usr/local
endif
CC=gcc
CFLAGS=-Wall -O3 -march=native

all: ichi-keygen ichi-lock ichi-sign

full: clean all tests

%.o: %.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

ichi-keygen: ichi-keygen.o base64/base64.o monocypher/monocypher.o utils.o
	$(CC) -o $@ $^

ichi-lock: ichi-lock.o base64/base64.o \
			monocypher/monocypher.o utils.o lock_stream.o \
			readpassphrase.o
	$(CC) -o $@ $^

ichi-sign: ichi-sign.o base64/base64.o monocypher/monocypher.o utils.o
	$(CC) -o $@ $^

clean:
	-rm *.o */*.o
	-rm -rf test
	-rm ichi-lock ichi-keygen ichi-sign

tests: ichi-keygen ichi-lock ichi-sign
	bats test_lock.sh test_sign.sh

# install: kurv luck
# 	install -d $(DESTDIR)$(PREFIX)/bin/
# 	install ./kurv $(DESTDIR)$(PREFIX)/bin/kurv
# 	install ./luck $(DESTDIR)$(PREFIX)/bin/luck

# uninstall:
# 	rm $(DESTDIR)$(PREFIX)/bin/kurv
# 	rm $(DESTDIR)$(PREFIX)/bin/luck
