dflag = MUTEX_LOCK
cc = gcc
cflag = -lpthread
input = spinlock
all : $(input).c
	$(cc) -D $(dflag) $^ $(cflag)
clean:
	rm -rf a.out *.o
