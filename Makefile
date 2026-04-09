CC      = gcc
CFLAGS  = -Wall -O2
LIBS    = -lcodec2 -lasound

all: pttkey_rec play_spool pttkey_ctrl

pttkey_rec: pttkey_rec.o log.o ini.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

play_spool: play_spool.o
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

pttkey_ctrl: pttkey_ctrl.o
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o pttkey_rec play_spool pttkey_ctrl
