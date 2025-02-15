CC = mpicc
CFLAGS = -Wall -Wextra -pthread -O2
TARGET = tema2

all: build

build: $(TARGET)

$(TARGET): tema2.o
	$(CC) $(CFLAGS) -o $(TARGET) tema2.o

tema2.o: tema2.c
	$(CC) $(CFLAGS) -c tema2.c -o tema2.o

clean:
	rm -f $(TARGET) *.o